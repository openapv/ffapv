#!/usr/bin/env bash
# =============================================================================
# APV 인코딩/디코딩 테스트 (실험은 ./test/out 에서 수행)
#
# 사용법:  bash test/run_test.sh
# =============================================================================
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG="$TEST_DIR/bld/ffmpeg"
FFPROBE="$TEST_DIR/bld/ffprobe"
OUT="$TEST_DIR/out"
mkdir -p "$OUT"

W=1280; H=720; FRAMES=60

echo "== [1/5] liboapv 인코더 정보"
"$FFMPEG" -hide_banner -h encoder=liboapv | head -15

echo "== [2/5] APV 인코딩 (testsrc2 ${W}x${H} ${FRAMES}f -> out.apv)"
"$FFMPEG" -hide_banner -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=30" \
  -frames:v "$FRAMES" -pix_fmt yuv420p -c:v liboapv "$OUT/out.apv"

echo "== [3/5] APV 디코딩 (out.apv -> dec.yuv)"
"$FFMPEG" -hide_banner -y -i "$OUT/out.apv" -c:v rawvideo -pix_fmt yuv420p "$OUT/dec.yuv"

echo "== [4/5] MP4 컨테이너 왕복 (out.mp4 -> dec_mp4.yuv)"
"$FFMPEG" -hide_banner -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=30" \
  -frames:v "$FRAMES" -pix_fmt yuv420p -c:v liboapv "$OUT/out.mp4"
"$FFMPEG" -hide_banner -y -i "$OUT/out.mp4" -c:v rawvideo -pix_fmt yuv420p "$OUT/dec_mp4.yuv"

echo "== [5/5] 스트림 정보"
"$FFPROBE" -hide_banner -v error -select_streams v -show_streams "$OUT/out.apv" \
  | grep -E "^(codec_name|width|height|pix_fmt|nb_frames)=" || true

EXPECTED=$((W * H * 3 / 2 * FRAMES))
ACTUAL=$(stat -c%s "$OUT/dec.yuv")
echo "   dec.yuv 크기: $ACTUAL (기대: $EXPECTED)"
[ "$ACTUAL" = "$EXPECTED" ]
echo "== 테스트 통과"
