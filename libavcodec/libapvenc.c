/*
 * APV (Advanced Professional Video) encoder using Open APV library (liboapv)
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

#include <float.h>
#include <stdlib.h>

#include <oapv/oapv.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"
#include "libavutil/cpu.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/avassert.h"
#include <libavutil/imgutils.h>

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"
#include "codec_internal.h"
#include "profiles.h"
#include "encode.h"
#include "apv_imgb.h"

#define MAX_BS_BUF   (128 * 1024 * 1024)
#define MAX_NUM_FRMS (1)           // supports only 1-frame in an access unit
#define FRM_IDX      (0)           // supports only 1-frame in an access unit
#define MAX_NUM_CC   (OAPV_MAX_CC) // Max number of color componets (upto 4:4:4:4)

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

    oapv_imgb_t *imgb_r;    // image buffer for read
    oapv_imgb_t *imgb_i;    // image buffer for input
    oapv_frms_t ifrms;      // frames for input

    int num_frames;         // number of frames in an access unit

    int preset_id;          // preset of apv ( fastest, fast, medium, slow, placebo)

    int qp;                 // quantization parameter (QP) [0,51]

    int input_depth;        // input data bit depth (8, 10)
    int input_csp;          // input data color space (chroma format)
                            //  - 0: YUV400
                            //  - 1: YUV420
                            //  - 2: YUV422
                            //  - 3: YUV444

    AVDictionary *oapv_params;
} ApvEncContext;

static int align_to_16(int value) {
    return (value + 15) & ~15; // Rounding to the nearest value divisible by 16
}

static AVFrame* copy_and_align_avframe_to_16(const AVFrame* src_frame) {
    AVFrame* dst_frame = NULL;
    int ret = 0;

    if (!src_frame) {
        return NULL;
    }

    // Allocate a new AVFrame
    dst_frame = av_frame_alloc();
    if (!dst_frame) {
        return NULL;
    }

    // Set parameters for the new frame
    dst_frame->format = src_frame->format;
    dst_frame->width = align_to_16(src_frame->width);
    dst_frame->height = align_to_16(src_frame->height);

    // Memory allocation for a new frame
    ret = av_frame_get_buffer(dst_frame, 16);
    if (ret < 0) {
        av_frame_free(&dst_frame);
        return NULL;
    }

    // Data copying
    av_frame_copy(dst_frame, src_frame);

    return dst_frame;
}

/**
 * Convert FFmpeg pixel format (AVPixelFormat) into APV pre-defined color format
 *
 * @param[in] px_fmt pixel format (@see https://ffmpeg.org/doxygen/trunk/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5)
 *
 * @return APV pre-defined color format (@see oapv.h) on success, OAPV_CF_UNKNOWN on failure
 */
static int liboapve_apv_color_format(enum AVPixelFormat av_pix_fmt)
{
    int cf = OAPV_CF_UNKNOWN;

    switch (av_pix_fmt) {
    case AV_PIX_FMT_GRAY10:
        cf = OAPV_CF_YCBCR400;
        break;
    case AV_PIX_FMT_GRAY12:
        cf = OAPV_CF_YCBCR400;
        break;
    case AV_PIX_FMT_YUV420P10:
        cf = OAPV_CF_YCBCR420;
        break;
    case AV_PIX_FMT_YUV422P10:
        cf = OAPV_CF_YCBCR422;
        break;
    case AV_PIX_FMT_YUV444P10:
        cf = OAPV_CF_YCBCR444;
        break;
    case AV_PIX_FMT_YUV422P12:
        cf = OAPV_CF_YCBCR422;
        break;
    case AV_PIX_FMT_YUV444P12:
        cf = OAPV_CF_YCBCR444;
        break;
    case AV_PIX_FMT_YUVA444P10:
        cf = OAPV_CF_YCBCR4444;
        break;
    case AV_PIX_FMT_YUVA444P12:
        cf = OAPV_CF_YCBCR4444;
        break;
    default:
        cf = OAPV_CF_UNKNOWN;
        break;
    }

    return cf;
}

/**
 * Convert FFmpeg pixel format (AVPixelFormat) into APV pre-defined color space
 *
 * @param[in] px_fmt pixel format (@see https://ffmpeg.org/doxygen/trunk/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5)
 *
 * @return APV pre-defined color space (@see oapv.h) on success, OAPV_CS_UNKNOWN on failure
 */
