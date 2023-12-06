/*
 * APV helper functions for muxers
 * Copyright (c) 2023 Dawid Kozinski <d.kozinski@samsung.com>
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

#ifndef AVFORMAT_APV_H
#define AVFORMAT_APV_H

#include <stdint.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/rational.h"
#include "libavcodec/apv.h"
#include "avio.h"


static inline uint32_t apv_read_frame_data_size(const uint8_t *bits, int bits_size)
{
    if (bits_size >= APV_FRAME_DATA_SIZE_PREFIX_LENGTH)
        return AV_RB32(bits);

    return 0;
}

/**
 * Writes APV sample metadata to the provided AVIOContext.
 *
 * @param pb pointer to the AVIOContext where the apv sample metadata shall be written
 * @param buf input data buffer
 * @param size size in bytes of the input data buffer
 * @param ps_array_completeness
 *
 * @return 0 in case of success, a negative error code in case of failure
 */
int ff_isom_write_apvc(AVIOContext *pb, const uint8_t *data,
                       int size, int ps_array_completeness);

#endif // AVFORMAT_APV_H
