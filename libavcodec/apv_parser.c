/*
 * APV format parser
 *
 * Copyright (C) 2025 Dawid Kozinski <d.kozinski@samsung.com>
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

#define APV_AU_SIZE_PREFIX_LENGTH (4)

typedef struct APVParserContext {
    int parsed_extradata;
} APVParserContext;

/**
 * @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-12.1
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
    if (!buf || buf_size <= 0) {
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

// Decoding Frame Data from apvC (APVDecoderConfigurationRecord)
static int decode_extradata(AVCodecParserContext *s, AVCodecContext *avctx)
{
    // version                          [ 1 byte ]
    // flags                            [ 3 bytes]
    // APVDecoderConfigurationRecord    [at least 18 bytes]
    const uint8_t *data = avctx->extradata;
    int size = avctx->extradata_size;

    int ret = 0;
    if (!data || size < 22) {
        return AVERROR_INVALIDDATA;
    }
    
    return ret;
}

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

    // poutbuf contains just one Access Unit
    *poutbuf      = buf;
    *poutbuf_size = buf_size;

    return next;
}

const AVCodecParser ff_apv_parser = {
    .codec_ids      = { AV_CODEC_ID_APV },
    .priv_data_size = sizeof(APVParserContext),
    .parser_parse   = apv_parse,
};
