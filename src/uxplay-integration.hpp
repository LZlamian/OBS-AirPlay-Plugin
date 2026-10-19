// SPDX-License-Identifier: GPL-3.0-or-later
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

#include "audio-decoder.hpp"
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
struct raop_callbacks_s;
typedef struct raop_ntp_s raop_ntp_t;

// Video frame callback: one compressed access unit. `data` belongs to UxPlay
// and is valid only during the call; copy it to keep it.
typedef std::function<void(const uint8_t* data, size_t size, uint64_t pts, bool is_h265)> VideoFrameCallback;

// Raw compressed AirPlay audio payload callback; `data` is valid only during
// the call.
typedef std::function<void(const uint8_t* data, size_t size, uint8_t codec_type, uint64_t pts)> AudioDataCallback;

// The audio format a sender negotiated for its next audio stream (SETUP).
typedef std::function<void(const AirPlayAudioFormat& format)> AudioFormatCallback;

// Connection reset callback (called when AirPlay client disconnects/reconnects)
// clear_output=false flushes decoders but keeps the last frame on screen.
// flush_decoders=false also leaves the mirror decoders untouched.
typedef std::function<void(bool clear_output, bool flush_decoders)> ConnectionResetCallback;

class UxPlayIntegration {
public:
    UxPlayIntegration();
    ~UxPlayIntegration();
    
    // The AirPlay TCP port, and the one tried when it is taken (macOS's own
    // AirPlay Receiver also listens on 7000). Senders find the port through
    // Bonjour, so either works.
    static constexpr int kDefaultPort = 7000;
    static constexpr int kFallbackPort = 7001;

    // Start the UxPlay server
    bool start(const std::string& device_id, int port = kDefaultPort,
               const std::string& server_name = "OBS AirPlay");
    
    // Stop the server
    void stop();
    
    // Check if running
    bool isRunning() const { return m_running.load(); }
    
    // Get the actual port UxPlay is running on
    uint16_t getActualPort() const { return m_actual_port; }

    // Why start() failed, or what is wrong with the running receiver (not
    // advertised); empty when all is well. For the source panel.
    std::string lastError() const;
    // "Waiting for a device", "Device connected", "Screen mirroring" or
    // "Playing AirPlay video".
    std::string connectionState() const;
    // Called, from any thread, when connectionState() has changed.
    void setStateChangedCallback(std::function<void()> callback);

    // The "AirPlay video" setting: Off (accept_video false) and Only from
    // the sending device (sender_only). Applies to the next POST /play (and,
    // for sender_only, to what the current item fetches next). Callable
    // before start().
    void setVideoPolicy(bool accept_video, bool sender_only);
    // The latest refused AirPlay video request, "HH:MM:SS: reason", or empty.
    // Cleared when an item is accepted.
    std::string lastRefusal() const;

    // Update the advertised server name in the UxPlay dnssd context (live, no restart)
    void updateServerName(const std::string& name);
    
    // Set callbacks for video, audio, and connection reset
    void setVideoCallback(VideoFrameCallback callback);
    void setAudioCallback(AudioDataCallback callback);
    void setAudioFormatCallback(AudioFormatCallback callback);
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
    AudioFormatCallback m_audio_format_callback;  // under m_mutex
    ConnectionResetCallback m_reset_callback;
    std::mutex m_mutex;

    // Safari's Media AirPlay mode sends a URL through POST /play rather than
    // the encoded mirroring stream used by Photos and Screen Mirroring.
    std::unique_ptr<MediaPlayer> m_media_player;
    
    // UxPlay RAOP instance
    raop_t* m_raop;

    // UxPlay DNS-SD state used by RAOP handlers (for /info and TXT payloads)
    dnssd_t* m_dnssd;
    // The context replaced by the last name change (see updateServerName).
    dnssd_t* m_retired_dnssd = nullptr;

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
    const char* m_clear_reason = "";            // under m_clear_mutex
    // Open AirPlay connections of any kind. When the last one closes, or the
    // device ends its session (a TEARDOWN of everything), no device is
    // sending: the frame it left behind must not stay on screen.
    std::atomic<int> m_open_connections{0};
    // A picture from a device has been shown since the output was last
    // cleared. Devices that only look at the receiver (GET /info while the
    // AirPlay menu is open) connect and leave without one.
    std::atomic<bool> m_frame_shown{false};
    std::atomic<bool> m_stream_nal_logged{false};
    // `reason` (a string literal) goes to the log when the clear happens.
    void scheduleDeferredClear(const char* reason = "no new AirPlay video item followed /stop");
    void cancelDeferredClear();
    void deferredClearLoop();
    void stopDeferredClearThread();
    struct raop_callbacks_s makeRaopCallbacks();

    // Held for the whole of start(), stop() and updateServerName(), which
    // replace m_raop / m_dnssd (OBS main thread, name debounce thread).
    // Never taken on UxPlay's threads.
    std::mutex m_lifecycle_mutex;
    mutable std::mutex m_status_mutex;   // leaf lock
    std::string m_last_error;            // under m_status_mutex
    std::string m_last_state;            // under m_status_mutex
    std::function<void()> m_state_changed;  // under m_status_mutex
    std::string m_last_refusal;          // under m_status_mutex
    std::atomic<bool> m_accept_video{true};
    std::atomic<bool> m_sender_only{true};
    // Logs nothing (the refusing code has); records it for the panel.
    void recordRefusal(const std::string& reason);
    void clearRefusal();
    void setLastError(std::string error);
    void notifyStateChanged();
};
