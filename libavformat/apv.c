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

 #include <stdbool.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/apv.h"
#include "avformat.h"
#include "avio.h"
#include "apv.h"
#include "avio_internal.h"


#define TILE_COLS_MAX 20
#define TILE_ROWS_MAX 20
#define NUM_TILES_MAX TILE_COLS_MAX*TILE_ROWS_MAX 

#define NUM_COMP_MAX 4

#define MB_WIDTH    16
#define MB_HEIGHT   16

/*****************************************************************************
 * PBU types
 *****************************************************************************/
#define APV_PBU_TYPE_RESERVED          (0)
#define APV_PBU_TYPE_PRIMARY_FRAME     (1)
#define APV_PBU_TYPE_NON_PRIMARY_FRAME (2)
#define APV_PBU_TYPE_PREVIEW_FRAME     (25)
#define APV_PBU_TYPE_DEPTH_FRAME       (26)
#define APV_PBU_TYPE_ALPHA_FRAME       (27)
#define APV_PBU_TYPE_AU_INFO           (65)
#define APV_PBU_TYPE_METADATA          (66)
#define APV_PBU_TYPE_FILLER            (67)
#define APV_PBU_TYPE_UNKNOWN           (-1)
#define APV_PBU_NUMS                   (10)

#define APV_FRAME_TYPE_PRIMARY_FRAME     (0)
#define APV_FRAME_TYPE_NON_PRIMARY_FRAME (1)
#define APV_FRAME_TYPE_PREVIEW_FRAME     (2)
#define APV_FRAME_TYPE_DEPTH_FRAME       (3)
#define APV_FRAME_TYPE_ALPHA_FRAME       (4)
#define APV_FRAME_TYPE_NON_FRAME         (-1)
#define APV_PBU_FRAME_TYPE_NUM           (5)
#define CONFIGURATIONS_MAX               (APV_PBU_FRAME_TYPE_NUM)

#define APV_PBU_SIZE_PREFIX_LENGTH       (4)
#define APV_SIGNATURE_LENGTH             (4)

typedef struct APVDecoderFrameInfo {
    uint8_t reserved_zero_6bits;                    // 6 bits
    uint8_t color_description_present_flag;         // 1 bit

    // The variable indicates whether the capture_time_distance value in the APV bitstream's frame header should be ignored during playback.
    // If capture_time_distance_ignored is set to true, the capture_time_distance information will not be utilized,
    // and timing information for playback should be calculated using an alternative method.
    // If set to false, the capture_time_distance value will be used as is from the frame header.
    // It is recommended to set this variable to true, allowing the use of MP4 timestamps for playback and recording,
    // which enables the conventional compression and playback methods based on the timestamp table defined by the ISO-based file format.
    uint8_t capture_time_distance_ignored;          // 1-bit

    uint8_t profile_idc;                            // 8 bits 
    uint8_t level_idc;                              // 8 bits
    uint8_t band_idc;                               // 8 bits
    uint32_t frame_width;                           // 32 bits
    uint32_t frame_height;                          // 32 bits
    uint8_t chroma_format_idc;                      // 4 bits
    uint8_t bit_depth_minus8;                       // 4 bits
    uint8_t capture_time_distance;                  // 8 bits

    // if (color_description_present_flag)
    uint8_t color_primaries;                        // 8 bits
    uint8_t transfer_characteristics;               // 8 bits
    uint8_t matrix_coefficients;                    // 8 bits
    uint8_t full_range_flag;                        // 1 bit
    uint8_t reserved_zero_7bits;                    // 7 bits

} APVDecoderFrameInfo;

typedef struct APVDecoderConfigurationEntry {
    uint8_t pbu_type;                   // 8 bits
    uint8_t number_of_frame_info;       // 8 bits

    APVDecoderFrameInfo** frame_info;   // An array of size number_of_frame_info storing elements of type APVDecoderFrameInfo*

} APVDecoderConfigurationEntry;

