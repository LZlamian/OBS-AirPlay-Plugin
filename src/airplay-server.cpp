#include "airplay-server.hpp"
#include "airplay-source.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <random>
#include <iomanip>
#include <vector>

std::atomic<bool> g_latency_telemetry_enabled{false};

AirPlayServer::AirPlayServer()
{
    m_mac_address = generateMACAddress();
    m_h264_decoder = std::make_unique<H264Decoder>();
    m_h265_decoder = std::make_unique<H264Decoder>(AV_CODEC_ID_HEVC);
    m_audio_decoder = std::make_unique<AudioDecoder>();
}

AirPlayServer::~AirPlayServer()
{
    stop();
}

std::string AirPlayServer::generateMACAddress()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::stringstream ss;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ss << ":";
        ss << std::hex << std::setw(2) << std::setfill('0') << std::uppercase << dis(gen);
    }
    
    return ss.str();
}

void AirPlayServer::resetDecoders(bool clear_output, bool flush_decoders)
{
    std::lock_guard<std::mutex> lock(m_decoder_mutex);
    if (flush_decoders) {
        if (m_h264_decoder) m_h264_decoder->flush();
        if (m_h265_decoder) m_h265_decoder->flush();
        std::lock_guard<std::mutex> audio_lock(m_audio_decoder_mutex);
        if (m_audio_decoder) m_audio_decoder->flush();
    }
    {
        // Native and URL playback both update these counters while holding
        // m_sources_mutex. Keep reconnect/source-switch resets synchronized.
        std::lock_guard<std::mutex> sources_lock(m_sources_mutex);
        for (obs_weak_source_t* weak : m_registered_sources) {
            if (!clear_output) break;
            obs_source_t* source = obs_weak_source_get_source(weak);
            if (source) {
                // OBS documents a null asynchronous frame as deactivating the
                // texture and releasing all retained frames.
                obs_source_output_video(source, nullptr);
                obs_source_release(source);
            }
        }
        m_video_frame_counter = 0; // re-enable first-frame timing log on reconnect
        m_mirror_stats_started_ns = 0;
        m_first_decoded_frame_ns = 0;
        m_audio_frame_counter = 0;
    }
    airplay_source_notify_frame_queued(0);
    resetStreamClock();
    blog(LOG_INFO, "AirPlay output reset (decoders %s, last OBS frame %s)",
         flush_decoders ? "flushed" : "kept", clear_output ? "cleared" : "kept");
}

void AirPlayServer::resetStreamClock()
{
    std::lock_guard<std::mutex> lock(m_timestamp_mutex);
    m_timestamp_initialized = false;
    m_timestamp_source_origin = 0;
    m_timestamp_obs_origin = 0;
}

uint64_t AirPlayServer::normalizeTimestamp(uint64_t source_timestamp)
{
    const uint64_t now = os_gettime_ns();
    if (source_timestamp == 0)
        return now;

    constexpr uint64_t kMaximumClockDeltaNs = UINT64_C(5000000000);
    std::lock_guard<std::mutex> lock(m_timestamp_mutex);

    auto resetAnchor = [&] {
        m_timestamp_initialized = true;
        m_timestamp_source_origin = source_timestamp;
        m_timestamp_obs_origin = now;
        return now;
    };

    if (!m_timestamp_initialized)
        return resetAnchor();

    uint64_t normalized = 0;
    if (source_timestamp >= m_timestamp_source_origin) {
        const uint64_t delta = source_timestamp - m_timestamp_source_origin;
        if (UINT64_MAX - m_timestamp_obs_origin < delta)
            return resetAnchor();
        normalized = m_timestamp_obs_origin + delta;
    } else {
        const uint64_t delta = m_timestamp_source_origin - source_timestamp;
        if (delta > m_timestamp_obs_origin)
            return resetAnchor();
        normalized = m_timestamp_obs_origin - delta;
    }

    // A sender restart or clock discontinuity must not leave OBS waiting on a
    // timestamp far in the future (or treating fresh media as stale).
    if ((normalized > now && normalized - now > kMaximumClockDeltaNs) ||
        (now > normalized && now - normalized > kMaximumClockDeltaNs)) {
        return resetAnchor();
    }
    return normalized;
}

void AirPlayServer::stop()
{
    // Release all weak source refs
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    for (obs_weak_source_t* weak : m_registered_sources) {
        obs_weak_source_release(weak);
    }
    m_registered_sources.clear();
}

