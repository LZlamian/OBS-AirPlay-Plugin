# OBS AirPlay Plugin - Stream Reception Implementation

## What's New in This Version

We've added **RTP stream reception** based on UxPlay's architecture! The plugin can now:

1. ✅ Handle POST /stream requests from iOS
2. ✅ Advertise RTP ports for video and audio
3. ✅ Receive RTP packets over UDP  
4. ✅ Parse RTP headers and extract H.264/AAC payloads
5. ✅ Set up parallel threads for video and audio reception
6. ⚠️  Decryption stub (needs implementation for encrypted streams)

## New Files Added

### RTP Packet Handling
- `src/rtp-handler.hpp/cpp` - Parses RTP packet headers, extracts payloads
- `src/rtp-receiver.hpp/cpp` - UDP socket management, packet reception
- `src/airplay-stream.hpp/cpp` - Complete stream coordinator

### How It Works

```
iOS Device                    OBS Plugin
    |                             |
    |--POST /stream-------------->| Parse config, set up UDP sockets
    |<--200 OK (RTP ports 6000)---|
    |                             |
    |==RTP Video (UDP 6000)======>| Video thread receives
    |                             |   -> Parse RTP header
    |                             |   -> Extract H.264 NAL
    |                             |   -> (Decrypt if needed)
    |                             |   -> Feed to H264Decoder
    |                             |   -> Output to OBS
    |                             |
    |==RTP Audio (UDP 7000)======>| Audio thread receives
    |                             |   -> Parse RTP header
    |                             |   -> Extract AAC data
    |                             |   -> (Decrypt if needed)
    |                             |   -> Feed to AudioDecoder
    |                             |   -> Output to OBS
```

## Building

### Dependencies

```bash
# Install OpenSSL (new requirement for decryption)
brew install openssl@3

# Existing dependencies
brew install cmake pkg-config ffmpeg libplist openssl@3
```

### Build Commands

```bash
tar -xzf obs-airplay-stream-receiver.tar.gz
cd obs-airplay-plugin
rm -rf build
./build.sh
```

## Testing

1. **Restart OBS** completely
2. **Add AirPlay source** to your scene
3. **On iPad**: Control Center → Screen Mirroring → "OBS AirPlay"
4. **Watch the logs** in OBS (Help → Log Files → View Current Log)

### Expected Log Output

```
AirPlay source created
RAOP HTTP: GET /info
Sent server info response
RAOP HTTP: POST /pair-setup
Pair-setup requested (skipped - no auth required)
RAOP HTTP: POST /stream              ← NEW!
Stream setup request - iOS wants to start streaming!
Sent stream setup response with RTP ports: video=6000, audio=7000
Video receiver thread started         ← NEW!
Audio receiver thread started         ← NEW!
RTP: Receiver started on port 6000    ← NEW!
RTP: Receiver started on port 7000    ← NEW!
```

If you see video packets:
```
RTP: Received packet seq=1234, timestamp=567890, payload=1456 bytes
```

## Current Limitations

### 1. Encryption Not Implemented ⚠️

AirPlay typically encrypts video/audio streams. Our `decryptData()` function is a stub:

```cpp
std::vector<uint8_t> AirPlayStream::decryptData(const std::vector<uint8_t>& encrypted)
{
    // TODO: Implement AES-CTR decryption
    blog(LOG_WARNING, "Decryption not yet implemented");
    return encrypted; // Pass through unencrypted
}
```

**Why this matters:** iOS will likely send encrypted data. We need to:
- Extract encryption keys from the handshake
- Implement AES-128-CTR decryption using OpenSSL
- Decrypt each RTP payload before decoding

**Workaround:** Some older iOS versions or specific configs might send unencrypted for testing.

### 2. H.264 NAL Unit Assembly

RTP packets contain H.264 **NAL units** (Network Abstraction Layer). For video:
- Small frames: Single NAL per packet
- Large frames: NAL fragmented across multiple packets (FU-A mode)