// ISOBMFF binding for APV
// @see https://github.com/openapv/openapv/blob/main/readme/apv_isobmff.md
typedef struct APVDecoderConfigurationRecord  {
    uint8_t configurationVersion;           // 8 bits
    uint8_t number_of_configuration_entry;  // 8 bits

    APVDecoderConfigurationEntry configuration_entry[CONFIGURATIONS_MAX]; // table of size number_of_configuration_entry

} APVDecoderConfigurationRecord ;

// 5.3.6. Frame information
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame-information
typedef struct frame_info_t {
    uint8_t profile_idc;            // 8 bits
    uint8_t level_idc;              // 8 bits
    uint8_t band_idc;               // 3 bits
    uint8_t reserved_zero_5bits;    // 5 bits

    uint32_t frame_width;           // 24 bits
    uint32_t frame_height;          // 24 bits

    uint8_t chroma_format_idc;      // 4 bits
    uint8_t bit_depth_minus8;       // 4 bits

    uint8_t capture_time_distance;  // 8 bits
    uint8_t reserved_zero_8bit;     // 8 bits
} frame_info_t;

// 5.3.7. Quantization matrix 
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-quantization-matrix
typedef struct quantization_matrix_t {
    uint8_t q_matrix[3][8][8]; // @todo use NumCmp instead of 3
} quantization_matrix_t;

// 5.3.8. Tile info
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-tile-info
typedef struct tile_info_t {
    uint32_t tile_width_in_mbs;                             // u(20)
    uint32_t tile_height_in_mbs;                            // u(20)
    uint8_t tile_size_present_in_fh_flag;                   // u(1)
    uint32_t tile_size_in_fh[TILE_ROWS_MAX*TILE_COLS_MAX];  // u(32) -> size of table: NumTiles

    // private
    uint32_t ColStarts[TILE_COLS_MAX];
    uint32_t RowStarts[TILE_ROWS_MAX];

    uint32_t TileCols; // The value of TileCols MUST be less than or equal to TILE_COLS_MAX.
    uint32_t TileRows; // The value of TileRows MUST be less than or equal to TILE_ROWS_MAX.
    uint32_t NumTiles;

} tile_info_t;


// 5.3.17. Byte alignment
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-byte-alignment
typedef struct byte_alignment_t {
    uint8_t alignment_bit_equal_to_zero; /* equal to 0*/ // f(1)
} byte_alignment_t;

// 5.3.5. Frame header
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame-header
typedef struct frame_header_t {
    frame_info_t frame_info;
    uint8_t reserved_zero_8bits;                // 8 bits
    uint8_t color_description_present_flag;     // 1 bits

    uint8_t color_primaries;                    // 8 bits
    uint8_t transfer_characteristics;           // 8 bits
    uint8_t matrix_coefficients;                // 8 bits
    uint8_t full_range_flag;                    // 1 bit

    uint8_t use_q_matrix;                       // 1 bit

    quantization_matrix_t quantization_matrix;
    tile_info_t tile_info;

    uint8_t reserved_zero_8bits_2;              // 8 bits
    byte_alignment_t byte_alignment_t;
} frame_header_t;

// 5.3.11. Filler
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-filler
typedef struct filler_t {
    uint8_t *ff_byte; /* is a byte equal to 0xFF */ // f(8)
} filler_t;

// 5.3.13. Tile header
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-tile-header
typedef struct tile_header_t {
    uint16_t tile_header_size;                  // u(16)
    uint16_t tile_index;                        // u(16)
    uint32_t tile_data_size[NUM_COMP_MAX];      // u(32) table of size NumComps MAX = 4
    uint8_t tile_qp[NUM_COMP_MAX];              // u(8) table of size NumComps
    uint8_t reserved_zero_8bits;                // u(8)
    byte_alignment_t byte_alignment;

} tile_header_t;

