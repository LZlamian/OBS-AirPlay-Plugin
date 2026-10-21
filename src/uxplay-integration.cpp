// SPDX-License-Identifier: GPL-3.0-or-later
#include "uxplay-integration.hpp"
#include "media-url-policy.hpp"
#include "hardware-decode-option.hpp"
#include "scope-exit.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/stat.h>
#include <utility>
#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#endif

// UxPlay headers (extern "C" to avoid C++ name mangling issues)
extern "C" {
#include "../uxplay/lib/raop.h"
#include "../uxplay/lib/netutils.h"
#include "../uxplay/lib/logger.h"
#include "../uxplay/lib/stream.h"
#include "../uxplay/lib/dnssd.h"
}

namespace {
#if defined(_WIN32)
// The Windows counterpart: the Multimedia Class Scheduler's "Playback" task,
// released when the receiver thread ends (UxPlay ends it with the session).
struct MmcssRegistration {
    HANDLE handle = nullptr;
    ~MmcssRegistration()
    {
        if (handle) AvRevertMmThreadCharacteristics(handle);
    }
};
#endif

// Boost the calling thread to USER_INTERACTIVE QoS exactly once per thread.
// UxPlay invokes our video/audio callbacks on its receiver thread; raising
// its scheduling class reduces jitter spikes when other apps compete for
// CPU. macOS's QoS API does not require special entitlements and gracefully
// no-ops if the requested class can't be granted.
void boost_caller_thread_qos_once(const char* tag)
{
#if defined(__APPLE__)
    static thread_local bool boosted = false;
    if (boosted) {
        return;
    }
    boosted = true;
    int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc == 0) {
        blog(LOG_INFO, "[RECEIVER] Boosted %s receiver thread to QOS_CLASS_USER_INTERACTIVE", tag);
    } else {
        blog(LOG_WARNING, "[RECEIVER] Failed to boost %s receiver thread QoS (rc=%d)", tag, rc);
    }
#elif defined(_WIN32)
    static thread_local MmcssRegistration registration;
    static thread_local bool boosted = false;
    if (boosted) {
        return;
    }
    boosted = true;
    DWORD task_index = 0;
    registration.handle = AvSetMmThreadCharacteristicsW(L"Playback", &task_index);
    if (registration.handle) {
        blog(LOG_INFO, "[RECEIVER] Registered %s receiver thread with MMCSS (Playback)", tag);
    } else {
        blog(LOG_WARNING, "[RECEIVER] Failed to register %s receiver thread with MMCSS (error %lu)", tag,
             static_cast<unsigned long>(GetLastError()));
    }
#else
    (void)tag;
#endif
}

// The AirPlay video protocol trace ([MEDIA-TRACE] lines: /play, /reverse,
// FCUP, playback controls) is off by default.  Enable it with the
// OBS_AIRPLAY_MEDIA_TRACE=1 environment variable or a file named
// obs-airplay-media-trace in the plugin's config folder (read when the
// receiver starts: restart OBS after creating it).
bool media_trace_requested()
{
    if (const char* env = std::getenv("OBS_AIRPLAY_MEDIA_TRACE")) {
        return env[0] && std::strcmp(env, "0") != 0;
    }
    return flag_file_exists("obs-airplay-media-trace");
}

// A localised message with one integer in it.
std::string format_text(const char* key, int value)
{
    char text[512];
    snprintf(text, sizeof(text), obs_module_text(key), value);
    return text;
}

// The AirServer profile advertises features 0x0E427FFFF7, which include
// FairPlay-protected video (bit 2). The receiver cannot play protected video;
// the claim draws POST /fp-setup2 and protected-HLS requests. Masking it is
// opt-in (OBS_AIRPLAY_MASK_FAIRPLAY_VIDEO=1, docs/DEVELOPMENT.md) until it
// has been checked on real iOS and macOS senders that connection speed is
// unchanged.
bool mask_fairplay_video_requested()
{
    const char* env = std::getenv("OBS_AIRPLAY_MASK_FAIRPLAY_VIDEO");
    return env && std::strcmp(env, "1") == 0;
}

void apply_fast_connect_profile(dnssd_t* dnssd)
{
    dnssd_set_airserver_profile(dnssd, true);
    if (mask_fairplay_video_requested()) {
        constexpr int kFeatureVideoFairPlay = 2;
        dnssd_set_airplay_features(dnssd, kFeatureVideoFairPlay, 0);
        static std::once_flag logged;
        std::call_once(logged, [] {
            blog(LOG_INFO, "[RECEIVER] Fast connection profile: FairPlay video feature bit masked "
                           "(OBS_AIRPLAY_MASK_FAIRPLAY_VIDEO=1)");
        });
    }
}
} // namespace

UxPlayIntegration::UxPlayIntegration()
    : m_media_player(std::make_unique<MediaPlayer>())
    , m_raop(nullptr)
    , m_dnssd(nullptr)
    , m_hw_addr{}
    , m_server_name("OBS AirPlay")
    , m_actual_port(0)
{
    m_clear_thread = std::thread(&UxPlayIntegration::deferredClearLoop, this);
    MediaPlayer::setRefusalCallback([this](const std::string& reason) { recordRefusal(reason); });
}

UxPlayIntegration::~UxPlayIntegration()
{
    stop();
    stopDeferredClearThread();
    MediaPlayer::setRefusalCallback(nullptr);
}

void UxPlayIntegration::stopDeferredClearThread()
{
    {
        std::lock_guard<std::mutex> lock(m_clear_mutex);
        m_clear_exit = true;
    }
    m_clear_cv.notify_all();
    if (m_clear_thread.joinable()) {
        m_clear_thread.join();
    }
}

void UxPlayIntegration::scheduleDeferredClear(const char* reason)
{
    constexpr auto kGracePeriod = std::chrono::milliseconds(450);
    {
        std::lock_guard<std::mutex> lock(m_clear_mutex);
        m_clear_pending = true;
        m_clear_reason = reason;
        m_clear_deadline = std::chrono::steady_clock::now() + kGracePeriod;
    }
    m_clear_cv.notify_all();
}

