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

#include "parser.h"

#include "apv.h"
#include "apv_parse.h"

// @deprecated
// @see https://datatracker.ietf.org/doc/html/draft-lim-apv-02#name-frame
// @todo reimplemntation is needed
int ff_apv_parse_frame_data(GetBitContext *gb, APVFrameData *fd)
{
    ff_apv_parse_frame_header(gb, &fd->frame_data_header);

    return 0;
}
