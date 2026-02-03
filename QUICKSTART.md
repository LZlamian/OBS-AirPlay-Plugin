# Quick Start Guide - OBS AirPlay Plugin

## TL;DR

```bash
# 1. Install dependencies
brew install cmake pkg-config ffmpeg libplist

# 2. Build and install
./build.sh

# 3. Restart OBS

# 4. Add AirPlay source in OBS

# 5. Screen mirror from iOS
```

## What This Plugin Does

This plugin makes your Mac running OBS Studio appear as an AirPlay receiver on your network, just like an Apple TV. When you open Screen Mirroring on your iPhone or iPad, you'll see "OBS AirPlay" in the list. Connect to it, and your device's screen appears in OBS.

## Key Differences from Other Solutions

Unlike the abandoned mika314/obs-airplay project, this plugin:

1. ✅ **Has proper Bonjour/mDNS advertising** - Your device actually shows up in iOS Screen Mirroring
2. ✅ **Works with modern OBS** - Compatible with OBS 28.0+ and latest macOS
3. ✅ **Native macOS APIs** - Uses Apple's DNS Service Discovery framework
4. ✅ **No external servers** - Everything runs inside OBS
5. ✅ **Proper AirPlay protocol** - Handles the full handshake and streaming

## Why Wasn't It Showing Up Before?