void UxPlayIntegration::onMirrorFrameOutput()
{
    if (!m_frame_shown.exchange(true, std::memory_order_relaxed)) {
        notifyStateChanged();
    }
    // Only a decoded picture counts; codec headers alone do not replace the
    // held frame (seen after iOS resumes an idle mirror stream).
    if (m_clear_pending) {
        cancelDeferredClear();
    }
}

void UxPlayIntegration::cancelDeferredClear()
{
    std::lock_guard<std::mutex> lock(m_clear_mutex);
    m_clear_pending = false;
}

void UxPlayIntegration::deferredClearLoop()
{
    std::unique_lock<std::mutex> lock(m_clear_mutex);
    while (!m_clear_exit) {
        if (!m_clear_pending) {
            m_clear_cv.wait(lock);
            continue;
        }
        if (std::chrono::steady_clock::now() < m_clear_deadline) {
            m_clear_cv.wait_until(lock, m_clear_deadline);
            continue;
        }
        m_clear_pending = false;
        const char* reason = m_clear_reason;
        lock.unlock();
        blog(LOG_INFO, "[MEDIA] %s; clearing output", reason);
        m_frame_shown.store(false);
        if (m_open_connections.load() <= 0) {
            // No device is connected any more: nothing may keep playing, and
            // the next stream starts with fresh decoders.
            if (m_media_active.exchange(false) && m_media_player) {
                m_media_player->stop();
            }
            notifyStateChanged();
            processConnReset(true, true);
        } else {
            // The mirror stream may still be running; only the picture is cleared.
            processConnReset(true, false);
        }
        lock.lock();
    }
}

