# OBS AirPlay v2.6.0

The receiver is harder to attack from the local network, updates are signed,
and screen mirroring and AirPlay video are faster and lighter on the CPU. The
AirPlay code underneath is now based on the UxPlay v1.73.7 release.

## Security

- **AirPlay video can only fetch from sensible places.** A sender can no longer
  point the receiver at your Mac itself, at link-local or multicast addresses,
  or at unusual ports on other hosts. Redirects and HLS playlists, segments and
  keys are checked the same way. HTTPS media servers must have a valid
  certificate; turn on **Allow self-signed media servers** in the source's
  properties if yours does not.
- **Large or stalled requests cannot exhaust the receiver.** Request bodies are
  limited, as is the memory and disk space they may take together. A device
  that stops reading responses is disconnected instead of blocking everyone
  else.
- **You decide what AirPlay video may fetch.** With AirPlay video, the device
  sends the receiver a web address and your Mac downloads the video itself.
  Two new settings in the source's properties control that: turn off
  **Accept AirPlay video** to allow only screen mirroring, or turn on **Only
  fetch AirPlay video from the sending device** to keep videos played from
  the device's own apps and pages working while refusing other web
  addresses. Each refused video is named, with the reason, in the OBS log
  and on the source's Status line.
- **Only the device that started an AirPlay video can control or stop it.**
- **Updates must be signed.** From this version on, an update is installed only
  if it carries a valid signature by the project's release key, in addition to
  the existing checks. It is checked again just before the plugin is replaced,
  only a plugin in your own home folder updates itself, and the previous
  version is kept until the new one has started.
- The media trace removes query strings and passwords from the URLs it logs.
- Problems found by fuzzing the receiver's request parser are fixed.

## Reliability and usability

- **All AirPlay sources share one set of receiver settings**, and the saved
  server name is used from the moment OBS starts.
- **A Status line in the source's properties** shows what the receiver is
  doing, and why when it is not working (for example, both ports in use).
- **The update prompt is less intrusive.** It can be turned off (**Check for
  plugin updates once a day**), never appears while you are streaming or
  recording, does not take the keyboard focus, and **Later** means a week.
- AirPlay video: seeks in MPEG-TS and HLS land where they should, and play
  and seek commands after the end of a video work.
- Mirroring: colours are right for standard-definition streams, and streams
  that change their parameters mid-way decode correctly.
- Nearby devices keep finding the receiver over Bluetooth after it has been
  idle.
- The installer puts one copy of the plugin in your user folder and removes
  the system-wide copy that earlier installers also left.

## Performance

- AirPlay video: hardware-decoded pictures go to OBS without a CPU copy (about
  half the CPU time for 4K), and decoding runs ahead of presentation.
- Mirroring: the hardware decoder is warmed up when OBS starts, so the first
  picture appears sooner; decoding runs on its own thread, and the receiver
  wakes up less often.
- The plugin is smaller.

## Updating

- **From v2.4.0 or later:** click **Install Update** in the prompt, then quit
  OBS when it suits you. The plugin is replaced as OBS quits.
- **From v2.3.0 or earlier:** close OBS Studio, download the Apple silicon
  `.pkg` installer, run it, and reopen OBS.

The plugin's settings move to
`~/Library/Application Support/obs-studio/plugin_config/obs-airplay/`; the
troubleshooting flag files in their old place still work.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 32 (built against its headers; earlier versions are untested)
- Apple silicon (`arm64`)

The project is now licensed GPL-3.0-or-later, with every bundled component's
license in the plugin bundle. See the [changelog](../../CHANGELOG.md) for the
full list of changes.
