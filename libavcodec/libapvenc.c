/*
 * libapve encoder
 * Advanced Professional Video codec library
 *
 * Copyright (C) 2023 Dawid Kozinski <d.kozinski@samsung.com>
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
    
    int profile_id;         // encoder profile (33,44,55,66,77,88,99)
    int preset_id;          // preset of apv ( fastest, fast, medium, slow, placebo)
    int level_idc;
    int band_idc;

    int tune_id;            // tune of apv (psnr, zerolatency)

    // variables for rate control types
    int rc_type;            // Rate control type [ 0(OFF) / 1(ABR) / 2(CRF) ]

    int qp;                 // quantization parameter (QP) [0,51]
    int crf;                // constant rate factor (CRF) [10,49]

    int hash;               // embed picture signature (HASH) for conformance checking in decoding

    int complexity;         // encoder complexity [ 0(no rdo) / 1( enable rdo quantization daed-zone) ]

    int input_depth;        // input data bit depth (8, 10)
    int input_csp;          // input data color space (chroma format)
                            //  - 0: YUV400
                            //  - 1: YUV420
                            //  - 2: YUV422
                            //  - 3: YUV444

    int qp_c1_offset;
    int qp_c2_offset;
    int qp_c3_offset;

    int tile_w_mb;
    int tile_h_mb;

    char q_matrix_c0[512];
    char q_matrix_c1[512];
    char q_matrix_c2[512];
    char q_matrix_c3[512];

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
static int libapve_apv_color_format(enum AVPixelFormat av_pix_fmt)
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
static int libapve_apv_color_space(enum AVPixelFormat av_pix_fmt)
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

    apvctx = avctx->priv_data;

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
            // fps can be float number, but apv API doesn't support it
            // cdsc->param[i].fps = lrintf(av_q2d(avctx->framerate));
            cdsc->param[i].fps_num = avctx->framerate.num;
            cdsc->param[i].fps_den = avctx->framerate.den;
        }

        cdsc->param[i].level_idc = avctx->level;

        //if (avctx->rc_buffer_size)   // VBV buf size
        //    cdsc->param[i].vbv_bufsize = (int)(avctx->rc_buffer_size / 1000);

        cdsc->param[i].rc_type = apvctx->rc_type;

        cdsc->param[i].preset = apvctx->preset_id;
        cdsc->param[i].profile_idc = apvctx->profile_id;
        cdsc->param[i].level_idc = apvctx->level_idc;
        cdsc->param[i].band_idc = apvctx->band_idc;
        
        if (apvctx->rc_type == OAPV_RC_CQP)
            cdsc->param[i].qp = apvctx->qp;
        else if (apvctx->rc_type == OAPV_RC_ABR) {
            if (avctx->bit_rate / 1000 > INT_MAX || avctx->rc_max_rate / 1000 > INT_MAX) {
                av_log(avctx, AV_LOG_ERROR, "Not supported bitrate bit_rate and rc_max_rate > %d000\n", INT_MAX);
                return AVERROR_INVALIDDATA;
            }
            cdsc->param[i].bitrate = (int)(avctx->bit_rate / 1000);
        } else {
            av_log(avctx, AV_LOG_ERROR, "Not supported rate control type: %d\n", apvctx->rc_type);
            return AVERROR_INVALIDDATA;
        }

        if (avctx->thread_count <= 0) {
            int cpu_count = av_cpu_count();
            cdsc->threads = (cpu_count < OAPV_MAX_THREADS) ? cpu_count : OAPV_MAX_THREADS;
        } else if (avctx->thread_count > OAPV_MAX_THREADS)
            cdsc->threads = OAPV_MAX_THREADS;
        else
            cdsc->threads = avctx->thread_count;
    }

    apvctx->input_csp = libapve_apv_color_space(avctx->pix_fmt);
    if(apvctx->input_csp == OAPV_CS_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name (avctx->pix_fmt));
        return AVERROR_INVALIDDATA;
    }

    cdsc->max_bs_buf_size = MAX_BS_BUF; /* maximum bitstream buffer size */
    cdsc->max_num_frms = MAX_NUM_FRMS;

    return 0;
}

/**
 * Set OAPV_CFG_SET_USE_FRM_HASH for encoder
 *
 * @param[in] logger context
 * @param[in] id APV encodec instance identifier
 * @param[in] ctx the structure stores all the states associated with the instance of APV encoder
 *
 * @return 0 on success, negative error code on failure
 */
