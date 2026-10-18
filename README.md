# OBS AirPlay Plugin

A native macOS plugin for OBS Studio: your Mac appears as an AirPlay receiver,
and an iPhone, iPad or Mac that mirrors its screen to it, or sends a video with
AirPlay, appears in OBS as a source.

Current release: **v2.6.0**: network hardening, signed updates, and faster,
lighter mirroring and AirPlay video ([release notes](docs/releases/RELEASE_NOTES_v2.6.0.md),
[changelog](CHANGELOG.md)). v2.5.1 and earlier can be crashed by a single
request from the local network, so please update.

## Features

- ✅ Native Bonjour/mDNS advertising (shows up in iOS Screen Mirroring)
- ✅ AirPlay protocol support
- ✅ **Safari Media AirPlay** — play website MP4 and HLS video directly in OBS
- ✅ **Page-local video support** — Safari `blob:` MP4 media is fetched over AirPlay's reverse channel
- ✅ **iOS AirPlay video from native apps** — AVPlayer local-file playback (`mediaType=file`) is fetched from the sender's media server, with play/pause, seek, and item switches
- ✅ **Sender-local media support** — stateful `file:` and `blob:` media URLs are fetched over AirPlay's reverse channel, including senders that establish `/reverse` after `/play`
- ✅ Audio with the mirrored screen
- ✅ H.264 and HEVC video decoding
- ✅ AAC audio decoding
- ✅ No password required
- ✅ Works on the same network/VLAN
- ✅ **Configurable server name** — rename the receiver directly from OBS source properties (great when running the plugin on multiple computers)
- ✅ **Debounced name updates** — mDNS only re-advertises after you stop typing, not on every keystroke
- ✅ **Instant Screen Mirroring refresh** — name changes appear in iOS Screen Mirroring immediately, without needing to connect first
- ✅ **Reset to Default button** — one click restores the server name to "OBS AirPlay"
- ✅ **Persistent receiver identity** — keeps Bonjour and AirPlay key identity stable across OBS restarts
- ✅ **Sub-second initial connection** — an optimized receiver profile and Bluetooth discovery signal eliminate the multi-second delay before iOS opens its first connection
- ✅ **Fast first frame** — optimized TCP negotiation and detailed connection telemetry bring a typical tap-to-OBS-frame time below one second
- ✅ **Clean disconnects** — the source goes empty when a device stops AirPlay, rather than holding its last frame
- ✅ **In step with the sending app** — optional: AirPlay video is timed from the sender's commands, for apps that also play the video on the device
- ✅ **One-click updates** — checks for a newer stable GitHub release at most once per day; one click downloads it and installs it when you quit OBS, with no administrator password

## Requirements

- macOS 12 Monterey or later, Apple silicon (Intel builds are produced by CI
  but not released yet)
- OBS Studio 32 (the plugin is built against its headers; earlier versions are
  untested)
- The sending device and the Mac on the same network and VLAN, with multicast
  (Bonjour) allowed between them

The receiver uses **TCP port 7000** (7001 if 7000 is taken), UDP ports chosen
by macOS for audio and timing, and Bonjour (UDP 5353). macOS's own **AirPlay
Receiver** also uses port 7000: if devices cannot connect, turn it off in
**System Settings > General > AirDrop & Handoff**. If the macOS firewall is on,
allow incoming connections for OBS when asked (or in **System Settings >
Network > Firewall > Options**); you do not need to turn the firewall off.

## Installation

### Before you begin

確保在安装前满足以下前提条件 / Ensure the following before installing:

1. <img width="272" height="344" alt="1" src="https://github.com/user-attachments/assets/7b756c57-56e6-4f57-a043-5d5e2350fe5c" />

2. <img width="703" height="613" alt="2" src="https://github.com/user-attachments/assets/2a247c6c-ef54-4d92-9532-8c338d124d69" />

3. <img width="716" height="619" alt="3" src="https://github.com/user-attachments/assets/423eebf2-14db-4a28-9940-7b43c04ca840" />

### Install

