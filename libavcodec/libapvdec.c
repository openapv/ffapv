/*
 * APV (Advanced Professional Video codec) decoding using APV codec library (libapv)
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
#include "libavutil/imgutils.h"
#include "libavutil/cpu.h"

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"
#include "codec_internal.h"
#include "profiles.h"
#include "decode.h"
#include "apv.h"

#define APV_ALIGN_VAL(val, align) ((((val)+(align)-1)/(align))*(align))

/**
 * The structure stores all the states associated with the instance of APV decoder
 */
typedef struct ApvDecContext {
    const AVClass *class;

    apvd_t id;            // apvd instance identifier @see apvd_t.h
    apvd_cdsc_t cdsc;     // decoding parameters @see apvd_t.h

    AVPacket *pkt;        // frame data
} ApvDecContext;

/**
 * The function populates the apvd_cdsc structure.
 * apvd_cdsc contains all decoder parameters that should be initialized before its use.
 *
 * @param[in] avctx codec context
 * @param[out] cdsc contains all decoder parameters that should be initialized before its use
 *
 */
static void get_conf(AVCodecContext *avctx, apvd_cdsc_t *cdsc)
{
    int cpu_count = av_cpu_count();

    /* clear apvd_cdsc structure */
    memset(cdsc, 0, sizeof(apvd_cdsc_t));

    /* init apvd_cdsc structure */
    if (avctx->thread_count <= 0)
        cdsc->threads = (cpu_count < APV_MAX_THREADS) ? cpu_count : APV_MAX_THREADS;
    else if (avctx->thread_count > APV_MAX_THREADS)
        cdsc->threads = APV_MAX_THREADS;
    else
        cdsc->threads = avctx->thread_count;
}

/**
 * @param[in] info the structure that stores information of bitstream
 * @param[out] avctx codec context
 * @return 0 on success, negative value on failure
 */
static int export_stream_params(const apvd_info_t* info, AVCodecContext *avctx)
{
    avctx->pix_fmt = info->cs;
    avctx->width = info->w;
    avctx->height = info->h;

    return 0;
}

/**
 * @brief Copy image in imgb to frame.
 *
 * @param avctx codec context
 * @param[in] imgb
 * @param[out] frame
 * @return 0 on success, negative value on failure
 */
