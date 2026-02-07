#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DIST_DIR="${ROOT_DIR}/dist"
STAGE_DIR="${BUILD_DIR}/pkg-root"
PKG_SCRIPTS_DIR="${BUILD_DIR}/pkg-scripts"

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

contains_item() {
  local needle="$1"
  shift
  local item
  for item in "$@"; do
    if [[ "${item}" == "${needle}" ]]; then
      return 0
    fi
  done
  return 1
}

collect_non_system_deps() {
  local file="$1"
  otool -L "${file}" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/homebrew|/usr/local)/' || true
}

vendor_runtime_deps() {
  local main_bin="${PLUGIN_BUNDLE}/Contents/MacOS/obs-airplay.so"
  local frameworks_dir="${PLUGIN_BUNDLE}/Contents/Frameworks"
  local queue=()
  local seen=()
  local copied=()
  local dep=""
  local lib=""

  if [[ ! -f "${main_bin}" ]]; then
    echo "error: plugin binary not found at ${main_bin}" >&2
    exit 1
  fi

  rm -rf "${frameworks_dir}"
  mkdir -p "${frameworks_dir}"

  while IFS= read -r dep; do
    [[ -n "${dep}" ]] && queue+=("${dep}")
  done < <(collect_non_system_deps "${main_bin}")

  while [[ ${#queue[@]} -gt 0 ]]; do
    lib="${queue[0]}"
    queue=("${queue[@]:1}")

    if contains_item "${lib}" "${seen[@]-}"; then
      continue
    fi
    seen+=("${lib}")

    if [[ ! -f "${lib}" ]]; then
      echo "warning: dependency not found (skipping): ${lib}"
      continue
    fi

    local dst="${frameworks_dir}/$(basename "${lib}")"
    cp -fL "${lib}" "${dst}"
    copied+=("${dst}")

    while IFS= read -r dep; do
      [[ -n "${dep}" ]] && queue+=("${dep}")
    done < <(collect_non_system_deps "${lib}")
  done

  # Rewrite the plugin binary to load vendored libraries from Contents/Frameworks.
  while IFS= read -r dep; do
    [[ -z "${dep}" ]] && continue
    local base="$(basename "${dep}")"
    local local_lib="${frameworks_dir}/${base}"
    if [[ -f "${local_lib}" ]]; then
      install_name_tool -change "${dep}" "@loader_path/../Frameworks/${base}" "${main_bin}"
    fi
  done < <(otool -L "${main_bin}" | tail -n +2 | awk '{print $1}')

  # Rewrite vendored libraries to reference each other locally.
  local vendored
  for vendored in "${copied[@]-}"; do
    [[ -z "${vendored}" ]] && continue
    local vendored_base="$(basename "${vendored}")"
    install_name_tool -id "@loader_path/${vendored_base}" "${vendored}"

    while IFS= read -r dep; do
      [[ -z "${dep}" ]] && continue
      local dep_base="$(basename "${dep}")"
      local local_dep="${frameworks_dir}/${dep_base}"
      if [[ -f "${local_dep}" ]]; then
        install_name_tool -change "${dep}" "@loader_path/${dep_base}" "${vendored}"
      fi
    done < <(otool -L "${vendored}" | tail -n +2 | awk '{print $1}')
  done

  local copied_count=0
  for _ in "${copied[@]-}"; do
    [[ -z "${_}" ]] && continue
    copied_count=$((copied_count + 1))
  done
  echo "==> Vendored ${copied_count} runtime libraries into ${frameworks_dir}"
}

ad_hoc_sign_bundle() {
  echo "==> Ad-hoc signing plugin bundle"
  codesign --force --deep --sign - --timestamp=none "${PLUGIN_BUNDLE}"
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
cmake --build "${BUILD_DIR}" --config Release --clean-first -j"${JOBS}"

if [[ ! -d "${PLUGIN_BUNDLE}" ]]; then
  echo "error: plugin bundle not found at ${PLUGIN_BUNDLE}" >&2
  exit 1
fi

echo "==> Vendoring runtime dependencies"
vendor_runtime_deps
ad_hoc_sign_bundle

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

echo "==> Creating installer postinstall script"
rm -rf "${PKG_SCRIPTS_DIR}"
mkdir -p "${PKG_SCRIPTS_DIR}"
cat > "${PKG_SCRIPTS_DIR}/postinstall" <<'EOS'
#!/bin/bash
set -euo pipefail

PLUGIN_NAME="obs-airplay.plugin"
SYSTEM_PLUGIN="/Library/Application Support/obs-studio/plugins/${PLUGIN_NAME}"

CONSOLE_USER="$(/usr/sbin/scutil <<< "show State:/Users/ConsoleUser" | /usr/bin/awk '/Name :/ && $3 != "loginwindow" { print $3 }')"
if [[ -z "${CONSOLE_USER}" ]]; then
  exit 0
fi

USER_HOME="$(/usr/bin/dscl . -read "/Users/${CONSOLE_USER}" NFSHomeDirectory | /usr/bin/awk '{print $2}')"
if [[ -z "${USER_HOME}" ]]; then
  exit 0
fi

TARGET_DIR="${USER_HOME}/Library/Application Support/obs-studio/plugins"
TARGET_PLUGIN="${TARGET_DIR}/${PLUGIN_NAME}"

/bin/mkdir -p "${TARGET_DIR}"
/bin/rm -rf "${TARGET_PLUGIN}"
/bin/cp -R "${SYSTEM_PLUGIN}" "${TARGET_PLUGIN}"
/usr/sbin/chown -R "${CONSOLE_USER}:staff" "${TARGET_PLUGIN}" || true

exit 0
EOS
/bin/chmod 755 "${PKG_SCRIPTS_DIR}/postinstall"

echo "==> Building pkg installer"
rm -f "${DIST_DIR}/${PKG_NAME}"
pkgbuild \
  --root "${STAGE_DIR}" \
  --scripts "${PKG_SCRIPTS_DIR}" \
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
echo "  - Pkg: installs to ${INSTALL_PATH} and also copies into the active user's ~/Library/Application Support/obs-studio/plugins/"
echo
echo "Tip: set ARCH_OVERRIDE=x86_64 or ARCH_OVERRIDE=\"arm64;x86_64\" before running to build other architectures."
