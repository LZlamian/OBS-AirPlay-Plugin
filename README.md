# OBS AirPlay Plugin

A native macOS plugin for OBS Studio that enables AirPlay screen mirroring from iOS and macOS devices directly into OBS as a source.

Current release: **v2.4.2** — update alerts show OBS's icon; one-click updates arrived in v2.4.0 ([release notes](RELEASE_NOTES_v2.4.2.md), [v2.4.0 notes](RELEASE_NOTES_v2.4.0.md)).

## What's new in v2.4.0

- **Install Update** in the update prompt: the new release is downloaded in the background and installed when you quit OBS, with no administrator password
- Downloads are checked against the SHA-256 digest GitHub publishes, and the plugin's code seal is verified before anything is replaced
- Nothing is downloaded or installed unless you click **Install Update**

## What's new in v2.3.0

- Hardware decoding for screen mirroring and AirPlay video, with automatic software fallback (force software with `OBS_AIRPLAY_HW_DECODE=0`)
- Portrait recordings play upright in AirPlay video
- OBS no longer goes black when an app cancels AirPlay video while mirroring
- AirPlay video items swapped in while paused stay paused, and OBS follows scrubber drags while paused
- A malformed request from the local network can no longer crash OBS
- Safer renaming, shutdown and stream restarts; stale temporary media files are cleaned up

## What's new in v2.2.0

- iOS "AirPlay video" from native apps that play local files with AVPlayer (`allowsExternalPlayback`): OBS fetches the video from the sender's own media server
- Item switches on the same player (`playlistRemove`/`playlistInsert`) play the new item instead of being rejected
- Playback that follows the sender: no keyframe pre-roll after seeks, preview frames when seeking while paused, still clips, accurate `/playback-info`, hold on the last frame at item end
- Paused items and still images (photos, PDF pages) always appear; no blank frames between items
- Screen mirroring comes back after an app stops AirPlay video (previously OBS stayed black)
- Robust transfers from the sender's media server: early-ended responses continue with fresh range requests instead of losing frames
- A rejected `/play` no longer drops the AirPlay connection
- Optional `[MEDIA-TRACE]` protocol trace for diagnosing AirPlay video senders

See the [v2.3.0](RELEASE_NOTES_v2.3.0.md) and [v2.2.0](RELEASE_NOTES_v2.2.0.md) release notes for the complete summaries, and the [v2.1.0 release notes](RELEASE_NOTES_v2.1.0.md) for Safari Media AirPlay.

## Features

- ✅ Native Bonjour/mDNS advertising (shows up in iOS Screen Mirroring)
- ✅ AirPlay protocol support
- ✅ **Safari Media AirPlay** — play website MP4 and HLS video directly in OBS
- ✅ **Page-local video support** — Safari `blob:` MP4 media is fetched over AirPlay's reverse channel
- ✅ **iOS AirPlay video from native apps** — AVPlayer local-file playback (`mediaType=file`) is fetched from the sender's media server, with play/pause, seek, and item switches
- ✅ **Sender-local media support** — stateful `file:` and `blob:` media URLs are fetched over AirPlay's reverse channel, including senders that establish `/reverse` after `/play`
- ✅ RAOP (Remote Audio Output Protocol) for audio streaming
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
- ✅ **One-click updates** — checks for a newer stable GitHub release at most once per day; one click downloads it and installs it when you quit OBS, with no administrator password

## Requirements

### macOS
- macOS 12 (Monterey) or later
- OBS Studio 28.0 or later
- Xcode Command Line Tools and Homebrew only when building from source

### System Requirements
- Your iOS device and Mac must be on the same network
- Firewall should allow incoming connections on ports 7000 and 5000
- Multicast DNS (mDNS) must be enabled on your network

## Installation

### Before You Begin
確保在安装前满足以下前提条件 / Ensure the following before installing:

1. <img width="272" height="344" alt="1" src="https://github.com/user-attachments/assets/7b756c57-56e6-4f57-a043-5d5e2350fe5c" />

2. <img width="703" height="613" alt="2" src="https://github.com/user-attachments/assets/2a247c6c-ef54-4d92-9532-8c338d124d69" />

3. <img width="716" height="619" alt="3" src="https://github.com/user-attachments/assets/423eebf2-14db-4a28-9940-7b43c04ca840" />

### macOS Security Warning
When double clicking to install you may get a notice indicating it's not safe to install. **DO NOT move it to trash.** Click **Done**.  
双击安装时，您可能会看到提示指出该安装程序不安全。**请勿移至废纸篓**，点击「完成」。

