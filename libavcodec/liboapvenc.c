/*
 * liboapv encoder
 * Advanced Professional Video codec library
 *
 * Copyright (C) 2025 Dawid Kozinski <d.kozinski@samsung.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <stdlib.h>

#include <oapv/oapv.h>

#include "libavutil/avassert.h"
#include "libavutil/hdr_dynamic_metadata.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/mastering_display_metadata.h"

#include "avcodec.h"
#include "apv.h"
#include "codec_internal.h"
#include "encode.h"
#include "itut35.h"
#include "profiles.h"

#define MAX_BS_BUF   (128 * 1024 * 1024)
#define MAX_NUM_FRMS (1)           // supports only 1-frame in an access unit
#define FRM_IDX      (0)           // supports only 1-frame in an access unit
#define MAX_NUM_CC   (OAPV_MAX_CC) // Max number of color components (upto 4:4:4:4)
#define MAX_QP(bd)   (63 + ((bd) - 10) * 6) // same rule as liboapv's MAX_QUANT(BD)

static inline int64_t rescale_rational(AVRational a, int b)
{
    return av_rescale(a.num, b, a.den);
}

static void *apv_mem_malloc(void *udata, unsigned int size)
{
    return av_malloc(size);
}

static void *apv_mem_calloc(void *udata, unsigned int count, unsigned int size)
{
    return av_calloc(count, size);
}

static void *apv_mem_realloc(void *udata, void *ptr, unsigned int size)
{
    return av_realloc(ptr, size);
}

static void apv_mem_free(void *udata, void *ptr)
{
    av_free(ptr);
}

static const oapv_ops_mem_t apv_mem_ops = {
    .magic   = OAPV_OPS_MAGIC_CODE_MEM,
    .malloc  = apv_mem_malloc,
    .calloc  = apv_mem_calloc,
    .realloc = apv_mem_realloc,
    .free    = apv_mem_free,
};

/**
 * The structure stores all the states associated with the instance of APV encoder
 */
typedef struct ApvEncContext {
    const AVClass *class;

    oapve_t id;             // APV instance identifier
    oapvm_t mid;
    oapve_cdesc_t   cdsc;   // coding parameters i.e profile, width & height of input frame, num of therads, frame rate ...
    oapv_bitb_t     bitb;   // bitstream buffer (output)
    oapve_stat_t    stat;   // encoding status (output)

    oapv_frms_t ifrms;      // frames for input

    int preset_id;          // preset of apv ( fastest, fast, medium, slow, placebo)
    
    int family_id;          // apv family:
                            // 1. High quality mezzanine                    APV 422 HQ 4:2:2
                            // 2. Standard quality mezzanine                APV 422 SQ 4:2:2
                            // 3. Editing-friendly low-data-rate workflows  APV 422 LQ 4:2:2
                            // 4. Finishing                                 APV 444 UQ 4:4:4

    int qp;                 // quantization parameter (QP) [0,63]

    int t35_set;            // an HDR10+ T.35 payload is currently attached to mid

    AVDictionary *oapv_params;
} ApvEncContext;

static int check_family_conf(AVCodecContext* avctx, ApvEncContext* apv){

    int p = apv->cdsc.param[FRM_IDX].profile_idc; // profile idc information 

    switch(apv->family_id) {
    case OAPV_FAMILY_422_LQ:
    case OAPV_FAMILY_422_SQ:
    case OAPV_FAMILY_422_HQ:
        if(p != OAPV_PROFILE_422_10) {
            av_log(avctx, AV_LOG_ERROR, "Family idc (%d) and profile idc (%d) are unmatched\n", apv->family_id, p);
            return AVERROR(EINVAL);
        }
        break;
    case OAPV_FAMILY_444_UQ:
        if(p != OAPV_PROFILE_444_10) {
            av_log(avctx, AV_LOG_ERROR, "Family idc(%d) and profile idc (%d) are unmatched\n", apv->family_id, p);
            return AVERROR(EINVAL);
        }
        break;
    default:
        return AVERROR(EINVAL); // invalid/unknown family
    }
    return 0;
}

static int apv_imgb_release(oapv_imgb_t *imgb)
{
    int refcnt = --imgb->refcnt;
    if (refcnt == 0) {
        for (int i = 0; i < imgb->np; i++)
            av_freep(&imgb->baddr[i]);
        av_free(imgb);
    }

    return refcnt;
}

