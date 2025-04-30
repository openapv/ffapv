/*
 * APV helper functions for muxers
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

#ifndef AVFORMAT_APV_H
#define AVFORMAT_APV_H

#include <stdint.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/rational.h"
#include "libavcodec/apv.h"
#include "avio.h"

#define APV_AU_SIZE_PREFIX_LENGTH (4)

static inline uint32_t apv_read_au_size(const uint8_t *bits, int bits_size)
{
    if (bits_size >= APV_AU_SIZE_PREFIX_LENGTH)
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

/**
 * @brief Creates and allocates memory for an APV decoder configuration record.
 *
 * This function allocates memory for an APVDecoderConfigurationRecord and
 * initializes it. The size of the record is returned through the `size` parameter.
 *
 * @param data Pointer to a pointer where the allocated data will be stored.
 * @param size Pointer to an integer where the size of the allocated record will be stored.
 * @return 0 on success, or AVERROR_INVALIDDATA if memory allocation fails.
 */
int ff_isom_create_apv_dconf_record(uint8_t **data, int *size);

/**
 * @brief Frees the memory allocated for the APV decoder configuration record.
 * 
 * @param data data to be freed
 */
void ff_isom_free_apv_dconf_record(uint8_t **data);

/**
 * @brief Fills an APV decoder configuration record with data.
 *
 * This function populates the APVDecoderConfigurationRecord pointed to by
 * `apvdcr` with the data from `data`, which has a specified size. The data
 * represents an access unit.
 *
 * @param apvdcr Pointer to the APVDecoderConfigurationRecord to be filled.
 * @param data Pointer to the data to fill the record with.
 * @param size Size of the data to be copied into the record.
 * @return 0 on success, or a negative value on error.
 */
int ff_isom_fill_apv_dconf_record(const uint8_t *apvc, const uint8_t *data, int size);

#endif // AVFORMAT_APV_H