// 5.3.16. AC coefficient coding
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-ac-coefficient-coding
typedef struct   {
    uint8_t coeff_zero_run;
    uint8_t abs_ac_coeff_minus1;
    uint8_t sign_ac_coeff;
} ac_coeff_coding_t;

// 5.3.15. Macroblock layer
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-macroblock-layer
typedef struct macroblock_layer_t {
    uint8_t abs_dc_coeff_diff;  // h(v)
    uint8_t sign_dc_coeff_diff; // u(1)
    ac_coeff_coding_t ac_coeff_coding;
} macroblock_layer_t;

// 5.3.14. Tile data
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-tile-data
typedef struct tile_data_t {
    macroblock_layer_t macroblock_layer;
    byte_alignment_t byte_alignment;
} tile_data_t;


// 5.3.11. Filler
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-filler
typedef struct tile_t {
    tile_header_t tile_header;
    uint8_t tile_data;
    uint8_t ff_byte; /* is a byte equal to 0xFF */ // f(8)
} tile_t;

// 5.3.4 Frame
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame
typedef struct frame_t {
    frame_header_t frame_header;
    uint32_t tile_size[NUM_TILES_MAX];           // table of NumTiles size
    tile_t tile[NUM_TILES_MAX];                   // table of NumTiles size
    filler_t filler; 
} frame_t;

// 5.3.3. Primitive bitstream unit header
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-primitive-bitstream-unit-he
typedef struct pbu_header_t {
    uint8_t pbu_type;               // 8 bits
    uint16_t group_id;              // 16 bits
    uint8_t reserved_zero_8bits;    // 8 bits
} pbu_header_t;

// 5.3.9. Access unit information 
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-access-unit-information
typedef struct au_info_t {
    uint16_t num_frames;            // 16 bits

    uint8_t pbu_type;               // 8 bits
    uint16_t group_id;              // 16 bits
    uint8_t reserved_zero_8bits;    // 8 bits
    frame_info_t frame_info;
    
    uint8_t reserved_zero_8bits_2;  // 8 bits
    byte_alignment_t byte_alignment;

} au_info_t;

typedef struct {
    uint64_t high;
    uint64_t low;
} uint128_t;

// 5.3.10. Metadata 
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-metadata
typedef struct metadata_t {
    uint16_t metadata_size;             // 32 bits

    /* is a byte equal to 0xFF */   
    uint8_t *ff_byte;                   // f(8) table of size payloadSize
    uint8_t metadata_payload_type;      // u(8)

    /* is a byte equal to 0xFF */
    uint8_t ff_byte_2;                  // f(8)
    uint8_t metadata_payload_size;      // u(8)

    // metadata ITU-T T.35
    uint8_t itu_t_t35_country_code;             //  u(8)
    uint8_t itu_t_t35_country_code_extension;   //  u(8)
    uint8_t itu_t_t35_payload;                  //  u(8)

    // metadata mdcv (Mastering display color volume metadata)
    uint16_t primary_chromaticity_x[3]; // u(16)
    uint16_t primary_chromaticity_y[3]; // u(16)
    uint16_t white_point_chromaticity_x; // u(16)
    uint16_t white_point_chromaticity_y; // u(16)
    uint32_t max_mastering_luminance;    // u(32)
    uint32_t min_mastering_luminance;    // u(32)

    // metadata ccl (Content light level information metadata)
    uint16_t max_cll;  // u(16)
    uint16_t max_fall; // u(16)

    // User defined metadata
    uint128_t uuid;
    uint8_t *user_defined_data_payload; // table of size payloadSize - 16

    // Undefined metadata
    uint8_t *undefined_metadata_payload_byte; // table of size payloadSize

    filler_t filler;

    uint8_t alignment_bit_equal_to_zero;
} metadata_t;

