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

// @notice Updated 10.12.2024
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

} APVDecoderFrameInfo;

// @notice Updated 10.12.2024
typedef struct APVDecoderConfigurationEntry {
    uint8_t pbu_type;                   // 8 bits
    uint8_t number_of_frame_info;       // 8 bits

    APVDecoderFrameInfo* frame_info;    // table of size number_of_frame_info

} APVDecoderConfigurationEntry;

// @notice Updated 10.12.2024
typedef struct APVDecoderConfigurationBox {
    uint8_t configurationVersion;           // 8 bits
    uint8_t number_of_configuration_entry;  // 8 bits

    APVDecoderConfigurationEntry *configuration_entry; // table of size number_of_configuration_entry

} APVDecoderConfigurationBox;

//  5.3.6. Frame information 
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

//  5.3.7. Quantization matrix 
typedef struct quantization_matrix_t {
    uint8_t q_matrix[3][8][8]; // @todo use NumCmp instead of 3
} quantization_matrix_t;

//  5.3.8. Tile info 
typedef struct tile_info_t {
    uint32_t tile_width_in_mbs;     // u(20)
    uint32_t tile_height_in_mbs;    // u(20)
    uint8_t tile_size_present_in_fh_flag; // u(1)
    uint32_t tile_size_in_fh[TILE_ROWS_MAX*TILE_COLS_MAX]; // u(32) -> size of table: NumTiles

    // private
    uint32_t NumTiles;
    
    // x0 = ColStarts[tileIdx % TileCols]
    uint32_t ColStarts[TILE_COLS_MAX];

    // The value of TileCols MUST be less than or equal to 20.
    // The value of TileRows MUST be less than or equal to 20.
    // NumTiles = TileCols * TileRows  
    // tileIdx <0; NumTiles)
    //
    // y0 = RowStarts[tileIdx // TileCols]  
    uint32_t RowStarts[TILE_ROWS_MAX];

    uint32_t TileCols;

} tile_info_t;

// 5.3.17. Byte alignment 
typedef struct byte_alignment_t {
    uint8_t alignment_bit_equal_to_zero; /* equal to 0*/ // f(1)
} byte_alignment_t;

// 5.3.5. Frame header 
typedef struct frame_header_t {
    frame_info_t frame_info;
    uint8_t reserved_zero_8bits;                // 8 bits
    uint8_t color_description_present_flag;     // 1 bits

    uint8_t color_primaries;                    // 8 bits
    uint8_t transfer_characteristics;           // 8 bits
    uint8_t matrix_coefficients;                // 8 bits

    uint8_t use_q_matrix;                       // 1 bit

    quantization_matrix_t quantization_matrix;
    tile_info_t tile_info;

    uint8_t reserved_zero_8bits_2;              // 8 bits
    byte_alignment_t byte_alignment_t;
} frame_header_t;

//  5.3.11. Filler 
typedef struct filler_t {
    uint8_t *ff_byte; /* is a byte equal to 0xFF */ // f(8)
} filler_t;

typedef struct tile_header_t {
    uint16_t tile_header_size;                  // u(16)
    uint16_t tile_index;                        // u(16)
    uint32_t tile_data_size[NUM_COMP_MAX];      // u(32) table of size NumComp MAX = 4
    uint8_t tile_qp[NUM_COMP_MAX];              // u(8) table of size NumComp
    uint8_t reserved_zero_8bits;                // u(8)
    byte_alignment_t byte_alignment;

} tile_header_t;

typedef struct   {
    uint8_t coeff_zero_run;
    uint8_t abs_ac_coeff_minus1;
    uint8_t sign_ac_coeff;
} ac_coeff_coding_t;

//  5.3.14. Filler 
typedef struct macroblock_layer_t {
    uint8_t abs_dc_coeff_diff;  // h(v)
    uint8_t sign_dc_coeff_diff; // u(1)
    ac_coeff_coding_t ac_coeff_coding;
} macroblock_layer_t;

//  5.3.14. Filler 
typedef struct tile_data_t {
    macroblock_layer_t macroblock_layer;
    byte_alignment_t byte_alignment;
} tile_data_t;


