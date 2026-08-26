#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${OBS_AIRPLAY_BUILD_DIR:-${ROOT_DIR}/build}"
DIST_DIR="${OBS_AIRPLAY_DIST_DIR:-${ROOT_DIR}/dist}"
STAGE_DIR="${BUILD_DIR}/pkg-root"
PKG_SCRIPTS_DIR="${BUILD_DIR}/pkg-scripts"

PLUGIN_NAME="obs-airplay.plugin"
PLUGIN_BUNDLE="${BUILD_DIR}/${PLUGIN_NAME}"
INSTALL_BASE="/Library/Application Support/obs-studio/plugins"
INSTALL_PATH="${INSTALL_BASE}/${PLUGIN_NAME}"

PACKAGE_ID="com.obsairplay.plugin"
VERSION="$(sed -nE 's/^project\(obs-airplay VERSION ([0-9]+\.[0-9]+\.[0-9]+)\).*/\1/p' "${ROOT_DIR}/CMakeLists.txt" | head -n1)"
VERSION="${VERSION:-2.0.1}"
ARCH="${ARCH_OVERRIDE:-$(uname -m)}"
MACOS_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET_OVERRIDE:-12.0}"
DEPS_PREFIX="${OBS_AIRPLAY_DEPS_PREFIX:-}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

if [[ -n "${DEPS_PREFIX}" ]]; then
  if [[ ! -d "${DEPS_PREFIX}/lib/pkgconfig" ]]; then
    echo "error: release dependency prefix is incomplete: ${DEPS_PREFIX}" >&2
    echo "Run scripts/build-macos-release-deps.sh first." >&2
    exit 1
  fi
  export PKG_CONFIG_LIBDIR="${DEPS_PREFIX}/lib/pkgconfig:${DEPS_PREFIX}/share/pkgconfig"
  export PKG_CONFIG_PATH="${PKG_CONFIG_LIBDIR}"
  # UxPlay appends this prefix to its pkg-config search path.
  export HOMEBREW_PREFIX="${DEPS_PREFIX}"
fi

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
    "${FFMPEG_LIBDIR}/libavutil.dylib"
    "${FFMPEG_LIBDIR}/libswscale.dylib"
    "${FFMPEG_LIBDIR}/libswresample.dylib"
    "${PLIST_LIBDIR}/libplist-2.0.a"
  )
  if [[ -n "${OPENSSL_PREFIX}" ]]; then
    required_files+=(
      "${OPENSSL_PREFIX}/lib/libcrypto.a"
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

version_greater_than() {
  /usr/bin/awk -v left="$1" -v right="$2" 'BEGIN {
    split(left, a, "."); split(right, b, ".");
    for (i = 1; i <= 3; i++) {
      av = (a[i] == "" ? 0 : a[i]) + 0;
      bv = (b[i] == "" ? 0 : b[i]) + 0;
      if (av > bv) exit 0;
      if (av < bv) exit 1;
    }
    exit 1;
  }'
}

validate_build_dependency_targets() {
  local dependencies=(
    "${FFMPEG_LIBDIR}/libavcodec.dylib"
    "${FFMPEG_LIBDIR}/libavutil.dylib"
    "${FFMPEG_LIBDIR}/libswscale.dylib"
    "${FFMPEG_LIBDIR}/libswresample.dylib"
    "${PLIST_LIBDIR}/libplist-2.0.a"
  )
  if [[ -n "${OPENSSL_PREFIX}" ]]; then
    dependencies+=("${OPENSSL_PREFIX}/lib/libcrypto.a")
  fi

  local dependency=""
  local minos=""
  local invalid=0
  echo "==> Verifying dependency deployment targets"
  for dependency in "${dependencies[@]}"; do
    [[ -f "${dependency}" ]] || continue
    while IFS= read -r minos; do
      [[ -n "${minos}" ]] || continue
      if version_greater_than "${minos}" "${MACOS_DEPLOYMENT_TARGET}"; then
        echo "error: ${dependency} was built for macOS ${minos}, above target ${MACOS_DEPLOYMENT_TARGET}"
        invalid=1
        break
      fi
    done < <(otool -l "${dependency}" | awk '$1 == "minos" {print $2}')
  done

  if [[ "${invalid}" -ne 0 ]]; then
    cat <<EOF

Packaging stopped before compilation because the selected dynamic or static
dependencies are newer than the promised macOS baseline. Build release
dependencies with MACOSX_DEPLOYMENT_TARGET=${MACOS_DEPLOYMENT_TARGET}, or
explicitly choose a newer target with MACOSX_DEPLOYMENT_TARGET_OVERRIDE.
EOF
    exit 3
  fi
}