// 5.3.2. Primitive bitstream unit
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-primitive-bitstream-unit
typedef struct pbu_t {
    pbu_header_t pbu_header;
    frame_t frame;
    au_info_t au_info;
    metadata_t metadata;
    filler_t filler;
} pbu_t;

static int apv_read_pbu_header(GetBitContext *gb, pbu_header_t *pbu_header)
{
    pbu_header->pbu_type = get_bits(gb, 8);
    pbu_header->group_id = get_bits(gb, 16);
    pbu_header->reserved_zero_8bits = get_bits(gb, 8);

    return 0;
}

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame-information
static int apv_read_frame_info(GetBitContext *gb, frame_info_t *frame_info)
{
    uint8_t reserved_zero_5bits;
    uint8_t reserved_zero_8bits;

    frame_info->profile_idc = get_bits(gb, 8);
    frame_info->level_idc = get_bits(gb, 8);
    frame_info->band_idc = get_bits(gb, 3);
    
    reserved_zero_5bits =  get_bits(gb, 5);

    frame_info->frame_width = get_bits(gb,24);
    frame_info->frame_height = get_bits(gb,24);

    frame_info->chroma_format_idc = get_bits(gb, 4);
    frame_info->bit_depth_minus8 = get_bits(gb, 4);

    frame_info->capture_time_distance = get_bits(gb, 8);
    reserved_zero_8bits = get_bits(gb, 8);
    
    if (reserved_zero_5bits != 0x00)
        return -1;  

    if (reserved_zero_8bits != 0x00)
        return -1;  

    return 0;
}

static int apv_read_frame_header(GetBitContext *gb, frame_header_t *frame_header)
{
    uint8_t reserved_zero_8bits[2];

    apv_read_frame_info(gb, &frame_header->frame_info);
    
    reserved_zero_8bits[0] = get_bits(gb, 8);
    if(reserved_zero_8bits[0]!=0) {
        return -1;
    }

    frame_header->color_description_present_flag = get_bits(gb, 1);
    if(frame_header->color_description_present_flag) {
        frame_header->color_primaries = get_bits(gb, 8);
        frame_header->transfer_characteristics = get_bits(gb, 8);
        frame_header->matrix_coefficients = get_bits(gb, 8);
        frame_header->full_range_flag = get_bits(gb, 1);
    }

    // The further parsing of AU is not needed

    return 0;
}

static inline uint32_t apv_read_pbu_size(const uint8_t *bits, int bits_size)
{
    uint32_t pbu_size = 0;

    if (bits_size < APV_PBU_SIZE_PREFIX_LENGTH) {
        av_log(NULL, AV_LOG_ERROR, "Can't read PBU (primitive bitstream unit) size\n");
        return 0;
    }

    pbu_size = AV_RB32(bits);

    return pbu_size;
}

static int apv_read_au_info(GetBitContext *gb, au_info_t *au_info)
{
    int ret = 0;
    au_info->num_frames = get_bits(gb, 16);

    for(int idx=0; idx<au_info->num_frames; idx++) {
        au_info->pbu_type = get_bits(gb, 8);
        au_info->group_id = get_bits(gb, 16);
        au_info->reserved_zero_8bits = get_bits(gb, 8);
        
        ret = apv_read_frame_info(gb, &au_info->frame_info);
        if(ret!=0) return ret;
    }
    au_info->reserved_zero_8bits_2 = get_bits(gb, 8);
    if(au_info->reserved_zero_8bits_2!=0) return -1;

    // The further parsing of AU Info is not needed

    return ret;
}

static int apv_read_frame(GetBitContext *gb, frame_t *frame)
{
    apv_read_frame_header(gb, &frame->frame_header);

    // The further parsing of frame is not needed

    return 0;
}

