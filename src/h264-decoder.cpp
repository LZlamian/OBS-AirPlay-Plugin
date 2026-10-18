// SPDX-License-Identifier: GPL-3.0-or-later
#include "h264-decoder.hpp"
#include "hardware-decode-option.hpp"
#include "video-warmup-data.h"
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

namespace {

// After this many consecutive hardware errors the decoder is reopened in
// software for the rest of the stream.
constexpr int kMaxHardwareErrors = 3;
// "Invalid data" is what a damaged or keyframe-less stream produces too, and
// software decoding cannot show that either: allow about a second of it
// before trying software.
constexpr int kMaxStreamErrors = 30;
// At most one "cannot decode" log line per this many consecutive errors.
constexpr int kErrorLogInterval = 120;

bool is_parameter_set(AVCodecID codec_id, uint8_t header)
{
    if (codec_id == AV_CODEC_ID_HEVC) {
        const int type = (header >> 1) & 0x3f;
        return type >= 32 && type <= 34; // VPS, SPS, PPS
    }
    const int type = header & 0x1f;
    return type == 7 || type == 8; // SPS, PPS
}

int nal_type(AVCodecID codec_id, uint8_t header)
{
    return codec_id == AV_CODEC_ID_HEVC ? ((header >> 1) & 0x3f) : (header & 0x1f);
}

// Splits an Annex B payload into access units (offset, length). A new unit
// starts at a picture's first slice, or at a parameter set, SEI or access
// unit delimiter, once the current unit has a slice. iOS sends one unit per
// payload; FFmpeg finishes a picture only at the end of a packet, so a
// second unit in the same packet would come out one packet late.
std::vector<std::pair<size_t, size_t>> split_access_units(AVCodecID codec_id, const uint8_t* data,
                                                          size_t size)
{
    std::vector<std::pair<size_t, size_t>> units;
    size_t unit_start = 0;
    bool unit_has_slice = false;
    const bool hevc = codec_id == AV_CODEC_ID_HEVC;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
        const size_t code = i > 0 && data[i - 1] == 0 ? i - 1 : i;
        const size_t header = i + 3;
        const size_t first_payload = header + (hevc ? 2 : 1);
        if (first_payload >= size) break;
        const int type = nal_type(codec_id, data[header]);
        const bool slice = hevc ? type < 32 : (type >= 1 && type <= 5);
        // first_mb_in_slice == 0 / first_slice_segment_in_pic_flag: the top bit.
        const bool first_slice = slice && (data[first_payload] & 0x80);
        const bool starts_unit = hevc ? (type >= 32 && type <= 40) : (type >= 6 && type <= 9);
        if (unit_has_slice && (first_slice || starts_unit)) {
            units.emplace_back(unit_start, code - unit_start);
            unit_start = code;
            unit_has_slice = false;
        }
        unit_has_slice = unit_has_slice || slice;
        i = header;
    }
    units.emplace_back(unit_start, size - unit_start);
    return units;
}

bool is_bt601(AVColorSpace space)
{
    return space == AVCOL_SPC_BT470BG || space == AVCOL_SPC_SMPTE170M;
}

bool is_slice(AVCodecID codec_id, uint8_t header)
{
    if (codec_id == AV_CODEC_ID_HEVC) {
        return ((header >> 1) & 0x3f) < 32;
    }
    const int type = header & 0x1f;
    return type >= 1 && type <= 5;
}

} // namespace

double warm_up_video_decoding()
{
    const uint64_t started = os_gettime_ns();
    struct Canned {
        AVCodecID codec;
        const uint8_t* data;
        size_t size;
    };
    const Canned canned[] = {
        {AV_CODEC_ID_H264, warmup::kH264Keyframe, sizeof(warmup::kH264Keyframe)},
        {AV_CODEC_ID_HEVC, warmup::kHevcKeyframe, sizeof(warmup::kHevcKeyframe)},
    };
    for (const Canned& keyframe : canned) {
        H264Decoder decoder(keyframe.codec, H264Decoder::Mode::Auto);
        DecodedVideoFrame frame;
        decoder.decode(keyframe.data, keyframe.size, frame);
        decoder.flush();
    }
    return (os_gettime_ns() - started) / 1e6;
}

