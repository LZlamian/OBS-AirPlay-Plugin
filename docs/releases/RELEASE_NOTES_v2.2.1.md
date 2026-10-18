# OBS AirPlay v2.2.1

This maintenance release fixes the OBS AirPlay Discovery helper being reported as "Not Responding".

## Fixes

- **Discovery helper no longer shows as "Not Responding".** The helper (the small background app that advertises the receiver over Bluetooth and checks for updates) ran a plain run loop instead of the application event loop, so macOS Activity Monitor flagged it as hung and counted hangs even though it was idle at 0% CPU. It now runs the normal event loop.
- **The helper always exits with OBS.** Its check that OBS is still running did not fire while the update prompt was on screen, so the helper could stay behind after OBS quit. The check now keeps running during the prompt.

No change to AirPlay mirroring or AirPlay video playback. See the [v2.2.0 release notes](RELEASE_NOTES_v2.2.0.md) for the features in this series.

## Notes

- The helper's "Virtual Memory Size" of several hundred gigabytes in Activity Monitor is reserved address space, which every process on Apple silicon shows. Its real memory use is about 26 MB.

## Updating

Close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS. The zip is also available for manual installation.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