Next go to **Apple Menu () → System Settings → Privacy & Security**, scroll to the bottom and click **Open Anyway**. Proceed to install.  
接下来前往 **Apple 菜单 () → 系统设置 → 隐私与安全性**，滚动到底部点击**「仍要打开」**，然后继续安装。

On first launch, macOS may ask whether **OBS AirPlay Discovery** can use Bluetooth. Click **Allow**. Bluetooth is used only to advertise the nearby receiver for faster discovery; mirroring media still travels over your local network.

### Quick Install (Recommended)

Download the latest `.pkg` installer from the [Releases page](https://github.com/LZlamian/OBS-AirPlay-Plugin/releases) and double-click to install.

The release package contains its runtime dependencies; Homebrew is not required
to install it.

### Updates

OBS AirPlay checks GitHub's stable-release endpoint at most once every 24 hours.
When a newer version is available, **OBS AirPlay Discovery** offers to install
it, show the release page, ask again later, or skip that version.

**Install Update** downloads the release zip in the background while OBS keeps
running, and replaces the plugin when you quit OBS. The next time OBS starts it
runs the new version. No administrator password is needed, because only the
copy in your own `~/Library/Application Support/obs-studio/plugins` is replaced.

- Nothing is downloaded or installed unless you click **Install Update**.
- The download must match the SHA-256 digest GitHub publishes for the release
  file, must be this plugin at the announced version, and must be intact
  (its code seal is checked after download and again before installing).
  Anything else is discarded and the installed plugin is left as it is.
- If the plugin is somewhere you cannot write to, or a release has no zip for
  your Mac, the prompt offers only the release page, as before.
- An update that is waiting can be cancelled by deleting the hidden folder
  `.obs-airplay.update` next to the plugin.

The check sends a standard HTTPS request to `api.github.com`. It contains the
installed plugin version in its User-Agent and no OBS settings or AirPlay data.
The download comes from `github.com`.

### Build from source

#### 1. Install build tools

```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install Homebrew if you don't have it
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install required libraries
brew install \
    cmake \
    pkg-config \
    ffmpeg \
    libplist \
    openssl@3
```

Release packaging rejects dependencies built for a newer macOS version than
the documented baseline. Run `scripts/build-macos-release-deps.sh` first and
pass its printed `OBS_AIRPLAY_DEPS_PREFIX` value to `scripts/package-macos.sh`
to build the pinned macOS 12 release dependencies. Set
`OBS_AIRPLAY_CODESIGN_IDENTITY` and `OBS_AIRPLAY_INSTALLER_IDENTITY` to the
appropriate Developer ID identities before running `scripts/package-macos.sh`.

#### 2. Clone OBS Studio Source

You need the OBS Studio source code to build plugins:

```bash
# Replace "30.2.0" with your installed OBS version
# Check your version in OBS: Help -> About
cd ~/Developer
git clone --recursive --branch 30.2.0 https://github.com/obsproject/obs-studio.git
```

#### 3. Clone This Plugin

```bash
cd ~/Developer
git clone --recursive https://github.com/LZlamian/OBS-AirPlay-Plugin.git
cd obs-airplay-plugin
```

#### 4. Build the Plugin

```bash
mkdir build
cd build

# Configure CMake
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="/Applications/OBS.app/Contents/Resources" \
    -DQT_VERSION=6

# Build
cmake --build . --config Release
```

#### 5. Install the Plugin

```bash
# Create the plugins directory if it doesn't exist
mkdir -p ~/Library/Application\ Support/obs-studio/plugins

# Copy the plugin
cp -r obs-airplay.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

#### 6. Restart OBS Studio

Close and reopen OBS Studio to load the plugin.

## Usage

### Setting Up in OBS
安装完成后，请按照以下步骤操作 / Once installed follow the steps below:

1. <img width="789" height="343" alt="Step 1" src="https://github.com/user-attachments/assets/29bc5a51-b681-4a4d-867b-8c7e86c99e16" />
2. <img width="969" height="649" alt="Step 2" src="https://github.com/user-attachments/assets/23dbe5f3-e9cf-4a2d-94df-1879f4ffdc12" />
3. <img width="959" height="717" alt="Step 3" src="https://github.com/user-attachments/assets/6e97e402-d92f-4477-a656-2e91ee01eff6" />
4. <img width="958" height="838" alt="Step 4" src="https://github.com/user-attachments/assets/9ed028ed-5354-4f8e-a112-a94bb3829d88" />
5. <img width="959" height="828" alt="Step 5" src="https://github.com/user-attachments/assets/bd24e97b-d491-47f2-9117-2553a3484c54" />

### Connect from iOS
完成上述操作后，在您的 iPhone/iPad 上执行以下步骤 / After this, on your iPhone/iPad:

1. ![Control Centre Screen Mirroring](https://github.com/user-attachments/assets/68b9ade9-0159-4835-9be7-2445a9dc31d9)
2. <img width="302" height="329" alt="Select OBS AirPlay" src="https://github.com/user-attachments/assets/125c8617-8ce8-4b01-b067-2b802304aaa2" />

现在，您应该可以看到您的设备已通过 AirPlay 镜像到 OBS 的「来源」中了！:)  
You should now see your device mirrored through AirPlay as an OBS source 🎉
   - In OBS, click the `+` button in the Sources panel
   - Select "AirPlay" from the list
   - Give it a name (e.g., "iPhone Screen")
   - Click OK

2. **Connect from iOS**
   - On your iPhone/iPad, open Control Center (swipe down from top-right)
   - Tap "Screen Mirroring"
   - You should see "OBS AirPlay" in the list
   - Tap it to connect

   To send a website video instead of mirroring the whole screen, open a
   compatible MP4 or HLS video in Safari, tap its AirPlay control, and select
   the OBS receiver. Safari may take a few seconds to transfer or open the
   media before the first frame appears.

3. **Start Streaming**
   - Your iOS screen should now appear in OBS
   - The source will automatically adjust to the device resolution

### Troubleshooting

#### Device Not Showing Up in Screen Mirroring List

1. **Check Network Connection**
   - Ensure your Mac and iOS device are on the same WiFi network
   - Try disconnecting and reconnecting to WiFi on both devices

2. **Check Firewall Settings**
   ```bash
   # Temporarily disable firewall to test
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --setglobalstate off
   
   # If this fixes it, add OBS to firewall exceptions:
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add /Applications/OBS.app
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblock /Applications/OBS.app
   
   # Re-enable firewall
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --setglobalstate on
   ```

3. **Check Plugin Logs**
   - In OBS, go to Help -> Log Files -> View Current Log
   - Look for "AirPlay" entries
   - You should see:
     ```
     OBS AirPlay Plugin loaded
     AirPlay server started successfully
     mDNS advertising started for 'OBS AirPlay'
     ```

4. **Verify mDNS is Working**
   ```bash
   # Install dns-sd tool (comes with Xcode)
   dns-sd -B _airplay._tcp
   
   # You should see "OBS AirPlay" listed
   # Press Ctrl+C to stop
   ```

5. **Check Port Availability**
   ```bash
   # Check if ports are in use
   lsof -i :7000
   lsof -i :5000
   
   # If something else is using these ports, you'll need to change them
   # or stop the conflicting application
   ```

#### Connection Drops or Stutters

1. **Network Issues**
   - Move closer to your WiFi router
   - Reduce network congestion
   - Use a 5GHz network if available

2. **System Resources**
   - Check Activity Monitor for high CPU usage
   - Close unnecessary applications
   - Lower OBS video resolution/quality settings

#### No Video or Audio

1. **Check OBS Audio Settings**
   - Go to Settings -> Audio
   - Ensure monitoring is set correctly
   - Check the audio mixer levels

2. **Check Source Settings**
   - Right-click the AirPlay source -> Properties
   - Verify the server is running

3. **Safari Media AirPlay**
   - Public media must be reachable by the Mac running OBS
   - Page-local videos are transferred completely before playback begins
   - DRM-protected and encrypted MediaSource content is not supported
   - Reverse-fetched `blob:` MP4 files are limited to 128 MiB

## Configuration

### Changing Server Name

In OBS, right-click the AirPlay source → **Properties** and edit the **Server Name** field.  
The default is `OBS AirPlay`. Change it to any name you like (e.g. `Studio Mac`, `MacBook Pro`).

The name is applied ~800 ms after you stop typing — mDNS re-advertises once, and the new name appears in the iOS Screen Mirroring list right away (no connection needed).

> **Tip:** If you run the plugin on multiple computers at the same time, give each a unique server name so iOS devices can tell them apart in the Screen Mirroring list.

### Changing Ports

The plugin listens on port 7000 and falls back to 7001 by itself when 7000 is
taken (for example by a second OBS on the same Mac). To use another port, change
the port passed to `g_uxplay_integration->start(...)` in `src/plugin-main.cpp`:

```cpp
g_uxplay_integration->start(mac_address, 7100, initial_name)
```

**Note:** After changing ports, you must rebuild and reinstall the plugin.

### Enabling Password Protection

Currently, password protection is disabled. To enable it, modify the TXT records in `src/uxplay-integration.cpp`.

## Technical Details

### Protocols Used

1. **Bonjour/mDNS** - Service discovery (makes the receiver visible)
   - Service Type: `_airplay._tcp`
   - Service Type: `_raop._tcp`
   - Port: 5353 (multicast)

2. **AirPlay Protocol**
   - Port: 7000 (TCP)
   - HTTP-based protocol
   - Handles screen mirroring plus Safari `/reverse`, `/play`, playback-info,
     rate, and scrub controls

3. **RAOP (Remote Audio Output Protocol)**
   - Port: 5000 (TCP)
   - RTSP-based protocol
   - Handles audio streaming

### Video Format
- Codecs: H.264 and HEVC
- Containers/protocols: MP4, MPEG-TS, and HLS
- Resolution: Depends on source device

### Audio Format
- Codec: AAC-LC
- Sample Rate: 44100 Hz
- Channels: 2 (Stereo)

## Development

### Project Structure

```
obs-airplay-plugin/
├── CMakeLists.txt           # Build configuration
├── README.md                # This file
└── src/
    ├── plugin-main.cpp      # Plugin entry point, server name debounce
    ├── airplay-source.cpp   # OBS source implementation
    ├── airplay-source.hpp
    ├── airplay-server.cpp   # AirPlay/RAOP socket server + FFmpeg decode pipeline
    ├── airplay-server.hpp
    ├── uxplay-integration.cpp  # UxPlay RAOP + mDNS (dnssd) integration
    ├── uxplay-integration.hpp
    ├── h264-decoder.cpp     # Video decoder
    ├── h264-decoder.hpp
    ├── audio-decoder.cpp    # Audio decoder
    ├── audio-decoder.hpp
    ├── media-player.cpp     # Safari URL/blob demux, decode, and playback clock
    └── media-player.hpp
```

### Building for Development

```bash
# Debug build with verbose output
mkdir build-debug
cd build-debug
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --config Debug -- -v

# Install to OBS
cp -r obs-airplay.plugin ~/Library/Application\ Support/obs-studio/plugins/

# View logs
tail -f ~/Library/Application\ Support/obs-studio/logs/$(ls -t ~/Library/Application\ Support/obs-studio/logs/ | head -1)
```

### Debugging

1. **Enable Debug Logging**
   - Set log level in OBS: Settings -> Advanced -> Recording -> Log Level -> Debug

2. **Use Console.app**
   - Open Console.app
   - Filter for "OBS" or "AirPlay"
   - Run OBS and watch for messages

3. **AirPlay video protocol trace**
   - Create `~/Library/Application Support/obs-studio/obs-airplay-media-trace` (or start OBS with `OBS_AIRPLAY_MEDIA_TRACE=1`) and restart OBS
   - The OBS log then shows every AirPlay video request and response (`/play`, `/action`, `/scrub`, `/rate`, `/stop`, `/playback-info`, reverse-HTTP) as `[MEDIA-TRACE]` lines, plus UxPlay's `[MEDIA]` lines
   - Delete the file and restart OBS to turn it off; the trace is verbose

4. **Attach Debugger**
   ```bash
   # Find OBS process ID
   ps aux | grep OBS
   
   # Attach lldb
   lldb -p <PID>
   ```

## Known Issues

1. **4K Streaming**
   - Currently limited to 1080p
   - 4K support planned for future release

2. **DRM Content**
   - Protected or encrypted Safari media cannot be fetched or decoded

3. **Safari Page-Local Media**
   - `blob:` MP4 media must be transferred completely before playback starts
   - Reverse-fetched files are currently limited to 128 MiB

4. **AirPlay Video Playback Rates**
   - Rates other than pause (0) and normal (1) apply to video only; audio is not time-stretched

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests if applicable
5. Submit a pull request

## References

- [OBS Plugin Documentation](https://obsproject.com/docs/plugins.html)
- [AirPlay Protocol Specification](https://openairplay.github.io/airplay-spec/)
- [Apple Bonjour Documentation](https://developer.apple.com/bonjour/)
- [mika314/obs-airplay](https://github.com/mika314/obs-airplay) - Original inspiration

## License

GPL-2.0 License - See LICENSE file for details

## Support

- **Issues**: [GitHub Issues](https://github.com/LZlamian/OBS-AirPlay-Plugin/issues)
- **Discussions**: [GitHub Discussions](https://github.com/LZlamian/OBS-AirPlay-Plugin/discussions)

## Acknowledgments

- Thanks to mika314 for the original obs-airplay project
- OBS Studio team for the excellent plugin API
- FFmpeg team for multimedia codecs
