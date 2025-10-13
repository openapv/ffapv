/*
 * APV (Advanced Professional Video) decoder using Open APV library (liboapv)
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

#include <oapv/oapv.h>

#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mem.h"
#include "libavutil/imgutils.h"
#include "libavutil/container_fifo.h"
#include "libavutil/avassert.h"

#include "codec_internal.h"
#include "profiles.h"
#include "decode.h"
#include "cbs_apv.h"

/**
 * The structure stores all the states associated with the instance of Open APV decoder
 */
typedef struct ApvDecContext {
    const AVClass *class;

    oapvd_t id;             // apvd instance identifier @see apvd_t.h
    oapvd_cdesc_t cdsc;     // decoding parameters @see apvd_t.h

    oapvm_t mid;            // OAPV metadata container

    int output_depth;

    struct AVContainerFifo *output_fifo;

    AVFrame* frames[OAPV_MAX_NUM_FRAMES];
    uint32_t frames_count;   // primary frames

    APVRawMetadataPayload payloads[CBS_APV_MAX_METADATA_PAYLOADS];
    uint32_t metadata_count;

    AVPacket *pkt;          // frame data
} ApvDecContext;

static int apv_imgb_release(oapv_imgb_t *imgb)
{
    int refcnt = --imgb->refcnt;
    if (refcnt == 0) {
        for (int i = 0; i < imgb->np; i++) {
            av_freep(&imgb->baddr[i]);
        }

        av_freep(&imgb);
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
 * @brief Converts liboapv color space into AVPixelFormat
 *
 * @param[in] cs libopav color space
 * @return AVPixelFormat
 */
static enum AVPixelFormat get_pixel_format(int cs)
{
    enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;

    switch(cs) {
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 0):  // profile 33
        pix_fmt = AV_PIX_FMT_YUV422P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 1):
        pix_fmt = AV_PIX_FMT_YUV422P10BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 0):  // profile 44
        pix_fmt = AV_PIX_FMT_YUV422P12LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 1):
        pix_fmt = AV_PIX_FMT_YUV422P12BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 10, 0):  // profile 55
        pix_fmt = AV_PIX_FMT_YUV444P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 10, 1):
        pix_fmt = AV_PIX_FMT_YUV444P10BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 0):   // profile 66
        pix_fmt = AV_PIX_FMT_YUV444P12LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 1):
        pix_fmt = AV_PIX_FMT_YUV444P12BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 10, 0):  // profile 77
        pix_fmt = AV_PIX_FMT_YUVA444P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 10, 1):
        pix_fmt = AV_PIX_FMT_YUVA444P10BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 12, 0):  // profile 88
        pix_fmt = AV_PIX_FMT_YUVA444P12LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR4444, 12, 1):
        pix_fmt = AV_PIX_FMT_YUVA444P12BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR400, 10, 0):   // profile 99
        pix_fmt = AV_PIX_FMT_GRAY10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR400, 10, 1):
        pix_fmt = AV_PIX_FMT_GRAY10BE;
        break;
    default:
        pix_fmt = AV_PIX_FMT_NONE;
        break;
    }

    return pix_fmt;
}

static oapv_imgb_t *apv_imgb_create(int w, int h, int cs, AVCodecContext *avctx)
{

    oapv_imgb_t *imgb;

    enum AVPixelFormat pix_fmt = get_pixel_format(cs);
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);

    av_assert0(desc);

    imgb = av_mallocz(sizeof(oapv_imgb_t));
    if (!imgb)
        goto fail;

    imgb->np = desc->nb_components;

    for (int i = 0; i < imgb->np; i++) {
        imgb->w[i]  = w >> ((i == 1 || i == 2) ? desc->log2_chroma_w : 0);
        imgb->h[i]  = h;
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
    av_log(avctx, AV_LOG_ERROR, "Cannot create image buffer\n");
    if (imgb) {
        for (int i = 0; i < imgb->np; i++)
            av_freep(&imgb->a[i]);
        av_freep(&imgb);
    }
    return NULL;
}

/**
 * @brief The function populates the cdsc
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

/**
 * @brief The function populates avctx fields based on information from frame
 *
 * @param[out] avctx codec context
 * @param[in] frame stores information of bitstream
 */
