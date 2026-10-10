# OBS AirPlay v2.4.0

From this release on, the plugin can update itself: one click in the update prompt, and the new version is installed when you quit OBS.

## What's new

- **Install Update.** When a newer release is available, the prompt from **OBS AirPlay Discovery** now offers **Install Update** next to View Release, Later and Skip This Version. The release is downloaded in the background while OBS keeps running, and the plugin is replaced when you quit OBS. The next time OBS starts it runs the new version.
- **No administrator password.** Only the copy in your own `~/Library/Application Support/obs-studio/plugins` is replaced, so each macOS account updates its own plugin.
- **Checked before it is installed.** The download must match the SHA-256 digest GitHub publishes for the release file, must be this plugin at the announced version for your Mac, and its code seal is verified after the download and again just before installing. Anything else is discarded and the installed plugin is left as it is.
- **Nothing happens without a click.** The plugin still only checks once a day; it downloads and installs only after you choose **Install Update**.

If the plugin is in a place you cannot write to, or a release has no zip for your Mac, the prompt offers the release page as before. An update that is waiting can be cancelled by deleting the hidden folder `.obs-airplay.update` next to the plugin.

## This one update is still manual

Versions up to v2.3.0 can only tell you about a release. Install v2.4.0 once with the installer; later versions can then be installed from the prompt.

## Housekeeping

- A change to the discovery helper alone now always reaches the plugin bundle in incremental builds.
- New `obs-airplay-updater-smoke` test for the update checks, staging and install.

## Updating

Close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS. The zip is also available for manual installation.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
