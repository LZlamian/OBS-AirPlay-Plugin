# OBS AirPlay v2.7.0

Follows the 2.6.1 security hotfix with the rest of the 2026-10-21 audit: safer fetching for "From any web address", steadier mirror audio, many robustness fixes, and a release pipeline you can check.

## AirPlay video

- **"From any web address" fetches from the address it checked.** Plain HTTP connects to the checked address and sends the site's name in the request, so a name that changes its answer after the check (DNS rebinding) cannot point the fetch elsewhere. HTTPS keeps relying on the site's certificate, which already binds the connection to the name. Redirects are followed by the plugin, and each one is checked before it is requested. The default, "Only from the sending device", was already safe.
- **Quitting OBS or stopping the receiver no longer waits for a slow name lookup.**
- **A still clip's last picture stays up for its own duration** instead of ending the item at once.
- Huge positions from a sender, refused redirects and unusual HLS attributes are handled correctly.

## Mirroring

- **Steadier audio.** Mirror audio starts with a small 40 ms margin against network jitter, follows the sender's clock instead of slowly drifting over long sessions, and keeps startup packets that are only a little late.
- Fixes for rare races at unload and in hardware decoding, and for a video frame held while the screen is still.

## Receiver name

- A rename that fails is shown and can be retried by typing the name again; the "name too long" warning appears at once; typing a name no longer rewrites the settings file on every keystroke.

## Updates and downloads

- **`SHA256SUMS` is signed** with the release key, so the checksums, including the `.pkg`'s, can be verified (see INSTALL.md); the public key is in `docs/release-signing-key.pem`.
- The updater stops a download that grows past the size the release lists.
- `diagnose.sh` now ships inside the plugin (`obs-airplay.plugin/Contents/Resources/diagnose.sh`).

## Under the hood

- The release build runs the tests against the FFmpeg it ships; CI actions are pinned, warnings are errors, and ThreadSanitizer runs weekly.
- Many new tests: the mirror decode thread and clock, orientation, still clips, audio-only, 10-bit, live, encrypted and multi-rendition HLS.
- Code cleanup in the plugin and the UxPlay fork; quieter logs at the default level, every line tagged.

## Updating

- **From v2.4.0 or later:** click **Install Update** in the prompt, then quit OBS when it suits you. The plugin is replaced as OBS quits.
- **From v2.3.0 or earlier:** close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