Our current implementation passes each payload directly to the decoder. We may need:
```cpp
// Assemble fragmented NAL units
if (isFragmented(rtp_packet)) {
    appendToBuffer(rtp_packet.payload);
    if (isLastFragment(rtp_packet)) {
        completeNAL = getBuffer();
        decoder->decode(completeNAL);
        clearBuffer();
    }
}
```

### 3. Timestamp Synchronization

Video and audio have separate RTP streams with different timebases:
- Video: 90kHz clock  
- Audio: Sample rate clock (usually 44100Hz or 48000Hz)

We need to:
- Convert both to a common timebase
- Sync audio/video based on timestamps
- Handle clock drift

## Next Steps to Complete

### Immediate (Get Basic Streaming Working)

1. **Test unencrypted stream** - See if any data arrives
   ```bash
   # Monitor UDP ports
   sudo tcpdump -i any -n port 6000 or port 7000
   ```

2. **Add RTP packet logging** - See what iOS is actually sending
   ```cpp
   blog(LOG_INFO, "RTP packet: seq=%d, ts=%u, len=%zu", 
        packet.seq, packet.timestamp, packet.payload_size);
   ```

3. **Verify H.264 NAL units** - Check if they're valid
   ```cpp
   uint8_t nal_type = payload[0] & 0x1F;
   blog(LOG_INFO, "NAL unit type: %d", nal_type);
   ```

### Medium Term (Full Functionality)

4. **Implement Decryption**
   - Parse encryption keys from handshake
   - Use OpenSSL EVP_* functions for AES-CTR
   - Test with encrypted iOS streams

5. **Handle NAL Fragmentation**
   - Detect FU-A fragments (type 28)
   - Buffer and reassemble
   - Pass complete NAL to decoder

6. **Proper A/V Sync**
   - Implement PTS calculation
   - Buffer management
   - Jitter handling

### Long Term (Polish)

7. **Error Recovery**
   - Handle packet loss gracefully
   - Request retransmission (RTCP)
   - Skip corrupted frames

8. **Performance Optimization**
   - Zero-copy where possible
   - Lock-free queues
   - GPU decoding

9. **UI Improvements**
   - Connection status indicator
   - FPS/bitrate display
   - Quality settings

## Debugging

### Check if RTP packets are arriving

```bash
# On Mac, monitor UDP traffic
sudo tcpdump -i any -n -X port 6000

# Should see packets from iOS device IP
```

### Check OBS logs

```bash
tail -f ~/Library/Application\ Support/obs-studio/logs/*.txt | grep -E "RTP|Stream|Video|Audio"
```

### Common Issues

**No RTP packets received:**
- Firewall blocking UDP
- iOS not sending yet (still in handshake)
- Wrong ports advertised

**Encrypted data:**
- Implement decryption
- Or try older iOS version
- Or use developer options to disable encryption

**Video decoder errors:**
- NAL units fragmented
- Missing SPS/PPS headers
- Wrong codec parameters

## Architecture

```
AirPlayStream (src/airplay-stream.cpp)
├── Video Thread
│   ├── UDP Socket (port 6000)
│   ├── RTPReceiver::processPacket()
│   ├── decryptData() [stub]
│   └── H264Decoder::decode()
│       └── obs_source_output_video()
│
└── Audio Thread
    ├── UDP Socket (port 7000)
    ├── RTPReceiver::processPacket()
    ├── decryptData() [stub]
    └── AudioDecoder::decode()
        └── obs_source_output_audio()
```

## Resources

- **RTP Specification:** RFC 3550
- **H.264/AVC Spec:** ITU-T H.264
- **AirPlay Protocol:** Reverse-engineered docs
- **UxPlay Source:** https://github.com/FDH2/UxPlay (reference)

## Contributing

If you implement decryption or NAL assembly, please share! This is a community effort to create a fully functional open-source AirPlay receiver for OBS.

## License

Same as the original plugin - check LICENSE file.
