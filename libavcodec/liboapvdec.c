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

#include "codec_internal.h"
#include "profiles.h"
#include "decode.h"
#include "cbs_apv.h"
#include "internal.h"

#define FRM_IDX    0 // only the first frame in AU needs to be decoded

/**
 * The structure stores all the states associated with the instance of Open APV decoder
 */
typedef struct ApvDecContext {
    const AVClass *class;

    // ids 
    oapvd_t id;             // apvd instance identifier @see apvd_t.h
    oapvm_t mid;            // OAPV metadata container

    APVRawMetadataPayload payloads[CBS_APV_MAX_METADATA_PAYLOADS];
    uint32_t metadata_count;

    AVPacket *pkt;          // encoder bitstream data

    oapv_frms_t ofrms;      // frames structure

} ApvDecContext;

/**
 * Release image buffer reference
 *
 * @param imgb pointer to image buffer structure
 * @return remaining reference count after release
 */
static int apv_imgb_release(oapv_imgb_t *imgb)
{
    int refcnt = --imgb->refcnt;
    if (refcnt == 0) {
        av_freep(&imgb);
    }
    return refcnt;
}

/**
 * Add reference to image buffer
 *
 * @param imgb pointer to image buffer structure
 * @return new reference count after adding reference
 */
static int apv_imgb_addref(oapv_imgb_t * imgb)
{
    int refcnt = ++imgb->refcnt;
    return refcnt;
}

/**
 * Get current reference count of image buffer
 *
 * @param imgb pointer to image buffer structure
 * @return current reference count
 */
static int apv_imgb_getref(oapv_imgb_t * imgb)
{
    return imgb->refcnt;
}

/**
 * Create image buffer structure with specified dimensions and color space
 *
 * @param w image width
 * @param h image height
 * @param cs color space format
 * @param avctx codec context
 * @param frame AV frame structure
 * @return pointer to created image buffer structure, NULL on failure
 */
static oapv_imgb_t *apv_imgb_create(int w, int h, int cs, AVCodecContext *avctx, AVFrame* frame)
{
    oapv_imgb_t *imgb = NULL;

    imgb = av_mallocz(sizeof(oapv_imgb_t));
    if (!imgb)
        goto fail;

    memset(imgb, 0, sizeof(oapv_imgb_t));

    if(OAPV_CS_GET_BIT_DEPTH(cs)!=10 && OAPV_CS_GET_BIT_DEPTH(cs)!=12) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format\n");
        goto fail;
    }

    imgb->w[0] = w;
    imgb->h[0] = h;

    switch(OAPV_CS_GET_FORMAT(cs))
    {
    case OAPV_CF_YCBCR422: // profile 33 (10bit), profile 44 (12bit)
        imgb->w[1] = imgb->w[2] = (w + 1) >> 1;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 3;
        break;
    case OAPV_CF_YCBCR444: // profile 55 (10bit), profile 66 (12bit)
        imgb->w[1] = imgb->w[2] = w;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 3;
        break;
   case OAPV_CF_YCBCR4444: // profile 77 (10bit), profile 88 (12bit)
        imgb->w[1] = imgb->w[2] = imgb->w[3] = w;
        imgb->h[1] = imgb->h[2] = imgb->h[3] = h;
        imgb->np = 4;
        break;
    case OAPV_CF_YCBCR400: // profile 99 (10bit)
        imgb->w[1] = imgb->w[2] = w;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 1;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format\n");
        goto fail;
    }

    for(int i = 0; i < imgb->np; i++)
    {
        imgb->aw[i] = FFALIGN(imgb->w[i], OAPV_MB_W);
        imgb->ah[i] = FFALIGN(imgb->h[i], OAPV_MB_H);
        imgb->s[i] = imgb->aw[i] * OAPV_CS_GET_BYTE_DEPTH(cs);
        imgb->e[i] = imgb->ah[i];

        imgb->bsize[i] = imgb->s[i] * imgb->e[i];
        imgb->a[i] = imgb->baddr[i] = (void*) frame->data[i];
        if (imgb->a[i] == NULL)
            goto fail;

        memset(imgb->a[i], 0, imgb->bsize[i]);
    }
    imgb->cs = cs;
    imgb->addref = apv_imgb_addref;
    imgb->getref = apv_imgb_getref;
    imgb->release = apv_imgb_release;

    imgb->addref(imgb); /* increase reference count */
    return imgb;

fail:
    av_log(avctx, AV_LOG_ERROR, "Cannot create image buffer\n");

    if (imgb) {
        av_freep(&imgb);
    }
    return NULL;
}

