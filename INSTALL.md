# Installation Guide - OBS AirPlay Plugin

## Prerequisites

You need:
1. **macOS 10.15 or later**
2. **OBS Studio** installed at `/Applications/OBS.app`
3. **Homebrew** package manager
4. **Xcode Command Line Tools**

## Step-by-Step Installation

### 1. Install Xcode Command Line Tools

```bash
xcode-select --install
```

Click "Install" when prompted.

### 2. Install Homebrew (if not already installed)

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

### 3. Install Dependencies

```bash
brew install cmake pkg-config ffmpeg libplist
```

**Optional but recommended:**
```bash
brew install fdk-aac
```

### 4. Download and Extract the Plugin

```bash
# Extract the archive
tar -xzf obs-airplay-plugin.tar.gz
cd obs-airplay-plugin
```

### 5. Build the Plugin

**Option A: Automated (Recommended)**
```bash
./build.sh
```

**Option B: Manual**
```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# Install
cp -r obs-airplay.plugin "$HOME/Library/Application Support/obs-studio/plugins/"
```

### 6. Restart OBS Studio

Close OBS completely and restart it.

## Build Release Artifacts (zip + pkg)

To create distributable macOS artifacts:

```bash
chmod +x scripts/package-macos.sh
./scripts/package-macos.sh
```

Generated files:

- `dist/obs-airplay-v<version>-macos-<arch>.zip`
- `dist/obs-airplay-v<version>-macos-<arch>.pkg`

The `.pkg` installer copies the plugin into the active user's:

- `~/Library/Application Support/obs-studio/plugins/obs-airplay.plugin`

### Any-Mac Distribution (Apple Silicon + Intel)

Build one artifact per architecture, then distribute both:

```bash
# On Apple Silicon host
ARCH_OVERRIDE=arm64 bash scripts/package-macos.sh

# On Intel host (or x86_64 environment with x86_64 OBS/deps)
ARCH_OVERRIDE=x86_64 bash scripts/package-macos.sh
```

This creates:

- `dist/obs-airplay-v<version>-macos-arm64.pkg`
- `dist/obs-airplay-v<version>-macos-x86_64.pkg`

Note: `scripts/package-macos.sh` now validates dependency architecture early and fails with a clear message if the requested arch is unavailable locally.

## Verify Installation

### 1. Check Plugin Loaded

In OBS:
1. Go to **Help** → **Log Files** → **View Current Log**
2. Search for "AirPlay" (Cmd+F)
3. You should see:
   ```
   OBS AirPlay Plugin loaded (version 1.1.0)
   AirPlay server started successfully
   mDNS advertising started for 'OBS AirPlay'
   ```

### 2. Add AirPlay Source

1. In OBS, click the **+** button in the Sources panel
2. Select **"AirPlay"** from the list
3. Name it (e.g., "iPhone Screen") and click **OK**

### 3. Test from iOS

1. On iPhone/iPad, open **Control Center**
2. Tap **Screen Mirroring**
3. You should see **"OBS AirPlay"** in the list
4. Tap it to connect

## Troubleshooting

### Plugin Not Showing in Sources

**Check OBS logs for errors:**
```bash
cat ~/Library/Application\ Support/obs-studio/logs/*.txt | grep -i "airplay.*error"
```

**Reinstall the plugin:**
```bash
cd obs-airplay-plugin
rm -rf build
./build.sh
```

### "OBS AirPlay" Not Showing on iOS

**1. Check both devices are on same WiFi network**

On Mac:
```bash
ipconfig getifaddr en0
```

On iOS: Settings → WiFi → (i) icon

First three numbers should match (e.g., 192.168.1.x)

**2. Check firewall settings**

```bash
# Check if firewall is blocking
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate

# If enabled, allow OBS
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add /Applications/OBS.app
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblock /Applications/OBS.app
```

**3. Verify mDNS advertising**

```bash
dns-sd -B _airplay._tcp
```

You should see "OBS AirPlay" appear. Press Ctrl+C to stop.

**4. Check ports**

```bash
lsof -i :7000
lsof -i :5000
```

Both should show OBS is listening.

**5. Run diagnostics**

```bash
cd obs-airplay-plugin
./diagnose.sh
```

### Build Errors

**"Could not find libobs"**
- Ensure OBS is installed at `/Applications/OBS.app`
- If installed elsewhere: `cmake .. -DOBS_BUNDLE_PATH="/path/to/OBS.app"`

**"Package 'libavcodec' not found"**
```bash
brew reinstall ffmpeg
```

**"Package 'libplist-2.0' not found"**
```bash
brew reinstall libplist
```

**"Cannot find dns_sd.h"**
```bash
xcode-select --install
```

### Connection Issues

**iOS connects but drops immediately:**
- WiFi signal too weak (move closer to router)
- Network congestion (close other apps)
- Firewall blocking after initial connection

**No video in OBS:**
- Ensure AirPlay source is added to a scene
- Check source is visible (eye icon not crossed out)
- Verify iOS shows green "connected" indicator

**Laggy or stuttering video:**
- Use 5GHz WiFi instead of 2.4GHz
- Reduce network traffic
- Lower OBS output resolution
- Check CPU usage in Activity Monitor

## Advanced Configuration

### Change Server Name

Edit `src/plugin-main.cpp`:
```cpp
g_airplay_server->start("My Custom Name", 7000, 5000);
```
Then rebuild: `./build.sh`

### Use Different Ports

If ports 7000/5000 are in use:
```cpp
g_airplay_server->start("OBS AirPlay", 7100, 5100);
```

### Enable Debug Logging

In OBS:
1. **Settings** → **Advanced**
2. **Log Level** → **Debug**
3. Restart OBS

## Uninstallation

```bash
rm -rf "$HOME/Library/Application Support/obs-studio/plugins/obs-airplay.plugin"
```

Restart OBS.

## Getting Help

1. **Run diagnostics:**
   ```bash
   ./diagnose.sh > diagnostics.txt
   ```

2. **Collect logs:**
   ```bash
   cat ~/Library/Application\ Support/obs-studio/logs/*.txt | grep -i airplay > airplay-logs.txt
   ```

3. **Create GitHub issue** with diagnostics and logs

## Common Questions

**Q: Do I need the OBS source code?**
A: No, this plugin builds against the installed OBS.app

**Q: Can I use this with OBS from Homebrew?**
A: Not currently. Install the official OBS from https://obsproject.com

**Q: Does this work with OBS version X?**
A: Works with OBS 28.0 or later. Check your version in Help → About OBS

**Q: Can I build a universal binary (Intel + Apple Silicon)?**
A: Yes, but requires building separately for each architecture

## Success Checklist

- [ ] Xcode Command Line Tools installed
- [ ] Homebrew installed
- [ ] Dependencies installed (cmake, pkg-config, ffmpeg, libplist)
- [ ] OBS Studio installed
- [ ] Plugin built successfully
- [ ] Plugin appears in OBS log
- [ ] AirPlay source available in OBS
- [ ] "OBS AirPlay" visible in iOS Screen Mirroring
- [ ] Both devices on same WiFi network
- [ ] Firewall allows OBS connections

If all checked, you're ready to stream! 🎉
