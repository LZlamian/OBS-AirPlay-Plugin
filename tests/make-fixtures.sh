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
# The decoder tests' streams as Annex B too: a build against the release
# FFmpeg (no bitstream filters) reads these instead of the MP4s
for clip in clip clip601 clip709; do
  gen "${clip}.h264" -i "${clip}.mp4" -map 0:v -c copy -bsf:v h264_mp4toannexb -f h264
done
# Audio only (AAC): an item with no picture
gen audio.m4a -f lavfi -i sine=frequency=440:sample_rate=48000 -t 4 -c:a aac
# 10-bit H.264, which VideoToolbox does not decode: the software fallback
gen clip10.mp4 -f lavfi -i testsrc=size=640x360:rate=30 -t 3 -c:v libx264 -profile:v high10 \
  -pix_fmt yuv420p10le
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
CROSS_PORT=$(( ${OBS_AIRPLAY_TEST_PORT_BASE:-18900} + 2 ))  # tests/with-media-servers.sh
sed -E "s#^(seg[0-9]+\.ts)\$#http://127.0.0.1:${CROSS_PORT}/\1#" hls/same.m3u8 > hls/cross.m3u8
# A live playlist (no end) and two renditions: the 800 kb/s one is chosen
grep -v 'EXT-X-ENDLIST' hls/same.m3u8 > hls/live.m3u8
cp -f hls/same.m3u8 hls/high.m3u8
printf '#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1280x720\nhigh.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\nsame.m3u8\n' \
  > hls/multi.m3u8
# AES-128 segments with the key next to the playlist
if [[ ! -s hls/aes.m3u8 ]]; then
  /usr/bin/openssl rand 16 > hls/aes.key
  printf 'aes.key\n%s/hls/aes.key\n' "$(pwd)" > hls/aes.keyinfo
  "${FFMPEG}" -v error -y -i clip8.mp4 -c copy -f hls -hls_time 2 -hls_list_size 0 \
    -hls_key_info_file hls/aes.keyinfo -hls_segment_filename 'hls/aes%d.ts' hls/aes.m3u8
fi
# Self-signed certificate for --tls-verify
if [[ ! -s tls/cert.pem ]]; then
  /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout tls/key.pem -out tls/cert.pem \
    -days 30 -subj /CN=localhost 2>/dev/null
fi
cp -f clip8.mp4 tls/clip.mp4
# Fuzz seeds (OBS_AIRPLAY_FUZZ): a few Annex B access units, behind the
# byte that picks the codec (0: H.264, 1: HEVC)
if [[ ! -s fuzz-h264.bin ]]; then
  { printf '\x00'; "${FFMPEG}" -v error -i clip601.mp4 -frames:v 3 -c copy \
      -bsf:v h264_mp4toannexb -f h264 -; } > fuzz-h264.bin
fi
if [[ ! -s fuzz-hevc.bin ]]; then
  { printf '\x01'; "${FFMPEG}" -v error -f lavfi -i testsrc=size=320x240:rate=30 -frames:v 3 \
      -c:v libx265 -x265-params log-level=error -f hevc -; } > fuzz-hevc.bin
fi
echo "fixtures ready in ${OUT}"