static void export_stream_params(AVCodecContext *avctx, const AVFrame* frame)
{
    avctx->width = frame->width;
    avctx->height = frame->height;

    avctx->pix_fmt = frame->format;

    avctx->color_primaries = frame->color_primaries;
    avctx->color_trc = frame->color_trc;
    avctx->colorspace = frame->colorspace;
    avctx->color_range = frame->color_range;
}

/**
 * @brief The function populates frame based on information from frm_info
 *
 * @param[out] frame
 * @param[in] frm_info
 * @return 0 on success, negative value on failure
 */
static int set_frame_metadata(AVFrame* frame, const oapv_frm_info_t* frm_info)
{
    frame->width = frm_info->w;
    frame->height = frm_info->h;

    frame->format = get_pixel_format(frm_info->cs);

    if(frame->format == AV_PIX_FMT_NONE) {
        return AVERROR_INVALIDDATA;
    }

    if (frm_info->color_description_present_flag) {
        frame->color_primaries = frm_info->color_primaries;
        frame->color_trc = frm_info->transfer_characteristics;
        frame->colorspace = frm_info->matrix_coefficients;
        frame->color_range = frm_info->full_range_flag ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    }
    return 0;
}

static void av_buffer_free(void *opaque, uint8_t *data) {
    av_free(data);
}

/**
 * @brief Transferring video frame data from the source oapv_imgb_t object to the target AVFrame object
 * 
 * @param frame dst
 * @param imgb src
 * @return 0 on success, negative value on failure
 */
static int move_frame_data(AVFrame *frame, oapv_imgb_t *imgb)
{
    for (int i = 0; i < imgb->np; i++) {

        int aw = FFALIGN(imgb->w[i], OAPV_MB_W);
        int ah = FFALIGN(imgb->h[i], OAPV_MB_H);
        int plane_size = aw * ah * OAPV_CS_GET_BYTE_DEPTH(imgb->cs);

        frame->linesize[i] = aw * OAPV_CS_GET_BYTE_DEPTH(imgb->cs);

        // Create reference-counted buffers from existing array for AVFrame
        // Transferring the data (a buffer containing plane data) to the AVBufferRef object
        // The data is owned by the AVBuffer
        frame->buf[i] = av_buffer_create(imgb->a[i], plane_size, av_buffer_free, NULL, 0);
        if(frame->buf[i] == NULL) {
            return AVERROR_INVALIDDATA;
        }    

        frame->data[i] = frame->buf[i]->data;
        
        // Leave the source object in a state that allows it to be safely deleted
        // Reset buffer pointers to ensure the source object is in a safe destruction state
        // (set pointer variable members to NULL to avoid double memory deallocation)
        imgb->a[i] = NULL;
        imgb->baddr[i] = NULL;

        imgb->bsize[i] = 0;
        imgb->w[i] = 0;
        imgb->h[i] = 0;
        imgb->aw[i] = 0;
        imgb->ah[i] = 0;
        imgb->s[i] = 0;
        imgb->e[i] = 0;
    }
    return 0;
}

/**
 * @brief The function moves image data from imgb into frame
 *
 * @param avctx codec context
 * @param[out] frame dst
 * @param[in] imgb src
 * @return 0 on success, negative value on failure
 */
static int set_frame_data(AVCodecContext *avctx, AVFrame *frame, oapv_imgb_t *imgb)
{
    int ret = 0;

    if (imgb->w[0] != avctx->width || imgb->h[0] != avctx->height) { // stream resolution changed
        if (ff_set_dimensions(avctx, imgb->w[0], imgb->h[0]) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot set new dimension\n");
            return AVERROR_INVALIDDATA;
        }
    }

    ret = move_frame_data(frame, imgb);

    return ret;
}

