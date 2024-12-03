/*
 * APV format parser
 *
 * Copyright (C) 2023 Dawid Kozinski <d.kozinski@samsung.com>
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

#include "parser.h"
#include "bytestream.h"
#include "apv.h"
#include "apv_parse.h"

typedef struct APVParserContext {
    APVParamSets ps;

    int parsed_extradata;
} APVParserContext;

// @deprecated
// @todo reimplemntation is needed
// @see WD1_APV_spec section 7.2
static int parse_frame_data(AVCodecParserContext *s, AVCodecContext *avctx,
                            const uint8_t *buf, int buf_size)
{
    APVParserContext *ctx = s->priv_data;
    GetBitContext gb;
    int ret;

    if (buf_size <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid Frame Data size: (%d)\n", buf_size);
        return AVERROR_INVALIDDATA;
    }

    ret = init_get_bits8(&gb, buf, buf_size);
    if (ret < 0)
        return ret;

    ff_apv_parse_frame_data(&gb, &ctx->ps.frame_data);

    return 0;
}

/**
 * @deprecated
 * @todo Provide implementation
 * @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#name-raw-bitstream-format
 * 
 * Parse APV bitstream
 *
 * @param s codec parser context
 * @param avctx codec context
 * @param buf buffer with field/frame data
 * @param buf_size size of the buffer
 */
static int parse_apv_bitstream(AVCodecParserContext *s, AVCodecContext *avctx, const uint8_t *buf, int buf_size)
{
    const uint8_t *data = buf;
    int data_size = buf_size;

    if (data_size > 0) {
        int au_size = 0;
        
        // Buffer size is not enough for buffer to store Frame Data 4-bytes prefix (length)
        if (data_size < APV_AU_SIZE_PREFIX_LENGTH)
            return AVERROR_INVALIDDATA;

        au_size = apv_read_au_size(data, APV_AU_SIZE_PREFIX_LENGTH, avctx);

        if (!au_size || au_size > INT_MAX)
            return AVERROR_INVALIDDATA;

        data += APV_AU_SIZE_PREFIX_LENGTH;
        data_size -= APV_AU_SIZE_PREFIX_LENGTH;

        if (data_size < au_size)
            return AVERROR_INVALIDDATA;
    }
    return 0;
}

// Decoding Frame Data from apvC (APVDecoderConfigurationRecord)
// @todo provide implementation
static int decode_extradata(AVCodecParserContext *s, AVCodecContext *avctx)
{
    return 0;
}

// @notice Consider whether there is a need to parse the stream.
static int apv_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    int next;
    int ret;
    APVParserContext *ctx = s->priv_data;

    s->picture_structure = AV_PICTURE_STRUCTURE_FRAME;
    s->key_frame = 1;

    if (avctx->extradata && !ctx->parsed_extradata) {
        decode_extradata(s, avctx);
        ctx->parsed_extradata = 1;
    }

    next = buf_size;

    ret = parse_apv_bitstream(s, avctx, buf, buf_size);
    if(ret < 0) {
        *poutbuf      = NULL;
        *poutbuf_size = 0;
        return buf_size;
    }

    // poutbuf contains just one Frame Data Unit
    *poutbuf      = buf;
    *poutbuf_size = buf_size;

    return next;
}

static void apv_parser_close(AVCodecParserContext *s)
{
    APVParserContext *ctx = s->priv_data;

    // @todo move to APVFrameData.delete()
    if(ctx->ps.frame_data.frame_data_header.tile_info.ColStarts) {
        free(ctx->ps.frame_data.frame_data_header.tile_info.ColStarts);
        ctx->ps.frame_data.frame_data_header.tile_info.ColStarts = NULL;
    }

    if(ctx->ps.frame_data.frame_data_header.tile_info.RowStarts) {
        free(ctx->ps.frame_data.frame_data_header.tile_info.RowStarts);
        ctx->ps.frame_data.frame_data_header.tile_info.RowStarts = NULL;
    }
    
    if(ctx->ps.frame_data.frame_data_header.tile_info.tile_size_minus1) {
        free(ctx->ps.frame_data.frame_data_header.tile_info.tile_size_minus1);
        ctx->ps.frame_data.frame_data_header.tile_info.tile_size_minus1 = NULL;
    }

    ff_apv_ps_free(&ctx->ps);
}

const AVCodecParser ff_apv_parser = {
    .codec_ids      = { AV_CODEC_ID_APV },
    .priv_data_size = sizeof(APVParserContext),
    .parser_parse   = apv_parse,
    .parser_close   = apv_parser_close,
};
