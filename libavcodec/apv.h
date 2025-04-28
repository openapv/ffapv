/*
 * APV definitions and enums
 *
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

#ifndef AVCODEC_APV_H
#define AVCODEC_APV_H

// The length field that indicates the length in bytes of the following Frame Data is configured to be of 4 bytes
// A four-byte length Frame Data Size syntax element, which indicates the size of the Frame Data in bytes, may precede the Frame Data, depending on the application.
#define APV_AU_SIZE_PREFIX_LENGTH           (4)  /* byte */
#define APV_PBU_SIZE_PREFIX_LENGTH          (4)  /* byte */
#define APV_SIGNATURE_LENGTH                (4)  /* byte */

// @deprecated
#define APV_FRAME_DATA_SIZE_PREFIX_LENGTH   (4)  /* byte */

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-5.3.3
#define APV_PBU_HEADER_SIZE                 (4)  /* byte */

// @see https://www.ietf.org/archive/id/draft-lim-apv-04.html#section-5.3.6
#define APV_FRAME_INFO_SIZE                 (14)  /* byte */

/* size of macroblock */
#define APV_LOG2_MB                     (4)
#define APV_MB                          (1<<APV_LOG2_MB)
#define APV_MB_W                        (1<<APV_LOG2_MB)
#define APV_MB_H                        (1<<APV_LOG2_MB)
#define APV_MB_D                        (APV_MB_W * APV_MB_H)

/* size of block */
#define APV_LOG2_BLOCK                  (3)
#define APV_BLOCK                       (1<<APV_LOG2_BLOCK)
#define APV_BLOCK_W                     (1<<APV_LOG2_BLOCK)
#define APV_BLOCK_H                     (1<<APV_LOG2_BLOCK)
#define APV_BLOCK_D                     (APV_BLOCK_W * APV_BLOCK_H)

/* @see WD1 APV spec section 7.3.2.2*/
#define APV_PREV_DC_DIFF 40

/* size of macroblock */
enum {
    APV_MB_WIDTH    = 16,
    APV_MB_HEIGHT   = 16,
};

/* Color components */
enum { 
    APV_COLOR_COMP_Y    = 0,    /* Y luma */
    APV_COLOR_COMP_U    = 1,    /* Cb Chroma */
    APV_COLOR_COMP_V    = 2,    /* Cr Chroma */
    APV_COLOR_COMP_A    = 3,    /* Alpha */
    APV_COLOR_COMP_NUM  = 4     /* number of color component */
};

/* size of block */
#define LOG2_BLK                   (3)
#define LOG2_BLK_W                 (3)
#define LOG2_BLK_H                 (3)
#define BLK_W                      (1 << LOG2_BLK)
#define BLK_H                      (1 << LOG2_BLK)
#define BLK_D                      (BLK_W * BLK_H)

const static uint16_t ScanOrder[BLK_D] =
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

#endif // AVCODEC_APV_H
