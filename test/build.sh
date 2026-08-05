#!/usr/bin/env bash
# =============================================================================
# FFmpeg 9.0 + openapv 빌드 스크립트
#
# - openapv 소스: ~/src/openapv/code (자주 변경되므로 매 빌드마다 다시 읽음)
# - openapv 빌드: ./test/oapv-bld (out-of-source, 소스 트리에 흔적 없음)
# - FFmpeg 빌드:  ./test/bld (out-of-tree)
# - ~/src/openapv/code, ~/src/openapv/test 에는 어떤 파일도 생성하지 않음
#
# 사용법:  bash test/build.sh
# FFmpeg configure를 다시 하려면: rm test/bld/config.h 후 재실행
# =============================================================================
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FF_SRC="$(cd "$TEST_DIR/.." && pwd)"
OAPV_SRC="$HOME/src/openapv/code"
OAPV_BLD="$TEST_DIR/oapv-bld"
FF_BLD="$TEST_DIR/bld"
JOBS="$(nproc)"

# --- 1) openapv 버전을 소스에서 매번 새로 읽음 -------------------------------
ver() { grep -E "#define OAPV_VER_$1[[:space:]]" "$OAPV_SRC/inc/oapv.h" | grep -oE "[0-9]+"; }
OAPV_VERSION="$(ver APISET).$(ver MAJOR).$(ver MINOR).$(ver PATCH)"
echo "== [1/6] openapv 소스 버전: $OAPV_VERSION ($OAPV_SRC)"

# --- 2) openapv out-of-source 빌드 -------------------------------------------
echo "== [2/6] openapv 빌드 -> $OAPV_BLD"
cmake -S "$OAPV_SRC" -B "$OAPV_BLD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$OAPV_BLD" -j"$JOBS"

# --- 3) FFmpeg이 요구하는 <oapv/oapv.h> 레이아웃 스테이징 ---------------------
#    소스의 inc/oapv.h 를 심볼릭 링크 (소스 변경 시 자동으로 최신 내용 참조)
#    oapv_exports.h 는 cmake 빌드 시 $OAPV_BLD/include/oapv/ 에 생성됨
echo "== [3/6] 헤더 스테이징"
mkdir -p "$OAPV_BLD/include/oapv"
ln -sf "$OAPV_SRC/inc/oapv.h" "$OAPV_BLD/include/oapv/oapv.h"

# --- 4) pkg-config 파일 생성 (버전은 매번 소스에서 추출) ----------------------
echo "== [4/6] oapv.pc 생성"
mkdir -p "$OAPV_BLD/pkgconfig"
cat > "$OAPV_BLD/pkgconfig/oapv.pc" <<EOF
prefix=$OAPV_BLD
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: oapv
Description: Advanced Professional Video Codec (local build from $OAPV_SRC)
Version: $OAPV_VERSION
Libs: -L\${libdir} -loapv
Cflags: -I\${includedir}
EOF

# --- 5) FFmpeg configure (최초 1회만; 재설정하려면 $FF_BLD/config.h 삭제) -----
echo "== [5/6] FFmpeg configure -> $FF_BLD"
mkdir -p "$FF_BLD"
cd "$FF_BLD"
if [ ! -f config.h ]; then
  PKG_CONFIG_PATH="$OAPV_BLD/pkgconfig" "$FF_SRC/configure" \
    --enable-ffplay \
    --enable-liboapv \
    --extra-cflags="-I$OAPV_BLD/include" \
    --extra-ldflags="-L$OAPV_BLD/lib -Wl,-rpath,$OAPV_BLD/lib"
else
  echo "   (기존 config.h 재사용 — 재configure하려면 config.h 삭제 후 실행)"
fi

# --- 6) FFmpeg 빌드 -----------------------------------------------------------
echo "== [6/6] FFmpeg 빌드"
make -j"$JOBS"
echo "== 완료: $FF_BLD/ffmpeg"