// UxPlay's callbacks into this object (cls). They run on UxPlay's threads:
// the HTTP thread (requests, AirPlay video), the mirror and audio threads.
// No exception can cross UxPlay's C frames, hence noexcept: one that escapes
// ends the process here, where the cause is visible.
struct UxPlayIntegration::RaopCallbacks {
    // Timing: fires when iOS first TCP-connects (before handshake).
    static void conn_init(void* cls) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (self) {
            self->m_open_connections.fetch_add(1);
            self->notifyStateChanged();
        }
        const uint64_t now = os_gettime_ns();
        bool expected = false;
        if (self && self->m_connection_timing_active.compare_exchange_strong(expected, true)) {
            self->m_first_video_logged.store(false, std::memory_order_relaxed);
            self->m_connection_started_ns.store(now);
            blog(LOG_INFO, "[CONNECT] +0.00ms TCP accepted — AirPlay negotiation starting");
        }
    }

    // A device that goes away leaves its last frame behind unless the
    // output is cleared: after a short grace period, so that a new
    // connection that shows something at once (an item switch, a
    // reconnect) replaces the frame without an empty source in between.
    static void conn_destroy(void* cls) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (!self) {
            return;
        }
        if (self->m_open_connections.fetch_sub(1) <= 1) {
            self->m_open_connections.store(0);
            self->m_connection_timing_active.store(false);
            if (self->m_frame_shown.load()) {
                self->scheduleDeferredClear("the last AirPlay connection closed");
            }
        }
        self->notifyStateChanged();
    }

    // A TEARDOWN that names no stream ends the device's whole mirroring
    // session (UxPlay reports a video reset only for a single stream).
    static void conn_teardown(void* cls, bool* teardown_96, bool* teardown_110) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (!self || (teardown_96 && *teardown_96) || (teardown_110 && *teardown_110)) {
            return;
        }
        self->processConnReset(false, true);
        // iOS also ends its mirroring session while AirPlay video goes
        // on over other connections; that item stays.
        if (!self->m_media_active.load() && self->m_frame_shown.load()) {
            self->scheduleDeferredClear("the device ended its AirPlay session");
        }
    }

    static void conn_request_timing(void* cls, const char* method, const char* url, bool started) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (!self || !method || !url) {
            return;
        }
        if (strcmp(url, "/feedback") == 0 || strcmp(url, "/playback-info") == 0) {
            return;
        }

        const uint64_t now = os_gettime_ns();
        const uint64_t origin = self->m_connection_started_ns.load();
        const double elapsed_ms = origin && now >= origin ? (now - origin) / 1e6 : 0.0;
        static thread_local uint64_t request_started_ns = 0;
        if (started) {
            request_started_ns = now;
            blog(LOG_INFO, "[CONNECT] +%.2fms -> %s %s", elapsed_ms, method, url);
        } else {
            const double request_ms = request_started_ns && now >= request_started_ns
                ? (now - request_started_ns) / 1e6 : 0.0;
            blog(LOG_INFO, "[CONNECT] +%.2fms <- %s %s (handler %.2fms)",
                 elapsed_ms, method, url, request_ms);
            request_started_ns = 0;
            if (strcmp(method, "TEARDOWN") == 0) {
                self->m_connection_timing_active.store(false);
            }
        }
    }

    static void mirror_connection_timing(void* cls, bool first_data) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        const uint64_t started_ns = self->m_connection_started_ns.load(std::memory_order_relaxed);
        if (!started_ns) {
            return;
        }
        const double elapsed_ms = (os_gettime_ns() - started_ns) / 1e6;
        blog(LOG_INFO, "[CONNECT] +%.2fms mirroring TCP %s",
             elapsed_ms, first_data ? "first header received" : "accepted");
    }

    // Set video and audio processing callbacks
    static void video_process(void* cls, raop_ntp_t*, video_decode_struct* data) noexcept
    {
        if (cls && data) {
            auto* self = static_cast<UxPlayIntegration*>(cls);
            self->processVideoData(data);
        }
    }

    // SETUP names the audio format of the stream about to start: the decoder
    // is set up for its rate, channels and depth (all 44.1 kHz stereo from
    // iOS so far, but the receiver accepts 48 kHz formats too).
    static void audio_get_format(void* cls, unsigned char* ct, unsigned short* spf, bool* using_screen, bool* is_media, uint64_t* audio_format) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (!self || !ct || !spf || !audio_format) return;
        AirPlayAudioFormat format;
        const bool known = airplay_audio::formatFromSetup(*audio_format, *spf, format);
        blog(known ? LOG_INFO : LOG_WARNING,
             "[AUDIO] stream format: %s, %u samples per packet (audioFormat 0x%llx, %s%s)%s",
             airplay_audio::describe(format, *ct).c_str(), static_cast<unsigned>(*spf),
             static_cast<unsigned long long>(*audio_format),
             using_screen && *using_screen ? "screen" : "audio only",
             is_media && *is_media ? ", media" : "",
             known ? "" : "; format not recognised, decoding as 44.1 kHz stereo");
        AudioFormatCallback callback;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            callback = self->m_audio_format_callback;
        }
        if (callback) callback(format);
    }

    static void audio_process(void* cls, raop_ntp_t*, audio_decode_struct* data) noexcept
    {
        if (cls && data) {
            auto* self = static_cast<UxPlayIntegration*>(cls);
            self->processAudioData(data);
        }
    }

    // Required by UxPlay runtime; provide safe defaults for callbacks
    // that UxPlay may call without null checks.
    static void conn_feedback(void* cls) noexcept
    {
        blog(LOG_DEBUG, "[UxPlay] feedback heartbeat received");
        UNUSED_PARAMETER(cls);
    }

    static void conn_reset(void* cls, int reason) noexcept
    {
        if (cls) {
            auto* self = static_cast<UxPlayIntegration*>(cls);
            blog(LOG_INFO, "[UxPlay] Connection reset (reason=%d) — flushing decoders", reason);
            self->m_connection_timing_active.store(false);
            self->processConnReset(!self->m_media_active.load());
        }
    }

    static void video_flush(void* cls) noexcept
    {
        UNUSED_PARAMETER(cls);
    }

    static void audio_flush(void* cls) noexcept
    {
        UNUSED_PARAMETER(cls);
    }

    static void audio_set_volume(void* cls, float volume) noexcept
    {
        UNUSED_PARAMETER(cls);
        UNUSED_PARAMETER(volume);
    }

    static void audio_set_metadata(void* cls, const void* buffer, int buflen) noexcept
    {
        UNUSED_PARAMETER(cls);
        UNUSED_PARAMETER(buffer);
        UNUSED_PARAMETER(buflen);
    }

    static void audio_set_coverart(void* cls, const void* buffer, int buflen) noexcept
    {
        UNUSED_PARAMETER(cls);
        UNUSED_PARAMETER(buffer);
        UNUSED_PARAMETER(buflen);
    }

    static void audio_set_progress(void* cls, uint32_t* start, uint32_t* curr, uint32_t* end) noexcept
    {
        UNUSED_PARAMETER(cls);
        UNUSED_PARAMETER(start);
        UNUSED_PARAMETER(curr);
        UNUSED_PARAMETER(end);
    }

    static double audio_set_client_volume(void* cls) noexcept
    {
        UNUSED_PARAMETER(cls);
        return -15.0;
    }

    static void on_video_play(void* cls, const char* location, const float start_position) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (!self || !self->m_media_player || !location || !*location) {
            blog(LOG_ERROR, "[MEDIA] POST /play did not provide a usable media URL");
            return;
        }
        if (!self->m_accept_video.load()) {
            // Turned off after the receiver accepted the item (an HLS item
            // starts once its playlists have come over the reverse channel).
            static const char reason[] = "AirPlay video is turned off on this receiver";
            blog(LOG_WARNING, "[MEDIA] AirPlay video item refused: %s", reason);
            self->recordRefusal(reason);
            return;
        }
        blog(LOG_INFO, "[MEDIA] POST /play supplied a media URL (start=%.3fs)",
             start_position);
        self->clearRefusal();
        // Stop a previous URL worker before resetting shared decoder
        // state. The previous frame stays on screen until the new item's
        // first frame replaces it, so switches never flash an empty source.
        self->cancelDeferredClear();
        self->m_media_player->stop();
        // Not a mirror stream boundary: iOS may cancel this item and keep
        // mirroring without a new keyframe, so leave the mirror decoder.
        self->processConnReset(false, false);
        self->m_media_active.store(true);
        self->notifyStateChanged();
        self->m_frame_shown.store(true);
        self->m_media_player->play(location, start_position);
    }

    static void on_video_refused(void* cls, const char* reason) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (self) self->recordRefusal(reason ? reason : "refused");
    }

    static void on_video_scrub(void* cls, const float position) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (self && self->m_media_player) {
            blog(LOG_INFO, "[MEDIA] POST /scrub requested %.3fs", position);
            self->m_media_player->seek(position);
        }
    }

    static void on_video_rate(void* cls, const float rate) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (self && self->m_media_player) {
            self->m_media_player->setRate(rate);
        }
    }

    static void on_video_stop(void* cls) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        if (self && self->m_media_player) {
            blog(LOG_INFO, "[MEDIA] stopping URL playback (output clears unless a new item follows)");
            self->m_media_player->stop();
            self->m_media_active.store(false);
            self->notifyStateChanged();
            self->processConnReset(false, false);
            self->scheduleDeferredClear();
        }
    }

    static void on_video_acquire_playback_info(void* cls, playback_info_t* playback_video) noexcept
    {
        if (!playback_video) {
            return;
        }
        auto* self = static_cast<UxPlayIntegration*>(cls);
        const MediaPlaybackInfo info = self && self->m_media_player
            ? self->m_media_player->getPlaybackInfo() : MediaPlaybackInfo{};
        playback_video->stallcount = 0;
        // Do not turn a live/unknown-duration HLS stream into an ever-
        // growing finite movie.  UxPlay emits empty range arrays for 0.
        // At the end of a finite item UxPlay either holds it paused (when
        // the sender set actionAtItemEnd=pause) or shuts the session down.
        playback_video->ended = info.ended;
        playback_video->duration = info.ended && !info.duration_known ? -1.0
            : (info.duration_known ? info.duration : 0.0);
        // While opening, report the requested start position so an
        // AVPlayer sender's timeline does not jump back to zero.
        playback_video->position = info.position;
        playback_video->seek_start = info.seek_start;
        playback_video->seek_duration = info.seek_duration;
        playback_video->rate = info.rate;
        playback_video->ready_to_play = info.ready_to_play;
        playback_video->playback_buffer_empty = info.playback_buffer_empty;
        playback_video->playback_buffer_full = info.playback_buffer_full;
        playback_video->playback_likely_to_keep_up = info.playback_likely_to_keep_up;
        playback_video->num_loaded_time_ranges = 0;
        playback_video->num_seekable_time_ranges = 0;
        playback_video->loadedTimeRanges = nullptr;
        playback_video->seekableTimeRanges = nullptr;
    }

    static float on_video_playlist_remove(void* cls) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        return self && self->m_media_player
            ? self->m_media_player->pauseForPlaylistRemoval() : 0.0f;
    }

    // Required by UxPlay mirror thread.
    static void video_pause(void* cls) noexcept
    {
        UNUSED_PARAMETER(cls);
    }

    static void video_resume(void* cls) noexcept
    {
        UNUSED_PARAMETER(cls);
    }

    static void video_reset(void* cls, reset_type_t reset_type) noexcept
    {
        if (cls) {
            auto* self = static_cast<UxPlayIntegration*>(cls);
            blog(LOG_INFO, "[UxPlay] Video reset (type=%d) — flushing decoders", static_cast<int>(reset_type));
            if (self->m_media_player &&
                (reset_type == RESET_TYPE_HLS_SHUTDOWN ||
                 reset_type == RESET_TYPE_HLS_EOS ||
                 reset_type == RESET_TYPE_NOHOLD)) {
                self->m_media_player->stop();
                self->m_media_active.store(false);
                self->notifyStateChanged();
                self->cancelDeferredClear();
            }
            // iOS tears down its (idle) mirror stream right after an
            // AirPlay video item starts; that must not blank the item.
            self->processConnReset(!self->m_media_active.load());
        }
    }

    static int video_set_codec(void* cls, video_codec_t codec) noexcept
    {
        auto* self = static_cast<UxPlayIntegration*>(cls);
        const uint64_t now = os_gettime_ns();
        const uint64_t origin = self ? self->m_connection_started_ns.load() : 0;
        const double elapsed_ms = origin && now >= origin ? (now - origin) / 1e6 : 0.0;
        blog(LOG_INFO, "[CONNECT] +%.2fms codec configuration received (%s)",
             elapsed_ms, codec == VIDEO_CODEC_H265 ? "H265/HEVC" : "H264");
        // A new mirror stream starts here (it begins with its parameter
        // sets and a keyframe): start its decoder state fresh, so a
        // software fallback or statistics from an earlier stream do not
        // carry over. The picture on screen is left alone.
        if (self) {
            self->processConnReset(false, true);
        }
        return 0;
    }
};