static int apv_imgb_addref(oapv_imgb_t * imgb)
{
    int refcnt = ++imgb->refcnt;
    return refcnt;
}

static int apv_imgb_getref(oapv_imgb_t * imgb)
{
    return imgb->refcnt;
}

/**
 * Convert FFmpeg pixel format (AVPixelFormat) into APV pre-defined color format
 *
 * @return APV pre-defined color format (@see oapv.h) on success, OAPV_CF_UNKNOWN on failure
 */
static inline int get_color_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    default:
        av_unreachable("Already checked via CODEC_PIXFMTS");
    case AV_PIX_FMT_GRAY10:
        return OAPV_CF_YCBCR400;
    case AV_PIX_FMT_YUV422P10:
        return OAPV_CF_YCBCR422;
    case AV_PIX_FMT_YUV422P12:
        return OAPV_CF_YCBCR422;
    case AV_PIX_FMT_YUV444P10:
        return OAPV_CF_YCBCR444;
    case AV_PIX_FMT_YUV444P12:
        return OAPV_CF_YCBCR444;
    case AV_PIX_FMT_YUVA444P10:
        return OAPV_CF_YCBCR4444;
    case AV_PIX_FMT_YUVA444P12:
        return OAPV_CF_YCBCR4444;
    }
}

static inline int get_chroma_format_idc(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    default:
        av_unreachable("Already checked via CODEC_PIXFMTS");
    case AV_PIX_FMT_GRAY10:
        return APV_CHROMA_FORMAT_400;
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_YUV422P12:
        return APV_CHROMA_FORMAT_422;
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_YUV444P12:
        return APV_CHROMA_FORMAT_444;
    case AV_PIX_FMT_YUVA444P10:
    case AV_PIX_FMT_YUVA444P12:
        return APV_CHROMA_FORMAT_4444;
    }
}

static inline int get_min_profile(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    default:
        av_unreachable("Already checked via CODEC_PIXFMTS");
    case AV_PIX_FMT_GRAY10:
        return AV_PROFILE_APV_400_10;
    case AV_PIX_FMT_YUV422P10:
        return AV_PROFILE_APV_422_10;
    case AV_PIX_FMT_YUV422P12:
        return AV_PROFILE_APV_422_12;
    case AV_PIX_FMT_YUV444P10:
        return AV_PROFILE_APV_444_10;
    case AV_PIX_FMT_YUV444P12:
        return AV_PROFILE_APV_444_12;
    case AV_PIX_FMT_YUVA444P10:
        return AV_PROFILE_APV_4444_10;
    case AV_PIX_FMT_YUVA444P12:
        return AV_PROFILE_APV_4444_12;
    }
}

static int profile_is_compatible(enum AVPixelFormat pix_fmt, int profile)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    int chroma_format_idc, bit_depth;

    av_assert0(desc);
    chroma_format_idc = get_chroma_format_idc(pix_fmt);
    bit_depth = desc->comp[0].depth;

    switch (profile) {
    case AV_PROFILE_APV_422_10:
        return chroma_format_idc == APV_CHROMA_FORMAT_422 && bit_depth == 10;
    case AV_PROFILE_APV_422_12:
        return chroma_format_idc == APV_CHROMA_FORMAT_422 &&
               bit_depth >= 10 && bit_depth <= 12;
    case AV_PROFILE_APV_444_10:
        return chroma_format_idc >= APV_CHROMA_FORMAT_422 &&
               chroma_format_idc <= APV_CHROMA_FORMAT_444 &&
               bit_depth == 10;
    case AV_PROFILE_APV_444_12:
        return chroma_format_idc >= APV_CHROMA_FORMAT_422 &&
               chroma_format_idc <= APV_CHROMA_FORMAT_444 &&
               bit_depth >= 10 && bit_depth <= 12;
    case AV_PROFILE_APV_4444_10:
        return chroma_format_idc >= APV_CHROMA_FORMAT_422 &&
               chroma_format_idc <= APV_CHROMA_FORMAT_4444 &&
               bit_depth == 10;
    case AV_PROFILE_APV_4444_12:
        return chroma_format_idc >= APV_CHROMA_FORMAT_422 &&
               chroma_format_idc <= APV_CHROMA_FORMAT_4444 &&
               bit_depth >= 10 && bit_depth <= 12;
    case AV_PROFILE_APV_400_10:
        return chroma_format_idc == APV_CHROMA_FORMAT_400 && bit_depth == 10;
    default:
        return 0;
    }
}

