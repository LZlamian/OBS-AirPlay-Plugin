#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

ARCH="${ARCH_OVERRIDE:-$(uname -m)}"
MACOS_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET_OVERRIDE:-12.0}"
# Outside the source tree (build.sh and `rm -rf build` used to wipe them),
# and without spaces, which libtool cannot handle.
PREFIX="${OBS_AIRPLAY_DEPS_PREFIX:-${HOME}/Library/Caches/obs-airplay-release-deps/${ARCH}-macos${MACOS_DEPLOYMENT_TARGET}}"
SOURCE_CACHE="${OBS_AIRPLAY_SOURCE_CACHE:-${HOME}/Library/Caches/obs-airplay-release-sources}"
# In the user's own caches, not a fixed /tmp path another account could
# create first.
WORK_DIR="${OBS_AIRPLAY_DEPS_WORK_DIR:-${HOME}/Library/Caches/obs-airplay-release-deps-work/${ARCH}-macos${MACOS_DEPLOYMENT_TARGET}}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

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

case "${ARCH}" in
  arm64)
    OPENSSL_TARGET="darwin64-arm64-cc"
    ;;
  x86_64)
    OPENSSL_TARGET="darwin64-x86_64-cc"
    ;;
  *)
    echo "error: unsupported macOS architecture: ${ARCH}" >&2
    exit 1
    ;;
esac

fetch_and_verify() {
  local archive="$1"
  local url="$2"
  local expected="$3"
  local path="${SOURCE_CACHE}/${archive}"

  if [[ ! -f "${path}" ]]; then
    echo "==> Downloading ${archive}"
    /usr/bin/curl -L --fail --retry 3 -o "${path}" "${url}"
  fi

  local actual
  actual="$(/usr/bin/shasum -a 256 "${path}" | /usr/bin/awk '{print $1}')"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "error: SHA-256 mismatch for ${archive}" >&2
    echo "expected: ${expected}" >&2
    echo "actual:   ${actual}" >&2
    exit 1
  fi
}

mkdir -p "${SOURCE_CACHE}" "${WORK_DIR}" "${PREFIX}"
for dir in "${SOURCE_CACHE}" "${WORK_DIR}" "${PREFIX}"; do
  if [[ ! -O "${dir}" ]]; then
    echo "error: ${dir} is not owned by $(id -un)" >&2
    exit 1
  fi
done

fetch_and_verify "${FFMPEG_ARCHIVE}" "${FFMPEG_URL}" "${FFMPEG_SHA256}"
fetch_and_verify "${LIBPLIST_ARCHIVE}" "${LIBPLIST_URL}" "${LIBPLIST_SHA256}"
fetch_and_verify "${OPENSSL_ARCHIVE}" "${OPENSSL_URL}" "${OPENSSL_SHA256}"

echo "==> Preparing verified sources"
rm -rf \
  "${WORK_DIR}/ffmpeg-${FFMPEG_VERSION}" \
  "${WORK_DIR}/libplist-${LIBPLIST_VERSION}" \
  "${WORK_DIR}/openssl-${OPENSSL_VERSION}"
/usr/bin/tar -xf "${SOURCE_CACHE}/${FFMPEG_ARCHIVE}" -C "${WORK_DIR}"
/usr/bin/tar -xf "${SOURCE_CACHE}/${LIBPLIST_ARCHIVE}" -C "${WORK_DIR}"
/usr/bin/tar -xf "${SOURCE_CACHE}/${OPENSSL_ARCHIVE}" -C "${WORK_DIR}"

COMMON_CFLAGS="-O2 -arch ${ARCH} -mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET}"

# OPENSSLDIR is /etc/ssl so OpenSSL finds macOS's CA bundle (/etc/ssl/cert.pem);
# a prefix built with the old in-prefix openssldir is rebuilt.
if [[ -f "${PREFIX}/lib/libcrypto.a" && -f "${PREFIX}/lib/pkgconfig/openssl.pc" ]] &&
   /usr/bin/grep -aq 'OPENSSLDIR: "/etc/ssl"' "${PREFIX}/lib/libcrypto.a"; then
  echo "==> Reusing OpenSSL ${OPENSSL_VERSION}"