static int set_extra_config(AVCodecContext *avctx, oapvd_t id, ApvEncContext *ctx)
{
    int ret = 0, size, value;

    if(ctx->hash) {
        size = 4;
        value = 1;
        ret = oapve_config(id, OAPV_CFG_SET_USE_FRM_HASH, &value, &size);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set config for using frame hash\n");
            return AVERROR_EXTERNAL;
        }
    }

    return ret;
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
 * @brief Initialize APV codec
 * Create an encoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int libapve_init(AVCodecContext *avctx)
{
    ApvEncContext *apvctx = avctx->priv_data;
    unsigned char *bs_buf = NULL;

    int cfmt;                               // color format

    oapve_cdesc_t *cdsc =  &(apvctx->cdsc);
    int ret = 0;

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
        av_log(avctx, AV_LOG_ERROR, "Cannot get OAPV configuration\n");
        return AVERROR(EINVAL);
    }

    {
        const AVDictionaryEntry *en = NULL;
        while (en = av_dict_iterate(apvctx->oapv_params, en)) {
            av_log(avctx, AV_LOG_WARNING, "-oapv-params not supported yet. Error parsing option '%s = %s'.\n", en->key, en->value);
        }
    }

    /* create encoder */
    apvctx->id = oapve_create(cdsc, NULL);
    if (apvctx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create OAPV encoder\n");
        return AVERROR_EXTERNAL;
    }

    /* create metadata handler */
    apvctx->mid = oapvm_create(&ret);
    if(apvctx->mid == NULL || OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "cannot create OAPV metadata handler\n");
        return AVERROR_EXTERNAL;
    }

    if ((ret = set_extra_config(avctx, apvctx->id, apvctx)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot set extra configuration\n");
        return AVERROR(EINVAL);
    }

    apvctx->input_depth = get_bit_depth(avctx, avctx->pix_fmt);
    if(apvctx->input_depth != 10 && apvctx->input_depth != 12)  {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format (%s)n", av_get_pix_fmt_name(avctx->pix_fmt));
        return AVERROR(EINVAL); 
    }

    apvctx->imgb_r = NULL; // image buffer for read
    apvctx->imgb_i = NULL; // image buffer for input
    apvctx->num_frames = MAX_NUM_FRMS; // number of frames in an access unit

    cfmt = libapve_apv_color_format(avctx->pix_fmt);

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
static int libapve_encode(AVCodecContext *avctx, AVPacket *avpkt,
                          const AVFrame *frame, int *got_packet)
{
    ApvEncContext *apvctx =  avctx->priv_data;
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
    AVFrame* tmp_frame = copy_and_align_avframe_to_16(frame);
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
    
    // @todo Find out more on the last param, on how can we use it - reconstructed image
    //
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

            avpkt->pts = avpkt->dts = frame->pts;  // @todo provide implementation in APV apvctx->bitb.ts[0];
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
static av_cold int libapve_close(AVCodecContext *avctx)
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
    
    oapvm_rem_all(apvctx->mid);

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
static const AVOption liboapv_options[] = {
    { "preset", "Encoding preset for setting encoding speed (optimization level control)", OFFSET(preset_id), AV_OPT_TYPE_INT, { .i64 = OAPV_PRESET_DEFAULT }, OAPV_PRESET_FASTEST, OAPV_PRESET_PLACEBO, VE, .unit = "preset" },
    { "fastest", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_FASTEST },    INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "fast",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_FAST },    INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "medium",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_MEDIUM },  INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "slow",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_SLOW },    INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "placebo", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_PLACEBO }, INT_MIN, INT_MAX, VE, .unit = "preset" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PRESET_DEFAULT }, INT_MIN, INT_MAX, VE, .unit = "preset" },

    { "profile", "Encoding profile", OFFSET(profile_id), AV_OPT_TYPE_INT, { .i64 = OAPV_PROFILE_422_10 }, OAPV_PROFILE_422_10,  99, VE, .unit = "profile" },
    { "422-10", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_PROFILE_422_10 }, INT_MIN, INT_MAX, VE, .unit = "profile" },
    { "422-12", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 44 }, INT_MIN, INT_MAX, VE, .unit = "profile" },
    { "444-10", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 55 }, INT_MIN, INT_MAX, VE, .unit = "profile" },
    { "444-12", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 66 }, INT_MIN, INT_MAX, VE, .unit = "profile" },
    { "4444-10", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 77 }, INT_MIN, INT_MAX, VE, .unit = "profile" },
    { "4444-12", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 88 }, INT_MIN, INT_MAX, VE, .unit = "profile" },
    { "400-10", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 99 }, INT_MIN, INT_MAX, VE, .unit = "profile" },

    // @see https://www.ietf.org/archive/id/draft-lim-apv-03.html#name-overview-of-profiles-levels
    // level_idc MUST be set equal to a value of 30 times the level number specified in Table 4
    //
    { "level", "level", OFFSET(level_idc), AV_OPT_TYPE_INT, { .i64 = (int)(4.1 * 30) }, 1,  (int)(7.1 * 30), VE, .unit = "level" },
    { "1",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(1 * 30) },   INT_MIN, INT_MAX, VE, .unit = "level" },
    { "1.1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(1.1 * 30) }, INT_MIN, INT_MAX, VE, .unit = "level" },
    { "2",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(2 * 30) },   INT_MIN, INT_MAX, VE, .unit = "level" },
    { "2.1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(2.1 * 30) }, INT_MIN, INT_MAX, VE, .unit = "level" },
    { "3",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(3 * 30) },   INT_MIN, INT_MAX, VE, .unit = "level" },
    { "3.1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(3.1 * 30) }, INT_MIN, INT_MAX, VE, .unit = "level" },
    { "4",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(4 * 30) },   INT_MIN, INT_MAX, VE, .unit = "level" },
    { "4.1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(4.1 * 30) }, INT_MIN, INT_MAX, VE, .unit = "level" },
    { "5",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(5 * 30) },   INT_MIN, INT_MAX, VE, .unit = "level" },
    { "5.1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(5.1 * 30) }, INT_MIN, INT_MAX, VE, .unit = "level" },
    { "6",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(6 * 30) },   INT_MIN, INT_MAX, VE, .unit = "level" },
    { "6.1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(6.1 * 30) }, INT_MIN, INT_MAX, VE, .unit = "level" },
    { "7",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(7 * 30) },   INT_MIN, INT_MAX, VE, .unit = "level" },
    { "7.1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (int)(7.1 * 30) }, INT_MIN, INT_MAX, VE, .unit = "level" },

    { "band-idc", "band_idc", OFFSET(band_idc), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, 3, VE },
    
    { "q-matrix-c0", "q_matrix_c0 \"q1 q2 ... q63 q64\" (not implemented yet)", OFFSET(q_matrix_c0), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },
    { "q-matrix-c1", "q_matrix_c1 \"q1 q2 ... q63 q64\" (not implemented yet)", OFFSET(q_matrix_c1), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },
    { "q-matrix-c2", "q_matrix_c2 \"q1 q2 ... q63 q64\" (not implemented yet)", OFFSET(q_matrix_c2), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },
    { "q-matrix-c3", "q_matrix_c3 \"q1 q2 ... q63 q64\" (not implemented yet)", OFFSET(q_matrix_c3), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },

    { "tile-w-mb", "Width of tile in units of MBs", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },
    { "tile-h-mb", "Height of tile in units of MBs", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },

    { "qp", "Quantization parameter value for CQP rate control mode", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },

    { "qp-offset-c1", "c1 qp offset (not implemented yet)", OFFSET(qp_c1_offset), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },
    { "qp-offset-c2", "c2 qp offset (not implemented yet)", OFFSET(qp_c2_offset), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },
    { "qp-offset-c3", "c3 qp offset (not implemented yet)", OFFSET(qp_c3_offset), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },

    { "rc-type", "Rate control type", OFFSET(rc_type), AV_OPT_TYPE_INT, { .i64 = OAPV_RC_ABR }, OAPV_RC_CQP,  OAPV_RC_ABR , VE, "rc_type" },
    { "CQP", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_RC_CQP }, INT_MIN, INT_MAX, VE, "rc_type" },
    { "ABR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OAPV_RC_ABR }, INT_MIN, INT_MAX, VE, "rc_type" },

    { "hash", "Embed picture signature (HASH) for conformance checking in decoding", OFFSET(hash), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },

    { "oapv-params",  "Override the apv configuration using a :-separated list of key=value parameters", OFFSET(oapv_params), AV_OPT_TYPE_DICT, { 0 }, 0, 0, VE },
    { NULL }
};