//  5.3.11. Filler 
typedef struct tile_t {
    tile_header_t tile_header;
    uint8_t tile_data;
    uint8_t ff_byte; /* is a byte equal to 0xFF */ // f(8)
} tile_t;

// 5.3.4 Frame
// @see https://datatracker.ietf.org/doc/html/draft-lim-apv-03#name-frame
typedef struct frame_t {
    frame_header_t frame_header;
    uint32_t tile_size[NUM_TILES_MAX];           // table of NumTiles size
    tile_t tile[NUM_TILES_MAX];                   // table of NumTiles size
    filler_t filler; 
} frame_t;

typedef struct pbu_header_t {
    uint8_t pbu_type;               // 8 bits
    uint16_t group_id;              // 16 bits
    uint8_t reserved_zero_8bits;    // 8 bits
} pbu_header_t;

// 5.3.9. Access unit information 
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

//  5.3.10. Metadata 
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

typedef struct pbu_t {
    pbu_header_t pbu_header;
    frame_t frame;
    au_info_t au_info;
    metadata_t metadata;
    filler_t filler;
} pbu_t;

static int getNumComp(frame_info_t* frame_info) {
    int NumComp;
    switch(frame_info->chroma_format_idc) {
        case 0:
            NumComp = 1;
            break;
        case 2:
        case 3:
            NumComp = 3;
            break;
        case 4:
            NumComp = 4;
            break;
        default:
            NumComp = -1;
    };

    return NumComp;
}

static int getSubWithC(frame_info_t* frame_info) {
    int SubWithC;
    switch(frame_info->chroma_format_idc) {
        case 0:
        case 3:
        case 4:
            SubWithC = 1;
            break;
        case 2:
            SubWithC = 2;
            break;
        default:
            SubWithC = -1;
    };

    return SubWithC;
}

static int byte_aligned(GetBitContext *gb)
{
    int bits_count = get_bits_count(gb);
    if (bits_count % 8)
        return 1;
    else
        return 0;
}

static int byte_alignment(GetBitContext *gb){
    int alignment_bit_equal_to_zero;
    while(!byte_aligned(gb)) {
        alignment_bit_equal_to_zero = get_bits1(gb);
        if(alignment_bit_equal_to_zero!=0)
            return 1;
    }

    return 0;
}

static int getSubHeightC(frame_info_t* frame_info) {
    int SubHeightC;
    switch(frame_info->chroma_format_idc) {
        case 0:
        case 2:
        case 3:
        case 4:
            SubHeightC = 1;
            break;
        default:
            SubHeightC = -1;
    };

    return SubHeightC;
}

static int apv_read_pbu_header(GetBitContext *gb, pbu_header_t *pbu_header)
{
    pbu_header->pbu_type = get_bits(gb, 8);
    pbu_header->group_id = get_bits(gb, 16);
    pbu_header->reserved_zero_8bits = get_bits(gb, 8);

    return 0;
}

// @see https://datatracker.ietf.org/doc/html/draft-lim-apv-03#name-frame-information
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
    
    if (reserved_zero_5bits != 0xE0)
        return 1;  

    if (reserved_zero_8bits != 0x00)
        return 1;  

    return 0;
}

static int apv_read_quantization_matrix(GetBitContext *gb, frame_header_t *frame_header) // quantization_matrix_t *m) 
{
    int NumComp = getNumComp(&frame_header->frame_info); // APV_MAX_NUM_COMP; // !!!! Pobrac rzeczywisty rozmiar
    for (int cIdx=0; cIdx < NumComp; cIdx++) {
        for (int y=0; y<8; y++) {
            for(int x=0; x<8; x++) {
                frame_header->quantization_matrix.q_matrix[cIdx][x][y] = get_bits(gb, 8);
            }
        }
    }
   return 0;
}