The most common issue with AirPlay receivers is **mDNS advertising**. iOS devices find AirPlay receivers using Bonjour (Apple's implementation of mDNS). If you don't advertise correctly:

- The device won't appear in the Screen Mirroring list
- iOS thinks there are no receivers available
- Connection attempts fail silently

This plugin implements proper mDNS advertising with:
- `_airplay._tcp` service on port 7000
- `_raop._tcp` service on port 5000  
- Correct TXT records with device ID, features, model info
- Proper MAC address formatting

## Network Requirements

**Critical:** Your Mac and iOS device MUST be on the same network segment (VLAN). AirPlay uses multicast DNS which doesn't cross router boundaries.

### Check Your Network
```bash
# On Mac, find your IP
ipconfig getifaddr en0

# On iOS: Settings -> WiFi -> (i) icon
# Compare the first three numbers (e.g., 192.168.1.x)
# They must match!
```

### Common Network Issues

❌ **Won't Work:**
- Mac on Ethernet (192.168.1.x), iPhone on WiFi (192.168.2.x)
- Mac on 5GHz WiFi, iPhone on 2.4GHz WiFi (if separate networks)
- Mac on VPN, iPhone not on VPN
- Guest WiFi network (often isolated)

✅ **Will Work:**
- Both on same WiFi network
- Both on same Ethernet network
- Both on same 5GHz or 2.4GHz WiFi (if bridged)

## Firewall Configuration

macOS firewall might block incoming AirPlay connections.

### Quick Test (Temporary)
```bash
# Disable firewall temporarily
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --setglobalstate off

# Test if AirPlay works now
# If yes, you need to add OBS to firewall exceptions

# Re-enable firewall
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --setglobalstate on
```

### Permanent Fix
```bash
# Add OBS to firewall exceptions
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add /Applications/OBS.app
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblock /Applications/OBS.app
```

## Verification Steps

### 1. Check Plugin Loaded
1. Open OBS
2. Go to Help -> Log Files -> View Current Log
3. Search for "AirPlay"
4. You should see:
   ```
   OBS AirPlay Plugin loaded (version 1.0.0)
   AirPlay server started successfully
   mDNS advertising started for 'OBS AirPlay'
   Server socket listening on port 7000
   Server socket listening on port 5000
   ```

### 2. Check mDNS Advertising
```bash
# Install dns-sd (comes with Xcode Command Line Tools)
dns-sd -B _airplay._tcp

# You should see output like:
# Timestamp     A/R    Flags  if Domain               Service Type         Instance Name
# 10:30:15.123  Add        2   4 local.               _airplay._tcp.       OBS AirPlay
```

Press Ctrl+C to stop.

### 3. Check Ports Are Open
```bash
# Check if OBS is listening
lsof -i :7000
lsof -i :5000

# You should see OBS (or obs-airplay) listed
```

### 4. Test from iOS
1. Open Control Center (swipe down from top-right on iPhone X+, swipe up on older)
2. Long-press Screen Mirroring button
3. Look for "OBS AirPlay"

## Troubleshooting

### "OBS AirPlay" doesn't appear on iOS

**Checklist:**
- [ ] Same WiFi network? (check IP addresses)
- [ ] Firewall allowing OBS?
- [ ] Plugin loaded in OBS? (check logs)
- [ ] mDNS advertising working? (run `dns-sd -B _airplay._tcp`)
- [ ] Ports 7000 and 5000 not in use by something else?
- [ ] Try restarting OBS
- [ ] Try restarting Mac
- [ ] Try forgetting and rejoining WiFi on iOS

### Connection Immediately Drops

**Possible causes:**
- Weak WiFi signal (move closer to router)
- Network congestion (too many devices)
- Firewall blocking after initial connection
- OBS source not added correctly

**Solutions:**
```bash
# Check network quality
ping -c 10 <iphone-ip-address>

# Should show low latency (< 10ms) and no packet loss
```

### No Video Appears in OBS

**Checklist:**
- [ ] AirPlay source added to scene?
- [ ] Source visible (eye icon not crossed out)?
- [ ] iOS device shows "OBS AirPlay" with green icon?
- [ ] Try stopping and restarting mirroring on iOS

### Video is Laggy or Stuttering

**Optimize your setup:**
1. Use 5GHz WiFi if available
2. Move closer to router
3. Close other apps on Mac
4. Lower OBS output resolution
5. Disable unnecessary OBS sources
6. Check Activity Monitor for CPU usage

### Logs Show "Failed to start mDNS advertising"

This usually means the DNS Service Discovery framework isn't working.

```bash
# Restart mDNSResponder
sudo killall -HUP mDNSResponder

# Check if it's running
ps aux | grep mDNSResponder
```

## Advanced Configuration

### Change Server Name

Edit `src/plugin-main.cpp` line where `start()` is called:

```cpp
g_airplay_server->start("My Custom Name", 7000, 5000);
```

Then rebuild:
```bash
./build.sh
```

### Use Different Ports

If ports 7000 or 5000 are in use:

```cpp
g_airplay_server->start("OBS AirPlay", 7100, 5100);
```

**Note:** Non-standard ports might cause issues with some iOS versions.

### Enable More Verbose Logging

In OBS:
1. Settings -> Advanced
2. Recording -> Log Level -> Debug
3. Restart OBS

Check logs at: `~/Library/Application Support/obs-studio/logs/`

## Performance Tips

### For Best Quality
- Use 5GHz WiFi
- Ensure strong signal (> -60 dBm)
- Close bandwidth-heavy apps
- Use wired Ethernet for Mac if possible

### For Low Latency
- Reduce video buffer in iOS (Settings -> Screen Mirroring)
- Lower OBS output resolution
- Use faster encoding preset (if streaming)

### For Stability
- Keep Mac plugged in (prevents throttling)
- Disable WiFi power saving
- Use dedicated network for streaming if possible

## Getting Help

If you're still having issues:

1. **Collect Information:**
   ```bash
   # OBS logs
   cat ~/Library/Application\ Support/obs-studio/logs/$(ls -t ~/Library/Application\ Support/obs-studio/logs/ | head -1)
   
   # Network info
   ifconfig | grep "inet "
   
   # mDNS status
   dns-sd -B _airplay._tcp
   
   # Port status
   lsof -i :7000
   lsof -i :5000
   ```

2. **Check System:**
   - macOS version: `sw_vers`
   - OBS version: About OBS
   - WiFi network name
   - iOS device model and version

3. **Create GitHub Issue** with all the above information

## FAQ

**Q: Does this work with Android?**
A: No, this is AirPlay (Apple's protocol). Android uses Miracast/Google Cast.

**Q: Can I use this over the internet?**
A: No, AirPlay requires local network. Use other streaming solutions for remote.

**Q: Does it support 4K?**
A: Currently limited to 1080p. 4K support planned.

**Q: Will it work with DRM content (Netflix, etc)?**
A: No, DRM content can't be mirrored. This is an Apple restriction.

**Q: Can multiple devices connect simultaneously?**
A: Currently one device at a time. Multi-device support is planned.

**Q: Is there latency?**
A: Yes, typically 100-300ms depending on network. This is inherent to AirPlay.

## Credits

Based on research from:
- [mika314/obs-airplay](https://github.com/mika314/obs-airplay)
- [OpenAirPlay Specification](https://openairplay.github.io/airplay-spec/)
- [FDH2/UxPlay](https://github.com/FDH2/UxPlay)

Built for the OBS community with ❤️
