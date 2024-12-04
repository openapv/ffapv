/*
 * APV definitions and enums
 *
 * Copyright (c) 2024 Dawid Kozinski <d.kozinski@samsung.com>
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

#ifndef AVCODEC_APV_IMGB_H
#define AVCODEC_APV_IMGB_H

#include <oapv/oapv.h>

oapv_imgb_t* apv_imgb_create(int w, int h, int cs, AVCodecContext *avctx);
int apv_imgb_release(oapv_imgb_t* imgb);

void apv_imgb_cpy(oapv_imgb_t *dst, oapv_imgb_t *src, AVCodecContext *avctx);

int apv_imgb_addref(oapv_imgb_t* imgb);
int apv_imgb_getref(oapv_imgb_t* imgb);

#endif // AVCODEC_APV_IMGB_H
