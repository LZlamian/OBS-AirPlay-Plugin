# OBS AirPlay v2.3.0

This release moves video decoding to the Mac's hardware decoder (VideoToolbox), for screen mirroring and for AirPlay video, and fixes a set of stability and security problems found in a full code review.

## What's new

- **Hardware decoding for screen mirroring.** H.264 and HEVC mirror streams are decoded by VideoToolbox and handed to OBS without a colour conversion on the CPU. On an M4 Mac mini a 1080p frame costs about 0.2 ms of CPU, against 2.5 ms (H.264) or 4 ms (HEVC) in software.
- **Hardware decoding for AirPlay video.** Files played through AirPlay video (Safari, AVPlayer apps) are decoded in hardware too. A 4K60 H.264 clip uses about a third of the CPU it did before.
- **Automatic software fallback.** If the hardware decoder is unavailable, does not accept a stream, or fails repeatedly, decoding continues in software for that stream; the next stream starts in hardware again. Software decoding of AirPlay video is now multi-threaded.
- **Portrait recordings play upright.** AirPlay video now applies the rotation a phone stores with a portrait recording; such clips used to appear on their side.
- **Same picture in hardware and software.** Mirror streams are full range; the software path now passes them through as full range, as the hardware path does, rather than converting every frame to video range.

To force software decoding for troubleshooting, start OBS with `OBS_AIRPLAY_HW_DECODE=0`, or create the file `~/Library/Application Support/obs-studio/obs-airplay-software-decode`, then restart OBS.

## Fixes

- **OBS no longer goes black when an app cancels AirPlay video.** When a sender started and immediately cancelled an AirPlay video item while still mirroring, the mirror decoder was reset and showed nothing until the next keyframe, which iOS might not send. The mirror decoder now keeps running across AirPlay video start and stop.
- **A malformed request can no longer crash OBS.** A device on the local network could crash OBS by sending a reverse-fetch reply (`POST /action`) for an item that never requested one. Such replies are now rejected, and the same path no longer leaks the reply's data.
- **A second mirror stream on one connection is safe.** Re-keying the mirror decryption for a new stream could free state the mirror thread was still using.
- **`/stop` cancels a media fetch in progress.** A sender's late reply to a fetch that was already stopped could start playback again.
- **A few bad packets no longer cost hardware decoding.** Only a sustained run of undecodable packets, or real hardware failures, switches a stream to software.
- **A decoder that could not be reopened recovers** at the next stream, not only after restarting OBS.
- **Renaming the receiver is safe while a device connects.** The previous advertisement was freed while request handlers could still read it; a failed rename now leaves the old name working.
- **An item swapped in while paused stays paused.** When a sender replaced the current AirPlay video item while paused, the new item started playing in OBS. It now takes the sender's last playback rate: OBS shows its first frame and waits.
- **OBS follows a scrubber drag.** While paused, each scrub decoded forward to the exact position and a newer scrub cancelled it, so a drag showed only a few pictures until the finger slowed down. During a drag OBS now shows the keyframe at or before each position at once, and the exact frame when the drag stops. A single jump still goes straight to the exact frame.
- **An audio-only item that starts paused waits** for playback to start; it used to be decoded straight to its end.
- Each mirror stream starts with fresh decoder state.
- Shutdown stops the network thread before the media player, so a late `/play` cannot race it.
- Temporary media files left behind by a crash are removed at the next start (after a day), and all items of a session are released when its last connection closes.

## Performance

- Large sender-hosted media bodies are received about twice as fast (the request buffer now grows geometrically).
- Mirror audio no longer waits for a video frame to be decoded.
- Decode errors are logged at most once per 120 consecutive errors.

## Diagnostics

- `[DECODE]` lines report the mirror decoder's mode, picture size, pixel layout and range whenever they change; a `[MIRROR]` line every ten seconds gives size, frame rate and decode mode. AirPlay video logs its mode once per item.

## Housekeeping

- Removed the unused built-in socket server and mDNS publisher (about 1,100 lines). UxPlay handles all networking and advertising.
- Documentation no longer describes changing ports through the removed server.

## Known limits

- HDR (10-bit) AirPlay video is converted to 8-bit without tone mapping, as before, and may look washed out.

## Updating

Close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS. The zip is also available for manual installation.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
