# Developing OBS AirPlay

## Layout

| Path | What it is |
|---|---|
| `src/plugin-main.cpp` | Module load/unload, receiver settings shared by all sources, name debounce and status threads |
| `src/airplay-source.cpp` | The OBS source: properties panel, settings |
| `src/uxplay-integration.cpp` | Drives UxPlay: callbacks, DNS-SD, renames, status, deferred clearing |
| `src/airplay-server.cpp` | Mirror decode thread and output of mirror and AirPlay video frames and audio to OBS |
| `src/h264-decoder.cpp` | Mirror video decoder (VideoToolbox with software fallback) |
| `src/audio-decoder.cpp` | Mirror audio decoder (AAC-ELD, AAC-LC, ALAC) |
| `src/audio-retimer.hpp` | Keeps mirror audio from reaching OBS late (startup burst, stalls) |
| `src/media-player.cpp` | AirPlay video ("URL") player: demux, decode, presenter thread, clock |
| `src/media-url-policy.cpp` | Which URLs AirPlay video may fetch (resolved addresses, redirects, HLS) |
| `src/plugin-updater.mm`, `src/update-signature.swift` | Self-updater: release checks, Ed25519 signature, staging, swap |
| `src/airplay-ble-discovery.mm` | The helper app: Bluetooth advertising, update prompts, install after OBS quits |
| `uxplay/` | The UxPlay fork (git submodule, LZlamian/UxPlay): AirPlay protocol, HTTP server, DNS-SD |
| `obs-headers/` | Vendored libobs and SIMDe headers (see `obs-headers/PROVENANCE.md`) |
| `tests/` | Smoke tests run by CTest, fixture generation |
| `scripts/` | Release dependencies, packaging, release signing |
| `licenses/` | Third-party license texts shipped in the bundle |

The 2026-10 audit of v2.5.1, its action plan and the completion record are in
[AUDIT_2026-10-18.md](AUDIT_2026-10-18.md).

## Build and test

```bash
git clone --recursive https://github.com/LZlamian/OBS-AirPlay-Plugin.git
brew install cmake pkg-config ffmpeg libplist openssl@3   # and OBS in /Applications
cmake --preset release            # also: debug, asan, tsan
cmake --build --preset release
ctest --preset release            # 29 tests, about 40 s; generates its media first
```

The plugin bundle is `build-release/obs-airplay.plugin`; copy it into
`~/Library/Application Support/obs-studio/plugins/` (or run `./build.sh`).
Another OBS: `-DOBS_BUNDLE_PATH=/path/to/OBS.app`.

The smokes can also be run by hand (see the usage line of each):

- `obs-airplay-protocol-smoke [--large-blob]`: the fork's HTTP/RTSP surface
  against a receiver on a free port (policy, limits, session binding, ...).
- `obs-airplay-media-smoke MODE URL`: the AirPlay video player (seek, scrub,
  pause, follow-clock, URL policy, TLS, TS seeks, stop races).
- `obs-airplay-decoder-smoke FILE_A [FILE_B]`, `--two-au`, `--colorspace`,
  `--first-decode`: the mirror decoder in hardware and software.
- `obs-airplay-updater-smoke BUNDLE SCRATCH`, `--verify-release ZIP`: the
  updater on scratch copies (absolute paths).
- `obs-airplay-unit-tests`: the HLS playlist reader, the media port list and
  the audio decoder, without network or OBS.

Timing checks (the large-blob latency budget, follow-clock tolerances) are
set for a developer Mac; `OBS_AIRPLAY_TIMING_SCALE=3` widens them on slower
machines, as CI does. The large-blob check also makes the spool write 1.5 s
slow (`raop_set_spool_delay_for_testing`): the receiver writes reverse-fetched
media on a worker thread and must keep answering meanwhile.

`media-resilient-http` plays through `tests/truncating-server.py`, which cuts
every response short, as some sender media servers do.

Fuzzing: configure with `--preset asan -DOBS_AIRPLAY_FUZZ=ON` for
`obs-airplay-fuzz-hls`, `-fuzz-h264` (mirror access units into the software
decoder), `-fuzz-http-request` (the fork's request parser) and
`-fuzz-requests` (request sequences through the receiver's handlers: `/play`,
`/action`, playback controls and RTSP, with binary plist bodies; seeds from
`tests/fuzz/make-request-seeds.py`, receiver log with
`OBS_AIRPLAY_FUZZ_VERBOSE=1`), each with a short `fuzz-*` CTest run. With a
clang that has libFuzzer's runtime (not Apple's) they are libFuzzer binaries;
otherwise `tests/fuzz/fuzz-driver.cpp` runs seeded random mutations:
`obs-airplay-fuzz-requests -runs=1000000 -seed=7 tests/fuzz/seeds/requests-*.bin`.

