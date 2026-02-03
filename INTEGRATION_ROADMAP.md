# OBS AirPlay Plugin - Current Status & Integration Path

## ✅ What We've Accomplished

1. **Plugin Infrastructure** ✓
   - Compiles successfully on macOS
   - Loads into OBS without errors
   - Proper CMake build system
   - Plugin bundle structure correct

2. **mDNS/Bonjour Discovery** ✓
   - Advertises as `_airplay._tcp` service
   - Advertises as `_raop._tcp` service
   - Shows up in iOS Screen Mirroring list
   - Correct TXT records with device ID, features, model

3. **Network Server** ✓
   - HTTP server on port 7000 (AirPlay)
   - RTSP/HTTP server on port 5000 (RAOP)
   - Accepts connections from iOS devices
   - Multi-threaded connection handling

4. **Protocol Handshake** ✓ (Partial)
   - Handles GET /info (server info)
   - Handles POST /pair-setup (authentication)
   - Handles POST /pair-verify
   - Handles POST /fp-setup (FairPlay)
   - Responds with proper HTTP headers

## ❌ What's Missing - The Video Stream

The handshake completes but iOS closes the connection because we're not handling the actual video/audio stream data that comes next.

## The Complete AirPlay Streaming Flow

```
1. [✓] Discovery         - iOS finds "OBS AirPlay" via mDNS
2. [✓] Connection        - iOS connects to port 7000
3. [✓] Server Info       - iOS sends GET /info, we respond
4. [✓] Pair Setup        - iOS sends POST /pair-setup, we respond
5. [✗] Stream Setup      - iOS sends POST /stream with configuration
6. [✗] UDP Stream Start  - iOS opens UDP ports for RTP video/audio
7. [✗] Video Reception   - Receive H.264 NAL units over RTP
8. [✗] Audio Reception   - Receive AAC/ALAC audio over RTP  
9. [✗] Decryption        - Decrypt encrypted streams (FairPlay)
10. [✗] Decoding         - Decode H.264 to YUV frames
11. [✗] OBS Output       - Feed frames to OBS source
```

## Integration Options

### Option 1: Full UxPlay Integration (Most Complete)

**Pros:**
- Complete, battle-tested implementation
- Handles encryption, decoding, everything
- Active development and bug fixes

**Cons:**
- Requires GStreamer (large dependency)
- Complex build system integration
- May be overkill for our needs

**Approach:**
```bash
# Add as submodule
git submodule add https://github.com/FDH2/UxPlay.git external/uxplay

# In CMakeLists.txt
add_subdirectory(external/uxplay)
target_link_libraries(obs-airplay PRIVATE uxplay)
```

**Code changes needed:**
- Modify UxPlay to output frames via callback instead of GStreamer
- Integrate UxPlay's server into our existing server infrastructure
- Convert UxPlay frames to OBS format

### Option 2: Extract UxPlay Core (Recommended)

**Pros:**
- Get the essential AirPlay protocol handling
- Avoid GStreamer dependency
- Use our existing FFmpeg for decoding
- Lighter weight

**Cons:**
- Need to understand which parts of UxPlay to extract
- Some integration work required

**What to extract from UxPlay:**
1. `lib/raop.c` - RAOP protocol handling
2. `lib/dnssd.c` - Service discovery (we have this)
3. `lib/stream.c` - Stream management
4. `lib/rtp.c` - RTP packet handling (KEY!)
5. `lib/fairplay.c` - FairPlay decryption
6. `renderers/video_renderer.c` - Frame output

**Integration steps:**
1. Copy these files to `src/uxplay/`
2. Remove GStreamer dependencies
3. Replace GStreamer output with OBS output
4. Use our existing FFmpeg decoders

### Option 3: Minimal Custom Implementation (Educational)

Build from scratch to understand the protocol.

**What we need to implement:**

#### A. Stream Setup Handler
```cpp
std::string AirPlayServer::handleStreamSetup(const std::string& request) {
    // Parse plist body to get stream configuration
    // Extract: video_codec, audio_codec, encryption keys, etc.
    
    // Set up UDP sockets for RTP reception
    int video_port = 6000;
    int audio_port = 7000;
    
    // Return response with our RTP ports
}
```

#### B. RTP Receiver
```cpp
void receiveRTPVideo(int udp_socket) {
    while (streaming) {
        uint8_t buffer[2048];
        recv(udp_socket, buffer, sizeof(buffer), 0);
        
        // Parse RTP header
        RTPHeader* rtp = parseRTPHeader(buffer);
        
        // Extract H.264 NAL unit
        uint8_t* h264_nal = buffer + rtp->header_length;
        
        // Feed to decoder
        decodeAndOutputFrame(h264_nal, rtp->payload_length);
    }
}
```

#### C. H.264 Decoder (we have this already!)
```cpp
// Our existing h264-decoder.cpp can be used
// Just need to feed it NAL units from RTP
```

#### D. FairPlay Decryption
```cpp
// This is the hardest part
// AirPlay video is encrypted with AES
// Need to:
// 1. Extract encryption keys from handshake
// 2. Decrypt each RTP packet before decoding
```

## My Recommendation

**Start with Option 2: Extract UxPlay Core**

Here's why:
1. We get proven, working code
2. We avoid the GStreamer complexity
3. We keep our existing server infrastructure
4. We use our existing FFmpeg decoders
5. It's the fastest path to a working plugin

## Next Steps (If You Want to Proceed)

1. **Download UxPlay source** to examine
   ```bash
   git clone https://github.com/FDH2/UxPlay.git
   cd UxPlay
   ```

2. **Identify key files** to extract:
   - `lib/raop.c` - Core protocol
   - `lib/rtp.c` - RTP packet handling
   - `lib/fairplay.c` - Decryption

3. **Create adapter layer**:
   ```cpp
   // src/uxplay-adapter.cpp
   // Bridges UxPlay code to our plugin
   ```

4. **Test incrementally**:
   - First: Can we receive RTP packets?
   - Second: Can we parse them?
   - Third: Can we decrypt them?
   - Fourth: Can we decode to frames?
   - Fifth: Can we output to OBS?

## Alternative: Use as Reference

We could also use UxPlay purely as a reference to understand the protocol, then implement our own version. This would be more work but give us full control.

## Time Estimate

- **Option 1 (Full UxPlay)**: 2-3 days of integration work
- **Option 2 (Extract core)**: 1-2 days
- **Option 3 (Custom)**: 1-2 weeks

## Current Blocker

The immediate issue is: **iOS closes the connection after pair-setup because we don't handle the /stream POST request that comes next.**

We need to:
1. Add handler for POST /stream
2. Parse the stream configuration plist
3. Set up UDP sockets for RTP
4. Start receiving video/audio data

Would you like me to:
A) Create handlers for the stream setup (quick win - at least keep connection alive)
B) Begin integrating UxPlay core files
C) Create a detailed protocol implementation guide
D) Something else?
