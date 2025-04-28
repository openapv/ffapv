/*
 * Copyright (c) 2025 Dawid Kozinski <d.kozinski@samsung.com>
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
#include <libavcodec/avcodec.h>

/* assert function */
#include <assert.h>

#include "apv_imgb.h"

#if defined(_MSC_VER) // Microsoft Visual C++
#include <intrin.h>
#define apv_atomic_inc(pcnt) _InterlockedIncrement(pcnt)
#define apv_atomic_dec(pcnt) _InterlockedDecrement(pcnt)

#elif defined(__GNUC__) || defined(__clang__) // GCC i Clang
#define apv_atomic_inc(pcnt) __sync_fetch_and_add(pcnt, 1) + 1
#define apv_atomic_dec(pcnt) __sync_fetch_and_sub(pcnt, 1) - 1

#else // In other cases, use mutexes
#include <pthread.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int apv_atomic_inc(volatile int* pcnt) {
    pthread_mutex_lock(&lock);
    int new_value = ++(*pcnt);
    pthread_mutex_unlock(&lock);
    return new_value;
}

static int apv_atomic_dec(volatile int* pcnt) {
    pthread_mutex_lock(&lock);
    int new_value = --(*pcnt);
    pthread_mutex_unlock(&lock);
    return new_value;
}
#endif

#define assert_rv(x,r) {if(!(x)){assert(x); return (r);}}

#define OAPV_IMG_CLIP_VAL(n, min, max) (((n) > (max)) ? (max) : (((n) < (min)) ? (min) : (n)))
#define OAPV_IMG_ALIGN_VAL(val, align) ((((val) + (align) - 1) / (align)) * (align))

static void * apv_picbuf_alloc(int size)
{
    return malloc(size);
}

static void apv_picbuf_free(void* p)
{
    if (p) {free(p);}
}

static void apv_imgb_cpy_plane(oapv_imgb_t *dst, oapv_imgb_t *src)
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

static void apv_imgb_cpy_shift_left_8b(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
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

static void apv_imgb_cpy_shift_right_8b(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
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
                d[k] = (unsigned char)(OAPV_IMG_CLIP_VAL(t0, 0, 255));
            }
            s = (short *)(((unsigned char *)s) + src->s[i]);
            d = d + dst->s[i];
        }
    }
}

static void apv_imgb_cpy_shift_left(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
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

static void apv_imgb_cpy_shift_right(oapv_imgb_t *dst, oapv_imgb_t *src, int shift)
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
                d[k] = (OAPV_IMG_CLIP_VAL(t0, clip_min, clip_max));
            }
            s = (unsigned short *)(((unsigned char *)s) + src->s[i]);
            d = (unsigned short *)(((unsigned char *)d) + dst->s[i]);
        }
    }
}

oapv_imgb_t * apv_imgb_create(int w, int h, int cs, AVCodecContext *avctx)
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
        imgb->aw[i] = OAPV_IMG_ALIGN_VAL(imgb->w[i], OAPV_MB_W);
        imgb->s[i] = imgb->aw[i] * bd;
        imgb->ah[i] = OAPV_IMG_ALIGN_VAL(imgb->h[i], OAPV_MB_H);
        imgb->e[i] = imgb->ah[i];

        imgb->bsize[i] = imgb->s[i] * imgb->e[i];
        imgb->a[i] = imgb->baddr[i] = apv_picbuf_alloc(imgb->bsize[i]);
        if(imgb->a[i] == NULL) goto ERR;

        memset(imgb->a[i], 0, imgb->bsize[i]);
    }
    imgb->cs = cs;
    imgb->addref = apv_imgb_addref;
    imgb->getref = apv_imgb_getref;
    imgb->release = apv_imgb_release;

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

int apv_imgb_release(oapv_imgb_t * imgb)
{
    int refcnt, i;
    assert_rv(imgb, OAPV_ERR_INVALID_ARGUMENT);
    refcnt = apv_atomic_dec(&imgb->refcnt);
    if(refcnt == 0) {
        for(i=0; i<OAPV_MAX_CC; i++) {
            if (imgb->baddr[i]) apv_picbuf_free(imgb->baddr[i]);
        }
        free(imgb);
    }

    return refcnt;
}

void apv_imgb_cpy(oapv_imgb_t *dst, oapv_imgb_t *src, AVCodecContext *avctx)
{
    int i, bd_src, bd_dst;
    bd_src = OAPV_CS_GET_BIT_DEPTH(src->cs);
    bd_dst = OAPV_CS_GET_BIT_DEPTH(dst->cs);

    if(src->cs == dst->cs) {
        apv_imgb_cpy_plane(dst, src);
    }
    else if(bd_src == 8 && bd_dst > 8) {
        apv_imgb_cpy_shift_left_8b(dst, src, bd_dst - bd_src);
    }
    else if(bd_src > 8 && bd_dst == 8) {
        apv_imgb_cpy_shift_right_8b(dst, src, bd_src - bd_dst);
    }
    else if(bd_src < bd_dst) {
        apv_imgb_cpy_shift_left(dst, src, bd_dst - bd_src);
    }
    else if(bd_src > bd_dst) {
        apv_imgb_cpy_shift_right(dst, src, bd_src - bd_dst);
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

int apv_imgb_addref(oapv_imgb_t * imgb)
{
    assert_rv(imgb, OAPV_ERR_INVALID_ARGUMENT);
    return apv_atomic_inc(&imgb->refcnt);
}

int apv_imgb_getref(oapv_imgb_t * imgb)
{
    assert_rv(imgb, OAPV_ERR_INVALID_ARGUMENT);
    return imgb->refcnt;
}