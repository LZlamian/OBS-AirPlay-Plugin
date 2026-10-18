# Security

The receiver accepts connections from any device on the local network, and
the plugin updates itself, so security problems matter.

## Reporting

Please report vulnerabilities privately, through GitHub's
**Security > Report a vulnerability** form on
https://github.com/LZlamian/OBS-AirPlay-Plugin, not in a public issue. Include
the version, what an attacker on the network (or elsewhere) can do, and how to
reproduce it. You can expect a reply within a week.

## Supported versions

Fixes go into the latest release; the self-updater brings it to installed
copies from v2.4.0 on.

## What the plugin does to stay safe

- Requests from the network are size-limited and time-limited; one connection
  cannot stall the receiver; AirPlay video can only be controlled by the device
  that started it.
- AirPlay video is never fetched from the Mac itself, link-local, multicast or
  reserved addresses, or unusual ports on other hosts; HTTPS certificates are
  verified unless the user allows self-signed servers.
- Updates are installed only with a valid Ed25519 signature by the project's
  release key, matching GitHub's published digest, and only into the user's
  own plugins folder.
