#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the two files of Apple's Bonjour SDK that the Windows build needs,
# without the SDK installer (which needs an Apple developer sign-in):
#   OUT_DIR/Include/dns_sd.h   Apple's header (BSD-3-Clause), pinned below
#   OUT_DIR/Lib/x64/dnssd.lib  an import library from windows/bonjour/dnssd.def
# UxPlay loads dnssd.dll at run time, so end users need Bonjour (the
# mDNSResponder service) installed; this only satisfies the build. Point
# the build at the result with BONJOUR_SDK_HOME=OUT_DIR.
#   scripts/windows-bonjour-sdk.sh OUT_DIR
set -euo pipefail

OUT="${1:?usage: $0 OUT_DIR}"
SOURCE="$(cd "$(dirname "$0")/.." && pwd)"
TAG="mDNSResponder-2881.120.11"
URL="https://raw.githubusercontent.com/apple-oss-distributions/mDNSResponder/${TAG}/mDNSShared/dns_sd.h"
SHA256="5d0ca50f207f6eb02e09d743f9b65d2ade65e8f81862dcd3703845b4fe87a9c1"

mkdir -p "${OUT}/Include" "${OUT}/Lib/x64"
if [[ ! -f "${OUT}/Include/dns_sd.h" ]]; then
  curl -L --fail --retry 3 -o "${OUT}/Include/dns_sd.h.part" "${URL}"
  mv "${OUT}/Include/dns_sd.h.part" "${OUT}/Include/dns_sd.h"
fi
actual="$(sha256sum "${OUT}/Include/dns_sd.h" | cut -d' ' -f1)"
if [[ "${actual}" != "${SHA256}" ]]; then
  echo "error: SHA-256 mismatch for dns_sd.h (${actual})" >&2
  rm -f "${OUT}/Include/dns_sd.h"
  exit 1
fi

DLLTOOL="${DLLTOOL:-dlltool}"
"${DLLTOOL}" -m i386:x86-64 -d "${SOURCE}/windows/bonjour/dnssd.def" -l "${OUT}/Lib/x64/dnssd.lib"
echo "Bonjour SDK files in ${OUT} (BONJOUR_SDK_HOME)"