static int liboapve_apv_color_space(enum AVPixelFormat av_pix_fmt)
{
    int cs = OAPV_CS_UNKNOWN;

    switch (av_pix_fmt) {
    case AV_PIX_FMT_YUV422P10:
        cs = OAPV_CS_SET(OAPV_CF_YCBCR422, 10, AV_HAVE_BIGENDIAN);
        break;
    case AV_PIX_FMT_YUV444P10:
        cs = OAPV_CS_SET(OAPV_CF_YCBCR444, 10, AV_HAVE_BIGENDIAN);
        break;
    case AV_PIX_FMT_YUV422P12:
        cs = OAPV_CS_SET(OAPV_CF_YCBCR422, 12, AV_HAVE_BIGENDIAN);
        break;
    case AV_PIX_FMT_YUV444P12:
        cs = OAPV_CS_SET(OAPV_CF_YCBCR444, 12, AV_HAVE_BIGENDIAN);
        break;
    default:
        cs = OAPV_CS_UNKNOWN;
        break;
    }

    return cs;
}

/**
 * The function returns a pointer to the object of the oapve_cdesc_t type.
 * oapve_cdesc_t contains all encoder parameters that should be initialized before the encoder is used.
 *
 * The field values of the oapve_cdesc_t structure are populated based on:
 * - the corresponding field values of the AvCodecConetxt structure,
 * - the apv encoder specific option values,
 *   (the full list of options available for apv encoder is displayed after executing the command ./ffmpeg --help encoder = liboapv)
 *
 * The order of processing input data and populating the apve_cdsc structure
 * 1) first, the fields of the AVCodecContext structure corresponding to the provided input options are processed,
 *    (i.e -pix_fmt yuv422p -s:v 1920x1080 -r 30 -profile:v 0)
 * 2) then apve-specific options added as AVOption to the apv AVCodec implementation
 *    (i.e -preset 0)
 *
 * Keep in mind that, there are options that can be set in different ways.
 * In this case, please follow the above-mentioned order of processing.
 * The most recent assignments overwrite the previous values.
 *
 * @param[in] avctx codec context (AVCodecContext)
 * @param[out] cdsc contains all APV encoder encoder parameters that should be initialized before the encoder is use
 *
 * @return 0 on success, negative error code on failure
 */
static int get_conf(AVCodecContext *avctx, oapve_cdesc_t *cdsc)
{
    ApvEncContext *apvctx = NULL;
    int ret;

    const char *option_name = "qp";
    uint8_t qp_default_value = 0;
    const AVOption *option = NULL;

    apvctx = avctx->priv_data;
    option = av_opt_find(&apvctx->class, option_name, NULL, 0, 0);

    if (option) {
        uint64_t default_value;
        av_opt_get_int(avctx->priv_data, option_name, 0, &default_value);

        if (default_value) {
            qp_default_value = (uint8_t)default_value;
        }
    }

    for(int i=0;i<OAPV_MAX_NUM_FRAMES;i++) {

        /* initialize apv_param struct with default values */
        ret = oapve_param_default(&cdsc->param[i]);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot set default parameter\n");
            return AVERROR_EXTERNAL;
        }

        /* read options from AVCodecContext */
        if (avctx->width > 0)
            cdsc->param[i].w = avctx->width;

        if (avctx->height > 0)
            cdsc->param[i].h = avctx->height;

        if (avctx->framerate.num > 0) {
            cdsc->param[i].fps_num = avctx->framerate.num;
            cdsc->param[i].fps_den = avctx->framerate.den;
        }

        cdsc->param[i].preset = apvctx->preset_id;
        cdsc->param[i].qp = apvctx->qp;
        if (avctx->bit_rate / 1000 > INT_MAX || avctx->rc_max_rate / 1000 > INT_MAX) {
            av_log(avctx, AV_LOG_ERROR, "Not supported bitrate bit_rate and rc_max_rate > %d000\n", INT_MAX);
            return AVERROR_INVALIDDATA;
        }
        cdsc->param[i].bitrate = (int)(avctx->bit_rate / 1000);
        if(cdsc->param[i].bitrate) {
            if(cdsc->param[i].qp!=qp_default_value) {
                av_log(avctx, AV_LOG_WARNING, "You cannot set both the bitrate and the QP parameter at the same time.\n"
                                              "If the bitrate is set, the rate control type is set to ABR, which means that the QP value is ignored.\n");
            }
            cdsc->param[i].rc_type = OAPV_RC_ABR;
        }

        cdsc->threads = OAPV_CDESC_THREADS_AUTO;

        if(avctx->color_primaries!=AVCOL_PRI_UNSPECIFIED) {
            cdsc->param[i].color_primaries = avctx->color_primaries;
            cdsc->param[i].color_description_present_flag = 1;
        }
        if(avctx->color_trc!=AVCOL_TRC_UNSPECIFIED) {
            cdsc->param[i].transfer_characteristics = avctx->color_trc;
            cdsc->param[i].color_description_present_flag = 1;
        }
        if(avctx->colorspace!=AVCOL_SPC_UNSPECIFIED) {
            cdsc->param[i].matrix_coefficients = avctx->colorspace;
            cdsc->param[i].color_description_present_flag = 1;
        }
        if(avctx->color_range!=AVCOL_RANGE_UNSPECIFIED) {
            cdsc->param[i].full_range_flag = (avctx->color_range==AVCOL_RANGE_JPEG)?1:0;
            cdsc->param[i].color_description_present_flag = 1;
        }
    }

    apvctx->input_csp = liboapve_apv_color_space(avctx->pix_fmt);
    if(apvctx->input_csp == OAPV_CS_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name (avctx->pix_fmt));
        return AVERROR_INVALIDDATA;
    }

    cdsc->max_bs_buf_size = MAX_BS_BUF; /* maximum bitstream buffer size */
    cdsc->max_num_frms = MAX_NUM_FRMS;

    return 0;
}

