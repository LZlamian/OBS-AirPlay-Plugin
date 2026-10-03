#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>

#include "media-player.hpp"

// UxPlay core includes
extern "C" {
#include "../uxplay/lib/stream.h"
#include "../uxplay/lib/dnssd.h"
}

// Forward declarations
struct obs_source;
typedef struct obs_source obs_source_t;

// Forward declare UxPlay types
typedef struct raop_s raop_t;
typedef struct raop_ntp_s raop_ntp_t;

// Video frame callback
typedef std::function<void(const uint8_t* data, size_t size, uint64_t pts, bool is_h265)> VideoFrameCallback;

// Raw compressed AirPlay audio payload callback
typedef std::function<void(const uint8_t* data, size_t size, uint8_t codec_type, uint64_t pts)> AudioDataCallback;

// Connection reset callback (called when AirPlay client disconnects/reconnects)
// clear_output=false flushes decoders but keeps the last frame on screen.
// flush_decoders=false also leaves the mirror decoders untouched.
typedef std::function<void(bool clear_output, bool flush_decoders)> ConnectionResetCallback;

class UxPlayIntegration {
public:
    UxPlayIntegration();
    ~UxPlayIntegration();
    
    // Start the UxPlay server
    bool start(const std::string& device_id, int port = 7000,
               const std::string& server_name = "OBS AirPlay");
    
    // Stop the server
    void stop();
    
    // Check if running
    bool isRunning() const { return m_running.load(); }
    
    // Get the actual port UxPlay is running on
    uint16_t getActualPort() const { return m_actual_port; }

    // Get the Public Key string from UxPlay
    std::string getPK() const;
    
    // Disable UxPlay's internal mDNS to prevent crashes
    void disableInternalMDNS();

    // Update the advertised server name in the UxPlay dnssd context (live, no restart)
    void updateServerName(const std::string& name);
    
    // Set callbacks for video, audio, and connection reset
    void setVideoCallback(VideoFrameCallback callback);
    void setAudioCallback(AudioDataCallback callback);
    void setMediaVideoCallback(MediaVideoCallback callback);
    void setMediaAudioCallback(MediaAudioCallback callback);
    void setConnectionResetCallback(ConnectionResetCallback callback);

    // A decoded mirroring frame reached OBS: it replaces the held AirPlay
    // video frame, so a pending post-/stop clear is no longer needed.
    void onMirrorFrameOutput();
    
private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_first_video_logged{false};
    std::atomic<bool> m_connection_timing_active{false};
    std::atomic<uint64_t> m_connection_started_ns{0};
    VideoFrameCallback m_video_callback;
    AudioDataCallback m_audio_callback;
    ConnectionResetCallback m_reset_callback;
    std::mutex m_mutex;

    // Safari's Media AirPlay mode sends a URL through POST /play rather than
    // the encoded mirroring stream used by Photos and Screen Mirroring.
    std::unique_ptr<MediaPlayer> m_media_player;
    
    // UxPlay RAOP instance
    raop_t* m_raop;

    // UxPlay DNS-SD state used by RAOP handlers (for /info and TXT payloads)
    dnssd_t* m_dnssd;

    // Hardware address and server name stored so dnssd can be reinitialized on name change
    std::array<char, 6> m_hw_addr;
    std::string m_server_name;
    
    // Actual port UxPlay is running on
    uint16_t m_actual_port;
    
    // Internal video/audio/reset processing
    void processVideoData(video_decode_struct* data);
    void processAudioData(audio_decode_struct* data);
    void processConnReset(bool clear_output = true, bool flush_decoders = true);

    // URL media (AirPlay video) item on screen: mirror teardowns must not
    // blank it, and /stop clears it only after a short grace period so an
    // immediate item switch never flashes an empty source.
    std::atomic<bool> m_media_active{false};
    std::thread m_clear_thread;
    std::mutex m_clear_mutex;
    std::condition_variable m_clear_cv;
    std::atomic<bool> m_clear_pending{false};  // written under m_clear_mutex
    bool m_clear_exit = false;
    std::chrono::steady_clock::time_point m_clear_deadline;
    void scheduleDeferredClear();
    void cancelDeferredClear();
    void deferredClearLoop();
};
