/*
 * APV (Advanced Professional Video codec) decoding using APV codec library (liboapv)
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
#include "libavutil/imgutils.h"
#include "libavutil/cpu.h"
#include "libavutil/container_fifo.h"

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"
#include "codec_internal.h"
#include "profiles.h"
#include "decode.h"
#include "apv.h"
#include "apv_imgb.h"

#define APV_AU_SIZE_PREFIX_LENGTH (4)

/**
 * The structure stores all the states associated with the instance of APV decoder
 */
typedef struct ApvDecContext {
    const AVClass *class;

    oapvd_t id;             // apvd instance identifier @see apvd_t.h
    oapvd_cdesc_t cdsc;     // decoding parameters @see apvd_t.h

    oapvm_t mid;            //  OAPV metadata container

    int hash;               // embed picture signature (HASH) for conformance checking in decoding

    int output_depth;
    int output_csp;

    struct AVContainerFifo *output_fifo;
    AVFrame* frames[OAPV_MAX_NUM_FRAMES];
    
    int frames_count;
    int total_frames_count;
    int au_count;
    
    AVPacket *pkt;          // frame data
} ApvDecContext;

/**
 * The function populates the apvd_cdsc structure.
 * apvd_cdsc contains all decoder parameters that should be initialized before its use.
 *
 * @param[in] avctx codec context
 * @param[out] cdsc contains all decoder parameters that should be initialized before its use
 *
 */
static void get_conf(AVCodecContext *avctx, oapvd_cdesc_t *cdsc)
{
    /* clear apvd_cdsc structure */
    memset(cdsc, 0, sizeof(oapvd_cdesc_t));

    /* init apvd_cdsc structure */
    cdsc->threads = OAPV_CDESC_THREADS_AUTO;
}

static int set_extra_config(AVCodecContext *avctx, oapvd_t id, ApvDecContext *ctx)
{
    int ret = 0, size, value;

    if(ctx->hash) {
        size = 4;
        value = 1;
        ret = oapvd_config(id, OAPV_CFG_SET_USE_FRM_HASH, &value, &size);
        if (OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set config for using frame hash\n");
            return AVERROR_EXTERNAL;
        }
    }
    return ret;
}

/**
 * @param[in] info the structure that stores information of bitstream
 * @param[out] avctx codec context
 * @return 0 on success, negative value on failure
 */