else
  echo "==> Building OpenSSL ${OPENSSL_VERSION}"
  (
    cd "${WORK_DIR}/openssl-${OPENSSL_VERSION}"
    MACOSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
      ./Configure "${OPENSSL_TARGET}" \
        "-mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET}" \
        no-shared no-tests no-apps no-docs \
        --prefix="${PREFIX}" --openssldir=/etc/ssl
    make -j"${JOBS}"
    make install_sw
  )
fi

# libplist is built with NDEBUG: its getters assert on strings with an
# embedded NUL, which a sender controls (the receiver also refuses such
# plists itself, uxplay/lib/plist_checked.h). The stamp rebuilds a prefix
# made before this change.
LIBPLIST_FEATURES="ndebug-v1"
LIBPLIST_STAMP="${PREFIX}/.obs-airplay-libplist-features"
if [[ -f "${PREFIX}/lib/libplist-2.0.a" && -f "${PREFIX}/lib/pkgconfig/libplist-2.0.pc" \
      && "$(cat "${LIBPLIST_STAMP}" 2>/dev/null)" == "${LIBPLIST_FEATURES}" ]]; then
  echo "==> Reusing libplist ${LIBPLIST_VERSION}"
else
  echo "==> Building libplist ${LIBPLIST_VERSION}"
  (
    cd "${WORK_DIR}/libplist-${LIBPLIST_VERSION}"
    CFLAGS="${COMMON_CFLAGS} -DNDEBUG" \
    CXXFLAGS="${COMMON_CFLAGS} -DNDEBUG" \
    LDFLAGS="-arch ${ARCH} -mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET}" \
      ./configure \
        --prefix="${PREFIX}" \
        --disable-shared \
        --enable-static \
        --without-cython \
        --without-tools \
        --without-tests
    make clean >/dev/null 2>&1 || true
    make -j"${JOBS}"
    make install
  )
  printf '%s' "${LIBPLIST_FEATURES}" > "${LIBPLIST_STAMP}"
fi

# The feature stamp makes a prefix built before a configure change (e.g.
# without VideoToolbox hardware decoding) rebuild instead of being reused.
FFMPEG_FEATURES="videotoolbox-hwaccel-v1;openssldir-etc-ssl"
FFMPEG_STAMP="${PREFIX}/.obs-airplay-ffmpeg-features"
if [[ -f "${PREFIX}/lib/libavformat.dylib" && -f "${PREFIX}/lib/pkgconfig/libavformat.pc" \
      && "$(cat "${FFMPEG_STAMP}" 2>/dev/null)" == "${FFMPEG_FEATURES}" ]]; then
  echo "==> Reusing FFmpeg ${FFMPEG_VERSION}"
else
  echo "==> Building FFmpeg ${FFMPEG_VERSION}"
  (
    cd "${WORK_DIR}/ffmpeg-${FFMPEG_VERSION}"
    MACOSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
      ./configure \
        --prefix="${PREFIX}" \
        --arch="${ARCH}" \
        --target-os=darwin \
        --cc=clang \
        --enable-cross-compile \
        --disable-static \
        --enable-shared \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-autodetect \
        --disable-everything \
        --disable-avdevice \
        --disable-avfilter \
        --enable-avformat \
        --enable-network \
        --enable-openssl \
        --enable-avcodec \
        --enable-avutil \
        --enable-swscale \
        --enable-swresample \
        --enable-decoder=aac,alac,h264,hevc,mp3 \
        --enable-videotoolbox \
        --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox \
        --enable-parser=aac,h264,hevc,mpegaudio \
        --enable-demuxer=aac,hls,mov,mp3,mpegts \
        --enable-protocol=crypto,file,http,https,tcp,tls,udp \
        --extra-cflags="${COMMON_CFLAGS} -I${PREFIX}/include" \
        --extra-ldflags="-arch ${ARCH} -mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET} -L${PREFIX}/lib"
    make -j"${JOBS}"
    make install
  )
  printf '%s' "${FFMPEG_FEATURES}" > "${FFMPEG_STAMP}"
fi

echo "==> Release dependencies are ready"
echo "Prefix: ${PREFIX}"
echo
echo "Package with:"
echo "  OBS_AIRPLAY_DEPS_PREFIX=\"${PREFIX}\" scripts/package-macos.sh"