static int validate_profile(AVCodecContext *avctx, int profile)
{
    const int minimum = get_min_profile(avctx->pix_fmt);
    const char *profile_name = av_get_profile_name(avctx->codec, profile);
    const char *minimum_name = av_get_profile_name(avctx->codec, minimum);

    if (!profile_is_compatible(avctx->pix_fmt, profile)) {
        av_log(avctx, AV_LOG_ERROR,
               "Profile %s (%d) is incompatible with pixel format %s; minimum compatible profile is %s (%d)\n",
               profile_name ? profile_name : "unknown", profile,
               av_get_pix_fmt_name(avctx->pix_fmt),
               minimum_name ? minimum_name : "unknown", minimum);
        return AVERROR(EINVAL);
    }

    return 0;
}

static oapv_imgb_t *apv_imgb_create(AVCodecContext *avctx)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    oapv_imgb_t *imgb;
    int input_depth;
    int cfmt;  // color format
    int cs;

    av_assert0(desc);

    imgb = av_mallocz(sizeof(oapv_imgb_t));
    if (!imgb)
        goto fail;

    input_depth = desc->comp[0].depth;
    cfmt = get_color_format(avctx->pix_fmt);
    cs = OAPV_CS_SET(cfmt, input_depth, AV_HAVE_BIGENDIAN);

    imgb->np = desc->nb_components;

    for (int i = 0; i < imgb->np; i++) {
        imgb->w[i]  = avctx->width >> ((i == 1 || i == 2) ? desc->log2_chroma_w : 0);
        imgb->h[i]  = avctx->height;
        imgb->aw[i] = FFALIGN(imgb->w[i], OAPV_MB_W);
        imgb->ah[i] = FFALIGN(imgb->h[i], OAPV_MB_H);
        imgb->s[i]  = imgb->aw[i] * OAPV_CS_GET_BYTE_DEPTH(cs);

        imgb->bsize[i] = imgb->e[i] = imgb->s[i] * imgb->ah[i];
        imgb->a[i] = imgb->baddr[i] = av_mallocz(imgb->bsize[i]);
        if (imgb->a[i] == NULL)
            goto fail;
    }

    imgb->cs = cs;
    imgb->addref = apv_imgb_addref;
    imgb->getref = apv_imgb_getref;
    imgb->release = apv_imgb_release;
    imgb->refcnt = 1;

    return imgb;
fail:
    av_log(avctx, AV_LOG_ERROR, "cannot create image buffer\n");
    if (imgb) {
        for (int i = 0; i < imgb->np; i++)
            av_freep(&imgb->a[i]);
        av_freep(&imgb);
    }
    return NULL;
}

/**
 * Translate a liboapv error code into an AVERROR, logging the offending value
 *
 * @param[in] avctx codec context
 * @param[in] err liboapv error code
 *
 * @return negative AVERROR code
 */
static int apv_map_error(AVCodecContext *avctx, int err)
{
    const ApvEncContext *apv = avctx->priv_data;
    const oapve_param_t *param = &apv->cdsc.param[FRM_IDX];

    switch (err) {
    case OAPV_ERR_INVALID_PROFILE:
        av_log(avctx, AV_LOG_ERROR, "Invalid profile idc: %d\n", param->profile_idc);
        return AVERROR(EINVAL);
    case OAPV_ERR_INVALID_LEVEL:
        av_log(avctx, AV_LOG_ERROR, "Invalid level idc: %d\n", param->level_idc);
        return AVERROR(EINVAL);
    case OAPV_ERR_INVALID_BAND:
        av_log(avctx, AV_LOG_ERROR, "Invalid band idc: %d\n", param->band_idc);
        return AVERROR(EINVAL);
    case OAPV_ERR_INVALID_WIDTH:
    case OAPV_ERR_INVALID_HEIGHT:
        av_log(avctx, AV_LOG_ERROR, "Invalid frame size: %dx%d\n", param->w, param->h);
        return AVERROR(EINVAL);
    case OAPV_ERR_INVALID_FPS:
        av_log(avctx, AV_LOG_ERROR, "Invalid frame rate: %d/%d\n", param->fps_num, param->fps_den);
        return AVERROR(EINVAL);
    case OAPV_ERR_INVALID_QP:
        av_log(avctx, AV_LOG_ERROR, "Invalid QP: %d\n", param->qp);
        return AVERROR(EINVAL);
    case OAPV_ERR_INVALID_FAMILY:
        av_log(avctx, AV_LOG_ERROR, "Invalid family idc: %d\n", apv->family_id);
        return AVERROR(EINVAL);
    case OAPV_ERR_OUT_OF_MEMORY:
        return AVERROR(ENOMEM);
    case OAPV_ERR_UNSUPPORTED:
    case OAPV_ERR_UNSUPPORTED_COLORSPACE:
        return AVERROR(ENOSYS);
    }

    return AVERROR_EXTERNAL;
}

