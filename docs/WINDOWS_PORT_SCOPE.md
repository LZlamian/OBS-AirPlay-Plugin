# Windows x64 port: scope (2026-10-19)

A scoping pass, not a release. Started from `main` at 563f805 (v2.6.0) with
the UxPlay fork at `uxplay-obs-airplay-v2.6.0` (f12a037). Nothing here was
run on a real Windows PC with an iPhone or iPad; what was run is listed in
§1 and the gaps are in §7.

**Short answer:** practical. The protocol core (UxPlay), the receiver, the
audio path, the HLS reader, the URL policy and the plugin DLL itself build
for Windows x64 with MinGW-w64 and link against the real `obs.dll` of OBS
Studio 32.0.4. What is missing is mostly at the edges: discovery for end
users (Bonjour), TLS trust for AirPlay video, FCUP spooling, a release
dependency build, an installer, and device testing. Estimate to a first
public Windows beta: **about 3–5 weeks of focused work**, plus device
testing (§8).

## 1. What builds and passes on Windows

CI job `windows` in `.github/workflows/ci.yml` (windows-2022, MSYS2 UCRT64,
GCC; the macOS jobs are unchanged and stay green):

| Piece | Builds | Tests on the windows-2022 runner |
|---|---|---|
| `uxplay/lib` (libairplay, playfair, llhttp) | yes, with `windows/uxplay-patches/0001-*.patch` | `protocol` smoke: see table below |
| Mirror decoder (`h264-decoder.cpp`) | yes, D3D11VA + software fallback | `decoder`, `decoder-two-au`, `decoder-colorspace` |
| Audio decoder, re-timer | yes | `unit` |
| HLS reader, media URL policy | yes | `unit`, `media-url-policy` |
| AirPlay video player (`media-player.cpp`) | yes | `media-*` (17 playback modes, HTTP(S) servers) |
| Plugin module `obs-airplay.dll` | yes, linked against OBS 32.0.4's `obs.dll` | export/import check only (not loaded into OBS) |

<!-- CI-RESULTS -->

The CI job uploads `obs-airplay.dll` as an artifact (14 days) together with
its import/export report. It is an unsigned test build linked against
MSYS2's shared FFmpeg/OpenSSL/libplist DLLs, **not** something to hand to
users (§5).

Before CI, the same tree was cross-built on Linux with Ubuntu's mingw-w64
(GCC 13, msvcrt) against OpenSSL 3.6.3, libplist 2.7.0 and a minimal static
FFmpeg 8.1.2 with D3D11VA/DXVA2 (the macOS release configuration with
VideoToolbox swapped out), and the tests were run under Wine 9:

- every target builds; `obs-airplay.dll` exports exactly the seven
  `obs_module_*` symbols and imports only Windows system DLLs and `obs.dll`;
- `unit`: all checks pass except the AAC/ALAC ones, which need FFmpeg
  *encoders* that the minimal build leaves out (CI has them);
- `media-url-policy`: passes (including `http://2130706433/` and
  `http://0x7f.1/` being refused, which exercises the Windows `inet_aton`
  replacement);
- `protocol`: everything passes except FCUP spooling (expected, §2.4),
  `session_binding` (its IPv6 `::1` peer got no answer under Wine) and
  `body_limits` (the over-budget connection saw no 413). The windows-2022
  results above say which of these are Wine artefacts.

## 2. Remaining blockers

### 2.1 Discovery: Bonjour (end-user dependency) — must fix before a beta

UxPlay advertises `_airplay._tcp` / `_raop._tcp` through Apple's DNS-SD API.
On Windows `uxplay/lib/dnssd.c` loads `dnssd.dll` at run time
(`LoadLibraryA`, lines ~197–220); `dnssd_init` fails with
`DNSSD_ERROR_LIBNOTFOUND` without it and `UxPlayIntegration::start`
(`src/uxplay-integration.cpp` ~677) then reports that the receiver could not
start. So **without Apple's Bonjour service installed, iPhones and iPads
never see the receiver.**

- Bonjour comes with iTunes / Apple Devices or "Bonjour Print Services for
  Windows". It may not be redistributed separately without Apple's
  agreement, so an installer can only detect it and point to the download.
