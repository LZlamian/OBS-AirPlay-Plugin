# Installing and troubleshooting OBS AirPlay

## Install

1. Quit OBS.
2. Download `obs-airplay-vX.Y.Z-macos-arm64.pkg` from the
   [Releases page](https://github.com/LZlamian/OBS-AirPlay-Plugin/releases)
   and open it. Releases are not signed with an Apple Developer ID: when macOS
   warns, click **Done**, open **System Settings > Privacy & Security**, and
   click **Open Anyway** at the bottom.
3. The installer puts the plugin in your own plugins folder:
   `~/Library/Application Support/obs-studio/plugins/obs-airplay.plugin`.
   Instead of the `.pkg` you can unzip `obs-airplay-vX.Y.Z-macos-arm64.zip` and
   copy `obs-airplay.plugin` into that folder.
4. Start OBS and add an **AirPlay** source. If macOS asks whether
   **OBS AirPlay Discovery** may use Bluetooth, allow it (faster discovery).

To check that it loaded: **Help > Log Files > View Current Log** should contain
`OBS AirPlay Plugin loaded (version X.Y.Z)` and
`UxPlay integration active on port 7000`.

### Checking a download (optional)

`SHA256SUMS` on the release lists the files' checksums and, from v2.7.0,
`SHA256SUMS.sig` signs it with the project's release key (the key the
self-updater trusts, in `docs/release-signing-key.pem` in the repository).
With OpenSSL 3 (`brew install openssl@3`; macOS's own `openssl` cannot check
Ed25519), in the folder with the downloads:

```bash
/opt/homebrew/opt/openssl@3/bin/openssl pkeyutl -verify -pubin -inkey release-signing-key.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig
shasum -a 256 -c SHA256SUMS --ignore-missing
```

Later versions install themselves when you choose **Install Update** in the
prompt (see the README); turn the daily check off in the source's properties (under **Advanced settings**).

## Uninstall

Quit OBS, then delete from `~/Library/Application Support/obs-studio/plugins/`:
`obs-airplay.plugin`, and `.obs-airplay.update` / `.obs-airplay.previous` if
they exist (hidden: press Cmd-Shift-. in Finder). Settings are in
`~/Library/Application Support/obs-studio/plugin_config/obs-airplay.so/`; the
helper's preferences go with `defaults delete com.lzlamian.obs-airplay.discovery`.
Versions before 2.6 also installed a copy in
`/Library/Application Support/obs-studio/plugins/`: delete it too.

## Troubleshooting

Start with the source's **Status** line (right-click the source > Properties):
it says whether the receiver runs, on which port, whether a device is connected,
and what is wrong when it does not run. Then run the diagnostics script while
OBS is open; it needs no administrator rights and changes nothing. It comes
inside the plugin (from v2.7.0):

```bash
bash ~/Library/Application\ Support/obs-studio/plugins/obs-airplay.plugin/Contents/Resources/diagnose.sh
```

(from a source checkout: `./diagnose.sh`).

### Devices do not list the receiver

- The Mac and the device must be on the same network and VLAN. Guest networks
  and some mesh or enterprise networks block Bonjour between devices; a VPN on
  the Mac can take the default route away from Wi-Fi.
- `dns-sd -B _airplay._tcp` (Ctrl-C to stop) should list the receiver's name.
  If it does not, check the OBS log for `Bonjour registration failed`.

### The receiver is listed but connecting fails

- **Port 7000**: the receiver uses TCP 7000, or 7001 when 7000 is taken.
  `lsof -nP -iTCP:7000 -sTCP:LISTEN` shows who has it. macOS's own AirPlay
  Receiver (`ControlCenter`) uses 7000: turn it off in **System Settings >
  General > AirDrop & Handoff > AirPlay Receiver**, then restart OBS.
- **Firewall**: if it is on, OBS must be allowed incoming connections:
  **System Settings > Network > Firewall > Options**, set OBS to **Allow
  incoming connections**. Audio and timing use UDP ports macOS picks, so
  per-port rules do not work; allow the app.

### Video or audio problems

- Stutter: put the Mac on Ethernet or 5 GHz Wi-Fi; turn on **Log latency
  telemetry** and look at the `[latency]` and `[MIRROR]` lines in the log.
- AirPlay video: DRM-protected and encrypted media cannot be received. Videos
  that only exist on the device (Safari `blob:` URLs, app files) are copied
  whole before they play, up to 128 MiB. The receiver refuses to fetch from
  loopback, link-local and multicast addresses and from unusual ports on
  hosts other than the device (`[MEDIA] ... refused` in the log); HTTPS
  servers with self-signed certificates need **Allow self-signed media
  servers**.

## Getting help

Open an issue at https://github.com/LZlamian/OBS-AirPlay-Plugin/issues with the
OBS, plugin and macOS versions, the device and what you did, and the log lines
containing `AirPlay`, `[MEDIA]` or `UxPlay`.

> **Before you post logs publicly:** OBS logs and `diagnose.sh` output can contain your
> device names, local IP addresses and MAC-style device IDs. If you turned on the AirPlay
> media trace (`OBS_AIRPLAY_MEDIA_TRACE=1`), the log also contains the addresses of the
> videos you played and AirPlay session IDs; query strings and passwords in those URLs are
> replaced with `[redacted]`, but the host and path remain. Read through what you paste,
> remove anything you don't want public, and turn the trace off again afterwards.

## Building from source

See the README's *Building from source* and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
