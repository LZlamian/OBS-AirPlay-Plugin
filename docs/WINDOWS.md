# OBS AirPlay on Windows (preview)

The Windows x64 build is complete as far as it can be checked without a
Windows PC and an iPhone or iPad: it builds in CI, every automated test
passes on a Windows runner, and it is packaged as an installer and a zip.
It has **not been run inside OBS with a real device yet** and is not
released. This page is for trying it and reporting back; the background is
in [WINDOWS_PORT_SCOPE.md](WINDOWS_PORT_SCOPE.md).

## Requirements

- Windows 10 1809 or later, or Windows 11, x64; OBS Studio 32 (64-bit).
  OBS 30 and 31 may work (the source description the plugin registers is
  unchanged since 30.0) but are untested.
- The sending device and the PC on the same network, with multicast (mDNS,
  UDP 5353) allowed between them, and the PC's network set to **Private**
- Bonjour is **not** needed: when it is not installed the plugin advertises
  the receiver through Windows' own DNS service. When Bonjour is installed
  (it comes with Apple Devices or iTunes) the plugin uses it.

## Getting a test build

Beta releases (tags `windows-v*`, marked pre-release) are on the
[releases page](https://github.com/LZlamian/OBS-AirPlay-Plugin/releases).
Every push to the Windows branch builds one: open the CI run on GitHub
(Actions > CI > the run > **Artifacts**) and download
`obs-airplay-windows-x64`. It holds:

| File | What it is |
|---|---|
| `obs-airplay-v<version>-windows-x64-setup.exe` | Installer (unsigned, so Windows SmartScreen warns: **More info > Run anyway**) |
| `obs-airplay-v<version>-windows-x64.zip` | The same files, to copy by hand |
| `obs-airplay-v<version>-windows-x64-symbols.zip` | The unstripped DLL, for crash reports |

## Install

**Installer:** quit OBS, run the setup, keep "Allow OBS through Windows
Firewall" ticked. It installs into
`C:\ProgramData\obs-studio\plugins\obs-airplay` (OBS 30 and later loads
plugins from there), and adds a firewall rule "OBS AirPlay" for
`obs64.exe` on private networks. Uninstall from **Settings > Apps**; that
also removes the rule.

**Zip:** quit OBS and extract the `obs-airplay` folder into
`C:\ProgramData\obs-studio\plugins\`, so that
`C:\ProgramData\obs-studio\plugins\obs-airplay\bin\64bit\obs-airplay.dll`
exists. Allow OBS through the firewall when Windows asks the first time OBS
starts with the plugin (tick **Private networks**).

## What to test

Run `diagnose.ps1` first, while OBS is open: it is in the plugin folder
(`C:\ProgramData\obs-studio\plugins\obs-airplay`); right-click > **Run with
PowerShell**, or `powershell -ExecutionPolicy Bypass -File diagnose.ps1`; it checks the install, discovery, firewall, network
profile and ports and reads the OBS log.

1. **Load:** OBS starts; **Help > Log Files > Show Log Files**: the log has
   `OBS AirPlay Plugin loaded`, `AirPlay discovery (DNS-SD) through Windows
   DNS service` (or `Bonjour`) and `UxPlay integration started successfully`.
   Add an **AirPlay** source; its properties open.
2. **Discovery:** the receiver appears in Control Center > Screen Mirroring
   on the iPhone/iPad within a few seconds. Try it without Bonjour and, if
   you can, with Bonjour installed. Rename the receiver in the source
   properties: the new name appears. If the receiver does not appear,
   capture the network with Wireshark (filter `udp.port == 5353`) while the
   iPhone's Screen Mirroring list is open and attach the capture: the CI
   test resolves the registration only from the PC itself, so what an
   iPhone sees is unproven.
3. **Mirroring:** start mirroring; video in OBS within about a second,
   portrait and landscape, rotation. The log line `[DECODE] mirror video ...:
   D3D11VA hardware decoding` (or `software decoding`) says which decoder
   ran. Note the GPU (Intel/AMD/NVIDIA) and CPU use in Task Manager.
4. **Audio:** sound with the mirror, in sync (clap test); volume; a long
   session (30 min) without drift.
5. **AirPlay video:** play a video from Safari and from an app (e.g. YouTube)
   with AirPlay; seek, pause, resume, stop. Safari videos that are page-local
   (`blob:`) use the spool files in `%TEMP%` (`obs-airplay-fcup-*`), which
   are removed afterwards.
6. **Reconnects:** stop and restart mirroring; lock the phone; switch from
   mirroring to video and back; quit OBS during a session.
7. **Firewall and network:** with the network set to **Public** devices
   cannot connect (expected); Wi-Fi and Ethernet; a VPN running. With
   another AirPlay receiver running (AirServer, 5KPlayer, a second OBS),
   the log says port 7000 is busy and the receiver uses 7001.

8. **Bluetooth:** with Bluetooth on, the OBS log shows `[BLE] discovery
   helper Bluetooth state: on`; turn Bluetooth off and on in Windows
   settings: it logs "off or not supported", then "on" again within about
   30 s. With no adapter: "unavailable" and nothing else. If it keeps
   logging "off or not supported" every 30 s while Bluetooth is on, Windows
   refuses the advertisement itself: report it (AirPlay works without it).
9. **Updates** (needs a signed test release, or wait for the next one):
   the prompt appears about a minute after OBS starts, never while
   streaming or recording; **Install** asks for administrator rights,
   and the update installs after OBS quits. **Later** and **Skip This
   Version** are remembered (`updater.json` in the plugin's config folder).

Please report each item as works / fails with the OBS log (it names your
network; read it first), the `diagnose.ps1` output and the Windows build.
On a crash, OBS writes a crash log; attach it with the matching symbols zip.

## Known gaps

- The self-updater and the Bluetooth "fast discovery" advertisement exist on
  Windows but are untested on real hardware (see below). The log says
  whether Bluetooth advertising runs (`[BLE] ...`); AirPlay works without it.
- The self-updater only offers a release that carries a signed
  `-windows-x64-setup.exe`; there is none yet, so it stays quiet for now.
- Not code-signed: SmartScreen warns about the installer.
- `UXPLAY_DNSSD_BACKEND=windows` or `=bonjour` (environment variable, set
  before starting OBS) forces one discovery backend, for testing.
