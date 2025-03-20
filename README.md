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

### Parameters available as ffmpeg options

All the parametes for the APV encoder that available as ffmpeg options can be listed by executing the following command:
```
ffmpeg --help encoder=liboapv
```

For example bitrate can be changed by using `-b:v [value]` as follows:
```
./ffmpeg -f rawvideo -pixel_format yuv422p10le -video_size 640x480 -i test_yuv422p10le_640x480.yuv -c:v liboapv -b:v 10M test_yuv422p10le_640x480_10M.apv.mp4
```

And rate control `-rc-type [ABR(default)/CQP]`  type as well as qp parameter `-qp [value]` can be changed as follows:
```
./ffmpeg -f rawvideo -pixel_format yuv422p10le -video_size 640x480 -i test_yuv422p10le_640x480.yuv -c:v liboapv -rc-type CQP -qp 50 test_yuv422p10le_640x480_CQP_qp50.apv.mp4
```

### Other parameters

All the other options provided by the `liboapv` library can be passed using the `oapv-params` parameter in the ffmpeg command.  
These options can be specified after the `oapv-params` parameter  as a colon-separated list in the format `key=value`.

Please see example below:
```
ffmpeg -f rawvideo -pix_fmt yuv422p10le -i ${INPUT_FILE} -c:v liboapv -oapv-params "profile=422-10:level=7.1:band=3:preset=medium:width=352:height=288:fps=24:qp=63:bitrate=1M" -f rawvideo ${OUTPUT_FILE}
```
All the options that can be passed as a colon-separated list in the format `key=value` right next to the `oapv-params` fparameter in the ffmpeg command can be listed by running the reference application `oapv_app_enc`, which is provided with the `liboapv` library as follows:

```
oapv_app_enc --help
```

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