// @see https://datatracker.ietf.org/doc/html/draft-lim-apv-03#name-tile-info
static int apv_read_tile_info(GetBitContext *gb, frame_info_t *frame_info, frame_header_t *frame_header) {
    uint32_t startMb = 0;
    uint32_t MbWidth = 16;
    uint32_t MbHeight = 16;

    uint32_t FrameWidthInSamplesY = frame_info->frame_width;
    uint32_t FrameHeightInSamplesY = frame_info->frame_height;
    

    uint32_t FrameWidthInMbsY = ceil(FrameWidthInSamplesY / MbWidth);
    uint32_t FrameHeightInMbsY = ceil(FrameHeightInSamplesY / MbHeight);
    
    uint32_t TileCols = 0; // The value of TileCols MUST be less than or equal to 20.
    uint32_t TileRows = 0; // The value of TileRows MUST be less than or equal to 20.
    uint32_t NumTiles = 0;

    tile_info_t* tile_info = &frame_header->tile_info;

    int i = 0;
    
    tile_info->tile_width_in_mbs = get_bits(gb, 20);

    if(tile_info->tile_width_in_mbs<16) {
        return -1;
    }

    tile_info->tile_height_in_mbs = get_bits(gb, 20);
    if(tile_info->tile_height_in_mbs<8) {
        return -1;
    }

    for(i = 0; startMb < FrameWidthInMbsY; i++) {
        tile_info->ColStarts[i] = startMb * MbWidth;
        startMb += tile_info->tile_width_in_mbs;
    }
    
    tile_info->ColStarts[i] = FrameWidthInMbsY*MbWidth;
    tile_info->TileCols = i;
    startMb = 0;

    for(i = 0; startMb < FrameHeightInMbsY; i++) {
        tile_info->RowStarts[i] = startMb * MbWidth;
        startMb += tile_info->tile_width_in_mbs;
    }
    tile_info->RowStarts[i] = FrameHeightInMbsY*MbHeight;
    TileRows = i;
    tile_info->TileCols = 

    NumTiles = TileCols * TileRows;
    tile_info->NumTiles = NumTiles;

    tile_info->tile_size_present_in_fh_flag = get_bits(gb, 1);
    
    if(tile_info->tile_size_present_in_fh_flag) {
        for(int tileIdx=0; tileIdx<NumTiles; tileIdx++) {
            tile_info->tile_size_in_fh[tileIdx] = get_bits(gb, 32);
        }
    }

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
    }

    frame_header->use_q_matrix = get_bits(gb, 1);
    if(frame_header->use_q_matrix) {
        apv_read_quantization_matrix(gb, frame_header);
    }

    apv_read_tile_info(gb, &frame_header->frame_info, frame_header);

    reserved_zero_8bits[1] = get_bits(gb, 8);
    if(reserved_zero_8bits[1]!=0) {
        return -1;
    }

    byte_alignment(gb); 
    
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
    au_info->num_frames = get_bits(gb, 16);

    for(int idx=0; idx<au_info->num_frames; idx++) {
        au_info->pbu_type = get_bits(gb, 8);
        au_info->group_id = get_bits(gb, 16);
        au_info->reserved_zero_8bits = get_bits(gb, 8);
        apv_read_frame_info(gb, &au_info->frame_info);
    }
    au_info->reserved_zero_8bits_2 = get_bits(gb, 8);
    byte_alignment(gb);

    return 0;
}

static int metadata_itu_t_t35(GetBitContext *gb, metadata_t *metadata, int payloadSize) {
    int readSize = payloadSize - 1;

    metadata->itu_t_t35_country_code = get_bits(gb, 8); // b(8)
    
    if(metadata->itu_t_t35_country_code == 0xFF) {
        metadata->itu_t_t35_country_code_extension = get_bits(gb, 8); //  b(8)
        readSize--;
    }

    while (readSize > 0) {
        metadata->itu_t_t35_payload = get_bits(gb, 8); // b(8)
        readSize--;
    }
    return 0;
}  

static int metadata_mdcv(GetBitContext *gb, metadata_t *metadata, int payloadSize) {
    for(int i = 0; i < 3; i++ ) {
        metadata->primary_chromaticity_x[i] = get_bits(gb, 16); // u(16)
        metadata->primary_chromaticity_y[i] = get_bits(gb, 16); // u(16)
    }
    metadata->white_point_chromaticity_x = get_bits(gb, 16); // u(16)
    metadata->white_point_chromaticity_y = get_bits(gb, 16); // u(16)
    metadata->max_mastering_luminance = get_bits(gb, 32);    // u(32)
    metadata->min_mastering_luminance = get_bits(gb, 32);    // u(32)

    return 0;
}  