/**
 * Populate the liboapv configuration from AVCodecContext and encoder options.
 *
 * AVCodecContext fields are applied first, followed by liboapv private options
 * and finally oapv-params. The APV profile defaults to the minimum profile
 * implied by pix_fmt, and later overrides must remain compatible with that
 * pixel format.
 *
 * @param[in] avctx codec context (AVCodecContext)
 * @param[out] cdsc contains all APV encoder encoder parameters that should be initialized before the encoder is use
 *
 * @return 0 on success, negative error code on failure
 */
static int get_conf(AVCodecContext *avctx, oapve_cdesc_t *cdsc)
{
    ApvEncContext *apv = avctx->priv_data;

    /* initialize apv_param struct with default values */
    int ret = oapve_param_default(&cdsc->param[FRM_IDX]);
    if (OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot set default parameter\n");
        return AVERROR_EXTERNAL;
    }

    /* read options from AVCodecContext */
    cdsc->param[FRM_IDX].w = avctx->width;
    cdsc->param[FRM_IDX].h = avctx->height;

    if (avctx->framerate.num > 0) {
        cdsc->param[FRM_IDX].fps_num = avctx->framerate.num;
        cdsc->param[FRM_IDX].fps_den = avctx->framerate.den;
    } else if (avctx->time_base.num > 0) {
        cdsc->param[FRM_IDX].fps_num = avctx->time_base.den;
        cdsc->param[FRM_IDX].fps_den = avctx->time_base.num;
    }

    cdsc->param[FRM_IDX].profile_idc = get_min_profile(avctx->pix_fmt);
    if (avctx->profile != AV_PROFILE_UNKNOWN) {
        ret = validate_profile(avctx, avctx->profile);
        if (ret < 0)
            return ret;
        cdsc->param[FRM_IDX].profile_idc = avctx->profile;
    }
    cdsc->param[FRM_IDX].preset = apv->preset_id;

    int max_qp = MAX_QP(av_pix_fmt_desc_get(avctx->pix_fmt)->comp[0].depth);
    if (apv->qp > max_qp) {
        av_log(avctx, AV_LOG_ERROR, "QP %d out of range for %s input (max %d)\n",
               apv->qp, av_get_pix_fmt_name(avctx->pix_fmt), max_qp);
        return AVERROR(EINVAL);
    }
    if (apv->qp >= 0)
        cdsc->param[FRM_IDX].qp = apv->qp;
    if (avctx->bit_rate / 1000 > INT_MAX || avctx->rc_max_rate / 1000 > INT_MAX) {
        av_log(avctx, AV_LOG_ERROR, "bit_rate and rc_max_rate > %d000 is not supported\n", INT_MAX);
        return AVERROR(EINVAL);
    }
    cdsc->param[FRM_IDX].bitrate = (int)(avctx->bit_rate / 1000);
    if (cdsc->param[FRM_IDX].bitrate)
        cdsc->param[FRM_IDX].rc_type = OAPV_RC_ABR;

    cdsc->threads = avctx->thread_count;

    if (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED) {
        cdsc->param[FRM_IDX].color_primaries = avctx->color_primaries;
        cdsc->param[FRM_IDX].color_description_present_flag = 1;
    }

    if (avctx->color_trc != AVCOL_TRC_UNSPECIFIED) {
        cdsc->param[FRM_IDX].transfer_characteristics = avctx->color_trc;
        cdsc->param[FRM_IDX].color_description_present_flag = 1;
    }

    if (avctx->colorspace != AVCOL_SPC_UNSPECIFIED) {
        cdsc->param[FRM_IDX].matrix_coefficients = avctx->colorspace;
        cdsc->param[FRM_IDX].color_description_present_flag = 1;
    }

    if (avctx->color_range != AVCOL_RANGE_UNSPECIFIED) {
        cdsc->param[FRM_IDX].full_range_flag = (avctx->color_range == AVCOL_RANGE_JPEG);
        cdsc->param[FRM_IDX].color_description_present_flag = 1;
    }

    cdsc->max_bs_buf_size = MAX_BS_BUF; /* maximum bitstream buffer size */
    cdsc->max_num_frms = MAX_NUM_FRMS;
    cdsc->ops_mem = &apv_mem_ops;

    const AVDictionaryEntry *en = NULL;
    while ((en = av_dict_iterate(apv->oapv_params, en))) {
        ret = oapve_param_parse(&cdsc->param[FRM_IDX], en->key, en->value);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_WARNING, "Error parsing option '%s = %s'.\n", en->key, en->value);
        }
    }

    ret = validate_profile(avctx, cdsc->param[FRM_IDX].profile_idc);
    if (ret < 0)
        return ret;

    avctx->profile = cdsc->param[FRM_IDX].profile_idc;

    // family to bitrate conversion
    if (apv->family_id) {
        ret = check_family_conf(avctx, apv);
        if (ret < 0)
            return ret;

        int kbps = 0;
        ret = oapve_family_bitrate(apv->family_id, cdsc->param[FRM_IDX].w, cdsc->param[FRM_IDX].h, cdsc->param[FRM_IDX].fps_num, cdsc->param[FRM_IDX].fps_den, &kbps);
        if (OAPV_FAILED(ret)) {
            return apv_map_error(avctx, ret);
        }
        cdsc->param[FRM_IDX].bitrate = kbps;
        cdsc->param[FRM_IDX].rc_type = OAPV_RC_ABR;
    }

    /* bitrate source priority: -family > -oapv-params bitrate > -b:v */
    const AVDictionaryEntry *params_bitrate = av_dict_get(apv->oapv_params, "bitrate", NULL, 0);
    if (apv->family_id) {
        if (params_bitrate || avctx->bit_rate)
            av_log(avctx, AV_LOG_WARNING, "-family takes priority: the bitrate given with %s%s%s is ignored.\n",
                   avctx->bit_rate ? "-b:v" : "",
                   avctx->bit_rate && params_bitrate ? " and " : "",
                   params_bitrate ? "-oapv-params" : "");
    } else if (params_bitrate && avctx->bit_rate) {
        av_log(avctx, AV_LOG_WARNING, "The bitrate from -oapv-params takes priority: -b:v is ignored.\n");
    }

    if (cdsc->param[FRM_IDX].rc_type == OAPV_RC_ABR &&
        cdsc->param[FRM_IDX].qp != OAPVE_PARAM_QP_AUTO)
        av_log(avctx, AV_LOG_WARNING, "QP %d applies to the first frame only; rate control adjusts it afterwards.\n",
               cdsc->param[FRM_IDX].qp);

    /* keep the wrapper's CQP-32 default; untouched, liboapv would switch to
       ABR at the level's maximum rate */
    if (cdsc->param[FRM_IDX].rc_type == OAPV_RC_CQP &&
        cdsc->param[FRM_IDX].qp == OAPVE_PARAM_QP_AUTO)
        cdsc->param[FRM_IDX].qp = 32;

    return 0;
}