static int export_stream_params(const oapv_au_info_t* aui, AVCodecContext *avctx)
{
    avctx->width = aui->frm_info->w;
    avctx->height = aui->frm_info->h;

    switch(aui->frm_info->cs) {
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 0):  // profile 33
        avctx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUV422P10BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 0):  // profile 44
        avctx->pix_fmt = AV_PIX_FMT_YUV422P12LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUV422P12BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 10, 0):  // profile 55
        avctx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 10, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUV444P10BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 0):   // profile 66
        avctx->pix_fmt = AV_PIX_FMT_YUV444P12LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUV444P12BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 10, 0):  // profile 77
        avctx->pix_fmt = AV_PIX_FMT_YUVA444P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 10, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUVA444P10BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 12, 0):  // profile 88
        avctx->pix_fmt = AV_PIX_FMT_YUVA444P12LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 12, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUVA444P12BE;
        break;        
    case OAPV_CS_SET(OAPV_CF_YCBCR400, 10, 0):   // profile 99
        avctx->pix_fmt = AV_PIX_FMT_GRAY10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR400, 10, 1):
        avctx->pix_fmt = AV_PIX_FMT_GRAY10BE;
        break;    
    default:
        av_log(avctx, AV_LOG_ERROR, "Unknown color space\n");
        avctx->pix_fmt = AV_PIX_FMT_NONE;
        return AVERROR_INVALIDDATA;
    }

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
static int libapvd_image_copy(struct AVCodecContext *avctx, oapv_imgb_t *imgb, struct AVFrame *frame, oapv_frm_info_t *frm_info)
{
    int ret;

    if (imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 0)  && // profile 33
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 1)  && // profile 33 
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 0)  && // profile 44
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 1)  && // profile 44
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR444, 10, 0)  && // profile 55
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR444, 10, 1)  && // profile 55
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 0)  && // profile 66
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 1)  && // profile 66
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR4444, 10, 0) && // profile 77
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR4444, 10, 1) && // profile 77
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR4444, 12, 0) && // profile 88
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR4444, 12, 1) && // profile 88
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR400, 10, 0)  && // profile 99
        imgb->cs != OAPV_CS_SET(OAPV_CF_YCBCR400, 10, 1)) {  // profile 99 
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
    
    
    if (frm_info->color_description_present_flag) {
        avctx->color_primaries = frm_info->color_primaries;
        avctx->color_trc = frm_info->transfer_characteristics;
        avctx->colorspace = frm_info->matrix_coefficients;
        avctx->color_range = frm_info->full_range_flag ? AVCOL_RANGE_UNSPECIFIED : AVCOL_RANGE_MPEG;
    }

    return 0;
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
    oapvd_cdesc_t *cdsc = &(apvctx->cdsc);
    int ret = 0;

    /* read configurations from AVCodecContext and populate the apvd_cdsc structure */
    get_conf(avctx, cdsc);

    /* create decoder instance */
    apvctx->id = oapvd_create(&(apvctx->cdsc), NULL);
    if (apvctx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create apvd decoder\n");
        return AVERROR_EXTERNAL;
    }
        
    /* create metadata container */
    apvctx->mid = oapvm_create(&ret);
    if(OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "ERROR: cannot create OAPV metadata container (err=%d)\n", ret);
        return AVERROR_EXTERNAL;
    }

    if ((ret = set_extra_config(avctx, apvctx->id, apvctx)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot set extra configuration\n");
        return AVERROR(EINVAL);
    }

    apvctx->pkt = av_packet_alloc();

    // Allocate an AVContainerFifo instance for AVFrames
    apvctx->output_fifo = av_container_fifo_alloc_avframe(0);
    if (!apvctx->output_fifo)
        return AVERROR(ENOMEM);

    for (int i = 0; i < FF_ARRAY_ELEMS(apvctx->frames); i++) {
        apvctx->frames[i] = NULL;
    }

    apvctx->frames_count = 0;
    apvctx->total_frames_count = 0;
    apvctx->au_count = 0;

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

    uint8_t *bs_buf = NULL;
    uint32_t bs_buf_size = 0;

    oapvd_stat_t stat;
    oapv_bitb_t bitb;
    oapv_frms_t ofrms;
    oapv_imgb_t *imgb_w = NULL;
    oapv_imgb_t *imgb_o = NULL;
    oapv_frm_t  *frm = NULL;

    oapv_au_info_t aui;
    oapv_frm_info_t *finfo = NULL;

    AVPacket* pkt_fd; // encoded frame data

    if (av_container_fifo_can_read(apvctx->output_fifo))
        goto do_output;

    for(int i =0; i<apvctx->frames_count;i++ ) {
        av_frame_unref(apvctx->frames[i]);
        apvctx->frames_count = 0;
    }

    // frame data (input data)
    ret = ff_decode_get_packet(avctx, pkt);
    if (ret < 0 && ret != AVERROR_EOF) {
        av_packet_unref(pkt);
        return ret;
    }

    if (pkt->size <= 0) {
        av_packet_unref(pkt);
        return ret;
    }   
    
    memset(&ofrms, 0, sizeof(oapv_frms_t));
    memset(&aui, 0, sizeof(oapv_au_info_t));

    pkt_fd = av_packet_clone(pkt);
    av_packet_unref(pkt);

    bs_buf = pkt_fd->data + APV_AU_SIZE_PREFIX_LENGTH;
    bs_buf_size = pkt_fd->size - APV_AU_SIZE_PREFIX_LENGTH;

    if (OAPV_FAILED(oapvd_info(bs_buf, bs_buf_size, &aui)))
    {
        av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    if ((ret = export_stream_params(&aui, avctx)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to export stream params\n");
        goto end;
    }

    /* create decoding frame buffers */
    ofrms.num_frms = aui.num_frms;
    for(int i = 0; i < ofrms.num_frms; i++) {

        finfo = &aui.frm_info[i];

        if(apvctx->output_csp == 1) {
            ofrms.frm[i].imgb = apv_imgb_create(finfo->w, finfo->h, OAPV_CS_SET(OAPV_CF_PLANAR2, 10, 0), avctx);
        } else {
            ofrms.frm[i].imgb = apv_imgb_create(finfo->w, finfo->h, finfo->cs, avctx);
        }

        if(ofrms.frm[i].imgb == NULL) {
            av_log(avctx, AV_LOG_ERROR, "cannot allocate image buffer (w:%d, h:%d, cs:%d)\n",
                    finfo->w, finfo->h, finfo->cs);

            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }

    if(apvctx->output_depth == 0) {
        apvctx->output_depth = OAPV_CS_GET_BIT_DEPTH(finfo->cs);
    }

    /* main decoding block */
    bitb.addr = bs_buf;
    bitb.ssize = bs_buf_size;
    memset(&stat, 0, sizeof(oapvd_stat_t));

    ret = oapvd_decode(apvctx->id, &bitb, &ofrms, apvctx->mid, &stat);
    if(OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR,"failed to decode bitstream\n");

        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if(stat.read != bs_buf_size) {
        av_log(avctx, AV_LOG_ERROR,"\t=> different reading of bitstream (in:%d, read:%d)\n",
                bs_buf_size, stat.read);
    }

    /* Testing of metadata reading */
    if(apvctx->mid) {
        oapvm_payload_t *pld = NULL;   // metadata payload
        int              num_plds = 0; // number of metadata payload

        ret = oapvm_get_all(apvctx->mid, NULL, &num_plds);

        if(OAPV_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR,"failed to read metadata\n");

            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if(num_plds > 0) {
            pld = malloc(sizeof(oapvm_payload_t) * num_plds);
            ret = oapvm_get_all(apvctx->mid, pld, &num_plds);
            if(OAPV_FAILED(ret)) {
                av_log(avctx, AV_LOG_ERROR,"failed to read metadata\n");

                if(pld != NULL)
                    free(pld);

                ret = AVERROR_INVALIDDATA;
                goto end;
            }
        }
        if(pld != NULL)
            free(pld);
    }

    /* Write decoded frames into AVFrame objects */
    for(int i = 0; i < ofrms.num_frms; i++) {
        frm = &ofrms.frm[i];
        if(OAPV_CS_GET_BIT_DEPTH(frm->imgb->cs) != apvctx->output_depth) {
            if(imgb_w == NULL) {
                imgb_w = apv_imgb_create(frm->imgb->w[0], frm->imgb->h[0],
                                        OAPV_CS_SET(OAPV_CS_GET_FORMAT(frm->imgb->cs), apvctx->output_depth, 0), avctx);
                if(imgb_w == NULL) {
                    av_log(avctx, AV_LOG_ERROR,"cannot allocate image buffer (w:%d, h:%d, cs:%d)\n",
                            frm->imgb->w[0], frm->imgb->h[0], frm->imgb->cs);

                    ret = AVERROR_INVALIDDATA;
                    goto end;
                }
            }
            apv_imgb_cpy(imgb_w, frm->imgb, avctx);

            imgb_o = imgb_w;
        }
        else {
            imgb_o = frm->imgb;
        }

        if(apvctx->frames[i] == NULL) {
            apvctx->frames[i] = av_frame_alloc();
        }
        
        /* Copy decoded image into AVFrame object */
        ret = libapvd_image_copy(avctx, imgb_o, apvctx->frames[i], &stat.aui.frm_info[i]);
        if(ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Image copying error\n");
            av_frame_unref(apvctx->frames[i]);

            goto end;
        }

        /* Use ff_decode_frame_props_from_pkt() to fill frame properties */
        ret = ff_decode_frame_props_from_pkt(avctx, apvctx->frames[i], pkt_fd);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_decode_frame_props_from_pkt error\n");
            av_frame_unref(apvctx->frames[i]);

            goto end;
        }

        if (pkt_fd->flags & AV_PKT_FLAG_KEY) {
            apvctx->frames[i]->pict_type = AV_PICTURE_TYPE_I;
            apvctx->frames[i]->flags |= AV_FRAME_FLAG_KEY;
        }
        
        apvctx->frames_count++;

        /* Write the AVFrame data to the FIFO */
        ret = av_container_fifo_write(apvctx->output_fifo, apvctx->frames[i], AV_CONTAINER_FIFO_FLAG_REF);
    }
    apvctx->total_frames_count += apvctx->frames_count;
    apvctx->au_count++;

end:
    av_packet_unref(pkt_fd);

    for(int i = 0; i < ofrms.num_frms; i++) {
        if(ofrms.frm[i].imgb != NULL) {
            ofrms.frm[i].imgb->release(ofrms.frm[i].imgb);
            ofrms.frm[i].imgb = NULL;
        }
    }
    if (imgb_w) {
        imgb_w->release(imgb_w);
        imgb_w = NULL;
    }
    
    imgb_o = NULL;

    if (av_container_fifo_can_read(apvctx->output_fifo))
        goto do_output;

    return AVERROR(EAGAIN);

do_output:
    /* Read the next available object from the FIFO into frame */
    if (av_container_fifo_read(apvctx->output_fifo, frame, 0) >= 0) {
        return 0;
    }
    return 0;
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
        oapvd_delete(apvctx->id);
        apvctx->id = NULL;
    }

    if (apvctx->mid) {
        oapvm_rem_all(apvctx->mid);
        oapvm_delete(apvctx->mid);
        apvctx->mid = NULL;
    }

    av_packet_free(&apvctx->pkt);

    av_container_fifo_free(&apvctx->output_fifo);

    for (int i = 0; i < FF_ARRAY_ELEMS(apvctx->frames); i++) {
        av_frame_free(&apvctx->frames[i]);
    }

    return 0;
}

#define OFFSET(x) offsetof(ApvDecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

// Consider using following options (./ffmpeg --help encoder=liboapv)
//
static const AVOption libapvd_options[] = {
    { "output_csp", "Color space", OFFSET(output_csp),AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VD },
    { "output_depth", "Color space", OFFSET(output_depth),AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VD },
    { NULL }
};

static const AVClass libapvd_class = {
    .class_name = "libapvd",
    .item_name  = av_default_item_name,
    .option     = libapvd_options,
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
    .p.wrapper_name     = "libapvd",
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_apv_profiles),
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_PKT_DTS | FF_CODEC_CAP_SETS_FRAME_PROPS
};
