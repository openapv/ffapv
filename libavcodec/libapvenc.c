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

#include <apv/apv.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"
#include "libavutil/cpu.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"
#include "codec_internal.h"
#include "profiles.h"
#include "encode.h"

#define MAX_BS_BUF (16*1024*1024)

/**
 * The structure stores all the states associated with the instance of APV encoder
 */
typedef struct ApvEncContext {
    const AVClass *class;

    apve_t id;            // APV instance identifier
    apve_cdsc_t cdsc;     // coding parameters i.e profile, width & height of input frame, num of therads, frame rate ...
    apv_bitb_t bitb;      // bitstream buffer (output)
    apve_stat_t stat;     // encoding status (output)
    apv_imgb_t imgb_inp;  // image buffer (input)

    // @todo check whether needed
    apv_imgb_t imgb_rec;  // recon image

    int profile_id;     // encoder profile (baseline)
                        // the first version of the APV codec defines a profile, Baseline profile,
                        // which supports 16x16 MB size and 8x8 transform size.

    int preset_id;      // preset of apv ( fast, medium, slow, placebo)
    int tune_id;        // tune of apv (psnr, zerolatency)

    // variables for rate control types
    int rc_type;        // Rate control type [ 0(OFF) / 1(ABR) / 2(CRF) ]

    int qp;             // quantization parameter (QP) [0,51]
    int crf;            // constant rate factor (CRF) [10,49]

    int hash;           // embed picture signature (HASH) for conformance checking in decoding

    int complexity;     // encoder complexity [ 0(no rdo) / 1( enable rdo quantization daed-zone) ]

    int input_depth;    // input data bit depth (8, 10)
    int input_csp;      // input data color space (chroma format)
                        //  - 0: YUV400
                        //  - 1: YUV420
                        //  - 2: YUV422
                        //  - 3: YUV444

    int qp_cb_offset;
    int qp_cr_offset;
    int tile_w_mb;
    int tile_h_mb;

    char q_matrix_y[512];
    char q_matrix_u[512];
    char q_matrix_v[512];

    AVDictionary *apve_params;
} ApvEncContext;

/**
 * Convert FFmpeg pixel format (AVPixelFormat) into APV pre-defined color space
 *
 * @param[in] px_fmt pixel format (@see https://ffmpeg.org/doxygen/trunk/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5)
 *
 * @return APV pre-defined color space (@see apv.h) on success, APV_CF_UNKNOWN on failure
 */
static int libapve_apv_color_space(enum AVPixelFormat av_pix_fmt)
{
    int cs = APV_CS_UNKNOWN;

    switch (av_pix_fmt) {
    case AV_PIX_FMT_YUV422P10:
        cs = APV_CS_SET(APV_CS_YCBCR422, 10, AV_HAVE_BIGENDIAN);
        break;
    case AV_PIX_FMT_YUV444P10:
        cs = APV_CS_SET(APV_CS_YCBCR444, 10, AV_HAVE_BIGENDIAN);
        break;
    case AV_PIX_FMT_YUV422P12:
        cs = APV_CS_SET(APV_CS_YCBCR422, 12, AV_HAVE_BIGENDIAN);
        break;
    case AV_PIX_FMT_YUV444P12:
        cs = APV_CS_SET(APV_CS_YCBCR444, 12, AV_HAVE_BIGENDIAN);
        break;
    default:
        cs = APV_CS_UNKNOWN;
        break;
    }

    return cs;
}