static int handle_side_data(AVCodecContext *avctx, ApvEncContext *apv)
{
    int size = 0;
    uint8_t payload[64];

    const AVFrameSideData *cll_sd =
        av_frame_side_data_get(avctx->decoded_side_data,
            avctx->nb_decoded_side_data, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    const AVFrameSideData *mdcv_sd =
        av_frame_side_data_get(avctx->decoded_side_data,
            avctx->nb_decoded_side_data,
            AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);

    if (cll_sd) {
        const AVContentLightMetadata *cll = (AVContentLightMetadata *)cll_sd->data;
        oapvm_payload_cll_t pl_cll = {
            .max_cll  = cll->MaxCLL,
            .max_fall = cll->MaxFALL,
        };

        int ret = oapvm_write_cll(&pl_cll, payload, &size);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot write content light level metadata\n");
            return AVERROR(EINVAL);
        }

        // oapvm_set() copies the payload
        ret = oapvm_set(apv->mid, 1, OAPV_METADATA_CLL, payload, size);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot set content light level metadata\n");
            return apv_map_error(avctx, ret);
        }
    }

    if (mdcv_sd) {
        const AVMasteringDisplayMetadata *mdcv = (AVMasteringDisplayMetadata *)mdcv_sd->data;
        oapvm_payload_mdcv_t pl_mdcv;

        // RFC 9924: chromaticities are 0.16 fixed point, max luminance 24.8,
        // min luminance 18.14 (i = 0, 1, 2 specifies Red, Green, Blue)
        for (int i = 0; i < 3; i++) {
            pl_mdcv.primary_chromaticity_x[i] = rescale_rational(mdcv->display_primaries[i][0], 1 << 16);
            pl_mdcv.primary_chromaticity_y[i] = rescale_rational(mdcv->display_primaries[i][1], 1 << 16);
        }

        pl_mdcv.white_point_chromaticity_x = rescale_rational(mdcv->white_point[0], 1 << 16);
        pl_mdcv.white_point_chromaticity_y = rescale_rational(mdcv->white_point[1], 1 << 16);

        pl_mdcv.max_mastering_luminance = rescale_rational(mdcv->max_luminance, 1 << 8);
        pl_mdcv.min_mastering_luminance = rescale_rational(mdcv->min_luminance, 1 << 14);

        int ret = oapvm_write_mdcv(&pl_mdcv, payload, &size);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot write mastering display metadata\n");
            return AVERROR(EINVAL);
        }

        ret = oapvm_set(apv->mid, 1, OAPV_METADATA_MDCV, payload, size);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot set mastering display metadata\n");
            return apv_map_error(avctx, ret);
        }
    }

    return 0;
}

