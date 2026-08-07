#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstddef>

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
typedef std::function<void()> ConnectionResetCallback;

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
    void setConnectionResetCallback(ConnectionResetCallback callback);
    
private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_first_video_logged{false};
    std::atomic<bool> m_connection_timing_active{false};
    std::atomic<uint64_t> m_connection_started_ns{0};
    VideoFrameCallback m_video_callback;
    AudioDataCallback m_audio_callback;
    ConnectionResetCallback m_reset_callback;
    std::mutex m_mutex;
    
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
    void processConnReset();
};
