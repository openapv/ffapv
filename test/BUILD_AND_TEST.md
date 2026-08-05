# FFmpeg 9.0 + openapv 빌드 및 테스트 방법

## 구성

| 항목 | 위치 |
|---|---|
| FFmpeg 소스 | `~/src/ffapv/code` (브랜치 `update/ffmpeg-9.0`, FFmpeg n9.0 태그와 동일) |
| openapv 소스 | `~/src/openapv/code` (**자주 변경됨 → 매 빌드마다 다시 읽음**) |
| openapv 빌드 | `test/oapv-bld` (out-of-source, 소스 트리에 흔적 없음) |
| FFmpeg 빌드 | `test/bld` (out-of-tree) |
| 테스트 결과물 | `test/out` |

`~/src/openapv/code`, `~/src/openapv/test` 에는 어떤 파일도 생성하지 않는다.

## 빌드

```bash
bash test/build.sh
```

스크립트가 하는 일:

1. `~/src/openapv/code/inc/oapv.h` 에서 버전(`OAPV_VER_*`)을 매번 새로 읽음
2. openapv를 `test/oapv-bld` 에 cmake out-of-source 빌드
3. FFmpeg이 요구하는 `<oapv/oapv.h>` 레이아웃으로 헤더 스테이징
   (`test/oapv-bld/include/oapv/oapv.h` → 소스 헤더 심볼릭 링크,
   `oapv_exports.h` 는 cmake 빌드 시 자동 생성)
4. `test/oapv-bld/pkgconfig/oapv.pc` 생성 (버전은 소스에서 매번 추출)
5. FFmpeg configure (최초 1회):
   ```
   PKG_CONFIG_PATH=test/oapv-bld/pkgconfig ../../configure \
     --enable-ffplay --enable-liboapv \
     --extra-cflags="-I<test/oapv-bld>/include" \
     --extra-ldflags="-L<test/oapv-bld>/lib -Wl,-rpath,<test/oapv-bld>/lib"
   ```
   - rpath를 박아 두어 실행 시 `LD_LIBRARY_PATH` 설정 불필요
6. `make -j$(nproc)`

### FFmpeg configure를 다시 하고 싶을 때

```bash
rm test/bld/config.h
bash test/build.sh
```

### 완전 초기화 후 재빌드

```bash
rm -rf test/bld test/oapv-bld
bash test/build.sh
```

## 테스트

```bash
bash test/run_test.sh
```

수행 내용: liboapv 인코더 정보 출력 → testsrc2(1280x720, 60프레임) APV 인코딩
→ 디코딩 → MP4 컨테이너 왕복 → 스트림 정보 및 디코딩 크기 검증.
결과물은 `test/out/` 에 생성된다.

## 주의 사항

- openapv 소스가 변경되면 `bash test/build.sh` 만 다시 실행하면 된다.
  openapv는 매번 재빌드되고, FFmpeg는 `make`로 변경분만 다시 컴파일/링크된다.
- FFmpeg 9.0의 configure는 `oapv >= 0.2.0.0` 을 요구한다.
  현재 `~/src/openapv/code` 의 버전은 1.0.1.0 이므로 조건을 만족한다.
  (시스템에 설치된 `/usr/local/lib/liboapv.so`(0.1.13.1)는 사용하지 않는다.)