static int metadata_cll(GetBitContext *gb, metadata_t *metadata, int payloadSize) {
    metadata->max_cll = get_bits(gb, 16);  //  u(16)
    metadata->max_fall = get_bits(gb, 16); //  u(16)
    
    return 0;
}  

static int metadata_filler(GetBitContext *gb, metadata_t *metadata, int payloadSize) {
    metadata->filler.ff_byte = malloc(payloadSize);
    for(int i = 0; i < payloadSize; i++){
        metadata->filler.ff_byte[i] = get_bits(gb, 8);    // 10.3.2. Filler metadata 
    }  
    return 0;
}  

static int metadata_user_defined(GetBitContext *gb, metadata_t *metadata, int payloadSize) {
    // uuid u(128)
    metadata->uuid.low = get_bits(gb, 64);
    metadata->uuid.high = get_bits(gb, 64);

    metadata->user_defined_data_payload = malloc((payloadSize - 16));

    for(int i = 0; i < (payloadSize - 16); i++) {
        metadata->user_defined_data_payload[i] = get_bits(gb, 8); // b(8)
    }
    return 0;
}  

static int metadata_undefined(GetBitContext *gb, metadata_t *metadata, int payloadSize) {
    metadata->undefined_metadata_payload_byte = malloc(payloadSize);

    for(int i = 0; i < payloadSize; i++) {
        metadata->undefined_metadata_payload_byte[i] = get_bits(gb, 8); // b(8)
    }
    
    return 0;
}  

static int apv_filler(GetBitContext *gb) {
    // uint8_t ff_byte;
    while(show_bits(gb, 8) == 0XFF) {
        // ff_byte = get_bits(gb, 8);
        skip_bits(gb, 8);
    }
    return 0;
}

static int apv_read_tile_header(GetBitContext *gb, tile_header_t *tile_header, frame_info_t *frame_info) {
    int NumComp = getNumComp(frame_info);
    if(NumComp == -1) return -1; 

    tile_header->tile_header_size = get_bits(gb, 16);   // u(16)
    tile_header->tile_index = get_bits(gb, 16);         // u(16)

    for(int i=0; i<NumComp; i++) {
        tile_header->tile_data_size[i] = get_bits(gb, 32);      // u(32)
    }

    for(int i=0; i<NumComp; i++) {
        tile_header->tile_qp[i] = get_bits(gb, 8);              // u(8)
    }

    tile_header->reserved_zero_8bits = get_bits(gb, 8);
    byte_alignment(gb);

    return 0;
}

static int ac_coeff_coding(GetBitContext *gb, int x0, int y0, int log2BlkWidth, int log2BlkHeight, int cIdx, int Prev1stAcLevel, int16_t (*TransCoeff)[8][8]) {
    int scanPos = 1;
    int firstAC = 1;
    int PrevLevel = Prev1stAcLevel;
    // int PrevRun = 0;

    // int ScanOrder[(1 << log2BlkWidth) * (1 << log2BlkHeight) - 1 + 1];   
    int abs_ac_coeff_minus1;
    int sign_ac_coeff;
    int level;
    int blkPos;
    int xC;
    int yC;

    do {
        int coeff_zero_run = get_ue_golomb(gb);     // h(v)
        for(int i = 0; i < coeff_zero_run; i++) {
            int blkPos = ScanOrder[scanPos];
            int xC = blkPos & ((1 << log2BlkWidth) - 1);
            int yC = blkPos >> log2BlkWidth;
            TransCoeff[cIdx][x0+xC][y0 + yC] = 0;
            scanPos++;
        }
        
        // PrevRun = coeff_zero_run;
        
        if(scanPos < (1 << (log2BlkWidth + log2BlkHeight))) {;
            abs_ac_coeff_minus1 = get_ue_golomb(gb);        //  h(v)
            sign_ac_coeff = get_bits1(gb);                  // u(1)
            level = (abs_ac_coeff_minus1 + 1) * (1 - 2 * sign_ac_coeff);
            blkPos = ScanOrder[scanPos];
            xC = blkPos & ((1 << log2BlkWidth) - 1);
            yC = blkPos >> log2BlkWidth;
            
            TransCoeff[cIdx][x0 + xC][y0 + yC] = level;
            
            scanPos++;
            PrevLevel = abs_ac_coeff_minus1 + 1;
            if(firstAC == 1){
                firstAC = 0;
                Prev1stAcLevel = PrevLevel;
            }
        }
    } while(scanPos < (1 << (log2BlkWidth + log2BlkHeight)));
    return 0;
}