/**
 * @brief Initialize APV codec
 * Create an encoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int liboapve_init(AVCodecContext *avctx)
{
    ApvEncContext *apv = avctx->priv_data;
    oapve_cdesc_t *cdsc = &apv->cdsc;
    oapvm_cdesc_t mdsc = { .ops_mem = &apv_mem_ops };
    unsigned char *bs_buf;
    int ret;

    /* allocate bitstream buffer */
    bs_buf = (unsigned char *)av_malloc(MAX_BS_BUF);
    if (bs_buf == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot allocate bitstream buffer, size=%d\n", MAX_BS_BUF);
        return AVERROR(ENOMEM);
    }
    apv->bitb.addr = bs_buf;
    apv->bitb.bsize = MAX_BS_BUF;

    /* read configurations and set values for created descriptor (APV_CDSC) */
    ret = get_conf(avctx, cdsc);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot get OAPV configuration\n");
        return ret;
    }

    /* create encoder */
    apv->id = oapve_create(cdsc, &ret);
    if (apv->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create OAPV encoder\n");
        return apv_map_error(avctx, ret);
    }

    /* create metadata handler */
    apv->mid = oapvm_create(&mdsc, &ret);
    if (apv->mid == NULL || OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "cannot create OAPV metadata handler\n");
        return AVERROR_EXTERNAL;
    }

    ret = handle_side_data(avctx, apv);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed handling side data! (%s)\n",
               av_err2str(ret));
        return ret;
    }

    int value = OAPV_CFG_VAL_AU_BS_FMT_NONE;
    int size = 4;
    /* AU-global config: no OAPV_CFG_FRM() frame index (only per-frame configs take one) */
    ret = oapve_config(apv->id, OAPV_CFG_SET_AU_BS_FMT, &value, &size);
    if (OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set config for using encoder output format\n");
        return AVERROR_EXTERNAL;
    }

    apv->ifrms.frm[FRM_IDX].imgb = apv_imgb_create(avctx);
    if (apv->ifrms.frm[FRM_IDX].imgb == NULL)
        return AVERROR(ENOMEM);
    apv->ifrms.num_frms++;

    /* color description values */
    if (cdsc->param[FRM_IDX].color_description_present_flag) {
        avctx->color_primaries = cdsc->param[FRM_IDX].color_primaries;
        avctx->color_trc = cdsc->param[FRM_IDX].transfer_characteristics;
        avctx->colorspace = cdsc->param[FRM_IDX].matrix_coefficients;
        avctx->color_range = (cdsc->param[FRM_IDX].full_range_flag) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    }

    return 0;
}