void AirPlayServer::registerSource(obs_source_t* source)
{
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    m_registered_sources.push_back(obs_source_get_weak_source(source));
    blog(LOG_INFO, "Source registered with AirPlay server");
}

void AirPlayServer::unregisterSource(obs_source_t* source)
{
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    auto it = std::find_if(m_registered_sources.begin(), m_registered_sources.end(),
        [source](obs_weak_source_t* weak) {
            obs_source_t* s = obs_weak_source_get_source(weak);
            bool match = (s == source);
            if (s) obs_source_release(s);
            return match;
        });
    if (it != m_registered_sources.end()) {
        obs_weak_source_release(*it);
        m_registered_sources.erase(it);
        blog(LOG_INFO, "Source unregistered from AirPlay server");
    }
}

void AirPlayServer::ingestVideoBitstream(const uint8_t* data, size_t size, uint64_t pts, bool is_h265)
{
    if (!data || size == 0) {
        return;
    }

    H264Decoder* decoder = is_h265 ? m_h265_decoder.get() : m_h264_decoder.get();
    if (!decoder) {
        static bool logged_decoder_missing = false;
        if (!logged_decoder_missing) {
            blog(LOG_WARNING, "No %s decoder instance available",
                 is_h265 ? "H265/HEVC" : "H264");
            logged_decoder_missing = true;
        }
        return;
    }

    // DecodedVideoFrame references buffers owned by the decoder. Keep this lock
    // until OBS has copied the frame so another packet or reconnect reset cannot
    // invalidate those buffers underneath obs_source_output_video().
    std::unique_lock<std::mutex> dec_lock(m_decoder_mutex);
    DecodedVideoFrame decoded;
    const bool tele = g_latency_telemetry_enabled.load(std::memory_order_relaxed);
    const bool first_frame = m_video_frame_counter == 0;
    const uint64_t t_decode_start = (tele || first_frame) ? os_gettime_ns() : 0;
    if (!decoder->decodeToI420(data, size, decoded)) {
        return;
    }
    const uint64_t t_decode_end = (tele || first_frame) ? os_gettime_ns() : 0;

    obs_source_frame frame = {};
    frame.data[0] = decoded.data[0];
    frame.data[1] = decoded.data[1];
    frame.data[2] = decoded.data[2];
    frame.linesize[0] = decoded.linesize[0];
    frame.linesize[1] = decoded.linesize[1];
    frame.linesize[2] = decoded.linesize[2];
    frame.width = static_cast<uint32_t>(decoded.width);
    frame.height = static_cast<uint32_t>(decoded.height);
    // Hardware decoding delivers NV12; OBS takes it directly, so no colour
    // conversion happens on the CPU.
    frame.format = decoded.nv12 ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_I420;
    frame.full_range = decoded.full_range;
    frame.trc = VIDEO_TRC_DEFAULT;
    video_format_get_parameters_for_format(VIDEO_CS_709,
                                           decoded.full_range ? VIDEO_RANGE_FULL
                                                              : VIDEO_RANGE_PARTIAL,
                                           frame.format,
                                           frame.color_matrix,
                                           frame.color_range_min,
                                           frame.color_range_max);
    frame.timestamp = normalizeTimestamp(pts);

    std::lock_guard<std::mutex> lock(m_sources_mutex);
    const uint64_t t_output_start = tele ? os_gettime_ns() : 0;

    // Log the very first frame output to OBS for connection timing diagnostics.
    if (first_frame) {
        m_first_decoded_frame_ns = t_decode_end;
        blog(LOG_INFO, "[CONNECT] first decoded frame queued to OBS (%dx%d, decode %.2fms)",
             decoded.width, decoded.height, (t_decode_end - t_decode_start) / 1e6);
    }

    const uint64_t frame_number = m_video_frame_counter + 1;
    if (frame_number == 1 || frame_number == 30 || frame_number == 60 ||
        frame_number == 120 || frame_number == 180 || frame_number == 240) {
        uint64_t luma_sum = 0;
        uint64_t luma_hash = UINT64_C(1469598103934665603);
        uint32_t samples = 0;
        uint8_t luma_min = 255;
        uint8_t luma_max = 0;
        for (int y = 0; y < decoded.height; y += 24) {
            const uint8_t* row = decoded.data[0] + y * decoded.linesize[0];
            for (int x = 0; x < decoded.width; x += 24) {
                const uint8_t value = row[x];
                luma_sum += value;
                luma_min = std::min(luma_min, value);
                luma_max = std::max(luma_max, value);
                luma_hash ^= value;
                luma_hash *= UINT64_C(1099511628211);
                ++samples;
            }
        }
        const uint64_t sampled_ns = os_gettime_ns();
        const double elapsed_ms = m_first_decoded_frame_ns && sampled_ns >= m_first_decoded_frame_ns
            ? (sampled_ns - m_first_decoded_frame_ns) / 1e6 : 0.0;
        blog(LOG_INFO,
             "[DISPLAY] decoded frame #%llu +%.1fms luma avg=%.1f range=%u-%u hash=%016llx",
             static_cast<unsigned long long>(frame_number), elapsed_ms,
             samples ? static_cast<double>(luma_sum) / samples : 0.0,
             static_cast<unsigned>(luma_min), static_cast<unsigned>(luma_max),
             static_cast<unsigned long long>(luma_hash));
    }

    for (obs_weak_source_t* weak : m_registered_sources) {
        obs_source_t* source = obs_weak_source_get_source(weak);
        if (source) {
            obs_source_output_video(source, &frame);
            obs_source_release(source);
        }
    }
    if (first_frame) {
        airplay_source_notify_frame_queued(os_gettime_ns());
    }
    if (m_mirror_frame_output_callback) {
        m_mirror_frame_output_callback();
    }
    const uint64_t t_output_end = tele ? os_gettime_ns() : 0;

    ++m_video_frame_counter;

    // One line every ~10 s of mirroring: size, frame rate, decode mode and a
    // coarse brightness marker (black is 0 in a full-range stream, 16 in a
    // video-range one), to tell a moving picture from a held or black one
    // after the fact.
    {
        const uint64_t stats_now = os_gettime_ns();
        if (!m_mirror_stats_started_ns) {
            m_mirror_stats_started_ns = stats_now;
            m_mirror_stats_frames = 0;
        }
        ++m_mirror_stats_frames;
        constexpr uint64_t kMirrorStatsIntervalNs = UINT64_C(10000000000);
        if (stats_now - m_mirror_stats_started_ns >= kMirrorStatsIntervalNs) {
            uint64_t luma_sum = 0;
            uint32_t samples = 0;
            for (int y = 0; y < decoded.height; y += 48) {
                const uint8_t* row = decoded.data[0] + y * decoded.linesize[0];
                for (int x = 0; x < decoded.width; x += 48) {
                    luma_sum += row[x];
                    ++samples;
                }
            }
            const double seconds = (stats_now - m_mirror_stats_started_ns) / 1e9;
            blog(LOG_INFO, "[MIRROR] %dx%d %s, %u frames in %.1fs (%.1f fps), %s decoding, luma avg %.0f",
                 decoded.width, decoded.height, is_h265 ? "HEVC" : "H264",
                 m_mirror_stats_frames, seconds, m_mirror_stats_frames / seconds,
                 decoder->usingHardware() ? "hardware" : "software",
                 samples ? static_cast<double>(luma_sum) / samples : 0.0);
            m_mirror_stats_started_ns = stats_now;
            m_mirror_stats_frames = 0;
        }
    }

    if (tele) {
        const uint64_t decode_ns = t_decode_end - t_decode_start;
        const uint64_t output_ns = t_output_end - t_output_start;
        m_v_decode_ns_sum += decode_ns;
        if (decode_ns > m_v_decode_ns_max) m_v_decode_ns_max = decode_ns;
        m_v_output_ns_sum += output_ns;
        if (output_ns > m_v_output_ns_max) m_v_output_ns_max = output_ns;
        if (m_v_last_output_ns != 0) {
            const uint64_t interval = t_output_end - m_v_last_output_ns;
            m_v_interval_ns_sum += interval;
            if (interval > m_v_interval_ns_max) m_v_interval_ns_max = interval;
        }
        m_v_last_output_ns = t_output_end;
        ++m_v_window_count;
        if (m_v_window_count >= 120) {
            const double n = static_cast<double>(m_v_window_count);
            const double intervals_n = n > 1.0 ? n - 1.0 : 1.0;
            blog(LOG_INFO,
                 "[latency] video N=%u  decode avg=%.2fms max=%.2fms  "
                 "output avg=%.2fms max=%.2fms  interval avg=%.2fms max=%.2fms (~%.1ffps)",
                 m_v_window_count,
                 (m_v_decode_ns_sum / n) / 1e6,
                 m_v_decode_ns_max / 1e6,
                 (m_v_output_ns_sum / n) / 1e6,
                 m_v_output_ns_max / 1e6,
                 (m_v_interval_ns_sum / intervals_n) / 1e6,
                 m_v_interval_ns_max / 1e6,
                 1e9 * intervals_n / static_cast<double>(m_v_interval_ns_sum ? m_v_interval_ns_sum : 1));
            m_v_decode_ns_sum = m_v_decode_ns_max = 0;
            m_v_output_ns_sum = m_v_output_ns_max = 0;
            m_v_interval_ns_sum = m_v_interval_ns_max = 0;
            m_v_window_count = 0;
        }
    } else if (m_v_window_count != 0) {
        m_v_decode_ns_sum = m_v_decode_ns_max = 0;
        m_v_output_ns_sum = m_v_output_ns_max = 0;
        m_v_interval_ns_sum = m_v_interval_ns_max = 0;
        m_v_window_count = 0;
        m_v_last_output_ns = 0;
    }

    if ((m_video_frame_counter % 120) == 0) {
        blog(LOG_INFO, "Output video frame #%llu (%s, %ux%u)",
             static_cast<unsigned long long>(m_video_frame_counter),
             is_h265 ? "HEVC" : "H264",
             frame.width,
             frame.height);
    }
}

