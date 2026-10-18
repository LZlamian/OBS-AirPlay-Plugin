# Changelog

Release notes for each version are in [docs/releases/](docs/releases/).

## Unreleased

Receiver settings for AirPlay video:
- **Accept AirPlay video** (on by default): when off, POST /play (and an
  item swapped in with playlistInsert) is answered 400, so the device falls
  back or gives up; screen mirroring is unaffected.
- **Only fetch AirPlay video from the sending device** (off by default): the
  item URL must be the sender's own address, and redirects and HLS
  sub-resources must stay on it; other hosts are refused.
- Every refusal is logged with its reason, and the latest one is shown on the
  source's Status line.

## 2.6.0 (2026-10-18)

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

Project:
- The UxPlay fork is rebased onto the UxPlay v1.73.7 release (iOS 27
  mirroring-stop fix and other upstream changes); two upstream crashes on a
  missing optional callback or an early `/action` are guarded.
- GPL-3.0-or-later with all third-party licenses shipped; CTest runs every
  smoke; CI on Apple silicon and Intel; presets for sanitizer builds; unit
  tests and fuzz targets for network-facing parsers.