static int decode_metadata(AVCodecContext *avctx, AVFrame *frame, const APVRawMetadataPayload *payloads, uint32_t metadata_count)
{
    int err;
    for(int i=0; i<metadata_count; i++) {
        const APVRawMetadataPayload *pld = &payloads[i];
        switch (pld->payload_type) {
        case APV_METADATA_MDCV:  {
                const APVRawMetadataMDCV *mdcv = &pld->mdcv;
                AVMasteringDisplayMetadata *mdm;

                err = ff_decode_mastering_display_new(avctx, frame, &mdm);
                if (err < 0)
                    return err;

                if (mdm) {
                    for (int j = 0; j < 3; j++) {
                        mdm->display_primaries[j][0] =
                            av_make_q(mdcv->primary_chromaticity_x[j], 1 << 16);
                        mdm->display_primaries[j][1] =
                            av_make_q(mdcv->primary_chromaticity_y[j], 1 << 16);
                    }

                    mdm->white_point[0] =
                        av_make_q(mdcv->white_point_chromaticity_x, 1 << 16);
                    mdm->white_point[1] =
                        av_make_q(mdcv->white_point_chromaticity_y, 1 << 16);

                    mdm->max_luminance =
                        av_make_q(mdcv->max_mastering_luminance, 1 << 8);
                    mdm->min_luminance =
                        av_make_q(mdcv->min_mastering_luminance, 1 << 14);

                    mdm->has_primaries = 1;
                    mdm->has_luminance = 1;
                }
            }
            break;
        case APV_METADATA_CLL:
            {
                const APVRawMetadataCLL *cll = &pld->cll;
                AVContentLightMetadata *clm;

                err = ff_decode_content_light_new(avctx, frame, &clm);
                if (err < 0)
                    return err;

                if (clm) {
                    clm->MaxCLL  = cll->max_cll;
                    clm->MaxFALL = cll->max_fall;
                }
            }
            break;
        case APV_METADATA_ITU_T_T35:
        case APV_METADATA_USER_DEFINED:
            {
                av_log(avctx, AV_LOG_WARNING, "Not supported metadata type\n");
            }
            break;
        default:
            // Ignore other types of metadata.
            break;
        }
    }

    return 0;
}

/**
 * @brief Initialize OpenAPV decoder
 * Create a decoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int liboapvd_init(AVCodecContext *avctx)
{
    ApvDecContext *apvctx = avctx->priv_data;
    oapvd_cdesc_t *cdsc = &(apvctx->cdsc);
    int ret = 0;

    /* read configurations from AVCodecContext and populate the apvd_cdsc structure */
    get_conf(avctx, cdsc);

    /* create decoder instance */
    apvctx->id = oapvd_create(&(apvctx->cdsc), NULL);
    if (apvctx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create oapv decoder\n");
        return AVERROR_EXTERNAL;
    }

    /* create metadata container */
    apvctx->mid = oapvm_create(&ret);
    if(OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create oapv metadata container (err=%d)\n", ret);
        return AVERROR_EXTERNAL;
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
static int liboapvd_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    ApvDecContext *apvctx = avctx->priv_data;
    AVPacket *pkt = apvctx->pkt;
    int ret = 0;

    uint8_t *bs_buf = NULL;
    uint32_t bs_buf_size = 0;

    oapvd_stat_t stat;
    oapv_bitb_t bitb;
    oapv_frms_t ofrms;
    oapv_frm_t  *frm = NULL;

    oapv_au_info_t aui;
    oapv_frm_info_t *finfo = NULL;

    if (av_container_fifo_can_read(apvctx->output_fifo))
        goto do_output;

    for(int i =0; i<apvctx->frames_count;i++ ) {
        av_frame_unref(apvctx->frames[i]);
        apvctx->frames_count = 0;
    }

    // frame data (input data) - AU
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


    bs_buf = pkt->data;
    bs_buf_size = pkt->size;

    if (OAPV_FAILED(oapvd_info(bs_buf, bs_buf_size, &aui)))
    {
        av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    /* create decoding frame buffers */
    ofrms.num_frms = aui.num_frms;
    for(int i = 0; i < ofrms.num_frms; i++) {

        finfo = &aui.frm_info[i];

        ofrms.frm[i].imgb = apv_imgb_create(finfo->w, finfo->h, finfo->cs, avctx);
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
            pld = av_mallocz(sizeof(oapvm_payload_t) * num_plds);
            ret = oapvm_get_all(apvctx->mid, pld, &num_plds);
            if(OAPV_FAILED(ret)) {
                av_log(avctx, AV_LOG_ERROR,"failed to read metadata\n");

                if(pld != NULL)
                    av_freep(&pld);

                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            for(int i = 0; i < num_plds; i++) {
                oapvm_payload_t *oapv_pld = &pld[i];

                APVRawMetadataPayload pld = apvctx->payloads[i];
                pld.payload_type = oapv_pld->type;
                pld.payload_size = oapv_pld->size;

                apvctx->metadata_count = num_plds;

                switch(oapv_pld->type) {
                    case OAPV_METADATA_ITU_T_T35:
                        {
                            memcpy(&pld.itu_t_t35, oapv_pld->data, oapv_pld->size);
                            break;
                        }
                    case OAPV_METADATA_MDCV:
                        {
                            memcpy(&pld.mdcv, oapv_pld->data, oapv_pld->size);
                            break;
                        }
                    case OAPV_METADATA_CLL:
                        {
                            memcpy(&pld.cll, oapv_pld->data, oapv_pld->size);
                            break;
                        }
                    case OAPV_METADATA_FILLER:
                        {
                            memcpy(&pld.filler, oapv_pld->data, oapv_pld->size);
                            break;
                        }
                    case OAPV_METADATA_USER_DEFINED:
                        {
                            memcpy(&pld.user_defined, oapv_pld->data, oapv_pld->size);
                            break;
                        }
                    default:
                        // Ignore other types of metadata.
                        break;
                }
            }
        }

        if(pld != NULL)
            av_freep(&pld);
    }

    /* Write decoded frames into AVFrame objects */
    for(int i = 0, j = 0; i < ofrms.num_frms; i++) {

        frm = &ofrms.frm[i];
        if(frm->pbu_type != OAPV_PBU_TYPE_PRIMARY_FRAME) {
            av_log(avctx, AV_LOG_WARNING,
                "Stream contains additional non-primary frames "
                "which will be ignored by the decoder.\n");
        } else {

            if(apvctx->frames[j] == NULL) {
                apvctx->frames[j] = av_frame_alloc();
            }

            /* Set frame info into AVFrame object */
            ret = set_frame_metadata(apvctx->frames[j], &stat.aui.frm_info[i]);
            if(ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Frame info setting error\n");
                av_frame_unref(apvctx->frames[j]);

                goto end;
            }

            /* Move decoded frame data form oapv_imgb_t into AVFrame object */
            ret = set_frame_data(avctx, apvctx->frames[j], frm->imgb);
            if(ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Frame data moving error\n");
                av_frame_unref(apvctx->frames[j]);

                goto end;
            }

            /* Use ff_decode_frame_props_from_pkt() to fill frame properties */
            ret = ff_decode_frame_props_from_pkt(avctx, apvctx->frames[j], pkt);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "ff_decode_frame_props_from_pkt error\n");
                av_frame_unref(apvctx->frames[j]);

                goto end;
            }

            /* set metadata */
            ret = decode_metadata(avctx, apvctx->frames[j], apvctx->payloads, apvctx->metadata_count);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "decode_metadata error\n");
                av_frame_unref(apvctx->frames[j]);

                goto end;
            }

            if (pkt->flags & AV_PKT_FLAG_KEY) {
                apvctx->frames[j]->pict_type = AV_PICTURE_TYPE_I;
                apvctx->frames[j]->flags |= AV_FRAME_FLAG_KEY;
            }

            apvctx->frames_count++;

            /* Write the AVFrame data to the FIFO */
            ret = av_container_fifo_write(apvctx->output_fifo, apvctx->frames[j], AV_CONTAINER_FIFO_FLAG_REF);
            j++;
        }
    }

