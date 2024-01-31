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

#include "libavutil/intreadwrite.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/apv.h"
#include "avformat.h"
#include "avio.h"
#include "apv.h"
#include "avio_internal.h"

/**
 * @brief Specifies the decoder configuration information for APV video content.
 */
typedef struct APVDecoderConfigurationRecord {
    uint8_t  configurationVersion;          // 8 bits
    uint8_t  profile_idc;                   // 8 bits
    uint8_t  level_idc;                     // 8 bits
    uint8_t  chroma_format_idc;             // 4 bits // The reference application uses 4 bits for chroma_format_idc while the documentation says that chroma_format_idc takes up 2 bits in the
    uint8_t  bit_depth_minus8;              // 4 bits
    uint32_t pic_width_in_luma_samples;     // 32 bits
    uint32_t pic_height_in_luma_samples;    // 32 bits
} APVDecoderConfigurationRecord;

static int apvc_parse_frame_header(const uint8_t *bs, int bs_size, APVDecoderConfigurationRecord *apvc)
{
    GetBitContext gb;

    uint8_t reserved_zero_8bits;
    uint32_t frame_width_minus1;
    uint32_t frame_height_minus1;
    uint8_t bit_depth_minus8;

    int ret = init_get_bits8(&gb, bs, bs_size);
    if (ret < 0)
        return ret;

    // skip frame_header_size bits 
    skip_bits_long(&gb, 16);

    apvc->profile_idc = get_bits(&gb, 8); // @todo check value range AnnexA
    apvc->level_idc = get_bits(&gb, 8);   // @todo check value range AnnexA

    reserved_zero_8bits = get_bits(&gb, 8);
    if (reserved_zero_8bits!=0)
        return AVERROR_INVALIDDATA;

    frame_width_minus1 = get_bits(&gb, 32);
    apvc->pic_width_in_luma_samples = frame_width_minus1 + 1;

    frame_height_minus1 = get_bits(&gb, 32);
    apvc->pic_height_in_luma_samples = frame_height_minus1 + 1;

    // @todo documentation inconsistency with the reference implementation.
    //
    // The reference application uses 4 bits for chroma_format_idc
    // while the documentation says that chroma_format_idc takes up 2 bits in the header
    // 0 - monochrome
    // 1 - 4:2:0
    // 2 - 4:2:2
    // 3 - 4:4:4
    apvc->chroma_format_idc = get_bits(&gb, 4);
    if (apvc->chroma_format_idc  < 2 || apvc->chroma_format_idc > 3)
        return AVERROR_INVALIDDATA;

    bit_depth_minus8 = get_bits(&gb, 4);
    if (bit_depth_minus8  < 2 || bit_depth_minus8 > 8)

    apvc->bit_depth_minus8  = bit_depth_minus8;

    return 0;
}

static void apvc_init(APVDecoderConfigurationRecord *apvc)
{
    memset(apvc, 0, sizeof(APVDecoderConfigurationRecord));
    apvc->configurationVersion = 1;
}

static void apvc_close(APVDecoderConfigurationRecord *apvc)
{
}

static int apvc_write(AVIOContext *pb, APVDecoderConfigurationRecord *apvc)
{
    av_log(NULL, AV_LOG_TRACE,  "configurationVersion:                %"PRIu8"\n",
           apvc->configurationVersion);
    av_log(NULL, AV_LOG_TRACE,  "profile_idc:                         %"PRIu8"\n",
           apvc->profile_idc);
    av_log(NULL, AV_LOG_TRACE,  "level_idc:                           %"PRIu8"\n",
           apvc->level_idc);
    av_log(NULL, AV_LOG_TRACE, "chroma_format_idc:                    %"PRIu8"\n",
           apvc->chroma_format_idc);
    av_log(NULL, AV_LOG_TRACE,  "bit_depth_luma_minus8:               %"PRIu8"\n",
           apvc->bit_depth_minus8);
    av_log(NULL, AV_LOG_TRACE,  "pic_width_in_luma_samples:           %"PRIu32"\n",
           apvc->pic_width_in_luma_samples);
    av_log(NULL, AV_LOG_TRACE,  "pic_height_in_luma_samples:          %"PRIu32"\n",
           apvc->pic_height_in_luma_samples);

    /* unsigned int(8) configurationVersion = 1; */
    avio_w8(pb, apvc->configurationVersion);

    /* unsigned int(8) profile_idc */
    avio_w8(pb, apvc->profile_idc);

    /* unsigned int(8) profile_idc */
    avio_w8(pb, apvc->level_idc);

    /*
     * unsigned int(4) chroma_format_idc;
     * unsigned int(4) bit_depth_minus8;
     */
    avio_w8(pb, apvc->chroma_format_idc << 4 |
            apvc->bit_depth_minus8 );

    /* unsigned int(32) pic_width_in_luma_samples; */
    avio_wb32(pb, apvc->pic_width_in_luma_samples);

    /* unsigned int(32) pic_width_in_luma_samples; */
    avio_wb32(pb, apvc->pic_height_in_luma_samples);

    return 0;
}

int ff_isom_write_apvc(AVIOContext *pb, const uint8_t *data,
                       int size, int ps_array_completeness)
{
    APVDecoderConfigurationRecord apvc;
    size_t frame_data_size;
    uint16_t frame_header_size;

    int bytes_to_read = size;

    int ret = 0;

    if (size < 8) {
        /* We can't write a valid apvC from the provided data */
        return AVERROR_INVALIDDATA;
    } else if (*data == 1) {
        /* Data is already apvC-formatted */
        avio_write(pb, data, size);
        return 0;
    }

    apvc_init(&apvc);

    // Todo change to if
    if (bytes_to_read > APV_FRAME_DATA_SIZE_PREFIX_LENGTH) {
        frame_data_size = apv_read_frame_data_size(data, APV_FRAME_DATA_SIZE_PREFIX_LENGTH);
        if (frame_data_size == 0) goto end;

        data += APV_FRAME_DATA_SIZE_PREFIX_LENGTH;
        bytes_to_read -= APV_FRAME_DATA_SIZE_PREFIX_LENGTH;

        if (bytes_to_read < frame_data_size) goto end;

        frame_header_size = AV_RB16(data);

        ret = apvc_parse_frame_header(data, frame_header_size, &apvc);
        if (ret < 0)
            goto end;
    }

    ret = apvc_write(pb, &apvc);

end:
    apvc_close(&apvc);
    return ret;
}