/**
 * The function returns a pointer to the object of the apve_cdsc_t type.
 * apve_cdsc_t contains all encoder parameters that should be initialized before the encoder is used.
 *
 * The field values of the apve_cdsc structure are populated based on:
 * - the corresponding field values of the AvCodecConetxt structure,
 * - the apv encoder specific option values,
 *   (the full list of options available for apv encoder is displayed after executing the command ./ffmpeg --help encoder = libapve)
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
static int get_conf(AVCodecContext *avctx, apve_cdsc_t *cdsc)
{
    ApvEncContext *apvctx = NULL;
    int ret;

    apvctx = avctx->priv_data;

    /* initialize apv_param struct with default values */
    ret = apve_param_default(&cdsc->param);
    if (APV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot set_default parameter\n");
        return AVERROR_EXTERNAL;
    }

    /* read options from AVCodecContext */
    if (avctx->width > 0)
        cdsc->param.w = avctx->width;

    if (avctx->height > 0)
        cdsc->param.h = avctx->height;

    if (avctx->framerate.num > 0) {
        // fps can be float number, but apv API doesn't support it
        cdsc->param.fps = lrintf(av_q2d(avctx->framerate));
    }

    cdsc->param.level_idc = avctx->level;

    if (avctx->rc_buffer_size)   // VBV buf size
        cdsc->param.vbv_bufsize = (int)(avctx->rc_buffer_size / 1000);

    cdsc->param.rc_type = apvctx->rc_type;

    if (apvctx->rc_type == APV_RC_CQP)
        cdsc->param.qp = apvctx->qp;
    else if (apvctx->rc_type == APV_RC_ABR) {
        if (avctx->bit_rate / 1000 > INT_MAX || avctx->rc_max_rate / 1000 > INT_MAX) {
            av_log(avctx, AV_LOG_ERROR, "Not supported bitrate bit_rate and rc_max_rate > %d000\n", INT_MAX);
            return AVERROR_INVALIDDATA;
        }
        cdsc->param.bitrate = (int)(avctx->bit_rate / 1000);
    } else if (apvctx->rc_type == APV_RC_CRF)
        cdsc->param.crf = apvctx->crf;
    else {
        av_log(avctx, AV_LOG_ERROR, "Not supported rate control type: %d\n", apvctx->rc_type);
        return AVERROR_INVALIDDATA;
    }

    if (avctx->thread_count <= 0) {
        int cpu_count = av_cpu_count();
        cdsc->param.threads = (cpu_count < APV_MAX_THREADS) ? cpu_count : APV_MAX_THREADS;
    } else if (avctx->thread_count > APV_MAX_THREADS)
        cdsc->param.threads = APV_MAX_THREADS;
    else
        cdsc->param.threads = avctx->thread_count;

    apvctx->input_csp = libapve_apv_color_space(avctx->pix_fmt);
    if(apvctx->input_csp == APV_CS_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name (avctx->pix_fmt));
        return AVERROR_INVALIDDATA;
    }

    cdsc->max_bs_buf_size = MAX_BS_BUF;

    return 0;
}

/**
 * Set APV_CFG_SET_USE_PIC_SIGNATURE for encoder
 *
 * @param[in] logger context
 * @param[in] id APV encodec instance identifier
 * @param[in] ctx the structure stores all the states associated with the instance of APV encoder
 *
 * @return 0 on success, negative error code on failure
 */
