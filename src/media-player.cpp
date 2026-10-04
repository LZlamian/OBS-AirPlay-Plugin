#include "media-player.hpp"
#include "hardware-decode-option.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
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

namespace {

std::string ffmpeg_error(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

std::string safe_location_for_log(std::string location)
{
    // Media URLs frequently carry short-lived authentication tokens. Keep the
    // useful scheme/host/path diagnostic without writing those credentials to
    // the OBS log.
    const size_t scheme = location.find("://");
    if (scheme != std::string::npos) {
        const size_t authority_start = scheme + 3;
        const size_t authority_end = location.find('/', authority_start);
        const size_t at = location.find('@', authority_start);
        if (at != std::string::npos &&
            (authority_end == std::string::npos || at < authority_end)) {
            location.replace(authority_start, at - authority_start + 1,
                             "[redacted]@");
        }
    }
    const size_t sensitive = location.find_first_of("?#");
    if (sensitive != std::string::npos) {
        location.erase(sensitive);
        location += "?[redacted]";
    }
    return location;
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
    // multivariant playlist. Pick one balanced rendition before stream-info
    // analysis; 3 Mbps normally maps to a good 720p starting point.
    constexpr int64_t kTargetBitrate = 3000000;
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

bool starts_with(const std::string& value, const char* prefix)
{
    const size_t length = std::strlen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
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

std::string trim_line(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    const size_t first = line.find_first_not_of(" \t");
    return first == std::string::npos ? std::string() : line.substr(first);
}

std::string hls_attribute(const std::string& attributes, const char* name)
{
    const std::string needle = std::string(name) + "=";
    size_t start = attributes.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    start += needle.size();
    if (start < attributes.size() && attributes[start] == '"') {
        const size_t end = attributes.find('"', start + 1);
        return end == std::string::npos ? std::string()
            : attributes.substr(start + 1, end - start - 1);
    }
    const size_t end = attributes.find(',', start);
    return attributes.substr(start, end == std::string::npos
        ? std::string::npos : end - start);
}

std::string normalize_url_path(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const size_t path_start = url.find('/', scheme + 3);
    if (path_start == std::string::npos) {
        return url;
    }
    const size_t suffix_start = url.find_first_of("?#", path_start);
    const std::string origin = url.substr(0, path_start);
    const std::string path = url.substr(path_start,
        suffix_start == std::string::npos ? std::string::npos : suffix_start - path_start);
    const std::string suffix = suffix_start == std::string::npos
        ? std::string() : url.substr(suffix_start);

    std::vector<std::string> parts;
    size_t cursor = 1;
    while (cursor <= path.size()) {
        const size_t slash = path.find('/', cursor);
        const std::string part = path.substr(cursor,
            slash == std::string::npos ? std::string::npos : slash - cursor);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }

    std::string normalized = origin + "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) normalized += "/";
        normalized += parts[i];
    }
    if (!path.empty() && path.back() == '/' && normalized.back() != '/') {
        normalized += "/";
    }
    return normalized + suffix;
}

std::string resolve_hls_url(const std::string& base, const std::string& reference)
{
    if (is_http_url(reference)) {
        return reference;
    }
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    if (starts_with(reference, "//")) {
        return base.substr(0, scheme) + ":" + reference;
    }
    const size_t authority_end = base.find('/', scheme + 3);
    const std::string origin = authority_end == std::string::npos
        ? base : base.substr(0, authority_end);
    if (!reference.empty() && reference.front() == '/') {
        return normalize_url_path(origin + reference);
    }

    std::string clean_base = base.substr(0, base.find_first_of("?#"));
    const size_t slash = clean_base.rfind('/');
    if (slash == std::string::npos || slash < scheme + 3) {
        return {};
    }
    return normalize_url_path(clean_base.substr(0, slash + 1) + reference);
}

struct HlsVariant {
    std::string uri;
    int64_t bandwidth = 0;
    bool supported_video = false;
};

std::vector<HlsVariant> parse_hls_variants(const std::string& playlist)
{
    std::vector<HlsVariant> variants;
    std::string pending_attributes;
    size_t cursor = 0;
    while (cursor <= playlist.size()) {
        const size_t end = playlist.find('\n', cursor);
        const std::string line = trim_line(playlist.substr(cursor,
            end == std::string::npos ? std::string::npos : end - cursor));
        if (starts_with(line, "#EXT-X-STREAM-INF:")) {
            pending_attributes = line.substr(std::strlen("#EXT-X-STREAM-INF:"));
        } else if (!pending_attributes.empty() && !line.empty() && line.front() != '#') {
            HlsVariant variant;
            variant.uri = line;
            const std::string bandwidth = hls_attribute(pending_attributes, "BANDWIDTH");
            variant.bandwidth = bandwidth.empty() ? 0
                : std::strtoll(bandwidth.c_str(), nullptr, 10);
            const std::string codecs = hls_attribute(pending_attributes, "CODECS");
            variant.supported_video = codecs.empty() ||
                codecs.find("avc1") != std::string::npos ||
                codecs.find("avc3") != std::string::npos ||
                codecs.find("hvc1") != std::string::npos ||
                codecs.find("hev1") != std::string::npos;
            variants.push_back(std::move(variant));
            pending_attributes.clear();
        }
        if (end == std::string::npos) break;
        cursor = end + 1;
    }
    return variants;
}

const HlsVariant* choose_hls_variant(const std::vector<HlsVariant>& variants)
{
    // Prefer quick startup over the largest rendition. This player currently
    // uses one rendition for the session rather than adapting between all of
    // them, so 1.5 Mbps is a practical quality/latency compromise.
    constexpr int64_t kTargetBitrate = 1500000;
    const HlsVariant* best_under = nullptr;
    const HlsVariant* lowest_over = nullptr;
    for (const HlsVariant& variant : variants) {
        if (!variant.supported_video) continue;
        if (variant.bandwidth > 0 && variant.bandwidth <= kTargetBitrate &&
            (!best_under || variant.bandwidth > best_under->bandwidth)) {
            best_under = &variant;
        } else if (variant.bandwidth > kTargetBitrate &&
                   (!lowest_over || variant.bandwidth < lowest_over->bandwidth)) {
            lowest_over = &variant;
        } else if (variant.bandwidth == 0 && !best_under) {
            best_under = &variant;
        }
    }
    return best_under ? best_under : lowest_over;
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
    ResilientHttpIo(std::string url, AVIOInterruptCB interrupt)
        : m_url(std::move(url)), m_interrupt(interrupt)
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
        if (!reopen(0) || m_size <= 0) return false;
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
    int reopens() const { return m_reopens; }

private:
    static constexpr int kMaxAttempts = 5;
    static constexpr int64_t kForwardSkipBytes = 256 * 1024;

    bool interrupted() const
    {
        return m_interrupt.callback && m_interrupt.callback(m_interrupt.opaque);
    }

    bool reopen(int64_t offset)
    {
        if (m_inner) avio_closep(&m_inner);
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", "10000000", 0);
        av_dict_set(&options, "user_agent", "AppleCoreMedia/1.0 OBS-AirPlay/" PLUGIN_VERSION, 0);
        av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
        av_dict_set(&options, "max_redirects", "8", 0);
        av_dict_set_int(&options, "offset", offset, 0);
        AVIOInterruptCB interrupt = m_interrupt;
        const int result = avio_open2(&m_inner, m_url.c_str(), AVIO_FLAG_READ, &interrupt, &options);
        av_dict_free(&options);
        if (result < 0) {
            m_inner = nullptr;
            return false;
        }
        if (m_size <= 0) {
            const int64_t reported = avio_size(m_inner);
            // With an offset the server reports the whole file size.
            m_size = reported > 0 ? reported : -1;
        }
        m_position = offset;
        return true;
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
            if (!self->reopen(self->m_position)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
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
            if (self->reopen(target)) return target;
            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        }
        return AVERROR(EIO);
    }

    std::string m_url;
    AVIOInterruptCB m_interrupt;
    AVIOContext* m_inner = nullptr;
    AVIOContext* m_context = nullptr;
    int64_t m_position = 0;
    int64_t m_size = -1;
    int m_reopens = 0;
};

} // namespace

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

    void play(const std::string& location, double start_position)
    {
        stop();
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            info = {};
            info.position = std::max(0.0, start_position);
            info.rate = 1.0f;
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
            clock_initialized = false;
        }
        stop_requested.store(false, std::memory_order_release);
        worker = std::thread(&Impl::run, this, location, std::max(0.0, start_position));
    }

