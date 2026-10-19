#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the static libraries the Windows release links into obs-airplay.dll:
# FFmpeg (decoders and demuxers the plugin uses, D3D11VA decoding, Schannel
# TLS so HTTPS uses Windows' certificate store), OpenSSL's libcrypto (UxPlay's
# pairing and FairPlay) and libplist. Nothing is shared: OBS ships its own
# FFmpeg DLLs, and ours must not be confused with them.
#
# Run in an MSYS2 UCRT64 shell (as CI does), or on Linux with a mingw-w64
# cross compiler: OBS_AIRPLAY_CROSS=x86_64-w64-mingw32 (posix threads).
#   scripts/build-windows-release-deps.sh
# Then configure with -DOBS_AIRPLAY_STATIC_DEPS=ON and
# PKG_CONFIG_PATH=<prefix>/lib/pkgconfig (the script prints both).
set -euo pipefail

CROSS="${OBS_AIRPLAY_CROSS:-}"
CACHE_ROOT="${OBS_AIRPLAY_CACHE_ROOT:-${HOME}/.cache}"
PREFIX="${OBS_AIRPLAY_DEPS_PREFIX:-${CACHE_ROOT}/obs-airplay-release-deps/windows-x64}"
SOURCE_CACHE="${OBS_AIRPLAY_SOURCE_CACHE:-${CACHE_ROOT}/obs-airplay-release-sources}"
WORK_DIR="${OBS_AIRPLAY_DEPS_WORK_DIR:-${TMPDIR:-/tmp}/obs-airplay-release-deps-work/windows-x64}"
JOBS="$(nproc 2>/dev/null || echo 4)"

# Same versions as scripts/build-macos-release-deps.sh.
FFMPEG_VERSION="8.1.2"
FFMPEG_ARCHIVE="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_ARCHIVE}"
FFMPEG_SHA256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"

LIBPLIST_VERSION="2.7.0"
LIBPLIST_ARCHIVE="libplist-${LIBPLIST_VERSION}.tar.bz2"
LIBPLIST_URL="https://github.com/libimobiledevice/libplist/releases/download/${LIBPLIST_VERSION}/${LIBPLIST_ARCHIVE}"
LIBPLIST_SHA256="7ac42301e896b1ebe3c654634780c82baa7cb70df8554e683ff89f7c2643eb8b"

OPENSSL_VERSION="3.6.3"
OPENSSL_ARCHIVE="openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/${OPENSSL_ARCHIVE}"
OPENSSL_SHA256="243a86649cf6f23eeb6a2ff2456e09e5d77dd9018a54d3d96b0c6bdd6ba6c7f1"

if [[ -n "${CROSS}" ]]; then
  export CC="${CROSS}-gcc-posix" CXX="${CROSS}-g++-posix" AR="${CROSS}-ar"
  export RANLIB="${CROSS}-ranlib" RC="${CROSS}-windres" WINDRES="${CROSS}-windres"
  HOST_ARGS=(--host="${CROSS}")
  FFMPEG_CROSS_ARGS=(--enable-cross-compile --cross-prefix="${CROSS}-" --target-os=mingw32
                     --arch=x86_64 --cc="${CC}" --cxx="${CXX}")
else
  export CC=gcc CXX=g++
  HOST_ARGS=()
  FFMPEG_CROSS_ARGS=()
fi

fetch_and_verify() {
  local archive="$1" url="$2" expected="$3"
  local path="${SOURCE_CACHE}/${archive}"
  if [[ ! -f "${path}" ]]; then
    echo "==> Downloading ${archive}"
    curl -L --fail --retry 3 -o "${path}.part" "${url}"
    mv "${path}.part" "${path}"
  fi
  local actual
  actual="$(sha256sum < "${path}" | cut -d' ' -f1)"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "error: SHA-256 mismatch for ${archive}" >&2
    echo "expected: ${expected}" >&2
    echo "actual:   ${actual}" >&2
    exit 1
  fi
}

mkdir -p "${SOURCE_CACHE}" "${WORK_DIR}" "${PREFIX}"
fetch_and_verify "${FFMPEG_ARCHIVE}" "${FFMPEG_URL}" "${FFMPEG_SHA256}"
fetch_and_verify "${LIBPLIST_ARCHIVE}" "${LIBPLIST_URL}" "${LIBPLIST_SHA256}"
fetch_and_verify "${OPENSSL_ARCHIVE}" "${OPENSSL_URL}" "${OPENSSL_SHA256}"

