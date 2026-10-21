// SPDX-License-Identifier: GPL-3.0-or-later
#include "media-player.hpp"
#include "hardware-decode-option.hpp"
#include "hls-playlist.hpp"
#include "media-fetch.hpp"
#include "media-url-policy.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

namespace {

std::atomic<bool> g_allow_insecure_tls{false};
std::mutex g_refusal_mutex; // leaf
MediaRefusalCallback g_refusal_callback;

// A URL the policy refused: already logged by the caller.
void report_refusal(const std::string& reason)
{
    MediaRefusalCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_refusal_mutex);
        callback = g_refusal_callback;
    }
    if (callback) callback(reason);
}

std::string ffmpeg_error(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

// Options for every HTTP(S) fetch: web protocols only and certificate
// verification unless the user allowed self-signed servers. Redirects are
// followed by media_fetch::Session, not FFmpeg.
void set_tls_options(AVDictionary** options)
{
    media_fetch::setTlsOptions(options, g_allow_insecure_tls.load(std::memory_order_relaxed));
}

void set_network_options(AVDictionary** options)
{
    av_dict_set(options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
    set_tls_options(options);
}

std::string safe_location_for_log(std::string location)
{
    return media_fetch::safeForLog(std::move(location));
}

// See hls::kTargetBitrate: the manifest preflight and FFmpeg's program
// selection pick the same rendition.
constexpr int64_t kHlsTargetBitrate = hls::kTargetBitrate;

// Probe limits for HLS: consulted while the demuxer opens its first
// segment and again by find_stream_info, so startup does not download
// several whole segments to learn what the playlist already says.
void apply_hls_probe_limits(AVFormatContext* format)
{
    format->probesize = 384 * 1024;
    format->max_analyze_duration = 2 * AV_TIME_BASE;
    format->max_probe_packets = 256;
    format->skip_estimate_duration_from_pts = 1;
}

bool is_hls_mime_type(const std::string& type)
{
    std::string lower;
    for (char c : type) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower.find("mpegurl") != std::string::npos;  // application/vnd.apple.mpegurl, audio/mpegurl
}

// Positions are relative to the stream's start; av_seek_frame takes the
// container's absolute timestamps (MPEG-TS and HLS rarely start at 0).
double stream_start_seconds(const AVFormatContext* format)
{
    return format && format->start_time != AV_NOPTS_VALUE
        ? format->start_time / static_cast<double>(AV_TIME_BASE) : 0.0;
}

// Sender positions are finite but otherwise unchecked (an item of unknown
// duration has nothing to clamp them to): about 115 days at most, so they
// convert to int64_t microseconds.
constexpr double kMaxPositionSeconds = 1e7;

double clamp_position(double seconds)
{
    if (!std::isfinite(seconds)) return 0.0;
    return std::min(std::max(0.0, seconds), kMaxPositionSeconds);
}

// av_seek_frame's timestamp for a position (both clamped first).
int64_t seek_timestamp(double position, double start_seconds)
{
    return std::llrint((clamp_position(position) + clamp_position(start_seconds)) * AV_TIME_BASE);
}

int64_t variant_bitrate(const AVFormatContext* format, const AVProgram* program)
{
    if (!format || !program) {
        return 0;
    }
    const AVDictionaryEntry* entry = av_dict_get(program->metadata, "variant_bitrate", nullptr, 0);
    if (entry && entry->value) {
        return std::strtoll(entry->value, nullptr, 10);
    }
    for (unsigned int i = 0; i < program->nb_stream_indexes; ++i) {
        const unsigned int index = program->stream_index[i];
        if (index >= format->nb_streams) {
            continue;
        }
        entry = av_dict_get(format->streams[index]->metadata, "variant_bitrate", nullptr, 0);
        if (entry && entry->value) {
            return std::strtoll(entry->value, nullptr, 10);
        }
    }
    return 0;
}

bool program_has_video(const AVFormatContext* format, const AVProgram* program)
{
    if (!format || !program) {
        return false;
    }
    bool stream_types_known = false;
    for (unsigned int i = 0; i < program->nb_stream_indexes; ++i) {
        const unsigned int index = program->stream_index[i];
        if (index >= format->nb_streams) {
            continue;
        }
        const AVMediaType type = format->streams[index]->codecpar->codec_type;
        stream_types_known |= type != AVMEDIA_TYPE_UNKNOWN;
        if (type == AVMEDIA_TYPE_VIDEO) {
            return true;
        }
    }
    // Some manifests omit CODECS, so stream types are unavailable until
    // probing. Do not accidentally discard every candidate in that case.
    return !stream_types_known;
}

AVProgram* select_hls_program(AVFormatContext* format, int64_t* selected_bitrate)
{
    if (selected_bitrate) {
        *selected_bitrate = 0;
    }
    if (!format || format->nb_programs <= 1 || !format->iformat ||
        !format->iformat->name || !std::strstr(format->iformat->name, "hls")) {
        return nullptr;
    }

    // FFmpeg otherwise probes the first segments of every rendition in a
    // multivariant playlist. Pick one rendition before stream-info analysis,
    // by the same rule as the manifest preflight (hls::chooseVariant).
    constexpr int64_t kTargetBitrate = kHlsTargetBitrate;
    AVProgram* best_under_target = nullptr;
    AVProgram* lowest_over_target = nullptr;
    int64_t best_under_bitrate = -1;
    int64_t lowest_over_bitrate = INT64_MAX;
    AVProgram* fallback = nullptr;

    for (unsigned int i = 0; i < format->nb_programs; ++i) {
        AVProgram* program = format->programs[i];
        if (!program || !program_has_video(format, program)) {
            continue;
        }
        if (!fallback) {
            fallback = program;
        }
        const int64_t bitrate = variant_bitrate(format, program);
        if (bitrate > 0 && bitrate <= kTargetBitrate && bitrate > best_under_bitrate) {
            best_under_target = program;
            best_under_bitrate = bitrate;
        } else if (bitrate > kTargetBitrate && bitrate < lowest_over_bitrate) {
            lowest_over_target = program;
            lowest_over_bitrate = bitrate;
        }
    }

    AVProgram* selected = best_under_target ? best_under_target
        : (lowest_over_target ? lowest_over_target : fallback);
    if (!selected) {
        return nullptr;
    }

    for (unsigned int i = 0; i < format->nb_programs; ++i) {
        format->programs[i]->discard = format->programs[i] == selected
            ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    for (unsigned int i = 0; i < format->nb_streams; ++i) {
        format->streams[i]->discard = AVDISCARD_ALL;
    }
    for (unsigned int i = 0; i < selected->nb_stream_indexes; ++i) {
        const unsigned int index = selected->stream_index[i];
        if (index < format->nb_streams) {
            format->streams[index]->discard = AVDISCARD_DEFAULT;
        }
    }

    if (selected_bitrate) {
        *selected_bitrate = variant_bitrate(format, selected);
    }
    return selected;
}

int find_program_stream(const AVFormatContext* format, const AVProgram* program,
                        AVMediaType type)
{
    if (!format || !program) {
        return -1;
    }
    for (unsigned int i = 0; i < program->nb_stream_indexes; ++i) {
        const unsigned int index = program->stream_index[i];
        if (index < format->nb_streams &&
            format->streams[index]->codecpar->codec_type == type) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool starts_with_case_insensitive(const std::string& value, const char* prefix)
{
    const size_t length = std::strlen(prefix);
    if (value.size() < length) return false;
    for (size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool is_http_url(const std::string& value)
{
    return starts_with_case_insensitive(value, "http://") ||
           starts_with_case_insensitive(value, "https://");
}

bool looks_like_hls_url(const std::string& value)
{
    const size_t suffix = value.find(".m3u8");
    return suffix != std::string::npos &&
        (suffix + 5 == value.size() || value[suffix + 5] == '?' || value[suffix + 5] == '#');
}

double timestamp_seconds(const AVFrame* frame, const AVStream* stream,
                         double format_start_seconds)
{
    if (!frame || !stream || frame->best_effort_timestamp == AV_NOPTS_VALUE) {
        return -1.0;
    }
    const double absolute = frame->best_effort_timestamp * av_q2d(stream->time_base);
    return std::max(0.0, absolute - format_start_seconds);
}

// Clockwise quarter turns needed to show a stream upright. Phones store a
// portrait recording as a landscape picture plus a display matrix; the
// decoder does not apply it.
int display_quarter_turns(const AVStream* stream)
{
    if (!stream || !stream->codecpar) {
        return 0;
    }
    const AVPacketSideData* side = av_packet_side_data_get(
        stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (!side || side->size < 9 * sizeof(int32_t)) {
        return 0;
    }
    // Counter-clockwise degrees the picture is rotated by for display.
    const double angle = av_display_rotation_get(reinterpret_cast<const int32_t*>(side->data));
    if (std::isnan(angle)) {
        return 0;
    }
    const int turns = static_cast<int>(std::lround(-angle / 90.0));
    return ((turns % 4) + 4) % 4;
}

// Rotates one plane by clockwise quarter turns. `pixel` is the bytes per
// sample group: 1, or 2 for NV12's interleaved chroma. After one or three
// turns the destination is `height` wide and `width` tall.
void rotate_plane(const uint8_t* src, int src_stride, int width, int height, int pixel,
                  int turns, uint8_t* dst, int dst_stride)
{
    if (turns == 2) {
        for (int y = 0; y < height; ++y) {
            const uint8_t* in = src + static_cast<ptrdiff_t>(height - 1 - y) * src_stride +
                                static_cast<ptrdiff_t>(width - 1) * pixel;
            uint8_t* out = dst + static_cast<ptrdiff_t>(y) * dst_stride;
            for (int x = 0; x < width; ++x, in -= pixel, out += pixel) {
                out[0] = in[0];
                if (pixel == 2) out[1] = in[1];
            }
        }
        return;
    }
    // One turn: destination (x, y) is source column y, row height-1-x.
    // Three turns: source column width-1-y, row x.
    // Worked through in bands of destination rows, so that the source is
    // read along its rows and only a few destination rows are open at a
    // time (a plain column walk misses the cache on every sample of a 4K
    // picture).
    constexpr int kBand = 16;
    for (int band = 0; band < width; band += kBand) {
        const int band_end = std::min(width, band + kBand);
        for (int x = 0; x < height; ++x) {
            const int row = turns == 1 ? height - 1 - x : x;
            const uint8_t* in_row = src + static_cast<ptrdiff_t>(row) * src_stride;
            for (int y = band; y < band_end; ++y) {
                const int column = turns == 1 ? y : width - 1 - y;
                const uint8_t* in = in_row + static_cast<ptrdiff_t>(column) * pixel;
                uint8_t* out = dst + static_cast<ptrdiff_t>(y) * dst_stride +
                               static_cast<ptrdiff_t>(x) * pixel;
                out[0] = in[0];
                if (pixel == 2) out[1] = in[1];
            }
        }
    }
}

// Byte-exact input for progressive HTTP media (MP4/MOV).  Sender media
// servers (iOS AVPlayer's among them) end responses early or close kept-alive
// connections; FFmpeg's HTTP reader then reports end of file part-way through
// a sample, and the demuxer silently drops it ("partial file").  This layer
// tracks the exact byte position and, whenever a read stops before the known
// file size, opens a fresh request at that offset, so the demuxer only ever
// sees complete data.
class ResilientHttpIo {
public:
    ResilientHttpIo(std::string url, AVIOInterruptCB interrupt, media_fetch::Session& fetch)
        : m_url(std::move(url)), m_interrupt(interrupt), m_fetch(fetch)
    {
    }

    ~ResilientHttpIo()
    {
        if (m_context) {
            av_freep(&m_context->buffer);
            avio_context_free(&m_context);
        }
        if (m_inner) avio_closep(&m_inner);
    }

    ResilientHttpIo(const ResilientHttpIo&) = delete;
    ResilientHttpIo& operator=(const ResilientHttpIo&) = delete;

    // True when the server reports a size; otherwise the caller falls back
    // to FFmpeg's own HTTP input (live or unsized content).
    bool open()
    {
        const Reopen opened = reopen(0);
        m_refused = opened == Reopen::Refused;
        if (opened != Reopen::Opened || m_size <= 0) return false;
        constexpr int kBufferSize = 256 * 1024;
        auto* buffer = static_cast<unsigned char*>(av_malloc(kBufferSize));
        if (!buffer) return false;
        m_context = avio_alloc_context(buffer, kBufferSize, 0, this, &ResilientHttpIo::read,
                                       nullptr, &ResilientHttpIo::seek);
        if (!m_context) {
            av_free(buffer);
            return false;
        }
        m_context->seekable = AVIO_SEEKABLE_NORMAL;
        m_reopens = 0;
        return true;
    }

    AVIOContext* context() const { return m_context; }
    int64_t size() const { return m_size; }
    // The server's Content-Type, as FFmpeg's HTTP input reports it.
    std::string mimeType() const
    {
        uint8_t* type = nullptr;
        std::string result;
        if (m_inner && av_opt_get(m_inner, "mime_type", AV_OPT_SEARCH_CHILDREN, &type) >= 0 && type) {
            result = reinterpret_cast<char*>(type);
        }
        av_free(type);
        return result;
    }
    int reopens() const { return m_reopens; }
    // open() failed because the policy refused the URL or a redirect
    // (already reported): no other input may try it again.
    bool refused() const { return m_refused; }

private:
    static constexpr int kMaxAttempts = 5;
    static constexpr int64_t kForwardSkipBytes = 256 * 1024;

    bool interrupted() const
    {
        return m_interrupt.callback && m_interrupt.callback(m_interrupt.opaque);
    }

    // Waits before another attempt (50 ms more each time) in 10 ms steps,
    // so stop() does not wait for the backoff. False when interrupted.
    bool backOff(int attempt) const
    {
        for (int waited = 0; waited < 50 * (attempt + 1); waited += 10) {
            if (interrupted()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return !interrupted();
    }

    // Refused: the policy refused the URL or a redirect; retrying cannot help.
    enum class Reopen { Opened, Failed, Refused };

    Reopen reopen(int64_t offset)
    {
        if (m_inner) avio_closep(&m_inner);
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", "10000000", 0);
        av_dict_set(&options, "user_agent", "AppleCoreMedia/1.0 OBS-AirPlay/" PLUGIN_VERSION, 0);
        set_network_options(&options);
        av_dict_set_int(&options, "offset", offset, 0);
        AVIOInterruptCB interrupt = m_interrupt;
        std::string final_url;
        const int result = m_fetch.open(
            m_url, false, &options,
            [&](const std::string& url, AVDictionary** used) {
                return avio_open2(&m_inner, url.c_str(), AVIO_FLAG_READ, &interrupt, used);
            },
            &final_url);
        av_dict_free(&options);
        if (result < 0) {
            m_inner = nullptr;
            return result == AVERROR(EPERM) ? Reopen::Refused : Reopen::Failed;
        }
        // Later range requests go straight to where the redirects led.
        if (!final_url.empty()) m_url = final_url;
        if (m_size <= 0) {
            const int64_t reported = avio_size(m_inner);
            // With an offset the server reports the whole file size.
            m_size = reported > 0 ? reported : -1;
        }
        m_position = offset;
        return Reopen::Opened;
    }

    static int read(void* opaque, uint8_t* buffer, int wanted)
    {
        auto* self = static_cast<ResilientHttpIo*>(opaque);
        if (self->m_size > 0 && self->m_position >= self->m_size) return AVERROR_EOF;
        int last = AVERROR_EOF;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (self->interrupted()) return AVERROR_EXIT;
            if (self->m_inner) {
                const int got = avio_read_partial(self->m_inner, buffer, wanted);
                if (got > 0) {
                    self->m_position += got;
                    return got;
                }
                last = got < 0 ? got : AVERROR_EOF;
            }
            if (self->m_size <= 0 || self->m_position >= self->m_size) return last;
            // Stopped short of the known size: continue on a fresh request.
            ++self->m_reopens;
            const Reopen reopened = self->reopen(self->m_position);
            if (reopened == Reopen::Refused) return AVERROR(EPERM);
            if (reopened == Reopen::Failed && !self->backOff(attempt)) {
                return AVERROR_EXIT;
            }
        }
        return last == AVERROR_EOF ? AVERROR(EIO) : last;
    }

    static int64_t seek(void* opaque, int64_t offset, int whence)
    {
        auto* self = static_cast<ResilientHttpIo*>(opaque);
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) return self->m_size > 0 ? self->m_size : AVERROR(ENOSYS);
        int64_t target = -1;
        if (whence == SEEK_SET) target = offset;
        else if (whence == SEEK_CUR) target = self->m_position + offset;
        else if (whence == SEEK_END && self->m_size > 0) target = self->m_size + offset;
        if (target < 0) return AVERROR(EINVAL);
        if (target == self->m_position && self->m_inner) return target;
        // Short forward skips read through the current response.
        if (self->m_inner && target > self->m_position &&
            target - self->m_position <= kForwardSkipBytes) {
            uint8_t scratch[16384];
            while (self->m_position < target) {
                const int chunk = static_cast<int>(std::min<int64_t>(
                    sizeof(scratch), target - self->m_position));
                if (read(self, scratch, chunk) <= 0) break;
            }
            if (self->m_position == target) return target;
        }
        if (target >= self->m_size && self->m_size > 0) {
            // Seeking to the end needs no request; reads report EOF.
            if (self->m_inner) avio_closep(&self->m_inner);
            self->m_position = target;
            return target;
        }
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (self->interrupted()) return AVERROR_EXIT;
            const Reopen reopened = self->reopen(target);
            if (reopened == Reopen::Opened) return target;
            if (reopened == Reopen::Refused) return AVERROR(EPERM);
            if (!self->backOff(attempt)) return AVERROR_EXIT;
        }
        return AVERROR(EIO);
    }

    std::string m_url;
    AVIOInterruptCB m_interrupt;
    media_fetch::Session& m_fetch;
    AVIOContext* m_inner = nullptr;
    AVIOContext* m_context = nullptr;
    int64_t m_position = 0;
    int64_t m_size = -1;
    int m_reopens = 0;
    bool m_refused = false;
};

} // namespace

namespace {
std::atomic<bool> g_follow_sender_clock{false};
std::atomic<int> g_open_delay_for_testing_ms{0};
std::atomic<int> g_seek_worker_delay_for_testing_ms{0};
std::atomic<int> g_seek_presenter_delay_for_testing_ms{0};
}

// Runs a cleanup when the scope ends (early returns included).
struct ScopeExit {
    std::function<void()> cleanup;
    ~ScopeExit()
    {
        if (cleanup) cleanup();
    }
};

// Planes of a picture to hand on: an AVFrame's, or a locked pixel buffer's.
struct PictureView {
    const uint8_t* data[4] = {};
    int linesize[4] = {};
    int width = 0;
    int height = 0;
    int format = AV_PIX_FMT_NONE;

    static PictureView of(const AVFrame* frame)
    {
        PictureView view;
        for (int i = 0; i < 4; ++i) {
            view.data[i] = frame->data[i];
            view.linesize[i] = frame->linesize[i];
        }
        view.width = frame->width;
        view.height = frame->height;
        view.format = frame->format;
        return view;
    }
};

// Proof that the caller holds a lock. A function that must run with a mutex
// held takes one of these, made from the caller's lock, so it cannot be
// called without one in scope (CODE-4). Which mutex is named by the
// function's parameter name.
class HeldLock {
public:
    explicit HeldLock(const std::lock_guard<std::mutex>&) {}
    explicit HeldLock(const std::unique_lock<std::mutex>& lock) {
        assert(lock.owns_lock());
        (void)lock;
    }
};

// Read access to a VideoToolbox frame's 8-bit NV12 planes in place, for as
// long as this object lives (the decoded AVFrame keeps the buffer itself).
class LockedPixelBuffer {
public:
    LockedPixelBuffer() = default;
    LockedPixelBuffer(const LockedPixelBuffer&) = delete;
    LockedPixelBuffer& operator=(const LockedPixelBuffer&) = delete;
    ~LockedPixelBuffer() { unlock(); }

    bool lock(const AVFrame* frame, PictureView* view)
    {
#if defined(__APPLE__)
        auto buffer = static_cast<CVPixelBufferRef>(static_cast<void*>(frame->data[3]));
        if (!buffer) return false;
        const OSType type = CVPixelBufferGetPixelFormatType(buffer);
        if ((type != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
             type != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
            CVPixelBufferGetPlaneCount(buffer) != 2 ||
            CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            return false;
        }
        m_buffer = buffer;
        PictureView locked;
        locked.width = std::min<int>(frame->width,
                                     static_cast<int>(CVPixelBufferGetWidthOfPlane(buffer, 0)));
        locked.height = std::min<int>(frame->height,
                                      static_cast<int>(CVPixelBufferGetHeightOfPlane(buffer, 0)));
        locked.format = AV_PIX_FMT_NV12;
        for (size_t plane = 0; plane < 2; ++plane) {
            locked.data[plane] =
                static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
            locked.linesize[plane] =
                static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(buffer, plane));
        }
        if (!locked.data[0] || !locked.data[1]) {
            unlock();
            return false;
        }
        *view = locked;
        return true;
#else
        (void)frame;
        (void)view;
        return false;
#endif
    }

    void unlock()
    {
#if defined(__APPLE__)
        if (m_buffer) {
            CVPixelBufferUnlockBaseAddress(m_buffer, kCVPixelBufferLock_ReadOnly);
            m_buffer = nullptr;
        }
#endif
    }

    bool held() const { return m_buffer != nullptr; }

private:
#if defined(__APPLE__)
    CVPixelBufferRef m_buffer = nullptr;
#else
    void* m_buffer = nullptr;
#endif
};

class MediaPlayer::Impl {
public:
    ~Impl()
    {
        stop();
    }

    void setVideoCallback(MediaVideoCallback callback)
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        video_callback = std::move(callback);
    }

    void setAudioCallback(MediaAudioCallback callback)
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        audio_callback = std::move(callback);
    }

    void play(const std::string& location, double start_position, float initial_rate = 1.0f)
    {
        // control_mutex serialises play() and stop(), which run on the HTTP
        // thread, the UxPlay mirror thread, the deferred-clear thread and the
        // OBS main thread; `worker` is only touched under it.
        std::lock_guard<std::mutex> control(control_mutex);
        stopLocked(HeldLock(control));
        // The start position comes from the sender: +inf survives max(0, x)
        // and converting a huge one to int64_t is undefined.
        start_position = clamp_position(start_position);
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            info = {};
            current_location = location;
            // Scrub-burst detection compares against the previous item's seeks.
            seek_generation = 0;
            last_seek_at = {};
            coarse_preview_after = {};
            info.position = start_position;
            info.rate = initial_rate;
            anchor_valid = initial_rate > 0.0f;
            anchor_position = info.position;
            anchor_wall_time = std::chrono::steady_clock::now();
            catching_up_video = catching_up_audio = true;
            skipped_video_frames = 0;
            seek_pending = false;
            seek_target = info.position;
            // The demuxer lands on the keyframe before the start position;
            // decode but do not present the frames in between.
            discard_video_before = discard_audio_before =
                info.position > 0.0 ? info.position : -1.0;
            // An item that starts paused (e.g. a still clip) must still show
            // its first frame.
            preview_pending = true;
            resync_on_resume = false;
            last_seek_target = -1.0;
            scrub_burst = false;
            coarse_preview_shown = false;
            clock_initialized = false;
        }
        stop_requested.store(false, std::memory_order_release);
        // Reached from UxPlay's C HTTP thread: nothing may throw out of here.
        try {
            worker = std::thread(&Impl::run, this, location, start_position);
        } catch (const std::exception& e) {
            stop_requested.store(true, std::memory_order_release);
            markOpenFailure((std::string("could not start the media thread: ") + e.what()).c_str());
        }
    }

    void stop()
    {
        std::lock_guard<std::mutex> control(control_mutex);
        stopLocked(HeldLock(control));
    }

    void stopLocked(HeldLock /*control_mutex*/)
    {
        {
            // Stored under state_mutex so the worker cannot miss the wake-up
            // between checking stop_requested and waiting on state_cv.
            std::lock_guard<std::mutex> lock(state_mutex);
            stop_requested.store(true, std::memory_order_release);
        }
        state_cv.notify_all();
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue_cv.notify_all();
        }
        if (worker.joinable()) {
            worker.join();
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        info.rate = 0.0f;
        info.ready_to_play = false;
        info.playback_buffer_empty = true;
        info.playback_buffer_full = false;
        info.playback_likely_to_keep_up = false;
        clock_initialized = false;
        anchor_valid = false;
    }

    void seek(double position)
    {
        if (!std::isfinite(position)) {
            blog(LOG_WARNING, "[MEDIA] scrub to a non-finite position ignored");
            return;
        }
        position = clamp_position(position);
        std::string restart_location;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (info.duration_known && info.duration > 0.0) {
                position = std::min(position, info.duration);
            }
            // The worker has finished at the end of the item: nothing would
            // act on a seek, yet /playback-info would report the new
            // position. Start the item again there instead, paused.
            if (info.ended && !current_location.empty()) {
                restart_location = current_location;
            } else {
                seekLocked(position, HeldLock(lock));
                return;
            }
        }
        blog(LOG_INFO, "[MEDIA] scrub to %.3fs after the end of the item; reopening it there",
             position);
        play(restart_location, position, 0.0f);
    }

    void seekLocked(double position, HeldLock /*state_mutex*/)
    {
        // Senders that keep a local player in sync send small corrective
        // scrubs while playing. A re-seek restarts decoding from a keyframe
        // (a visible hitch), so ignore corrections the output already meets.
        constexpr double kPlayingSeekTolerance = 0.35;
        if (info.rate > 0.0f && info.ready_to_play && !info.ended && !seek_pending &&
            clock_initialized && std::fabs(position - info.position) < kPlayingSeekTolerance) {
            blog(LOG_DEBUG, "[MEDIA] scrub to %.3fs ignored (output at %.3fs)",
                 position, info.position);
            return;
        }
        // A held scrubber re-sends the same position; the frame for it is
        // already on screen, so do not fetch and decode it again.
        constexpr double kSamePositionTolerance = 0.0005;
        if (info.rate <= 0.0f && !info.ended && !seek_pending && !preview_pending &&
            last_seek_target >= 0.0 &&
            std::fabs(position - last_seek_target) < kSamePositionTolerance) {
            return;
        }
        // Scrubs arriving in quick succession are a drag on the sender's
        // scrubber: each one may be replaced before its exact frame has been
        // decoded, so an earlier frame is shown when the exact one is late
        // (see takeCoarsePreview). The exact frame gets half the gap between
        // scrubs, at most 60 ms, to arrive first.
        const auto seek_time = std::chrono::steady_clock::now();
        const auto scrub_gap = seek_time - last_seek_at;
        scrub_burst = seek_generation > 0 && scrub_gap < std::chrono::milliseconds(200);
        if (scrub_burst) {
            coarse_preview_after = seek_time +
                std::min<std::chrono::steady_clock::duration>(
                    scrub_gap / 2, std::chrono::milliseconds(60));
        }
        last_seek_at = seek_time;
        coarse_preview_shown = false;
        last_seek_target = std::max(0.0, position);
        seek_target = std::max(0.0, position);
        seek_pending = true;
        ++seek_generation;
        // A worker waiting for room in the video queue takes the seek now.
        queue_cv.notify_all();
        info.position = seek_target;
        // The sender is at this position now (if it is playing).
        anchor_valid = info.rate > 0.0f;
        anchor_position = seek_target;
        anchor_wall_time = seek_time;
        catching_up_video = catching_up_audio = true;
        skipped_video_frames = 0;
        discard_video_before = discard_audio_before = seek_target;
        resync_on_resume = false;
        // While paused, show the frame at the new position once so the
        // output follows the sender's scrubber.
        preview_pending = true;
        clock_initialized = false;
        state_cv.notify_all();
    }

    void setRate(float rate)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!std::isfinite(rate)) rate = 0.0f;
        // After the end nothing plays; resuming needs a scrub (which reopens
        // the item) or a new /play.
        if (info.ended && rate > 0.0f) {
            blog(LOG_INFO, "[MEDIA] rate %.2f after the end of the item ignored", rate);
            return;
        }
        // Safari normally sends only 0 (pause) and 1 (play). Positive values
        // are still accepted so protocol diagnostics remain honest.
        const float previous_rate = info.rate;
        info.rate = rate > 0.0f ? rate : 0.0f;
        const auto rate_time = std::chrono::steady_clock::now();
        if (info.rate <= 0.0f) {
            anchor_valid = false;
        } else if (previous_rate <= 0.0f) {
            // The sender starts playing now, from where the output stands.
            anchor_valid = true;
            anchor_position = seek_pending ? seek_target : info.position;
            anchor_wall_time = rate_time;
            catching_up_video = catching_up_audio = true;
            skipped_video_frames = 0;
        } else if (anchor_valid && previous_rate != info.rate) {
            // A new speed: the sender has moved on at the old one since its
            // last command.
            anchor_position += std::chrono::duration<double>(
                rate_time - anchor_wall_time).count() * previous_rate;
            anchor_wall_time = rate_time;
        }
        if (clock_initialized) {
            // Continue from the last presented frame at the new rate.
            clock_media_position = info.position;
            clock_wall_time = std::chrono::steady_clock::now();
        }
        if (info.rate > 0.0f) {
            preview_pending = false;
            if (resync_on_resume && !seek_pending) {
                // Audio after a paused seek was skipped to reach the preview
                // frame; restart both streams from the previewed position.
                seek_target = info.position;
                seek_pending = true;
                discard_video_before = discard_audio_before = seek_target;
            }
            resync_on_resume = false;
        }
        state_cv.notify_all();
        blog(LOG_INFO, "[MEDIA] playback rate changed to %.3f", info.rate);
    }

    float pauseForPlaylistRemoval()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        info.rate = 0.0f;
        clock_initialized = false;
        anchor_valid = false;
        state_cv.notify_all();
        return static_cast<float>(info.position);
    }

    MediaPlaybackInfo getPlaybackInfo() const
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return info;
    }