static int libapvd_image_copy(struct AVCodecContext *avctx, apv_imgb_t *imgb, struct AVFrame *frame)
{
    int ret;
    if (imgb->cs != APV_CS_YCBCR422_10LE) {
        av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
        return AVERROR_INVALIDDATA;
    }

    if (imgb->w[0] != avctx->width || imgb->h[0] != avctx->height) { // stream resolution changed
        if (ff_set_dimensions(avctx, imgb->w[0], imgb->h[0]) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot set new dimension\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (ret = ff_get_buffer(avctx, frame, 0) < 0)
        return ret;

    av_image_copy(frame->data, frame->linesize, (const uint8_t **)imgb->a,
                  imgb->s, avctx->pix_fmt,
                  imgb->w[0], imgb->h[0]);

    return 0;
}

/**
 * @brief Create image
 *
 * @param w width
 * @param h height
 * @param cs color space
 * @return apv_imgb_t*
 */
static apv_imgb_t * imgb_create(int w, int h, int cs, struct AVCodecContext *avctx)
{
    int i, bd;
    apv_imgb_t * imgb;

    imgb = (apv_imgb_t *)malloc(sizeof(apv_imgb_t));
    if(imgb == NULL) goto ERR;
    memset(imgb, 0, sizeof(apv_imgb_t));

    bd = APV_CS_GET_BYTE_DEPTH(cs); /* byte unit */

    imgb->w[0] = w;
    imgb->h[0] = h;
    switch(APV_CS_GET_FORMAT(cs))
    {
    case APV_CF_YCBCR400:
        imgb->w[1] = imgb->w[2] = w;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 1;
        break;
    case APV_CF_YCBCR420:
        imgb->w[1] = imgb->w[2] = (w + 1) >> 1;
        imgb->h[1] = imgb->h[2] = (h + 1) >> 1;
        imgb->np = 3;
        break;
    case APV_CF_YCBCR422:
        imgb->w[1] = imgb->w[2] = (w + 1) >> 1;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 3;
        break;
    case APV_CF_YCBCR444:
        imgb->w[1] = imgb->w[2] = w;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 3;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "unsupported color format\n");
        goto ERR;
    }

    for(i = 0; i < imgb->np; i++)
    {
        imgb->aw[i] = APV_ALIGN_VAL(imgb->w[i], APV_MB_W);
        imgb->s[i] = imgb->aw[i] * bd;
        imgb->ah[i] = APV_ALIGN_VAL(imgb->h[i], APV_MB_H);
        imgb->e[i] = imgb->ah[i];

        imgb->bsize[i] = imgb->s[i] * imgb->e[i];
        imgb->a[i] = imgb->baddr[i] = malloc(imgb->bsize[i]);
        if(imgb->a[i] == NULL) goto ERR;

        memset(imgb->a[i], 0, imgb->bsize[i]);
    }
    imgb->cs = cs;

    imgb->addref(imgb); /* increase reference count */
    return imgb;

ERR:
    av_log(avctx, AV_LOG_ERROR, "cannot create image buffer\n");
    if(imgb)
    {
        for (int i = 0; i < APV_IMGB_MAX_PLANE; i++)
        {
            if(imgb->a[i]) free(imgb->a[i]);
        }
        free(imgb);
    }
    return NULL;
}

/**
 * Initialize decoder
 * Create a decoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int libapvd_init(AVCodecContext *avctx)
{
    ApvDecContext *apvctx = avctx->priv_data;
    apvd_cdsc_t *cdsc = &(apvctx->cdsc);

    /* read configurations from AVCodecContext and populate the apvd_cdsc structure */
    get_conf(avctx, cdsc);

    /* create decoder instance */
    apvctx->id = apvd_create(&(apvctx->cdsc), NULL);
    if (apvctx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create apvd decoder\n");
        return AVERROR_EXTERNAL;
    }

    apvctx->pkt = av_packet_alloc();

    return 0;
}

/**
  * Decode frame with decoupled packet/frame dataflow
  *
  * @param avctx codec context
  * @param[out] frame decoded frame
  *
  * @return 0 on success, negative error code on failure
  */