// cIdx - [1,3,4] - NumComp
static int macroblock_layer(GetBitContext *gb, int xMb, int yMb, int cIdx, frame_info_t* frame_info, int PrevDC, int PrevDcDiff, int Prev1stAcLevel) {

    int MbWidth = MB_WIDTH;
    int MbHeight = MB_HEIGHT;

    int SubWidthC = getSubWithC(frame_info); // [1,2]
    int SubHeightC = getSubHeightC(frame_info); // [1]

    int MbWidthC = MbWidth/SubWidthC;
    int MbHeightC = MbHeight/SubHeightC;

    int subW = (cIdx == 0)? 1 : SubWidthC;
    int subH = (cIdx == 0)? 1 : SubHeightC;
    int blkWidth = (cIdx == 0)? MbWidth : MbWidthC;
    int blkHeight = (cIdx == 0)? MbHeight : MbHeightC;
    int TrSize = 8;

    uint8_t sign_dc_coeff_diff;

    // Sizes based on 5.3.7 from draft-lim-apv-02.html
    int16_t TransCoeff[4][8][8];

    for(int y = 0; y < blkHeight; y+=TrSize) {
        for(int x = 0; x < blkWidth; x+=TrSize) {
            uint32_t abs_dc_coeff_diff = get_ue_golomb_long(gb); // h(v)
            if(abs_dc_coeff_diff)
                sign_dc_coeff_diff = get_bits1(gb); // u(1)
            TransCoeff[cIdx][xMb/subW + x][yMb/subH + y] = PrevDC + abs_dc_coeff_diff * (1 - 2*sign_dc_coeff_diff); // TransCoeff[cIdx][xMb // subW + x][yMb // subH + y]
            PrevDC = TransCoeff[cIdx][xMb/subW + x][yMb/subH + y];
            PrevDcDiff = abs_dc_coeff_diff;
            ac_coeff_coding(gb, xMb/subW + x, yMb/subH + y, log2(TrSize), log2(TrSize), cIdx, Prev1stAcLevel, TransCoeff); // ac_coeff_coding(xMb // subW + x, yMb // subH + y, log2(TrSize), log2(TrSize), cIdx)
        }
    }
    return 0;
}

static int apv_read_tile_data(GetBitContext *gb, int tileIdx, int cIdx, frame_header_t* frame_header) {
    int x0, y0;
    int TileCols;

    int numMbColsInTile;
    int numMbRowsInTile;
    int numMbsInTile;
    int PrevDC;
    int PrevDcDiff;
    int Prev1stAcLevel;

    int MbWidth = APV_MB_WIDTH;
    int MbHeight = APV_MB_HEIGHT;

    tile_info_t *tile_info = &frame_header->tile_info;
    frame_info_t *frame_info = &frame_header->frame_info;
    
    TileCols = tile_info->TileCols;

    x0 = tile_info->ColStarts[tileIdx % TileCols];
    y0 = tile_info->RowStarts[tileIdx / TileCols]; // @todo check it   y0 = RowStarts[tileIdx // TileCols]  // : an integer division with rounding of the result toward zero. For example, 7//4 and -7//-4 are rounded to 1 and -7//4 and 7//-4 are rounded to -1

    numMbColsInTile = (tile_info->ColStarts[tileIdx % TileCols + 1] -  tile_info->ColStarts[tileIdx % TileCols]) / MbWidth;
    numMbRowsInTile = (tile_info->RowStarts[tileIdx / TileCols + 1] -  tile_info->RowStarts[tileIdx / TileCols]) / MbHeight; // tileIdx // TileCols
    numMbsInTile = numMbColsInTile * numMbRowsInTile;
    PrevDC = 0;
    PrevDcDiff = 20;
    Prev1stAcLevel = 0;
    
    for(int i = 0; i < numMbsInTile; i++){
        int xMb = x0 + ((i % numMbColsInTile) * MbWidth);
        int yMb = y0 + ((i / numMbColsInTile) * MbHeight); // @notice i // numMbColsInTile
        macroblock_layer(gb, xMb, yMb, cIdx, frame_info, PrevDC, PrevDcDiff, Prev1stAcLevel);
    }
    byte_alignment(gb);

    return 0;
}

