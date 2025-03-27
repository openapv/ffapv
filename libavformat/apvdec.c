/*
 * RAW APV video demuxer
 *
 * Copyright (c) 2024 Dawid Kozinski <d.kozinski@samsung.com>
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

#include "libavutil/opt.h"

#include "rawdec.h"
#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"

typedef struct APVParserContext {
    int got_frame_data;

    APVPBUHeader pbu_header;
    APVFrameInfo frame_info;
} APVParserContext;

typedef struct APVDemuxContext {
    const AVClass *class;
    AVRational framerate;

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

// @see https://datatracker.ietf.org/doc/html/draft-lim-apv-03#section-5.3.3
// @see https://datatracker.ietf.org/doc/html/draft-lim-apv-03#name-primitive-bitstream-unit-he
static int apv_parse_pbu_header(GetBitContext *gb, APVPBUHeader *pbuh)
{
    pbuh->pbu_type                      = get_bits(gb, 8);
    pbuh->group_id                      = get_bits(gb, 16);
    pbuh->reserved_zer_8bits            = get_bits(gb, 8);
        
    return 0;
}

// https://www.ietf.org/archive/id/draft-lim-apv-03.html#name-frame-information
static int apv_parse_frame_info(GetBitContext *gb, APVFrameInfo *frame_info)
{
    frame_info->profile_idc                    = get_bits(gb, 8);
    frame_info->level_idc                      = get_bits(gb, 8);
    frame_info->band_idc                       = get_bits(gb, 3);
    frame_info->reserved_zero_5bits            = get_bits(gb, 5);
    frame_info->frame_width                    = get_bits(gb, 24);
    frame_info->frame_height                   = get_bits(gb, 24);
    frame_info->chroma_format_idc              = get_bits(gb, 4);
    frame_info->bit_depth_minus8               = get_bits(gb, 4);
    frame_info->capture_time_distance          = get_bits(gb, 8);
    frame_info->reserved_zero_8bits            = get_bits(gb, 8);
    
    return 0;
}

// The implementation of the probe function is in accordance with the documentation provided in draft-lim-apv-02 version 02. 
// https://datatracker.ietf.org/doc/html/draft-lim-apv-02
static int apv_annexb_probe(const AVProbeData *p)
{
    APVParserContext ev = {0};

    GetBitContext gb;

    unsigned char *bs = (unsigned char *)p->buf;
    int bs_size = p->buf_size;
    int ret = 0;
    uint32_t signature;

    if (bs_size < APV_PBU_SIZE_PREFIX_LENGTH + APV_AU_SIZE_PREFIX_LENGTH + APV_PBU_HEADER_SIZE)
        return 0;

    ret = init_get_bits8(&gb, bs, bs_size);
    if (ret < 0)
        return 0;

    // skip a four-byte length Access Unit Size syntax element, which indicates the size of the AU in bytes
    skip_bits_long(&gb, 32);

    // A four-character code that identifies the bitstream as an APV AU. The value MUST be 'aPv1' (0x61507631)
    signature = get_bits_long(&gb, 32);
    if (signature != 0x61507631) {
        return 0;
    }

    // skip a four-byte length PBU Size syntax element, which indicates the size of the PBU in bytes
    skip_bits_long(&gb, 32);

    apv_parse_pbu_header(&gb, &ev.pbu_header);
    
    if( ev.pbu_header.pbu_type == 1  ||
        ev.pbu_header.pbu_type == 2  ||
        ev.pbu_header.pbu_type == 25 ||
        ev.pbu_header.pbu_type == 26 ||
        ev.pbu_header.pbu_type == 27 ) {

        apv_parse_frame_info(&gb, &ev.frame_info);
    
        // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.1
        // Conformance of a coded frame to the 422-10 profile is indicated by profile_idc equal to 33
        if(ev.frame_info.profile_idc == 33 && 
            ev.frame_info.chroma_format_idc == 2 &&  
            ev.frame_info.bit_depth_minus8 == 2 && 
            ev.pbu_header.pbu_type == 1 )
        {
            ev.got_frame_data = 1;
        }
        // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.2
        // Conformance of a coded frame to the 422-12 profile is indicated by profile_idc equal to 44
        else if(ev.frame_info.profile_idc == 44 && 
            ev.frame_info.chroma_format_idc == 2 &&  
            ev.frame_info.bit_depth_minus8 >= 2 && ev.frame_info.bit_depth_minus8 <= 4 && 
            ev.pbu_header.pbu_type == 1 )
        {
            ev.got_frame_data = 1;
        }
        // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.3
        // Conformance of a coded frame to the 444-10 profile is indicated by profile_idc equal to 55
        else if(ev.frame_info.profile_idc == 55 && 
            ev.frame_info.chroma_format_idc >= 2 && ev.frame_info.chroma_format_idc <= 3  &&  
            ev.frame_info.bit_depth_minus8 == 2 && 
            ev.pbu_header.pbu_type == 1 )
        {
            ev.got_frame_data = 1;
        }
        // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.4
        // Conformance of a coded frame to the 444-12 profile is indicated by profile_idc equal to 66
        else if(ev.frame_info.profile_idc == 66 && 
            ev.frame_info.chroma_format_idc >= 2 && ev.frame_info.chroma_format_idc <= 3  &&  
            ev.frame_info.bit_depth_minus8 >= 2 && ev.frame_info.bit_depth_minus8 <= 4 && 
            ev.pbu_header.pbu_type == 1 )
        {
            ev.got_frame_data = 1;
        }
        // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.5
        // Conformance of a coded frame to the 4444-10 profile is indicated by profile_idc equal to 77
        else if(ev.frame_info.profile_idc == 77 && 
            ev.frame_info.chroma_format_idc >= 2 && ev.frame_info.chroma_format_idc <= 4  &&  
            ev.frame_info.bit_depth_minus8 == 2  && 
            ev.pbu_header.pbu_type == 1 )
        {
            ev.got_frame_data = 1;
        }
        // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.6
        // Conformance of a coded frame to the 4444-12 profile is indicated by profile_idc equal to 88
        else if(ev.frame_info.profile_idc == 88 && 
            ev.frame_info.chroma_format_idc >= 2 && ev.frame_info.chroma_format_idc <= 4  &&  
            ev.frame_info.bit_depth_minus8 >= 2 && ev.frame_info.bit_depth_minus8 <= 4 &&
            ev.pbu_header.pbu_type == 1 )
        {
            ev.got_frame_data = 1;
        }
        // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.7
        // Conformance of a coded frame to the 400-10 profile is indicated by profile_idc equal to 99
        else if(ev.frame_info.profile_idc == 99 && 
            ev.frame_info.chroma_format_idc == 0  &&  
            ev.frame_info.bit_depth_minus8 == 2 &&
            ev.pbu_header.pbu_type == 1 )
        {
            ev.got_frame_data = 1;
        }
    }
    
    if (ev.got_frame_data && 
        ev.pbu_header.reserved_zer_8bits == 0 && 
        ev.frame_info.reserved_zero_5bits == 0 &&
        ev.frame_info.reserved_zero_8bits == 0 ) {
        return AVPROBE_SCORE_MAX;
    }

    return 0;
}

static int apv_read_header(AVFormatContext *s)
{
    AVStream *st;
    FFStream *sti;

    APVDemuxContext *c = s->priv_data;
    APVFrameInfo frame_info;
    APVPBUHeader pbu_header;

    GetBitContext gb;
    int ret;
    uint32_t to_read = 0;

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

    to_read += APV_AU_SIZE_PREFIX_LENGTH;
    to_read += APV_SIGNATURE_LENGTH;
    to_read += APV_PBU_SIZE_PREFIX_LENGTH;
    to_read += APV_PBU_HEADER_SIZE;
    to_read += APV_FRAME_INFO_SIZE;

    if(s->pb->buffer_size < to_read)
        return 0; 
    
    skip_bits_long(&gb, 32); // AU size
    skip_bits_long(&gb, 32); // signature
    skip_bits_long(&gb, 32); // PBU size

    apv_parse_pbu_header(&gb, &pbu_header);

    if((1  <= pbu_header.pbu_type && pbu_header.pbu_type <=2) ||
       (25 <= pbu_header.pbu_type && pbu_header.pbu_type <= 27))    {
        apv_parse_frame_info(&gb,  &frame_info);
    } else if(pbu_header.pbu_type == 65) {
        apv_parse_frame_info(&gb, &frame_info);
    } else {
        return 0;
    }

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_APV;
    st->codecpar->width = frame_info.frame_width;
    st->codecpar->height = frame_info.frame_height;

    // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.1
    // Conformance of a coded frame to the 422-10 profile is indicated by profile_idc equal to 33
    if(frame_info.profile_idc == 33 && 
       frame_info.chroma_format_idc == 2 &&  
       frame_info.bit_depth_minus8 == 2 && 
       pbu_header.pbu_type == 1 ) 
    {
        st->codecpar->format = AV_PIX_FMT_YUV422P10;
    } 
    // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.2
    // Conformance of a coded frame to the 422-12 profile is indicated by profile_idc equal to 44
    else if(frame_info.profile_idc == 44 && 
            frame_info.chroma_format_idc == 2 &&  
            frame_info.bit_depth_minus8 >= 2 && frame_info.bit_depth_minus8 <= 4 && 
            pbu_header.pbu_type == 1 )
    {
        if(frame_info.bit_depth_minus8 == 2) {
            st->codecpar->format = AV_PIX_FMT_YUV422P10;
        } else {
            st->codecpar->format = AV_PIX_FMT_YUV422P12;
        }
    }
    // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.3
    // Conformance of a coded frame to the 444-10 profile is indicated by profile_idc equal to 55
    else if(frame_info.profile_idc == 55 && 
            frame_info.chroma_format_idc >= 2 && frame_info.chroma_format_idc <= 3  &&  
            frame_info.bit_depth_minus8 == 2 && 
            pbu_header.pbu_type == 1 )
    {
        if(frame_info.chroma_format_idc == 2) {
            st->codecpar->format = AV_PIX_FMT_YUV422P10;
        } else {
            st->codecpar->format = AV_PIX_FMT_YUV444P10;
        }
    }
    // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.4
    // Conformance of a coded frame to the 444-12 profile is indicated by profile_idc equal to 66
    else if(frame_info.profile_idc == 66 && 
            frame_info.chroma_format_idc >= 2 && frame_info.chroma_format_idc <= 3  &&  
            frame_info.bit_depth_minus8 >= 2 && frame_info.bit_depth_minus8 <= 4 && 
            pbu_header.pbu_type == 1 )
    {
            if(frame_info.chroma_format_idc == 2) {
                if(frame_info.bit_depth_minus8 == 2) {
                    st->codecpar->format = AV_PIX_FMT_YUV422P10;
                } else {
                    st->codecpar->format = AV_PIX_FMT_YUV422P12;
                }
            } else {
                if(frame_info.bit_depth_minus8 == 2) {
                    st->codecpar->format = AV_PIX_FMT_YUV444P10;
                } else {
                    st->codecpar->format = AV_PIX_FMT_YUV444P12;
                }
            }  
    }
    // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.5
    // Conformance of a coded frame to the 4444-10 profile is indicated by profile_idc equal to 77
    else if(frame_info.profile_idc == 77 && 
            frame_info.chroma_format_idc >= 2 && frame_info.chroma_format_idc <= 4  &&  
            frame_info.bit_depth_minus8 == 2  && 
            pbu_header.pbu_type == 1 )
    {
        if(frame_info.chroma_format_idc == 2) {
            st->codecpar->format = AV_PIX_FMT_YUV422P10;
        } else if (frame_info.chroma_format_idc == 3) {
            st->codecpar->format = AV_PIX_FMT_YUV444P10;
        } else {
            st->codecpar->format = AV_PIX_FMT_YUVA444P10;
        }
    }
    // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.6
    // Conformance of a coded frame to the 4444-12 profile is indicated by profile_idc equal to 88
    else if(frame_info.profile_idc == 88 && 
        frame_info.chroma_format_idc >= 2 && frame_info.chroma_format_idc <= 4  &&  
        frame_info.bit_depth_minus8 >= 2 && frame_info.bit_depth_minus8 <= 4 &&
        pbu_header.pbu_type == 1 )
    {
        if(frame_info.chroma_format_idc == 2) {
            if(frame_info.bit_depth_minus8 == 2) {
                st->codecpar->format = AV_PIX_FMT_YUV422P10;
            } else {
                st->codecpar->format = AV_PIX_FMT_YUV422P12;
            }
        } else if(frame_info.chroma_format_idc == 3) {
            if(frame_info.bit_depth_minus8 == 2) {
                st->codecpar->format = AV_PIX_FMT_YUV444P10;
            } else {
                st->codecpar->format = AV_PIX_FMT_YUV444P12;
            }
        } else {
            if(frame_info.bit_depth_minus8 == 2) {
                st->codecpar->format = AV_PIX_FMT_YUVA444P10;
            } else {
                st->codecpar->format = AV_PIX_FMT_YUVA444P12;
            }
        }  
    }
    // @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#section-10.1.3.1.7
    // Conformance of a coded frame to the 400-10 profile is indicated by profile_idc equal to 99
    else if(frame_info.profile_idc == 99 && 
            frame_info.chroma_format_idc == 0  &&  
            frame_info.bit_depth_minus8 == 2 &&
            pbu_header.pbu_type == 1 )
    {
        st->codecpar->format = AV_PIX_FMT_GRAY10;
    } else {
        st->codecpar->format = AV_PIX_FMT_NONE;
    }

    sti->need_parsing = PARSER_FLAG_COMPLETE_FRAMES; // AVSTREAM_PARSE_FULL_RAW;

    st->avg_frame_rate = c->framerate;

    avpriv_set_pts_info(st, 64, 1, 1200000);

    return 0;
}

static int apv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    uint32_t au_size;
    uint8_t buf[APV_AU_SIZE_PREFIX_LENGTH];

    int eof = avio_feof (s->pb);
    if(eof) {
        return AVERROR_EOF;
    }

    ret = ffio_ensure_seekback(s->pb, APV_AU_SIZE_PREFIX_LENGTH);
    if (ret < 0)
        return ret;
    ret = avio_read(s->pb, buf, APV_AU_SIZE_PREFIX_LENGTH);
    if (ret < 0) {
        return ret;
    }

    if (ret != APV_AU_SIZE_PREFIX_LENGTH)
        return AVERROR_INVALIDDATA;

    au_size = apv_read_au_size(buf, APV_AU_SIZE_PREFIX_LENGTH, s);
    if (!au_size || au_size > INT_MAX)
            return AVERROR_INVALIDDATA;

    avio_seek(s->pb, -APV_AU_SIZE_PREFIX_LENGTH, SEEK_CUR);

    ret = av_get_packet(s->pb, pkt, au_size + APV_AU_SIZE_PREFIX_LENGTH);
    if (ret < 0)
        return ret;
    
    pkt->pos= avio_tell(s->pb);
    pkt->stream_index = 0;
    
    if (ret != (au_size + APV_AU_SIZE_PREFIX_LENGTH)) {
            return AVERROR_INVALIDDATA;
    }

    return ret;
}

const FFInputFormat ff_apv_demuxer = {
    .p.name         = "apv",
    .p.long_name    = NULL_IF_CONFIG_SMALL("APV Annex B"),
    .p.extensions   = "apv",
    .p.flags        = AVFMT_GENERIC_INDEX | AVFMT_NOTIMESTAMPS,
    .p.priv_class   = &apv_demuxer_class,
    .read_probe     = apv_annexb_probe,
    .read_header    = apv_read_header,
    .read_packet    = apv_read_packet,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .raw_codec_id   = AV_CODEC_ID_APV,
    .priv_data_size = sizeof(APVDemuxContext),
};