echo "==> Preparing verified sources"
rm -rf "${WORK_DIR:?}"/*
tar -xf "${SOURCE_CACHE}/${FFMPEG_ARCHIVE}" -C "${WORK_DIR}"
tar -xf "${SOURCE_CACHE}/${LIBPLIST_ARCHIVE}" -C "${WORK_DIR}"
tar -xf "${SOURCE_CACHE}/${OPENSSL_ARCHIVE}" -C "${WORK_DIR}"

# Each library is rebuilt when its recipe (the stamp) changes.
built() {  # built NAME FEATURES FILE
  [[ -f "${PREFIX}/$3" && "$(cat "${PREFIX}/.obs-airplay-$1" 2>/dev/null)" == "$2" ]]
}

OPENSSL_FEATURES="${OPENSSL_VERSION};libcrypto-static-v1"
if built openssl "${OPENSSL_FEATURES}" lib/libcrypto.a; then
  echo "==> Reusing OpenSSL ${OPENSSL_VERSION}"
else
  echo "==> Building OpenSSL ${OPENSSL_VERSION}"
  (
    cd "${WORK_DIR}/openssl-${OPENSSL_VERSION}"
    # UxPlay needs libcrypto only; no engines, modules or apps.
    ./Configure mingw64 no-shared no-module no-engine no-tests no-apps no-docs \
      --prefix="${PREFIX}" --libdir=lib
    make -j"${JOBS}" build_libs
    make install_dev
  )
  printf '%s' "${OPENSSL_FEATURES}" > "${PREFIX}/.obs-airplay-openssl"
fi

# NDEBUG: libplist's getters assert on strings with an embedded NUL, which a
# sender controls (as in the macOS build).
LIBPLIST_FEATURES="${LIBPLIST_VERSION};ndebug-static-v1"
if built libplist "${LIBPLIST_FEATURES}" lib/libplist-2.0.a; then
  echo "==> Reusing libplist ${LIBPLIST_VERSION}"
else
  echo "==> Building libplist ${LIBPLIST_VERSION}"
  (
    cd "${WORK_DIR}/libplist-${LIBPLIST_VERSION}"
    CFLAGS="-O2 -DNDEBUG" CXXFLAGS="-O2 -DNDEBUG" \
      ./configure "${HOST_ARGS[@]}" --prefix="${PREFIX}" \
        --disable-shared --enable-static --without-cython --without-tools --without-tests
    make -j"${JOBS}"
    make install
  )
  printf '%s' "${LIBPLIST_FEATURES}" > "${PREFIX}/.obs-airplay-libplist"
fi

# The macOS recipe with VideoToolbox swapped for D3D11VA (and DXVA2 for older
# drivers), Windows threads, and Schannel instead of OpenSSL for TLS.
FFMPEG_FEATURES="${FFMPEG_VERSION};d3d11va-schannel-w32threads-v1"
if built ffmpeg "${FFMPEG_FEATURES}" lib/libavformat.a; then
  echo "==> Reusing FFmpeg ${FFMPEG_VERSION}"
else
  echo "==> Building FFmpeg ${FFMPEG_VERSION}"
  (
    cd "${WORK_DIR}/ffmpeg-${FFMPEG_VERSION}"
    ./configure "${FFMPEG_CROSS_ARGS[@]}" \
      --prefix="${PREFIX}" \
      --pkg-config-flags=--static \
      --enable-static \
      --disable-shared \
      --disable-programs \
      --disable-doc \
      --disable-debug \
      --disable-autodetect \
      --disable-everything \
      --disable-avdevice \
      --disable-avfilter \
      --enable-w32threads \
      --enable-avformat \
      --enable-network \
      --enable-schannel \
      --enable-avcodec \
      --enable-avutil \
      --enable-swscale \
      --enable-swresample \
      --enable-decoder=aac,alac,h264,hevc,mp3 \
      --enable-d3d11va \
      --enable-dxva2 \
      --enable-hwaccel=h264_d3d11va,hevc_d3d11va,h264_d3d11va2,hevc_d3d11va2,h264_dxva2,hevc_dxva2 \
      --enable-parser=aac,h264,hevc,mpegaudio \
      --enable-demuxer=aac,hls,mov,mp3,mpegts \
      --enable-protocol=crypto,file,http,https,tcp,tls,udp
    make -j"${JOBS}"
    make install
  )
  printf '%s' "${FFMPEG_FEATURES}" > "${PREFIX}/.obs-airplay-ffmpeg"
fi

echo "==> Release dependencies are ready"
echo "Prefix: ${PREFIX}"
echo
echo "Configure with:"
echo "  PKG_CONFIG_PATH=\"${PREFIX}/lib/pkgconfig\" cmake ... -DOBS_AIRPLAY_STATIC_DEPS=ON \\"
echo "    -DOPENSSL_ROOT_DIR=\"${PREFIX}\""
