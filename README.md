# OBS AirPlay Plugin

A native macOS plugin for OBS Studio that enables AirPlay screen mirroring from iOS and macOS devices directly into OBS as a source.

Current release: **v1.2.1**

## Features

- ✅ Native Bonjour/mDNS advertising (shows up in iOS Screen Mirroring)
- ✅ AirPlay protocol support
- ✅ RAOP (Remote Audio Output Protocol) for audio streaming
- ✅ H.264 video decoding
- ✅ AAC audio decoding
- ✅ No password required
- ✅ Works on the same network/VLAN

## Requirements

### macOS
- macOS 10.15 (Catalina) or later
- OBS Studio 28.0 or later
- Xcode Command Line Tools
- Homebrew (for dependencies)

### System Requirements
- Your iOS device and Mac must be on the same network
- Firewall should allow incoming connections on ports 7000 and 5000
- Multicast DNS (mDNS) must be enabled on your network

## Installation

### 1. Install Dependencies

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
    libplist
```

### 2. Clone OBS Studio Source

You need the OBS Studio source code to build plugins:

```bash
# Replace "30.2.0" with your installed OBS version
# Check your version in OBS: Help -> About
cd ~/Developer
git clone --recursive --branch 30.2.0 https://github.com/obsproject/obs-studio.git
```

### 3. Clone This Plugin

```bash
cd ~/Developer
git clone https://github.com/yourusername/obs-airplay-plugin.git
cd obs-airplay-plugin
```

### 4. Build the Plugin

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

### 5. Install the Plugin

```bash
# Create the plugins directory if it doesn't exist
mkdir -p ~/Library/Application\ Support/obs-studio/plugins

# Copy the plugin
cp -r obs-airplay.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

### 6. Restart OBS Studio

Close and reopen OBS Studio to load the plugin.

## Usage

### Setting Up in OBS

1. **Add the AirPlay Source**
   - In OBS, click the `+` button in the Sources panel
   - Select "AirPlay" from the list
   - Give it a name (e.g., "iPhone Screen")
   - Click OK

2. **Connect from iOS**
   - On your iPhone/iPad, open Control Center (swipe down from top-right)
   - Tap "Screen Mirroring"
   - You should see "OBS AirPlay" in the list
   - Tap it to connect

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

## Configuration

### Changing Server Name

Edit `src/plugin-main.cpp`:

```cpp
g_airplay_server->start("Your Custom Name", 7000, 5000);
```

### Changing Ports

If ports 7000 or 5000 are already in use, you can change them:

```cpp
// AirPlay port (default 7000), RAOP port (default 5000)
g_airplay_server->start("OBS AirPlay", 7100, 5100);
```

**Note:** After changing ports, you must rebuild and reinstall the plugin.

### Enabling Password Protection

Currently, password protection is disabled. To enable it, modify the TXT records in `src/mdns-publisher.cpp`.

## Technical Details

### Protocols Used

1. **Bonjour/mDNS** - Service discovery (makes the receiver visible)
   - Service Type: `_airplay._tcp`
   - Service Type: `_raop._tcp`
   - Port: 5353 (multicast)

2. **AirPlay Protocol**
   - Port: 7000 (TCP)
   - HTTP-based protocol
   - Handles video streaming and control

3. **RAOP (Remote Audio Output Protocol)**
   - Port: 5000 (TCP)
   - RTSP-based protocol
   - Handles audio streaming

### Video Format
- Codec: H.264
- Container: MPEG-TS
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
    ├── plugin-main.cpp      # Plugin entry point
    ├── airplay-source.cpp   # OBS source implementation
    ├── airplay-source.hpp
    ├── airplay-server.cpp   # AirPlay server
    ├── airplay-server.hpp
    ├── mdns-publisher.cpp   # Bonjour advertising
    ├── mdns-publisher.hpp
    ├── h264-decoder.cpp     # Video decoder
    ├── h264-decoder.hpp
    ├── audio-decoder.cpp    # Audio decoder
    ├── audio-decoder.hpp
    ├── raop-server.cpp      # Audio server
    └── raop-server.hpp
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

3. **Attach Debugger**
   ```bash
   # Find OBS process ID
   ps aux | grep OBS
   
   # Attach lldb
   lldb -p <PID>
   ```

## Known Issues

1. **iOS 17+ Compatibility**
   - Some iOS 17 devices may require additional authentication
   - Working on implementing HAP (HomeKit Accessory Protocol)

2. **4K Streaming**
   - Currently limited to 1080p
   - 4K support planned for future release

3. **DRM Content**
   - Protected content (Netflix, etc.) cannot be mirrored
   - This is an Apple limitation

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

- **Issues**: [GitHub Issues](https://github.com/yourusername/obs-airplay-plugin/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yourusername/obs-airplay-plugin/discussions)

## Acknowledgments

- Thanks to mika314 for the original obs-airplay project
- OBS Studio team for the excellent plugin API
- FFmpeg team for multimedia codecs