raop_callbacks_t UxPlayIntegration::makeRaopCallbacks()
{
    raop_callbacks_t callbacks = {};
    callbacks.cls = this;
    callbacks.conn_init = &RaopCallbacks::conn_init;
    callbacks.conn_destroy = &RaopCallbacks::conn_destroy;
    callbacks.conn_teardown = &RaopCallbacks::conn_teardown;
    callbacks.conn_request_timing = &RaopCallbacks::conn_request_timing;
    callbacks.mirror_connection_timing = &RaopCallbacks::mirror_connection_timing;
    callbacks.video_process = &RaopCallbacks::video_process;
    callbacks.audio_get_format = &RaopCallbacks::audio_get_format;
    callbacks.audio_process = &RaopCallbacks::audio_process;
    callbacks.conn_feedback = &RaopCallbacks::conn_feedback;
    callbacks.conn_reset = &RaopCallbacks::conn_reset;
    callbacks.video_flush = &RaopCallbacks::video_flush;
    callbacks.audio_flush = &RaopCallbacks::audio_flush;
    callbacks.audio_set_volume = &RaopCallbacks::audio_set_volume;
    callbacks.audio_set_metadata = &RaopCallbacks::audio_set_metadata;
    callbacks.audio_set_coverart = &RaopCallbacks::audio_set_coverart;
    callbacks.audio_set_progress = &RaopCallbacks::audio_set_progress;
    callbacks.audio_set_client_volume = &RaopCallbacks::audio_set_client_volume;
    callbacks.on_video_play = &RaopCallbacks::on_video_play;
    callbacks.on_video_refused = &RaopCallbacks::on_video_refused;
    callbacks.on_video_scrub = &RaopCallbacks::on_video_scrub;
    callbacks.on_video_rate = &RaopCallbacks::on_video_rate;
    callbacks.on_video_stop = &RaopCallbacks::on_video_stop;
    callbacks.on_video_acquire_playback_info = &RaopCallbacks::on_video_acquire_playback_info;
    callbacks.on_video_playlist_remove = &RaopCallbacks::on_video_playlist_remove;
    callbacks.video_pause = &RaopCallbacks::video_pause;
    callbacks.video_resume = &RaopCallbacks::video_resume;
    callbacks.video_reset = &RaopCallbacks::video_reset;
    callbacks.video_set_codec = &RaopCallbacks::video_set_codec;
    return callbacks;
}