- Building needs only `dns_sd.h` and an import library;
  `scripts/windows-bonjour-sdk.sh` fetches Apple's header at a pinned tag
  (SHA-256 checked) and makes `dnssd.lib` from `windows/bonjour/dnssd.def`.
  No Bonjour SDK installer, no Apple sign-in. Because the DLL is loaded with
  `LoadLibrary`, nothing is imported from it at link time.
- **Recommended:** a second DNS-SD backend on Windows 10 1809+'s built-in
  mDNS responder (`DnsServiceRegister` / `DnsServiceDeRegister` in
  `windns.h`, `dnsapi.lib`), with Bonjour as the fallback. That removes the
  dependency for most users. It lives in `uxplay/lib/dnssd.c` (TXT records
  become `DnsServiceConstructInstance` key/value arrays; renames re-register).
  Risk to verify on devices: whether iOS accepts the records the Windows
  responder publishes, and how it coexists with a running Bonjour service
  on the same port 5353.
- The BLE "fast discovery" helper is macOS-only and is stubbed out
  (`src/airplay-ble-helper-stub.cpp`). A Windows equivalent
  (`Windows.Devices.Bluetooth.Advertisement` from C++/WinRT) is optional;
  AirPlay works without it.

### 2.2 TLS trust for AirPlay video (HTTPS URLs) — must fix

