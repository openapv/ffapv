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


#include <oapv/oapv.h>

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

/* assert function */
#include <assert.h>
#define assert_rv(x,r) {if(!(x)){assert(x); return (r);}}

#define LIBOAPV_CLIP_VAL(n, min, max) (((n) > (max)) ? (max) : (((n) < (min)) ? (min) : (n)))
#define LIBOAPV_ALIGN_VAL(val, align) ((((val) + (align) - 1) / (align)) * (align))

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
    int cpu_count = av_cpu_count();

    /* clear apvd_cdsc structure */
    memset(cdsc, 0, sizeof(oapvd_cdesc_t));

    /* init apvd_cdsc structure */
    if (avctx->thread_count <= 0)
        cdsc->threads = (cpu_count < OAPV_MAX_THREADS) ? cpu_count : OAPV_MAX_THREADS;
    else if (avctx->thread_count > OAPV_MAX_THREADS)
        cdsc->threads = OAPV_MAX_THREADS;
    else
        cdsc->threads = avctx->thread_count;
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
    case OAPV_CS_YCBCR422_10LE:
        avctx->pix_fmt = AV_PIX_FMT_YUV422P10;
        break;
    case OAPV_CS_YCBCR444_10LE:
        avctx->pix_fmt = AV_PIX_FMT_YUV444P10;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 0):
        avctx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 0):
        avctx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR422, 12, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUV422P12BE;
        break;
    case OAPV_CS_SET(OAPV_CF_YCBCR444, 12, 1):
        avctx->pix_fmt = AV_PIX_FMT_YUV444P12BE;
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
static int libapvd_image_copy(struct AVCodecContext *avctx, oapv_imgb_t *imgb, struct AVFrame *frame)
{
    int ret;
    if (imgb->cs != OAPV_CS_YCBCR422_10LE) {
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

/* Function for atomic increament:
   This function might need to modify according to O/S or CPU platform
*/
static int libapvd_atomic_inc(volatile int* pcnt)
{
    int ret;
    ret = *pcnt;
    ret++;
    *pcnt = ret;
    return ret;
}

/* Function for atomic decrement:
   This function might need to modify according to O/S or CPU platform
*/
static int libapvd_atomic_dec(volatile int* pcnt)
{
    int ret;
    ret = *pcnt;
    ret--;
    *pcnt = ret;
    return ret;
}

/* Function to allocate memory for picture buffer:
   This function might need to modify according to O/S or CPU platform
*/
static void * libapvd_picbuf_alloc(int size)
{
    return malloc(size);
}

/* Function to free memory allocated for picture buffer:
   This function might need to modify according to O/S or CPU platform
*/
static void libapvd_picbuf_free(void* p)
{
    if (p) {free(p);}
}

static int libapvd_imgb_addref(oapv_imgb_t * imgb)
{
    assert_rv(imgb, OAPV_ERR_INVALID_ARGUMENT);
    return libapvd_atomic_inc(&imgb->refcnt);
}

static int libapvd_imgb_getref(oapv_imgb_t * imgb)
{
    assert_rv(imgb, OAPV_ERR_INVALID_ARGUMENT);
    return imgb->refcnt;
}

static int libapvd_imgb_release(oapv_imgb_t * imgb)
{
    int refcnt, i;
    assert_rv(imgb, OAPV_ERR_INVALID_ARGUMENT);
    refcnt = libapvd_atomic_dec(&imgb->refcnt);
    if(refcnt == 0) {
        for(i=0; i<OAPV_MAX_CC; i++) {
            if (imgb->baddr[i]) libapvd_picbuf_free(imgb->baddr[i]);
        }
        free(imgb);
    }
    return refcnt;
}

static void libapvd_imgb_cpy_plane(oapv_imgb_t *dst, oapv_imgb_t *src)
{
    int            i, j;
    unsigned char *s, *d;
    int            numbyte = OAPV_CS_GET_BYTE_DEPTH(src->cs);

    for(i = 0; i < src->np; i++) {
        s = (unsigned char *)src->a[i];
        d = (unsigned char *)dst->a[i];

        for(j = 0; j < src->ah[i]; j++) {
            memcpy(d, s, numbyte * src->aw[i]);
            s += src->s[i];
            d += dst->s[i];
        }
    }
}

static void libapvd_imgb_cpy_shift_left_8b(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
{
    int            i, j, k;

    unsigned char *s;
    short         *d;

    for(i = 0; i < dst->np; i++) {
        s = (unsigned char *)src->a[i];
        d = (short *)dst->a[i];

        for(j = 0; j < src->ah[i]; j++) {
            for(k = 0; k < src->aw[i]; k++) {
                d[k] = (short)(s[k] << shift);
            }
            s = s + src->s[i];
            d = (short *)(((unsigned char *)d) + dst->s[i]);
        }
    }
}

static void libapvd_imgb_cpy_shift_right_8b(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
{
    int            i, j, k, t0, add;

    short         *s;
    unsigned char *d;

    if(shift)
        add = 1 << (shift - 1);
    else
        add = 0;

    for(i = 0; i < dst->np; i++) {
        s = (short *)src->a[i];
        d = (unsigned char *)dst->a[i];

        for(j = 0; j < src->ah[i]; j++) {
            for(k = 0; k < src->aw[i]; k++) {
                t0 = ((s[k] + add) >> shift);
                d[k] = (unsigned char)(LIBOAPV_CLIP_VAL(t0, 0, 255));
            }
            s = (short *)(((unsigned char *)s) + src->s[i]);
            d = d + dst->s[i];
        }
    }
}

static void libapvd_imgb_cpy_shift_left(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
{
    int             i, j, k;

    unsigned short *s;
    unsigned short *d;

    for(i = 0; i < dst->np; i++) {
        s = (unsigned short *)src->a[i];
        d = (unsigned short *)dst->a[i];

        for(j = 0; j < src->h[i]; j++) {
            for(k = 0; k < src->w[i]; k++) {
                d[k] = (unsigned short)(s[k] << shift);
            }
            s = (unsigned short *)(((unsigned char *)s) + src->s[i]);
            d = (unsigned short *)(((unsigned char *)d) + dst->s[i]);
        }
    }
}

static void libapvd_imgb_cpy_shift_right(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
{
    int             i, j, k, t0, add;

    int             clip_min = 0;
    int             clip_max = 0;

    unsigned short *s;
    unsigned short *d;

    if(shift)
        add = 1 << (shift - 1);
    else
        add = 0;

    clip_max = (1 << (OAPV_CS_GET_BIT_DEPTH(dst->cs))) - 1;

    for(i = 0; i < dst->np; i++) {
        s = (unsigned short *)src->a[i];
        d = (unsigned short *)dst->a[i];

        for(j = 0; j < src->h[i]; j++) {
            for(k = 0; k < src->w[i]; k++) {
                t0 = ((s[k] + add) >> shift);
                d[k] = (LIBOAPV_CLIP_VAL(t0, clip_min, clip_max));
            }
            s = (unsigned short *)(((unsigned char *)s) + src->s[i]);
            d = (unsigned short *)(((unsigned char *)d) + dst->s[i]);
        }
    }
}

static void libapvd_imgb_cpy(oapv_imgb_t *dst, oapv_imgb_t *src, struct AVCodecContext *avctx)
{
    int i, bd_src, bd_dst;
    bd_src = OAPV_CS_GET_BIT_DEPTH(src->cs);
    bd_dst = OAPV_CS_GET_BIT_DEPTH(dst->cs);

    if(src->cs == dst->cs) {
        libapvd_imgb_cpy_plane(dst, src);
    }
    else if(bd_src == 8 && bd_dst > 8) {
        libapvd_imgb_cpy_shift_left_8b(dst, src, bd_dst - bd_src);
    }
    else if(bd_src > 8 && bd_dst == 8) {
        libapvd_imgb_cpy_shift_right_8b(dst, src, bd_src - bd_dst);
    }
    else if(bd_src < bd_dst) {
        libapvd_imgb_cpy_shift_left(dst, src, bd_dst - bd_src);
    }
    else if(bd_src > bd_dst) {
        libapvd_imgb_cpy_shift_right(dst, src, bd_src - bd_dst);
    }
    else {
        av_log(avctx, AV_LOG_ERROR, "ERROR: unsupported image copy\n");
        return;
    }
    for(i = 0; i < OAPV_MAX_CC; i++) {
        dst->x[i] = src->x[i];
        dst->y[i] = src->y[i];
        dst->w[i] = src->w[i];
        dst->h[i] = src->h[i];
        dst->ts[i] = src->ts[i];
    }
}

static oapv_imgb_t * libapvd_imgb_create(int w, int h, int cs, struct AVCodecContext *avctx)
{
    int i, bd;
    oapv_imgb_t * imgb;

    imgb = (oapv_imgb_t *)malloc(sizeof(oapv_imgb_t));
    if(imgb == NULL) goto ERR;
    memset(imgb, 0, sizeof(oapv_imgb_t));

    bd = OAPV_CS_GET_BYTE_DEPTH(cs); /* byte unit */

    imgb->w[0] = w;
    imgb->h[0] = h;
    switch(OAPV_CS_GET_FORMAT(cs))
    {
    case OAPV_CF_YCBCR400:
        imgb->w[1] = imgb->w[2] = w;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 1;
        break;
    case OAPV_CF_YCBCR420:
        imgb->w[1] = imgb->w[2] = (w + 1) >> 1;
        imgb->h[1] = imgb->h[2] = (h + 1) >> 1;
        imgb->np = 3;
        break;
    case OAPV_CF_YCBCR422:
        imgb->w[1] = imgb->w[2] = (w + 1) >> 1;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 3;
        break;
    case OAPV_CF_YCBCR444:
        imgb->w[1] = imgb->w[2] = w;
        imgb->h[1] = imgb->h[2] = h;
        imgb->np = 3;
        break;
   case OAPV_CF_YCBCR4444:
        imgb->w[1] = imgb->w[2] = imgb->w[3] = w;
        imgb->h[1] = imgb->h[2] = imgb->h[3] = h;
        imgb->np = 4;
        break;
    case OAPV_CF_PLANAR2:
        imgb->w[1] = w;
        imgb->h[1] = h;
        imgb->np = 2;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "unsupported color format\n");
        goto ERR;
    }

    for(i = 0; i < imgb->np; i++)
    {
        imgb->aw[i] = LIBOAPV_ALIGN_VAL(imgb->w[i], OAPV_MB_W);
        imgb->s[i] = imgb->aw[i] * bd;
        imgb->ah[i] = LIBOAPV_ALIGN_VAL(imgb->h[i], OAPV_MB_H);
        imgb->e[i] = imgb->ah[i];

        imgb->bsize[i] = imgb->s[i] * imgb->e[i];
        imgb->a[i] = imgb->baddr[i] = libapvd_picbuf_alloc(imgb->bsize[i]);
        if(imgb->a[i] == NULL) goto ERR;

        memset(imgb->a[i], 0, imgb->bsize[i]);
    }
    imgb->cs = cs;
    imgb->addref = libapvd_imgb_addref;
    imgb->getref = libapvd_imgb_getref;
    imgb->release = libapvd_imgb_release;

    imgb->addref(imgb); /* increase reference count */
    return imgb;

ERR:
    av_log(avctx, AV_LOG_ERROR, "cannot create image buffer\n");
    if(imgb)
    {
        for (int i = 0; i < OAPV_MAX_CC; i++)
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
    oapvd_cdesc_t *cdsc = &(apvctx->cdsc);
    int ret;

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

    unsigned char *bs_buf = NULL;
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
    int frm_cnt[OAPV_MAX_NUM_FRAMES];

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
    
    memset(frm_cnt, 0, sizeof(int) * OAPV_MAX_NUM_FRAMES);
    memset(&ofrms, 0, sizeof(oapv_frms_t));
    memset(&aui, 0, sizeof(oapv_au_info_t));

    pkt_fd = av_packet_clone(pkt);
    av_packet_unref(pkt);

    bs_buf = pkt_fd->data;
    bs_buf_size = pkt_fd->size;

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
        frm = &ofrms.frm[i];

        if(frm->imgb != NULL && (frm->imgb->w[0] != finfo->w || frm->imgb->h[0] != finfo->h)) {
            frm->imgb->release(frm->imgb);
            frm->imgb = NULL;
        }

        if(frm->imgb == NULL) {
            if(apvctx->output_csp == 1) {
                frm->imgb = libapvd_imgb_create(finfo->w, finfo->h, OAPV_CS_SET(OAPV_CF_PLANAR2, 10, 0), avctx);
            } else {
                frm->imgb = libapvd_imgb_create(finfo->w, finfo->h, finfo->cs, avctx);
            }

            if(frm->imgb == NULL) {
                av_log(avctx, AV_LOG_ERROR, "cannot allocate image buffer (w:%d, h:%d, cs:%d)\n",
                        finfo->w, finfo->h, finfo->cs);

                ret = AVERROR_INVALIDDATA;
                goto end;
            }
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

    /* testing of metadata reading */
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

                ret = AVERROR_INVALIDDATA;
                goto end;
            }
        }
        if(pld != NULL)
            free(pld);
    }

    // @todo Write decoded frames into AVFrame objects
    // @notice The current implementation supports only 1 frame per access unit
    // 
    for(int i = 0; i < ofrms.num_frms; i++) {
        frm = &ofrms.frm[i];
        if(ofrms.num_frms > 0) {
            if(OAPV_CS_GET_BIT_DEPTH(frm->imgb->cs) != apvctx->output_depth) {
                if(imgb_w == NULL) {
                    imgb_w = libapvd_imgb_create(frm->imgb->w[0], frm->imgb->h[0],
                                            OAPV_CS_SET(OAPV_CS_GET_FORMAT(frm->imgb->cs), apvctx->output_depth, 0), avctx);
                    if(imgb_w == NULL) {
                        av_log(avctx, AV_LOG_ERROR,"cannot allocate image buffer (w:%d, h:%d, cs:%d)\n",
                                frm->imgb->w[0], frm->imgb->h[0], frm->imgb->cs);

                        ret = AVERROR_INVALIDDATA;
                        goto end;
                    }
                }
                libapvd_imgb_cpy(imgb_w, frm->imgb, avctx);
                imgb_o = imgb_w;
            }
            else {
                imgb_o = frm->imgb;
            }

            // @todo Copy decoded image into AVFrame object
            // 
            // @notice  The current implementation of the openAPV codec does not allow adding multiple frames to a single Access Unit.
            //          However, the final implementation of the codec is expected to support Access Units containing multiple frames.
            //          Therefore, a FIFO queue containing AVFrame objects should be used here and AVFrame objects should be added to the queue.
            //          In subsequent calls to the libapvd_receive_frame function, the next AVFrame objects should be returned from the queue.
            //
            ret = libapvd_image_copy(avctx, imgb_o, frame);
            if(ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Image copying error\n");

                imgb_o->release(frm->imgb);
                imgb_o = NULL;

                av_frame_unref(frame);

                goto end;
            }

            // Use ff_decode_frame_props_from_pkt() to fill frame properties
            ret = ff_decode_frame_props_from_pkt(avctx, frame, pkt_fd);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "ff_decode_frame_props_from_pkt error\n");

                frm->imgb->release(frm->imgb);
                frm->imgb = NULL;

                goto end;
            }

            // @todo Check why the following code causes a problem with APV stream playback
            // If frame->pkt_dts and frame->pts are set to a value other than AV_NOPTS_VALUE, the stream does not play properly.
            // Only one frame is displayed.
            //
            frame->pkt_dts = AV_NOPTS_VALUE;
            frame->pts = AV_NOPTS_VALUE;

            if (pkt_fd->flags & AV_PKT_FLAG_KEY) {
                frame->pict_type = AV_PICTURE_TYPE_I;
                frame->flags |= AV_FRAME_FLAG_KEY;
            }
            
            // apvd_t_pull uses pool of objects of type apv_imgb.
            // The pool size is equal MAX_PB_SIZE (26), so release object when it is no more needed
            imgb_o->release(frm->imgb);
            imgb_o = NULL;


            frm_cnt[i]++;
        }
    }

end:
    av_packet_free(&pkt_fd);

    if(imgb_w != NULL) {
        imgb_w->release(imgb_w);
        imgb_w = NULL;
    }

    for(int i = 0; i < ofrms.num_frms; i++) {
        if(ofrms.frm[i].imgb != NULL) {
            ofrms.frm[i].imgb->release(ofrms.frm[i].imgb);
            ofrms.frm[i].imgb = NULL;
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
        oapvd_delete(apvctx->id);
        apvctx->id = NULL;
    }

    if (apvctx->mid) {
        oapvm_rem_all(apvctx->mid);
        oapvm_delete(apvctx->mid);
        apvctx->mid = NULL;
    }

    av_packet_free(&apvctx->pkt);

    return 0;
}

#define OFFSET(x) offsetof(ApvDecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

// Consider using following options (./ffmpeg --help encoder=libapv)
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
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_PKT_DTS | FF_CODEC_CAP_SETS_FRAME_PROPS
};