namespace {

// "AA:BB:CC:DD:EE:FF" (colons optional) to six bytes; false if malformed.
bool parse_device_id(const std::string& value, std::array<char, 6>& out_hw_addr)
{
    std::string compact;
    compact.reserve(12);
    for (char ch : value) {
        if (ch != ':') compact.push_back(ch);
    }
    if (compact.size() != 12) return false;
    for (char ch : compact) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    for (size_t i = 0; i < out_hw_addr.size(); ++i) {
        const std::string byte_str = compact.substr(i * 2, 2);
        out_hw_addr[i] = static_cast<char>(std::strtoul(byte_str.c_str(), nullptr, 16));
    }
    return true;
}

std::string hex_no_colon(const std::string& value)
{
    std::string compact;
    compact.reserve(12);
    for (char ch : value) {
        if (ch != ':') compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return compact;
}

// UxPlay's log lines into the OBS log.
void forward_uxplay_log(void*, int level, const char* msg)
{
    int obs_level;
    switch (level) {
    case 0: // LOGGER_EMERG
    case 1: // LOGGER_ALERT
    case 2: // LOGGER_CRIT
    case 3: // LOGGER_ERR
        obs_level = LOG_ERROR;
        break;
    case 4: // LOGGER_WARNING
        obs_level = LOG_WARNING;
        break;
    case 5: // LOGGER_NOTICE
    case 6: // LOGGER_INFO
        obs_level = LOG_INFO;
        break;
    default: // LOGGER_DEBUG, LOGGER_DEBUG_DATA
        obs_level = LOG_DEBUG;
        break;
    }
    // Strip trailing newline if present as blog() adds one
    std::string log_msg = msg;
    if (!log_msg.empty() && log_msg.back() == '\n') log_msg.pop_back();
    blog(obs_level, "[UxPlay] %s", log_msg.c_str());
}

// The AirPlay video rules the receiver applies to POST /play: the web ports
// allowed on other hosts (OBS_AIRPLAY_MEDIA_PORTS="80,443,..." overrides
// them) and the protocol trace.
void configure_media(raop_t* raop, bool accept_video, bool sender_only)
{
    if (const char* env = std::getenv("OBS_AIRPLAY_MEDIA_PORTS")) {
        const auto ports = media_url_policy::parsePortList(env);
        if (ports.empty()) {
            blog(LOG_WARNING, "[RECEIVER] Ignoring invalid OBS_AIRPLAY_MEDIA_PORTS=%s", env);
        } else {
            media_url_policy::setAllowedPorts(ports);
        }
    }
    const auto ports = media_url_policy::allowedPorts();
    raop_set_media_ports(raop, ports.data(), static_cast<int>(ports.size()));
    std::string list;
    for (unsigned short port : ports) list += (list.empty() ? "" : ",") + std::to_string(port);
    blog(LOG_INFO, "[RECEIVER] AirPlay video may fetch from other hosts on ports %s", list.c_str());
    raop_set_media_policy(raop, accept_video, sender_only);

    if (media_trace_requested()) {
        // Raise UxPlay's logger (default LOGGER_WARNING) so the [MEDIA]
        // INFO lines and the [MEDIA-TRACE] protocol dump reach the OBS log.
        raop_set_log_level(raop, LOGGER_INFO);
        raop_set_media_trace_level(raop, LOGGER_INFO);
        blog(LOG_INFO, "[RECEIVER] AirPlay media protocol trace enabled ([MEDIA-TRACE] lines)");
    }
}

} // namespace

bool UxPlayIntegration::start(const std::string& device_id_str, int port,
                              const std::string& server_name)
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
    blog(LOG_INFO, "[RECEIVER] === Starting UxPlay Integration ===");
    blog(LOG_DEBUG, "[RECEIVER] Requested port: %d", port);

    if (m_running) {
        blog(LOG_WARNING, "[RECEIVER] UxPlay integration already running");
        return true;
    }
    if (!m_clear_thread.joinable()) {
        // stop() joined the deferred-clear thread; restart it.
        {
            std::lock_guard<std::mutex> lock(m_clear_mutex);
            m_clear_exit = false;
            m_clear_pending = false;
        }
        m_clear_thread = std::thread(&UxPlayIntegration::deferredClearLoop, this);
    }

    // Initialize network first
    blog(LOG_DEBUG, "[RECEIVER] Initializing network...");
    int net_result = netutils_init();
    if (net_result != 0) {
        blog(LOG_ERROR, "[RECEIVER] Failed to initialize network, error code: %d", net_result);
        return false;
    }
    blog(LOG_DEBUG, "[RECEIVER] Network initialized successfully");

    // Whatever was set up is released on every way out but success.
    bool started = false;
    ScopeExit release{[&] {
        if (started) return;
        if (m_raop) raop_destroy(m_raop);
        if (m_dnssd) dnssd_destroy(m_dnssd);
        m_raop = nullptr;
        m_dnssd = nullptr;
    }};

