# OBS AirPlay v2.1.0

This release adds Safari Media AirPlay while preserving the existing native-app and screen-mirroring path.

## What's new

- Added Safari's HTTP media playback mode, including `/reverse`, `/play`, `/playback-info`, `/rate`, `/scrub`, and related property messages.
- Added receiver-side fetching and playback for public MP4 and HLS URLs.
- Added AirPlay reverse-channel fetching for page-local `blob:` MP4 media used by imported, generated, and offline Safari videos.
- Added H.264 and HEVC video decoding, AAC audio decoding, HLS rendition selection, seeking, pause/resume, and OBS-clock presentation.
- Added secure temporary spooling for reverse-fetched media with mode `0600`, automatic cleanup, validated macOS temporary directories, and a 128 MiB request limit.
- Fixed Safari reverse-fetch success responses that use status code `0` instead of an HTTP 2xx value.
- Fixed malformed `/action` messages that could access uninitialized cleanup state and crash OBS.
- Fixed old media workers and retained OBS frames remaining visible after playback stopped, failed, or switched sources.
- Added focused media and protocol logging without exposing full page-local media URLs.

## Verification

The release was tested with:

- Native AirPlay protocol and playback-control regression paths
- Public H.264/AAC MP4
- Public Apple HLS with audio and video
- Local H.264/AAC MP4 and HLS
- HEVC/AAC MP4 and video-only MP4
- Safari-style reverse-channel `blob:` transfer using the observed success status `0`
- An exact 96,168,758-byte blob transfer matching the captured device case
- Repeated malformed and out-of-session `/action` messages
- Secure spool permissions and cleanup after source replacement

## Updating

Close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS. The zip is also available for manual installation.

## Compatibility and limitations

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
- Public MP4/HLS media must be reachable by the Mac running OBS.
- Safari page-local media is transferred completely before playback begins and is limited to 128 MiB.
- Reverse-fetched page-local media currently supports ISO-BMFF MP4 payloads.
- DRM-protected and encrypted MediaSource content is not supported.
- Intel macOS, Windows, and Linux release artifacts are not included yet.