static int more_data_in_tile(GetBitContext *gb, tile_t *tile, int tileIdx) {
    return 0;
}

static int apv_read_tile(GetBitContext *gb, tile_t *tile, frame_header_t *frame_header) {
    
    int NumComp;
    int tileIdx = tile->tile_header.tile_index;
    frame_info_t* frame_info = &frame_header->frame_info;

    apv_read_tile_header(gb, &tile->tile_header, frame_info);
    NumComp = getNumComp(frame_info);
    if(NumComp == -1) return -1; 

    for(int i=0; i<NumComp;i++) {
        apv_read_tile_data(gb, tileIdx, i, frame_header);
    }

    while(more_data_in_tile(gb, tile, tileIdx)) {
        skip_bits(gb, 8);
    }

    return 0;
}

static int apv_read_frame(GetBitContext *gb, frame_t *frame)
{
    uint32_t NumTiles;
    
    apv_read_frame_header(gb, &frame->frame_header);
    NumTiles =  frame->frame_header.tile_info.NumTiles;
    for (int tileIdx = 0; tileIdx <NumTiles; tileIdx++) {
        frame->tile_size[tileIdx] = get_bits(gb, 32);
        apv_read_tile(gb, &frame->tile[tileIdx], &frame->frame_header);
       
    }
    apv_filler(gb);
    return 0;
}

static int metadata_payload(GetBitContext *gb, metadata_t *metadata,  int payloadType, int payloadSize) {
  if(payloadType == 4){
    metadata_itu_t_t35(gb, metadata, payloadSize);
  }
  else if(payloadType == 5){
    metadata_mdcv(gb, metadata, payloadSize);
  }
  else if(payloadType == 6){
    metadata_cll(gb, metadata, payloadSize);
  }
  else if(payloadType == 10){
    metadata_filler(gb, metadata, payloadSize);
  }
  else if(payloadType == 170){
    metadata_user_defined(gb, metadata, payloadSize);
  }
  else{                            
    metadata_undefined(gb, metadata, payloadSize);
  }

  byte_alignment(gb);

  return 0;
}    

static int apv_read_metadata(GetBitContext *gb, metadata_t *metadata)
{
    int currReadSize = 0;

    metadata->metadata_size = get_bits(gb, 32);
    
    do {
        int payloadType = 0;
        int payloadSize = 0;
        uint8_t metadata_payload_type;
        uint8_t metadata_payload_size;

        while (show_bits(gb, 8) == 0xFF)
        {
            int8_t ff_byte = get_bits(gb, 8); // check it  f(8)
            payloadType += ff_byte;
            currReadSize++; 
        }
        metadata_payload_type = get_bits(gb, 8);
        payloadType += metadata_payload_type;
        currReadSize++;

        while (show_bits(gb, 8) == 0xFF)
        {
            int8_t ff_byte = get_bits(gb, 8); // check it  f(8)
            payloadSize += ff_byte;
            currReadSize++; 
        }
        metadata_payload_size = get_bits(gb, 8);
        payloadType += metadata_payload_size;
        currReadSize++;

        metadata_payload(gb, metadata, payloadType, payloadSize);
        currReadSize += payloadSize;
    } while(metadata->metadata_size>currReadSize);

    apv_filler(gb);    
    
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
        apv_read_frame(&gb, &pbu->frame);
    } else if(pbu->pbu_header.pbu_type ==65) {
        apv_read_au_info(&gb, &pbu->au_info);
    } else if(pbu->pbu_header.pbu_type ==66) {
        apv_read_metadata(&gb, &pbu->metadata);
    } else if(pbu->pbu_header.pbu_type ==67) {
        apv_filler(&gb);
    }

    return 0;
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

static void apvc_init(APVDecoderConfigurationBox* apvc)
{
    memset(apvc, 0, sizeof(APVDecoderConfigurationBox));
    apvc->configurationVersion = 1;
}

