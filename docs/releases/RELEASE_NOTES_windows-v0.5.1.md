**Beta for testers.** Fixes for the Windows beta from the 2026-10-21 audit,
before it goes to more testers. Each of them could otherwise look like
"iOS does not accept the receiver", the question this beta is meant to
answer. Still untested with real devices. The macOS plugin is unchanged by
this beta.

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

## What testers should try first

1. With no Bonjour: install, start OBS, check the log for "the receiver is
   announced on the network", and capture `udp.port==5353` in Wireshark
   while the iPhone opens Screen Mirroring.
2. Rename the receiver within two seconds of starting OBS; quit OBS a
   second after it opens.
3. With Bonjour installed, stop the "Bonjour Service" and start OBS: the
   receiver must still be found.

## Requirements and install

As for 0.5.0: Windows 10 (1809 or later) or 11, 64-bit; OBS Studio 30 or
later, 64-bit; run `obs-airplay-v0.5.1-windows-x64-setup.exe` (not
code-signed yet: **More info**, then **Run anyway**).