private:
    static int interruptCallback(void* opaque)
    {
        auto* self = static_cast<Impl*>(opaque);
        return self && self->stop_requested.load(std::memory_order_acquire) ? 1 : 0;
    }

    static int policyIoOpen(AVFormatContext* format, AVIOContext** pb, const char* url,
                            int flags, AVDictionary** options)
    {
        auto* self = static_cast<Impl*>(format->opaque);
        std::string target = url ? url : "";
        // The HLS demuxer opens AES-128 segments as "crypto+<url>".
        std::string prefix;
        if (starts_with_case_insensitive(target, "crypto+") ||
            starts_with_case_insensitive(target, "crypto:")) {
            prefix = target.substr(0, 7);
            target.erase(0, 7);
        }
        if (!is_http_url(target) || !self->fetch_session) {
            blog(LOG_WARNING, "[MEDIA] refused to fetch %s: not an HTTP(S) URL",
                 safe_location_for_log(target).c_str());
            report_refusal("not an HTTP(S) URL");
            return AVERROR(EPERM);
        }
        // Nested opens do not inherit the item's TLS options.
        AVDictionary* local_options = nullptr;
        AVDictionary** used_options = options ? options : &local_options;
        set_tls_options(used_options);
        const int result = self->fetch_session->open(
            target, false, used_options,
            [&](const std::string& pinned, AVDictionary** attempt) {
                return self->default_io_open(format, pb, (prefix + pinned).c_str(), flags,
                                             attempt);
            });
        av_dict_free(&local_options);
        return result;
    }

    std::string resolveHlsVariant(const std::string& location)
    {
        if (!is_http_url(location) || !looks_like_hls_url(location)) {
            return location;
        }

        const auto started = std::chrono::steady_clock::now();
        AVIOContext* io = nullptr;
        AVIOInterruptCB interrupt = {&Impl::interruptCallback, this};
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", "7000000", 0);
        set_network_options(&options);
        av_dict_set(&options, "user_agent", "AppleCoreMedia/1.0 OBS-AirPlay/" PLUGIN_VERSION, 0);
        // The URL that answered after redirects (each one checked): the
        // base for the variant URIs.
        std::string final_location = location;
        const int open_result = fetch_session->open(
            location, false, &options,
            [&](const std::string& url, AVDictionary** used) {
                return avio_open2(&io, url.c_str(), AVIO_FLAG_READ, &interrupt, used);
            },
            &final_location);
        av_dict_free(&options);
        if (open_result < 0 || !io) {
            if (!stop_requested.load(std::memory_order_acquire) &&
                open_result != AVERROR(EPERM)) {
                blog(LOG_WARNING, "[MEDIA] HLS manifest preflight failed: %s",
                     ffmpeg_error(open_result).c_str());
            }
            if (io) avio_closep(&io);
            return location;
        }

        constexpr size_t kMaximumManifestBytes = 1024 * 1024;
        std::string playlist;
        std::vector<uint8_t> buffer(16384);
        bool too_large = false;
        while (!stop_requested.load(std::memory_order_acquire)) {
            const int read = avio_read(io, buffer.data(), static_cast<int>(buffer.size()));
            if (read == AVERROR_EOF || read == 0) break;
            if (read < 0) {
                playlist.clear();
                break;
            }
            if (playlist.size() + static_cast<size_t>(read) > kMaximumManifestBytes) {
                too_large = true;
                playlist.clear();
                break;
            }
            playlist.append(reinterpret_cast<const char*>(buffer.data()),
                            static_cast<size_t>(read));
        }
        avio_closep(&io);

        if (too_large) {
            blog(LOG_WARNING, "[MEDIA] HLS manifest exceeded the 1 MiB safety limit");
            return location;
        }
        if (playlist.empty() || playlist.find("#EXTM3U") == std::string::npos) {
            return location;
        }

        const std::vector<hls::Variant> variants = hls::parseVariants(playlist);
        const hls::Variant* selected = hls::chooseVariant(variants);
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (!selected) {
            blog(LOG_INFO, "[MEDIA] HLS media playlist preflight completed in %.1fms",
                 elapsed_ms);
            return location;
        }

        const std::string resolved = hls::resolveUrl(final_location, selected->uri);
        if (!is_http_url(resolved)) {
            blog(LOG_WARNING, "[MEDIA] HLS variant resolved to an unsupported URL scheme");
            return location;
        }
        blog(LOG_INFO,
             "[MEDIA] selected HLS rendition from %zu variants in %.1fms (bandwidth=%lld bps)",
             variants.size(), elapsed_ms, static_cast<long long>(selected->bandwidth));
        return resolved;
    }

    // Per-item video output state, owned by run().
    struct VideoOutput {
        SwsContext* scaler = nullptr;
        AVFrame* i420 = nullptr;      // software conversion target
        AVFrame* transfer = nullptr;  // hardware surface copied to memory
        AVFrame* rotated = nullptr;   // upright picture for rotated streams
        int turns = 0;                // clockwise quarter turns to apply
        // Read and written by both the worker (decode, send) and the
        // presenter (picture read-back).
        std::atomic<bool> hardware{false}; // the decoder was opened for hardware
        std::atomic<int> hardware_errors{0}; // consecutive, since the last picture
        // Set by the presenter, taken by the worker (which first waits for
        // the presenter to go idle before it changes the fields above).
        std::atomic<bool> reopen_in_software{false};
        bool mode_logged = false;
    };

    // Decoded video frames waiting for the presenter thread: decoding runs up
    // to kVideoQueueFrames ahead of what is on screen, instead of stopping
    // while each frame waits for its presentation time.
    struct QueuedFrame {
        AVFrame* frame = nullptr;
        AVStream* stream = nullptr;
        double format_start_seconds = 0.0;
        uint64_t epoch = 0;
    };
    static constexpr size_t kVideoQueueFrames = 3;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<QueuedFrame> video_queue;      // under queue_mutex
    bool presenter_busy = false;              // under queue_mutex
    bool presenter_quit = false;              // under queue_mutex
    // Advanced by the worker at every seek it carries out, under state_mutex
    // as it clears seek_pending; queued frames from an older epoch are dropped.
    std::atomic<uint64_t> queue_epoch{0};
    // The epoch of the frame the presenter is showing (presenter thread only):
    // a seek carried out while it waits makes it stale.
    uint64_t presenting_epoch = 0;

    void clearVideoQueue()
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        for (QueuedFrame& queued : video_queue) av_frame_free(&queued.frame);
        video_queue.clear();
        queue_cv.notify_all();
    }

    // Worker: hands a decoded frame to the presenter, waiting while the
    // queue is full. Gives up (dropping the frame) on stop or a new seek.
    void queueVideoFrame(AVFrame* decoded, AVStream* stream, double format_start_seconds)
    {
        AVFrame* copy = av_frame_clone(decoded);
        if (!copy) return;
        std::unique_lock<std::mutex> lock(queue_mutex);
        // (Rechecked every 50 ms: seek() notifies without queue_mutex.)
        while (!(video_queue.size() < kVideoQueueFrames || stopOrSeekPending())) {
            queue_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (stopOrSeekPending()) {
            av_frame_free(&copy);
            return;
        }
        video_queue.push_back({copy, stream, format_start_seconds, queue_epoch.load()});
        queue_cv.notify_all();
    }

    bool stopOrSeekPending()
    {
        if (stop_requested.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock(state_mutex);
        return seek_pending;
    }

    // Worker: waits until every queued frame has been presented (end of
    // stream, or before the worker changes what the presenter uses).
    void waitForPresenterIdle()
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        while (!(video_queue.empty() && !presenter_busy) &&
               !stop_requested.load(std::memory_order_acquire)) {
            queue_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    void presenterLoop(VideoOutput* out)
    {
        for (;;) {
            QueuedFrame queued;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&] {
                    return presenter_quit || !video_queue.empty() ||
                           stop_requested.load(std::memory_order_acquire);
                });
                if (stop_requested.load(std::memory_order_acquire) ||
                    (presenter_quit && video_queue.empty())) {
                    break;
                }
                queued = video_queue.front();
                video_queue.pop_front();
                presenter_busy = true;
                queue_cv.notify_all();
            }
            presenting_epoch = queued.epoch;
            if (queued.epoch == queue_epoch.load()) {
                emitVideo(queued.frame, queued.stream, queued.format_start_seconds, out);
            }
            av_frame_free(&queued.frame);
            std::lock_guard<std::mutex> lock(queue_mutex);
            presenter_busy = false;
            queue_cv.notify_all();
        }
        std::lock_guard<std::mutex> lock(queue_mutex);
        presenter_busy = false;
        queue_cv.notify_all();
    }

    bool openDecoder(AVFormatContext* format, int stream_index, AVCodecContext** decoder,
                     bool allow_hardware = false, bool* hardware = nullptr)
    {
        if (!format || stream_index < 0 || !decoder) {
            return false;
        }
        AVStream* stream = format->streams[stream_index];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            blog(LOG_ERROR, "[MEDIA] no decoder for codec %s",
                 avcodec_get_name(stream->codecpar->codec_id));
            return false;
        }
        AVCodecContext* context = avcodec_alloc_context3(codec);
        if (!context) {
            return false;
        }
        int result = avcodec_parameters_to_context(context, stream->codecpar);
        if (hardware) *hardware = false;
        if (result >= 0 && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVBufferRef* device = nullptr;
            if (allow_hardware && hardware_decode_requested() &&
                av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                       nullptr, nullptr, 0) >= 0 && device) {
                // FFmpeg's default format choice uses the device when the
                // stream can be decoded in hardware. The hardware decoder
                // does the work; no decoder threads needed.
                context->hw_device_ctx = device;
                context->thread_count = 1;
                if (hardware) *hardware = true;
            } else {
                // FFmpeg's default is a single thread, too slow for 4K or
                // HEVC files. This is file playback, so the frame of delay
                // per thread does not matter.
                context->thread_count = 0;
                context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            }
        }
        if (result >= 0) {
            result = avcodec_open2(context, codec, nullptr);
        }
        if (result < 0) {
            blog(LOG_ERROR, "[MEDIA] failed to open %s decoder: %s",
                 codec->name, ffmpeg_error(result).c_str());
            avcodec_free_context(&context);
            return false;
        }
        *decoder = context;
        return true;
    }

    // Frames before a pending start/seek target are decoded but never shown.
    // During a scrubber drag (paused, scrubs in quick succession) the exact
    // frame is often never reached before the next scrub arrives: decoding
    // forward from the keyframe can take longer than the gap between scrubs.
    // When the exact frame is late, the frame the decoder has reached by
    // then (the keyframe at or before the target, or one after it) is shown
    // so the output follows the finger; the exact frame replaces it if no
    // newer scrub arrives first. An exact frame that is in time is shown
    // alone, so the picture does not step back and forward on every scrub.
    // A single jump is not a burst and goes straight to the exact frame.
    bool takeCoarsePreview(double position)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        constexpr double kWorthwhileGap = 0.15;
        if (seek_pending || presenting_epoch != queue_epoch.load() ||
            info.rate > 0.0f || !preview_pending || !scrub_burst ||
            coarse_preview_shown || discard_video_before < 0.0 ||
            discard_video_before - position < kWorthwhileGap ||
            std::chrono::steady_clock::now() < coarse_preview_after) {
            return false;
        }
        coarse_preview_shown = true;
        return true;
    }

    // `shown_for`: how long the frame is on screen (a video frame's
    // duration; 0 when unknown, and for audio).
    bool shouldDiscard(double position, bool is_video, double shown_for = 0.0)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        // Frames still in the decoder from before a pending seek are stale,
        // and so is a frame the presenter took from the queue before the
        // worker carried the seek out: checked here, under state_mutex, it
        // must not clear discard_video_before for the new target.
        if (seek_pending || (is_video && presenting_epoch != queue_epoch.load())) {
            return true;
        }
        constexpr double kFrameTolerance = 0.020;
        // Following the sender's clock: after a start, seek or resume, the
        // frames the sender has already passed (while the file was opened,
        // or the frame was found from its keyframe) are skipped, so the
        // output starts in step with it. A frame is late only once it has
        // ended: the picture the sender shows at a position is the last frame
        // that starts at or before it, however long ago that was (a still
        // clip of one frame a second, or of a single frame). Each stream
        // catches up by itself: audio reaching the sender's position first
        // must not let the video frames it skipped be shown in a burst.
        bool& catching_up = is_video ? catching_up_video : catching_up_audio;
        if (catching_up) {
            if (followsSenderClock(HeldLock(lock)) &&
                position + std::max(0.0, shown_for) + kFrameTolerance < catchUpPositionLocked(HeldLock(lock))) {
                if (is_video) ++skipped_video_frames;
                return true;
            }
            if (is_video && followsSenderClock(HeldLock(lock))) {
                // How well the output starts in step, for the log.
                blog(LOG_INFO, "[MEDIA] in step with the sender: picture at %.3fs, sender at "
                     "%.3fs, %.0f ms after its command (%u frames skipped)",
                     position, senderPositionLocked(HeldLock(lock)),
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - anchor_wall_time).count(),
                     skipped_video_frames);
            }
            catching_up = false;
        }
        double& discard_before = is_video ? discard_video_before : discard_audio_before;
        if (discard_before < 0.0) return false;
        if (position + kFrameTolerance < discard_before) return true;
        discard_before = -1.0;
        return false;
    }

    // Audio is handed to OBS this much before it is due, stamped with its
    // own time: OBS buffers it, and the worker is free to decode video.
    static constexpr double kAudioLeadSeconds = 0.150;

    bool waitForPresentation(double position, uint64_t* timestamp_ns, bool is_video)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        std::chrono::steady_clock::time_point target;
        for (;;) {
            if (stop_requested.load(std::memory_order_acquire) || seek_pending ||
                (is_video && presenting_epoch != queue_epoch.load())) {
                return false;
            }
            if (info.rate <= 0.0f) {
                // (An audio-only item has no picture to preview: it simply
                // waits for playback to start.)
                if (preview_pending && info.has_video) {
                    if (!is_video) {
                        // Keep decoding until the video frame at the seek
                        // target is reached; resume re-seeks, so no audio is lost.
                        return false;
                    }
                    preview_pending = false;
                    resync_on_resume = true;
                    info.position = position;
                    clock_initialized = false;
                    *timestamp_ns = os_gettime_ns();
                    return true;
                }
                if (!pause_before_first_frame_logged.exchange(true)) {
                    blog(LOG_INFO, "[MEDIA] playback paused; waiting for rate > 0");
                }
                // A frame that was waiting when playback paused is kept and
                // shown at its proper time after resume, not dropped.
                state_cv.wait_for(lock, std::chrono::milliseconds(250), [&] {
                    return stop_requested.load(std::memory_order_acquire) || seek_pending ||
                           info.rate > 0.0f || (preview_pending && info.has_video) ||
                           (is_video && presenting_epoch != queue_epoch.load());
                });
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!clock_initialized) {
                clock_initialized = true;
                if (followsSenderClock(HeldLock(lock))) {
                    // Timed from the sender's command, not from this frame.
                    clock_media_position = anchor_position;
                    clock_wall_time = anchor_wall_time;
                } else {
                    clock_media_position = position;
                    clock_wall_time = now;
                }
            }
            // Recomputed on every wake-up: setRate() re-anchors the clock at
            // the last presented frame, so rate changes apply immediately
            // and never collapse the gap to a sparse next frame.
            const double delta = std::max(0.0, position - clock_media_position) /
                                 std::max(0.001f, info.rate);
            target = clock_wall_time +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(delta));
            const auto wake = is_video ? target
                : target - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(kAudioLeadSeconds));
            if (now >= wake) {
                break;
            }
            state_cv.wait_until(lock, wake);
        }

        // The output's position follows the picture (audio, sent ahead,
        // only for an audio-only item, and then by what is audible).
        if (is_video) {
            info.position = std::max(info.position, position);
        } else if (!info.has_video) {
            const double audible = position -
                std::max(0.0, std::chrono::duration<double>(
                    target - std::chrono::steady_clock::now()).count() * info.rate);
            info.position = std::max(info.position, audible);
        }
        const auto target_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            target.time_since_epoch()).count();
        // steady_clock and OBS use the host monotonic clock on macOS. If an
        // implementation ever gives them different epochs, fall back to now.
        const uint64_t obs_now = os_gettime_ns();
        const uint64_t candidate = target_ns > 0 ? static_cast<uint64_t>(target_ns) : obs_now;
        constexpr uint64_t kClockToleranceNs = UINT64_C(5000000000);
        *timestamp_ns = candidate > obs_now + kClockToleranceNs ||
                        obs_now > candidate + kClockToleranceNs
            ? obs_now : candidate;
        return true;
    }

    // How far before a target to seek in a container without a keyframe
    // index (MPEG-TS): FFmpeg's timestamp search lands on any packet, and
    // decoding then starts at the next keyframe, after the target. Seeking
    // this much earlier (about a GOP) lets the discard logic stop exactly on
    // the target. HLS segments and indexed containers start on keyframes.
    static double seekPreroll(const AVFormatContext* format, int video_index)
    {
        constexpr double kUnindexedSeekPreroll = 2.0;
        if (!format || video_index < 0 || !format->iformat || !format->iformat->name ||
            std::strstr(format->iformat->name, "hls")) {
            return 0.0;
        }
        return avformat_index_get_entries_count(format->streams[video_index]) > 0
            ? 0.0 : kUnindexedSeekPreroll;
    }

    bool handlePendingSeek(AVFormatContext* format, AVCodecContext* video_decoder,
                           AVCodecContext* audio_decoder, double preroll)
    {
        double target = 0.0;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!seek_pending) {
                return false;
            }
            target = seek_target;
            seek_pending = false;
            clock_initialized = false;
            // Frames decoded before the seek are not shown, including one the
            // presenter is waiting with. Advanced together with clearing
            // seek_pending, so the presenter's checks under state_mutex
            // (shouldDiscard, waitForPresentation) see one or the other.
            queue_epoch.fetch_add(1);
            state_cv.notify_all();
        }
        if (const int delay = g_seek_worker_delay_for_testing_ms.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        clearVideoQueue();

        const int64_t timestamp =
            seek_timestamp(target - preroll, stream_start_seconds(format));
        const int result = av_seek_frame(format, -1, timestamp, AVSEEK_FLAG_BACKWARD);
        if (result < 0) {
            blog(LOG_WARNING, "[MEDIA] seek to %.3fs failed: %s",
                 target, ffmpeg_error(result).c_str());
        } else {
            if (video_decoder) avcodec_flush_buffers(video_decoder);
            if (audio_decoder) avcodec_flush_buffers(audio_decoder);
            blog(LOG_INFO, "[MEDIA] seeked to %.3fs", target);
        }
        return true;
    }

    // How long a decoded video frame is shown: its own duration, or the
    // stream's frame interval when the frame carries none; 0 if neither is known.
    static double frameSeconds(const AVFrame* decoded, const AVStream* stream)
    {
        if (decoded->duration > 0) {
            return decoded->duration * av_q2d(stream->time_base);
        }
        const AVRational rate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
            ? stream->avg_frame_rate : stream->r_frame_rate;
        return rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 0.0;
    }

    // Converts a picture OBS cannot take as it is (10-bit, 4:2:2, ...) to
    // I420 in out->i420. swscale converts full-range (YUVJ) layouts to video
    // range, so `full_range` may change.
    bool convertToI420(VideoOutput* out, PictureView& picture, bool& full_range)
    {
        // swscale converts full-range (YUVJ) layouts to video range and
        // leaves the range of everything else untouched.
        const AVPixFmtDescriptor* descriptor =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(picture.format));
        if (descriptor && std::strncmp(descriptor->name, "yuvj", 4) == 0) {
            full_range = false;
        }
        out->scaler = sws_getCachedContext(
            out->scaler, picture.width, picture.height,
            static_cast<AVPixelFormat>(picture.format),
            picture.width, picture.height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!out->scaler) {
            blog(LOG_ERROR, "[MEDIA] failed to create video converter");
            return false;
        }
        AVFrame* i420_frame = out->i420;
        if (i420_frame->width != picture.width || i420_frame->height != picture.height ||
            i420_frame->format != AV_PIX_FMT_YUV420P) {
            av_frame_unref(i420_frame);
            i420_frame->format = AV_PIX_FMT_YUV420P;
            i420_frame->width = picture.width;
            i420_frame->height = picture.height;
            if (av_frame_get_buffer(i420_frame, 32) < 0) {
                av_frame_unref(i420_frame);
                blog(LOG_ERROR, "[MEDIA] failed to allocate converted video frame");
                return false;
            }
        }
        if (av_frame_make_writable(i420_frame) < 0) {
            return false;
        }
        sws_scale(out->scaler, picture.data, picture.linesize, 0, picture.height,
                  i420_frame->data, i420_frame->linesize);
        picture = PictureView::of(i420_frame);
        return true;
    }

    // Turns the picture upright (out->turns quarter turns) into out->rotated.
    bool rotateUpright(VideoOutput* out, PictureView& picture, bool nv12)
    {
        const int width = picture.width;
        const int height = picture.height;
        const int rotated_width = out->turns == 2 ? width : height;
        const int rotated_height = out->turns == 2 ? height : width;
        const AVPixelFormat layout = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        AVFrame* rotated = out->rotated;
        if (rotated->width != rotated_width || rotated->height != rotated_height ||
            rotated->format != layout) {
            av_frame_unref(rotated);
            rotated->format = layout;
            rotated->width = rotated_width;
            rotated->height = rotated_height;
            if (av_frame_get_buffer(rotated, 32) < 0) {
                av_frame_unref(rotated);
                blog(LOG_ERROR, "[MEDIA] failed to allocate rotated video frame");
                return false;
            }
        }
        if (av_frame_make_writable(rotated) < 0) {
            return false;
        }
        const int chroma_width = (width + 1) / 2;
        const int chroma_height = (height + 1) / 2;
        rotate_plane(picture.data[0], picture.linesize[0], width, height, 1,
                     out->turns, rotated->data[0], rotated->linesize[0]);
        if (nv12) {
            rotate_plane(picture.data[1], picture.linesize[1], chroma_width,
                         chroma_height, 2, out->turns, rotated->data[1],
                         rotated->linesize[1]);
        } else {
            for (int plane = 1; plane <= 2; ++plane) {
                rotate_plane(picture.data[plane], picture.linesize[plane],
                             chroma_width, chroma_height, 1, out->turns,
                             rotated->data[plane], rotated->linesize[plane]);
            }
        }
        picture = PictureView::of(rotated);
        return true;
    }

    void emitVideo(AVFrame* decoded, AVStream* stream, double format_start_seconds,
                   VideoOutput* out)
    {
        const bool hardware_frame = decoded->format == AV_PIX_FMT_VIDEOTOOLBOX;
        if (out->hardware && !hardware_frame) {
            // The stream cannot be decoded in hardware and FFmpeg carried on
            // in software inside the single-threaded hardware context:
            // run() reopens a threaded software decoder.
            out->reopen_in_software = true;
        }

        const double position = timestamp_seconds(decoded, stream, format_start_seconds);
        if (position < 0.0 || presenting_epoch != queue_epoch.load()) {
            return;
        }
        bool coarse_preview = false;
        if (const int delay = g_seek_presenter_delay_for_testing_ms.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        if (shouldDiscard(position, true, frameSeconds(decoded, stream))) {
            if (!takeCoarsePreview(position)) {
                return;
            }
            coarse_preview = true;
        }

        // The picture as planes: the decoder's own, a locked VideoToolbox
        // buffer's NV12 planes (no copy to memory: OBS copies them once), or
        // a conversion target.
        PictureView picture = PictureView::of(decoded);
        LockedPixelBuffer locked;
        if (hardware_frame) {
            if (!locked.lock(decoded, &picture)) {
                // Not 8-bit NV12 (10-bit, 4:2:2): copy the surface out and
                // convert it.
                av_frame_unref(out->transfer);
                if (av_hwframe_transfer_data(out->transfer, decoded, 0) < 0) {
                    noteHardwareError(out, "could not read the decoded picture");
                    return;
                }
                picture = PictureView::of(out->transfer);
            }
        }
        out->hardware_errors = 0;

        // NV12 (hardware) and I420 go to OBS as they are, in the stream's
        // own range; everything else (10-bit, 4:2:2, ...) is converted.
        const bool nv12 = picture.format == AV_PIX_FMT_NV12;
        const bool i420 = picture.format == AV_PIX_FMT_YUV420P ||
                          picture.format == AV_PIX_FMT_YUVJ420P;
        bool full_range = decoded->color_range == AVCOL_RANGE_JPEG ||
                          picture.format == AV_PIX_FMT_YUVJ420P;
        const bool bt601 = decoded->colorspace == AVCOL_SPC_SMPTE170M ||
                           decoded->colorspace == AVCOL_SPC_BT470BG;
        if (!out->mode_logged) {
            out->mode_logged = true;
            blog(LOG_INFO, "[MEDIA] video %dx%d: %s decoding (%s, %s range%s%s)",
                 picture.width, picture.height,
                 hardware_frame ? "VideoToolbox hardware" : "software",
                 av_get_pix_fmt_name(static_cast<AVPixelFormat>(picture.format)),
                 full_range ? "full" : "video",
                 out->turns ? ", rotated upright" : "",
                 locked.held() ? ", zero-copy" : "");
        }
        if (!nv12 && !i420 && !convertToI420(out, picture, full_range)) {
            return;
        }
        if (out->turns) {
            if (!rotateUpright(out, picture, nv12)) {
                return;
            }
            locked.unlock();
        }

        uint64_t timestamp_ns = 0;
        if (coarse_preview) {
            // Shown as it is, now; the reported position stays the target.
            timestamp_ns = os_gettime_ns();
        } else if (!waitForPresentation(position, &timestamp_ns, true)) {
            return;
        }

        MediaVideoCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = video_callback;
        }
        if (callback) {
            // The planes are valid only during the call (a locked buffer is
            // unlocked when emitVideo returns); OBS copies them synchronously.
            MediaVideoFrame frame;
            frame.width = picture.width;
            frame.height = picture.height;
            frame.timestamp_ns = timestamp_ns;
            frame.nv12 = nv12;
            frame.full_range = full_range;
            frame.bt601 = bt601;
            for (int i = 0; i < (nv12 ? 2 : 3); ++i) {
                frame.data[i] = picture.data[i];
                frame.linesize[i] = picture.linesize[i];
            }
            callback(frame);
        }

        if (!first_video_logged.exchange(true)) {
            blog(LOG_INFO, "[MEDIA] first decoded video frame (%dx%d, %.3fs)",
                 picture.width, picture.height, position);
        }
    }

    // Counts a hardware decoding failure; after a few in a row run() reopens
    // the decoder in software for the rest of the item.
    void noteHardwareError(VideoOutput* out, const char* what)
    {
        if (!out->hardware || out->reopen_in_software) {
            return;
        }
        constexpr int kMaxHardwareErrors = 3;
        if (++out->hardware_errors >= kMaxHardwareErrors) {
            blog(LOG_WARNING, "[MEDIA] hardware decoding stopped (%s); continuing in software",
                 what);
            out->reopen_in_software = true;
        }
    }

    void emitAudio(AVFrame* decoded, AVStream* stream, AVCodecContext* decoder,
                   double format_start_seconds, SwrContext** resampler,
                   AVChannelLayout* input_layout, int* input_rate,
                   AVSampleFormat* input_format)
    {
        const double position = timestamp_seconds(decoded, stream, format_start_seconds);
        if (position < 0.0 || shouldDiscard(position, false)) {
            return;
        }

        const AVChannelLayout* layout = decoded->ch_layout.nb_channels > 0
            ? &decoded->ch_layout : &decoder->ch_layout;
        const int sample_rate = decoded->sample_rate > 0
            ? decoded->sample_rate : decoder->sample_rate;
        const auto sample_format = static_cast<AVSampleFormat>(decoded->format);
        if (layout->nb_channels <= 0 || sample_rate <= 0) {
            return;
        }

        // Planar float stereo (AAC, most streams) goes to OBS as decoded.
        if (sample_format == AV_SAMPLE_FMT_FLTP && layout->nb_channels == 2 &&
            decoded->nb_samples > 0 && decoded->extended_data[0] && decoded->extended_data[1]) {
            presentAudio(reinterpret_cast<const float*>(decoded->extended_data[0]),
                         reinterpret_cast<const float*>(decoded->extended_data[1]),
                         decoded->nb_samples, sample_rate, position);
            return;
        }

        const bool layout_changed = input_layout->nb_channels == 0 ||
            av_channel_layout_compare(input_layout, layout) != 0;
        if (!*resampler || layout_changed || *input_rate != sample_rate ||
            *input_format != sample_format) {
            swr_free(resampler);
            av_channel_layout_uninit(input_layout);
            if (av_channel_layout_copy(input_layout, layout) < 0) {
                return;
            }
            AVChannelLayout stereo;
            av_channel_layout_default(&stereo, 2);
            const int result = swr_alloc_set_opts2(
                resampler, &stereo, AV_SAMPLE_FMT_FLTP, sample_rate,
                input_layout, sample_format, sample_rate, 0, nullptr);
            av_channel_layout_uninit(&stereo);
            if (result < 0 || !*resampler || swr_init(*resampler) < 0) {
                blog(LOG_ERROR, "[MEDIA] failed to configure audio converter");
                swr_free(resampler);
                return;
            }
            *input_rate = sample_rate;
            *input_format = sample_format;
        }

        const int capacity = static_cast<int>(av_rescale_rnd(
            swr_get_delay(*resampler, sample_rate) + decoded->nb_samples,
            sample_rate, sample_rate, AV_ROUND_UP));
        if (capacity <= 0) {
            return;
        }
        // Reused across packets (worker thread only); grows, never shrinks.
        if (audio_left.size() < static_cast<size_t>(capacity)) {
            audio_left.resize(static_cast<size_t>(capacity));
            audio_right.resize(static_cast<size_t>(capacity));
        }
        uint8_t* output[2] = {
            reinterpret_cast<uint8_t*>(audio_left.data()),
            reinterpret_cast<uint8_t*>(audio_right.data())
        };
        const int converted = swr_convert(
            *resampler, output, capacity,
            const_cast<const uint8_t**>(decoded->extended_data), decoded->nb_samples);
        if (converted <= 0) {
            return;
        }
        presentAudio(audio_left.data(), audio_right.data(), converted, sample_rate, position);
    }

    void presentAudio(const float* left, const float* right, int samples, int sample_rate,
                      double position)
    {
        const int converted = samples;
        uint64_t timestamp_ns = 0;
        if (!waitForPresentation(position, &timestamp_ns, false)) {
            return;
        }
        MediaAudioCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = audio_callback;
        }
        if (callback) {
            MediaAudioFrame frame;
            frame.data[0] = left;
            frame.data[1] = right;
            frame.frames = static_cast<uint32_t>(converted);
            frame.sample_rate = static_cast<uint32_t>(sample_rate);
            frame.timestamp_ns = timestamp_ns;
            callback(frame);
        }
        if (!first_audio_logged.exchange(true)) {
            blog(LOG_INFO, "[MEDIA] first decoded audio frame (%d Hz, %d samples, %.3fs)",
                 sample_rate, converted, position);
        }
    }

    void drainDecoder(AVCodecContext* decoder, AVStream* stream, bool video,
                      double format_start_seconds, VideoOutput* video_output,
                      SwrContext** resampler,
                      AVChannelLayout* input_layout, int* input_rate,
                      AVSampleFormat* input_format, AVFrame* decoded)
    {
        while (!stop_requested.load(std::memory_order_acquire)) {
            const int result = avcodec_receive_frame(decoder, decoded);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                blog(LOG_WARNING, "[MEDIA] decode failed: %s", ffmpeg_error(result).c_str());
                if (video) noteHardwareError(video_output, "repeated decode errors");
                break;
            }
            if (video) {
                (void)video_output;
                queueVideoFrame(decoded, stream, format_start_seconds);
            } else {
                emitAudio(decoded, stream, decoder, format_start_seconds,
                          resampler, input_layout, input_rate, input_format);
            }
            av_frame_unref(decoded);
        }
    }

    // Everything an item needs while it plays. The destructor releases the
    // decoders and frames, then the demuxer, then its custom input (MEM-7:
    // no early return can leak one).
    struct Item {
        std::unique_ptr<ResilientHttpIo> resilient_io;
        AVFormatContext* format = nullptr;
        bool format_opened = false;
        AVCodecContext* video_decoder = nullptr;
        AVCodecContext* audio_decoder = nullptr;
        VideoOutput video_output;
        int video_index = -1;
        int audio_index = -1;
        bool duration_known = false;
        double duration = 0.0;
        double format_start_seconds = 0.0;
        double seek_preroll = 0.0;

        Item() = default;
        Item(const Item&) = delete;
        Item& operator=(const Item&) = delete;
        ~Item()
        {
            if (video_output.scaler) sws_freeContext(video_output.scaler);
            av_frame_free(&video_output.rotated);
            av_frame_free(&video_output.transfer);
            av_frame_free(&video_output.i420);
            avcodec_free_context(&audio_decoder);
            avcodec_free_context(&video_decoder);
            if (format) {
                if (format_opened) avformat_close_input(&format);
                else avformat_free_context(format);
            }
        }
    };

    // Opens the item: demuxer, streams, decoders, the initial seek. False
    // (after markOpenFailure, unless stopping) when it cannot be played.
    bool openItem(const std::string& playback_location, bool http_item, double start_position,
                  Item& item)
    {
        AVFormatContext*& format = item.format;
        std::unique_ptr<ResilientHttpIo>& resilient_io = item.resilient_io;
        AVCodecContext*& video_decoder = item.video_decoder;
        AVCodecContext*& audio_decoder = item.audio_decoder;
        VideoOutput& video_output = item.video_output;
        int& video_index = item.video_index;
        int& audio_index = item.audio_index;
        bool& duration_known = item.duration_known;
        double& duration = item.duration;
        double& format_start_seconds = item.format_start_seconds;
        double& seek_preroll = item.seek_preroll;

        const auto open_started = std::chrono::steady_clock::now();
        format = avformat_alloc_context();
        if (!format) {
            markOpenFailure("could not allocate demuxer");
            return false;
        }
        format->interrupt_callback.callback = &Impl::interruptCallback;
        format->interrupt_callback.opaque = this;
        if (http_item) {
            // Every URL the demuxer opens (the item, HLS playlists, segments,
            // keys) goes through the URL policy.
            format->opaque = this;
            default_io_open = format->io_open;
            format->io_open = &Impl::policyIoOpen;
        }
        // Progressive HTTP media goes through the byte-exact input layer;
        // HLS and unsized responses keep FFmpeg's own HTTP input. HLS is
        // known by its .m3u8 name or, for token-style URLs, by the server's
        // content type.
        bool hls_input = looks_like_hls_url(playback_location);
        if (is_http_url(playback_location) && !hls_input) {
            resilient_io = std::make_unique<ResilientHttpIo>(
                playback_location, AVIOInterruptCB{&Impl::interruptCallback, this},
                *fetch_session);
            if (resilient_io->open() && is_hls_mime_type(resilient_io->mimeType())) {
                blog(LOG_INFO, "[MEDIA] the URL serves an HLS playlist (%s)",
                     resilient_io->mimeType().c_str());
                hls_input = true;
                resilient_io.reset();
            } else if (resilient_io->context()) {
                format->pb = resilient_io->context();
                format->flags |= AVFMT_FLAG_CUSTOM_IO;
            } else if (resilient_io->refused()) {
                avformat_free_context(format);
                format = nullptr;
                resilient_io.reset();
                markOpenFailure("media URL refused");
                return false;
            } else {
                resilient_io.reset();
            }
        }
        if (hls_input) {
            apply_hls_probe_limits(format);
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", "10000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "2", 0);
        av_dict_set(&options, "user_agent", "AppleCoreMedia/1.0 OBS-AirPlay/" PLUGIN_VERSION, 0);
        if (http_item) {
            set_network_options(&options);
        } else {
            // The receiver's spool file: no nested protocols, no references
            // to other files from inside the container.
            av_dict_set(&options, "protocol_whitelist", "file", 0);
            av_dict_set(&options, "enable_drefs", "0", 0);
        }
        int result = avformat_open_input(&format, playback_location.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (result < 0) {
            // avformat_open_input has freed the context and cleared `format`.
            if (!stop_requested.load()) {
                markOpenFailure(("URL open failed: " + ffmpeg_error(result)).c_str());
            }
            return false;
        }
        item.format_opened = true;

        const auto transport_opened = std::chrono::steady_clock::now();
        const double transport_ms = std::chrono::duration<double, std::milli>(
            transport_opened - open_started).count();
        blog(LOG_INFO, "[MEDIA] transport opened in %.1fms (streams=%u, programs=%u)",
             transport_ms, format->nb_streams, format->nb_programs);

        int64_t selected_bitrate = 0;
        AVProgram* selected_program = select_hls_program(format, &selected_bitrate);
        if (selected_program) {
            apply_hls_probe_limits(format);
            blog(LOG_INFO,
                 "[MEDIA] selected one of %u HLS variants (bandwidth=%lld bps)",
                 format->nb_programs, static_cast<long long>(selected_bitrate));
        }

        result = avformat_find_stream_info(format, nullptr);
        if (result < 0) {
            markOpenFailure(("stream discovery failed: " + ffmpeg_error(result)).c_str());
            return false;
        }

        video_index = selected_program
            ? find_program_stream(format, selected_program, AVMEDIA_TYPE_VIDEO)
            : av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audio_index = selected_program
            ? find_program_stream(format, selected_program, AVMEDIA_TYPE_AUDIO)
            : av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (video_index < 0 && audio_index < 0) {
            markOpenFailure("URL contains no decodable audio or video streams");
            return false;
        }

        bool hardware = false;
        if (video_index >= 0 &&
            !openDecoder(format, video_index, &video_decoder, true, &hardware)) {
            blog(LOG_WARNING, "[MEDIA] video stream will be skipped");
        }
        video_output.hardware = hardware;
        if (video_decoder) {
            video_output.turns = display_quarter_turns(format->streams[video_index]);
        }
        if (audio_index >= 0 && !openDecoder(format, audio_index, &audio_decoder)) {
            blog(LOG_WARNING, "[MEDIA] audio stream will be skipped");
        }
        if (!video_decoder && !audio_decoder) {
            markOpenFailure("no advertised stream could be decoded");
            return false;
        }

        duration_known = format->duration != AV_NOPTS_VALUE && format->duration > 0;
        duration = duration_known
            ? format->duration / static_cast<double>(AV_TIME_BASE) : 0.0;
        format_start_seconds = format->start_time != AV_NOPTS_VALUE
            ? format->start_time / static_cast<double>(AV_TIME_BASE) : 0.0;
        if (duration_known) start_position = std::min(start_position, duration);
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            // The sender's start position, reported until frames arrive, and
            // only ever raised by them: not past the end.
            if (duration_known) info.position = std::min(info.position, duration);
            info.duration = duration;
            info.seek_start = 0.0;
            info.seek_duration = duration;
            info.duration_known = duration_known;
            info.has_video = video_decoder != nullptr;
            info.has_audio = audio_decoder != nullptr;
            info.ready_to_play = true;
            info.playback_buffer_empty = false;
            info.playback_buffer_full = true;
            info.playback_likely_to_keep_up = true;
        }

        const char* video_name = video_decoder ? avcodec_get_name(video_decoder->codec_id) : "none";
        const char* audio_name = audio_decoder ? avcodec_get_name(audio_decoder->codec_id) : "none";
        blog(LOG_INFO, "[MEDIA] URL ready (format=%s, video=%s, audio=%s, duration=%.3fs)",
             format->iformat && format->iformat->name ? format->iformat->name : "unknown",
             video_name, audio_name, duration);

        seek_preroll = seekPreroll(format, video_index);
        if (start_position > 0.0) {
            const int64_t target =
                seek_timestamp(start_position - seek_preroll, format_start_seconds);
            if (av_seek_frame(format, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
                blog(LOG_WARNING, "[MEDIA] initial seek to %.3fs was not available", start_position);
            }
        }

        return true;
    }

    // Demuxes and decodes the item until its end, a stop or a fatal error;
    // video goes to the presenter thread, audio is presented here.
    void playItem(Item& item)
    {
        AVFormatContext* format = item.format;
        AVCodecContext*& video_decoder = item.video_decoder;
        AVCodecContext*& audio_decoder = item.audio_decoder;
        VideoOutput& video_output = item.video_output;
        const int video_index = item.video_index;
        const int audio_index = item.audio_index;
        const bool duration_known = item.duration_known;
        const double duration = item.duration;
        const double format_start_seconds = item.format_start_seconds;
        const double seek_preroll = item.seek_preroll;
        int result = 0;

        AVPacket* packet = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        video_output.i420 = av_frame_alloc();
        video_output.transfer = av_frame_alloc();
        video_output.rotated = av_frame_alloc();
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            presenter_quit = false;
            presenter_busy = false;
        }
        std::thread presenter;
        if (video_decoder) {
            presenter = std::thread(&Impl::presenterLoop, this, &video_output);
        }
        SwrContext* resampler = nullptr;
        AVChannelLayout input_layout = {};
        ScopeExit release_frames{[&] {
            av_channel_layout_uninit(&input_layout);
            swr_free(&resampler);
            av_frame_free(&decoded);
            av_packet_free(&packet);
        }};
        if (!packet || !decoded || !video_output.i420 || !video_output.transfer ||
            !video_output.rotated) {
            markOpenFailure("could not allocate decoding buffers");
            return;
        }
        int input_rate = 0;
        AVSampleFormat input_format = AV_SAMPLE_FMT_NONE;

        // Some sender media servers (iOS AVPlayer's among them) end an HTTP
        // response early or close a kept-alive connection, which FFmpeg
        // reports as end of file right after a seek.  A read that ends well
        // before the known duration re-seeks (a fresh request) instead.
        constexpr int kMaxReadRecoveries = 3;
        // Output progress (seconds) past the last recovery point that counts
        // as real progress and earns a fresh set of attempts.
        constexpr double kRecoveryProgress = 0.5;
        // An end of file this close to the stated duration (seconds) is the
        // real end, not a dropped response.
        constexpr double kEndTolerance = 1.0;
        // Demuxed packets reaching this close to the duration cover it all
        // (container durations are rounded).
        constexpr double kDemuxedEndTolerance = 0.1;
        int read_recoveries = 0;
        double position_at_recovery = -1.0;
        uint64_t seek_generation_at_recovery = 0;
        // Furthest media time covered by a demuxed packet (pts + duration);
        // a read that already covered the stated duration is a genuine end.
        double demuxed_until = 0.0;
        uint64_t demuxed_generation = 0;
        // Whether any packet was read since the last (re)seek: a failed
        // re-request can leave the read position at the end of the file
        // (e.g. after reading a trailing moov) without delivering media.
        bool demuxed_since_seek = false;

        while (!stop_requested.load(std::memory_order_acquire)) {
            if (handlePendingSeek(format, video_decoder, audio_decoder, seek_preroll)) {
                demuxed_since_seek = false;
                swr_free(&resampler);
                av_channel_layout_uninit(&input_layout);
                input_rate = 0;
                input_format = AV_SAMPLE_FMT_NONE;
            }

            result = av_read_frame(format, packet);
            if (result < 0 && !stop_requested.load(std::memory_order_acquire)) {
                bool retry = false;
                double resume_at = 0.0;
                // Every byte read means a genuine end, whatever the container
                // duration says (a still clip's last frame can start seconds
                // before the stated duration).
                const int64_t io_size = format->pb ? avio_size(format->pb) : -1;
                const bool io_at_end = io_size > 0 && format->pb &&
                    avio_tell(format->pb) >= io_size && demuxed_since_seek;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    // Only real progress earns a fresh set of attempts: the
                    // output moved on, or the sender asked for a new position.
                    // Re-showing the same frame after a re-request is not.
                    if (info.position > position_at_recovery + kRecoveryProgress ||
                        seek_generation != seek_generation_at_recovery) {
                        read_recoveries = 0;
                    }
                    seek_generation_at_recovery = seek_generation;
                    if (seek_pending) {
                        retry = true;  // the pending seek repositions the demuxer
                    } else if (!io_at_end && duration_known &&
                               info.position < duration - kEndTolerance &&
                               !(demuxed_generation == seek_generation &&
                                 demuxed_until >= duration - kDemuxedEndTolerance) &&
                               read_recoveries < kMaxReadRecoveries) {
                        ++read_recoveries;
                        resume_at = info.position;
                        position_at_recovery = resume_at;
                        seek_target = resume_at;
                        seek_pending = true;
                        discard_video_before = discard_audio_before = resume_at;
                        clock_initialized = false;
                        retry = true;
                        blog(LOG_WARNING,
                             "[MEDIA] media read ended early at %.3fs of %.3fs (%s); "
                             "re-requesting from that position (attempt %d)",
                             resume_at, duration, ffmpeg_error(result).c_str(),
                             read_recoveries);
                    }
                }
                if (retry) {
                    demuxed_since_seek = false;
                    if (format->pb) {
                        format->pb->eof_reached = 0;
                        format->pb->error = 0;
                    }
                    continue;
                }
            }
            if (result < 0) {
                if (result != AVERROR_EOF && !stop_requested.load()) {
                    blog(LOG_WARNING, "[MEDIA] media read ended: %s", ffmpeg_error(result).c_str());
                }
                break;
            }

            if (packet->pts != AV_NOPTS_VALUE &&
                packet->stream_index >= 0 &&
                packet->stream_index < static_cast<int>(format->nb_streams)) {
                const AVStream* packet_stream = format->streams[packet->stream_index];
                const double packet_end = (packet->pts + std::max<int64_t>(0, packet->duration)) *
                    av_q2d(packet_stream->time_base) - format_start_seconds;
                std::lock_guard<std::mutex> lock(state_mutex);
                if (demuxed_generation != seek_generation) {
                    demuxed_generation = seek_generation;
                    demuxed_until = 0.0;
                }
                demuxed_until = std::max(demuxed_until, packet_end);
                demuxed_since_seek = true;
            }

            AVCodecContext* decoder = nullptr;
            AVStream* stream = nullptr;
            bool is_video = false;
            if (packet->stream_index == video_index && video_decoder) {
                decoder = video_decoder;
                stream = format->streams[video_index];
                is_video = true;
            } else if (packet->stream_index == audio_index && audio_decoder) {
                decoder = audio_decoder;
                stream = format->streams[audio_index];
            }

            if (decoder) {
                const int sent = avcodec_send_packet(decoder, packet);
                if (sent >= 0) {
                    drainDecoder(decoder, stream, is_video, format_start_seconds,
                                 &video_output, &resampler, &input_layout,
                                 &input_rate, &input_format, decoded);
                } else if (is_video) {
                    noteHardwareError(&video_output, "repeated errors sending packets");
                }
            }
            av_packet_unref(packet);

            if (video_output.reopen_in_software.load()) {
                // Hardware decoding failed or does not take this stream:
                // decode the rest of the item in software, continuing from
                // the picture on screen.
                waitForPresenterIdle();
                video_output.reopen_in_software = false;
                video_output.hardware = false;
                video_output.hardware_errors = 0;
                video_output.mode_logged = false;
                avcodec_free_context(&video_decoder);
                if (!openDecoder(format, video_index, &video_decoder)) {
                    blog(LOG_WARNING, "[MEDIA] software video decoder could not be opened");
                }
                std::lock_guard<std::mutex> lock(state_mutex);
                if (!seek_pending) {
                    seek_target = info.position;
                    seek_pending = true;
                    discard_video_before = discard_audio_before = seek_target;
                    clock_initialized = false;
                }
            }
        }

        if (!stop_requested.load(std::memory_order_acquire)) {
            if (video_decoder) {
                avcodec_send_packet(video_decoder, nullptr);
                drainDecoder(video_decoder, format->streams[video_index], true,
                             format_start_seconds, &video_output, &resampler,
                             &input_layout, &input_rate, &input_format, decoded);
            }
            if (audio_decoder) {
                avcodec_send_packet(audio_decoder, nullptr);
                drainDecoder(audio_decoder, format->streams[audio_index], false,
                             format_start_seconds, &video_output, &resampler,
                             &input_layout, &input_rate, &input_format, decoded);
            }
            // The end is reached once the last queued picture is on screen.
            waitForPresenterIdle();
            std::lock_guard<std::mutex> lock(state_mutex);
            info.ended = true;
            info.rate = 0.0f;
            info.playback_buffer_empty = true;
            info.playback_buffer_full = false;
            blog(LOG_INFO, "[MEDIA] playback reached end of stream at %.3fs", info.position);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            presenter_quit = true;
            queue_cv.notify_all();
        }
        if (presenter.joinable()) presenter.join();
        clearVideoQueue();
    }

    void run(std::string location, double start_position)
    {
        static std::once_flag network_once;
        std::call_once(network_once, [] { avformat_network_init(); });
        first_video_logged.store(false);
        first_audio_logged.store(false);
        pause_before_first_frame_logged.store(false);

        const std::string log_location = safe_location_for_log(location);
        blog(LOG_INFO, "[MEDIA] opening Safari media URL: %s", log_location.c_str());
        const bool http_item = is_http_url(location);
        // Every fetch for this item goes through it; its lookups are
        // interrupted by stop() (they run while stop() waits for this thread).
        media_fetch::Session fetch(
            location, g_allow_insecure_tls.load(std::memory_order_relaxed),
            [this] { return stop_requested.load(std::memory_order_acquire); }, report_refusal);
        fetch_session = &fetch;
        ScopeExit clear_fetch{[this] { fetch_session = nullptr; }};
        const std::string refused = http_item ? fetch.check(location, true)
                                              : media_url_policy::checkLocalFile(location);
        if (stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (!refused.empty()) {
            markOpenFailure(("media URL refused: " + refused).c_str());
            report_refusal(refused);
            return;
        }
        const std::string playback_location =
            http_item ? resolveHlsVariant(location) : location;
        if (stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (playback_location != location) {
            const std::string variant_refused = fetch.check(playback_location, false);
            if (!variant_refused.empty()) {
                markOpenFailure(("HLS rendition URL refused: " + variant_refused).c_str());
                report_refusal(variant_refused);
                return;
            }
        }
        if (const int delay = g_open_delay_for_testing_ms.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        Item item;
        if (!openItem(playback_location, http_item, start_position, item)) {
            return;
        }
        playItem(item);
        if (item.resilient_io && item.resilient_io->reopens() > 0) {
            blog(LOG_INFO, "[MEDIA] media server responses ended early %d time(s); "
                 "continued with fresh range requests", item.resilient_io->reopens());
        }
    }

    void markOpenFailure(const char* message)
    {
        blog(LOG_ERROR, "[MEDIA] %s", message);
        std::lock_guard<std::mutex> lock(state_mutex);
        info.ready_to_play = false;
        info.playback_buffer_empty = true;
        info.playback_buffer_full = false;
        info.playback_likely_to_keep_up = false;
        info.rate = 0.0f;
        info.ended = true;
    }

    mutable std::mutex state_mutex;
    std::condition_variable state_cv;
    MediaPlaybackInfo info;
    // The current item, for reopening it after its end (seek()).
    std::string current_location;
    // Converted audio, reused for every packet (worker thread only).
    std::vector<float> audio_left;
    std::vector<float> audio_right;
    bool seek_pending = false;
    double seek_target = 0.0;
    uint64_t seek_generation = 0;
    std::chrono::steady_clock::time_point last_seek_at;
    std::chrono::steady_clock::time_point coarse_preview_after;
    bool scrub_burst = false;
    bool coarse_preview_shown = false;
    double last_seek_target = -1.0;
    double discard_video_before = -1.0;
    double discard_audio_before = -1.0;
    bool preview_pending = false;
    bool resync_on_resume = false;
    // Where the sender said playback is, and when it said so (a play, seek
    // or resume command while playing). See setFollowSenderClock().
    bool anchor_valid = false;
    double anchor_position = 0.0;
    std::chrono::steady_clock::time_point anchor_wall_time;

    bool followsSenderClock(HeldLock /*state_mutex*/) const
    {
        // (An item of unknown length is left as it was: there is no telling
        // how much of it may be skipped.)
        return anchor_valid && info.rate > 0.0f && info.duration_known && info.duration > 0.0 &&
               g_follow_sender_clock.load(std::memory_order_relaxed);
    }
    double senderPositionLocked(HeldLock /*state_mutex*/) const
    {
        return anchor_position + std::chrono::duration<double>(
            std::chrono::steady_clock::now() - anchor_wall_time).count() * info.rate;
    }
    // Frames that ended before this are skipped while a stream catches up.
    // Never past the end of the item: one that is shorter than the time it
    // took to open (a still clip on a slow network, a start close to the end)
    // still shows its last frame, also when that frame's own length isn't
    // known. With no known duration nothing is skipped.
    double catchUpPositionLocked(HeldLock held) const
    {
        if (!info.duration_known || info.duration <= 0.0) {
            return anchor_position;
        }
        constexpr double kKeptAtEnd = 0.25;
        return std::min(senderPositionLocked(held),
                        std::max(anchor_position, info.duration - kKeptAtEnd));
    }
    // A stream is still skipping to the sender's position (see shouldDiscard).
    bool catching_up_video = false;
    bool catching_up_audio = false;
    unsigned int skipped_video_frames = 0;

    bool clock_initialized = false;
    double clock_media_position = 0.0;
    std::chrono::steady_clock::time_point clock_wall_time;

    std::mutex callback_mutex;
    MediaVideoCallback video_callback;
    MediaAudioCallback audio_callback;

    std::atomic<bool> stop_requested{true};
    std::atomic<bool> first_video_logged{false};
    std::atomic<bool> first_audio_logged{false};
    std::atomic<bool> pause_before_first_frame_logged{false};
    std::mutex control_mutex;
    std::thread worker;
    // The current item's fetches, for policyIoOpen (worker thread only).
    media_fetch::Session* fetch_session = nullptr;
    int (*default_io_open)(AVFormatContext*, AVIOContext**, const char*, int,
                           AVDictionary**) = nullptr;
};

MediaPlayer::MediaPlayer()
    : m_impl(std::make_unique<Impl>())
{
}

MediaPlayer::~MediaPlayer() = default;

void MediaPlayer::setVideoCallback(MediaVideoCallback callback)
{
    m_impl->setVideoCallback(std::move(callback));
}

void MediaPlayer::setAudioCallback(MediaAudioCallback callback)
{
    m_impl->setAudioCallback(std::move(callback));
}

void MediaPlayer::play(const std::string& location, double start_position)
{
    m_impl->play(location, start_position);
}

void MediaPlayer::stop()
{
    m_impl->stop();
}

void MediaPlayer::seek(double position)
{
    m_impl->seek(position);
}

void MediaPlayer::setRate(float rate)
{
    m_impl->setRate(rate);
}

float MediaPlayer::pauseForPlaylistRemoval()
{
    return m_impl->pauseForPlaylistRemoval();
}

void MediaPlayer::setAllowInsecureTls(bool allow)
{
    g_allow_insecure_tls.store(allow, std::memory_order_relaxed);
}

void MediaPlayer::setRefusalCallback(MediaRefusalCallback callback)
{
    std::lock_guard<std::mutex> lock(g_refusal_mutex);
    g_refusal_callback = std::move(callback);
}

void MediaPlayer::setFollowSenderClock(bool follow)
{
    g_follow_sender_clock.store(follow, std::memory_order_relaxed);
}

void MediaPlayer::setOpenDelayForTesting(int milliseconds)
{
    g_open_delay_for_testing_ms.store(std::max(0, milliseconds), std::memory_order_relaxed);
}

void MediaPlayer::setSeekDelaysForTesting(int worker_milliseconds, int presenter_milliseconds)
{
    g_seek_worker_delay_for_testing_ms.store(std::max(0, worker_milliseconds),
                                             std::memory_order_relaxed);
    g_seek_presenter_delay_for_testing_ms.store(std::max(0, presenter_milliseconds),
                                                std::memory_order_relaxed);
}

MediaPlaybackInfo MediaPlayer::getPlaybackInfo() const
{
    return m_impl->getPlaybackInfo();
}
