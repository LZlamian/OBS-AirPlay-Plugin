# Third-party software notices

OBS AirPlay incorporates or links to third-party open-source software. The
corresponding source for the exact plugin release is available from:

https://github.com/LZlamian/OBS-AirPlay-Plugin

The Git tag matching the installed plugin version identifies the source used
for that release, including the pinned UxPlay submodule revision.

## UxPlay

UxPlay provides the AirPlay protocol implementation and is statically linked
into the plugin. It is distributed under the GNU General Public License,
version 3. Its full license is packaged as `LICENSE-UxPlay-GPL-3.0.txt` and is
present in the source tree at `uxplay/LICENSE`.

https://github.com/FDH2/UxPlay

## FFmpeg

FFmpeg provides video and audio decoding and format conversion. Release
packages may include FFmpeg dynamic libraries. The applicable LGPL/GPL terms
depend on how those libraries were configured by their distributor.

https://ffmpeg.org/legal.html

## libplist

libplist is statically linked through UxPlay and is distributed under the GNU
Lesser General Public License, version 2.1 or later.

https://github.com/libimobiledevice/libplist

## OpenSSL

OpenSSL libcrypto is statically linked through UxPlay. OpenSSL 3 is distributed
under the Apache License 2.0.

https://www.openssl.org/source/license.html

Additional small components included by UxPlay retain the copyright and
license notices found in their source files. This notice is informational and
does not replace any applicable license text.
