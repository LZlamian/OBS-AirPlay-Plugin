# OBS AirPlay v2.6.1

A security hotfix. **Please update.** Version 2.6.0 and earlier can be crashed by any device on the local network: no pairing and no PIN are needed. Version 2.6.1 closes those holes. It has no new features.

## Security fixes

- **An HLS playlist can no longer crash OBS.** The receiver's AirPlay video code (from upstream UxPlay) chooses an audio language from the playlist a sender returns for its own HLS video. A language code longer than six characters overflowed a buffer, a playlist with exactly one language divided by zero, and one with no default language read outside its list. These are now checked, and a playlist that does not have the expected shape is used as it is. The same playlist code no longer reads past the end of a playlist that ends in `m3u8`, or of an empty media list.
- **A screen-mirroring packet can no longer crash OBS.** The lengths of the video parameter sets in the mirror stream's codec packet are now checked against the packet before they are read; a packet that does not fit is dropped. Before, a peer that set up a mirroring session could make the receiver read and copy outside the packet.
- **"From any web address" refuses a host name that does not resolve.** Before, a lookup error let the URL through, and FFmpeg looked the name up again on its own; a name that timed out for the check could then point somewhere the check would have refused. The default, "Only from the sending device", was not affected.

All three crashes come from code shared with upstream UxPlay; the fixes are offered upstream too.

## Release pipeline

- The release build no longer restores its FFmpeg build from a cache that any push could have filled; it builds it from the checksummed sources every time.
- The fuzz tests now run in CI, under AddressSanitizer and UndefinedBehaviorSanitizer, together with every other test (before, only part of the tests ran there and the fuzz tests ran only locally). A new fuzz test covers the HLS playlist code above and finds the old crashes in seconds.

## Updating

- **From v2.4.0 or later:** click **Install Update** in the prompt, then quit OBS when it suits you. The plugin is replaced as OBS quits.
- **From v2.3.0 or earlier:** close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