static int get_bit_depth(AVCodecContext *avctx, enum AVPixelFormat pixel_format)
 {
     const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pixel_format);
     if (desc == NULL) {
         av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format (%s)\n", av_get_pix_fmt_name(pixel_format));
         return AVERROR_EXTERNAL;
     }
     return desc->comp[0].depth;
 }


/**
 * @brief Initialize OpenAPV encoder
 * Create an encoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int liboapve_init(AVCodecContext *avctx)
{
    ApvEncContext *apvctx = avctx->priv_data;
    unsigned char *bs_buf = NULL;
    int cfmt = OAPV_CF_UNKNOWN;  // color format
    oapve_cdesc_t *cdsc =  &(apvctx->cdsc);
    int ret = 0;

    apvctx->id = NULL;
    apvctx->mid = NULL;
    apvctx->bitb.addr = NULL;
    memset(cdsc, 0, sizeof(oapve_cdesc_t));

    /* allocate bitstream buffer */
    bs_buf = (unsigned char *)av_malloc(MAX_BS_BUF);
    if (bs_buf == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot allocate bitstream buffer, size=%d\n", MAX_BS_BUF);
        return AVERROR(ENOMEM);
    }
    apvctx->bitb.addr = bs_buf;
    apvctx->bitb.bsize = MAX_BS_BUF;

    /* read configurations and set values for created descriptor (APV_CDSC) */
    if ((ret = get_conf(avctx, cdsc)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot get oapv configuration\n");
        return AVERROR(EINVAL);
    }

    {
        const AVDictionaryEntry *en = NULL;
        while (en = av_dict_iterate(apvctx->oapv_params, en)) {
            for(int i=0; i<cdsc->max_num_frms; i++) {
                if ((ret = oapve_param_parse(&cdsc->param[i], en->key, en->value)) < 0) {
                    av_log(avctx, AV_LOG_WARNING, "Error parsing option '%s = %s'.\n", en->key, en->value);
                }
            }
        }
    }

    /* create encoder */
    apvctx->id = oapve_create(cdsc, &ret);
    if (apvctx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create oapv encoder\n");
        if(ret==OAPV_ERR_INVALID_LEVEL) {
            av_log(avctx, AV_LOG_ERROR, "Invalid level idc: %d\n", cdsc->param[0].level_idc);
        }
        return AVERROR_EXTERNAL;
    }

    /* create metadata handler */
    apvctx->mid = oapvm_create(&ret);
    if(apvctx->mid == NULL || OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create oapv metadata handler\n");
        return AVERROR_EXTERNAL;
    }

    apvctx->input_depth = get_bit_depth(avctx, avctx->pix_fmt);
    if(apvctx->input_depth != 10 && apvctx->input_depth != 12)  {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format (%s)n", av_get_pix_fmt_name(avctx->pix_fmt));
        return AVERROR(EINVAL);
    }

    apvctx->imgb_r = NULL; // image buffer for read
    apvctx->imgb_i = NULL; // image buffer for input
    apvctx->num_frames = MAX_NUM_FRMS; // number of frames in an access unit

    cfmt = liboapve_apv_color_format(avctx->pix_fmt);

    // create input and reconstruction image buffers
    memset(&apvctx->ifrms, 0, sizeof(oapv_frms_t));

    for(int i = 0; i < apvctx->num_frames; i++) {
        if(apvctx->input_depth  == 10) {
            apvctx->ifrms.frm[FRM_IDX].imgb = apv_imgb_create(avctx->width, avctx->height, OAPV_CS_SET(cfmt, apvctx->input_depth, 0), avctx);
        }
        else {
            apvctx->imgb_r = apv_imgb_create(avctx->width, avctx->height, OAPV_CS_SET(cfmt, apvctx->input_depth, 0), avctx);
            apvctx->ifrms.frm[FRM_IDX].imgb = apv_imgb_create(avctx->width, avctx->height, OAPV_CS_SET(cfmt, 10, 0), avctx);
        }
        apvctx->ifrms.num_frms++;
    }

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
    ApvEncContext *apvctx =  avctx->priv_data;
    AVFrame* tmp_frame = NULL;
    int  ret = -1;

    if (frame==NULL) {
        return 0;
    }

    if(apvctx->input_depth == 10) {
        apvctx->imgb_i = apvctx->ifrms.frm[FRM_IDX].imgb;
    }
    else {
        apvctx->imgb_i = apvctx->imgb_r;
    }

    // The liboapv library requires that the frame size be a multiple of 16
    tmp_frame = copy_and_align_avframe_to_16(frame);
    for (int i = 0; i < apvctx->imgb_i->np; i++) {
        memcpy(apvctx->imgb_i->a[i], tmp_frame->data[i], tmp_frame->linesize[i]*tmp_frame->height);
    }
    av_frame_free(&tmp_frame);

    // @todo Possibility of optimization (skipping memory allocation for an additional temporary object of type AVFrame).
    //
    // int h_chroma, v_chroma;
    // int frame_height[4];

    // av_pix_fmt_get_chroma_subsample(frame->format, &h_chroma, &v_chroma);

    // frame_height[0] = frame->height;
    // frame_height[1] = frame->height / v_chroma;
    // frame_height[2] = frame->height / v_chroma;
    // frame_height[3] = 0;

    // for (int i = 0; i < apvctx->imgb_i->np; i++) {
    //     for(int j=0; j < frame_height[i]; j++) {
    //         memcpy(apvctx->imgb_i->a[i]+j*apvctx->imgb_i->s[i], frame->data[i]+j*frame->linesize[i], frame->linesize[i]);
    //     }
    // }

    if(apvctx->input_depth != 10) {
        apv_imgb_cpy(apvctx->ifrms.frm[FRM_IDX].imgb, apvctx->imgb_i, avctx);
    }

    apvctx->ifrms.frm[FRM_IDX].imgb->ts[0] = frame->pts;

    apvctx->ifrms.frm[FRM_IDX].group_id = 1; // @todo FIX-ME : need to set properly in case of multi-frame
    apvctx->ifrms.frm[FRM_IDX].pbu_type = OAPV_PBU_TYPE_PRIMARY_FRAME;

    ret = oapve_encode(apvctx->id, &apvctx->ifrms, apvctx->mid, &(apvctx->bitb), &(apvctx->stat), NULL);
    if (OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "oapve_encode() failed\n");
        return AVERROR_EXTERNAL;
    }

    /* store bitstream */
    if(OAPV_SUCCEEDED(ret)) {
        if(apvctx->stat.write > 0) {
            ret = ff_get_encode_buffer(avctx, avpkt, apvctx->stat.write, 0);
            if (ret < 0)
                return ret;

            memcpy(avpkt->data, apvctx->bitb.addr, apvctx->stat.write);

            avpkt->time_base.num = apvctx->cdsc.param->fps_num;
            avpkt->time_base.den = apvctx->cdsc.param->fps_den;

            avpkt->pts = avpkt->dts = frame->pts;
            avpkt->flags |= AV_PKT_FLAG_KEY;

            ff_side_data_set_encoder_stats(avpkt, apvctx->qp * FF_QP2LAMBDA, NULL, 0, AV_PICTURE_TYPE_I);

            *got_packet = 1;
        } else {
            *got_packet = 0;
        }
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
    ApvEncContext *apvctx = avctx->priv_data;
    (void)apvctx;

    if(apvctx->imgb_r != NULL)
        apvctx->imgb_r->release(apvctx->imgb_r);

    for(int i = 0; i < apvctx->num_frames; i++) {
        if(apvctx->ifrms.frm[i].imgb != NULL) {
            apvctx->ifrms.frm[i].imgb->release(apvctx->ifrms.frm[i].imgb);
        }
    }

    if (apvctx->mid) {
        oapvm_rem_all(apvctx->mid);
    }

    if (apvctx->id) {
        oapve_delete(apvctx->id);
        apvctx->id = NULL;
    }

    if (apvctx->mid) {
        oapvm_delete(apvctx->mid);
        apvctx->mid = NULL;
    }

    av_free(apvctx->bitb.addr); /* release bitstream buffer */

    return 0;
}