void AirPlayServer::ingestAudioBitstream(const uint8_t* data, size_t size, uint8_t codec_type, uint64_t pts)
{
    if (!data || size == 0 || !m_audio_decoder) {
        return;
    }

    std::vector<float> left;
    std::vector<float> right;
    int sample_rate = 0;
    const bool tele = g_latency_telemetry_enabled.load(std::memory_order_relaxed);
    const uint64_t t_decode_start = tele ? os_gettime_ns() : 0;
    {
        // Its own lock: the video lock is held while a picture is decoded
        // and copied into OBS, and audio must not wait for that.
        std::lock_guard<std::mutex> dec_lock(m_audio_decoder_mutex);
        if (!m_audio_decoder->decode(data, size, codec_type, left, right, sample_rate)) {
            return;
        }
    }
    const uint64_t t_decode_end = tele ? os_gettime_ns() : 0;

    if (left.empty() || right.empty() || left.size() != right.size()) {
        return;
    }

    obs_source_audio audio = {};
    audio.data[0] = reinterpret_cast<uint8_t*>(left.data());
    audio.data[1] = reinterpret_cast<uint8_t*>(right.data());
    audio.frames = static_cast<uint32_t>(left.size());
    audio.speakers = SPEAKERS_STEREO;
    audio.samples_per_sec = static_cast<uint32_t>(sample_rate);
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.timestamp = normalizeTimestamp(pts);

    std::lock_guard<std::mutex> lock(m_sources_mutex);
    const uint64_t t_output_start = tele ? os_gettime_ns() : 0;
    for (obs_weak_source_t* weak : m_registered_sources) {
        obs_source_t* source = obs_weak_source_get_source(weak);
        if (source) {
            obs_source_output_audio(source, &audio);
            obs_source_release(source);
        }
    }
    const uint64_t t_output_end = tele ? os_gettime_ns() : 0;

    ++m_audio_frame_counter;

    if (tele) {
        const uint64_t decode_ns = t_decode_end - t_decode_start;
        const uint64_t output_ns = t_output_end - t_output_start;
        m_a_decode_ns_sum += decode_ns;
        if (decode_ns > m_a_decode_ns_max) m_a_decode_ns_max = decode_ns;
        m_a_output_ns_sum += output_ns;
        if (output_ns > m_a_output_ns_max) m_a_output_ns_max = output_ns;
        ++m_a_window_count;
        if (m_a_window_count >= 240) {
            const double n = static_cast<double>(m_a_window_count);
            blog(LOG_INFO,
                 "[latency] audio N=%u  decode avg=%.2fms max=%.2fms  "
                 "output avg=%.2fms max=%.2fms",
                 m_a_window_count,
                 (m_a_decode_ns_sum / n) / 1e6,
                 m_a_decode_ns_max / 1e6,
                 (m_a_output_ns_sum / n) / 1e6,
                 m_a_output_ns_max / 1e6);
            m_a_decode_ns_sum = m_a_decode_ns_max = 0;
            m_a_output_ns_sum = m_a_output_ns_max = 0;
            m_a_window_count = 0;
        }
    } else if (m_a_window_count != 0) {
        m_a_decode_ns_sum = m_a_decode_ns_max = 0;
        m_a_output_ns_sum = m_a_output_ns_max = 0;
        m_a_window_count = 0;
    }

    if ((m_audio_frame_counter % 240) == 0) {
        blog(LOG_INFO, "Output audio frame #%llu (codec=%u, frames=%u, rate=%u)",
             static_cast<unsigned long long>(m_audio_frame_counter),
             static_cast<unsigned int>(codec_type),
             audio.frames,
             audio.samples_per_sec);
    }
}

