[![Build OAPV_FFmpeg](https://github.com/openapv/ffapv/actions/workflows/build.yml/badge.svg)](https://github.com/openapv/ffapv/actions/workflows/build.yml)

# ffapv — FFmpeg plus up-to-date codec integrations

This project delivers FFmpeg plus the latest codec integration work. Its
purpose is fast source distribution: improvements and fixes for the codec
wrappers are developed, reviewed and released here, so that users can pick
them up quickly without waiting for the next FFmpeg release cycle.

The maintained codec integrations are:

- **APV** — encoding through the [OpenAPV](https://github.com/AcademySoftwareFoundation/openapv) library, decoding through FFmpeg's native APV decoder
- **EVC** — encoding through [xeve](https://github.com/mpeg5/xeve), decoding through [xevd](https://github.com/mpeg5/xevd)

The FFmpeg code base itself is updated regularly from upstream FFmpeg, so the
tree stays close to current FFmpeg while carrying the newest codec patches on
top. Everything outside the integrations listed above works the same as in
pure FFmpeg. The project is intended to be registered as a project under the
[Academy Software Foundation](https://www.aswf.io/), alongside OpenAPV.

## Branches

| Branch | Content |
|---|---|
| `main` | The distribution branch: current FFmpeg base plus the latest codec patches. |
| `upstream` | Pristine FFmpeg, no local patches. Updated regularly from upstream FFmpeg and then merged into `main`. |
| feature branches | Work in progress, opened as pull requests against `main`. |

# APV (OpenAPV)

Pure FFmpeg currently has the OpenAPV encoder and a native APV decoder.
On top of that, this project provides **APV family/profile selection** for the
OpenAPV encoder (`-family` option: 422_LQ, 422_SQ, 422_HQ, 444_UQ), keeps the
wrapper in sync with the evolving OpenAPV library API, and carries encoder
fixes ahead of their arrival in FFmpeg releases.

## Encoding with liboapv

The APV encoder is selected with `-c:v liboapv`. Supported input pixel formats:
`gray10`, `yuv422p10`, `yuv422p12`, `yuv444p10`, `yuv444p12`, `yuva444p10`, `yuva444p12`.

Encoder options:

| Option | Values | Description |
|---|---|---|
| `-family` | `422_LQ`, `422_SQ`, `422_HQ`, `444_UQ` | APV family. Sets the target bitrate for the resolution/frame rate and switches to ABR rate control. The family must match the profile implied by the pixel format (422 families need a 4:2:2 input, `444_UQ` needs 4:4:4). |
| `-b:v` | e.g. `200M` | Target bitrate (ABR). Ignored when `-family` is used. |
| `-qp` | 0–63 for 10-bit, 0–75 for 12-bit input (default 32) | Quantization parameter for constant-QP mode. Used when neither `-b:v` nor `-family` is given. |
| `-preset` | `fastest`, `fast`, `medium`, `slow`, `placebo` | Speed/quality trade-off. |
| `-oapv-params` | `key=value:key=value` | Pass options directly to the OpenAPV library, e.g. `qp=30:tile-w=512`. See below. |

### OpenAPV parameters (`-oapv-params`)

`-oapv-params` forwards a `:`-separated list of `key=value` pairs to
`oapve_param_parse()` in the OpenAPV library. An invalid pair is reported as a
warning and skipped; valid pairs are still applied.

| Key | Values | Description |
|---|---|---|
| `profile` | `422-10`, `422-12`, `444-10`, `444-12`, `4444-10`, `4444-12`, `400-10`, `4444-16C12` | Override the profile (normally derived from the pixel format). |
| `level` | `auto`, `1`, `1.1`, … `7.1` | Level idc. |
| `band` | `auto`, `0`–`3` | Band idc. |
| `preset` | `fastest`, `fast`, `medium`, `slow`, `placebo` | Same as `-preset`. |
| `qp` | 0–63 (10-bit), 0–75 (12-bit) | Quantization parameter (CQP). |
| `qp-offset-c1` / `-c2` / `-c3` | integer | Per-component QP offset added to `qp`. |
| `bitrate` | e.g. `100000`, `100000k`, `100m` | Target bitrate in kbps; switches to ABR. |
| `q-matrix-c0` … `q-matrix-c3` | 64 space-separated values | Custom quantization matrix per component. |
| `tile-w`, `tile-h` | multiple of 16; min 256 / 128 | Tile size in pixels. |
| `color-primaries` | `bt709`, `bt470m`, `bt470bg`, `smpte170m`, `smpte240m`, … | Color primaries written into the bitstream. |
| `color-transfer` | `bt709`, … | Transfer characteristics. |
| `color-matrix` | `bt709`, … | Matrix coefficients. |
| `color-range` | `limited`/`tv`, `full`/`pc` | Full range flag. |

`width`, `height` and `fps` are also accepted but normally come from the input
stream; setting them here is only useful for special cases.

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

12-bit encoding (QP range extends to 75):

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p12 -qp 70 output.mp4

4:4:4:4 with alpha (4444 profile; APV families do not cover alpha profiles):

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuva444p10 -qp 30 output.mp4

Grayscale (400 profile):

    ffmpeg -i input.mov -c:v liboapv -pix_fmt gray10 output.mp4

MOV container for NLE workflows:

    ffmpeg -i input.mp4 -c:v liboapv -pix_fmt yuv422p10 -family 422_SQ output.mov

Re-wrap a raw .apv bitstream into MP4 without re-encoding:

    ffmpeg -i input.apv -c:v copy output.mp4

Fine-grained control through OpenAPV parameters:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -oapv-params "qp=30:tile-w=512:tile-h=512" output.mp4

Target bitrate and level through OpenAPV parameters:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -oapv-params "bitrate=50m:level=4.1" output.mp4

Chroma QP offsets:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -oapv-params "qp=30:qp-offset-c1=2:qp-offset-c2=2" output.mp4

Color description signalling:

    ffmpeg -i input.mov -c:v liboapv -pix_fmt yuv422p10 -oapv-params "color-primaries=bt709:color-transfer=bt709:color-matrix=bt709:color-range=full" output.mp4

Decode APV (FFmpeg's native APV decoder is used automatically):

    ffmpeg -i input.mp4 -pix_fmt yuv422p10 output.yuv

# EVC (xeve / xevd)

EVC (MPEG-5 Essential Video Coding) support uses FFmpeg's `libxeve` encoder
and `libxevd` decoder wrappers. They are enabled at configure time with
`--enable-libxeve` and `--enable-libxevd` (the xeve and xevd libraries must be
installed and visible to `pkg-config`).

The encoder is selected with `-c:v libxeve`. Supported input pixel formats:
`yuv420p`, `yuv420p10`. The main options are `-profile` (`baseline`, `main`),
`-preset` (`fast`, `medium`, `slow`, `placebo`), `-rc_mode` (`CQP`, `ABR`,
`CRF`) with `-qp` (0–51) or `-crf` (10–49), and `-xeve-params` for passing
`key=value` pairs directly to the xeve library.

## Examples

Encode to EVC with the main profile and a target bitrate:

    ffmpeg -i input.mov -c:v libxeve -profile main -b:v 5M output.mp4

Constant-QP encoding:

    ffmpeg -i input.mov -c:v libxeve -rc_mode CQP -qp 30 output.mp4

Decode EVC:

    ffmpeg -i input.mp4 output.yuv

# Building

Two components are built in order: the OpenAPV library
(https://github.com/AcademySoftwareFoundation/openapv, CMake), then FFmpeg
itself, which finds OpenAPV through `pkg-config` (`oapv.pc` is installed by
the OpenAPV build).

Common prerequisites: a C compiler, `make`, `cmake`, `pkg-config`, `git`, and
`nasm` for the x86 assembly optimizations (or configure FFmpeg with
`--disable-x86asm` to build without them).

## Linux

Install the build dependencies (Debian/Ubuntu):

    sudo apt-get install build-essential cmake git pkg-config nasm

Optional FFmpeg features pull in extra packages; the CI build
(`.github/workflows/build.yml`) installs the full list, including
`libsdl2-dev` for ffplay.

1. Build and install the OpenAPV library:

       git clone https://github.com/AcademySoftwareFoundation/openapv.git
       cmake -S openapv -B openapv/build -DCMAKE_BUILD_TYPE=Release
       cmake --build openapv/build -j$(nproc)
       sudo cmake --install openapv/build
       sudo ldconfig

2. Configure and build FFmpeg with the liboapv encoder enabled:

       git clone https://github.com/openapv/ffapv.git
       cd ffapv
       ./configure --enable-liboapv
       make -j$(nproc)

   If OpenAPV is installed in a non-default prefix, point pkg-config at it:

       PKG_CONFIG_PATH=/path/to/prefix/lib/pkgconfig ./configure --enable-liboapv

3. Verify the encoder is available:

       ./ffmpeg -h encoder=liboapv

## Windows (MinGW-w64)

Windows builds use the MinGW-w64 toolchain. FFmpeg's build system needs a
POSIX shell, which [MSYS2](https://www.msys2.org/) provides. Install MSYS2,
open the **MSYS2 MINGW64** shell and install the tools:

    pacman -S --needed base-devel git \
        mingw-w64-x86_64-toolchain \
        mingw-w64-x86_64-cmake \
        mingw-w64-x86_64-ninja \
        mingw-w64-x86_64-nasm \
        mingw-w64-x86_64-pkgconf

1. Build and install OpenAPV into the MinGW prefix:

       git clone https://github.com/AcademySoftwareFoundation/openapv.git
       cmake -S openapv -B openapv/build -G Ninja \
           -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/mingw64
       cmake --build openapv/build
       cmake --install openapv/build

2. Configure and build FFmpeg in the same shell:

       git clone https://github.com/openapv/ffapv.git
       cd ffapv
       ./configure --enable-liboapv
       make -j$(nproc)

3. The resulting `ffmpeg.exe` depends on `liboapv.dll` (in `/mingw64/bin`)
   and the MinGW runtime DLLs; keep them next to the executable or in `PATH`
   when running outside the MSYS2 shell.

Building with MSVC is possible (FFmpeg's `--toolchain=msvc` from an MSYS2
shell started inside a Visual Studio Developer Command Prompt, OpenAPV with
the Visual Studio CMake generator), but the MinGW-w64 route above is the
simpler and better-tested path.

The same MinGW-w64 toolchain also works from a Linux host if you prefer
cross-compiling: build OpenAPV with the toolchain file it ships
(`-DCMAKE_TOOLCHAIN_FILE=windows_x86_64_toolchain.cmake`), then configure
FFmpeg with `--enable-cross-compile --target-os=mingw32 --arch=x86_64
--cross-prefix=x86_64-w64-mingw32- --pkg-config=pkg-config` and point
`PKG_CONFIG_LIBDIR` / `--extra-cflags` / `--extra-ldflags` at the OpenAPV
install prefix.

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

# Contributing to ffapv

The section above describes upstream FFmpeg. **This repository is different:
contributions are made through GitHub pull requests.**

1. Fork the repository and create a feature branch from `main`.
2. Build and test your change (see the Building section above; `test/build.sh`
   and `test/run_test.sh` cover the basic encode/decode paths).
3. Keep commit subjects to a single line and sign off your commits
   (`git commit -s`).
4. Open a pull request against `main` with a short description of what the
   change does and how it was tested.

When opening a PR from a fork, please **enable "Allow edits from
maintainers"**. It lets maintainers rebase your branch or apply small review
fixes directly instead of going through another request/response round trip,
which can shorten the review cycle considerably.