Untrusted binary plists are parsed with `plist_from_bin_checked`
(`uxplay/lib/plist_checked.h`), which refuses strings and keys with a NUL
byte: libplist's getters assert on them. The release libplist is also built
with `NDEBUG`.

Leak check: `MallocStackLogging=1 leaks --atExit -- <smoke> <args>`. Note that
`leaks` exits 0 even when the program fails: take the result from a plain run.

## Threads

| Thread | Runs | Talks to |
|---|---|---|
| OBS main | module load/unload, source create/destroy, properties | everything at load/unload |
| OBS graphics/UI | source `update` (may be deferred), `video_tick` | module settings |
| Name debounce (`plugin-main`) | applies a server name 800 ms after the last edit | `UxPlayIntegration::updateServerName` |
| Status (`plugin-main`) | once a second: refresh panels, tell the helper whether OBS is live, log Bluetooth state | sources, helper preferences |
| UxPlay HTTP (`httpd`) | every RTSP/HTTP request: pairing, `/info`, `/play`, `/action`, playback controls | `MediaPlayer` play/stop/seek/rate |
| UxPlay mirror | receives and decrypts the mirror stream | `AirPlayServer::ingestVideoBitstream` (enqueues) |
| Mirror decode (`AirPlayServer`) | decodes mirror video in order, outputs to OBS | `H264Decoder`, OBS |
| UxPlay audio, NTP | RTP audio (decoded and output on this thread), clock sync | `AirPlayServer::ingestAudioBitstream` |
| Deferred clear (`UxPlayIntegration`) | clears the output 450 ms after a device leaves | `MediaPlayer::stop`, decoders |
| Media worker (per item) | demux, decode, audio presentation (150 ms ahead) | OBS audio |
| Media presenter (per item) | video presentation from a 3-frame queue | OBS video |
| Warm-up (at load) | creates the first VideoToolbox sessions | |
| Helper app (own process) | Bluetooth advertising, update checks and prompts, the swap after OBS quits | GitHub, the plugin folder |

Lock order (take left before right, never the reverse):

- `AirPlayServer`: `m_decoder_mutex` → `m_audio_decoder_mutex` → `m_sources_mutex`;
  `m_timestamp_mutex` and `m_video_queue_mutex` are leaves.
- `UxPlayIntegration`: `m_lifecycle_mutex` (start/stop/rename) is outermost;
  `m_mutex` is never held while calling out; `m_status_mutex` is a leaf.
- `MediaPlayer`: `control_mutex` → `state_mutex`; `queue_mutex` → `state_mutex`;
  `callback_mutex` is a leaf.

Frame lifetime: the planes in `MediaVideoFrame`, `MediaAudioFrame` and
`DecodedVideoFrame` belong to the decoder (an AVFrame or a locked VideoToolbox
buffer) and are valid only during the callback; libobs copies them in
`obs_source_output_video/audio`.

## The UxPlay fork

The submodule tracks branches of https://github.com/LZlamian/UxPlay
(`origin`); upstream is https://github.com/FDH2/UxPlay (`upstream`). Fork
changes are committed in `uxplay/`, the gitlink is committed in this repository,
and the fork branch is pushed before this repository's branch, so every commit
here points at a fork commit that exists on GitHub. Keep the fork's diff to
upstream small and each change attributable; prefer porting upstream commits.

The fork is based on the UxPlay v1.73.7 release (branch
`obs-airplay-rebase-1.73.7`, one commit per change on top of the tag).
To move it to a newer upstream release: `git rebase <tag>` in `uxplay/`, then
build and run every test. Upstream changes to watch for in this receiver:
callbacks the plugin relies on that upstream does not use (`conn_teardown`,
`conn_request_timing`, `mirror_connection_timing`), optional callbacks
called without a NULL check, asserts on request paths, and the shape of
`httpd_thread` (the fork's staged first read and dead-connection sweep).

## Windows

Windows x64 is built with MinGW-w64 in MSYS2's UCRT64 environment (the C
runtime OBS itself uses); why not MSVC is in
[WINDOWS_PORT_SCOPE.md](WINDOWS_PORT_SCOPE.md), testing in
[WINDOWS.md](WINDOWS.md). In an MSYS2 UCRT64 shell:

