#include "media-player.hpp"

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
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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
        seek_target = std::max(0.0, position);
        seek_pending = true;
        info.position = seek_target;
        clock_initialized = false;
        state_cv.notify_all();
    }

    void setRate(float rate)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        // Safari normally sends only 0 (pause) and 1 (play). Positive values
        // are still accepted so protocol diagnostics remain honest.
        info.rate = rate > 0.0f ? rate : 0.0f;
        clock_initialized = false;
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

    bool openDecoder(AVFormatContext* format, int stream_index, AVCodecContext** decoder)
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

    bool waitForPresentation(double position, uint64_t* timestamp_ns)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        while (!stop_requested.load(std::memory_order_acquire) && info.rate <= 0.0f) {
            if (!pause_before_first_frame_logged.exchange(true)) {
                blog(LOG_INFO,
                     "[MEDIA] Safari paused before the first frame; waiting for rate=1");
            }
            state_cv.wait(lock);
        }
        if (stop_requested.load(std::memory_order_acquire) || seek_pending) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!clock_initialized) {
            clock_initialized = true;
            clock_media_position = position;
            clock_wall_time = now;
        }
        const double delta = std::max(0.0, position - clock_media_position) /
                             std::max(0.001f, info.rate);
        const auto target = clock_wall_time +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(delta));

        while (!stop_requested.load(std::memory_order_acquire) && !seek_pending &&
               info.rate > 0.0f && std::chrono::steady_clock::now() < target) {
            state_cv.wait_until(lock, target);
        }
        if (stop_requested.load(std::memory_order_acquire) || seek_pending || info.rate <= 0.0f) {
            return false;
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
                   SwsContext** scaler, AVFrame* i420)
    {
        const double position = timestamp_seconds(decoded, stream, format_start_seconds);
        if (position < 0.0) {
            return;
        }

        AVFrame* output = decoded;
        if (decoded->format != AV_PIX_FMT_YUV420P) {
            *scaler = sws_getCachedContext(
                *scaler, decoded->width, decoded->height,
                static_cast<AVPixelFormat>(decoded->format),
                decoded->width, decoded->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!*scaler) {
                blog(LOG_ERROR, "[MEDIA] failed to create video converter");
                return;
            }
            if (i420->width != decoded->width || i420->height != decoded->height ||
                i420->format != AV_PIX_FMT_YUV420P) {
                av_frame_unref(i420);
                i420->format = AV_PIX_FMT_YUV420P;
                i420->width = decoded->width;
                i420->height = decoded->height;
                if (av_frame_get_buffer(i420, 32) < 0) {
                    blog(LOG_ERROR, "[MEDIA] failed to allocate converted video frame");
                    return;
                }
            }
            if (av_frame_make_writable(i420) < 0) {
                return;
            }
            sws_scale(*scaler, decoded->data, decoded->linesize, 0, decoded->height,
                      i420->data, i420->linesize);
            output = i420;
        }

        uint64_t timestamp_ns = 0;
        if (!waitForPresentation(position, &timestamp_ns)) {
            return;
        }

        MediaVideoCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = video_callback;
        }
        if (callback) {
            MediaVideoFrame frame;
            frame.width = output->width;
            frame.height = output->height;
            frame.timestamp_ns = timestamp_ns;
            for (int i = 0; i < 3; ++i) {
                frame.data[i] = output->data[i];
                frame.linesize[i] = output->linesize[i];
            }
            callback(frame);
        }

        if (!first_video_logged.exchange(true)) {
            blog(LOG_INFO, "[MEDIA] first decoded video frame (%dx%d, %.3fs)",
                 output->width, output->height, position);
        }
    }

    void emitAudio(AVFrame* decoded, AVStream* stream, AVCodecContext* decoder,
                   double format_start_seconds, SwrContext** resampler,
                   AVChannelLayout* input_layout, int* input_rate,
                   AVSampleFormat* input_format)
    {
        const double position = timestamp_seconds(decoded, stream, format_start_seconds);
        if (position < 0.0) {
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
        if (!waitForPresentation(position, &timestamp_ns)) {
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
                      double format_start_seconds, SwsContext** scaler,
                      AVFrame* i420, SwrContext** resampler,
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
                break;
            }
            if (video) {
                emitVideo(decoded, stream, format_start_seconds, scaler, i420);
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
        if (video_index >= 0 && !openDecoder(format, video_index, &video_decoder)) {
            blog(LOG_WARNING, "[MEDIA] video stream will be skipped");
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
        AVFrame* i420 = av_frame_alloc();
        SwsContext* scaler = nullptr;
        SwrContext* resampler = nullptr;
        AVChannelLayout input_layout = {};
        int input_rate = 0;
        AVSampleFormat input_format = AV_SAMPLE_FMT_NONE;

        while (!stop_requested.load(std::memory_order_acquire)) {
            if (handlePendingSeek(format, video_decoder, audio_decoder)) {
                swr_free(&resampler);
                av_channel_layout_uninit(&input_layout);
                input_rate = 0;
                input_format = AV_SAMPLE_FMT_NONE;
            }

            result = av_read_frame(format, packet);
            if (result < 0) {
                if (result != AVERROR_EOF && !stop_requested.load()) {
                    blog(LOG_WARNING, "[MEDIA] media read ended: %s", ffmpeg_error(result).c_str());
                }
                break;
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

            if (decoder && avcodec_send_packet(decoder, packet) >= 0) {
                drainDecoder(decoder, stream, is_video, format_start_seconds,
                             &scaler, i420, &resampler, &input_layout,
                             &input_rate, &input_format, decoded);
            }
            av_packet_unref(packet);
        }

        if (!stop_requested.load(std::memory_order_acquire)) {
            if (video_decoder) {
                avcodec_send_packet(video_decoder, nullptr);
                drainDecoder(video_decoder, format->streams[video_index], true,
                             format_start_seconds, &scaler, i420, &resampler,
                             &input_layout, &input_rate, &input_format, decoded);
            }
            if (audio_decoder) {
                avcodec_send_packet(audio_decoder, nullptr);
                drainDecoder(audio_decoder, format->streams[audio_index], false,
                             format_start_seconds, &scaler, i420, &resampler,
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
        if (scaler) sws_freeContext(scaler);
        av_frame_free(&i420);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&audio_decoder);
        avcodec_free_context(&video_decoder);
        avformat_close_input(&format);
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