#define OFFSET(x) offsetof(ApvEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const enum AVPixelFormat supported_pixel_formats[] = {
    AV_PIX_FMT_YUV422P10,
    AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV422P12,
    AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_NONE
};

// Consider using following options (./ffmpeg --help encoder=liboapv)
//
// Using oapv-params:
// ffmpeg -f rawvideo -pix_fmt yuv422p10le -i ${INPUT_FILE} -c:v liboapv -oapv-params "profile=422-10:level=7.1:band=3:preset=medium:width=352:height=288:fps=24:qp=63:bitrate=1M" -f rawvideo ${OUTPUT_FILE}
//
static const AVOption liboapv_options[] = {
    { "preset", "Encoding preset for setting encoding speed (optimization level control)", OFFSET(preset_id), AV_OPT_TYPE_INT, { .i64 = OAPV_PRESET_DEFAULT }, OAPV_PRESET_FASTEST, OAPV_PRESET_PLACEBO, VE, .unit = "preset" },
    { "fastest", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_FASTEST },    INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "fast",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_FAST },    INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "medium",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_MEDIUM },  INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "slow",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_SLOW },    INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "placebo", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_PLACEBO }, INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_DEFAULT }, INT_MIN, INT_MAX, VE, .unit = "preset" },

    { "qp", "Quantization parameter value for CQP rate control mode", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },
    { "oapv-params",  "Override the apv configuration using a :-separated list of key=value parameters", OFFSET(oapv_params), AV_OPT_TYPE_DICT, { 0 }, 0, 0, VE },
    { NULL }
};

