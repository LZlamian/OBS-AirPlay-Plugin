# OBS AirPlay v2.5.1

One fix: the AirPlay source no longer keeps a device's last frame after the device has disconnected.

## Fixes

- **The source goes empty when a device disconnects.** When a device ended its whole AirPlay session, for example by stopping Screen Mirroring, or when an app stopped AirPlay video and the device then dropped the connection, the last picture it had sent stayed frozen in OBS until another device connected. The source is now cleared about half a second after the device ends its session or its last connection closes, and the decoders start fresh for the next stream.
- **No empty source in between.** The half second is deliberate: a device that reconnects at once, or an app that switches to its next item, replaces the picture directly.
- An AirPlay video item that keeps playing after iOS ends its mirroring session is not affected.

## Known limits

- A device that disappears without closing its connection (Wi-Fi lost, out of range) can still leave its last frame until the connection times out.

## Diagnostics

- The OBS log says why the output was cleared: `[MEDIA] the device ended its AirPlay session; clearing output` or `[MEDIA] the last AirPlay connection closed; clearing output`.

## Updating

- **From v2.4.0 or later:** click **Install Update** in the prompt, then quit OBS when it suits you. The plugin is replaced as OBS quits.
- **From v2.3.0 or earlier:** close OBS Studio, download the Apple silicon `.pkg` installer, run it, and reopen OBS.

## Compatibility

- macOS 12 Monterey or later
- OBS Studio 28 or later
- Apple silicon (`arm64`)
