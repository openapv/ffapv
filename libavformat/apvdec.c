/*
 * RAW APV video demuxer
 *
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

#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/internal.h"
#include "libavcodec/apv.h"
#include "libavcodec/apv_parse.h"
#include "libavcodec/bsf.h"

#include "libavutil/opt.h"

#include "rawdec.h"
#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"


#define RAW_PACKET_SIZE 1024
#define APV_FRAME_DATA_HEADER_SIZE 24

typedef struct APVParserContext {
    int got_frame_data;
    APVFrameDataHeader frame_data_header;
} APVParserContext;

typedef struct APVDemuxContext {
    const AVClass *class;
    AVRational framerate;
    int width;
    int height;

    AVBSFContext *bsf;
    APVParamSets ps;

} APVDemuxContext;

#define DEC AV_OPT_FLAG_DECODING_PARAM
#define OFFSET(x) offsetof(APVDemuxContext, x)
static const AVOption apv_options[] = {
    { "framerate", "", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, DEC},
    { NULL },
};
#undef OFFSET

static const AVClass apv_demuxer_class = {
    .class_name = "APV Annex B demuxer",
    .item_name  = av_default_item_name,
    .option     = apv_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int apv_parse_frame_data_header(GetBitContext *gb, APVFrameDataHeader *fdh)
{
    fdh->frame_header_size              = get_bits(gb, 16);
    fdh->profile_idc                    = get_bits(gb, 8);
    fdh->level_idc                      = get_bits(gb, 8);
    fdh->reserved_zero_8bits            = get_bits(gb, 8);
    fdh->frame_width_minus1             = get_bits(gb, 32);
    fdh->frame_height_minus1            = get_bits(gb, 32);

    // @todo documentation inconsistency with the reference implementation.
    //
    // The reference application uses 4 bits for chroma_format_idc
    // while the documentation says that chroma_format_idc takes up 2 bits in the header
    fdh->chroma_format_idc              = get_bits(gb, 4);

    fdh->bit_depth_minus8               = get_bits(gb, 4);
    fdh->capture_time_distance          = get_bits(gb, 8);
    fdh->reserved_zero_16bits           = get_bits(gb, 16);
    fdh->color_description_present_flag = get_bits(gb, 1);
    if(fdh->color_description_present_flag) {
        fdh->color_primaries            = get_bits(gb, 8);
        fdh->transfer_characteristics   = get_bits(gb, 8);
        fdh->matrix_coefficients        = get_bits(gb, 8);
    }
    else {
      fdh->color_primaries          = 2;
      fdh->transfer_characteristics = 2;
      fdh->matrix_coefficients      = 2;
    }

    fdh->use_q_matrix                   = get_bits(gb, 1);

    return 0;
}

static int apv_annexb_probe(const AVProbeData *p)
{
    APVParserContext ev = {0};

    size_t frame_data_header_size;
    GetBitContext gb;

    unsigned char *bs = (unsigned char *)p->buf;
    int bs_size = p->buf_size;
    int ret = 0;

    if (bs_size < APV_FRAME_DATA_SIZE_PREFIX_LENGTH + APV_FRAME_DATA_HEADER_SIZE)
        return 0;

    ret = init_get_bits8(&gb, bs, bs_size);
    if (ret < 0)
        return 0;

    // skip a four-byte length Frame Data Size syntax element, which indicates the size of the Frame Data in bytes
    skip_bits_long(&gb, 32);

    // read frame data header size in bytes
    frame_data_header_size  = show_bits(&gb, 16);

    if(bs_size < APV_FRAME_DATA_SIZE_PREFIX_LENGTH + frame_data_header_size)
        return 0;

    apv_parse_frame_data_header(&gb, &ev.frame_data_header);

    if( ev.frame_data_header.profile_idc == 33 && // @see Annex A - A.3.2 Profiles (Baseline profile)
        ev.frame_data_header.chroma_format_idc == 2 &&
        ev.frame_data_header.bit_depth_minus8 >= 2 && ev.frame_data_header.bit_depth_minus8 <= 4 &&
        ev.frame_data_header.reserved_zero_16bits == 0 )
        ev.got_frame_data = 1;

    if (ev.got_frame_data) {
        return AVPROBE_SCORE_EXTENSION + 1;  // 1 more than .mpg
    }

    return 0;
}

static int apv_read_header(AVFormatContext *s)
{
    AVStream *st;
    FFStream *sti;

    APVDemuxContext *c = s->priv_data;
    APVFrameDataHeader frame_data_header;
    GetBitContext gb;
    int ret;
    int frame_data_header_size;

    int eof = avio_feof (s->pb);
    if(eof) {
        return AVERROR_EOF;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    sti = ffstream(st);

    ret = init_get_bits8(&gb, s->pb->buffer, s->pb->buffer_size);
    if (ret < 0)
        return 0;

    skip_bits_long(&gb, 32); // frame data length

    // read frame data header size in bytes
    frame_data_header_size  = show_bits(&gb, 16);

    if(s->pb->buffer_size < APV_FRAME_DATA_SIZE_PREFIX_LENGTH + frame_data_header_size)
        return 0;

    apv_parse_frame_data_header(&gb,  &frame_data_header);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_APV;
    st->codecpar->width = frame_data_header.frame_width_minus1+1;
    st->codecpar->height = frame_data_header.frame_height_minus1+1;

    if(frame_data_header.chroma_format_idc == 2 && frame_data_header.bit_depth_minus8 == 2) {
        st->codecpar->format = AV_PIX_FMT_YUV422P10;
    } else if(frame_data_header.chroma_format_idc == 3 && frame_data_header.bit_depth_minus8 == 2) {
        st->codecpar->format = AV_PIX_FMT_YUV444P10;
    } else {
        st->codecpar->format = AV_PIX_FMT_NONE;
    }

    // This causes sending to the parser full frames, not chunks of data
    // The flag PARSER_FLAG_COMPLETE_FRAMES will be set in demux.c (demux.c: 1316)
    sti->need_parsing = AVSTREAM_PARSE_HEADERS;

    st->avg_frame_rate = c->framerate;

    // taken from rawvideo demuxers
    avpriv_set_pts_info(st, 64, 1, 1200000);

    return 0;
}

static int apv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    uint32_t frame_data_size;
    uint8_t buf[APV_FRAME_DATA_SIZE_PREFIX_LENGTH];

    int eof = avio_feof (s->pb);
    if(eof) {
        return AVERROR_EOF;
    }

    ret = ffio_ensure_seekback(s->pb, APV_FRAME_DATA_SIZE_PREFIX_LENGTH);
    if (ret < 0)
        return ret;

    ret = avio_read(s->pb, buf, APV_FRAME_DATA_SIZE_PREFIX_LENGTH);
    if (ret < 0) {
        return ret;
    }
    if (ret != APV_FRAME_DATA_SIZE_PREFIX_LENGTH)
        return AVERROR_INVALIDDATA;


    frame_data_size = apv_read_frame_data_size(buf, APV_FRAME_DATA_SIZE_PREFIX_LENGTH, s);
    if (!frame_data_size || frame_data_size > INT_MAX)
            return AVERROR_INVALIDDATA;


    avio_seek(s->pb, -APV_FRAME_DATA_SIZE_PREFIX_LENGTH, SEEK_CUR);

    // put the FrameData into pkt
    ret = av_get_packet(s->pb, pkt, frame_data_size + APV_FRAME_DATA_SIZE_PREFIX_LENGTH);
    if (ret < 0)
        return ret;

    if (ret != (frame_data_size + APV_FRAME_DATA_SIZE_PREFIX_LENGTH))
        return AVERROR_INVALIDDATA;

    return ret;
}

static int apv_read_close(AVFormatContext *s)
{
    APVDemuxContext *const c = s->priv_data;

    av_bsf_free(&c->bsf);
    return 0;
}

const AVInputFormat ff_apv_demuxer = {
    .name           = "apv",
    .long_name      = NULL_IF_CONFIG_SMALL("APV Annex B"),
    .read_probe     = apv_annexb_probe,
    .read_header    = apv_read_header, // annexb_read_header
    .read_packet    = apv_read_packet, // annexb_read_packet
    .read_close     = apv_read_close,
    .extensions     = "apv",
    .flags          = AVFMT_GENERIC_INDEX,
    .flags_internal = FF_FMT_INIT_CLEANUP,
    .raw_codec_id   = AV_CODEC_ID_APV,
    .priv_data_size = sizeof(APVDemuxContext),
    .priv_class     = &apv_demuxer_class,
};