    try {
        raop_callbacks_t callbacks = makeRaopCallbacks();

        // Initialize RAOP
        blog(LOG_DEBUG, "[RECEIVER] Initializing RAOP...");
        m_raop = raop_init(&callbacks);
        if (!m_raop) {
            blog(LOG_ERROR, "[RECEIVER] Failed to initialize RAOP");
            return false;
        }
        raop_set_log_callback(m_raop, forward_uxplay_log, this);
        blog(LOG_DEBUG, "[RECEIVER] RAOP initialized successfully");
        configure_media(m_raop, m_accept_video.load(), m_sender_only.load());

        // Initialize DNS-SD context needed by UxPlay's RAOP handlers (/info and TXT payloads)
        std::array<char, 6> hw_addr = {};
        std::string normalized_device_id = device_id_str;
        if (!parse_device_id(device_id_str, hw_addr)) {
            normalized_device_id = "AA:BB:CC:DD:EE:FF";
            parse_device_id(normalized_device_id, hw_addr);
            blog(LOG_WARNING, "[RECEIVER] Invalid device_id '%s'; using fallback '%s'",
                 device_id_str.c_str(), normalized_device_id.c_str());
        }

        int dnssd_error = 0;
        // The caller's name (the saved one, from obs_module_load); without
        // one, a name updateServerName() stored before start() ran.
        const std::string svc_name = !server_name.empty() ? server_name
                                     : (!m_server_name.empty() ? m_server_name : "OBS AirPlay");
        m_server_name = svc_name;     // canonical record
        m_hw_addr = hw_addr;          // store for later name updates
        m_dnssd = dnssd_init(svc_name.c_str(),
                             static_cast<int>(svc_name.size()),
                             hw_addr.data(),
                             static_cast<int>(hw_addr.size()),
                             &dnssd_error,
                             0);
        if (!m_dnssd || dnssd_error != DNSSD_ERROR_NOERROR) {
#if defined(_WIN32)
            if (dnssd_error == DNSSD_ERROR_LIBNOTFOUND) {
                blog(LOG_ERROR, "[DISCOVERY] AirPlay discovery is unavailable: this Windows has "
                                "neither Bonjour nor the DNS service discovery API (Windows 10 "
                                "1809 or later). Install Bonjour (it comes with Apple Devices or "
                                "iTunes) and restart OBS.");
            }
#endif
            blog(LOG_ERROR, "[DISCOVERY] Failed to initialize UxPlay dnssd context (error=%d)", dnssd_error);
            return false;
        }

        // Bonjour, or on Windows the Windows DNS service when Bonjour is
        // missing or stopped.
        blog(LOG_INFO, "[DISCOVERY] AirPlay discovery (DNS-SD) through %s", dnssd_get_backend(m_dnssd));

#if OBS_AIRPLAY_FAST_CONNECT
        // Match AirServer's Bonjour and connection profiles captured from the
        // same iPhone: AppleTV5,3 / 220.68 / 0x427FFFF7,0xE, then answer the
        // initial qualified GET /info with the full runtime profile. Direct
        // pair-verify remains a fallback for senders that still request it.
        raop_set_fast_connect(m_raop, true);
        apply_fast_connect_profile(m_dnssd);
        blog(LOG_INFO, "[DISCOVERY] Fast initial connection enabled (AirServer Bonjour + /info profile; direct-verify fallback)");
#else
        blog(LOG_INFO, "[RECEIVER] Fast initial connection disabled (UxPlay compatibility profile)");
#endif

        // Set port
        blog(LOG_DEBUG, "[RECEIVER] Setting RAOP port to %d", port);
        raop_set_port(m_raop, port);

        // Initialize RAOP with provided device ID and empty keyfile
        blog(LOG_DEBUG, "[RECEIVER] Calling raop_init2 with Device ID: %s", device_id_str.c_str());
        if (raop_init2(m_raop, 1, device_id_str.c_str(), "") != 0) {
            blog(LOG_ERROR, "[DISCOVERY] Failed to initialize RAOP with device ID");
            setLastError(obs_module_text("AirPlay.Error.SetupFailed"));
            return false;
        }
        blog(LOG_DEBUG, "[RECEIVER] raop_init2 completed successfully");
        raop_set_dnssd(m_raop, m_dnssd);

        // Safari does not use the type-110 screen-mirroring stream for a
        // website's Media AirPlay button. It opens the HTTP AirPlay channel
        // (/reverse, /play, /playback-info), which UxPlay disables by default.
        // Keep the advertised feature bits and the request handlers in sync.
        if (raop_set_plist(m_raop, "hls", 1) != 0) {
            blog(LOG_ERROR, "[RECEIVER] Failed to enable Safari Media AirPlay protocol mode");
            setLastError(obs_module_text("AirPlay.Error.SetupFailed"));
            return false;
        }
        dnssd_set_airplay_features(m_dnssd, 0, 1);
        dnssd_set_airplay_features(m_dnssd, 4, 1);
        blog(LOG_INFO, "[RECEIVER] Safari Media AirPlay enabled (HTTP /reverse + /play URL playback)");

        // Start the HTTP server, on the fallback port if the default one is
        // busy (macOS's own AirPlay Receiver uses 7000).
        blog(LOG_DEBUG, "[RECEIVER] Starting HTTP server...");
        const auto listen_on = [&](unsigned short wanted, unsigned short* actual) {
            *actual = wanted;
            const int result = raop_start_httpd(m_raop, actual);
            if (result < 0) {
                const int error_code = errno;
                blog(LOG_ERROR, "[RECEIVER] Failed to start RAOP HTTP server on port %d, error code: %d "
                                "(errno %d: %s)", wanted, result, error_code, strerror(error_code));
            }
            return result >= 0;
        };
        unsigned short actual_port = 0;
        if (!listen_on(static_cast<unsigned short>(port), &actual_port)) {
            if (port != kDefaultPort) {
                setLastError(format_text("AirPlay.Error.PortBusy", port));
                return false;
            }
            blog(LOG_INFO, "[RECEIVER] Trying alternative port %d...", kFallbackPort);
            if (!listen_on(static_cast<unsigned short>(kFallbackPort), &actual_port)) {
                setLastError(obs_module_text("AirPlay.Error.PortsBusy"));
                return false;
            }
        }
        blog(LOG_INFO, "[RECEIVER] UxPlay integration started successfully on port %d", actual_port);

        // Store the actual port for mDNS advertising
        m_actual_port = actual_port;
        raop_set_port(m_raop, m_actual_port);
        blog(LOG_DEBUG, "[RECEIVER] UxPlay integration running on actual port: %d", m_actual_port);

        // Register DNS-SD services so UxPlay can build correct TXT payloads for /info.
        int raop_register_error = dnssd_register_raop(m_dnssd, m_actual_port);
        if (raop_register_error != DNSSD_ERROR_NOERROR) {
            blog(LOG_WARNING, "[DISCOVERY] dnssd_register_raop returned %d", raop_register_error);
        }
        int airplay_register_error = dnssd_register_airplay(m_dnssd, m_actual_port);
        if (airplay_register_error != DNSSD_ERROR_NOERROR) {
            blog(LOG_WARNING, "[DISCOVERY] dnssd_register_airplay returned %d", airplay_register_error);
        }
        if (raop_register_error != DNSSD_ERROR_NOERROR ||
            airplay_register_error != DNSSD_ERROR_NOERROR) {
            setLastError(format_text("AirPlay.Error.Bonjour",
                                     raop_register_error ? raop_register_error
                                                         : airplay_register_error));
        } else {
            setLastError({});
        }
        m_registration_reported = false;
        blog(LOG_INFO, "[DISCOVERY] UxPlay device ID (RAOP): %s", hex_no_colon(normalized_device_id).c_str());

        m_running = true;
        started = true;
        return true;
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[RECEIVER] Exception in UxPlayIntegration::start: %s", e.what());
        return false;
    }
}

