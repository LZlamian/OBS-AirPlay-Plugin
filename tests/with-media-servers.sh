#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a command while local HTTP (base+1, base+2), HTTPS (base+43),
# truncating HTTP (base+3, responses cut after 64 KiB) and redirecting HTTP
# (base+4, also on [::1]) servers serve the fixtures: --nested-policy,
# --tls-verify, --resilient, --fetch-pinning and the HLS cases fetch from
# them. The base is OBS_AIRPLAY_TEST_PORT_BASE (18900), set per build tree by
# CMake so two trees can run their tests at once.
#   tests/with-media-servers.sh FIXTURE_DIR COMMAND...
set -euo pipefail
DIR="$1"; shift
SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
BASE="${OBS_AIRPLAY_TEST_PORT_BASE:-18900}"
export OBS_AIRPLAY_TEST_PORT_BASE="${BASE}"
python3 -I -m http.server $((BASE + 1)) --bind 127.0.0.1 --directory "${DIR}/hls" >/dev/null 2>&1 &
P1=$!
python3 -I -m http.server $((BASE + 2)) --bind 127.0.0.1 --directory "${DIR}/hls" >/dev/null 2>&1 &
P2=$!
python3 -I "${SCRIPTS}/https-server.py" "${DIR}/tls" $((BASE + 43)) >/dev/null 2>&1 &
P3=$!
python3 -I "${SCRIPTS}/truncating-server.py" "${DIR}" $((BASE + 3)) 65536 >/dev/null 2>&1 &
P4=$!
python3 -I "${SCRIPTS}/redirect-server.py" "${DIR}" $((BASE + 4)) >/dev/null 2>&1 &
P5=$!
trap 'kill ${P1} ${P2} ${P3} ${P4} ${P5} 2>/dev/null || true' EXIT
for _ in $(seq 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$((BASE + 1))" && exec 3<>"/dev/tcp/127.0.0.1/$((BASE + 43))" &&
   exec 3<>"/dev/tcp/127.0.0.1/$((BASE + 3))" && exec 3<>"/dev/tcp/127.0.0.1/$((BASE + 4))") 2>/dev/null && break
  sleep 0.1
done
"$@"