static int handle_hdr10plus(AVCodecContext *avctx, ApvEncContext *apv,
                            const AVFrame *frame)
{
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    uint8_t *buf, *payload;
    size_t payload_size;
    int ret;

    if (!sd) {
        if (apv->t35_set) {
            oapvm_rem(apv->mid, 1 /* group_id of the primary frame */,
                      OAPV_METADATA_ITU_T_T35, NULL);
            apv->t35_set = 0;
        }
        return 0;
    }

    ret = av_dynamic_hdr_plus_to_t35((const AVDynamicHDRPlus *)sd->data,
                                     NULL, &payload_size);
    if (ret < 0)
        return ret;

    buf = av_malloc(payload_size + 6);
    if (!buf)
        return AVERROR(ENOMEM);

    buf[0] = ITU_T_T35_COUNTRY_CODE_US;
    AV_WB16(buf + 1, ITU_T_T35_PROVIDER_CODE_SAMSUNG);
    AV_WB16(buf + 3, 1); // provider_oriented_code
    buf[5] = 4;          // application_identifier
    payload = buf + 6;

    ret = av_dynamic_hdr_plus_to_t35((const AVDynamicHDRPlus *)sd->data,
                                     &payload, &payload_size);
    if (ret < 0) {
        av_free(buf);
        return ret;
    }

    // oapvm_set() copies the payload and replaces an existing one of the same type
    ret = oapvm_set(apv->mid, 1 /* group_id of the primary frame */,
                    OAPV_METADATA_ITU_T_T35, buf, payload_size + 6);
    av_free(buf);
    if (OAPV_FAILED(ret))
        return apv_map_error(avctx, ret);
    apv->t35_set = 1;

    return 0;
}

/**
  * Encode raw data frame into APV packet
  *
  * @param[in]  avctx codec context
  * @param[out] avpkt output AVPacket containing encoded data
  * @param[in]  frame AVFrame containing the raw data to be encoded
  * @param[out] got_packet encoder sets to 0 or 1 to indicate that a
  *                         non-empty packet was returned in pkt
  *
  * @return 0 on success, negative error code on failure
  */
static int liboapve_encode(AVCodecContext *avctx, AVPacket *avpkt,
                          const AVFrame *frame, int *got_packet)
{
    ApvEncContext *apv =  avctx->priv_data;
    const oapve_cdesc_t *cdsc = &apv->cdsc;
    oapv_frm_t *frm = &apv->ifrms.frm[FRM_IDX];
    oapv_imgb_t *imgb = frm->imgb;
    int ret;

    av_image_copy2((uint8_t **)imgb->a, imgb->s, frame->data, frame->linesize,
                   frame->format, frame->width, frame->height);

    imgb->ts[0] = frame->pts;

    frm->group_id = 1; // @todo FIX-ME : need to set properly in case of multi-frame
    frm->pbu_type = OAPV_PBU_TYPE_PRIMARY_FRAME;

    ret = handle_hdr10plus(avctx, apv, frame);
    if (ret < 0)
        return ret;

    ret = oapve_encode(apv->id, &apv->ifrms, apv->mid, &apv->bitb, &apv->stat, NULL);
    if (OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "oapve_encode() failed\n");
        return AVERROR_EXTERNAL;
    }

    /* store bitstream */
    if (apv->stat.write > 0) {
        uint8_t *data = apv->bitb.addr;
        int size = apv->stat.write;

        // The encoder may return a "Raw bitstream" formatted AU, including au_size.
        // Discard it as we only need the access_unit() structure.
        if (size > 4 && AV_RB32(data) != APV_SIGNATURE) {
            data += 4;
            size -= 4;
        }

        ret = ff_get_encode_buffer(avctx, avpkt, size, 0);
        if (ret < 0)
            return ret;

        memcpy(avpkt->data, data, size);
        avpkt->pts = avpkt->dts = frame->pts;
        avpkt->flags |= AV_PKT_FLAG_KEY;

        if (cdsc->param[FRM_IDX].rc_type == OAPV_RC_CQP)
            ff_encode_add_stats_side_data(avpkt, cdsc->param[FRM_IDX].qp * FF_QP2LAMBDA, NULL, 0, AV_PICTURE_TYPE_I);

        *got_packet = 1;
    }

    return 0;
}