bool UxPlayIntegration::updateServerName(const std::string& name)
{
    // Not while start() or stop() replace m_raop / m_dnssd.
    std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
    // Hold m_mutex only for the running/raop check; release before calling
    // into UxPlay/DNS-SD to avoid deadlock with audio/video callback threads.
    raop_t* raop_ref = nullptr;
    dnssd_t* old_dnssd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_raop || !m_running) {
            // UxPlay not (yet) running: keep the name for start().
            m_server_name = name.empty() ? "OBS AirPlay" : name;
            return true;
        }
        raop_ref = m_raop;
        old_dnssd = m_dnssd;
        m_dnssd   = nullptr; // Claimed — we own it now.
    }

    const std::string svc_name = name.empty() ? "OBS AirPlay" : name;
    blog(LOG_INFO, "[DISCOVERY] Updating UxPlay dnssd service name to: %s", svc_name.c_str());

    // Create the new context before touching the old one: UxPlay's request
    // handlers keep using the old context (and its advertisement stays up)
    // if this fails.
    int dnssd_error = 0;
    dnssd_t* new_dnssd = dnssd_init(svc_name.c_str(),
                                    static_cast<int>(svc_name.size()),
                                    m_hw_addr.data(),
                                    static_cast<int>(m_hw_addr.size()),
                                    &dnssd_error,
                                    0);
    if (!new_dnssd || dnssd_error != DNSSD_ERROR_NOERROR) {
        blog(LOG_WARNING, "[DISCOVERY] Failed to reinitialize dnssd for name update (error=%d)", dnssd_error);
        if (new_dnssd) {
            dnssd_destroy(new_dnssd);
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_dnssd = old_dnssd;
        }
        setLastError(format_text("AirPlay.Error.RenameFailed", dnssd_error));
        return false;
    }

#if OBS_AIRPLAY_FAST_CONNECT
    apply_fast_connect_profile(new_dnssd);
#endif

    // raop_set_dnssd must be called before dnssd_register_raop/airplay because
    // it sets dnssd->pk (the public key string) via dnssd_set_pk(). Without it,
    // dnssd->pk is NULL and dnssd_register_raop crashes in strlen(dnssd->pk).
    // It also comes before unregistering the old context, so GET /info on the
    // HTTP thread stops reading the old TXT records before they are freed.
    raop_set_dnssd(raop_ref, new_dnssd);

    // Unregister the old service before registering the new one so iOS sees
    // it disappear, which forces a fresh mDNS discovery when the new name is
    // announced.  If we register the new name before removing the old one,
    // iOS caches the old entry and ignores the new advertisement until the
    // next connection.
    if (old_dnssd) {
        dnssd_unregister_raop(old_dnssd);
        dnssd_unregister_airplay(old_dnssd);
    }

    // A request handler on UxPlay's HTTP thread may still be reading the old
    // context, so it is not freed now: it is kept until the next name change
    // (or stop), long after any such handler has returned.
    dnssd_t* retired = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        retired = m_retired_dnssd;
        m_retired_dnssd = old_dnssd;
    }
    if (retired) {
        dnssd_destroy(retired);
    }

    int raop_reg_err = dnssd_register_raop(new_dnssd, m_actual_port);
    if (raop_reg_err != DNSSD_ERROR_NOERROR) {
        blog(LOG_WARNING, "[DISCOVERY] dnssd_register_raop returned %d for updated context", raop_reg_err);
    }
    int airplay_reg_err = dnssd_register_airplay(new_dnssd, m_actual_port);
    if (airplay_reg_err != DNSSD_ERROR_NOERROR) {
        blog(LOG_WARNING, "[DISCOVERY] dnssd_register_airplay returned %d for updated context", airplay_reg_err);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dnssd       = new_dnssd;
        m_server_name = svc_name;
    }
    if (raop_reg_err != DNSSD_ERROR_NOERROR || airplay_reg_err != DNSSD_ERROR_NOERROR) {
        setLastError(format_text("AirPlay.Error.Bonjour",
                                 raop_reg_err ? raop_reg_err : airplay_reg_err));
    } else {
        setLastError({});
    }
    m_registration_reported = false;

    blog(LOG_INFO, "[DISCOVERY] UxPlay dnssd service name updated successfully");
    return true;
}

#if defined(_WIN32)
void UxPlayIntegration::checkRegistration()
{
    // Not while start(), stop() or a rename replace m_dnssd; never waits.
    std::unique_lock<std::mutex> lifecycle(m_lifecycle_mutex, std::try_to_lock);
    if (!lifecycle.owns_lock() || m_registration_reported || !m_dnssd) return;
    const int status = dnssd_get_registration_status(m_dnssd);
    if (status == DNSSD_REGISTRATION_PENDING) return;
    m_registration_reported = true;
    if (status == 0) {
        blog(LOG_INFO, "[DISCOVERY] the receiver is announced on the network (%s)",
             dnssd_get_backend(m_dnssd));
        return;
    }
    blog(LOG_WARNING, "[DISCOVERY] %s could not announce the receiver (error %d)",
         dnssd_get_backend(m_dnssd), status);
    setLastError(format_text("AirPlay.Error.WindowsDiscovery", status));
}
#endif

void UxPlayIntegration::setLastError(std::string error)
{
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_last_error = std::move(error);
    }
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        callback = m_state_changed;
    }
    if (callback) callback();
}

std::string UxPlayIntegration::lastError() const
{
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_last_error;
}

void UxPlayIntegration::setVideoPolicy(bool accept_video, bool sender_only)
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
    const bool accept_changed = m_accept_video.exchange(accept_video) != accept_video;
    const bool sender_only_changed = m_sender_only.exchange(sender_only) != sender_only;
    const bool changed = accept_changed || sender_only_changed;
    if (m_raop) raop_set_media_policy(m_raop, accept_video, sender_only);
    if (changed) {
        blog(LOG_INFO, "[AirPlay] AirPlay video %s%s", accept_video ? "accepted" : "turned off",
             accept_video && sender_only ? ", only from the sending device" : "");
    }
}

