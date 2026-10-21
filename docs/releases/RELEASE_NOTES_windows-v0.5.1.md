**Beta for testers.** Fixes for the Windows beta from the 2026-10-21 audit,
before it goes to more testers. Each of them could otherwise look like
"iOS does not accept the receiver", the question this beta is meant to
answer. It also carries everything the macOS plugin gained in 2.6.1 and
the coming 2.7, including the 2.6.1 security fixes. Still untested with
real devices.

## Fixes

- **Bonjour installed but not running** (it comes with Apple Devices or
  iTunes): the plugin now notices and announces the receiver through
  Windows instead, rather than not being found at all. Bonjour's
  `dnssd.dll` is only loaded from the Windows system folder.
- **Discovery failures are shown.** If Windows cannot announce the receiver
  (the name is taken, or multicast DNS is turned off), the AirPlay source
  says so and the OBS log has the error; it used to report success.
- **No crash when renaming the receiver, or quitting OBS, right after it
  starts.**
- **A receiver name with a dot in it** ("Studio 2.0") is listed by iPhones;
  without Bonjour the dot is shown as a similar-looking character.
- **A Windows user name with non-English characters** no longer gives the
  receiver a new identity on every OBS start (iPhones saw a new receiver
  each time).
- **Updates:** the signature and installer downloads no longer depend on
  how Windows treats an empty header list; quitting OBS while Windows asks
  for permission to install no longer makes OBS wait for the prompt.
- **Another AirPlay receiver on the PC** (AirServer, 5KPlayer, a second
  OBS): the plugin no longer shares TCP port 7000 with it; it moves to 7001
  as on macOS. The log names the real socket error.
- **Quicker to react:** a Safari video starts without a second's delay,
  stopping the receiver or quitting OBS no longer waits up to a second, and
  idle mirror and audio threads no longer wake 200 times a second.
- **Installer and `diagnose.ps1`** find OBS installed in a custom folder;
  `diagnose.ps1` no longer appears to hang on the firewall check and no
  longer reports the receiver as missing when Windows simply does not answer
  for itself.

## Shared with macOS 2.6.1 and 2.7

- Security: a malformed video playlist or mirror stream from a device on the
  network could crash OBS (the 2.6.1 fixes).
- "From any web address" video fetches only from the address it checked;
  quitting OBS no longer waits for a slow name lookup.
- Mirror audio: a short jitter margin at the start and no drift over long
  sessions; the many robustness fixes listed in the macOS changelog.

## What testers should try first

1. With no Bonjour: install, start OBS, check the log for "the receiver is
   announced on the network", and capture `udp.port==5353` in Wireshark
   while the iPhone opens Screen Mirroring.
2. Rename the receiver within two seconds of starting OBS; quit OBS a
   second after it opens.
3. With Bonjour installed, stop the "Bonjour Service" and start OBS: the
   receiver must still be found.

## Requirements and install

As for 0.5.0: Windows 10 (1809 or later) or 11, 64-bit; OBS Studio 32,
64-bit (30 and 31 may work, untested); run `obs-airplay-v0.5.1-windows-x64-setup.exe` (not
code-signed yet: **More info**, then **Run anyway**).
