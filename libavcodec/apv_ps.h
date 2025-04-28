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

#ifndef AVCODEC_APV_PS_H
#define AVCODEC_APV_PS_H

#include <stdint.h>

#include "apv.h"
#include "get_bits.h"

/* size of block */
#define APV_LOG2_BLOCK                  (3)
#define APV_BLOCK                       (1<<APV_LOG2_BLOCK)
#define APV_BLOCK_W                     (1<<APV_LOG2_BLOCK)
#define APV_BLOCK_H                     (1<<APV_LOG2_BLOCK)
#define APV_BLOCK_D                     (APV_BLOCK_W * APV_BLOCK_H)

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-5.3.17
typedef struct APVByteAlignemnt {
    uint8_t alignment_bit_equal_to_zero; /* equal to 0*/ // f(1)
} APVByteAlignemnt;

// The sturcture reflects Tile Header layout
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-5.3.13

// The following descriptors specify the parsing process of each element
// u(n) - unsigned integer using n bits
// ue(v) - unsigned integer 0-th order Exp_Golomb-coded syntax element with the left bit first
typedef struct APVTileHeader {
    uint16_t tile_header_size;                          // u(16)
    uint16_t tile_index;                                // u(16)

    uint32_t tile_data_size[APV_COLOR_COMP_NUM];        // u(32)


    uint8_t tile_qp[APV_COLOR_COMP_NUM];                // u(8)
    uint8_t reserved_zero_8bits;                        // u(8)
    
    APVByteAlignemnt byte_alignent;

} APVTileHeader;

// The sturcture reflects Tile Info sturcture layout
// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-5.3.8
//
// The following descriptors specify the parsing process of each element
// u(n) - unsigned integer using n bits
// ue(v) - unsigned integer 0-th order Exp_Golomb-coded syntax element with the left bit first
typedef struct APVTileInfo {
    uint32_t tile_width_in_mbs;                     // u(20)
    uint32_t tile_height_in_mbs;                    // u(20)
    uint8_t  tile_size_present_in_fh_flag;          // u(1)
    uint32_t *tile_size_in_fh;                      // table of size NumTiles; elements of u(32) type

    uint32_t *ColStarts; // table of size FrameWidthInMbsY
    uint32_t *RowStarts; // table of size FrameHeightInMbsY
    uint32_t TileCols;
    uint32_t TileRows;
    uint32_t NumTiles;

} APVTileInfo;

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-quantization-matrix
typedef struct APVQuantizationMatrix {
    uint8_t q_matrix[3][8][8];
} APVQuantizationMatrix;

// @see  https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame-information
// 5.3.6. Frame information
typedef struct APVFrameInfo {
    uint8_t profile_idc;                             // u(8)
    uint8_t level_idc;                               // u(8)
    uint8_t band_idc;                                // u(3)
    uint8_t reserved_zero_5bits;                     // u(5)
    uint32_t frame_width;                            // u(24)
    uint32_t frame_height;                           // u(24)
    uint8_t chroma_format_idc;                       // u(4)
    uint8_t bit_depth_minus8;                        // u(4)
    uint8_t capture_time_distance;                   // u(8)
    uint16_t reserved_zero_8bits;                    // u(8)
} APVFrameInfo;

typedef struct APVFrameDataHeader {
    
    struct APVFrameInfo frame_info;
    uint8_t reserved_zero_8bits;                    // u(8)

    uint8_t color_description_present_flag;         // u(1)
    uint8_t color_primaries;                        // u(8)
    uint8_t transfer_characteristics;               // u(8)
    uint8_t matrix_coefficients;                    // u(8)

    uint8_t use_q_matrix;                           // u(1)

    APVQuantizationMatrix quantization_matrix;
    APVTileInfo tile_info;

    uint8_t reserved_zero_8bits_2;                  // u(8)

    APVByteAlignemnt byte_alignent;

} APVFrameDataHeader;

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-5.3.3
typedef struct APVPBUHeader {
    uint8_t pbu_type;                               // u(8)
    uint8_t group_id;                               // u(16)
    uint8_t reserved_zer_8bits;                     // u(8)
} APVPBUHeader;

typedef struct APVTileData {
    uint32_t PrevDc;
    uint32_t PrevDcDiff;
    uint32_t numMbColsInTile;
    uint32_t numMbRowsInTile;
    uint32_t numMbsInTile;
    uint32_t Prev1stAcLevel;

    APVByteAlignemnt byte_alignent;
} APVTileData;

typedef struct APVTile {
    APVTileHeader tile_header;
    APVTileData tile_data[3];
} APVTile;

typedef struct APVFrameData {
    APVFrameDataHeader frame_data_header;
    APVTile **tiles; // table of pointers to elements of type APVTile; the size of table is NumTiles
} APVFrameData;

typedef struct APVParamSets {
    APVFrameData frame_data;
} APVParamSets;

// https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-primitive-bitstream-unit-he
int ff_apv_parse_pbu_header(GetBitContext *gb, APVPBUHeader *fdh);

// https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame-header
int ff_apv_parse_frame_header(GetBitContext *gb, APVFrameDataHeader *fdh);

// https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame-information
int ff_apv_parse_frame_info(GetBitContext *gb, APVFrameInfo *frame_info);

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-frame-information
int ff_apv_tile_info(GetBitContext *gb, const APVFrameDataHeader *fdh, APVTileInfo *ti);

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-tile
int ff_apv_parse_tile(GetBitContext *gb, const APVFrameDataHeader *fdh, APVTile *tile, uint8_t tileIdx);

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-byte-alignment
int ff_apv_byte_alignment(GetBitContext *gb, APVByteAlignemnt *ba);

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-quantization-matrix
int ff_apv_quantization_matrix(GetBitContext *gb, int num_comp, APVQuantizationMatrix *qm);

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-metadata
int ff_apv_parse_metadata(GetBitContext *gb, APVFrameData *fd);

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#name-filler
int ff_apv_parse_filler_data(GetBitContext *gb, APVFrameData *fd);

void ff_apv_ps_free(APVParamSets *ps);

#endif /* AVCODEC_APV_PS_H */
