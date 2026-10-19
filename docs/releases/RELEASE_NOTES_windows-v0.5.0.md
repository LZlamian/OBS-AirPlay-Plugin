**Beta for testers.** This is the first Windows build of OBS AirPlay: mirror an
iPhone or iPad into OBS Studio on Windows. Every automated test passes on
Windows, but it has not yet been used with real devices, so expect rough
edges and please report what you find. The macOS plugin is unchanged; its
current release is still v2.6.0.

## Requirements

- Windows 10 (1809 or later) or Windows 11, 64-bit
- OBS Studio 30 or later, 64-bit (built and tested against 32.0.4)
- The iPhone/iPad and the PC on the same network, with the PC's network set
  to **Private** (Settings > Network & internet > your network)
- Bonjour is **not** required; if it is installed (it comes with Apple
  Devices or iTunes), the plugin uses it

## Install

1. Quit OBS.
2. Download `obs-airplay-v0.5.0-windows-x64-setup.exe` below and run it.
   It is not code-signed yet, so Windows SmartScreen warns:
   click **More info**, then **Run anyway**.
3. Keep **Allow OBS through Windows Firewall** ticked.
4. Start OBS, add an **AirPlay** source, then on the iPhone/iPad open
   Control Center > **Screen Mirroring** and pick the receiver.

The zip has the same files for a manual install: extract its `obs-airplay`
folder into `C:\ProgramData\obs-studio\plugins\`. Uninstall from
**Settings > Apps**.

## What to report

If something does not work, run `diagnose.ps1` from
`C:\ProgramData\obs-studio\plugins\obs-airplay` (right-click >
**Run with PowerShell**) while OBS is open, and open an issue with:

- what you did and what happened
- the `diagnose.ps1` output and the OBS log (**Help > Log Files > Upload
  Current Log File**)
- your Windows version, GPU and iPhone/iPad model and iOS version

The full test checklist is in
[docs/WINDOWS.md](https://github.com/LZlamian/OBS-AirPlay-Plugin/blob/claude/upbeat-meitner-zf2a2e/docs/WINDOWS.md#what-to-test).
On a crash, attach the OBS crash log; `-symbols.zip` lets us read it.

## Known limits

- Not code-signed: SmartScreen warns on first run.
- This beta does not update itself: install new betas from the releases page.
- Bluetooth "fast discovery" is included but untested; AirPlay works without
  it.

## Files

| File | What it is |
|---|---|
| `obs-airplay-v0.5.0-windows-x64-setup.exe` | Installer |
| `obs-airplay-v0.5.0-windows-x64.zip` | The same plugin, for a manual install |
| `obs-airplay-v0.5.0-windows-x64-symbols.zip` | Debug symbols, for crash reports only |
| `SHA256SUMS-windows` | Checksums |