void UxPlayIntegration::recordRefusal(const std::string& reason)
{
    char when[16] = "";
    const time_t now = time(nullptr);
    struct tm local = {};
#if defined(_WIN32)
    const bool have_time = localtime_s(&local, &now) == 0;
#else
    const bool have_time = localtime_r(&now, &local) != nullptr;
#endif
    if (have_time) strftime(when, sizeof(when), "%H:%M:%S", &local);
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_last_refusal = std::string(when) + ": " + reason;
        callback = m_state_changed;
    }
    if (callback) callback();
}

void UxPlayIntegration::clearRefusal()
{
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        if (m_last_refusal.empty()) return;
        m_last_refusal.clear();
        callback = m_state_changed;
    }
    if (callback) callback();
}

std::string UxPlayIntegration::lastRefusal() const
{
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_last_refusal;
}

std::string UxPlayIntegration::connectionState() const
{
    if (m_media_active.load()) return obs_module_text("AirPlay.State.Video");
    if (m_open_connections.load() > 0) {
        return obs_module_text(m_frame_shown.load() ? "AirPlay.State.Mirroring"
                                                    : "AirPlay.State.Connected");
    }
    return obs_module_text("AirPlay.State.Waiting");
}

void UxPlayIntegration::setStateChangedCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_status_mutex);
    m_state_changed = std::move(callback);
}

// Calls the state-changed callback only when the summary actually changed
// (iOS opens a connection for every GET /info while its menu is open).
void UxPlayIntegration::notifyStateChanged()
{
    const std::string state = connectionState();
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        if (state == m_last_state) return;
        m_last_state = state;
        callback = m_state_changed;
    }
    if (callback) callback();
}

void UxPlayIntegration::stop()
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_running = false;
    }
    
    blog(LOG_INFO, "[RECEIVER] Stopping UxPlay integration");

    // raop_destroy/dnssd_destroy are called outside m_mutex to avoid deadlock:
    // UxPlay's audio/video threads may be blocked waiting for m_mutex (in
    // processVideoData/processAudioData), and raop_destroy joins those threads.
    // The HTTP thread goes first: it is the one that starts and stops URL
    // playback (POST /play), so the media player is stopped only once no
    // request can start it again.
    if (m_raop) {
        raop_stop_httpd(m_raop);
    }
    // The deferred-clear thread also stops the player; join it first.
    stopDeferredClearThread();
    if (m_media_player) {
        m_media_player->stop();
    }
    if (m_raop) {
        raop_destroy(m_raop);
        m_raop = nullptr;
    }

    if (m_dnssd) {
        dnssd_unregister_raop(m_dnssd);
        dnssd_unregister_airplay(m_dnssd);
        dnssd_destroy(m_dnssd);
        m_dnssd = nullptr;
    }
    if (m_retired_dnssd) {
        dnssd_destroy(m_retired_dnssd);
        m_retired_dnssd = nullptr;
    }
    
    blog(LOG_INFO, "[RECEIVER] UxPlay integration stopped");
}

void UxPlayIntegration::setConnectionResetCallback(ConnectionResetCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_reset_callback = callback;
}

void UxPlayIntegration::processConnReset(bool clear_output, bool flush_decoders)
{
    if (flush_decoders) {
        m_stream_nal_logged.store(false);
    }
    ConnectionResetCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_reset_callback;
    }
    if (callback) {
        callback(clear_output, flush_decoders);
    }
}

void UxPlayIntegration::setVideoCallback(VideoFrameCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_video_callback = callback;
}

void UxPlayIntegration::setAudioCallback(AudioDataCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_audio_callback = callback;
}

void UxPlayIntegration::setAudioFormatCallback(AudioFormatCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_audio_format_callback = std::move(callback);
}

void UxPlayIntegration::setMediaVideoCallback(MediaVideoCallback callback)
{
    if (m_media_player) {
        m_media_player->setVideoCallback(std::move(callback));
    }
}

void UxPlayIntegration::setMediaAudioCallback(MediaAudioCallback callback)
{
    if (m_media_player) {
        m_media_player->setAudioCallback(std::move(callback));
    }
}

void UxPlayIntegration::processVideoData(video_decode_struct* data)
{
    if (!data || !data->data || data->data_len <= 0) {
        return;
    }

    boost_caller_thread_qos_once("video");

    // Log the first video packet to measure iOS screen-capture startup latency.
    if (!m_first_video_logged.exchange(true)) {
        const uint64_t now = os_gettime_ns();
        const uint64_t origin = m_connection_started_ns.load();
        const double elapsed_ms = origin && now >= origin ? (now - origin) / 1e6 : 0.0;
        blog(LOG_INFO, "[CONNECT] +%.2fms first encoded video packet (%d bytes, H265=%s)",
             elapsed_ms, data->data_len, data->is_h265 ? "yes" : "no");
    }

    VideoFrameCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_video_callback;
    }

    if (!callback) {
        return;
    }

    // Once per stream (not per frame): how many NAL units a payload holds,
    // which tells whether slice threading could help the software decoder.
    if (!m_stream_nal_logged.exchange(true)) {
        blog(LOG_INFO, "[MIRROR] stream: %s, %d NAL units in the first payload",
             data->is_h265 ? "HEVC" : "H.264", data->nal_count);
    }
    callback(data->data,
             static_cast<size_t>(data->data_len),
             data->ntp_time_local,
             data->is_h265);
}

void UxPlayIntegration::processAudioData(audio_decode_struct* data)
{
    if (!data || !data->data || data->data_len <= 0) {
        return;
    }

    boost_caller_thread_qos_once("audio");

    AudioDataCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_audio_callback;
    }

    if (!callback) {
        return;
    }

    callback(data->data,
             static_cast<size_t>(data->data_len),
             data->ct,
             data->ntp_time_local);
}
