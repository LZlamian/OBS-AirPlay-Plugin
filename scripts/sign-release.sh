#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Signs a release asset for the self-updaters: the macOS zip (v2.6.0 and
# later) or the Windows installer.
#
#   scripts/sign-release.sh dist/obs-airplay-v2.6.0-macos-arm64.zip
#   scripts/sign-release.sh dist/obs-airplay-v2.7.0-windows-x64-setup.exe
#
# Writes <asset>.sig, which must be uploaded to the GitHub release next to the
# asset. The updaters only install an asset whose signature verifies against
# a key in kTrustedUpdateKeys (src/update-trusted-keys.h). The signed text
# binds the asset name, version, size and SHA-256, so a signature cannot be
# reused for another file or release.
#
# OBS_AIRPLAY_SIGNING_KEY  Ed25519 private key (PEM); default
#                          ~/.obs-airplay-release-signing/ed25519.pem
# OBS_AIRPLAY_OPENSSL      OpenSSL 3 binary (macOS's /usr/bin/openssl is
#                          LibreSSL and cannot sign Ed25519)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZIP="${1:?usage: sign-release.sh ASSET}"
KEY="${OBS_AIRPLAY_SIGNING_KEY:-${HOME}/.obs-airplay-release-signing/ed25519.pem}"

find_openssl() {
  local candidate
  for candidate in "${OBS_AIRPLAY_OPENSSL:-}" /opt/homebrew/bin/openssl /usr/local/bin/openssl \
                   /opt/homebrew/opt/openssl@3/bin/openssl "$(command -v openssl || true)"; do
    if [[ -n "${candidate}" && -x "${candidate}" ]] &&
       "${candidate}" version 2>/dev/null | grep -q '^OpenSSL 3'; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

OPENSSL="$(find_openssl)" || { echo "error: OpenSSL 3 not found (set OBS_AIRPLAY_OPENSSL)" >&2; exit 1; }
[[ -f "${ZIP}" ]] || { echo "error: ${ZIP} not found" >&2; exit 1; }
[[ -f "${KEY}" ]] || { echo "error: signing key ${KEY} not found (set OBS_AIRPLAY_SIGNING_KEY)" >&2; exit 1; }

NAME="$(basename "${ZIP}")"
if [[ ! "${NAME}" =~ ^obs-airplay-v([0-9]+\.[0-9]+\.[0-9]+)-(macos-(arm64|x86_64)\.zip|windows-x64-setup\.exe)$ ]]; then
  echo "error: ${NAME} is not named obs-airplay-v<x.y.z>-macos-<arch>.zip" >&2
  echo "       or obs-airplay-v<x.y.z>-windows-x64-setup.exe" >&2
  exit 1
fi
VERSION="${BASH_REMATCH[1]}"
SIZE="$(wc -c < "${ZIP}" | tr -d ' ')"
if command -v shasum >/dev/null 2>&1; then
  SHA256="$(shasum -a 256 < "${ZIP}" | cut -d' ' -f1)"
else
  SHA256="$(sha256sum < "${ZIP}" | cut -d' ' -f1)"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
# Must match OBSAirPlayUpdateSignedMessage in src/plugin-updater.mm and
# windows_update::signed_message in src/windows-update.cpp.
printf 'obs-airplay-update-v1\nname=%s\nversion=%s\nsize=%s\nsha256=%s\n' \
  "${NAME}" "${VERSION}" "${SIZE}" "${SHA256}" > "${WORK}/message"
"${OPENSSL}" pkeyutl -sign -inkey "${KEY}" -rawin -in "${WORK}/message" -out "${WORK}/signature"
"${OPENSSL}" pkey -in "${KEY}" -pubout -out "${WORK}/public.pem" 2>/dev/null
"${OPENSSL}" pkeyutl -verify -pubin -inkey "${WORK}/public.pem" -rawin -in "${WORK}/message" \
  -sigfile "${WORK}/signature" >/dev/null

PUBLIC_HEX="$("${OPENSSL}" pkey -in "${KEY}" -pubout -outform DER 2>/dev/null | tail -c 32 |
  od -An -v -tx1 | tr -d ' \n')"
if ! grep -q "\"${PUBLIC_HEX}\"" "${ROOT_DIR}/src/update-trusted-keys.h"; then
  echo "error: the signing key's public key ${PUBLIC_HEX} is not in kTrustedUpdateKeys" >&2
  exit 1
fi

{
  echo "obs-airplay-update-signature-v1"
  echo "sig: $(base64 < "${WORK}/signature" | tr -d '\n')"
} > "${ZIP}.sig"
echo "==> Signed ${NAME} (${SIZE} bytes, sha256 ${SHA256}) -> ${ZIP}.sig"