```bash
pacman -S git make perl unzip mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,nasm,pkgconf,python,ffmpeg}
for p in windows/uxplay-patches/*.patch; do git -C uxplay apply "../$p"; done
scripts/build-windows-release-deps.sh          # once; ~/.cache/obs-airplay-release-deps
scripts/windows-bonjour-sdk.sh "$HOME/bonjour-sdk"
# obs.dll: bin/64bit/obs.dll from the OBS Studio 32.0.4 Windows zip
deps="$HOME/.cache/obs-airplay-release-deps/windows-x64"
BONJOUR_SDK_HOME="$HOME/bonjour-sdk" PKG_CONFIG_PATH="$deps/lib/pkgconfig" PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig" \
  cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DOBS_AIRPLAY_STATIC_DEPS=ON -DOPENSSL_ROOT_DIR="$deps" -DOBS_LIBOBS=/path/to/obs.dll
cmake --build build-windows && ctest --test-dir build-windows
scripts/package-windows.sh                     # dist/: zip, setup.exe (Inno Setup 6), symbols
```

(`mingw-w64-ucrt-x86_64-ffmpeg` is only for the `ffmpeg` tool that makes the
test media; the build links the static FFmpeg from the deps script. Set both
`PKG_CONFIG_PATH` and `PKG_CONFIG_LIBDIR`: MSYS2's shell sets
`PKG_CONFIG_PATH` to its own libraries, which pkg-config searches first.)
The same works on Linux with mingw-w64 (posix threads):
`OBS_AIRPLAY_CROSS=x86_64-w64-mingw32` for both scripts and a CMake
toolchain file for that compiler.

Differences from macOS, all behind `_WIN32`/`WIN32`: D3D11VA instead of
VideoToolbox (`src/hw-decode-platform.hpp`); no discovery helper, BLE or
self-updater (`src/airplay-ble-helper-stub.cpp`); the DLL exports only
`obs_module_*` through a `.def` made from `src/obs-airplay.exports`, and
carries its MinGW runtime. UxPlay's Windows changes are the patches in
`windows/uxplay-patches/` (made with `git format-patch` from a branch on
the pinned commit), applied by CI until they are committed to the fork:
build fixes, DNS-SD through the Windows DNS service when Bonjour is missing,
private spool files, a lingering close after 413, no llhttp exports. The
macOS build does not apply them, so plugin code that needs them stays
behind `_WIN32` until the submodule moves.

## Releasing

1. Set the version in `CMakeLists.txt` (`project(obs-airplay VERSION X.Y.Z)`);
   both Info.plists are generated from it. Write
   `docs/releases/RELEASE_NOTES_vX.Y.Z.md` and move the changelog entry.
2. Push the fork branch, then this branch, and tag `vX.Y.Z`.
3. CI builds and tests, and after a maintainer approves the `release`
   environment attaches the zip, pkg, `SHA256SUMS` and (with the signing key as
   a secret) `<zip>.sig` to a draft release. Locally, the same:
   `scripts/build-macos-release-deps.sh` (once; cached in `~/Library/Caches`),
   then `OBS_AIRPLAY_DEPS_PREFIX=~/Library/Caches/obs-airplay-release-deps/arm64-macos12.0
   OBS_AIRPLAY_RELEASE_TAG=vX.Y.Z scripts/package-macos.sh` (paths without
   spaces). It signs the zip with `~/.obs-airplay-release-signing/ed25519.pem`
   via `scripts/sign-release.sh`.
4. Check `obs-airplay-updater-smoke --verify-release dist/obs-airplay-vX.Y.Z-macos-arm64.zip`.
5. Publish the release (not a draft, not a pre-release) with the zip, its
   `.sig`, the pkg and `SHA256SUMS`. The self-updater needs: the asset name
   `obs-airplay-vX.Y.Z-macos-<arch>.zip`, the plugin as its only top-level
   entry (`ditto -c -k --keepParent`), bundle version equal to the tag, an
   intact code seal, the `.sig` next to it, and GitHub's published digest.
   Do not replace assets of a published release: bump the version instead.
6. Windows: with the repository variable `OBS_AIRPLAY_RELEASE_WINDOWS` set to
   `true` (Settings > Secrets and variables > Actions > Variables), the
   `release-windows` job attaches the tag's Windows packages
   (`obs-airplay-vX.Y.Z-windows-x64.zip`, `-setup.exe`, `-symbols.zip`,
   `SHA256SUMS-windows`) to the draft after the same approval. Leave it unset
   until Windows has been tested on devices; the `windows` job still builds
   and uploads the packages as a CI artifact for every push and tag.

The release signing key's public half is in `kTrustedUpdateKeys`
(`src/plugin-updater.mm`). To rotate it, ship one release signed by the old
key that trusts both keys, then drop the old one.
