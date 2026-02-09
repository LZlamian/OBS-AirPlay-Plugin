#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
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

class UxPlayIntegration {
public:
    UxPlayIntegration();
    ~UxPlayIntegration();
    
    // Start the UxPlay server
    bool start(const std::string& device_id, int port = 7000);
    
    // Stop the server
    void stop();
    
    // Check if running
    bool isRunning() const { return m_running; }
    
    // Get the actual port UxPlay is running on
    uint16_t getActualPort() const { return m_actual_port; }

    // Get the Public Key string from UxPlay
    std::string getPK() const;
    
    // Disable UxPlay's internal mDNS to prevent crashes
    void disableInternalMDNS();
    
    // Set callbacks for video and audio
    void setVideoCallback(VideoFrameCallback callback);
    void setAudioCallback(AudioDataCallback callback);
    
private:
    bool m_running;
    VideoFrameCallback m_video_callback;
    AudioDataCallback m_audio_callback;
    std::mutex m_mutex;
    
    // Thread for handling incoming data
    std::thread* m_worker_thread;
    std::condition_variable m_cv;
    std::atomic<bool> m_should_stop;
    
    // UxPlay RAOP instance
    raop_t* m_raop;

    // UxPlay DNS-SD state used by RAOP handlers (for /info and TXT payloads)
    dnssd_t* m_dnssd;
    
    // Actual port UxPlay is running on
    uint16_t m_actual_port;
    
    // Internal video/audio processing
    void processVideoData(video_decode_struct* data);
    void processAudioData(audio_decode_struct* data);
    
    // Worker thread function
    void workerThread();
};