H264Decoder::H264Decoder(AVCodecID codec_id, Mode mode)
    : m_codec_id(codec_id)
    , m_hardware_preferred(mode == Mode::Auto && hardware_decode_requested())
    , m_hardware_allowed(m_hardware_preferred)
    , m_codec_context(nullptr)
    , m_frame(av_frame_alloc())
    , m_frame_next(av_frame_alloc())
    , m_frame_transfer(av_frame_alloc())
    , m_frame_i420(av_frame_alloc())
    , m_packet(av_packet_alloc())
    , m_sws_context(nullptr)
{
    if (!reopen()) {
        return;
    }
    blog(LOG_INFO, "Video decoder initialized for codec id %d (%s)",
         static_cast<int>(codec_id),
         m_hardware_open ? "VideoToolbox hardware decoding, software fallback"
                         : "software decoding");
}

H264Decoder::~H264Decoder()
{
    close();
    if (m_sws_context) {
        sws_freeContext(m_sws_context);
    }
    av_frame_free(&m_frame);
    av_frame_free(&m_frame_next);
    av_frame_free(&m_frame_transfer);
    av_frame_free(&m_frame_i420);
    av_packet_free(&m_packet);
}

enum AVPixelFormat H264Decoder::chooseFormat(AVCodecContext* context,
                                             const enum AVPixelFormat* formats)
{
    auto* self = static_cast<H264Decoder*>(context->opaque);
    const enum AVPixelFormat* software = nullptr;
    for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VIDEOTOOLBOX && self && self->m_hardware_open) {
            return *format;
        }
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
        if (!software && descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            software = format;
        }
    }
    // FFmpeg offers only software formats when the hardware decoder cannot
    // take this stream. This context has no decoder threads, so decode() then
    // reopens a proper software decoder for the stream.
    if (self && self->m_hardware_open) {
        self->m_hardware_refused = true;
    }
    return software ? *software : formats[0];
}

bool H264Decoder::open(bool allow_hardware)
{
    const AVCodec* codec = avcodec_find_decoder(m_codec_id);
    if (!codec) {
        blog(LOG_ERROR, "Video codec not found (%d)", static_cast<int>(m_codec_id));
        return false;
    }

    m_codec_context = avcodec_alloc_context3(codec);
    if (!m_codec_context) {
        blog(LOG_ERROR, "Failed to allocate codec context");
        return false;
    }

    // Low-latency tuning for live AirPlay mirroring:
    //  - LOW_DELAY  : decoder will not buffer frames waiting for B-frame reorder
    //  - FAST       : allow non-spec-compliant speedups
    //  - SLICE threading keeps per-frame latency minimal (FRAME threading
    //    would add 1+ frame of pipeline delay). Mirror streams have few
    //    slices per picture, so more threads than that only sit idle (one
    //    per core by default).
    m_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codec_context->thread_type = FF_THREAD_SLICE;
    m_codec_context->thread_count = 4;
    m_codec_context->opaque = this;

    m_hardware_open = false;
    m_hardware_refused = false;
    if (allow_hardware) {
        // Fails (and leaves software decoding) when the linked FFmpeg was
        // built without VideoToolbox.
        const int result = av_hwdevice_ctx_create(&m_hw_device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                                  nullptr, nullptr, 0);
        if (result >= 0 && m_hw_device) {
            m_codec_context->hw_device_ctx = av_buffer_ref(m_hw_device);
            m_codec_context->get_format = &H264Decoder::chooseFormat;
            // The hardware decoder does the work; no decoder threads needed.
            m_codec_context->thread_count = 1;
            m_hardware_open = m_codec_context->hw_device_ctx != nullptr;
        } else {
            blog(LOG_INFO, "VideoToolbox hardware decoding unavailable (%d); using software",
                 result);
        }
    }

    const int opened = avcodec_open2(m_codec_context, codec, nullptr);
    if (opened < 0) {
        blog(LOG_ERROR, "Failed to open codec");
        close();
        return false;
    }
    m_hardware_errors = 0;
    return true;
}

// Opens a decoder for a new stream: hardware when wanted, software when the
// hardware decoder cannot be opened.
bool H264Decoder::reopen()
{
    close();
    m_hardware_allowed = m_hardware_preferred;
    m_parameter_set_units.clear();
    m_parameter_sets.clear();
    m_hardware_errors = 0;
    m_stream_errors = 0;
    m_errors_logged = 0;
    if (open(m_hardware_allowed)) {
        return true;
    }
    if (!m_hardware_allowed) {
        return false;
    }
    m_hardware_allowed = false;
    return open(false);
}

// Counts a decode error; true when hardware decoding should be given up.
bool H264Decoder::noteError(int error)
{
    if ((m_errors_logged++ % kErrorLogInterval) == 0) {
        blog(LOG_ERROR, "Error decoding mirror video (%d)", error);
    }
    if (!m_hardware_open) {
        return false;
    }
    if (error == AVERROR_INVALIDDATA) {
        return ++m_stream_errors >= kMaxStreamErrors;
    }
    return ++m_hardware_errors >= kMaxHardwareErrors;
}