/**
 * Decode and process metadata payloads from APV bitstream
 *
 * @param avctx codec context
 * @param frame output frame to store metadata
 * @param payloads array of metadata payloads
 * @param metadata_count number of metadata payloads
 * @return 0 on success, negative error code on failure
 */
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
    oapvd_cdesc_t cdsc;
    int ret = 0;

    // cdesc structure initialization
    memset(&cdsc, 0, sizeof(oapvd_cdesc_t));
    cdsc.threads = OAPV_CDESC_THREADS_AUTO;

    // create decoder instance 
    apvctx->id = oapvd_create(&cdsc, &ret);
    if (apvctx->id == NULL || ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create oapv decoder\n");
        return AVERROR_EXTERNAL;
    }

    // create metadata container
    apvctx->mid = oapvm_create(&ret);
    if(OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create oapv metadata container (err=%d)\n", ret);
        return AVERROR_EXTERNAL;
    }

    apvctx->pkt = avctx->internal->in_pkt;

    // ofrms initialization
    memset(&(apvctx->ofrms), 0, sizeof(oapv_frms_t));

    return 0;
}
static const enum AVPixelFormat apv_format_table[5][5] = {
    { AV_PIX_FMT_GRAY8,    AV_PIX_FMT_GRAY10,     AV_PIX_FMT_GRAY12,     AV_PIX_FMT_GRAY14, AV_PIX_FMT_GRAY16 },
    { 0 }, // 4:2:0 is not valid.
    { AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV422P10,  AV_PIX_FMT_YUV422P12,  AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV422P16 },
    { AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV444P10,  AV_PIX_FMT_YUV444P12,  AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV444P16 },
    { AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUVA444P10, AV_PIX_FMT_YUVA444P12, 0                   ,AV_PIX_FMT_YUVA444P16 },
};

/**
 * Decode frame with decoupled packet/frame dataflow
 *
 * @param avctx codec context
 * @param aui APV access unit information structure
 * @return 0 on success, negative error code on failure
 */
static int apv_decode_check_format(AVCodecContext *avctx,
                                   oapv_au_info_t *aui)
{
    int err, bit_depth;

    avctx->profile = aui->frm_info[FRM_IDX].profile_idc;
    avctx->level   = aui->frm_info[FRM_IDX].level_idc;

    bit_depth = aui->frm_info[FRM_IDX].bit_depth;
    if (bit_depth < 8 || bit_depth > 16 || bit_depth % 2) {
        avpriv_request_sample(avctx, "Bit depth %d", bit_depth);
        return AVERROR_PATCHWELCOME;
    }
    avctx->pix_fmt =
        apv_format_table[aui->frm_info[FRM_IDX].chroma_format_idc][bit_depth - 4 >> 2];

    if (!avctx->pix_fmt) {
        avpriv_request_sample(avctx, "YUVA444P14");
        return AVERROR_PATCHWELCOME;
    }

    err = ff_set_dimensions(avctx,
                            FFALIGN(aui->frm_info[0].w,  16),
                            FFALIGN(aui->frm_info[0].h, 16));
    if (err < 0) {
        // Unsupported frame size.
        return err;
    }
    avctx->width  = aui->frm_info[FRM_IDX].w;
    avctx->height = aui->frm_info[FRM_IDX].h;

    avctx->sample_aspect_ratio = (AVRational){ 1, 1 };

    if (aui->frm_info[FRM_IDX].color_description_present_flag){
        avctx->color_primaries = aui->frm_info[FRM_IDX].color_primaries;
        avctx->color_trc       = aui->frm_info[FRM_IDX].transfer_characteristics;
        avctx->colorspace      = aui->frm_info[FRM_IDX].matrix_coefficients;
        avctx->color_range     = aui->frm_info[FRM_IDX].full_range_flag ? AVCOL_RANGE_JPEG
                                                        : AVCOL_RANGE_MPEG;
    }
    else{
        avctx->color_primaries = AVCOL_PRI_UNSPECIFIED; 
        avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
        avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;
        avctx->color_range     = aui->frm_info[FRM_IDX].full_range_flag ? AVCOL_RANGE_JPEG
                                                        : AVCOL_RANGE_MPEG;
    }

    avctx->chroma_sample_location = AVCHROMA_LOC_TOPLEFT;

    avctx->refs = 0;
    avctx->has_b_frames = 0;

    return 0;
}
/**
 * Receive and decode a frame from the decoder
 *
 * @param avctx codec context
 * @param frame output frame structure for decoded data
 * @return 0 on success, negative error code on failure
 */