static const AVClass liboapve_class = {
    .class_name = "liboapv",
    .item_name  = av_default_item_name,
    .option     = liboapv_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/**
 *  libavcodec generic global options, which can be set on all the encoders and decoders
 *  @see https://www.ffmpeg.org/ffmpeg-codecs.html#Codec-Options
 */
static const FFCodecDefault liboapve_defaults[] = {
    { "b", "0" },       // bitrate in terms of kilo-bits per second (support for bit-rates from a few hundred Mbps to a few Gbps for 2K, 4K and 8K resolution content)
    { "threads", "0"},  // number of threads to be used (0: automatically select the number of threads to set)
    { NULL },
};

const FFCodec ff_liboapv_encoder = {
    .p.name             = "apv",
    .p.long_name        = NULL_IF_CONFIG_SMALL("OpenAPV / Open Advanced Professional Video"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_APV,
    .init               = liboapve_init,
    FF_CODEC_ENCODE_CB(liboapve_encode),
    .close              = liboapve_close,
    .priv_data_size     = sizeof(ApvEncContext),
    .p.priv_class       = &liboapve_class,
    .defaults           = liboapve_defaults,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_DR1,
    .p.wrapper_name     = "liboapv",
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_apv_profiles),
    .p.pix_fmts         = supported_pixel_formats,
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_NOT_INIT_THREADSAFE,
};
