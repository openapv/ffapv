[![Build OAPV_FFmpeg](https://github.com/openapv/ffapv/actions/workflows/build.yml/badge.svg)](https://github.com/openapv/ffapv/actions/workflows/build.yml)

# FFmpeg with OpenAPV support

This project contains FFmpeg project implementation with added support for APV codec using fully OpenAPV library.  
https://github.com/AcademySoftwareFoundation/openapv

Pure FFmpeg currently have OpenAPV encoder and native APV decoder only.
This project additionally provides **APV family/profile selection** for the OpenAPV encoder (`-family` option: 422_LQ, 422_SQ, 422_HQ, 444_UQ).

Rest functionallities are the same as in pure ffmpeg.  

# Encoding with liboapv

The APV encoder is selected with `-c:v liboapv`. Supported input pixel formats:
`gray10`, `yuv422p10`, `yuv422p12`, `yuv444p10`, `yuv444p12`, `yuva444p10`, `yuva444p12`.

Encoder options:

| Option | Values | Description |
|---|---|---|
| `-family` | `422_LQ`, `422_SQ`, `422_HQ`, `444_UQ` | APV family. Sets the target bitrate for the resolution/frame rate and switches to ABR rate control. The family must match the profile implied by the pixel format (422 families need a 4:2:2 input, `444_UQ` needs 4:4:4). |
| `-b:v` | e.g. `200M` | Target bitrate (ABR). Ignored when `-family` is used. |
| `-qp` | 0–63 for 10-bit, 0–75 for 12-bit input (default 32) | Quantization parameter for constant-QP mode. Used when neither `-b:v` nor `-family` is given. |
| `-preset` | `fastest`, `fast`, `medium`, `slow`, `placebo` | Speed/quality trade-off. |
| `-oapv-params` | `key=value:key=value` | Pass options directly to the OpenAPV library, e.g. `qp=30:tile-w=512`. |

## Examples

Encode to a raw APV bitstream:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 output.apv

Encode into MP4 (the `apv1` sample entry is used):

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 output.mp4

Encode with an APV family (recommended; picks the standard bitrate for the
resolution and frame rate):

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -family 422_HQ output.mp4

4:4:4 finishing quality:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv444p10 -family 444_UQ output.mp4

Explicit target bitrate instead of a family:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -b:v 200M output.mp4

Constant-QP encoding with a slower preset:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -qp 25 -preset slow output.mp4

Fine-grained control through OpenAPV parameters:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -oapv-params "qp=30:tile-w=512:tile-h=512" output.mp4

Decode APV (FFmpeg's native APV decoder is used automatically):

    ffmpeg -i input.mp4 -pix_fmt yuv422p10 output.yuv

# Building

Prerequisites: a C compiler, `make`, `cmake`, `pkg-config`, and `nasm`
(or configure FFmpeg with `--disable-x86asm`).

1. Build and install the OpenAPV library:

       git clone https://github.com/AcademySoftwareFoundation/openapv.git
       cmake -S openapv -B openapv/build -DCMAKE_BUILD_TYPE=Release
       cmake --build openapv/build -j
       sudo cmake --install openapv/build

2. Configure and build FFmpeg with the liboapv encoder enabled:

       git clone https://github.com/openapv/ffapv.git
       cd ffapv
       ./configure --enable-liboapv
       make -j

   If OpenAPV is installed in a non-default prefix, point pkg-config at it:

       PKG_CONFIG_PATH=/path/to/prefix/lib/pkgconfig ./configure --enable-liboapv

3. Verify the encoder is available:

       ./ffmpeg -h encoder=liboapv

# FFmpeg README

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides means to alter decoded audio and video through a directed graph of connected filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.
