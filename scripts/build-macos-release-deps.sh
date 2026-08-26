#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${ARCH_OVERRIDE:-$(uname -m)}"
MACOS_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET_OVERRIDE:-12.0}"
PREFIX="${OBS_AIRPLAY_DEPS_PREFIX:-${ROOT_DIR}/build/release-deps/${ARCH}-macos${MACOS_DEPLOYMENT_TARGET}}"
SOURCE_CACHE="${OBS_AIRPLAY_SOURCE_CACHE:-${ROOT_DIR}/build/release-sources}"
WORK_DIR="${OBS_AIRPLAY_DEPS_WORK_DIR:-${TMPDIR:-/tmp}/obs-airplay-release-deps-work/${ARCH}-macos${MACOS_DEPLOYMENT_TARGET}}"
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

if [[ -f "${PREFIX}/lib/libcrypto.a" && -f "${PREFIX}/lib/pkgconfig/openssl.pc" ]]; then
  echo "==> Reusing OpenSSL ${OPENSSL_VERSION}"
else
  echo "==> Building OpenSSL ${OPENSSL_VERSION}"
  (
    cd "${WORK_DIR}/openssl-${OPENSSL_VERSION}"
    MACOSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
      ./Configure "${OPENSSL_TARGET}" \
        "-mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET}" \
        no-shared no-tests no-apps no-docs \
        --prefix="${PREFIX}" --openssldir="${PREFIX}/ssl"
    make -j"${JOBS}"
    make install_sw
  )
fi

if [[ -f "${PREFIX}/lib/libplist-2.0.a" && -f "${PREFIX}/lib/pkgconfig/libplist-2.0.pc" ]]; then
  echo "==> Reusing libplist ${LIBPLIST_VERSION}"
else
  echo "==> Building libplist ${LIBPLIST_VERSION}"
  (
    cd "${WORK_DIR}/libplist-${LIBPLIST_VERSION}"
    CFLAGS="${COMMON_CFLAGS}" \
    CXXFLAGS="${COMMON_CFLAGS}" \
    LDFLAGS="-arch ${ARCH} -mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET}" \
      ./configure \
        --prefix="${PREFIX}" \
        --disable-shared \
        --enable-static \
        --without-cython \
        --without-tools \
        --without-tests
    make -j"${JOBS}"
    make install
  )
fi

if [[ -f "${PREFIX}/lib/libavcodec.dylib" && -f "${PREFIX}/lib/pkgconfig/libavcodec.pc" ]]; then
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
        --disable-avformat \
        --disable-network \
        --enable-avcodec \
        --enable-avutil \
        --enable-swscale \
        --enable-swresample \
        --enable-decoder=aac,alac,h264,hevc \
        --enable-parser=aac,h264,hevc \
        --extra-cflags="${COMMON_CFLAGS}" \
        --extra-ldflags="-arch ${ARCH} -mmacosx-version-min=${MACOS_DEPLOYMENT_TARGET}"
    make -j"${JOBS}"
    make install
  )
fi

echo "==> Release dependencies are ready"
echo "Prefix: ${PREFIX}"
echo
echo "Package with:"
echo "  OBS_AIRPLAY_DEPS_PREFIX=\"${PREFIX}\" scripts/package-macos.sh"