void H264Decoder::close()
{
    releaseLockedBuffer();
    if (m_frame) av_frame_unref(m_frame);
    if (m_frame_next) av_frame_unref(m_frame_next);
    if (m_frame_transfer) av_frame_unref(m_frame_transfer);
    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }
    if (m_hw_device) {
        av_buffer_unref(&m_hw_device);
    }
    m_hardware_open = false;
}

void H264Decoder::releaseLockedBuffer()
{
#if defined(__APPLE__)
    if (m_locked_buffer) {
        auto buffer = static_cast<CVPixelBufferRef>(m_locked_buffer);
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(buffer);
        m_locked_buffer = nullptr;
    }
#endif
}

void H264Decoder::releaseFrame()
{
    releaseLockedBuffer();
}

void H264Decoder::flush()
{
    releaseLockedBuffer();
    // A new stream sends its own parameter sets.
    m_parameter_set_units.clear();
    m_parameter_sets.clear();
    // A flush marks a stream boundary. A software fallback applies to the
    // stream that caused it; the next stream starts in hardware again.
    // The same applies to a decoder that could not be reopened earlier.
    if (!m_codec_context || (m_hardware_preferred && !m_hardware_allowed)) {
        if (reopen() && m_hardware_open) {
            blog(LOG_INFO, "[DECODE] new stream: hardware decoding enabled again");
        }
        return;
    }
    m_hardware_errors = 0;
    m_stream_errors = 0;
    m_errors_logged = 0;
    if (m_codec_context)
        avcodec_flush_buffers(m_codec_context);
    if (m_frame)
        av_frame_unref(m_frame);
    if (m_frame_transfer)
        av_frame_unref(m_frame_transfer);
}

// Keeps the stream's most recent SPS/PPS(/VPS) so a decoder opened later
// (software fallback) can be configured without waiting for the sender to
// repeat them. Parameter sets precede the slices of an access unit.
void H264Decoder::rememberParameterSets(const uint8_t* data, size_t size)
{
    bool changed = false;
    size_t position = 0;
    const auto next_start = [&](size_t from, size_t* code_length) -> size_t {
        for (size_t i = from; i + 3 <= size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0) {
                if (data[i + 2] == 1) {
                    *code_length = 3;
                    return i;
                }
                if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
                    *code_length = 4;
                    return i;
                }
            }
        }
        return size;
    };

    size_t code_length = 0;
    position = next_start(0, &code_length);
    while (position < size) {
        const size_t nal_start = position + code_length;
        if (nal_start >= size) break;
        const uint8_t header = data[nal_start];
        if (is_slice(m_codec_id, header)) {
            // Checked before looking for the unit's end: that search would
            // read through the whole picture, on every frame.
            break;
        }
        size_t next_length = 0;
        const size_t nal_end = next_start(nal_start, &next_length);
        if (is_parameter_set(m_codec_id, header)) {
            std::vector<uint8_t>& unit = m_parameter_set_units[nal_type(m_codec_id, header)];
            if (unit.size() != nal_end - nal_start ||
                !std::equal(unit.begin(), unit.end(), data + nal_start)) {
                unit.assign(data + nal_start, data + nal_end);
                changed = true;
            }
        }
        position = nal_end;
        code_length = next_length;
    }
    if (changed) {
        // VPS, SPS, PPS: ascending NAL type is decoding order for both codecs.
        static const uint8_t start_code[] = {0, 0, 0, 1};
        m_parameter_sets.clear();
        for (const auto& entry : m_parameter_set_units) {
            m_parameter_sets.insert(m_parameter_sets.end(), start_code,
                                    start_code + sizeof(start_code));
            m_parameter_sets.insert(m_parameter_sets.end(), entry.second.begin(),
                                    entry.second.end());
        }
    }
}

bool H264Decoder::fallBackToSoftware(const char* reason)
{
    blog(LOG_WARNING, "[DECODE] hardware decoding stopped (%s); continuing in software",
         reason);
    close();
    m_hardware_allowed = false;
    m_hardware_errors = 0;
    m_stream_errors = 0;
    if (!open(false)) {
        blog(LOG_ERROR, "[DECODE] software decoder could not be opened; "
                        "retrying at the next stream");
        return false;
    }
    if (!m_parameter_sets.empty()) {
        av_packet_unref(m_packet);
        if (av_new_packet(m_packet, static_cast<int>(m_parameter_sets.size())) >= 0) {
            memcpy(m_packet->data, m_parameter_sets.data(), m_parameter_sets.size());
            if (avcodec_send_packet(m_codec_context, m_packet) >= 0) {
                while (avcodec_receive_frame(m_codec_context, m_frame) >= 0) {
                    av_frame_unref(m_frame);
                }
            }
        }
    }
    return true;
}

