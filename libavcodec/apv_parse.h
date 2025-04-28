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

/**
 * @file
 * APV decoder/parser shared code
 */

#ifndef AVCODEC_APV_PARSE_H
#define AVCODEC_APV_PARSE_H

#include <stdint.h>

#include "libavutil/log.h"
#include "apv.h"
#include "apv_ps.h"

static inline uint32_t apv_read_frame_data_size(const uint8_t *bits, int bits_size, void *logctx)
{
    uint32_t frame_data_size = 0;

    if (bits_size < APV_FRAME_DATA_SIZE_PREFIX_LENGTH) {
        av_log(logctx, AV_LOG_ERROR, "Can't read Frame Data size\n");
        return 0;
    }

    frame_data_size = AV_RB32(bits);

    return frame_data_size;
}

// @see 12.1. Raw bitstream format [https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-12.1]
static inline uint32_t apv_read_au_size(const uint8_t *bits, int bits_size, void *logctx)
{
    uint32_t au_size = 0;

    if (bits_size < APV_AU_SIZE_PREFIX_LENGTH) {
        av_log(logctx, AV_LOG_ERROR, "Can't read Access Unit size\n");
        return 0;
    }

    au_size = AV_RB32(bits);

    return au_size;
}

static inline uint32_t apv_read_pbu_size(const uint8_t *bits, int bits_size, void *logctx)
{
    uint32_t pbu_size = 0;

    if (bits_size < APV_PBU_SIZE_PREFIX_LENGTH) {
        av_log(logctx, AV_LOG_ERROR, "Can't read PBU (primitive bitstream unit) size\n");
        return 0;
    }

    pbu_size = AV_RB32(bits);

    return pbu_size;
}

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame
int ff_apv_parse_frame_data(GetBitContext *gb, APVFrameData *fd);

#endif /* AVCODEC_APV_PARSE_H */