validate_deployment_target() {
  local helper_bin="${PLUGIN_BUNDLE}/Contents/Resources/OBS AirPlay Discovery.app/Contents/MacOS/OBS AirPlay Discovery"
  local files=("${PLUGIN_BUNDLE}/Contents/MacOS/obs-airplay.so" "${helper_bin}")
  local framework=""
  local minos=""
  local invalid=0

  while IFS= read -r -d '' framework; do
    files+=("${framework}")
  done < <(find "${PLUGIN_BUNDLE}/Contents/Frameworks" -type f -name '*.dylib' -print0 2>/dev/null)

  echo "==> Verifying macOS ${MACOS_DEPLOYMENT_TARGET}+ compatibility"
  for framework in "${files[@]}"; do
    [[ -f "${framework}" ]] || continue
    while IFS= read -r minos; do
      [[ -n "${minos}" ]] || continue
      if version_greater_than "${minos}" "${MACOS_DEPLOYMENT_TARGET}"; then
        echo "error: ${framework} requires macOS ${minos}, above target ${MACOS_DEPLOYMENT_TARGET}"
        invalid=1
      fi
    done < <(otool -l "${framework}" | awk '$1 == "minos" {print $2}')
  done

  if [[ "${invalid}" -ne 0 ]]; then
    cat <<EOF

Packaging stopped because one or more bundled binaries would raise the real
minimum macOS version. Rebuild those dependencies with
MACOSX_DEPLOYMENT_TARGET=${MACOS_DEPLOYMENT_TARGET}, or explicitly choose a
newer target with MACOSX_DEPLOYMENT_TARGET_OVERRIDE.
EOF
    exit 3
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
  otool -L "${file}" | tail -n +2 | awk '{print $1}' |
    awk '/^\// && !/^\/System\// && !/^\/usr\/lib\// && !/^\/Applications\/OBS\.app\//' || true
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
    chmod u+w "${dst}"
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

  local copied_count
  copied_count="$(find "${frameworks_dir}" -type f -name '*.dylib' | wc -l | tr -d ' ')"
  echo "==> Vendored ${copied_count} runtime libraries into ${frameworks_dir}"
}

sign_bundle() {
  local identity="${OBS_AIRPLAY_CODESIGN_IDENTITY:--}"
  if [[ "${identity}" == "-" ]]; then
    echo "==> Ad-hoc signing plugin bundle (development artifact)"
    codesign --force --deep --sign - --timestamp=none "${PLUGIN_BUNDLE}"
  else
    echo "==> Signing plugin bundle with ${identity}"
    codesign --force --deep --options runtime --timestamp --sign "${identity}" "${PLUGIN_BUNDLE}"
  fi
  codesign --verify --deep --strict --verbose=2 "${PLUGIN_BUNDLE}"
}

ZIP_NAME="obs-airplay-v${VERSION}-macos-${ARCH}.zip"
PKG_NAME="obs-airplay-v${VERSION}-macos-${ARCH}.pkg"

echo "==> Packaging OBS AirPlay plugin"
echo "Root:    ${ROOT_DIR}"
echo "Build:   ${BUILD_DIR}"
echo "Dist:    ${DIST_DIR}"
echo "Version: ${VERSION}"
echo "Arch:    ${ARCH}"
echo "macOS:   ${MACOS_DEPLOYMENT_TARGET}+"
echo "Jobs:    ${JOBS}"
if [[ -n "${DEPS_PREFIX}" ]]; then
  echo "Deps:    ${DEPS_PREFIX}"
fi

IFS=';' read -r -a ARCH_LIST <<< "${ARCH}"
for a in "${ARCH_LIST[@]}"; do
  check_arch_support "${a}"
done
validate_build_dependency_targets

mkdir -p "${BUILD_DIR}" "${DIST_DIR}"

PKG_CONFIG_EXECUTABLE_OVERRIDE=""
if [[ -n "${DEPS_PREFIX}" ]]; then
  REAL_PKG_CONFIG="$(command -v pkg-config)"
  PKG_CONFIG_WRAPPER="${BUILD_DIR}/pkg-config-isolated"
  export OBS_AIRPLAY_REAL_PKG_CONFIG="${REAL_PKG_CONFIG}"
  export OBS_AIRPLAY_DEPS_PREFIX="${DEPS_PREFIX}"
  cat > "${PKG_CONFIG_WRAPPER}" <<'EOS'
