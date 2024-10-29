# FFapv
FFmpeg with OpenAPV (Open Advanced Professional Video Codec) support.

## Prequisities

1. Install in system OpenAPV codec (dev package).  
Detailed and latest info at https://github.com/openapv/openapv

2. Install required FFmpeg build dependencies

```
sudo apt-get update -qq && sudo apt-get -y install \
autoconf \
automake \
build-essential \
cmake \
pkg-config \
libsdl2-dev \ 
nasm
```

## Build
To enable OpenAPV in FFmpeg it need to be configured with flag ``--enable-apv``

### Sample static configuration

```
PATH="$HOME/bin:$PATH" PKG_CONFIG_PATH="$HOME/ffmpeg_build/lib/pkgconfig" ./configure \
--prefix="$HOME/ffmpeg_build" \
--pkg-config-flags="--static" \
--extra-cflags="-I$HOME/ffmpeg_build/include" \
--extra-ldflags="-L$HOME/ffmpeg_build/lib" \
--extra-libs="-lpthread -lm" \
--bindir="$HOME/bin" \
--enable-ffplay \
--enable-apv
```

### Shared configuration

For shared build, FFmpeg need to be configured with flag ``--enable-shared`` and removed ``--pkg-config-flags="--static"``.

### Compilation & installation

```
PATH="$HOME/bin:$PATH" make -j $(nproc)
make install
```

## References

[OpenAPV](https://github.com/openapv/openapv)  
[FFmpeg](https://trac.ffmpeg.org/wiki/CompilationGuide/Ubuntu)
