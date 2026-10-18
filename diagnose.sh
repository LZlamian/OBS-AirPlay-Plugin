#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# OBS AirPlay diagnostics: run while OBS is open if devices cannot see or
# connect to the receiver. Needs no administrator rights and changes nothing.
#
#   ./diagnose.sh                 (OBS_BUNDLE_PATH=/path/to/OBS.app to override)
#
# The output names your network interface, local IP address and the AirPlay
# devices on your network: read it before posting it publicly.

set -uo pipefail

OBS_BUNDLE_PATH="${OBS_BUNDLE_PATH:-/Applications/OBS.app}"
OBS_SUPPORT="${HOME}/Library/Application Support/obs-studio"
PLUGIN="${OBS_SUPPORT}/plugins/obs-airplay.plugin"
LEGACY_PLUGIN="/Library/Application Support/obs-studio/plugins/obs-airplay.plugin"
SETTINGS="${OBS_SUPPORT}/plugin_config/obs-airplay.so/settings.json"
ISSUES="https://github.com/LZlamian/OBS-AirPlay-Plugin/issues"

errors=0
warnings=0
ok()   { printf '  \033[0;32mok\033[0m    %s\n' "$*"; }
warn() { printf '  \033[1;33mwarn\033[0m  %s\n' "$*"; warnings=$((warnings + 1)); }
fail() { printf '  \033[0;31mfail\033[0m  %s\n' "$*"; errors=$((errors + 1)); }
info() { printf '        %s\n' "$*"; }
section() { printf '\n\033[1m%s\033[0m\n' "$*"; }

SCRATCH="$(mktemp -d)"
trap 'rm -rf "${SCRATCH}"' EXIT

section "OBS"
if [[ -d "${OBS_BUNDLE_PATH}" ]]; then
  ok "OBS $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
      "${OBS_BUNDLE_PATH}/Contents/Info.plist" 2>/dev/null || echo '?') at ${OBS_BUNDLE_PATH}"
else
  fail "OBS not found at ${OBS_BUNDLE_PATH} (set OBS_BUNDLE_PATH if it is elsewhere)"
fi
if pgrep -xq OBS; then ok "OBS is running"; else warn "OBS is not running: start it, then run this again"; fi

section "Plugin"
if [[ -d "${PLUGIN}" ]]; then
  ok "installed: ${PLUGIN}"
  info "version $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
      "${PLUGIN}/Contents/Info.plist" 2>/dev/null || echo '?')"
  if /usr/bin/codesign --verify --deep --strict "${PLUGIN}" 2>/dev/null; then
    ok "code seal intact"
  else
    fail "code seal broken: reinstall the plugin from the release page"
  fi
  # Release packages carry their own FFmpeg; a developer build uses Homebrew's.
  if compgen -G "${PLUGIN}/Contents/Frameworks/libavcodec*.dylib" >/dev/null; then
    ok "self-contained release build (bundled FFmpeg)"
  elif command -v brew >/dev/null 2>&1 && brew list ffmpeg >/dev/null 2>&1; then
    ok "developer build using Homebrew FFmpeg"
  else
    fail "developer build, but Homebrew FFmpeg is missing (brew install ffmpeg libplist)"
  fi
else
  fail "not installed in ${OBS_SUPPORT}/plugins"
fi
if [[ -d "${LEGACY_PLUGIN}" ]]; then
  warn "an old system-wide copy exists (${LEGACY_PLUGIN}); OBS may load it instead."
  info "Installing the current .pkg removes it, or delete it in Finder."
fi

section "Network"
iface="$(route -n get default 2>/dev/null | awk '/interface:/ {print $2}')"
if [[ "${iface}" == utun* || "${iface}" == ipsec* ]]; then
  warn "the default route goes through a VPN (${iface}); AirPlay uses the local network:"
  for lan in en0 en1; do
    address="$(ipconfig getifaddr "${lan}" 2>/dev/null)"
    [[ -n "${address}" ]] && info "${lan}: ${address}"
  done
  info "if devices cannot find the receiver, try with the VPN off"
elif [[ -n "${iface}" ]]; then
  ok "default interface ${iface}, address $(ipconfig getifaddr "${iface}" 2>/dev/null || echo '?')"
  info "the device must be on the same network (and VLAN) as this Mac"
else
  fail "no default network interface"
fi

section "Firewall"
fw="$(/usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate 2>/dev/null)"
if [[ "${fw}" == *enabled* ]]; then
  blocked="$(/usr/libexec/ApplicationFirewall/socketfilterfw --getappblocked "${OBS_BUNDLE_PATH}" 2>/dev/null)"
  if [[ "${blocked}" == *"is blocked"* ]]; then
    fail "the firewall blocks OBS: System Settings > Network > Firewall > Options, allow OBS"
  else
    ok "firewall on; OBS is not blocked"
  fi
else
  ok "firewall off"
fi

section "Ports (the receiver uses TCP 7000, or 7001 if 7000 is taken)"
for port in 7000 7001; do
  owner="$(lsof -nP -iTCP:"${port}" -sTCP:LISTEN 2>/dev/null | awk 'NR == 2 {print $1}')"
  if [[ -z "${owner}" ]]; then
    info "port ${port}: free"
  elif [[ "${owner}" == OBS* || "${owner}" == obs* ]]; then
    ok "port ${port}: OBS (the receiver)"
  elif [[ "${owner}" == ControlCe* || "${owner}" == AirPlayXP* ]]; then
    warn "port ${port}: macOS AirPlay Receiver. If OBS has neither port, turn it off in"
    info "System Settings > General > AirDrop & Handoff > AirPlay Receiver"
  else
    warn "port ${port}: ${owner}"
  fi
done

section "Bonjour (5 s scan of _airplay._tcp)"
name="OBS AirPlay"
if [[ -f "${SETTINGS}" ]]; then
  saved="$(/usr/bin/plutil -extract server_name raw -o - "${SETTINGS}" 2>/dev/null)"
  [[ -n "${saved}" ]] && name="${saved}"
fi
dns-sd -B _airplay._tcp > "${SCRATCH}/scan.txt" 2>&1 &
scan_pid=$!
sleep 5
kill "${scan_pid}" 2>/dev/null
wait "${scan_pid}" 2>/dev/null
if grep -qF "${name}" "${SCRATCH}/scan.txt"; then
  ok "\"${name}\" is advertised"
else
  fail "\"${name}\" is not advertised (is OBS running with the plugin loaded?)"
fi
others="$(grep -c ' Add ' "${SCRATCH}/scan.txt" 2>/dev/null)"
info "${others:-0} AirPlay service(s) seen in total"

section "OBS log"
latest="$(ls -t "${OBS_SUPPORT}/logs" 2>/dev/null | head -1)"
if [[ -n "${latest}" ]]; then
  log="${OBS_SUPPORT}/logs/${latest}"
  ok "latest log: ${latest}"
  if grep -q 'OBS AirPlay Plugin loaded' "${log}"; then
    ok "the plugin loaded"
  else
    fail "the plugin did not load in that session"
  fi
  grep -E 'UxPlay integration (started|active)|Failed to start UxPlay|\[LOAD\]' "${log}" | tail -5 |
    sed 's/^/        /'
else
  warn "no OBS logs found"
fi

section "Summary"
if (( errors == 0 && warnings == 0 )); then
  ok "nothing wrong found"
else
  info "${errors} problem(s), ${warnings} warning(s)"
fi
info "Report problems at ${ISSUES} (read the output first: it names your network and devices)."
exit $(( errors > 0 ? 1 : 0 ))