// primitive bytestream unit
static int apv_read_pbu(const uint8_t *bs, int bs_size, pbu_t *pbu)
{
    GetBitContext gb;
    
    int ret = init_get_bits8(&gb, bs, bs_size);
    if (ret < 0)
        return ret;

    ret = apv_read_pbu_header(&gb, &pbu->pbu_header);
    if (ret < 0)
        return ret;
    if(( 1 <= pbu->pbu_header.pbu_type && pbu->pbu_header.pbu_type <= 2) ||
       ( 25 <= pbu->pbu_header.pbu_type  && pbu->pbu_header.pbu_type  <= 27) ) {
        ret = apv_read_frame(&gb, &pbu->frame);
    } else if(pbu->pbu_header.pbu_type ==65) {
        ret = apv_read_au_info(&gb, &pbu->au_info);
    } else  {
        ret = -1;
    }

    return ret;
}

static int apv_number_of_pbu_entry(const uint8_t *data, uint32_t au_size) {
    uint32_t currReadSize = 0;
    int ret = 0;
    
    do {
        uint32_t pbu_size = apv_read_pbu_size(data + currReadSize, APV_PBU_SIZE_PREFIX_LENGTH);
        if (pbu_size == 0) return -1;

        currReadSize += APV_PBU_SIZE_PREFIX_LENGTH;
        currReadSize += pbu_size;
        ret++;
    } while(au_size > currReadSize);

    return ret;
}

static void apvc_init(APVDecoderConfigurationRecord * apvc)
{
    memset(apvc, 0, sizeof(APVDecoderConfigurationRecord ));
    apvc->configurationVersion = 1;
}

static void apvc_close(APVDecoderConfigurationRecord *apvc)
{
    for(int i=0;i<apvc->number_of_configuration_entry;i++) {
        for(int j=0;j<apvc->configuration_entry[i].number_of_frame_info;j++) {
            free(apvc->configuration_entry[i].frame_info[j]);
        } 
        free(apvc->configuration_entry[i].frame_info);
        apvc->configuration_entry[i].number_of_frame_info = 0;
    }
    apvc->number_of_configuration_entry = 0;
}

