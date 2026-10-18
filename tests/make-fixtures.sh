#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Generates the media the smoke tests play (ctest runs this first).
#   tests/make-fixtures.sh FFMPEG OUT_DIR
set -euo pipefail
FFMPEG="$1"
OUT="$2"
mkdir -p "${OUT}/hls" "${OUT}/tls"
cd "${OUT}"
gen() {  # gen FILE ffmpeg-args...
  local file="$1"; shift
  [[ -s "${file}" ]] || "${FFMPEG}" -v error -y "$@" "${file}"
}
AV=(-f lavfi -i testsrc=size=640x360:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000)
# 60 s, 1 s GOP: seek, scrub and decoder tests
gen clip.mp4 "${AV[@]}" -t 60 -c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac -movflags +faststart
# 8 s: modes that play to the end within 15 s
gen clip8.mp4 "${AV[@]}" -t 8 -c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac -movflags +faststart
# 60 s with keyframes 10 s apart: pre-roll in --seek-sync matters
gen clip-g300.mp4 "${AV[@]}" -t 60 -c:v libx264 -pix_fmt yuv420p -g 300 -keyint_min 300 \
  -sc_threshold 0 -c:a aac -movflags +faststart
# 90 s: --follow-clock
gen clip90.mp4 "${AV[@]}" -t 90 -c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac -movflags +faststart
# Portrait recording (rotation in the metadata): played upright
[[ -s rot90.mp4 ]] || "${FFMPEG}" -v error -y -display_rotation 90 -i clip8.mp4 -c copy rot90.mp4
# MPEG-TS starting at 10 s: --ts-seek
gen clip-ts.ts -f lavfi -i testsrc=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 \
  -t 30 -c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac -output_ts_offset 10 -f mpegts
# No B-frames (like a mirror stream), BT.601 and BT.709: decoder tests
gen clip601.mp4 -f lavfi -i testsrc=size=640x360:rate=30 -t 3 -c:v libx264 -bf 0 -g 30 \
  -pix_fmt yuv420p -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m
gen clip709.mp4 -f lavfi -i testsrc=size=1280x720:rate=30 -t 3 -c:v libx264 -bf 0 -g 30 \
  -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709
# Stills: a two-frame 1 fps clip and a 0.5 fps one
gen still2.mp4 -f lavfi -i testsrc=size=640x360:rate=1 -t 2 -c:v libx264 -pix_fmt yuv420p -r 1 \
  -movflags +faststart
gen sparse.mp4 -f lavfi -i testsrc=size=640x360:rate=0.5 -t 6 -c:v libx264 -pix_fmt yuv420p \
  -r 0.5 -movflags +faststart
# HLS for --nested-policy: segments on the playlist's own server, and the
# same playlist pointing at a second server
if [[ ! -s hls/same.m3u8 ]]; then
  "${FFMPEG}" -v error -y -i clip8.mp4 -c copy -f hls -hls_time 2 -hls_list_size 0 \
    -hls_segment_filename 'hls/seg%d.ts' hls/same.m3u8
fi
sed -E 's#^(seg[0-9]+\.ts)$#http://127.0.0.1:18902/\1#' hls/same.m3u8 > hls/cross.m3u8
# Self-signed certificate for --tls-verify
if [[ ! -s tls/cert.pem ]]; then
  /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout tls/key.pem -out tls/cert.pem \
    -days 30 -subj /CN=localhost 2>/dev/null
fi
cp -f clip8.mp4 tls/clip.mp4
echo "fixtures ready in ${OUT}"
