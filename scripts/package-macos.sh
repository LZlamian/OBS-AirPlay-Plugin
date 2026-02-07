#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DIST_DIR="${ROOT_DIR}/dist"
STAGE_DIR="${BUILD_DIR}/pkg-root"

PLUGIN_NAME="obs-airplay.plugin"
PLUGIN_BUNDLE="${BUILD_DIR}/${PLUGIN_NAME}"
INSTALL_BASE="/Library/Application Support/obs-studio/plugins"
INSTALL_PATH="${INSTALL_BASE}/${PLUGIN_NAME}"

PACKAGE_ID="com.obsairplay.plugin"
VERSION="$(sed -nE 's/^project\(obs-airplay VERSION ([0-9]+\.[0-9]+\.[0-9]+)\).*/\1/p' "${ROOT_DIR}/CMakeLists.txt" | head -n1)"
VERSION="${VERSION:-1.0.0}"
ARCH="${ARCH_OVERRIDE:-$(uname -m)}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

OBS_LIB="/Applications/OBS.app/Contents/Frameworks/libobs.framework/libobs"
FFMPEG_LIBDIR="$(pkg-config --variable=libdir libavcodec 2>/dev/null || true)"
PLIST_LIBDIR="$(pkg-config --variable=libdir libplist-2.0 2>/dev/null || true)"
OPENSSL_PREFIX="$(pkg-config --variable=prefix openssl 2>/dev/null || true)"
if [[ -z "${OPENSSL_PREFIX}" ]] && command -v brew >/dev/null 2>&1; then
  OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || true)"
fi

check_arch_support() {
  local target_arch="$1"
  local missing=0

  local required_files=(
    "${OBS_LIB}"
    "${FFMPEG_LIBDIR}/libavcodec.dylib"
    "${FFMPEG_LIBDIR}/libavformat.dylib"
    "${FFMPEG_LIBDIR}/libavutil.dylib"
    "${FFMPEG_LIBDIR}/libswscale.dylib"
    "${FFMPEG_LIBDIR}/libswresample.dylib"
    "${PLIST_LIBDIR}/libplist-2.0.dylib"
  )
  if [[ -n "${OPENSSL_PREFIX}" ]]; then
    required_files+=(
      "${OPENSSL_PREFIX}/lib/libcrypto.dylib"
      "${OPENSSL_PREFIX}/lib/libssl.dylib"
    )
  fi

  echo "==> Verifying dependency architecture support for ${target_arch}"
  for f in "${required_files[@]}"; do
    [[ -z "${f}" ]] && continue
    if [[ ! -f "${f}" ]]; then
      echo "warning: dependency not found: ${f}"
      continue
    fi
    local info
    info="$(lipo -info "${f}" 2>/dev/null || true)"
    if [[ -z "${info}" ]]; then
      info="$(file "${f}" 2>/dev/null || true)"
    fi
    if [[ "${info}" != *"${target_arch}"* ]]; then
      echo "error: ${f} does not contain architecture ${target_arch}"
      echo "${info}"
      missing=1
    fi
  done

  if [[ "${missing}" -ne 0 ]]; then
    cat <<EOF

Cannot build for architecture '${target_arch}' on this machine with current dependencies.
Install ${target_arch}-compatible dependencies and a ${target_arch}-compatible OBS libobs, or build that arch on a matching host.
EOF
    exit 2
  fi
}

ZIP_NAME="obs-airplay-v${VERSION}-macos-${ARCH}.zip"
PKG_NAME="obs-airplay-v${VERSION}-macos-${ARCH}.pkg"

echo "==> Packaging OBS AirPlay plugin"
echo "Root:    ${ROOT_DIR}"
echo "Build:   ${BUILD_DIR}"
echo "Dist:    ${DIST_DIR}"
echo "Version: ${VERSION}"
echo "Arch:    ${ARCH}"
echo "Jobs:    ${JOBS}"

IFS=';' read -r -a ARCH_LIST <<< "${ARCH}"
for a in "${ARCH_LIST[@]}"; do
  check_arch_support "${a}"
done

mkdir -p "${BUILD_DIR}" "${DIST_DIR}"

echo "==> Configuring build"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="${ARCH}"

echo "==> Building plugin"
cmake --build "${BUILD_DIR}" --config Release -j"${JOBS}"

if [[ ! -d "${PLUGIN_BUNDLE}" ]]; then
  echo "error: plugin bundle not found at ${PLUGIN_BUNDLE}" >&2
  exit 1
fi

echo "==> Creating zip artifact"
rm -f "${DIST_DIR}/${ZIP_NAME}"
(
  cd "${BUILD_DIR}"
  /usr/bin/ditto -c -k --sequesterRsrc --keepParent "${PLUGIN_NAME}" "${DIST_DIR}/${ZIP_NAME}"
)

echo "==> Creating pkg staging layout"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}${INSTALL_BASE}"
cp -R "${PLUGIN_BUNDLE}" "${STAGE_DIR}${INSTALL_BASE}/"

echo "==> Building pkg installer"
rm -f "${DIST_DIR}/${PKG_NAME}"
pkgbuild \
  --root "${STAGE_DIR}" \
  --identifier "${PACKAGE_ID}" \
  --version "${VERSION}" \
  --install-location "/" \
  "${DIST_DIR}/${PKG_NAME}"

echo
echo "Artifacts created:"
echo "  ${DIST_DIR}/${ZIP_NAME}"
echo "  ${DIST_DIR}/${PKG_NAME}"
echo
echo "Install notes:"
echo "  - Zip: unpack and copy ${PLUGIN_NAME} into ~/Library/Application Support/obs-studio/plugins/"
echo "  - Pkg: install system-wide into ${INSTALL_PATH}"
echo
echo "Tip: set ARCH_OVERRIDE=x86_64 or ARCH_OVERRIDE=\"arm64;x86_64\" before running to build other architectures."