Download the latest `.pkg` from the [Releases page](https://github.com/LZlamian/OBS-AirPlay-Plugin/releases)
and open it. It installs the plugin for the logged-in user, in
`~/Library/Application Support/obs-studio/plugins/obs-airplay.plugin` (and
removes the system-wide copy that versions before 2.6 also installed). The
package contains its own FFmpeg and other libraries; Homebrew is not needed.
The `.zip` contains the same `obs-airplay.plugin`: copy it into that folder
yourself if you prefer.

**Signing:** releases are not signed with an Apple Developer ID or notarized,
so macOS warns before opening the installer. Do not move it to the Trash: click
**Done**, then go to **System Settings > Privacy & Security**, scroll down and
click **Open Anyway**.  
双击安装时可能会提示安装程序不安全：**请勿移至废纸篓**，点击「完成」，然后前往
**系统设置 → 隐私与安全性**，滚动到底部点击「仍要打开」。

When OBS first starts with the plugin, macOS may ask whether **OBS AirPlay
Discovery** may use Bluetooth. Allowing it lets nearby devices find the receiver
faster; mirroring itself always goes over the local network.

### Updates

Once a day the plugin's helper asks GitHub whether a newer release exists. If
one does, it asks you (never while OBS is streaming or recording, and without
taking the keyboard focus from OBS): **Install Update**, **View Release**,
**Later** (asks again in a week) or **Skip This Version**.

**Install Update** downloads the release while OBS keeps running and replaces
the plugin when you quit OBS; no administrator password is needed. A download
is used only if it carries a valid signature by the project's release key, has
the SHA-256 digest GitHub publishes, and is this plugin, at the announced
version, built for your Mac and macOS version, with an intact code seal; it is
checked again just before the swap. The replaced version is kept next to the
plugin (`.obs-airplay.previous`) until the new one has started. Only a plugin
in your own home folder updates itself.

To make no update requests at all, turn off **Check for plugin updates once a
day** in the AirPlay source's properties. The request goes to `api.github.com`
and carries only the plugin version in its User-Agent.

### Uninstall

Quit OBS and delete `~/Library/Application Support/obs-studio/plugins/obs-airplay.plugin`
(and, if present, `.obs-airplay.update` and `.obs-airplay.previous` in the same
folder). The plugin's settings are in
`~/Library/Application Support/obs-studio/plugin_config/obs-airplay/`; its
helper's preferences can be removed with
`defaults delete com.lzlamian.obs-airplay.discovery`.

## Usage

### Set up in OBS

安装完成后，请按照以下步骤操作 / Once installed follow the steps below:

1. <img width="789" height="343" alt="Step 1" src="https://github.com/user-attachments/assets/29bc5a51-b681-4a4d-867b-8c7e86c99e16" />
2. <img width="969" height="649" alt="Step 2" src="https://github.com/user-attachments/assets/23dbe5f3-e9cf-4a2d-94df-1879f4ffdc12" />
3. <img width="959" height="717" alt="Step 3" src="https://github.com/user-attachments/assets/6e97e402-d92f-4477-a656-2e91ee01eff6" />
4. <img width="958" height="838" alt="Step 4" src="https://github.com/user-attachments/assets/9ed028ed-5354-4f8e-a112-a94bb3829d88" />
5. <img width="959" height="828" alt="Step 5" src="https://github.com/user-attachments/assets/bd24e97b-d491-47f2-9117-2553a3484c54" />

In OBS, click **+** in the Sources panel, choose **AirPlay** and give it a name.

### Connect

完成上述操作后，在您的 iPhone/iPad 上执行以下步骤 / After this, on your iPhone/iPad:

1. ![Control Centre Screen Mirroring](https://github.com/user-attachments/assets/68b9ade9-0159-4835-9be7-2445a9dc31d9)
2. <img width="302" height="329" alt="Select OBS AirPlay" src="https://github.com/user-attachments/assets/125c8617-8ce8-4b01-b067-2b802304aaa2" />

现在，您应该可以看到您的设备已通过 AirPlay 镜像到 OBS 的「来源」中了！:)  
You should now see your device mirrored through AirPlay as an OBS source 🎉
On the device, open Control Center, tap **Screen Mirroring** and choose
**OBS AirPlay** (or the name you gave the receiver). To send only a video, tap
the AirPlay control in Safari or in an app's player and choose the receiver.
When the device stops, the source goes empty about half a second later.

### Settings

All AirPlay sources share these settings (there is one receiver per OBS):

- **Server Name**: the name devices show. Applied about a second after you stop
  typing; at most 63 bytes. Give each Mac a different name if several run the
  plugin. **Reset to Default** restores "OBS AirPlay".
- **Status**: what the receiver is doing, and why it is not working when it is
  not (for example, both ports in use), and the last AirPlay video it refused
  and why.
- **AirPlay video**: AirPlay video (the AirPlay button in Safari, YouTube and
  other apps) hands OBS a web address that this Mac then downloads from.
  Screen mirroring is not affected by this setting.
  - **From any web address** (the default): videos from websites play, as on
    an Apple TV. Private LAN addresses and internet hosts on web ports are
    allowed.
  - **Only from the sending device**: only video the device serves from its
    own address plays (its own files, video local to the page). Links to
    other hosts, redirects to them and HLS playlists that point at them are
    refused, so apps that stream from a website (YouTube, most video sites)
    will not play.
  - **Off (screen mirroring only)**: every AirPlay video is refused; the
    device then plays the video itself or shows an error.

  Any device on the network can send AirPlay video without a password; each
  refusal is logged with its reason.
- **Keep AirPlay video in step with the sending app**: for apps that also play
  the video on the device; leave off for Safari.
- **Allow self-signed media servers**: AirPlay video from HTTPS servers whose
  certificate cannot be verified is refused unless this is on.
- **Log latency telemetry**: timing figures in the OBS log, for diagnosing.
- **Check for plugin updates once a day**.

For troubleshooting, two switches are files in
`~/Library/Application Support/obs-studio/plugin_config/obs-airplay/`
(restart OBS after creating or deleting them): `obs-airplay-software-decode`
forces software decoding (or start OBS with `OBS_AIRPLAY_HW_DECODE=0`), and
`obs-airplay-media-trace` logs the AirPlay video protocol as `[MEDIA-TRACE]`
lines (or `OBS_AIRPLAY_MEDIA_TRACE=1`; URLs in it have their query strings
removed). `OBS_AIRPLAY_MEDIA_PORTS="80,443,8080,8443"` sets the ports AirPlay
video may fetch from on hosts other than the sending device.

## Troubleshooting

Run `./diagnose.sh` from the source folder while OBS is open (no administrator
rights needed). It checks the installation, ports, firewall, Bonjour
advertisement and the OBS log, and says what to change.

- **The receiver is not in the Screen Mirroring list**: check the source's
  Status line; make sure both devices are on the same network (not a guest
  network, not split by a VPN); check `dns-sd -B _airplay._tcp` lists the name.
- **It is listed but connecting fails**: port 7000 may be taken by macOS's
  AirPlay Receiver (see Requirements), or the firewall blocks OBS.
- **Stutter**: use 5 GHz Wi-Fi or Ethernet for the Mac; turn on latency
  telemetry and look at the `[latency]` lines.
- **AirPlay video does not play**: DRM-protected video cannot be received;
  page-local Safari videos are transferred whole first (up to 128 MiB); videos
  on hosts the receiver will not fetch from (loopback, link-local, unusual
  ports) are refused, see the OBS log.

More in [INSTALL.md](INSTALL.md). Before posting logs or `diagnose.sh` output
in an issue, read them: they name your network, devices and, with the media
trace on, the addresses of the videos you played.

## Building from source

```bash
git clone --recursive https://github.com/LZlamian/OBS-AirPlay-Plugin.git
cd OBS-AirPlay-Plugin
brew install cmake pkg-config ffmpeg libplist openssl@3
cmake --preset release
cmake --build --preset release
ctest --preset release
```

`./build.sh` builds and installs into your OBS plugins folder. See
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the layout of the code, its
threads, the tests and the release process, and [CONTRIBUTING.md](CONTRIBUTING.md).
Report security problems as described in [SECURITY.md](SECURITY.md).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
which lists every bundled component and its license (the texts are in [licenses/](licenses/)
and in the plugin bundle's `Contents/Resources/licenses/`). Up to v2.5.2 the project's own
code was offered under GPL-2.0-or-later; the released plugin was always GPL-3.0 in effect
because it links UxPlay.

## Acknowledgments

- [UxPlay](https://github.com/FDH2/UxPlay), whose AirPlay implementation the receiver is built on
- mika314, for the original [obs-airplay](https://github.com/mika314/obs-airplay)
- The OBS Studio and FFmpeg projects