static int set_extra_config(AVCodecContext *avctx, apvd_t id, ApvEncContext *ctx)
{
    int ret, size, value;

    if(ctx->hash) {
        size = 4;
        value = 1;
        ret = apve_config(id, APV_CFG_SET_USE_PIC_SIGNATURE, &value, &size);
        if (APV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set config for picture signature\n");
            return AVERROR_EXTERNAL;
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
static av_cold int libapve_init(AVCodecContext *avctx)
{
    ApvEncContext *apvctx = avctx->priv_data;
    unsigned char *bs_buf = NULL;
    int i;
    int shift_h = 0;
    int shift_v = 0;
    int width_chroma = 0;
    int height_chroma = 0;
    apv_imgb_t *imgb_inp = NULL;

    apve_cdsc_t *cdsc =  &(apvctx->cdsc);
    int ret = 0;

    /* allocate bitstream buffer */
    bs_buf = av_malloc(MAX_BS_BUF);
    if (bs_buf == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot allocate bitstream buffer\n");
        return AVERROR(ENOMEM);
    }
    apvctx->bitb.addr = bs_buf;
    apvctx->bitb.bsize = MAX_BS_BUF;

    /* read configurations and set values for created descriptor (APV_CDSC) */
    if ((ret = get_conf(avctx, cdsc)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot get configuration\n");
        return AVERROR(EINVAL);
    }

    // @todo provide apve_param_check implementationAV_PIX_FMT_YUV422P10
    //
    // if ((ret = apve_param_check(&cdsc->param)) != 0) {
    //     av_log(avctx, AV_LOG_ERROR, "Invalid configuration\n");
    //     return AVERROR(EINVAL);
    // }

    // @todo provide apve_param_parse implementation
    //
    // {
    //     AVDictionaryEntry *en = NULL;
    //     while (en = av_dict_get(apvctx->apve_params, "", en, AV_DICT_IGNORE_SUFFIX)) {
    //         if ((ret = apve_param_parse(&cdsc->param, en->key, en->value)) < 0) {
    //             av_log(avctx, AV_LOG_WARNING,
    //                    "Error parsing option '%s = %s'.\n",
    //                    en->key, en->value);
    //         }
    //     }
    // }

    /* create encoder */
    apvctx->id = apve_create(cdsc, NULL);
    if (apvctx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create APV encoder\n");
        return AVERROR_EXTERNAL;
    }

    if ((ret = set_extra_config(avctx, apvctx->id, apvctx)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot set extra configuration\n");
        return AVERROR(EINVAL);
    }

    if ((ret = av_pix_fmt_get_chroma_sub_sample(avctx->pix_fmt, &shift_h, &shift_v)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get  chroma shift\n");
        return AVERROR(EINVAL);
    }

    // Chroma subsampling
    //
    // YUV format explanation
    // shift_h == 1 && shift_v == 1 : YUV420
    // shift_h == 1 && shift_v == 0 : YUV422
    // shift_h == 0 && shift_v == 0 : YUV444
    //
    width_chroma = AV_CEIL_RSHIFT(avctx->width, shift_h);
    height_chroma = AV_CEIL_RSHIFT(avctx->height, shift_v);

    /* set default values for input image buffer */
    imgb_inp = &apvctx->imgb_inp;
    imgb_inp->cs = libapve_apv_color_space(avctx->pix_fmt);
    imgb_inp->np = 3; /* only for yuv420p, yuv420ple */

    for (i = 0; i < imgb_inp->np; i++)
        imgb_inp->x[i] = imgb_inp->y[i] = 0;

    imgb_inp->w[0] = imgb_inp->aw[0] = avctx->width; // width luma
    imgb_inp->w[1] = imgb_inp->w[2] = imgb_inp->aw[1] = imgb_inp->aw[2] = width_chroma;
    imgb_inp->h[0] = imgb_inp->ah[0] = avctx->height; // height luma
    imgb_inp->h[1] = imgb_inp->h[2] = imgb_inp->ah[1] = imgb_inp->ah[2] = height_chroma;

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
    int i;

    apv_imgb_t *imgb_inp = NULL;

    imgb_inp = &apvctx->imgb_inp;

    if (frame==NULL) {
        return 0;
    }

    for (i = 0; i < imgb_inp->np; i++) {
        imgb_inp->a[i] = frame->data[i];
        imgb_inp->s[i] = frame->linesize[i];
    }

    imgb_inp->ts[0] = frame->pts;

    // @todo Find out more on the last param, on how can we use it - reconstructed image
    //
    ret = apve_encode(apvctx->id, imgb_inp, &(apvctx->bitb), &(apvctx->stat), NULL);
    if (APV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "xeve_push() failed\n");
        return AVERROR_EXTERNAL;
    }

    if(apvctx->stat.write > 0) {
        ret = ff_get_encode_buffer(avctx, avpkt, apvctx->stat.write, 0);
        if (ret < 0)
            return ret;

        memcpy(avpkt->data, apvctx->bitb.addr, apvctx->stat.write);

        avpkt->time_base.num = 1;
        avpkt->time_base.den = apvctx->cdsc.param.fps;

        avpkt->pts = avpkt->dts = apvctx->bitb.ts[0];

        ff_side_data_set_encoder_stats(avpkt, apvctx->stat.qp * FF_QP2LAMBDA, NULL, 0, AV_PICTURE_TYPE_I);

        *got_packet = 1;
    } else {
        *got_packet = 0;
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

    if (apvctx->id) {
        apve_delete(apvctx->id);
        apvctx->id = NULL;
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

// Consider using following options (./ffmpeg --help encoder=libapve)
//
static const AVOption libapve_options[] = {
    { "complexity", "Encoder complexity", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },
    { "nordo", "no rdo", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 1, VE, "complexity" },
    { "rdo",  "enable rdo quantization daed-zone", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 1, VE, "complexity" },

    { "q-matrix-y", "q_matrix_y \"q1 q2 ... q63 q64\"", OFFSET(q_matrix_y), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },
    { "q-matrix-u", "q_matrix_u \"q1 q2 ... q63 q64\"", OFFSET(q_matrix_u), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },
    { "q-matrix-v", "q_matrix_v \"q1 q2 ... q63 q64\"", OFFSET(q_matrix_v), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },

    { "tile-w-mb", "Width of tile in units of MBs", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },
    { "tile-h-mb", "Height of tile in units of MBs", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },

    { "qp-cb-offset", "cb qp offset", OFFSET(qp_cb_offset), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },
    { "qp-cr-offset", "cr qp offset", OFFSET(qp_cr_offset), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },

    { "qp-cr-offset", "cr qp offset", OFFSET(qp_cr_offset), AV_OPT_TYPE_INT, { .i64 = 0 }, INT_MIN, INT_MAX, VE },

    { "rc_type", "Rate control type", OFFSET(rc_type), AV_OPT_TYPE_INT, { .i64 = APV_RC_CQP }, APV_RC_CQP,  APV_RC_CRF, VE, "rc_type" },
    { "CQP", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = APV_RC_CQP }, INT_MIN, INT_MAX, VE, "rc_type" },
    { "ABR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = APV_RC_ABR }, INT_MIN, INT_MAX, VE, "rc_type" },
    { "CRF", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = APV_RC_CRF }, INT_MIN, INT_MAX, VE, "rc_type" },

    { "qp", "Quantization parameter value for CQP rate control mode", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },
    { "crf", "Constant rate factor value for CRF rate control mode", OFFSET(crf), AV_OPT_TYPE_INT, { .i64 = 32 }, 10, 49, VE },

    { "hash", "Embed picture signature (HASH) for conformance checking in decoding", OFFSET(hash), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },

    { "apve-params",  "Override the apv configuration using a :-separated list of key=value parameters", OFFSET(apve_params), AV_OPT_TYPE_DICT, { 0 }, 0, 0, VE },
    { NULL }
};

static const AVClass libapve_class = {
    .class_name = "libapve",
    .item_name  = av_default_item_name,
    .option     = libapve_options,
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
    .p.name             = "libapve",
    .p.long_name        = NULL_IF_CONFIG_SMALL("libapve APV"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_APV,
    .init               = libapve_init,
    FF_CODEC_ENCODE_CB(libapve_encode),
    .close              = libapve_close,
    .priv_data_size     = sizeof(ApvEncContext),
    .p.priv_class       = &libapve_class,
    .defaults           = libapve_defaults,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_DR1,
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_apv_profiles),
    .p.wrapper_name     = "libapve",
    .p.pix_fmts         = supported_pixel_formats,
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_NOT_INIT_THREADSAFE,
};
