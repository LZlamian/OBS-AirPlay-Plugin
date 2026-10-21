# Changelog

Release notes for each version are in [docs/releases/](docs/releases/).

## Unreleased (2.7)

Audit 2026-10-21, Phase 1:
- "From any web address" now fetches from the address it checked: plain HTTP
  connects to it with the name in the Host header, HTTPS relies on the
  certificate; redirects are followed by the plugin, each one checked before
  it is requested (NET-5).
- `stop()` and quitting OBS no longer wait for a slow DNS lookup (MP-1).
- Mirror audio: starts with a 40 ms jitter margin, follows the sender's clock
  after the first RTP sync instead of drifting, and keeps startup packets
  that are only a little late (MP-2).
- A failed rename is shown and can be retried; the over-long-name warning
  appears at once; name edits no longer rewrite settings.json per keystroke
  (MP-6, MP-7, MP-11).
- Races and robustness: hardware-error counter (MP-3), unload order (MP-4),
  huge sender positions (MP-5), a VideoToolbox surface held while the screen
  is still (MP-8), refused redirects retried (MP-9), HLS attribute names
  (MP-10), thread creation on UxPlay's thread (MP-12).
- The last picture of an AirPlay video item is shown for its own duration
  before the item ends (a still clip's last frame used to end it at once).

Audit 2026-10-21, Phase 2:
- Release pipeline: actions pinned by commit and no stored token in
  checkouts (SC-4); warnings are errors in CI and a weekly ThreadSanitizer
  run (BC-2, BC-3); the release job runs the tests against the release
  FFmpeg it ships (BC-1); dependencies are built in a private folder (SC-5);
  `SHA256SUMS` is signed with the release key, which covers the `.pkg`
  (SC-6); csrp and LGPL notices (SC-7).
- The updater cancels a download larger than the release lists, and a
  release check larger than 1 MiB (SC-3).
- Tests: `AirPlayServer` with libobs stubbed (decode thread, backlog,
  clock anchor, re-timer wiring); orientation, still clips, audio-only,
  10-bit (software fallback), live and AES-128 HLS, two renditions; deadlines
  scale with `OBS_AIRPLAY_TIMING_SCALE`; the protocol smoke names a failed
  condition; configurable test ports (TS-2 to TS-5).

## 2.6.1 (2026-10-21)

Security hotfix (audit 2026-10-21, Phase 0):
- HLS master playlists from a sender (upstream UxPlay code): the audio
  language parser no longer overflows on a long `LANGUAGE`, divides by zero
  on a single entry or reads `languages[-1]` without a default (NET-1, NET-2,
  NET-3); the media URI table no longer reads past the playlist, and an
  empty one no longer leaves a zero-length store. Fork commits `99cbde2`,
  `d324806`, `1beb90d`.
- Mirror codec packets: SPS/PPS/VPS lengths are unsigned and checked against
  the payload (NET-4, fork `4a077a8`).
- "From any web address": a host name that does not resolve is refused
  (NET-5, first part).
- The release job no longer restores the dependency cache (SC-1).
- CI runs every test, with the fuzz targets, under ASan and UBSan; new
  `obs-airplay-fuzz-master-playlist`; the protocol smoke sends the NET-1/2/3
  playlists (TS-1).

## 2.6.0 (2026-10-18)

Receiver setting **AirPlay video** (Off / Only from the sending device, the
default / From any web address). Video from websites, allowed in earlier
versions, now needs From any web address:
- **Off**: POST /play (and an item swapped in with playlistInsert) is
  answered 400, so the device falls back or gives up; screen mirroring is
  unaffected.
- **Only from the sending device**: the item URL must be the sender's own
  address, and redirects and HLS sub-resources must stay on it; other hosts
  are refused.
- Every refusal is logged with its reason, and the latest one is shown on the
  source's Status line.

Security and network hardening:
- AirPlay video URLs are checked: no loopback, link-local (except the sender),
  multicast or reserved targets, web ports only on other hosts; resolved
  addresses, redirects and HLS sub-resources are checked too. HTTPS is
  verified (new option: allow self-signed media servers).
- Request bodies are limited (16 MiB, or 129 MiB for a reverse-fetch reply),
  with a 256 MiB shared budget and a 512 MiB spool budget; 413 when over.
- A peer that stops reading cannot block the receiver; no SIGPIPE.
- The request parser no longer reads past received data for the protocol
  (found by fuzzing).
- A request whose binary plist holds a text value with a NUL character can
  no longer crash OBS (libplist asserted on it); found by fuzzing the request
  handlers. The bundled libplist is now built without asserts as well.
- Only the device that started AirPlay video can control or end it.
- The media trace redacts URL query strings and credentials.
- Updates require an Ed25519 signature (`<zip>.sig`) and are re-checked in
  full before the swap; only a plugin in the user's home folder updates
  itself; the previous version is kept until the new one starts.

Reliability and usability:
- Receiver settings are shared by all AirPlay sources; the saved name is used
  from the start.
- The source panel shows what the receiver is doing and why it is not working.
- The update prompt can be turned off, never appears while streaming or
  recording, and does not take the keyboard focus; "Later" means a week.
- Seeks in MPEG-TS/HLS land exactly; commands after the end of an item work;
  decoder fixes for colour matrix, parameter sets and multi-picture payloads.
- The pkg installs one copy, for the logged-in user.

Performance:
- AirPlay video: no CPU copy of hardware frames; decoding runs ahead of
  presentation; audio is sent 150 ms early.
- Mirroring: decoding on its own thread; VideoToolbox warmed up at load; the
  discovery helper starts in the background; fewer wakeups and allocations.
- Release builds are smaller (dead-stripped, only module entry points exported).
- Reverse-fetched (page-local) media is written to its spool file on a worker
  thread; the receiver keeps answering while it is written.
- Audio streams are decoded in the format the sender negotiates (48 kHz,
  24-bit ALAC as well as 44.1 kHz), with the RTP clock at the same rate;
  until now every stream was assumed to be 44.1 kHz stereo.
- Mirrored audio no longer reaches OBS late. Its timestamps stay continuous
  and follow the sender only while that is on time: the burst of queued
  packets a stream starts with is dropped, the jump back when the sender's
  first clock sync arrives is ridden over, and after a stall the stream
  restarts at the current time. Until now OBS raised its audio buffering to
  the 960 ms maximum at the start of mirroring, adding that delay to every
  audio source until OBS was restarted.

Project:
- The UxPlay fork is rebased onto the UxPlay v1.73.7 release (iOS 27
  mirroring-stop fix and other upstream changes); two upstream crashes on a
  missing optional callback or an early `/action` are guarded.
- GPL-3.0-or-later with all third-party licenses shipped; CTest runs every
  smoke; CI on Apple silicon and Intel; presets for sanitizer builds; unit
  tests and fuzz targets for network-facing parsers.