static void apvc_close(APVDecoderConfigurationBox* apvc)
{
    for(int i=0;i<apvc->number_of_configuration_entry;i++) {

        free(apvc->configuration_entry[i].frame_info);
        apvc->configuration_entry[i].frame_info = NULL;
    }

    free(apvc->configuration_entry);
    apvc->configuration_entry = NULL;
}

static int apvc_write(AVIOContext *pb, APVDecoderConfigurationBox* apvc)
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
            apvc->configuration_entry[i].frame_info[j].color_description_present_flag);

            av_log(NULL, AV_LOG_TRACE, "capture_time_distance_ignored:          %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j].capture_time_distance_ignored);

            av_log(NULL, AV_LOG_TRACE, "profile_idc:                            %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j].profile_idc);

            av_log(NULL, AV_LOG_TRACE, "level_idc:                              %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j].level_idc);

            av_log(NULL, AV_LOG_TRACE, "band_idc:                               %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j].band_idc);

            av_log(NULL, AV_LOG_TRACE, "frame_width:                            %"PRIu32"\n", 
            apvc->configuration_entry[i].frame_info[j].frame_width);

            av_log(NULL, AV_LOG_TRACE, "frame_height:                           %"PRIu32"\n", 
            apvc->configuration_entry[i].frame_info[j].frame_height);

            av_log(NULL, AV_LOG_TRACE, "chroma_format_idc:                      %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j].chroma_format_idc);

            av_log(NULL, AV_LOG_TRACE, "bit_depth_minus8:                       %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j].bit_depth_minus8);

            av_log(NULL, AV_LOG_TRACE, "capture_time_distance:                  %"PRIu8"\n", 
            apvc->configuration_entry[i].frame_info[j].capture_time_distance);

            if(apvc->configuration_entry[i].frame_info[j].color_description_present_flag) {
          
                av_log(NULL, AV_LOG_TRACE, "color_primaries:                    %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j].color_primaries);
                
                av_log(NULL, AV_LOG_TRACE, "transfer_characteristics:           %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j].transfer_characteristics);
                
                av_log(NULL, AV_LOG_TRACE, "matrix_coefficients:                %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j].matrix_coefficients);

                av_log(NULL, AV_LOG_TRACE, "full_range_flag:                    %"PRIu8"\n", 
                apvc->configuration_entry[i].frame_info[j].full_range_flag);
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
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j].reserved_zero_6bits << 2 |
                        apvc->configuration_entry[i].frame_info[j].color_description_present_flag << 1 | 
                        apvc->configuration_entry[i].frame_info[j].capture_time_distance_ignored);
            
            /* unsigned int(8) profile_idc */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j].profile_idc);

            /* unsigned int(8) level_idc */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j].level_idc);

            /* unsigned int(8) band_idc */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j].band_idc);
            
            /* unsigned int(32) frame_width_minus1 */
            avio_wb32(pb, apvc->configuration_entry[i].frame_info[j].frame_width);

            /* unsigned int(32) frame_height_minus1 */
            avio_wb32(pb, apvc->configuration_entry[i].frame_info[j].frame_height);

            /* unsigned int(4) chroma_format_idc */
            /* unsigned int(4) bit_depth_minus8 */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j].chroma_format_idc << 4 | apvc->configuration_entry[i].frame_info[j].bit_depth_minus8);

            /* unsigned int(8) capture_time_distance */
            avio_w8(pb, apvc->configuration_entry[i].frame_info[j].capture_time_distance);

            if(apvc->configuration_entry[i].frame_info[j].color_description_present_flag) {
                /* unsigned int(8) color_primaries */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j].color_primaries);
                
                /* unsigned int(8) transfer_characteristics */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j].transfer_characteristics);

                /* unsigned int(8) matrix_coefficients */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j].matrix_coefficients);

                /* unsigned int(1) full_range_flag */
                avio_w8(pb, apvc->configuration_entry[i].frame_info[j].full_range_flag);
            }
        }
    }

    return 0;
}