static const AVClass libapve_class = {
    .class_name = "liboapv",
    .item_name  = av_default_item_name,
    .option     = liboapv_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/**
 *  libavcodec generic global options, which can be set on all the encoders and decoders
 *  @see https://www.ffmpeg.org/ffmpeg-codecs.html#Codec-Options
 */
static const FFCodecDefault libapve_defaults[] = {
    { "b", "0" },       // bitrate in terms of kilo-bits per second (support for bit-rates from a few hundred Mbps to a few Gbps for 2K, 4K and 8K resolution content)
    { "threads", "0"},  // number of threads to be used (0: automatically select the number of threads to set)
    { NULL },
};

const FFCodec ff_libapv_encoder = {
    .p.name             = "liboapv",
    .p.long_name        = NULL_IF_CONFIG_SMALL("liboapv APV"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_APV,
    .init               = libapve_init,
    FF_CODEC_ENCODE_CB(libapve_encode),
    .close              = libapve_close,
    .priv_data_size     = sizeof(ApvEncContext),
    .p.priv_class       = &libapve_class,
    .defaults           = libapve_defaults,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_DR1,
    .p.wrapper_name     = "liboapv",
    .p.pix_fmts         = supported_pixel_formats,
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_NOT_INIT_THREADSAFE,
};