#!/usr/bin/env bash
set -euo pipefail

: "${OBS_AIRPLAY_REAL_PKG_CONFIG:?missing real pkg-config path}"
: "${OBS_AIRPLAY_DEPS_PREFIX:?missing release dependency prefix}"

export PKG_CONFIG_LIBDIR="${OBS_AIRPLAY_DEPS_PREFIX}/lib/pkgconfig:${OBS_AIRPLAY_DEPS_PREFIX}/share/pkgconfig"
export PKG_CONFIG_PATH="${PKG_CONFIG_LIBDIR}"
exec "${OBS_AIRPLAY_REAL_PKG_CONFIG}" "$@"
EOS
  chmod 755 "${PKG_CONFIG_WRAPPER}"
  PKG_CONFIG_EXECUTABLE_OVERRIDE="${PKG_CONFIG_WRAPPER}"
fi

echo "==> Configuring build"
CMAKE_ARGS=(
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_OSX_ARCHITECTURES="${ARCH}"
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}"
  -DNO_MARCH_NATIVE=ON
)
if [[ -n "${DEPS_PREFIX}" ]]; then
  CMAKE_ARGS+=(
    -DCMAKE_PREFIX_PATH="${DEPS_PREFIX}"
    -DCMAKE_LIBRARY_PATH="${DEPS_PREFIX}/lib"
    -DCMAKE_INCLUDE_PATH="${DEPS_PREFIX}/include"
    -DPKG_CONFIG_EXECUTABLE="${PKG_CONFIG_EXECUTABLE_OVERRIDE}"
  )
fi
cmake "${CMAKE_ARGS[@]}"

echo "==> Building plugin"
cmake --build "${BUILD_DIR}" --config Release --clean-first -j"${JOBS}"

if [[ ! -d "${PLUGIN_BUNDLE}" ]]; then
  echo "error: plugin bundle not found at ${PLUGIN_BUNDLE}" >&2
  exit 1
fi

echo "==> Vendoring runtime dependencies"
vendor_runtime_deps
validate_deployment_target
# Source/build volumes can add provenance and resource-fork metadata that turns
# into spurious ._* files in zip/pkg payloads. The bundle does not use it.
xattr -cr "${PLUGIN_BUNDLE}"
sign_bundle

echo "==> Creating zip artifact"
rm -f "${DIST_DIR}/${ZIP_NAME}"
(
  cd "${BUILD_DIR}"
  /usr/bin/ditto -c -k --norsrc --keepParent "${PLUGIN_NAME}" "${DIST_DIR}/${ZIP_NAME}"
)

echo "==> Creating pkg staging layout"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}${INSTALL_BASE}"
COPYFILE_DISABLE=1 cp -R "${PLUGIN_BUNDLE}" "${STAGE_DIR}${INSTALL_BASE}/"
xattr -cr "${STAGE_DIR}"
# Some filesystems materialize AppleDouble sidecars during a recursive copy.
# They are metadata artifacts, not part of the plugin payload.
find "${STAGE_DIR}" -type f -name '._*' -delete

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
INSTALLER_IDENTITY="${OBS_AIRPLAY_INSTALLER_IDENTITY:-}"
if [[ -n "${INSTALLER_IDENTITY}" ]]; then
  UNSIGNED_PKG="${BUILD_DIR}/${PKG_NAME%.pkg}-unsigned.pkg"
  rm -f "${UNSIGNED_PKG}"
  pkgbuild \
    --root "${STAGE_DIR}" \
    --scripts "${PKG_SCRIPTS_DIR}" \
    --identifier "${PACKAGE_ID}" \
    --version "${VERSION}" \
    --install-location "/" \
    "${UNSIGNED_PKG}"
  productsign --sign "${INSTALLER_IDENTITY}" \
    "${UNSIGNED_PKG}" "${DIST_DIR}/${PKG_NAME}"
  pkgutil --check-signature "${DIST_DIR}/${PKG_NAME}"
else
  pkgbuild \
    --root "${STAGE_DIR}" \
    --scripts "${PKG_SCRIPTS_DIR}" \
    --identifier "${PACKAGE_ID}" \
    --version "${VERSION}" \
    --install-location "/" \
    "${DIST_DIR}/${PKG_NAME}"
  echo "warning: installer is unsigned; set OBS_AIRPLAY_INSTALLER_IDENTITY for release artifacts"
fi

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