void AirPlayServer::outputMediaVideoFrame(const MediaVideoFrame& decoded)
{
    if (!decoded.data[0] || !decoded.data[1] || (!decoded.nv12 && !decoded.data[2]) ||
        decoded.width <= 0 || decoded.height <= 0) {
        return;
    }

    obs_source_frame frame = {};
    for (int i = 0; i < (decoded.nv12 ? 2 : 3); ++i) {
        frame.data[i] = const_cast<uint8_t*>(decoded.data[i]);
        frame.linesize[i] = decoded.linesize[i];
    }
    frame.width = static_cast<uint32_t>(decoded.width);
    frame.height = static_cast<uint32_t>(decoded.height);
    frame.format = decoded.nv12 ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_I420;
    frame.full_range = decoded.full_range;
    frame.trc = VIDEO_TRC_DEFAULT;
    video_format_get_parameters_for_format(decoded.bt601 ? VIDEO_CS_601 : VIDEO_CS_709,
                                           decoded.full_range ? VIDEO_RANGE_FULL
                                                              : VIDEO_RANGE_PARTIAL,
                                           frame.format,
                                           frame.color_matrix,
                                           frame.color_range_min,
                                           frame.color_range_max);
    frame.timestamp = decoded.timestamp_ns ? decoded.timestamp_ns : os_gettime_ns();

    const bool first_frame = m_video_frame_counter == 0;
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    for (obs_weak_source_t* weak : m_registered_sources) {
        obs_source_t* source = obs_weak_source_get_source(weak);
        if (source) {
            obs_source_output_video(source, &frame);
            obs_source_release(source);
        }
    }
    ++m_video_frame_counter;
    if (first_frame) {
        blog(LOG_INFO, "[MEDIA] first Safari video frame queued to OBS (%dx%d)",
             decoded.width, decoded.height);
        airplay_source_notify_frame_queued(os_gettime_ns());
    } else if ((m_video_frame_counter % 300) == 0) {
        blog(LOG_INFO, "[MEDIA] output Safari video frame #%llu (%dx%d)",
             static_cast<unsigned long long>(m_video_frame_counter),
             decoded.width, decoded.height);
    }
}