static int liboapvd_receive_frame(AVCodecContext *avctx, AVFrame *frame){

    ApvDecContext *apvctx = avctx->priv_data;
    int ret = 0;

    oapv_au_info_t aui;
    oapv_frm_info_t *finfo = NULL;
    oapv_frm_t  *frm = NULL;
    oapv_bitb_t bitb;
    oapvd_stat_t stat;

    // fill the bsbuf and the bsbufsize
    ret = ff_decode_get_packet(avctx, apvctx->pkt);
    if (ret < 0 && ret != AVERROR_EOF) {
        av_packet_unref(apvctx->pkt);
        return ret;
    }

    // if pkt size is zero
    if (apvctx->pkt->size <= 0) {
        av_packet_unref(apvctx->pkt);
        return ret;
    }

    // Handle End of Stream (EOS) flushing
    if (ret == AVERROR_EOF || apvctx->pkt->size <= 0) {
        av_packet_unref(apvctx->pkt);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    
    // popularing the aui struct from bitstream
    memset(&aui, 0, sizeof(oapv_au_info_t));
    if (OAPV_FAILED(oapvd_info(apvctx->pkt->data, apvctx->pkt->size, &aui))){
        av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    // populating the AVCodec context using the aui information
    ret = apv_decode_check_format(avctx, &aui);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "apv_decode_check_format failed\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    
    // buffer allocation for the frame
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0) goto end;

    apvctx->ofrms.num_frms = aui.num_frms; // this is for aui.num_frms = 1 only
    for (int frm_idx = 0; frm_idx < apvctx->ofrms.num_frms; frm_idx++){
        finfo = &aui.frm_info[frm_idx];
        frm = &(apvctx->ofrms.frm[frm_idx]);
        
        apvctx->ofrms.frm[frm_idx].imgb = apv_imgb_create(finfo->w, finfo->h, finfo->cs, avctx, frame);
        if(apvctx->ofrms.frm[frm_idx].imgb == NULL) {
            av_log(avctx, AV_LOG_ERROR, "cannot allocate image buffer (w:%d, h:%d, cs:%d)\n",
                    finfo->w, finfo->h, finfo->cs);

            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }
    
    // main decoding 
    bitb.addr = apvctx->pkt->data;
    bitb.ssize = apvctx->pkt->size;
    memset(&stat, 0, sizeof(oapvd_stat_t));
    ret = oapvd_decode(apvctx->id, &bitb, &(apvctx->ofrms), apvctx->mid, &stat);
    if(OAPV_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR,"failed to decode bitstream\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if(stat.read != apvctx->pkt->size) {
        av_log(avctx, AV_LOG_ERROR,"\t=> different reading of bitstream (in:%d, read:%d)\n",
                 apvctx->pkt->size, stat.read);
    }

    // metadata handling
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

    /* Use ff_decode_frame_props_from_pkt() to fill frame properties */
    ret = ff_decode_frame_props_from_pkt(avctx, frame, apvctx->pkt);
    if (apvctx->pkt->flags & AV_PKT_FLAG_KEY) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
    }

    /* set metadata */
    ret = decode_metadata(avctx, frame, apvctx->payloads, apvctx->metadata_count);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "decode_metadata error\n");
        av_frame_unref(frame);

        goto end;
    }

    ret = AVERROR(EAGAIN);
end:
    for(int i = 0; i < apvctx->ofrms.num_frms; i++) {
        if(apvctx->ofrms.frm[i].imgb != NULL) {
            apvctx->ofrms.frm[i].imgb->release(apvctx->ofrms.frm[i].imgb);
            apvctx->ofrms.frm[i].imgb = NULL;
        }
    }
    return ret;
}

/**
 * Flush the decoder buffer and reset internal state
 *
 * @param avctx codec context
 */
static av_cold void liboapvd_flush(AVCodecContext* avctx){
    ApvDecContext *apvctx = avctx->priv_data;
    av_packet_unref(apvctx->pkt);
}

/**
 * Close the decoder and free all allocated resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int liboapvd_close(AVCodecContext *avctx)
{
    ApvDecContext *apvctx = avctx->priv_data;

    // ofrms deallocation
    for(int i = 0; i < apvctx->ofrms.num_frms; i++) {
        if(apvctx->ofrms.frm[i].imgb != NULL) {
            apvctx->ofrms.frm[i].imgb->release(apvctx->ofrms.frm[i].imgb);
            apvctx->ofrms.frm[i].imgb = NULL;
        }
    }

    // decoder id deallocation
    if (apvctx->id) {
        oapvd_delete(apvctx->id);
        apvctx->id = NULL;
    }

    // metadata id deallocation
    if (apvctx->mid) {
        oapvm_rem_all(apvctx->mid);
        oapvm_delete(apvctx->mid);
        apvctx->mid = NULL;
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
    .flush              = liboapvd_flush,
    .priv_data_size     = sizeof(ApvDecContext),
    .p.priv_class       = &liboapvd_class,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_AVOID_PROBING,
    .p.wrapper_name     = "liboapv",
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_apv_profiles),
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS | FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_FRAME_PROPS
};