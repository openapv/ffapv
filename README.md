# FFapv
[OpenAPV](https://github.com/AcademySoftwareFoundation/openapv) video codec integration with FFmpeg.

# Build

* Prerequisities
    * Install required FFmpeg dependencies [Guide](https://trac.ffmpeg.org/#CompilingFFmpeg)
    * Install in system [OpenAPV latest release](https://github.com/AcademySoftwareFoundation/openapv/releases/latest)
* Compilation
    ```
    ./configure --extra-libs="-lpthread -lm" --enable-ffplay --enable-liboapv
    make -j$(nproc)
    sudo make install
    ```

# Usage examples

All conversions can be used like any other codec in ffmpeg. Below are examples how to use **apv** in FFmpeg. This is just exaples and it can be done in a different way accoriding to FFmpeg manual.


### Generate raw yuv and y4m file for input testing

```
./ffmpeg -f lavfi -i testsrc=size=640x480:rate=30 -pix_fmt yuv422p10le -t 10 test_yuv422p10le_640x480.yuv
./ffmpeg -f lavfi -i testsrc=size=640x480:rate=30 -strict -1 -pix_fmt yuv422p10le -t 10 test_yuv422p10le_640x480.y4m
```

### APV Encoding
 
Select apv codec in ffmpeg `-c:v liboapv`

raw file
```
./ffmpeg -f rawvideo -pixel_format yuv422p10le -video_size 640x480 -i test_yuv422p10le_640x480.yuv -c:v liboapv test_yuv422p10le_640x480.apv
```

mp4
```
./ffmpeg -f rawvideo -pixel_format yuv422p10le -video_size 640x480 -i test_yuv422p10le_640x480.yuv -c:v liboapv test_yuv422p10le_640x480.apv.mp4
```

### Playing apv video

```
./ffplay test_yuv422p10le_640x480.apv.mp4 -autoexit
```

## Change encoding parameters

#### Bitrate
Bitrate can be changed by using `-b:v [value]`
```
./ffmpeg -f rawvideo -pixel_format yuv422p10le -video_size 640x480 -i test_yuv422p10le_640x480.yuv -c:v liboapv -b:v 10M test_yuv422p10le_640x480_10M.apv.mp4
```

#### Other parameters
Rate control `-rc-type [ABR(default)/CQP]`  
`-qp [value]`

```
./ffmpeg -f rawvideo -pixel_format yuv422p10le -video_size 640x480 -i test_yuv422p10le_640x480.yuv -c:v liboapv -rc-type CQP -qp 50 test_yuv422p10le_640x480_CQP_qp50.apv.mp4
```

#### Threads

Use `-oapv-params:threads=4` because ffmpeg `-threads` mechanism isn't compatible with liboapv internal threads management. 

## Conversions
To use **mp4** container just add mp4 file extension to output file.  

apv -> y4m
```
./ffmpeg -i test_yuv422p10le_640x480.apv -strict -1 -f yuv4mpegpipe test_yuv422p10le_640x480.y4m
```

h264 -> apv
```
./ffmpeg -i test_yuv422p10le_640x480.h264.mp4 -c:v liboapv test_yuv422p10le_640x480.apv.mp4
```

apv -> h264
```
./ffmpeg -i test_yuv422p10le_640x480.apv.mp4 -c:v libx264 test_yuv422p10le_640x480.h264.mp4
```

