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

/**
 * @brief Specifies the decoder configuration information for APV video content.
 */
typedef struct APVDecoderConfigurationBox {
    uint8_t configurationVersion;                   // 6 bits
    uint8_t static_frame_header;                    // 1 bits
    uint8_t capture_time_distance_ignored;          // 1 bits
    uint16_t largest_frame_header_size;             // 16 bits
    uint8_t largest_profile_idc;                    // 8 bits
    uint8_t largest_level_idc;                      // 8 bits
    uint32_t largest_frame_width_minus1;            // 32 bits
    uint32_t largest_frame_height_minus1;           // 32 bits
    uint8_t largest_chroma_format_idc;              // 4 bits
    uint8_t largest_bit_depth_minus8;               // 4 bits
    uint8_t largest_capture_time_distance;          // 8 bits
   
    // if (static_frame_header)
    uint8_t reserved_zero_7bits;                    // 7 bits
    uint8_t frame_header_repeated;                  // 1 bits
    
    // if (!frame_header_repeated)
    uint8_t reserved_zero_6bits;                    // 6 bits
    uint8_t color_description_present_flag_info;    // 1 bits
    uint8_t use_q_matrix_info;                      // 1 bits
    
    // if (color_description_present_flag_info)
    uint8_t color_primaries_info;                   // 8 bits
    uint8_t transfer_characteristics_info;          // 8 bits
    uint8_t matrix_coefficients_info;               // 8 bits

    // if (use_q_matrix_info)
    uint8_t size_of_q_matrix_info;                  // 8 bits
    uint8_t *quantization_matrix_info;              // 8 bits *size_of_q_matrix_info

    uint8_t size_of_tile_info_info;                 // 8 bits
    uint8_t *tile_info_info;                        // (8*size_of_tile_info_info) bits
} APVDecoderConfigurationBox;

static int apvc_parse_frame_header(const uint8_t *bs, int bs_size, APVDecoderConfigurationBox *apvc)
{
    GetBitContext gb;
   
    int ret = init_get_bits8(&gb, bs, bs_size);
    if (ret < 0)
        return ret;

    // skip frame_header_size bits 
    skip_bits_long(&gb, 16);

    apvc->configurationVersion = get_bits(&gb, 6);
    apvc->static_frame_header = get_bits(&gb, 1);
    apvc->capture_time_distance_ignored = get_bits(&gb, 1);

    apvc->largest_frame_header_size = get_bits(&gb, 16);

    // @todo documentation lacks of information on allowable values
    apvc->largest_profile_idc = get_bits(&gb, 8);

    // @todo documentation lacks of information on allowable values
    apvc->largest_level_idc = get_bits(&gb, 8);

    apvc->largest_frame_width_minus1 = get_bits(&gb, 32);
    apvc->largest_frame_height_minus1 = get_bits(&gb, 32);

    // @todo documentation lacks of information on allowable values
    //
    // The reference application uses 4 bits for chroma_format_idc
    // while the documentation says that chroma_format_idc takes up 2 bits in the header
    // 0 - monochrome
    // 1 - 4:2:0
    // 2 - 4:2:2
    // 3 - 4:4:4
    apvc->largest_chroma_format_idc = get_bits(&gb, 4);
    if (apvc->largest_chroma_format_idc  < 2 || apvc->largest_chroma_format_idc > 3)
        return AVERROR_INVALIDDATA;

    apvc->largest_bit_depth_minus8 = get_bits(&gb, 4);
    if (apvc->largest_bit_depth_minus8  < 2 || apvc->largest_bit_depth_minus8 > 8)
        return AVERROR_INVALIDDATA;

    apvc->largest_capture_time_distance= get_bits(&gb, 8);
    
    if(apvc->static_frame_header) {
        
        apvc->reserved_zero_7bits = get_bits(&gb, 7);
        if (apvc->reserved_zero_7bits!=0) {
            return AVERROR_INVALIDDATA;
        }

        apvc->frame_header_repeated = get_bits(&gb, 1);

        if(!apvc->frame_header_repeated) {

            apvc->reserved_zero_6bits = get_bits(&gb, 6);
            if (apvc->reserved_zero_6bits!=0) {
                return AVERROR_INVALIDDATA;
            }

            apvc->color_description_present_flag_info = get_bits(&gb, 1);
            apvc->use_q_matrix_info = get_bits(&gb, 1);

            if(apvc->color_description_present_flag_info) {
                apvc->color_primaries_info = get_bits(&gb, 8);
                apvc->transfer_characteristics_info = get_bits(&gb, 8);
                apvc->matrix_coefficients_info = get_bits(&gb, 8);
            }

            if(apvc->use_q_matrix_info) {
                apvc->size_of_q_matrix_info =  get_bits(&gb, 8);
                apvc->quantization_matrix_info = av_malloc(apvc->size_of_q_matrix_info*8);
                for(int i=0;i<apvc->size_of_q_matrix_info;i++) {
                    apvc->quantization_matrix_info[i] = get_bits(&gb, 8);
                }

            }
            apvc->size_of_tile_info_info = get_bits(&gb, 8);
            apvc->tile_info_info = av_malloc(apvc->size_of_tile_info_info*8);
            for(int i=0;i<apvc->size_of_tile_info_info;i++) {
                    apvc->tile_info_info[i] = get_bits(&gb, 8);
             }
        }
    }

    return 0;
}

