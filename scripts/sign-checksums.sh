#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes SHA256SUMS for the release files in DIST and signs it with the
# release key (SHA256SUMS.sig, a raw Ed25519 signature), so the .pkg, which
# the self-updater never sees, is covered by the same key as the zip.
# INSTALL.md shows how to check it with docs/release-signing-key.pem.
#
#   scripts/sign-checksums.sh DIST
#
# OBS_AIRPLAY_SIGNING_KEY and OBS_AIRPLAY_OPENSSL as for sign-release.sh;
# without the key only SHA256SUMS is written.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${1:?usage: sign-checksums.sh DIST}"
KEY="${OBS_AIRPLAY_SIGNING_KEY:-${HOME}/.obs-airplay-release-signing/ed25519.pem}"

(
  cd "${DIST}"
  rm -f SHA256SUMS SHA256SUMS.sig
  shasum -a 256 -- * > SHA256SUMS
)
if [[ ! -f "${KEY}" ]]; then
  echo "==> SHA256SUMS written (no signing key: not signed)"
  exit 0
fi

OPENSSL=""
for candidate in "${OBS_AIRPLAY_OPENSSL:-}" /opt/homebrew/bin/openssl /usr/local/bin/openssl \
                 /opt/homebrew/opt/openssl@3/bin/openssl "$(command -v openssl || true)"; do
  if [[ -n "${candidate}" && -x "${candidate}" ]] &&
     "${candidate}" version 2>/dev/null | grep -q '^OpenSSL 3'; then
    OPENSSL="${candidate}"
    break
  fi
done
[[ -n "${OPENSSL}" ]] || { echo "error: OpenSSL 3 not found (set OBS_AIRPLAY_OPENSSL)" >&2; exit 1; }

"${OPENSSL}" pkeyutl -sign -inkey "${KEY}" -rawin -in "${DIST}/SHA256SUMS" \
  -out "${DIST}/SHA256SUMS.sig"
# The published public key must be the one that signed.
"${OPENSSL}" pkeyutl -verify -pubin -inkey "${ROOT_DIR}/docs/release-signing-key.pem" -rawin \
  -in "${DIST}/SHA256SUMS" -sigfile "${DIST}/SHA256SUMS.sig" >/dev/null
echo "==> SHA256SUMS written and signed (SHA256SUMS.sig)"