/**
 * Destroy the encoder and release all the allocated resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int liboapve_close(AVCodecContext *avctx)
{
    ApvEncContext *apv = avctx->priv_data;

    for (int i = 0; i < apv->ifrms.num_frms; i++) {
        if (apv->ifrms.frm[i].imgb != NULL)
            apv->ifrms.frm[i].imgb->release(apv->ifrms.frm[i].imgb);
        apv->ifrms.frm[i].imgb = NULL;
    }

    if (apv->mid) {
        oapvm_rem_all(apv->mid);
    }

    if (apv->id) {
        oapve_delete(apv->id);
        apv->id = NULL;
    }

    if (apv->mid) {
        oapvm_delete(apv->mid);
        apv->mid = NULL;
    }

    av_freep(&apv->bitb.addr); /* release bitstream buffer */

    return 0;
}

#define OFFSET(x) offsetof(ApvEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption liboapv_options[] = {
    { "preset", "Encoding preset for setting encoding speed (optimization level control)", OFFSET(preset_id), AV_OPT_TYPE_INT, { .i64 = OAPV_PRESET_DEFAULT }, OAPV_PRESET_FASTEST, OAPV_PRESET_PLACEBO, VE, .unit = "preset" },
    { "fastest", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_FASTEST }, 0, 0, VE, .unit = "preset" },
    { "fast",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_FAST },    0, 0, VE, .unit = "preset" },
    { "medium",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_MEDIUM },  0, 0, VE, .unit = "preset" },
    { "slow",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_SLOW },    0, 0, VE, .unit = "preset" },
    { "placebo", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_PLACEBO }, 0, 0, VE, .unit = "preset" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_DEFAULT }, 0, 0, VE, .unit = "preset" },

    { "family", "APV Family", OFFSET(family_id), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, OAPV_FAMILY_444_UQ, VE, .unit = "family" },
    { "422_LQ",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_FAMILY_422_LQ },  0, 0, VE, .unit = "family" },
    { "422_SQ",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_FAMILY_422_SQ },  0, 0, VE, .unit = "family" },
    { "422_HQ",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_FAMILY_422_HQ },  0, 0, VE, .unit = "family" },
    { "444_UQ",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_FAMILY_444_UQ },  0, 0, VE, .unit = "family" },

    { "qp", "Quantization parameter (max 63 for 10-bit, 75 for 12-bit input; CQP default 32, in ABR it seeds the first frame)", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, MAX_QP(12), VE, .unit = NULL },
    { "oapv-params",  "Override the apv configuration using a :-separated list of key=value parameters", OFFSET(oapv_params), AV_OPT_TYPE_DICT, { 0 }, 0, 0, VE, .unit = NULL },
    { NULL }
};

static const AVClass liboapve_class = {
    .class_name = "liboapv",
    .item_name  = av_default_item_name,
    .option     = liboapv_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault liboapve_defaults[] = {
    { "b", "0" },       // bitrate in terms of kilo-bits per second (support for bit-rates from a few hundred Mbps to a few Gbps for 2K, 4K and 8K resolution content)
    { NULL },
};

const FFCodec ff_liboapv_encoder = {
    .p.name             = "liboapv",
    .p.long_name        = NULL_IF_CONFIG_SMALL("liboapv APV"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_APV,
    .init               = liboapve_init,
    FF_CODEC_ENCODE_CB(liboapve_encode),
    .close              = liboapve_close,
    .priv_data_size     = sizeof(ApvEncContext),
    .p.priv_class       = &liboapve_class,
    .defaults           = liboapve_defaults,
    .p.capabilities     = AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_DR1,
    .p.wrapper_name     = "liboapv",
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_apv_profiles),
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS | FF_CODEC_CAP_NOT_INIT_THREADSAFE,
    CODEC_PIXFMTS(AV_PIX_FMT_GRAY10,
                  AV_PIX_FMT_YUV422P10,  AV_PIX_FMT_YUV422P12,
                  AV_PIX_FMT_YUV444P10,  AV_PIX_FMT_YUV444P12,
                  AV_PIX_FMT_YUVA444P10, AV_PIX_FMT_YUVA444P12),
};
