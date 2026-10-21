#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a command while local HTTP (18901, 18902), HTTPS (18443), truncating
# HTTP (18903, responses cut after 64 KiB) and redirecting HTTP (18904, also
# on [::1]) servers serve the fixtures: --nested-policy, --tls-verify,
# --resilient and --fetch-pinning fetch from them.
#   tests/with-media-servers.sh FIXTURE_DIR COMMAND...
set -euo pipefail
DIR="$1"; shift
SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
python3 -I -m http.server 18901 --bind 127.0.0.1 --directory "${DIR}/hls" >/dev/null 2>&1 &
P1=$!
python3 -I -m http.server 18902 --bind 127.0.0.1 --directory "${DIR}/hls" >/dev/null 2>&1 &
P2=$!
python3 -I "${SCRIPTS}/https-server.py" "${DIR}/tls" 18443 >/dev/null 2>&1 &
P3=$!
python3 -I "${SCRIPTS}/truncating-server.py" "${DIR}" 18903 65536 >/dev/null 2>&1 &
P4=$!
python3 -I "${SCRIPTS}/redirect-server.py" "${DIR}" 18904 >/dev/null 2>&1 &
P5=$!
trap 'kill ${P1} ${P2} ${P3} ${P4} ${P5} 2>/dev/null || true' EXIT
for _ in $(seq 50); do
  (exec 3<>/dev/tcp/127.0.0.1/18901 && exec 3<>/dev/tcp/127.0.0.1/18443 &&
   exec 3<>/dev/tcp/127.0.0.1/18903 && exec 3<>/dev/tcp/127.0.0.1/18904) 2>/dev/null && break
  sleep 0.1
done
"$@"