void AirPlayServer::outputMediaAudioFrame(const MediaAudioFrame& decoded)
{
    if (!decoded.data[0] || !decoded.data[1] || decoded.frames == 0 ||
        decoded.sample_rate == 0) {
        return;
    }

    obs_source_audio audio = {};
    audio.data[0] = reinterpret_cast<uint8_t*>(const_cast<float*>(decoded.data[0]));
    audio.data[1] = reinterpret_cast<uint8_t*>(const_cast<float*>(decoded.data[1]));
    audio.frames = decoded.frames;
    audio.speakers = SPEAKERS_STEREO;
    audio.samples_per_sec = decoded.sample_rate;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.timestamp = decoded.timestamp_ns ? decoded.timestamp_ns : os_gettime_ns();

    std::lock_guard<std::mutex> lock(m_sources_mutex);
    for (obs_weak_source_t* weak : m_registered_sources) {
        obs_source_t* source = obs_weak_source_get_source(weak);
        if (source) {
            obs_source_output_audio(source, &audio);
            obs_source_release(source);
        }
    }
    ++m_audio_frame_counter;
    if (m_audio_frame_counter == 1) {
        blog(LOG_INFO, "[MEDIA] first Safari audio frame queued to OBS (%u Hz, %u frames)",
             decoded.sample_rate, decoded.frames);
    } else if ((m_audio_frame_counter % 500) == 0) {
        blog(LOG_INFO, "[MEDIA] output Safari audio frame #%llu",
             static_cast<unsigned long long>(m_audio_frame_counter));
    }
}