`set_tls_options` in `src/media-player.cpp` (~line 73) verifies
certificates and points OpenSSL at `/etc/ssl/cert.pem`. Windows has no such
file and OpenSSL has no default trust store there, so every HTTPS AirPlay
video URL (YouTube, most apps) would fail verification (safe, but broken).
Fix: build the Windows FFmpeg with `--enable-schannel` and without
OpenSSL, so FFmpeg's TLS uses Windows' certificate store; `tls_verify=1`
keeps working. (Alternative: OpenSSL 3.2+'s `org.openssl.winstore` store.)
UxPlay itself still needs OpenSSL's libcrypto for pairing and FairPlay.

### 2.3 Release dependencies and the C runtime — must fix

- Shipping build: FFmpeg, OpenSSL (libcrypto only) and libplist linked
  **statically** into `obs-airplay.dll`, like the local cross-build above
  (a `scripts/build-windows-release-deps.sh` mirroring
  `scripts/build-macos-release-deps.sh`). OBS 32.0.4 ships its own FFmpeg
  7.1 as `avcodec-61.dll` etc.; our FFmpeg 8.1 DLLs would be named `-62`
  and not clash today, but the next OBS FFmpeg bump could load ours or
  theirs by name, so static (or `--build-suffix`) is the safe choice.
- libplist's headers mark its API `dllimport` unless `LIBPLIST_STATIC` is
  defined: a static release build must define it for `uxplay/lib` and the
  tests (seen in the local static build as `__imp_plist_*` link errors).
- The MinGW runtime is already linked statically (`CMakeLists.txt`, the
  `if(WIN32)` block after `add_subdirectory(uxplay/lib)`): UxPlay's
  `pthread` becomes `libwinpthread.a`, libgcc/libstdc++ are static. The CRT
  is the UCRT (MSYS2 UCRT64), the same one OBS's MSVC build uses.

### 2.4 FCUP spooling (blob:/file: AirPlay video) — should fix

`uxplay/lib/http_handlers.h` spools reverse-fetched media to a private temp
file (`mkstemp`, `fchmod 0600`, `F_SETFD`, `getuid`/`lstat` owner checks,
`/private/tmp`), and `media_url_policy::checkLocalFile`
(`src/media-url-policy.cpp`) re-checks the owner. None of that exists on
Windows; the patch makes spooling fail cleanly and the plugin refuses
`file:` items. Safari "blob:" videos and sender-local files therefore do
not play on Windows yet. Port: `%LOCALAPPDATA%\Temp`, `CreateFileW` with
`CREATE_NEW`, an owner-only DACL and `FILE_ATTRIBUTE_TEMPORARY`, and an
owner check with `GetSecurityInfo`; the media policy must accept
`C:/...` paths only under that directory.

### 2.5 UxPlay patches belong in the fork — housekeeping

`windows/uxplay-patches/0001-build-uxplay-lib-with-mingw-w64.patch` (applied
by CI; the submodule pointer is unchanged):

- `media_url_policy.h`: Winsock headers; `inet_aton` mapped to
  `RtlIpv4StringToAddressA(..., Strict=FALSE)`, Windows' own lenient
  parser, so short/octal/hex forms classify the same way Windows resolves
  them; `ntdll` linked.
- `media_trace.h`: no `memmem` in the Windows CRT.
- `http_handlers.h`: spooling stubbed (§2.4).

They should become a commit and tag in LZlamian/UxPlay, with the submodule
bumped. Upstream UxPlay already carries the rest of its Windows support
(Winsock, `LoadLibrary("dnssd.dll")`, `_WIN32` branches in `netutils.c`,
`raop_ntp.c`, `raop_rtp_mirror.c`, `httpd.c`).

### 2.6 Smaller items

| Where | What | Status |
|---|---|---|
| `src/hw-decode-platform.hpp`, `h264-decoder.cpp`, `media-player.cpp` | VideoToolbox → D3D11VA. Frames are copied to system memory (`av_hwframe_transfer_data`) and then to OBS; zero-copy would need a D3D11 shared texture via `obs_source_output_video` alternatives (libobs has no async-source texture input) | done, copy path; verify cost at 4K |
| `src/uxplay-integration.cpp` `boost_caller_thread_qos_once` | macOS QoS only; Windows could use `AvSetMmThreadCharacteristicsW(L"Games")` (OBS links AVRT already) or `SetThreadPriority` | optional |
| `src/hardware-decode-option.hpp` | legacy `~/Library/...` flag folder now macOS-only; the per-plugin config folder works on Windows | done |
| `src/airplay-source.cpp` | "Check for updates" hidden off macOS | done |
| `src/uxplay-integration.cpp` `recordRefusal` | `localtime_r` → `localtime_s` on Windows | done |
| `src/plugin-updater.mm`, `update-signature.swift`, `airplay-ble-discovery.mm` | Self-updater is AppKit/CryptoKit; on Windows it would be a separate small helper (WinHTTP + Ed25519 via libcrypto, which is already linked) that swaps the DLL after OBS quits | not started, not needed for a beta |
| `scripts/package-macos.sh`, `Info.plist.in`, bundle layout | No Windows packaging yet (§5) | not started |
| `tests/protocol-smoke.cpp` | sockets via helpers (`closeSocket`, `setTimeout`), `SIGPIPE`/`SO_NOSIGPIPE` guarded | done |
| `tests/video-decoder-smoke.cpp` | CPU time via `GetProcessTimes` | done |
| `tests/dnssd-stub.c` | test-only `dnssd.dll` stand-in (UxPlay loads the real one even to create a context) | done |
| `uxplay/lib/httpd.c` (body budget) | On Windows the 413 for a body over the shared in-flight budget did not reach the client (`protocol` smoke `body_limits`, real windows-2022 runner): most likely because the receiver closes the socket with the rest of the body unread, Windows then sends a reset, and a reset discards the response the client had not read yet. The budget itself holds (the next request is not refused). Fix: `shutdown(SD_SEND)` and drain briefly before `closesocket` when answering 413 | 0.5–1 day |
| `src/plugin-main.cpp:360`, `airplay-server.cpp:121`, `media-player.cpp:1072` | GCC warns that `%zu` is unknown: MinGW checks OBS's `PRINTFATTR` against the old msvcrt `printf`. Harmless at run time (`blog` formats inside `obs.dll` with the UCRT, which has `%zu`), but noisy; build with `-D__USE_MINGW_ANSI_STDIO=1` or cast to `unsigned long long` + `%llu` | 0.5 day |
| `diagnose.sh`, `build.sh` | macOS-only; a PowerShell `diagnose.ps1` (ports, firewall rule, Bonjour service state) would help support | not started |

## 3. Toolchain: MinGW-w64 (MSYS2 UCRT64), not MSVC

OBS plugins on Windows are normally MSVC-built with the obs-plugintemplate,
which builds libobs from source. This plugin chose MinGW-w64 because:

1. **UxPlay is written for it.** `uxplay/lib` (≈16.8k lines of C) uses
   pthreads throughout (`threads.h`, 213 call sites of
   `pthread_*`/`MUTEX_*`/`COND_*`/`THREAD_*`), `unistd.h`/`strings.h`/
   `dirent.h`/`sys/time.h` (7 files), C11 `stdatomic.h` (`raop.c`), GCC
   flags in its CMakeLists (`-fPIC`, `-Wall` strings), and upstream UxPlay's
   only supported Windows build is MSYS2. With MinGW it needed one
   41-line patch (§2.5). MSVC would need pthreads4w (or OBS's own
   `w32-pthreads.dll`), POSIX shims, `/experimental:c11atomics`, and a fork
   of its CMakeLists: a lasting diff against upstream.
2. **The ABI boundary is plain C.** Everything the plugin calls in libobs
   is `extern "C"` (`obs-module.h`, `obs.h`, `util/*`); no C++ types, no
   STL, no exceptions cross it. On x64 there is one calling convention, and
   C structs lay out identically (MinGW uses MS bitfield layout by default).
   Memory libobs allocates is freed with `bfree` and vice versa, never with
   the plugin's own `free`. With UCRT64 even the C runtime is the same
   `ucrtbase.dll` OBS uses (OBS 32.0.4's `obs.dll` imports
   `api-ms-win-crt-*`).
3. **No libobs build needed.** GNU ld links `obs.dll` directly, so CI takes
   `bin/64bit/obs.dll` from the official OBS release zip (SHA-256 pinned);
   every libobs function the plugin imports is checked at link time against
   a real OBS build.

Implications to keep in mind:

- `EXPORT` in `obs-headers/libobs/util/c99defs.h` is `dllexport` only for
  MSVC; under MinGW it is `visibility("default")`, which does nothing on
  PE. The module is therefore linked with a `.def` generated from
  `src/obs-airplay.exports` (exactly the seven `obs_module_*` symbols,
  checked in CI). Without it GNU ld would export every symbol, statically
  linked FFmpeg/OpenSSL included.
- Debug symbols are DWARF, not PDB: OBS's crash handler shows no names for
  frames in our DLL. Keep the unstripped DLL per release, or use
  `clang` + `-gcodeview` (MSYS2 CLANG64) if PDBs are wanted later.
- `long double` and `printf` differ between the runtimes; neither crosses
  the libobs API (`blog` formats inside `obs.dll`, i.e. with the UCRT, so
  `%zu`/`%lld` are fine).
- Should OBS ever require MSVC plugins (it does not today), the port to
  MSVC is mostly the UxPlay items in (1).

## 4. Firewall and ports

Same ports as macOS (README "Requirements"): TCP 7000 (7001 if taken), UDP
ports chosen by UxPlay for timing/control/audio, a TCP mirror data port, and
outbound HTTP(S) for AirPlay video; mDNS UDP 5353 is handled by the Bonjour
or Windows DNS service, not by OBS.

- Windows Defender Firewall prompts "Allow obs64.exe to communicate" the
  first time OBS listens; users who dismiss it (or are on a "Public"
  network profile) block every connection. The installer should add an
  inbound rule for `obs64.exe` on the Private profile (TCP+UDP), e.g.
  `netsh advfirewall firewall add rule name="OBS AirPlay" dir=in
  action=allow program="%ProgramFiles%\obs-studio\bin\64bit\obs64.exe"
  profile=private`, and say plainly that Public networks are blocked.
- There is no Windows AirPlay receiver competing for port 7000 by default;
  other AirPlay receivers (AirServer, LonelyScreen, 5KPlayer, Apple Devices'
  services) may.

## 5. Packaging and installer

- OBS 30+ on Windows loads plugins from
  `%ProgramData%\obs-studio\plugins\obs-airplay\bin\64bit\obs-airplay.dll`
  with data in `...\obs-airplay\data\` (locale files from `data/`), or the
  legacy `C:\Program Files\obs-studio\obs-plugins\64bit\`. Portable OBS uses
  `obs-plugins\64bit` next to `obs64.exe`.
- Deliverables: a `.zip` in that layout plus an installer (Inno Setup or
  WiX/MSI) that finds OBS, installs to `%ProgramData%`, adds the firewall
  rule, checks for the Bonjour service (or, once §2.1 lands, the Windows
  version), and ships LICENSE, `THIRD_PARTY_NOTICES.md` and `licenses/`
  (plus Apple's dns_sd.h notice only if Bonjour code is ever bundled; today
  only the header is used at build time).
- Code signing: unsigned DLLs/installers trigger SmartScreen warnings; an
  OV/EV Authenticode certificate (or Azure Trusted Signing) is a cost
  decision for the maintainer.
- A `scripts/package-windows.sh` (MSYS2) and a tag-only CI job analogous to
  the macOS `release` job; the `release` job must stay macOS-only until
  then (the new `windows` job is not in its `needs:`).

## 6. Bonjour for end users

Today: required. `dnssd.dll` comes with Bonjour (Apple Devices / iTunes, or
Bonjour Print Services for Windows 2.0.2). If it is missing, the plugin logs
that the receiver could not start and the source shows "unavailable". The
Windows build should log a specific hint ("Bonjour is not installed: ...")
instead of the generic failure, which is a small change in
`UxPlayIntegration::start` keyed on `DNSSD_ERROR_LIBNOTFOUND`. With the
native `DnsServiceRegister` backend (§2.1) the requirement goes away on
Windows 10 1809 and later.

## 7. What cannot be verified without a real Windows PC and iOS devices

- OBS actually loading `obs-airplay.dll` (module load, source registration,
  properties UI, locale files, config folder path).
- Discovery: iPhone/iPad/Mac seeing the receiver through Bonjour, and
  through the Windows DNS service backend once written; renames; two
  receivers on one LAN.
- Pairing, FairPlay mirroring setup, mirroring at 1080p/4K, HEVC, rotation,
  reconnects; D3D11VA on Intel/AMD/NVIDIA GPUs (and the software fallback on
  a GPU without HEVC decode); CPU cost of the surface copy.
- Audio: mirror audio sync and the re-timer under Windows scheduling and
  OBS's Windows audio pipeline (WASAPI); AirPlay audio-only.
- AirPlay video (YouTube, Safari, Photos), HTTPS trust (§2.2), HLS, seek.
- Windows Firewall prompt behaviour, Private vs Public profiles, VPN
  adapters, Wi-Fi vs Ethernet, multiple NICs (UxPlay picks the MAC/interface
  differently on Windows: `netutils.c`).
- Sleep/resume, OBS shutdown with an active session, studio mode,
  scene-collection switches.
- What the CI runner cannot show: real GPUs (the runner has none, so the
  D3D11VA path falls back to software there), a running Bonjour service,
  and the Wine-only protocol results in §1.

## 8. Size estimate for the remaining pieces

| Piece | Effort | Notes |
|---|---|---|
| Move the UxPlay patch into the fork, tag, bump submodule | 0.5 day | mechanical |
| Static release deps script for Windows (FFmpeg w/ Schannel + D3D11VA, libcrypto, libplist with `LIBPLIST_STATIC`) + cache | 2–3 days | the local cross-build in §1 is the starting point |
| TLS trust (§2.2) | included above; 0.5 day to test | |
| Bonjour-missing hint in the log/source status | 0.5 day | |
| Windows DNS service backend for `dnssd.c` (§2.1) | 4–6 days | plus device testing; biggest unknown |
| FCUP spooling on Windows (§2.4) | 2–3 days | security-sensitive; needs review like the macOS version |
| Packaging: zip layout, Inno Setup installer, firewall rule, Bonjour check | 3–4 days | + code-signing setup if bought |
| Tag-time Windows release job (draft release, checksums, Ed25519 signature of the zip for a later updater) | 1–2 days | |
| `diagnose.ps1` and README/INSTALL for Windows | 1–2 days | |
| Device test pass and fixes (maintainer + follow-ups) | 1–2 weeks elapsed | unknowns: discovery, timing, GPU drivers |
| Optional: thread priority (MMCSS), Windows BLE advertiser, Windows self-updater | 1 / 3–4 / 4–6 days | not needed for a beta |

Total to a first public Windows beta: roughly **15–25 working days** of
development plus the device-testing loop.