    void stop()
    {
        stop_requested.store(true, std::memory_order_release);
        state_cv.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        info.rate = 0.0f;
        info.ready_to_play = false;
        info.playback_buffer_empty = true;
        info.playback_buffer_full = false;
        info.playback_likely_to_keep_up = false;
        clock_initialized = false;
    }

    void seek(double position)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
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
        last_seek_target = std::max(0.0, position);
        seek_target = std::max(0.0, position);
        seek_pending = true;
        ++seek_generation;
        info.position = seek_target;
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
        // Safari normally sends only 0 (pause) and 1 (play). Positive values
        // are still accepted so protocol diagnostics remain honest.
        info.rate = rate > 0.0f ? rate : 0.0f;
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
        av_dict_set(&options, "max_redirects", "8", 0);
        av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
        av_dict_set(&options, "user_agent", "AppleCoreMedia/1.0 OBS-AirPlay/" PLUGIN_VERSION, 0);
        const int open_result = avio_open2(&io, location.c_str(), AVIO_FLAG_READ,
                                           &interrupt, &options);
        av_dict_free(&options);
        if (open_result < 0 || !io) {
            if (!stop_requested.load(std::memory_order_acquire)) {
                blog(LOG_WARNING, "[MEDIA] HLS manifest preflight failed: %s",
                     ffmpeg_error(open_result).c_str());
            }
            if (io) avio_closep(&io);
            return location;
        }

