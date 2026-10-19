#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Packages a Windows build (release configuration: OBS_AIRPLAY_STATIC_DEPS)
# for OBS Studio 30 and later, which loads plugins from
#   %ProgramData%\obs-studio\plugins\obs-airplay\bin\64bit\obs-airplay.dll
# with their data in ...\obs-airplay\data. Writes to OBS_AIRPLAY_DIST_DIR:
#   obs-airplay-v<version>-windows-x64.zip          the obs-airplay folder
#   obs-airplay-v<version>-windows-x64-setup.exe    installer (if ISCC exists)
#   obs-airplay-v<version>-windows-x64-symbols.zip  the unstripped DLL
# Run in MSYS2 (CI) or on Linux with mingw-w64 (OBS_AIRPLAY_CROSS).
#   OBS_AIRPLAY_BUILD_DIR=build-windows scripts/package-windows.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${OBS_AIRPLAY_BUILD_DIR:-${ROOT_DIR}/build-windows}"
DIST_DIR="${OBS_AIRPLAY_DIST_DIR:-${ROOT_DIR}/dist}"
STAGE_DIR="${BUILD_DIR}/package"
CROSS="${OBS_AIRPLAY_CROSS:-}"
STRIP="${CROSS:+${CROSS}-}strip"
OBJDUMP="${CROSS:+${CROSS}-}objdump"

VERSION="$(sed -nE 's/^project\(obs-airplay VERSION ([0-9]+\.[0-9]+\.[0-9]+)\).*/\1/p' "${ROOT_DIR}/CMakeLists.txt" | head -n1)"
if [[ -z "${VERSION}" ]]; then
  echo "error: could not read the version from CMakeLists.txt" >&2
  exit 1
fi
if [[ -n "${OBS_AIRPLAY_RELEASE_TAG:-}" && "${OBS_AIRPLAY_RELEASE_TAG#v}" != "${VERSION}" ]]; then
  echo "error: tag ${OBS_AIRPLAY_RELEASE_TAG} does not match CMakeLists.txt version ${VERSION}" >&2
  exit 1
fi
BASENAME="obs-airplay-v${VERSION}-windows-x64"

DLL="${BUILD_DIR}/obs-airplay.dll"
if [[ ! -f "${DLL}" ]]; then
  echo "error: ${DLL} not found; build first" >&2
  exit 1
fi

# The DLL may need only Windows and OBS (the release links everything else).
echo "==> Checking ${DLL}"
imports="$("${OBJDUMP}" -p "${DLL}" | sed -nE 's/^[[:space:]]*DLL Name: (.*)$/\1/p')"
unexpected="$(printf '%s\n' "${imports}" | grep -viE \
  '^(obs|kernel32|user32|advapi32|ole32|ws2_32|bcrypt|crypt32|ncrypt|secur32|ntdll|msvcrt|api-ms-win-crt-[a-z0-9-]+)\.dll$' || true)"
if [[ -n "${unexpected}" ]]; then
  echo "error: obs-airplay.dll needs DLLs OBS does not provide:" >&2
  printf '%s\n' "${unexpected}" | sed 's/^/  /' >&2
  echo "Build with -DOBS_AIRPLAY_STATIC_DEPS=ON (scripts/build-windows-release-deps.sh)." >&2
  exit 1
fi
# Exactly what libobs looks up (src/obs-airplay.exports), nothing else.
exports="$("${OBJDUMP}" -p "${DLL}" |
  sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' |
  sed -nE 's/^[[:space:]]*\[ *[0-9]+\] +([A-Za-z_][A-Za-z0-9_]*)$/\1/p' | sort | tr '\n' ' ')"
expected="$(sed 's/^_//' "${ROOT_DIR}/src/obs-airplay.exports" | sort | tr '\n' ' ')"
if [[ "${exports}" != "${expected}" ]]; then
  echo "error: obs-airplay.dll exports [${exports}], expected [${expected}]" >&2
  exit 1
fi

echo "==> Staging ${BASENAME}"
rm -rf "${STAGE_DIR}"
PLUGIN_DIR="${STAGE_DIR}/obs-airplay"
mkdir -p "${PLUGIN_DIR}/bin/64bit" "${PLUGIN_DIR}/data" "${DIST_DIR}"
"${STRIP}" --strip-unneeded -o "${PLUGIN_DIR}/bin/64bit/obs-airplay.dll" "${DLL}"
cp -R "${ROOT_DIR}/data/." "${PLUGIN_DIR}/data/"
cp "${ROOT_DIR}/LICENSE" "${PLUGIN_DIR}/LICENSE.txt"
cp "${ROOT_DIR}/uxplay/LICENSE" "${PLUGIN_DIR}/LICENSE-UxPlay-GPL-3.0.txt"
cp "${ROOT_DIR}/THIRD_PARTY_NOTICES.md" "${PLUGIN_DIR}/THIRD_PARTY_NOTICES.md"
cp -R "${ROOT_DIR}/licenses" "${PLUGIN_DIR}/licenses"
printf '%s\n' "${VERSION}" > "${PLUGIN_DIR}/VERSION.txt"
cp "${ROOT_DIR}/diagnose.ps1" "${PLUGIN_DIR}/diagnose.ps1"

zip_dir() {  # zip_dir OUTPUT DIR_PARENT ENTRY
  rm -f "$1"
  if command -v zip >/dev/null 2>&1; then
    (cd "$2" && zip -q -r -X "$1" "$3")
  else
    (cd "$2" && python3 -I -c 'import shutil,sys; shutil.make_archive(sys.argv[1][:-4], "zip", ".", sys.argv[2])' "$1" "$3")
  fi
}

echo "==> ${BASENAME}.zip"
zip_dir "${DIST_DIR}/${BASENAME}.zip" "${STAGE_DIR}" obs-airplay

echo "==> ${BASENAME}-symbols.zip"
mkdir -p "${STAGE_DIR}/symbols"
cp "${DLL}" "${STAGE_DIR}/symbols/obs-airplay.dll"
zip_dir "${DIST_DIR}/${BASENAME}-symbols.zip" "${STAGE_DIR}" symbols

ISCC="${OBS_AIRPLAY_ISCC:-}"
if [[ -z "${ISCC}" ]]; then
  for candidate in iscc ISCC "/c/Program Files (x86)/Inno Setup 6/ISCC.exe"; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      ISCC="${candidate}"
      break
    fi
  done
fi
if [[ -n "${ISCC}" ]]; then
  echo "==> ${BASENAME}-setup.exe"
  to_windows() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }
  # MSYS2 must not rewrite the /D... switches as paths.
  MSYS2_ARG_CONV_EXCL='*' "${ISCC}" /Q \
    "/DAppVersion=${VERSION}" \
    "/DSourceDir=$(to_windows "${PLUGIN_DIR}")" \
    "/DOutputDir=$(to_windows "${DIST_DIR}")" \
    "/DOutputBaseName=${BASENAME}-setup" \
    "$(to_windows "${ROOT_DIR}/windows/installer/obs-airplay.iss")"
else
  echo "Inno Setup (ISCC) not found: no installer built"
fi

echo "==> Done"
ls -l "${DIST_DIR}"