static int apvc_write(AVIOContext *pb, APVDecoderConfigurationRecord * apvc)
{
    av_log(NULL, AV_LOG_TRACE, "configurationVersion:                           %"PRIu8"\n", 
    apvc->configurationVersion);
    
    av_log(NULL, AV_LOG_TRACE, "number_of_configuration_entry:                  %"PRIu8"\n", 
    apvc->number_of_configuration_entry);

    for(int i=0; i<apvc->number_of_configuration_entry;i++) {
        av_log(NULL, AV_LOG_TRACE, "pbu_type:                                   %"PRIu8"\n", 
        apvc->configuration_entry[i].pbu_type);

        av_log(NULL, AV_LOG_TRACE, "number_of_frame_info:                       %"PRIu8"\n", 
        apvc->configuration_entry[i].number_of_frame_info);

        for(int j=0; j < apvc->configuration_entry[i].number_of_frame_info; j++) {
            av_log(NULL, AV_LOG_TRACE, "color_description_present_flag:         %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->color_description_present_flag);

            av_log(NULL, AV_LOG_TRACE, "capture_time_distance_ignored:          %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->capture_time_distance_ignored);

            av_log(NULL, AV_LOG_TRACE, "profile_idc:                            %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->profile_idc);

            av_log(NULL, AV_LOG_TRACE, "level_idc:                              %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->level_idc);

            av_log(NULL, AV_LOG_TRACE, "band_idc:                               %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->band_idc);

            av_log(NULL, AV_LOG_TRACE, "frame_width:                            %"PRIu32"\n", 
            apvc->configuration_entry[i].frame_info[j]->frame_width);

            av_log(NULL, AV_LOG_TRACE, "frame_height:                           %"PRIu32"\n", 
            apvc->configuration_entry[i].frame_info[j]->frame_height);

            av_log(NULL, AV_LOG_TRACE, "chroma_format_idc:                      %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->chroma_format_idc);

            av_log(NULL, AV_LOG_TRACE, "bit_depth_minus8:                       %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->bit_depth_minus8);

            av_log(NULL, AV_LOG_TRACE, "capture_time_distance:                  %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j]->capture_time_distance);

            if(apvc->configuration_entry[i].frame_info[j]->color_description_present_flag) {
          
                av_log(NULL, AV_LOG_TRACE, "color_primaries:                    %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j]->color_primaries);
                
                av_log(NULL, AV_LOG_TRACE, "transfer_characteristics:           %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j]->transfer_characteristics);
                
                av_log(NULL, AV_LOG_TRACE, "matrix_coefficients:                %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j]->matrix_coefficients);

                av_log(NULL, AV_LOG_TRACE, "full_range_flag:                    %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j]->full_range_flag);
            }

        }
    }
    
    /* unsigned int(8) configurationVersion = 1; */
    avio_w8(pb, apvc->configurationVersion);

    avio_w8(pb, apvc->number_of_configuration_entry);
    
    for(int i=0; i<apvc->number_of_configuration_entry;i++) {
        avio_w8(pb, apvc->configuration_entry[i].pbu_type);
        avio_w8(pb, apvc->configuration_entry[i].number_of_frame_info);

        for(int j=0; j < apvc->configuration_entry[i].number_of_frame_info; j++) {

            /* unsigned int(6) reserved_zero_6bits
            * unsigned int(1) color_description_present_flag
            * unsigned int(1) capture_time_distance_ignored
            */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->reserved_zero_6bits << 2 |
                        apvc->configuration_entry[i].frame_info[j]->color_description_present_flag << 1 | 
                        apvc->configuration_entry[i].frame_info[j]->capture_time_distance_ignored);
            
            /* unsigned int(8) profile_idc */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->profile_idc);

            /* unsigned int(8) level_idc */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->level_idc);

            /* unsigned int(8) band_idc */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->band_idc);
            
            /* unsigned int(32) frame_width_minus1 */
            avio_wb32(pb, apvc->configuration_entry[i].frame_info[j]->frame_width);

            /* unsigned int(32) frame_height_minus1 */
            avio_wb32(pb, apvc->configuration_entry[i].frame_info[j]->frame_height);

            /* unsigned int(4) chroma_format_idc */
            /* unsigned int(4) bit_depth_minus8 */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->chroma_format_idc << 4 | apvc->configuration_entry[i].frame_info[j]->bit_depth_minus8);

            /* unsigned int(8) capture_time_distance */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->capture_time_distance);

            if(apvc->configuration_entry[i].frame_info[j]->color_description_present_flag) {
                /* unsigned int(8) color_primaries */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->color_primaries);
                
                /* unsigned int(8) transfer_characteristics */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->transfer_characteristics);

                /* unsigned int(8) matrix_coefficients */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->matrix_coefficients);

                /* unsigned int(1) full_range_flag */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j]->full_range_flag << 7 |
                            apvc->configuration_entry[i].frame_info[j]->reserved_zero_7bits);
            }
        }
    }

    return 0;
}

int ff_isom_write_apvc(AVIOContext *pb, const uint8_t *data,
                       int size, int ps_array_completeness)
{
    APVDecoderConfigurationRecord *apvc = (APVDecoderConfigurationRecord *)data;
    int ret = 0;
    
    if (size < 8) {
        /* We can't write a valid apvC from the provided data */
        return AVERROR_INVALIDDATA;
    }

    if(size!=sizeof(APVDecoderConfigurationRecord)) return -1;
    ret = apvc_write(pb, apvc);

    apvc_close(apvc);
    return ret;
}

static int apv_add_frameinfo(APVDecoderConfigurationEntry *configuration_entry, APVDecoderFrameInfo *frame_info) {
    APVDecoderFrameInfo **temp = NULL;
    if(configuration_entry->number_of_frame_info == 0) {
        temp = (APVDecoderFrameInfo **)malloc(sizeof(APVDecoderFrameInfo*));
        if (temp == NULL) {
            return AVERROR_INVALIDDATA;
        }
    } else {
        temp = (APVDecoderFrameInfo **)realloc(configuration_entry->frame_info, (configuration_entry->number_of_frame_info + 1) * sizeof(APVDecoderFrameInfo*));
        if (temp == NULL) {
            return AVERROR_INVALIDDATA;
        }
    }
    
    temp[configuration_entry->number_of_frame_info] = (APVDecoderFrameInfo*)malloc(sizeof(APVDecoderFrameInfo));
    memcpy(temp[configuration_entry->number_of_frame_info], frame_info, sizeof(APVDecoderFrameInfo));

    configuration_entry->frame_info = temp;

    configuration_entry->number_of_frame_info++;

    return 0;
}

static bool apv_cmp_frameinfo(const APVDecoderFrameInfo *a, const APVDecoderFrameInfo *b) {
    if (a->reserved_zero_6bits != b->reserved_zero_6bits) return false;
    if (a->color_description_present_flag != b->color_description_present_flag) return false;
    if (a->capture_time_distance_ignored != b->capture_time_distance_ignored) return false;
    if (a->profile_idc != b->profile_idc) return false;
    if (a->level_idc != b->level_idc) return false;
    if (a->band_idc != b->band_idc) return false;
    if (a->frame_width != b->frame_width) return false;
    if (a->frame_height != b->frame_height) return false;
    if (a->chroma_format_idc != b->chroma_format_idc) return false;
    if (a->bit_depth_minus8 != b->bit_depth_minus8) return false;
    if (a->capture_time_distance != b->capture_time_distance) return false;
    if (a->color_primaries != b->color_primaries) return false;
    if (a->transfer_characteristics != b->transfer_characteristics) return false;
    if (a->matrix_coefficients != b->matrix_coefficients) return false;
    if (a->full_range_flag != b->full_range_flag) return false;
    if (a->reserved_zero_7bits != b->reserved_zero_7bits) return false;

    return true;
}

static int apv_set_frameinfo(APVDecoderFrameInfo* frame_info, const pbu_t *pbu) {
    frame_info->reserved_zero_6bits = 0;

    frame_info->color_description_present_flag = pbu->frame.frame_header.color_description_present_flag;
    frame_info->capture_time_distance_ignored = 1;

    frame_info->profile_idc = pbu->frame.frame_header.frame_info.profile_idc;
    frame_info->level_idc = pbu->frame.frame_header.frame_info.level_idc;
    frame_info->band_idc = pbu->frame.frame_header.frame_info.band_idc;

    frame_info->frame_width = pbu->frame.frame_header.frame_info.frame_width;
    frame_info->frame_height = pbu->frame.frame_header.frame_info.frame_height;

    frame_info->chroma_format_idc = pbu->frame.frame_header.frame_info.chroma_format_idc;
    frame_info->bit_depth_minus8 = pbu->frame.frame_header.frame_info.bit_depth_minus8;

    frame_info->capture_time_distance = pbu->frame.frame_header.frame_info.capture_time_distance;


    if(frame_info->color_description_present_flag) {
        frame_info->color_primaries = pbu->frame.frame_header.color_primaries;
        frame_info->transfer_characteristics = pbu->frame.frame_header.transfer_characteristics;
        frame_info->matrix_coefficients = pbu->frame.frame_header.matrix_coefficients;

        frame_info->full_range_flag = pbu->frame.frame_header.full_range_flag;
        frame_info->reserved_zero_7bits = 0;
    }

    return 0;
}

int ff_isom_create_apv_dconf_record(uint8_t **data, int *size) {
    *size = sizeof(APVDecoderConfigurationRecord);
    *data = (uint8_t*)av_malloc(sizeof(APVDecoderConfigurationRecord));
    if(*data==NULL) {
        *size = 0;
         return AVERROR_INVALIDDATA;
    }
    apvc_init((APVDecoderConfigurationRecord*)*data);
    return 0;
}

void ff_isom_free_apv_dconf_record(uint8_t **data) {
    if (data != NULL && *data != NULL) {
        APVDecoderConfigurationRecord* apvc = (APVDecoderConfigurationRecord*)*data;
        apvc_close(apvc);

        free(*data);
        *data = NULL;
    }
}

int ff_isom_fill_apv_dconf_record(const uint8_t *apvdcr, const uint8_t *data, int size) {

    uint32_t au_size = 0;
    uint32_t number_of_pbu_entry = 0;

    uint32_t frame_type = -1;
    APVDecoderFrameInfo frame_info;

    int bytes_to_read = size;
    int ret = 0;

    APVDecoderConfigurationRecord* apvc = (APVDecoderConfigurationRecord*)apvdcr;
    if (size < 8) {
        /* We can't write a valid apvC from the provided data */
        return AVERROR_INVALIDDATA;
    }

    if (bytes_to_read > APV_AU_SIZE_PREFIX_LENGTH) {
        au_size = apv_read_au_size(data, APV_AU_SIZE_PREFIX_LENGTH);
        if (au_size == 0) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        data += APV_AU_SIZE_PREFIX_LENGTH;
        bytes_to_read -= APV_AU_SIZE_PREFIX_LENGTH;

        if (bytes_to_read < au_size) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        data += APV_SIGNATURE_LENGTH;
        bytes_to_read -= APV_SIGNATURE_LENGTH;

        // pbu (primitive bitstream units number)
        //
        number_of_pbu_entry = apv_number_of_pbu_entry(data, bytes_to_read);
        if (number_of_pbu_entry <= 0) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        for(int i=0;i<number_of_pbu_entry;i++) {

            pbu_t pbu;
            uint32_t pbu_size = apv_read_pbu_size(data, APV_PBU_SIZE_PREFIX_LENGTH);

            data += APV_AU_SIZE_PREFIX_LENGTH;

            if(!apv_read_pbu(data, pbu_size, &pbu)) {

                switch (pbu.pbu_header.pbu_type)
                {
                case APV_PBU_TYPE_PRIMARY_FRAME:
                    frame_type = APV_FRAME_TYPE_PRIMARY_FRAME;
                    break;
                case APV_PBU_TYPE_NON_PRIMARY_FRAME:
                    frame_type = APV_FRAME_TYPE_NON_PRIMARY_FRAME;
                    break;
                case APV_PBU_TYPE_PREVIEW_FRAME:
                    frame_type = APV_FRAME_TYPE_PREVIEW_FRAME;
                    break;
                case APV_PBU_TYPE_DEPTH_FRAME:
                    frame_type = APV_FRAME_TYPE_DEPTH_FRAME;
                    break;
                case APV_PBU_TYPE_ALPHA_FRAME:
                    frame_type = APV_FRAME_TYPE_ALPHA_FRAME;
                    break;
                default:
                    frame_type = APV_FRAME_TYPE_NON_FRAME;
                    break;
                };

                if(frame_type == APV_FRAME_TYPE_NON_FRAME) continue;

                apv_set_frameinfo(&frame_info, &pbu);

                if(apvc->configuration_entry[frame_type].number_of_frame_info == 0) {
                    apv_add_frameinfo(&apvc->configuration_entry[frame_type], &frame_info);
                    apvc->number_of_configuration_entry++;
                } else {
                    for(i=0; i<apvc->configuration_entry[frame_type].number_of_frame_info;i++) {
                        if(!apv_cmp_frameinfo(apvc->configuration_entry[frame_type].frame_info[i], &frame_info)) {
                            apv_add_frameinfo(&apvc->configuration_entry[i], &frame_info);
                            break;
                        }
                    }
                }   
            }
            data += pbu_size;
        }
    }

end:    
    return ret;
}