end:
    av_packet_unref(pkt);

    for(int i = 0; i < ofrms.num_frms; i++) {
        if(ofrms.frm[i].imgb != NULL) {
            ofrms.frm[i].imgb->release(ofrms.frm[i].imgb);
            ofrms.frm[i].imgb = NULL;
        }
    }

    if (av_container_fifo_can_read(apvctx->output_fifo))
        goto do_output;

    return AVERROR(EAGAIN);

do_output:
    /* Read the next available object from the FIFO into frame */
    if ((ret = av_container_fifo_read(apvctx->output_fifo, frame, 0)) >= 0) {
        export_stream_params(avctx, frame);
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
static av_cold int liboapvd_close(AVCodecContext *avctx)
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


static const AVClass liboapvd_class = {
    .class_name = "liboapv",
    .item_name  = av_default_item_name,
    .option     = NULL,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_liboapv_decoder = {
    .p.name             = "liboapv",
    .p.long_name        = NULL_IF_CONFIG_SMALL("liboapv APV"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_APV,
    .init               = liboapvd_init,
    FF_CODEC_RECEIVE_FRAME_CB(liboapvd_receive_frame),
    .close              = liboapvd_close,
    .priv_data_size     = sizeof(ApvDecContext),
    .p.priv_class       = &liboapvd_class,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_AVOID_PROBING,
    .p.wrapper_name     = "liboapv",
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_apv_profiles),
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS | FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_FRAME_PROPS
};