static int libapvd_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    ApvDecContext *apvctx = avctx->priv_data;
    AVPacket *pkt = apvctx->pkt;
    int ret = 0;

    // frame data (input data)
    ret = ff_decode_get_packet(avctx, pkt);
    if (ret < 0 && ret != AVERROR_EOF) {
        av_packet_unref(pkt);

        return ret;
    }

    if (pkt->size > 0) {
        int bs_read_pos = 0;
        int bs_size = 0;
        unsigned char *bs_buf = NULL;

        apvd_stat_t stat;
        apv_bitb_t bitb;
        apvd_info_t info;
        int fd_size;
        AVPacket* pkt_fd; // frame data
        apv_imgb_t *imgb = NULL;

        pkt_fd = av_packet_clone(pkt);
        av_packet_unref(pkt);

        // get all data frames
        while(pkt_fd->size > (bs_read_pos + APV_FRAME_DATA_SIZE_PREFIX_LENGTH)) {
            memset(&stat, 0, sizeof(apvd_stat_t));

            bs_buf = pkt_fd->data + bs_read_pos;

            if (APV_FAILED(apvd_info(bs_buf, APV_FRAME_DATA_SIZE_PREFIX_LENGTH, &info)))
            {
                av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
                av_packet_free(&pkt_fd);

                ret = AVERROR_INVALIDDATA;

                return ret;
            }
            bs_read_pos += APV_FRAME_DATA_SIZE_PREFIX_LENGTH;
            fd_size = info.frame_size;
            bs_size = APV_FRAME_DATA_SIZE_PREFIX_LENGTH + fd_size;

            if (APV_FAILED(apvd_info(bs_buf, bs_size, &info)))
            {
                av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
                av_packet_free(&pkt_fd);

                ret = AVERROR_INVALIDDATA;

                return ret;
            }
            bs_read_pos += fd_size;

            if (imgb == NULL) {
                imgb = imgb_create(info.w, info.h, info.cs, avctx);
                if (imgb == NULL) {
                    av_log(avctx, AV_LOG_ERROR, "cannot allocate image buffer (w:%d, h:%d, cs:%d)\n", info.w, info.h, info.cs);
                    ret = AVERROR_INVALIDDATA;

                    return ret;
                }
            }

            /* main decoding block */
            bitb.addr = bs_buf;
            bitb.ssize = bs_size;
            bitb.ts[0] = pkt_fd->dts;
            memset(&stat, 0, sizeof(apvd_stat_t));

            ret = apvd_decode(apvctx->id, &bitb, imgb, &stat);
            if (APV_FAILED(ret)) {
                av_log(avctx, AV_LOG_ERROR, "Failed to decode bitstream\n");
                av_packet_free(&pkt_fd);
                ret = AVERROR_EXTERNAL;

                return ret;
            }

            if ((ret = export_stream_params(&info, avctx)) != 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to export stream params\n");
                    av_packet_free(&pkt_fd);

                    return ret;
            }

            if (stat.read != bs_size)
                av_log(avctx, AV_LOG_INFO, "Different reading of bitstream (in:%d, read:%d)\n,", bs_size, stat.read);

            if (stat.avail) {

                ret = libapvd_image_copy(avctx, imgb, frame);
                if(ret < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Image copying error\n");

                    av_packet_free(&pkt_fd);

                    imgb->release(imgb);
                    imgb = NULL;

                    av_frame_unref(frame);

                    return ret;
                }

                // use ff_decode_frame_props_from_pkt() to fill frame properties
                ret = ff_decode_frame_props_from_pkt(avctx, frame, pkt_fd);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR, "ff_decode_frame_props_from_pkt error\n");

                    av_packet_free(&pkt_fd);

                    imgb->release(imgb);
                    imgb = NULL;

                    av_frame_unref(frame);

                    return ret;
                }

                frame->pkt_dts = imgb->ts[0];
                frame->pts = imgb->ts[0];

                // apvd_t_pull uses pool of objects of type apv_imgb.
                // The pool size is equal MAX_PB_SIZE (26), so release object when it is no more needed
                imgb->release(imgb);
                imgb = NULL;

                if(bs_read_pos == pkt->size) {
                    av_packet_free(&pkt_fd);

                    av_frame_unref(frame);
                    return 0;
                }
            }
        }
    }
    return ret;
}

/**
 * Destroy decoder
 *
 * @param avctx codec context
 * @return 0 on success
 */
static av_cold int libapvd_close(AVCodecContext *avctx)
{
    ApvDecContext *apvctx = avctx->priv_data;
    if (apvctx->id) {
        apvd_delete(apvctx->id);
        apvctx->id = NULL;
    }

    av_packet_free(&apvctx->pkt);

    return 0;
}

#define OFFSET(x) offsetof(ApvDecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVClass libapvd_class = {
    .class_name = "libapvd",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_libapv_decoder = {
    .p.name             = "apv",
    .p.long_name        = NULL_IF_CONFIG_SMALL("APV / Advanced Professional Video"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_APV,
    .init               = libapvd_init,
    FF_CODEC_RECEIVE_FRAME_CB(libapvd_receive_frame),
    .close              = libapvd_close,
    .priv_data_size     = sizeof(ApvDecContext),
    .p.priv_class       = &libapvd_class,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_AVOID_PROBING,
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_apv_profiles),
    .p.wrapper_name     = "libapvd",
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_PKT_DTS | FF_CODEC_CAP_SETS_FRAME_PROPS
};