void H264Decoder::noteMode(bool hardware, int width, int height, bool nv12, bool full_range)
{
    m_last_frame_hardware = hardware;
    if (m_mode_logged && m_logged_hardware == hardware && m_logged_width == width &&
        m_logged_height == height && m_logged_full_range == full_range) {
        return;
    }
    m_mode_logged = true;
    m_logged_hardware = hardware;
    m_logged_width = width;
    m_logged_height = height;
    m_logged_full_range = full_range;
    blog(LOG_INFO, "[DECODE] mirror video %dx%d: %s (%s, %s range)", width, height,
         hardware ? "VideoToolbox hardware decoding" : "software decoding",
         nv12 ? "NV12" : "I420", full_range ? "full" : "video");
}

bool H264Decoder::outputSoftwareFrame(AVFrame* frame, DecodedVideoFrame& out_frame)
{
    AVFrame* src = frame;
    const bool nv12 = frame->format == AV_PIX_FMT_NV12;
    // Mirror streams are full range, which FFmpeg's software decoders report
    // as YUVJ420P: the same layout as YUV420P. Hand it to OBS as it is,
    // flagged full range (as the hardware path does), instead of converting
    // every frame to video range.
    const bool i420 = frame->format == AV_PIX_FMT_YUV420P ||
                      frame->format == AV_PIX_FMT_YUVJ420P;
    bool full_range = frame->color_range == AVCOL_RANGE_JPEG ||
                      frame->format == AV_PIX_FMT_YUVJ420P;
    if (!i420 && !nv12) {
        // swscale converts the other full-range (YUVJ) layouts to video range
        // and leaves the range of everything else untouched.
        const AVPixFmtDescriptor* descriptor =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        if (descriptor && std::strncmp(descriptor->name, "yuvj", 4) == 0) {
            full_range = false;
        }
        m_sws_context = sws_getCachedContext(
            m_sws_context,
            frame->width,
            frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width,
            frame->height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!m_sws_context) {
            blog(LOG_ERROR, "Failed to create swscale context");
            return false;
        }

        if (m_frame_i420->width != frame->width || m_frame_i420->height != frame->height ||
            m_frame_i420->format != AV_PIX_FMT_YUV420P) {
            av_frame_unref(m_frame_i420);
            m_frame_i420->format = AV_PIX_FMT_YUV420P;
            m_frame_i420->width = frame->width;
            m_frame_i420->height = frame->height;
            if (av_frame_get_buffer(m_frame_i420, 32) < 0) {
                // av_frame_get_buffer needs the geometry set beforehand; clear
                // it again so the next frame retries the allocation instead of
                // matching it and scaling into NULL planes.
                av_frame_unref(m_frame_i420);
                blog(LOG_ERROR, "Failed to allocate I420 frame buffer");
                return false;
            }
        }

        sws_scale(m_sws_context,
                  frame->data,
                  frame->linesize,
                  0,
                  frame->height,
                  m_frame_i420->data,
                  m_frame_i420->linesize);
        src = m_frame_i420;
    }

    out_frame.width = src->width;
    out_frame.height = src->height;
    out_frame.nv12 = nv12;
    out_frame.full_range = full_range;
    out_frame.linesize[0] = src->linesize[0];
    out_frame.linesize[1] = src->linesize[1];
    out_frame.linesize[2] = nv12 ? 0 : src->linesize[2];
    out_frame.data[0] = src->data[0];
    out_frame.data[1] = src->data[1];
    out_frame.data[2] = nv12 ? nullptr : src->data[2];
    return true;
}

