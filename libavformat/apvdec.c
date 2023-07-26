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
#include "internal.h"


#define RAW_PACKET_SIZE 1024

typedef struct APVParserContext {
    int got_frame_data;
    APVFrameDataHeader frame_data_header;
} APVParserContext;

typedef struct APVDemuxContext {
    const AVClass *class;
    AVRational framerate;

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

static int parse_frame_data_units(const AVProbeData *p, APVParserContext *ev)
{
    size_t frame_data_size;
    unsigned char *bits = (unsigned char *)p->buf;
    int bytes_to_read = p->buf_size;
    int ret = 0;

    while (bytes_to_read > APV_FRAME_DATA_SIZE_PREFIX_LENGTH) {

        GetBitContext gb;

        frame_data_size = apv_read_frame_data_size(bits, APV_FRAME_DATA_SIZE_PREFIX_LENGTH, ev);
        if (frame_data_size == 0) break;

        bits += APV_FRAME_DATA_SIZE_PREFIX_LENGTH;
        bytes_to_read -= APV_FRAME_DATA_SIZE_PREFIX_LENGTH;

        if(bytes_to_read < frame_data_size) break;

        ret = init_get_bits8(&gb, bits, bytes_to_read);
        if (ret < 0)
            return ret;

        ff_apv_parse_frame_header(&gb, &ev->frame_data_header);
    }

    return 0;
}

static int annexb_probe(const AVProbeData *p)
{
    APVParserContext ev = {0};
    int ret = parse_frame_data_units(p, &ev);

    if( ev.frame_data_header.profile_idc == 33 && // @see Annex A - A.3.2 Profiles (Baseline profile)
        ev.frame_data_header.chroma_format_idc == 2 && // @see Annex A - A.3.2 Profiles (Baseline profile)
        ev.frame_data_header.bit_depth_minus8 >= 2 && ev.frame_data_header.bit_depth_minus8 <= 4 &&
        ev.frame_data_header.reserved_zero_16bits == 0 )
        ev.got_frame_data = 0;

    if (ret == 0 && ev.got_frame_data)
        return AVPROBE_SCORE_EXTENSION + 1;  // 1 more than .mpg

    return 0;
}

static int apv_read_header(AVFormatContext *s)
{
    AVStream *st;
    FFStream *sti;

    APVDemuxContext *c = s->priv_data;
    int ret = 0;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    sti = ffstream(st);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_APV;

    // This causes sending to the parser full frames, not chunks of data
    // The flag PARSER_FLAG_COMPLETE_FRAMES will be set in demux.c (demux.c: 1316)
    sti->need_parsing = AVSTREAM_PARSE_HEADERS;

    st->avg_frame_rate = c->framerate;
    st->codecpar->framerate = c->framerate;

    // taken from rawvideo demuxers
    avpriv_set_pts_info(st, 64, 1, 1200000);

    return 0;
}

static int apv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    int32_t frame_data_size;
    uint8_t buf[APV_FRAME_DATA_SIZE_PREFIX_LENGTH];

    int eof = avio_feof (s->pb);
    if(eof) {
        av_packet_unref(pkt);
        return AVERROR_EOF;
    }

    ret = avio_read(s->pb, (unsigned char *)&buf, APV_FRAME_DATA_SIZE_PREFIX_LENGTH);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }

    frame_data_size = apv_read_frame_data_size((const uint8_t *)&buf, APV_FRAME_DATA_SIZE_PREFIX_LENGTH, s);
    if(frame_data_size <= 0) {
        av_packet_unref(pkt);
        return -1;
    }

    // pyt the FrameData into pkt
    ret = av_get_packet(s->pb, pkt, frame_data_size);
    if (ret < 0)
        return ret;

    if (ret != frame_data_size)
        return AVERROR(EIO);

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
    .read_probe     = annexb_probe,
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