static void apvc_init(APVDecoderConfigurationBox *apvc)
{
    memset(apvc, 0, sizeof(APVDecoderConfigurationBox));
    apvc->configurationVersion = 1;
}

static void apvc_close(APVDecoderConfigurationBox *apvc)
{
    free(apvc->quantization_matrix_info);
    apvc->quantization_matrix_info = NULL;

    free(apvc->tile_info_info);
    apvc->tile_info_info = NULL;
}

static int apvc_write(AVIOContext *pb, APVDecoderConfigurationBox *apvc)
{
    av_log(NULL, AV_LOG_TRACE, "configurationVersion:                %"PRIu8"\n", 
    apvc->configurationVersion);
    
    av_log(NULL, AV_LOG_TRACE, "static_frame_header:                 %"PRIu8"\n", 
    apvc->static_frame_header);
    
    av_log(NULL, AV_LOG_TRACE, "capture_time_distance_ignored:       %"PRIu8"\n", 
    apvc->capture_time_distance_ignored);
    
    av_log(NULL, AV_LOG_TRACE, "largest_frame_header_size:           %"PRIu16"\n", 
    apvc->largest_frame_header_size);
    
    av_log(NULL, AV_LOG_TRACE, "largest_profile_idc:                 %"PRIu8"\n", 
    apvc->largest_profile_idc);
    
    av_log(NULL, AV_LOG_TRACE, "largest_level_idc:                   %"PRIu8"\n", 
    apvc->largest_level_idc);

    av_log(NULL, AV_LOG_TRACE, "largest_frame_width_minus1:          %"PRIu32"\n", 
    apvc->largest_frame_width_minus1);
    
    av_log(NULL, AV_LOG_TRACE, "largest_frame_height_minus1:         %"PRIu32"\n", 
    apvc->largest_frame_height_minus1);

    av_log(NULL, AV_LOG_TRACE, "largest_chroma_format_idc:           %"PRIu8"\n", 
    apvc->largest_chroma_format_idc);
    
    av_log(NULL, AV_LOG_TRACE, "largest_bit_depth_minus8:            %"PRIu8"\n", 
    apvc->largest_bit_depth_minus8);
    
    av_log(NULL, AV_LOG_TRACE, "largest_capture_time_distance:       %"PRIu8"\n", 
    apvc->largest_capture_time_distance);

    if (apvc->static_frame_header) {
        av_log(NULL, AV_LOG_TRACE, "frame_header_repeated:               %"PRIu8"\n", 
        apvc->frame_header_repeated);
        
        if (!apvc->frame_header_repeated) {
            av_log(NULL, AV_LOG_TRACE, "color_description_present_flag_info: %"PRIu8"\n",
            apvc->color_description_present_flag_info);
            
            av_log(NULL, AV_LOG_TRACE, "use_q_matrix_info:                   %"PRIu8"\n", 
            apvc->use_q_matrix_info);
            
            if (apvc->color_description_present_flag_info) {
                av_log(NULL, AV_LOG_TRACE, "color_primaries_info:                %"PRIu8"\n", 
                apvc->color_primaries_info);
                
                av_log(NULL, AV_LOG_TRACE, "transfer_characteristics_info:       %"PRIu8"\n", 
                apvc->transfer_characteristics_info);
                
                av_log(NULL, AV_LOG_TRACE, "matrix_coefficients_info:            %"PRIu8"\n", 
                apvc->matrix_coefficients_info);

            }
            if (apvc->use_q_matrix_info) {
                av_log(NULL, AV_LOG_TRACE, "size_of_q_matrix_info:               %"PRIu8"\n", 
                apvc->size_of_q_matrix_info);
            }
            
            av_log(NULL, AV_LOG_TRACE, "size_of_tile_info_info:              %"PRIu8"\n", 
            apvc->size_of_tile_info_info);
        }
    }       

    /* unsigned int(8) configurationVersion = 1; */
    avio_w8(pb, apvc->configurationVersion);

    avio_w8(pb, apvc->static_frame_header);
    
    av_log(NULL, AV_LOG_TRACE, "static_frame_header:                 %"PRIu8"\n", 
    apvc->static_frame_header);
    
    av_log(NULL, AV_LOG_TRACE, "capture_time_distance_ignored:       %"PRIu8"\n", 
    apvc->capture_time_distance_ignored);
    
    av_log(NULL, AV_LOG_TRACE, "largest_frame_header_size:           %"PRIu16"\n", 
    apvc->largest_frame_header_size);
    
    av_log(NULL, AV_LOG_TRACE, "largest_profile_idc:                 %"PRIu8"\n", 
    apvc->largest_profile_idc);
    
    av_log(NULL, AV_LOG_TRACE, "largest_level_idc:                   %"PRIu8"\n", 
    apvc->largest_level_idc);

    av_log(NULL, AV_LOG_TRACE, "largest_frame_width_minus1:          %"PRIu32"\n", 
    apvc->largest_frame_width_minus1);
    
    av_log(NULL, AV_LOG_TRACE, "largest_frame_height_minus1:         %"PRIu32"\n", 
    apvc->largest_frame_height_minus1);

    av_log(NULL, AV_LOG_TRACE, "largest_chroma_format_idc:           %"PRIu8"\n", 
    apvc->largest_chroma_format_idc);
    
    av_log(NULL, AV_LOG_TRACE, "largest_bit_depth_minus8:            %"PRIu8"\n", 
    apvc->largest_bit_depth_minus8);
    
    av_log(NULL, AV_LOG_TRACE, "largest_capture_time_distance:       %"PRIu8"\n", 
    apvc->largest_capture_time_distance);

    if (apvc->static_frame_header) {
        av_log(NULL, AV_LOG_TRACE, "frame_header_repeated:               %"PRIu8"\n", 
        apvc->frame_header_repeated);
        
        if (!apvc->frame_header_repeated) {
            av_log(NULL, AV_LOG_TRACE, "color_description_present_flag_info: %"PRIu8"\n",
            apvc->color_description_present_flag_info);
            
            av_log(NULL, AV_LOG_TRACE, "use_q_matrix_info:                   %"PRIu8"\n", 
            apvc->use_q_matrix_info);
            
            if (apvc->color_description_present_flag_info) {
                av_log(NULL, AV_LOG_TRACE, "color_primaries_info:                %"PRIu8"\n", 
                apvc->color_primaries_info);
                
                av_log(NULL, AV_LOG_TRACE, "transfer_characteristics_info:       %"PRIu8"\n", 
                apvc->transfer_characteristics_info);
                
                av_log(NULL, AV_LOG_TRACE, "matrix_coefficients_info:            %"PRIu8"\n", 
                apvc->matrix_coefficients_info);

            }
            if (apvc->use_q_matrix_info) {
                av_log(NULL, AV_LOG_TRACE, "size_of_q_matrix_info:               %"PRIu8"\n", 
                apvc->size_of_q_matrix_info);
            }
            
            av_log(NULL, AV_LOG_TRACE, "size_of_tile_info_info:              %"PRIu8"\n", 
            apvc->size_of_tile_info_info);
        }
    } 

    /* unsigned int(6) configurationVersion = 1
     * unsigned int(1) static_frame_header
	 * unsigned int(1) capture_time_distance_ignored
     */
    avio_w8(pb, apvc->configurationVersion << 2 | apvc->static_frame_header << 1 | apvc->capture_time_distance_ignored);

    /* unsigned int(16) largest_frame_header_size */
    avio_wb16(pb, apvc->largest_frame_header_size);

	/* unsigned int(8) largest_profile_idc */
    avio_w8(pb, apvc->largest_profile_idc);

	/* unsigned int(8) largest_level_idc */
    avio_w8(pb, apvc->largest_level_idc);

    /* unsigned int(32) largest_frame_width_minus1 */
    avio_wb32(pb, apvc->largest_frame_width_minus1);

    /* unsigned int(32) largest_frame_height_minus1 */
    avio_wb32(pb, apvc->largest_frame_height_minus1);

	/* unsigned int(4) largest_chroma_format_idc */
	/* unsigned int(4) largest_bit_depth_minus8 */
    avio_w8(pb, apvc->largest_chroma_format_idc << 4 | apvc->largest_bit_depth_minus8);

	/* unsigned int(8) largest_capture_time_distance */
    avio_w8(pb, apvc->largest_capture_time_distance);

    if (apvc->static_frame_header) {
		/* reserved_zero_7bits
         * unsigned int(1) frame_header_repeated
         */
        uint8_t reserved_zero_7bits = 0x01;
        avio_w8(pb, reserved_zero_7bits & apvc->frame_header_repeated);

		if (!apvc->frame_header_repeated) {
			/* reserved_zero_6bits
			 * unsigned int(1) color_description_present_flag_info
			 * unsigned int(1) use_q_matrix_info
             */
            uint8_t reserved_zero_6bits = 0x03;
            avio_w8(pb, reserved_zero_6bits & (apvc->color_description_present_flag_info << 1 | apvc->use_q_matrix_info));

			if (apvc->color_description_present_flag_info) {
				/* unsigned int(8) color_primaries_info */               
                avio_w8(pb, apvc->color_primaries_info);
                
                /* unsigned int(8) transfer_characteristics_info */
                avio_w8(pb, apvc->transfer_characteristics_info);
                
                /* unsigned int(8) matrix_coefficients_info */
                avio_w8(pb, apvc->matrix_coefficients_info);
			}
			if (apvc->use_q_matrix_info) {
				/* unsigned int (8) size_of_q_matrix_info */
                avio_w8(pb, apvc->size_of_q_matrix_info);

				/* unsigned int (8*size_of_q_matrix_info) quantization_matrix_info */
                for(int i=0; i<apvc->size_of_q_matrix_info; i++) {
                    avio_w8(pb, apvc->quantization_matrix_info[i]);
                }
			}
			/* unsigned int (8) size_of_tile_info_info */
            avio_w8(pb, apvc->size_of_tile_info_info);

			/* unsigned int (8*size_of_tile_info_info) tile_info_info; */
            for(int i=0; i<apvc->size_of_tile_info_info; i++) {
                avio_w8(pb, apvc->tile_info_info[i]);
            }
		}
	}

    return 0;
}

int ff_isom_write_apvc(AVIOContext *pb, const uint8_t *data,
                       int size, int ps_array_completeness)
{
    APVDecoderConfigurationBox apvc;
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

    // @deprecated
    // @todo reimplemntation is needed
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