bool H264Decoder::outputHardwareFrame(DecodedVideoFrame& out_frame)
{
#if defined(__APPLE__)
    auto buffer = static_cast<CVPixelBufferRef>(static_cast<void*>(m_frame->data[3]));
    if (buffer) {
        const OSType type = CVPixelBufferGetPixelFormatType(buffer);
        const bool video_range = type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        const bool full_range = type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
        if ((video_range || full_range) && CVPixelBufferGetPlaneCount(buffer) == 2 &&
            CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
            // Hand the decoder's own NV12 planes to the caller (OBS copies
            // them); no transfer or colour conversion on the CPU.
            CVPixelBufferRetain(buffer);
            m_locked_buffer = buffer;
            out_frame.width = std::min<int>(m_frame->width,
                static_cast<int>(CVPixelBufferGetWidthOfPlane(buffer, 0)));
            out_frame.height = std::min<int>(m_frame->height,
                static_cast<int>(CVPixelBufferGetHeightOfPlane(buffer, 0)));
            out_frame.nv12 = true;
            out_frame.full_range = full_range;
            out_frame.data[0] = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
            out_frame.data[1] = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
            out_frame.data[2] = nullptr;
            out_frame.linesize[0] = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(buffer, 0));
            out_frame.linesize[1] = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(buffer, 1));
            out_frame.linesize[2] = 0;
            if (out_frame.data[0] && out_frame.data[1]) {
                return true;
            }
            releaseLockedBuffer();
        }
    }
#endif
    // Any other pixel layout (e.g. 10-bit): copy the frame out of the
    // hardware surface and convert it like a software frame.
    av_frame_unref(m_frame_transfer);
    if (av_hwframe_transfer_data(m_frame_transfer, m_frame, 0) < 0) {
        return false;
    }
    m_frame_transfer->color_range = m_frame->color_range;
    return outputSoftwareFrame(m_frame_transfer, out_frame);
}

// Collects every picture the decoder has ready into m_frame, keeping the
// newest (what a live mirror shows). True if there was one.
bool H264Decoder::collectFrames(int* error)
{
    bool received = false;
    for (;;) {
        const int ret = avcodec_receive_frame(m_codec_context, m_frame_next);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            *error = ret;
            break;
        }
        av_frame_unref(m_frame);
        av_frame_move_ref(m_frame, m_frame_next);
        received = true;
    }
    return received;
}

// Hands the picture in m_frame to the caller.
bool H264Decoder::produceFrame(DecodedVideoFrame& out_frame, int* error)
{
    const bool hardware = m_frame->format == AV_PIX_FMT_VIDEOTOOLBOX;
    const bool produced = hardware ? outputHardwareFrame(out_frame)
                                   : outputSoftwareFrame(m_frame, out_frame);
    if (!produced) {
        *error = AVERROR_EXTERNAL;
        return false;
    }
    out_frame.bt601 = is_bt601(m_frame->colorspace);
    out_frame.pts = m_frame->pts;
    noteMode(hardware, out_frame.width, out_frame.height, out_frame.nv12, out_frame.full_range);
    return true;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, DecodedVideoFrame& out_frame,
                               int64_t pts)
{
    if (!m_codec_context || !m_frame || !m_frame_next || !m_frame_i420 || !m_frame_transfer ||
        !m_packet) {
        return false;
    }
    if (!data || size == 0 || size > static_cast<size_t>(INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)) {
        return false;
    }

    // The previous frame's planes are no longer needed by the caller.
    releaseLockedBuffer();
    rememberParameterSets(data, size);

    int error = 0;
    bool received = false;
    for (const auto& unit : split_access_units(m_codec_id, data, size)) {
        const uint8_t* unit_data = data + unit.first;
        const size_t unit_size = unit.second;
        const auto send = [&]() -> int {
            av_packet_unref(m_packet);
            if (av_new_packet(m_packet, static_cast<int>(unit_size)) < 0) {
                blog(LOG_ERROR, "Failed to allocate video packet buffer");
                return AVERROR(ENOMEM);
            }
            memcpy(m_packet->data, unit_data, unit_size);
            m_packet->pts = pts;
            return avcodec_send_packet(m_codec_context, m_packet);
        };

        int ret = send();
        if (m_hardware_refused) {
            m_hardware_refused = false;
            if (!fallBackToSoftware("stream not supported by the hardware decoder")) {
                return false;
            }
            received = false;
            ret = send();
        }
        if (ret == AVERROR(EAGAIN)) {
            // The decoder holds pictures nobody collected (not an error):
            // take them (the newest is kept) and send again.
            received = collectFrames(&error) || received;
            ret = send();
        }
        if (ret < 0) {
            if (!noteError(ret) || !fallBackToSoftware("repeated errors sending packets")) {
                return false;
            }
            received = false;
            ret = send();
            if (ret < 0) {
                return false;
            }
        }
        received = collectFrames(&error) || received;
    }

    if (received && produceFrame(out_frame, &error)) {
        m_hardware_errors = 0;
        m_stream_errors = 0;
        m_errors_logged = 0;
        return true;
    }
    if (error < 0 && noteError(error)) {
        fallBackToSoftware("repeated decode errors");
    }
    return false;
}
