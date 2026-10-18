# OBS AirPlay v2.0.1

This maintenance release focuses on safer updates, steadier playback, and more portable release builds.

## What's new

- Added an update notification that checks the latest stable GitHub release at most once per day. It can open the release page, defer the reminder, or skip that version; it never downloads or installs updates silently.
- Mapped AirPlay audio and video timestamps onto one OBS clock to reduce drift and improve synchronization during longer sessions.
- Reused the FFmpeg audio resampler between packets and reset it cleanly when a stream is flushed.
- Made receiver startup failures explicit instead of leaving a source that appears available but cannot accept connections.
- Hardened release packaging with architecture and minimum-macOS validation, portable x86 compiler settings, runtime dependency checks, code-signing hooks, and bundled third-party license notices.
- Added a reproducible, checksum-pinned dependency build for macOS 12 release artifacts.

## Updating

Close OBS Studio, download the `.pkg` installer for your Mac from this release, run it, and reopen OBS. The zip is also available for manual installation.

The update notification is introduced by v2.0.1, so it will notify users about later stable releases after this version is installed.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)

Intel macOS, Windows, and Linux builds are not included in this release yet.
