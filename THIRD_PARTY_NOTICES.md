# License and third-party notices

## OBS AirPlay

OBS AirPlay is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version (GPL-3.0-or-later). The full text is in `LICENSE` (in the plugin
bundle: `Contents/Resources/LICENSE` and `Contents/Resources/licenses/GPL-3.0.txt`).

Up to v2.5.2 the project's own code was offered under "GPL-2.0 or (at your
option) any later version"; from v2.6.0 it is distributed under
GPL-3.0-or-later. The released binary was already GPL-3.0 in effect, because
it statically links UxPlay (GPL-3.0).

The complete corresponding source for a release is the Git tag of the same
version at https://github.com/LZlamian/OBS-AirPlay-Plugin together with the
UxPlay submodule revision it pins (https://github.com/LZlamian/UxPlay). Clone
with `git clone --recursive`; GitHub's "Source code" archives do not include
the submodule.

## Components in the plugin bundle

The license texts listed here are shipped in `Contents/Resources/licenses/`
and kept in the source tree under `licenses/`.

| Component | Use | License | Text |
|---|---|---|---|
| UxPlay (fork, `uxplay/`) | AirPlay protocol; statically linked | GPL-3.0 | `GPL-3.0.txt` |
| shairplay and RPiPlay code (in UxPlay: `raop*.c`, `http*.c`, `dnssd.c` and others carrying that header) | AirPlay receiver; statically linked | LGPL-2.1-or-later | `libplist-LGPL-2.1.txt` (the same license text) |
| csrp (in UxPlay, `lib/srp.c`) | SRP pairing; statically linked | MIT | `csrp-MIT.txt` |
| playfair (in UxPlay) | FairPlay handshake; statically linked | GPL-3.0 | `playfair-GPL-3.0.md` |
| llhttp (in UxPlay) | HTTP/RTSP parser; statically linked | MIT | `llhttp-MIT.txt` |
| OpenSSL 3.6.3 | Cryptography; statically linked (also into the FFmpeg libraries) | Apache-2.0 | `OpenSSL-Apache-2.0.txt` |
| libplist 2.7.0 | Property lists; statically linked | LGPL-2.1-or-later | `libplist-LGPL-2.1.txt` |
| FFmpeg 8.1.2 | Decoding and demuxing; dynamic libraries in `Contents/Frameworks` | LGPL-2.1-or-later | `FFmpeg-LGPL-2.1.txt`, `FFmpeg-LICENSE.md` |
| SIMDe (headers in `obs-headers/simde`) | Compiled in through libobs headers | MIT | `simde-MIT.txt` |

FFmpeg in release packages is built by `scripts/build-macos-release-deps.sh`
from the official `ffmpeg-8.1.2.tar.xz` (SHA-256 checked in that script),
without `--enable-gpl` or `--enable-nonfree`, configured with:

```
--disable-static --enable-shared --disable-programs --disable-doc --disable-debug
--disable-autodetect --disable-everything --disable-avdevice --disable-avfilter
--enable-avformat --enable-network --enable-openssl --enable-avcodec --enable-avutil
--enable-swscale --enable-swresample --enable-decoder=aac,alac,h264,hevc,mp3
--enable-videotoolbox --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox
--enable-parser=aac,h264,hevc,mpegaudio --enable-demuxer=aac,hls,mov,mp3,mpegts
--enable-protocol=crypto,file,http,https,tcp,tls,udp
```

You may replace the FFmpeg libraries in `Contents/Frameworks` with your own
compatible build; re-sign the bundle afterwards (`codesign --force --deep --sign -`).

## Not shipped

- **libobs** (OBS Studio, GPL-2.0-or-later) is linked dynamically from the
  user's OBS installation. Headers from OBS Studio are kept in `obs-headers/`
  for building only.
- Apple system frameworks (Foundation, AppKit, CoreBluetooth, CryptoKit,
  VideoToolbox) are part of macOS.

Individual source files keep the copyright and license notices of their
authors. This file is a summary and does not replace any license text.
