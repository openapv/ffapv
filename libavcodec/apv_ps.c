/*
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

#include "golomb.h"
#include "parser.h"
#include "apv.h"
#include "apv_ps.h"

#define ffapv_max(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b;       \
})

#define ffapv_min(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b;       \
})

// @see WD1_APV_spec section 5.8 Mathematical functions
#define ffapv_clip3(min, max, val) ffapv_max( (min), ffapv_min((max), (val)) )


// @see APV Technical Note 2.6 Entropy coding (Zig-zag scanning of an 8x8 transform block)
const uint16_t ScanOrder[APV_BLOCK_D] =
{
    0,    1,    8,   16,    9,    2,    3,   10,
    17,   24,   32,   25,   18,   11,    4,    5,
    12,   19,   26,   33,   40,   48,   41,   34,
    27,   20,   13,    6,    7,   14,   21,   28,
    35,   42,   49,   56,   57,   50,   43,   36,
    29,   22,   15,   23,   30,   37,   44,   51,
    58,   59,   52,   45,   38,   31,   39,   46,
    53,   60,   61,   54,   47,   55,   62,   63,
};

// @see WD1_APV_spec 7.3.1.3 Tile info syntax
int ff_apv_tile_info(GetBitContext *gb, APVParamSets *ps) {
    // @see WD1_APV_spec section 6.2 Source, decoded and output frame formats
    int MbWidth = APV_MB_WIDTH;
    int MbHeight = APV_MB_HEIGHT;

    int FrameWidthInSamplesY;
    int FrameHeightInSamplesY;
    int FrameWidthInMbsY;
    int FrameHeightInMbsY;

    int startMb = 0;
    int i = 0;

    ps->tile_info.tile_width_in_mbs_minus1 = get_bits(gb, 28);
    ps->tile_info.tile_height_in_mbs_minus1 = get_bits(gb, 28);

    FrameWidthInSamplesY = ps->frame_data_header.frame_width_minus1 + 1;
    FrameHeightInSamplesY = ps->frame_data_header.frame_height_minus1 + 1;
    FrameWidthInMbsY = ceil( FrameWidthInSamplesY / MbWidth );
    FrameHeightInMbsY = ceil( FrameHeightInSamplesY / MbHeight );

    if(!ps->tile_info.ColStarts)
        free(ps->tile_info.ColStarts);

    // remember to free it
    ps->tile_info.ColStarts = malloc(sizeof(int)*FrameWidthInMbsY);

    for( i = 0; startMb < FrameWidthInMbsY; i++ ) {
        ps->tile_info.ColStarts[ i ] = startMb * MbWidth;
        startMb += ps->tile_info.tile_width_in_mbs_minus1 + 1;
    }

    ps->tile_info.ColStarts[ i ] = FrameWidthInMbsY * MbWidth;
    ps->tile_info.TileCols = i;

    startMb = 0;

    if(!ps->tile_info.RowStarts)
        free(ps->tile_info.RowStarts);

    // remember to free it
    ps->tile_info.RowStarts = malloc(sizeof(int)*FrameHeightInMbsY);

    for( i = 0; startMb < FrameHeightInMbsY; i++ ) {
        ps->tile_info.RowStarts[ i ] = startMb * MbHeight;
        startMb += ps->tile_info.tile_height_in_mbs_minus1 + 1;
    }

    ps->tile_info.RowStarts[ i ] = FrameHeightInMbsY * MbHeight;
    ps->tile_info.TileRows = i;
    ps->NumTiles = ps->tile_info.TileCols * ps->tile_info.TileRows;

    if(!ps->tile_info.tile_size_minus1)
        free(ps->tile_info.tile_size_minus1);

    // remember to free it
    ps->tile_info.tile_size_minus1 = malloc(sizeof(uint32_t)*ps->NumTiles);

    for( i = 0; i < ps->NumTiles - 1; i++ ) {
        ps->tile_info.tile_size_minus1[i] = get_bits(gb, 24);
    }

    return 0;
}

// @see WD1_APV_spec 7.2 Specification of syntax functions and descriptors
// byte_aligned( ) is specified as follows:
// — If the current position in the bitstream is on a byte boundary, i.e., the next bit in the bitstream is the first bit in a byte, the return value of byte_aligned( ) is equal to TRUE.
// — Otherwise, the return value of byte_aligned( ) is equal to FALSE.
//
static int byte_aligned(GetBitContext *gb) {
    int bits_count = get_bits_count(gb);
    if (bits_count % 8) {
        return 1;
    } else {
        return 0;
    }
}

// @see WD1_APV_spec 7.3.3
int ff_apv_byte_alignment(GetBitContext *gb, APVParamSets *ps) {
    while(!byte_aligned(gb)) {
        ps->alignment_bit_equal_to_zero = get_bits1(gb);
    }
    return 0;
}

// @see WD1_APV_spec 7.3.1.2 Quantization matrix syntax
int ff_apv_quantization_matrix(GetBitContext *gb, APVParamSets *ps) {
    for( int cIdx = 0; cIdx < 3; cIdx ++ ) {
        for( int y = 0; y < 8; y ++ ) {
            for( int x = 0; x < 8; x ++ ) {
                ps->q_matrix_minus1[ cIdx ][ x ][ y ] = get_bits(gb, 8);
            }
        }
    }
    return 0;
}

// @see WD1_APV_spec 7.3.1.1 Frame header syntax
int ff_apv_parse_frame_header(GetBitContext *gb, APVParamSets *ps) {

    int ret = 0;
    // @todo parese frame header
    ps->frame_data_header.frame_header_size = get_bits(gb, 16);
    ps->frame_data_header.profile_idc = get_bits(gb, 8);
    ps->frame_data_header.level_idc = get_bits(gb, 8);
    ps->frame_data_header.reserved_zero_8bits = get_bits(gb, 8);
    ps->frame_data_header.frame_width_minus1 = get_bits(gb, 32);
    ps->frame_data_header.frame_height_minus1 = get_bits(gb, 32);
    ps->frame_data_header.chroma_format_idc = get_bits(gb, 2);
    ps->frame_data_header.bit_depth_minus8 = get_bits(gb, 4);
    ps->frame_data_header.capture_time_distance = get_bits(gb, 8);
    ps->frame_data_header.reserved_zero_16bits = get_bits(gb, 16);
    ps->frame_data_header.color_description_present_flag = get_bits(gb, 1);

    if(ps->frame_data_header.color_description_present_flag) {
        ps->frame_data_header.color_primaries = get_bits(gb, 8);
        ps->frame_data_header.transfer_characteristics = get_bits(gb, 8);
        ps->frame_data_header.matrix_coefficients = get_bits(gb, 8);
    }
    ps->frame_data_header.use_q_matrix = get_bits(gb, 1);
    if(ps->frame_data_header.use_q_matrix) {
        ret = ff_apv_quantization_matrix(gb, ps);
    }

    ff_apv_tile_info(gb, ps);
    ps->frame_data_header.reserved_zero_8bits_2 = get_bits(gb, 8);

    ff_apv_byte_alignment(gb, ps);

    return ret;
}

// @see WD1_APV_spec 9.1.4 Parsing process for variable length codes
static int read_vlc(GetBitContext *gb, uint32_t kParam) {

    uint32_t symbolValue = 0;
    uint8_t parseExpGolomb = 1;
    uint32_t k = kParam;
    uint32_t stopLoop = 0;

    if( get_bits(gb, 1) == 1 ) {
        parseExpGolomb = 0;
    } else {
        if( get_bits(gb, 1) == 0 ) {
            symbolValue += ( 1 << k );
            parseExpGolomb = 0;
        } else {
            symbolValue += ( 2 << k );
            parseExpGolomb = 1;
        }
    }

    if( parseExpGolomb ) {
        do {
            if( get_bits(gb, 1) == 1 ) {
                stopLoop = 1;
            } else {
                symbolValue += ( 1 << k );
                k++;
            }
        } while( !stopLoop );
    }

    if( k > 0 )
        symbolValue += get_bits(gb, k);

    return symbolValue;
}

// @see WD1_APV_spec 7.3.2.4 AC coefficient codining syntax
static void ac_coeff_coding( GetBitContext *gb, uint32_t Prev1stAcLevel, int32_t TransCoeff[APV_COLOR_COMP_NUM][APV_MB_WIDTH][APV_MB_HEIGHT], uint32_t x0, uint32_t y0, int log2BlkWidth, int log2BlkHeight, int cIdx ) {
    int scanPos = 1;
    int firstAC = 1;
    int PrevLevel = Prev1stAcLevel;
    int PrevRun = 0;

    do {
        int kParam = ffapv_clip3( 0, 2, PrevRun >> 2 );
        int coeff_zero_run = read_vlc(gb, kParam); // h(v)

        for( int i = 0; i < coeff_zero_run; i++ ) {
            int blkPos = ScanOrder[ scanPos ];
            int xC = blkPos & ( ( 1 << log2BlkWidth ) - 1 );
            int yC = blkPos >> log2BlkWidth;
            TransCoeff[ cIdx ][ x0 + xC ][ y0 + yC ] = 0;
            scanPos++;
        }
        PrevRun = coeff_zero_run;
        if( scanPos < ( 1<< ( log2BlkWidth + log2BlkHeight ) ) ) {
            int kParam = ffapv_clip3( 0, 4, PrevLevel >> 2 );
            int abs_ac_coeff_minus1 = read_vlc(gb, kParam);

            uint8_t sign_ac_coeff = get_bits1(gb);
            int level = ( abs_ac_coeff_minus1 + 1 ) * ( 1 - 2 * sign_ac_coeff );
            uint16_t blkPos = ScanOrder[ scanPos ];
            int xC = blkPos & ( ( 1 << log2BlkWidth ) - 1 );
            int yC = blkPos >> log2BlkWidth;
            TransCoeff[ cIdx ][ x0 + xC ][ y0 + yC ] = level;
            scanPos++;
            PrevLevel = abs_ac_coeff_minus1 + 1;
            if( firstAC == 1 ) {
                firstAC = 0;
                Prev1stAcLevel = PrevLevel;
            }
        }
    } while ( scanPos < ( 1<< ( log2BlkWidth + log2BlkHeight ) ) );
}

// @see WD1_APV_spec 7.3.2.3 Macroblock layer syntax
static int macroblock_layer( GetBitContext *gb, APVParamSets *ps, uint32_t Prev1stAcLevel,  uint32_t xMb, uint32_t yMb, uint32_t cIdx ) {

    uint32_t MbWidth = APV_MB_WIDTH;
    uint32_t MbHeight = APV_MB_HEIGHT;

    // @see 6.2 Source, decoded and output frame formats
    // @see Table 2 — SubWidthC and SubHeightC values derived from chroma_format_idc
    uint32_t SubWidthC = (ps->frame_data_header.chroma_format_idc==2)? 2: 1;
    uint32_t SubHeightC = 1;

    // @see 6.2 Source, decoded and output frame formats
    uint32_t MbWidthC = MbWidth / SubWidthC;
    uint32_t MbHeightC = MbHeight / SubHeightC;

    uint32_t subW = ( cIdx == 0 ) ? 1 : SubWidthC;
    uint32_t subH = ( cIdx == 0 ) ? 1 : SubHeightC;
    uint32_t blkWidth = ( cIdx == 0 ) ? MbWidth : MbWidthC;
    uint32_t blkHeight = ( cIdx == 0 ) ? MbHeight : MbHeightC;

    uint32_t TrSize = 8;

    // moved from tile_data
    uint32_t PrevDC = 0;
    uint32_t PrevDcDiff = APV_PREV_DC_DIFF;

    int32_t TransCoeff[APV_COLOR_COMP_NUM][APV_MB_WIDTH][APV_MB_HEIGHT];

    for( int y = 0; y < blkHeight; y += TrSize ) {
        for( int x = 0; x < blkWidth; x += TrSize ) {

            int kParam = ffapv_clip3( 0, 5, PrevDcDiff >> 2 );
            int abs_dc_coeff_diff = read_vlc(gb, kParam); //  h(v)
            int sign_dc_coeff_diff; // u(1)

            if( abs_dc_coeff_diff )
                sign_dc_coeff_diff = get_bits1(gb); // u(1)

            TransCoeff[ cIdx ][ xMb / subW + x ][ yMb / subH + y ] = PrevDC + abs_dc_coeff_diff * ( 1 - 2 * sign_dc_coeff_diff );
            PrevDC = TransCoeff[ cIdx ][ xMb / subW + x ][ yMb / subH + y ];
            PrevDcDiff = abs_dc_coeff_diff;
            ac_coeff_coding( gb, Prev1stAcLevel, TransCoeff, xMb / subW + x, yMb / subH + y, log2( TrSize ), log2( TrSize ), cIdx );
        }
    }
    return 0;
}

// @see WD1_APV_spec 7.3.2.1 Tile header synatx
static int tile_header(GetBitContext *gb, APVParamSets *ps) {
    ps->tile_header.tile_header_size = get_bits(gb, 16);
    ps->tile_header.tile_qp = get_bits(gb, 7);
    ps->tile_header.tile_cb_qp = get_bits(gb, 7);
    ps->tile_header.tile_cr_qp = get_bits(gb, 7);
    ps->tile_header.tile_data_size_y_minus1 = get_bits(gb, 24);
    ps->tile_header.tile_data_size_cb_minus1 = get_bits(gb, 24);
    ps->tile_header.tile_data_size_cr_minus1 = get_bits(gb, 24);

    ff_apv_byte_alignment(gb, ps);

    return 0;
}

// @see WD1_APV_spec 7.3.2.2 Tile data synatx
// @param cIdx specifying the color component of the current block,
static int tile_data(GetBitContext *gb, APVParamSets *ps, uint8_t tileIdx, uint8_t cIdx) {
    int MbWidth = APV_MB_WIDTH;
    int MbHeight = APV_MB_HEIGHT;

    uint32_t x0 = ps->tile_info.ColStarts[ tileIdx % ps->tile_info.TileCols ];
    uint32_t y0 = ps->tile_info.RowStarts[ tileIdx / ps->tile_info.TileCols ];

    uint32_t numMbColsInTile = ( ps->tile_info.ColStarts[ tileIdx % ps->tile_info.TileCols + 1 ]
                               - ps->tile_info.ColStarts[ tileIdx % ps->tile_info.TileCols ] ) / MbWidth;

    uint32_t numMbRowsInTile = ( ps->tile_info.RowStarts[ tileIdx / ps->tile_info.TileCols + 1 ]
                               - ps->tile_info.RowStarts[ tileIdx / ps->tile_info.TileCols] ) / MbHeight;

    uint32_t numMbsInTile = numMbColsInTile * numMbRowsInTile;

    uint32_t Prev1stAcLevel = 0;
    for( int i = 0; i < numMbsInTile; i++ ) {
        uint32_t xMb = x0 + ( i % numMbColsInTile ) * MbWidth;
        uint32_t yMb = y0 + ( i / numMbColsInTile ) * MbHeight;
        macroblock_layer( gb, ps, Prev1stAcLevel, xMb, yMb, cIdx );
    }
    ff_apv_byte_alignment(gb, ps);

    return 0;
}

// @see WD1_APV_spec 7.3.2 Tile synatx
int ff_apv_parse_tile(GetBitContext *gb, APVParamSets *ps, uint8_t tileIdx) {
    int ret = 0;
    tile_header(gb, ps);
    tile_data(gb, ps, tileIdx, 0);
    tile_data(gb, ps, tileIdx, 1);
    tile_data(gb, ps, tileIdx, 2);

    return ret;
}

void ff_apv_ps_free(APVParamSets *ps) {
}

// @see WD1_APV_spec Annex C C.1.1 Metadata synatx
// @todo provide implementation
static int metadata_payload( GetBitContext *gb, uint32_t metadataSize ) {
    do {
        uint16_t size;

        skip_bits(gb, 16);          // u(16)

        size = get_bits(gb, 16);    // u(16)
        
        skip_bits(gb, size*8);      // value u(size*8);

    } while (get_bits(gb, 16) != 0xFFFF);

    return 0;
}

// @see WD1_APV_spec 7.3.1.4 Metadata syntax
int ff_apv_parse_metadata(GetBitContext *gb, APVParamSets *ps) {
    uint16_t metadata_num = get_bits(gb, 16); // u(16)
    for(int i = 0; i < metadata_num; i++ ) {
        uint32_t metadata_size_minus1 = get_bits(gb, 32); // u(32)
        metadata_payload( gb, metadata_size_minus1 + 1 );
    }
    return 0;
}

// @see WD1_APV_spec 7.3.1.5 Filler data syntax
int ff_apv_parse_filler_data(GetBitContext *gb, APVParamSets *ps) {
    uint8_t ff_byte = 0;
    while(  get_bits(gb, 8) == 0xFF )
        ff_byte = 0xff; // equal to 0xFF

    return ff_byte;
}