int ff_isom_write_apvc(AVIOContext *pb, const uint8_t *data,
                       int size, int ps_array_completeness)
{
    APVDecoderConfigurationBox apvc;
    uint32_t au_size = 0;
    uint32_t number_of_configuration_entry = 0;

    // @todo Figure out where to get number_of_frame_info from.
    //       The documentation https://github.com/openapv/openapv/blob/main/readme/apv_isobmff.md 
    //       does not explain this in a clear and understandable way.
    //
    uint32_t number_of_frame_info = 1;

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

    // @deprecated
    // @todo reimplemntation is needed
    if (bytes_to_read > APV_AU_SIZE_PREFIX_LENGTH) {
        au_size = apv_read_au_size(data, APV_AU_SIZE_PREFIX_LENGTH);
        if (au_size == 0) goto end;

        data += APV_AU_SIZE_PREFIX_LENGTH;
        bytes_to_read -= APV_AU_SIZE_PREFIX_LENGTH;

        if (bytes_to_read < au_size) goto end;

        // pbu (primitive bitstream units number)
        //
        // @todo We assumed that number_of_configuration_entry is the number of PBUs in the AU. 
        //       I'm not sure if this assumption is correct.
        //       This needs to be figured out.
        number_of_configuration_entry = apv_number_of_pbu_entry(data, au_size);
        if (number_of_configuration_entry <= 0) goto end;

        apvc.number_of_configuration_entry = number_of_configuration_entry;

        apvc.configuration_entry = malloc(sizeof(APVDecoderConfigurationEntry)*number_of_configuration_entry);

        for(int i=0;i<number_of_configuration_entry;i++) {

            pbu_t pbu;
            uint32_t pbu_size = apv_read_pbu_size(data, APV_PBU_SIZE_PREFIX_LENGTH);

            data += APV_AU_SIZE_PREFIX_LENGTH;
            
            apv_read_pbu(data, pbu_size, &pbu);

            // fill APVDecoderConfigurationBox
            apvc.configuration_entry[i].pbu_type = pbu.pbu_header.pbu_type;
            apvc.configuration_entry[i].number_of_frame_info = number_of_frame_info;

            apvc.configuration_entry[i].frame_info = malloc(sizeof(APVDecoderFrameInfo)*number_of_frame_info);

            for (int j=0;j<number_of_frame_info;j++) {
                apvc.configuration_entry[i].frame_info[j].reserved_zero_6bits = 0;

                // flags
                apvc.configuration_entry[i].frame_info[j].color_description_present_flag = pbu.frame.frame_header.color_description_present_flag;
                apvc.configuration_entry[i].frame_info[j].capture_time_distance_ignored = 1;

                apvc.configuration_entry[i].frame_info[j].profile_idc = pbu.frame.frame_header.frame_info.profile_idc;
                apvc.configuration_entry[i].frame_info[j].level_idc = pbu.frame.frame_header.frame_info.level_idc;
                apvc.configuration_entry[i].frame_info[j].band_idc = pbu.frame.frame_header.frame_info.band_idc;

                apvc.configuration_entry[i].frame_info[j].frame_width = pbu.frame.frame_header.frame_info.frame_width;
                apvc.configuration_entry[i].frame_info[j].frame_height = pbu.frame.frame_header.frame_info.frame_height;

                apvc.configuration_entry[i].frame_info[j].chroma_format_idc = pbu.frame.frame_header.frame_info.chroma_format_idc;
                apvc.configuration_entry[i].frame_info[j].bit_depth_minus8 = pbu.frame.frame_header.frame_info.bit_depth_minus8;

                apvc.configuration_entry[i].frame_info[j].capture_time_distance = pbu.frame.frame_header.frame_info.capture_time_distance;


                if(apvc.configuration_entry[i].frame_info[j].color_description_present_flag) {
                    apvc.configuration_entry[i].frame_info[j].color_primaries = pbu.frame.frame_header.color_primaries;
                    apvc.configuration_entry[i].frame_info[j].transfer_characteristics = pbu.frame.frame_header.transfer_characteristics;
                    apvc.configuration_entry[i].frame_info[j].matrix_coefficients = pbu.frame.frame_header.matrix_coefficients;

                    // @todo Figure out what value the field should have if number_of_frame_info is different from 1.
                    apvc.configuration_entry[i].frame_info[j].full_range_flag = (number_of_frame_info == 1) ? 1 : 0;
                }
            }
            data += pbu_size;
            // end pbu           
        }
    }

    ret = apvc_write(pb, &apvc);

end:
    apvc_close(&apvc);
    return ret;
}
