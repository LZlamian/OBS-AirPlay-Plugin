# OBS AirPlay v2.2.0

This release adds standard iOS "AirPlay video" from native apps that play local files with AVPlayer, and makes AirPlay video playback follow the sender closely enough for live presentation use.

## What's new

- **AVPlayer local-file AirPlay video.** iOS apps that set `allowsExternalPlayback` on an AVPlayer playing an app-local MP4/M4V/MOV now play in OBS. iOS sends these `/play` requests without a `Content-Location`: the sender serves the file from its own media server and describes it with `mediaType="file"`, `host`, and `path`. The receiver now fetches it from the sender's connection address (adding the IPv6 link-local zone iOS omits). It never fetches from any other host.
- **Item switches (`playlistInsert`).** When the sender replaces the item on the same player, iOS sends `POST /action` `playlistRemove` + `playlistInsert` instead of a new `/play`. UxPlay had never implemented `playlistInsert`; it now plays the new item exactly like `/play`, and later scrub/rate/stop requests apply to it.
- **Sender-synchronised playback controls.**
  - Starting mid-file and seeking no longer play the frames between the previous keyframe and the target, so OBS no longer trails the sender after a seek.
  - Seeking while paused shows the frame at the new position.
  - An item that starts paused (for example a still-image clip) shows its first frame.
  - Rate changes continue from the last frame shown, so sparse still clips keep their full length.
  - A frame that is waiting when playback pauses is shown on resume instead of dropped.
  - Small corrective scrubs within 0.35 s of the current position, while playing, are ignored rather than causing a keyframe re-decode hitch.
- **Accurate `/playback-info`.** While an item opens, it reports the requested start position instead of 0. When the sender sets `actionAtItemEnd` to pause, a finished item is held on its last frame with `position = duration` instead of shutting the session down.
- **No blank frames during switches.**
  - A new item keeps the previous frame on screen until its own first frame arrives.
  - `/stop` clears the output only if no new item or mirroring follows within 450 ms.
  - iOS tearing down its idle mirror stream no longer blanks an AirPlay video item.
  - If the sender closes its AirPlay video connections without `/stop`, playback stops and the output clears instead of freezing on the last frame.

## Fixes

- **Screen mirroring resumes after AirPlay video.** When iOS returned to mirroring on the same connection after an AirPlay video session, UxPlay re-keyed the mirror decryption but kept the previous stream's partial-block state, so every packet of the resumed mirror decoded as garbage and OBS stayed black. Re-keying now resets that state; the phone's screen comes back about 100 ms after the sender stops.
- **Robust transfers from sender media servers.** Progressive HTTP media (MP4/MOV) now goes through a byte-exact input layer: whenever a server ends a response early or drops a kept-alive connection, it continues with a fresh range request at the exact offset. Previously FFmpeg reported end of file mid-sample, which lost keyframes (still images showed nothing) or stopped playback during scrubbing.
- A read that ends well before the known duration re-requests from the current position; genuine ends (all data read, or demuxed packets covering the stated duration) are never retried, so short still clips cannot cause request storms.
- Only a decoded mirror picture cancels the post-`/stop` clear; mirroring codec headers alone no longer leave a frozen video frame on screen.
- A rejected `POST /play` no longer disconnects the AirPlay connection. It answers `400` and keeps the session, so iOS can fall back instead of dropping the whole connection.
- Every previously silent `/play` failure path now logs a specific reason, for example a binary plist that does not parse, a non-dictionary root, or a missing `Content-Location`.
- `playlistRemove` while no item is current is accepted instead of being rejected.

## Diagnostics

- New `[MEDIA-TRACE]` protocol trace logs every AirPlay video request and response:
  - `/play`, `/reverse` and the PTTH upgrade, `/action`, `/scrub`, `/rate`, `/stop`, `/setProperty`, `/playback-info`, and reverse-HTTP replies
  - the relevant headers, plus a compact decoded dump of binary-plist, XML-plist, or text bodies, with long strings truncated
- The trace is off by default. Enable it by starting OBS with `OBS_AIRPLAY_MEDIA_TRACE=1`, or by creating the file `~/Library/Application Support/obs-studio/obs-airplay-media-trace`, then restarting OBS. This also raises UxPlay's log level so the existing `[MEDIA]` lines appear in the OBS log.

## Verification

- The exact AVPlayer `/play` and `playlistRemove`/`playlistInsert` requests captured from an iPhone SE 2 (iOS 26), replayed in the protocol smoke test, including:
  - URL construction
  - start position
  - repeated `/play` for the same item
  - controls reaching an inserted item
  - rejected `/play` keeping the connection open
  - closing the last AirPlay connection stopping playback
- Media smoke tests for:
  - mid-file start without pre-roll
  - ignored small corrections
  - pause, seek-while-paused preview, and resume
  - a paused first frame
  - a sparse two-frame still clip receiving `rate=1` right after its first frame
- Regression: public and local MP4, Apple HLS startup, Safari `blob:`/`file:` reverse-fetch paths, malformed `/action` messages.
- Media tests against HTTP servers that cap range responses and drop connections, like on-device media servers: still clips with the index at the front or end, scrub drags, pause/seek/resume, paused-video → still switches.
- On device: AVPlayer local-file AirPlay video from an iOS app, with play/pause, scrubbing, repeated switches between videos, still images and black clips, and screen mirroring resuming after the app stops AirPlay video.

## Updating

Close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS. The zip is also available for manual installation.

## Compatibility and limitations

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
- Sender-hosted media must be reachable from the Mac over the same connection the sender uses; the sender's media server must support HTTP range requests (AVPlayer's does).
- Playback rates other than 0 and 1 apply to video; audio is not time-stretched.
- DRM-protected content is not supported.
