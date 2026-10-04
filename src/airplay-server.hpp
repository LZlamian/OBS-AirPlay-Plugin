#pragma once
#include <functional>

#include "uxplay-integration.hpp"
#include "h264-decoder.hpp"
#include "audio-decoder.hpp"
#include "media-player.hpp"
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstddef>

// Forward declaration
struct obs_source;
typedef struct obs_source obs_source_t;

// Decodes what UxPlay receives (the mirroring stream, and the pictures and
// sound of URL playback) and hands it to the registered OBS sources. UxPlay
// does all the networking; see UxPlayIntegration.
class AirPlayServer {
public:
    AirPlayServer();
    ~AirPlayServer();
    
    // Release the registered sources (plugin unload).
    void stop();

    // Flush FFmpeg decoder contexts (call on client reconnect for clean state)
    // clear_output=false keeps the last frame on screen (URL media item
    // switches); a later frame or reset replaces it.
    // flush_decoders=false leaves the mirror decoders alone: an AirPlay video
    // /play or /stop is not a mirror stream boundary, and a flushed H.264
    // decoder shows nothing until the next keyframe, which a mirror stream
    // that keeps running may not send.
    void resetDecoders(bool clear_output = true, bool flush_decoders = true);

    // Called after each decoded screen-mirroring frame reaches OBS.
    void setMirrorFrameOutputCallback(std::function<void()> callback)
    {
        m_mirror_frame_output_callback = std::move(callback);
    }

    // Register a source to receive AirPlay data
    void registerSource(obs_source_t* source);
    void unregisterSource(obs_source_t* source);
    
    // The receiver identity (device id) generated for this installation.
    std::string getMACAddress() const { return m_mac_address; }
    void setMACAddress(const std::string& mac) { m_mac_address = mac; }

    // Feed encoded video from UxPlay and output decoded frames to registered OBS sources.
    void ingestVideoBitstream(const uint8_t* data, size_t size, uint64_t pts, bool is_h265);
    void ingestAudioBitstream(const uint8_t* data, size_t size, uint8_t codec_type, uint64_t pts);
    void outputMediaVideoFrame(const MediaVideoFrame& decoded);
    void outputMediaAudioFrame(const MediaAudioFrame& decoded);
    
private:
    std::function<void()> m_mirror_frame_output_callback;
    // Registered sources (weak refs — do not prevent source destruction)
    std::mutex m_sources_mutex;
    std::vector<obs_weak_source_t*> m_registered_sources;
    
    // Generate MAC address
    std::string generateMACAddress();
    uint64_t normalizeTimestamp(uint64_t source_timestamp);
    void resetStreamClock();
    std::string m_mac_address;

    std::unique_ptr<H264Decoder> m_h264_decoder;
    std::unique_ptr<H264Decoder> m_h265_decoder;
    std::unique_ptr<AudioDecoder> m_audio_decoder;
    std::mutex m_decoder_mutex;        // video decoders and their frames
    std::mutex m_audio_decoder_mutex;  // taken alone, or after m_decoder_mutex
    uint64_t m_video_frame_counter = 0;
    uint64_t m_first_decoded_frame_ns = 0;
    // Mirror statistics window ([MIRROR] line every ~10 s while frames arrive).
    uint64_t m_mirror_stats_started_ns = 0;
    uint32_t m_mirror_stats_frames = 0;
    uint64_t m_audio_frame_counter = 0;

    // UxPlay timestamps use a stable nanosecond clock, while OBS expects its
    // own monotonic timebase. A shared anchor preserves A/V offsets.
    std::mutex m_timestamp_mutex;
    bool m_timestamp_initialized = false;
    uint64_t m_timestamp_source_origin = 0;
    uint64_t m_timestamp_obs_origin = 0;

    // Latency telemetry (rolling window stats)
    uint64_t m_v_decode_ns_sum = 0, m_v_decode_ns_max = 0;
    uint64_t m_v_output_ns_sum = 0, m_v_output_ns_max = 0;
    uint64_t m_v_last_output_ns = 0;
    uint64_t m_v_interval_ns_sum = 0, m_v_interval_ns_max = 0;
    uint32_t m_v_window_count = 0;

    uint64_t m_a_decode_ns_sum = 0, m_a_decode_ns_max = 0;
    uint64_t m_a_output_ns_sum = 0, m_a_output_ns_max = 0;
    uint32_t m_a_window_count = 0;
};

// Global toggle for per-frame latency telemetry. Off by default; flipped on
// from the source properties pane.
extern std::atomic<bool> g_latency_telemetry_enabled;
