# Windows x64 port: scope and status

Started 2026-10-19 from `main` at 563f805 (v2.6.0), UxPlay fork at
`uxplay-obs-airplay-v2.6.0` (f12a037). How to try it and what to test on a
real PC: [WINDOWS.md](WINDOWS.md). How to build it:
[DEVELOPMENT.md](DEVELOPMENT.md#windows).

**Status:** the port is done as far as it can be without a Windows PC and
an iPhone or iPad. The release configuration (FFmpeg, libcrypto and
libplist inside `obs-airplay.dll`) builds in CI against the real `obs.dll`
of OBS Studio 32.0.4, the tests run on a Windows runner, and every build is
packaged as an installer, a zip and a symbols zip. Nothing has been run
inside OBS with a device yet (§6); that, and moving the UxPlay patches into
the fork (§2), are what is left before a release.

## 1. What was done

| Piece | Where | Notes |
|---|---|---|
| Toolchain | MinGW-w64, MSYS2 UCRT64 (§3) | `obs.dll` linked directly; MinGW runtime static; UCRT like OBS |
| Hardware decoding | `src/hw-decode-platform.hpp` | D3D11VA (DXVA2 in FFmpeg too) instead of VideoToolbox; frames copied to memory as for 10-bit on macOS; software fallback unchanged |
| Release dependencies | `scripts/build-windows-release-deps.sh`, `-DOBS_AIRPLAY_STATIC_DEPS=ON` | static FFmpeg 8.1.2 (the macOS recipe, D3D11VA, Windows threads, **Schannel TLS**, so HTTPS AirPlay video is verified against Windows' certificate store), libcrypto 3.6.3, libplist 2.7.0 |
| Discovery | UxPlay patch 0002 | Bonjour when installed; otherwise the Windows DNS service (`DnsServiceRegister`, Windows 10 1809+), so Bonjour is not required. The log names the backend, and says what to install if there is none |
| AirPlay video spooling (Safari `blob:`, sender-local files) | patch 0003, `src/media-url-policy.cpp` | private files in the user's temp folder (`CREATE_NEW`, random name, DACL for the user and SYSTEM); the media policy accepts only such files owned by this user |
| 413 after an over-budget body | patch 0004 | lingering close: Windows dropped the 413 when the socket closed with unread data |
| Exports | `.def` from `src/obs-airplay.exports`, patch 0005 | exactly the seven `obs_module_*`; llhttp no longer exports its API (it did: 33 `llhttp_*` symbols, missed by the first scoping report) |
| Bluetooth LE advertiser | `src/windows-ble.cpp`, `src/windows-helper.cpp` | the macOS helper's empty advertisement through WinRT's `BluetoothLEAdvertisementPublisher` (combase.dll loaded at run time), in OBS's process; restarted after the radio returns. Smoke test `windows-ble` checks the declared interface against Windows |
| Self-updater | `src/windows-update.cpp`, `src/windows-helper.cpp`, `windows/installer/obs-airplay.iss` | daily WinHTTP check of the latest release; the `-windows-x64-setup.exe` must carry GitHub's SHA-256 and a `.sig` by the macOS updater's Ed25519 key (`src/update-trusted-keys.h`, `scripts/sign-release.sh`); prompt (Install / View Release / Later / Skip), never while live; installer started elevated with `/WAITPID`, installs after OBS quits. Test `windows-update` |
| Receiver thread priority | `src/uxplay-integration.cpp` | the mirror video and audio threads join MMCSS's "Playback" task (macOS: QoS user-interactive) |
| Portability fixes | `src/media-url-policy.cpp`, `src/uxplay-integration.cpp`, tests | Winsock, `inet_aton` → `RtlIpv4StringToAddressA` (same lenient forms Windows resolves), `localtime_s`, `%zu` → `%llu` |
| Packaging | `scripts/package-windows.sh`, `windows/installer/obs-airplay.iss` | zip in OBS 30+'s `%ProgramData%\obs-studio\plugins\obs-airplay` layout; Inno Setup installer with a firewall rule for `obs64.exe` (private networks), removed on uninstall; symbols zip; refuses a DLL with unexpected imports or exports |
| Support | `diagnose.ps1` | install, Bonjour or the DNS service, network profile, firewall, ports, OBS log; shipped in the package |
| CI | `.github/workflows/ci.yml` job `windows` | release configuration, all tests, package, artifact `obs-airplay-windows-x64` |
| Release hook | job `release-windows` | attaches a tag's Windows packages to the draft release after approval; off until the repository variable `OBS_AIRPLAY_RELEASE_WINDOWS` is `true` |
| Notices | `THIRD_PARTY_NOTICES.md`, `licenses/` | Windows section; MinGW-w64 runtime and winpthreads texts |

Last full CI run: [run 34](https://github.com/LZlamian/OBS-AirPlay-Plugin/actions/runs/39249389607)
(09d4d05), all jobs green. On windows-2022, against the release
configuration: all 29 tests pass, among them `dnssd-native` (registers
`_airplay._tcp` and `_raop._tcp` through the Windows DNS service and
resolves the AirPlay service back over mDNS with the right port and TXT
record), `protocol` and `protocol-large-blob` (Windows spooling, the 413
fix), `media-tls-verify` (Schannel) and all 17 AirPlay video playback
modes; the DLL check (imports: Windows and `obs.dll` only; exports: the
seven `obs_module_*`), `diagnose.ps1` parsing, and the zip, installer and
symbols zip (artifact `obs-airplay-windows-x64`). The macOS jobs are
unchanged and green.

## 2. Left to do (development)

| Item | Effort | Notes |
|---|---|---|
| Commit `windows/uxplay-patches/0001`–`0005` to LZlamian/UxPlay, tag, bump the submodule, drop the patch step from CI and the `_WIN32` guard around `dnssd_get_backend` in `src/uxplay-integration.cpp` | 0.5 day | needs push access to the fork (this session had none). Patch 0004 also changes macOS behaviour (for the better: RFC 9112 lingering close); run the macOS tests after the bump |
| Code signing (Authenticode) for the installer and DLL | cost decision | without it SmartScreen warns |
| Fixes from the device test pass (§6) | unknown | discovery through the Windows DNS service is the largest unknown |
| Device test of the updater (a signed test release) and BLE (a PC with Bluetooth, an iPhone) | 0.5 day | [WINDOWS.md](WINDOWS.md#what-to-test) items 8–9 |

## 3. Toolchain: MinGW-w64 (MSYS2 UCRT64), not MSVC

OBS plugins on Windows are usually built with MSVC from obs-plugintemplate,
which builds libobs from source. This plugin uses MinGW-w64 because:

1. **UxPlay is written for it.** `uxplay/lib` (≈16.8k lines of C) uses
   pthreads throughout (213 call sites of `pthread_*`/`MUTEX_*`/`COND_*`/
   `THREAD_*`), `unistd.h`/`strings.h`/`dirent.h`/`sys/time.h`, C11
   `stdatomic.h` and GCC flags in its CMakeLists; upstream UxPlay's only
   Windows build is MSYS2. MSVC would need pthreads4w (or OBS's
   `w32-pthreads.dll`), POSIX shims, `/experimental:c11atomics` and a
   different CMakeLists: a lasting diff against upstream.
2. **The ABI boundary is plain C.** Everything the plugin calls in libobs is
   `extern "C"`; no C++ types or exceptions cross it. x64 has one calling
   convention, and C structs lay out the same (MinGW uses MS bitfield layout
   by default). Memory libobs allocates is freed with `bfree`. With UCRT64
   the C runtime is the same `ucrtbase.dll` OBS uses (OBS 32.0.4's `obs.dll`
   imports `api-ms-win-crt-*`).
3. **No libobs build needed.** GNU ld links `obs.dll` itself, taken from the
   official OBS release zip (SHA-256 pinned in CI), so every libobs function
   the plugin imports is checked against a real OBS build at link time.

To keep in mind:

- OBS's `EXPORT` macro is `dllexport` only for MSVC; under MinGW the module
  is linked with a `.def` made from `src/obs-airplay.exports`, and
  `scripts/package-windows.sh` checks that nothing else is exported.
- The MinGW runtime (libgcc, libstdc++, winpthreads) is linked statically
  (`CMakeLists.txt`, the `if(WIN32)` block after `add_subdirectory(uxplay/lib)`):
  OBS ships none of it.
- Debug symbols are DWARF, not PDB: OBS's crash reports show no names for our
  frames. Releases keep the unstripped DLL (the symbols zip).
- GCC checks OBS's printf attribute against msvcrt's `printf`, which lacks
  `%zu`; log calls use `%llu` with a cast.
- If OBS ever requires MSVC-built plugins (it does not), the port is mostly
  the UxPlay items in point 1.

## 4. Firewall, ports, Bonjour

Same ports as macOS: TCP 7000 (7001 if taken), UDP ports chosen by UxPlay
for timing, control and audio, a TCP mirror data port, and outbound HTTP(S)
for AirPlay video. mDNS (UDP 5353) is handled by Bonjour or the Windows DNS
service, not by OBS.

- Windows asks to allow `obs64.exe` the first time OBS listens; on a
  **Public** network, or if the prompt is dismissed, devices cannot connect.
  The installer adds an inbound rule "OBS AirPlay" for `obs64.exe` on
  private networks; `diagnose.ps1` reports the network profile and the rule.
- Bonjour is optional. If installed (Apple Devices, iTunes, Bonjour Print
  Services) the plugin uses it, as UxPlay always has; otherwise the Windows
  DNS service. Bonjour may not be redistributed, so it is never bundled;
  only Apple's `dns_sd.h` is used at build time
  (`scripts/windows-bonjour-sdk.sh`, pinned and SHA-256 checked).

## 5. Packaging

- OBS 30+ loads per-machine plugins from
  `%ProgramData%\obs-studio\plugins\obs-airplay\bin\64bit\obs-airplay.dll`,
  data from `...\obs-airplay\data`. The installer installs there (admin,
  Windows 10+), warns about a second copy in OBS's own `obs-plugins\64bit`,
  and closes OBS if it holds the DLL. Portable OBS installs: copy the folder
  from the zip into `obs-plugins` by hand (untested).
- Artifacts: `obs-airplay-vX.Y.Z-windows-x64.zip`, `-setup.exe`,
  `-symbols.zip`; on a tag with `OBS_AIRPLAY_RELEASE_WINDOWS=true`, also
  `SHA256SUMS-windows` on the draft release.
- Unsigned: SmartScreen shows "Windows protected your PC"
  (**More info > Run anyway**).

## 6. What needs a real Windows PC and iOS devices

The checklist to run is in [WINDOWS.md](WINDOWS.md#what-to-test). In short:

- OBS loading the DLL from ProgramData (source registration, properties UI,
  locale, config folder).
- Discovery through the Windows DNS service (whether iOS accepts its
  records; the host name it publishes; coexistence with a running Bonjour)
  and through Bonjour; renames.
- Pairing and mirroring (1080p, 4K, HEVC, rotation, reconnects); D3D11VA on
  Intel, AMD and NVIDIA GPUs and its fallback; CPU cost of the frame copy.
- Audio sync and the re-timer with Windows scheduling and OBS's audio path.
- AirPlay video: HTTPS through Schannel, HLS, seek, Safari `blob:` spooling.
- Firewall prompt, Private/Public profiles, VPN adapters, several network
  adapters (UxPlay picks the interface and MAC differently on Windows).
- Sleep and resume, quitting OBS during a session.

The CI runner cannot show any of these: it has no GPU (D3D11VA falls back to
software there), no Bonjour, no OBS UI and no AirPlay sender.

## 7. History

The first scoping pass (same day) found the portable core building with one
UxPlay patch and listed the blockers: Bonjour as an end-user dependency, no
TLS trust store for AirPlay video, no static release build, no spooling,
the lost 413, no packaging. All of these are addressed above; the estimate
then was 15–25 working days of development plus device testing.