        std::string final_location = location;
        uint8_t* redirected_location = nullptr;
        if (av_opt_get(io, "location", AV_OPT_SEARCH_CHILDREN,
                       &redirected_location) >= 0 && redirected_location) {
            const std::string candidate(reinterpret_cast<char*>(redirected_location));
            if (is_http_url(candidate)) {
                final_location = candidate;
            }
            av_free(redirected_location);
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

        const std::vector<HlsVariant> variants = parse_hls_variants(playlist);
        const HlsVariant* selected = choose_hls_variant(variants);
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (!selected) {
            blog(LOG_INFO, "[MEDIA] HLS media playlist preflight completed in %.1fms",
                 elapsed_ms);
            return location;
        }

        const std::string resolved = resolve_hls_url(final_location, selected->uri);
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
        bool hardware = false;        // the decoder was opened for hardware
        int hardware_errors = 0;      // consecutive, since the last picture
        bool reopen_in_software = false;
        bool mode_logged = false;
    };

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
    bool shouldDiscard(double position, bool is_video)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        // Frames still in the decoder from before a pending seek are stale.
        if (seek_pending) return true;
        double& discard_before = is_video ? discard_video_before : discard_audio_before;
        if (discard_before < 0.0) return false;
        constexpr double kFrameTolerance = 0.020;
        if (position + kFrameTolerance < discard_before) return true;
        discard_before = -1.0;
        return false;
    }

    bool waitForPresentation(double position, uint64_t* timestamp_ns, bool is_video)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        std::chrono::steady_clock::time_point target;
        for (;;) {
            if (stop_requested.load(std::memory_order_acquire) || seek_pending) {
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
                state_cv.wait(lock);
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!clock_initialized) {
                clock_initialized = true;
                clock_media_position = position;
                clock_wall_time = now;
            }
            // Recomputed on every wake-up: setRate() re-anchors the clock at
            // the last presented frame, so rate changes apply immediately
            // and never collapse the gap to a sparse next frame.
            const double delta = std::max(0.0, position - clock_media_position) /
                                 std::max(0.001f, info.rate);
            target = clock_wall_time +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(delta));
            if (now >= target) {
                break;
            }
            state_cv.wait_until(lock, target);
        }

        info.position = std::max(info.position, position);
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

    bool handlePendingSeek(AVFormatContext* format, AVCodecContext* video_decoder,
                           AVCodecContext* audio_decoder)
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
        }

        const int64_t timestamp = static_cast<int64_t>(target * AV_TIME_BASE);
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
        if (position < 0.0 || shouldDiscard(position, true)) {
            return;
        }

        AVFrame* picture = decoded;
        if (hardware_frame) {
            av_frame_unref(out->transfer);
            if (av_hwframe_transfer_data(out->transfer, decoded, 0) < 0) {
                noteHardwareError(out, "could not read the decoded picture");
                return;
            }
            out->transfer->color_range = decoded->color_range;
            out->transfer->colorspace = decoded->colorspace;
            picture = out->transfer;
        }
        out->hardware_errors = 0;

        // NV12 (hardware) and I420 go to OBS as they are, in the stream's
        // own range; everything else (10-bit, 4:2:2, ...) is converted.
        const bool nv12 = picture->format == AV_PIX_FMT_NV12;
        const bool i420 = picture->format == AV_PIX_FMT_YUV420P ||
                          picture->format == AV_PIX_FMT_YUVJ420P;
        bool full_range = picture->color_range == AVCOL_RANGE_JPEG ||
                          picture->format == AV_PIX_FMT_YUVJ420P;
        const bool bt601 = picture->colorspace == AVCOL_SPC_SMPTE170M ||
                           picture->colorspace == AVCOL_SPC_BT470BG;
        if (!out->mode_logged) {
            out->mode_logged = true;
            blog(LOG_INFO, "[MEDIA] video %dx%d: %s decoding (%s, %s range%s)",
                 picture->width, picture->height,
                 hardware_frame ? "VideoToolbox hardware" : "software",
                 av_get_pix_fmt_name(static_cast<AVPixelFormat>(picture->format)),
                 full_range ? "full" : "video",
                 out->turns ? ", rotated upright" : "");
        }
        if (!nv12 && !i420) {
            // swscale converts full-range (YUVJ) layouts to video range and
            // leaves the range of everything else untouched.
            const AVPixFmtDescriptor* descriptor =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(picture->format));
            if (descriptor && std::strncmp(descriptor->name, "yuvj", 4) == 0) {
                full_range = false;
            }
            out->scaler = sws_getCachedContext(
                out->scaler, picture->width, picture->height,
                static_cast<AVPixelFormat>(picture->format),
                picture->width, picture->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!out->scaler) {
                blog(LOG_ERROR, "[MEDIA] failed to create video converter");
                return;
            }
            AVFrame* i420_frame = out->i420;
            if (i420_frame->width != picture->width || i420_frame->height != picture->height ||
                i420_frame->format != AV_PIX_FMT_YUV420P) {
                av_frame_unref(i420_frame);
                i420_frame->format = AV_PIX_FMT_YUV420P;
                i420_frame->width = picture->width;
                i420_frame->height = picture->height;
                if (av_frame_get_buffer(i420_frame, 32) < 0) {
                    blog(LOG_ERROR, "[MEDIA] failed to allocate converted video frame");
                    return;
                }
            }
            if (av_frame_make_writable(i420_frame) < 0) {
                return;
            }
            sws_scale(out->scaler, picture->data, picture->linesize, 0, picture->height,
                      i420_frame->data, i420_frame->linesize);
            picture = i420_frame;
        }

        if (out->turns) {
            const int width = picture->width;
            const int height = picture->height;
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
                    blog(LOG_ERROR, "[MEDIA] failed to allocate rotated video frame");
                    return;
                }
            }
            if (av_frame_make_writable(rotated) < 0) {
                return;
            }
            const int chroma_width = (width + 1) / 2;
            const int chroma_height = (height + 1) / 2;
            rotate_plane(picture->data[0], picture->linesize[0], width, height, 1,
                         out->turns, rotated->data[0], rotated->linesize[0]);
            if (nv12) {
                rotate_plane(picture->data[1], picture->linesize[1], chroma_width,
                             chroma_height, 2, out->turns, rotated->data[1],
                             rotated->linesize[1]);
            } else {
                for (int plane = 1; plane <= 2; ++plane) {
                    rotate_plane(picture->data[plane], picture->linesize[plane],
                                 chroma_width, chroma_height, 1, out->turns,
                                 rotated->data[plane], rotated->linesize[plane]);
                }
            }
            picture = rotated;
        }

        uint64_t timestamp_ns = 0;
        if (!waitForPresentation(position, &timestamp_ns, true)) {
            return;
        }

        MediaVideoCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = video_callback;
        }
        if (callback) {
            MediaVideoFrame frame;
            frame.width = picture->width;
            frame.height = picture->height;
            frame.timestamp_ns = timestamp_ns;
            frame.nv12 = nv12;
            frame.full_range = full_range;
            frame.bt601 = bt601;
            for (int i = 0; i < (nv12 ? 2 : 3); ++i) {
                frame.data[i] = picture->data[i];
                frame.linesize[i] = picture->linesize[i];
            }
            callback(frame);
        }

        if (!first_video_logged.exchange(true)) {
            blog(LOG_INFO, "[MEDIA] first decoded video frame (%dx%d, %.3fs)",
                 picture->width, picture->height, position);
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
        std::vector<float> left(static_cast<size_t>(capacity));
        std::vector<float> right(static_cast<size_t>(capacity));
        uint8_t* output[2] = {
            reinterpret_cast<uint8_t*>(left.data()),
            reinterpret_cast<uint8_t*>(right.data())
        };
        const int converted = swr_convert(
            *resampler, output, capacity,
            const_cast<const uint8_t**>(decoded->extended_data), decoded->nb_samples);
        if (converted <= 0) {
            return;
        }

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
            frame.data[0] = left.data();
            frame.data[1] = right.data();
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
                emitVideo(decoded, stream, format_start_seconds, video_output);
            } else {
                emitAudio(decoded, stream, decoder, format_start_seconds,
                          resampler, input_layout, input_rate, input_format);
            }
            av_frame_unref(decoded);
        }
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
        const std::string playback_location = resolveHlsVariant(location);
        if (stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        const auto open_started = std::chrono::steady_clock::now();
        AVFormatContext* format = avformat_alloc_context();
        if (!format) {
            markOpenFailure("could not allocate demuxer");
            return;
        }
        format->interrupt_callback.callback = &Impl::interruptCallback;
        format->interrupt_callback.opaque = this;
        if (looks_like_hls_url(playback_location)) {
            // These limits are consulted while the HLS demuxer opens its
            // first transport-stream segment, not only by find_stream_info().
            format->probesize = 384 * 1024;
            format->max_analyze_duration = 2 * AV_TIME_BASE;
            format->max_probe_packets = 256;
            format->skip_estimate_duration_from_pts = 1;
        }

        // Progressive HTTP media goes through the byte-exact input layer;
        // HLS and unsized responses keep FFmpeg's own HTTP input.
        std::unique_ptr<ResilientHttpIo> resilient_io;
        if (is_http_url(playback_location) && !looks_like_hls_url(playback_location)) {
            resilient_io = std::make_unique<ResilientHttpIo>(
                playback_location, AVIOInterruptCB{&Impl::interruptCallback, this});
            if (resilient_io->open()) {
                format->pb = resilient_io->context();
                format->flags |= AVFMT_FLAG_CUSTOM_IO;
            } else {
                resilient_io.reset();
            }
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", "10000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "2", 0);
        av_dict_set(&options, "user_agent", "AppleCoreMedia/1.0 OBS-AirPlay/" PLUGIN_VERSION, 0);
        if (is_http_url(playback_location)) {
            av_dict_set(&options, "max_redirects", "8", 0);
            av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
        }
        int result = avformat_open_input(&format, playback_location.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (result < 0) {
            if (!stop_requested.load()) {
                markOpenFailure(("URL open failed: " + ffmpeg_error(result)).c_str());
            }
            avformat_free_context(format);
            return;
        }

        const auto transport_opened = std::chrono::steady_clock::now();
        const double transport_ms = std::chrono::duration<double, std::milli>(
            transport_opened - open_started).count();
        blog(LOG_INFO, "[MEDIA] transport opened in %.1fms (streams=%u, programs=%u)",
             transport_ms, format->nb_streams, format->nb_programs);

        int64_t selected_bitrate = 0;
        AVProgram* selected_program = select_hls_program(format, &selected_bitrate);
        if (selected_program) {
            // Bound HLS analysis so startup does not download multiple full
            // segments merely to estimate properties already in the playlist.
            format->probesize = 512 * 1024;
            format->max_analyze_duration = 2 * AV_TIME_BASE;
            format->max_probe_packets = 256;
            blog(LOG_INFO,
                 "[MEDIA] selected one of %u HLS variants (bandwidth=%lld bps)",
                 format->nb_programs, static_cast<long long>(selected_bitrate));
        }

        result = avformat_find_stream_info(format, nullptr);
        if (result < 0) {
            markOpenFailure(("stream discovery failed: " + ffmpeg_error(result)).c_str());
            avformat_close_input(&format);
            return;
        }

        const int video_index = selected_program
            ? find_program_stream(format, selected_program, AVMEDIA_TYPE_VIDEO)
            : av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        const int audio_index = selected_program
            ? find_program_stream(format, selected_program, AVMEDIA_TYPE_AUDIO)
            : av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (video_index < 0 && audio_index < 0) {
            markOpenFailure("URL contains no decodable audio or video streams");
            avformat_close_input(&format);
            return;
        }

        AVCodecContext* video_decoder = nullptr;
        AVCodecContext* audio_decoder = nullptr;
        VideoOutput video_output;
        if (video_index >= 0 &&
            !openDecoder(format, video_index, &video_decoder, true, &video_output.hardware)) {
            blog(LOG_WARNING, "[MEDIA] video stream will be skipped");
        }
        if (video_decoder) {
            video_output.turns = display_quarter_turns(format->streams[video_index]);
        }
        if (audio_index >= 0 && !openDecoder(format, audio_index, &audio_decoder)) {
            blog(LOG_WARNING, "[MEDIA] audio stream will be skipped");
        }
        if (!video_decoder && !audio_decoder) {
            markOpenFailure("no advertised stream could be decoded");
            avformat_close_input(&format);
            return;
        }

        const bool duration_known = format->duration != AV_NOPTS_VALUE &&
                                    format->duration > 0;
        const double duration = duration_known
            ? format->duration / static_cast<double>(AV_TIME_BASE) : 0.0;
        const double format_start_seconds = format->start_time != AV_NOPTS_VALUE
            ? format->start_time / static_cast<double>(AV_TIME_BASE) : 0.0;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
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

        if (start_position > 0.0) {
            const int64_t target = static_cast<int64_t>(start_position * AV_TIME_BASE);
            if (av_seek_frame(format, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
                blog(LOG_WARNING, "[MEDIA] initial seek to %.3fs was not available", start_position);
            }
        }

        AVPacket* packet = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        video_output.i420 = av_frame_alloc();
        video_output.transfer = av_frame_alloc();
        video_output.rotated = av_frame_alloc();
        SwrContext* resampler = nullptr;
        AVChannelLayout input_layout = {};
        int input_rate = 0;
        AVSampleFormat input_format = AV_SAMPLE_FMT_NONE;

        // Some sender media servers (iOS AVPlayer's among them) end an HTTP
        // response early or close a kept-alive connection, which FFmpeg
        // reports as end of file right after a seek.  A read that ends well
        // before the known duration re-seeks (a fresh request) instead.
        constexpr int kMaxReadRecoveries = 3;
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
            if (handlePendingSeek(format, video_decoder, audio_decoder)) {
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
                    if (info.position > position_at_recovery + 0.5 ||
                        seek_generation != seek_generation_at_recovery) {
                        read_recoveries = 0;
                    }
                    seek_generation_at_recovery = seek_generation;
                    if (seek_pending) {
                        retry = true;  // the pending seek repositions the demuxer
                    } else if (!io_at_end && duration_known &&
                               info.position < duration - 1.0 &&
                               !(demuxed_generation == seek_generation &&
                                 demuxed_until >= duration - 0.1) &&
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

            if (video_output.reopen_in_software) {
                // Hardware decoding failed or does not take this stream:
                // decode the rest of the item in software, continuing from
                // the picture on screen.
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
            std::lock_guard<std::mutex> lock(state_mutex);
            info.ended = true;
            info.rate = 0.0f;
            info.playback_buffer_empty = true;
            info.playback_buffer_full = false;
            blog(LOG_INFO, "[MEDIA] playback reached end of stream at %.3fs", info.position);
        }

        av_channel_layout_uninit(&input_layout);
        swr_free(&resampler);
        if (video_output.scaler) sws_freeContext(video_output.scaler);
        av_frame_free(&video_output.rotated);
        av_frame_free(&video_output.transfer);
        av_frame_free(&video_output.i420);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&audio_decoder);
        avcodec_free_context(&video_decoder);
        avformat_close_input(&format);
        if (resilient_io && resilient_io->reopens() > 0) {
            blog(LOG_INFO, "[MEDIA] media server responses ended early %d time(s); "
                 "continued with fresh range requests", resilient_io->reopens());
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
    bool seek_pending = false;
    double seek_target = 0.0;
    uint64_t seek_generation = 0;
    double last_seek_target = -1.0;
    double discard_video_before = -1.0;
    double discard_audio_before = -1.0;
    bool preview_pending = false;
    bool resync_on_resume = false;
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
    std::thread worker;
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

MediaPlaybackInfo MediaPlayer::getPlaybackInfo() const
{
    return m_impl->getPlaybackInfo();
}
