#include "h264-decoder.hpp"
#include <obs-module.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

namespace {

// After this many consecutive hardware errors the decoder is reopened in
// software for the rest of the connection.
constexpr int kMaxHardwareErrors = 3;

// Hardware decoding is on by default. OBS_AIRPLAY_HW_DECODE=0, or the file
// ~/Library/Application Support/obs-studio/obs-airplay-software-decode,
// forces software decoding (for troubleshooting).
bool hardware_decode_requested()
{
    if (const char* env = std::getenv("OBS_AIRPLAY_HW_DECODE")) {
        return std::strcmp(env, "0") != 0;
    }
    if (const char* home = std::getenv("HOME")) {
        const std::string flag = std::string(home) +
            "/Library/Application Support/obs-studio/obs-airplay-software-decode";
        struct stat st {};
        if (stat(flag.c_str(), &st) == 0) {
            return false;
        }
    }
    return true;
}

bool is_parameter_set(AVCodecID codec_id, uint8_t header)
{
    if (codec_id == AV_CODEC_ID_HEVC) {
        const int type = (header >> 1) & 0x3f;
        return type >= 32 && type <= 34; // VPS, SPS, PPS
    }
    const int type = header & 0x1f;
    return type == 7 || type == 8; // SPS, PPS
}

bool is_sequence_parameter_set(AVCodecID codec_id, uint8_t header)
{
    return codec_id == AV_CODEC_ID_HEVC ? ((header >> 1) & 0x3f) == 33
                                        : (header & 0x1f) == 7;
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

H264Decoder::H264Decoder(AVCodecID codec_id, Mode mode)
    : m_codec_id(codec_id)
    , m_hardware_preferred(mode == Mode::Auto && hardware_decode_requested())
    , m_hardware_allowed(m_hardware_preferred)
    , m_codec_context(nullptr)
    , m_frame(av_frame_alloc())
    , m_frame_transfer(av_frame_alloc())
    , m_frame_i420(av_frame_alloc())
    , m_packet(av_packet_alloc())
    , m_sws_context(nullptr)
{
    if (!open(m_hardware_allowed)) {
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
    // take this stream; decoding then continues in software in this context.
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
    //  - SLICE thread type with auto thread_count keeps per-frame latency
    //    minimal (FRAME threading would add 1+ frame of pipeline delay).
    m_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codec_context->thread_type = FF_THREAD_SLICE;
    m_codec_context->thread_count = 0; // auto
    m_codec_context->opaque = this;

    m_hardware_open = false;
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

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "tune", "zerolatency", 0);
    const int opened = avcodec_open2(m_codec_context, codec, &opts);
    av_dict_free(&opts);
    if (opened < 0) {
        blog(LOG_ERROR, "Failed to open codec");
        close();
        return false;
    }
    m_hardware_errors = 0;
    return true;
}

void H264Decoder::close()
{
    releaseLockedBuffer();
    if (m_frame) av_frame_unref(m_frame);
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

void H264Decoder::flush()
{
    releaseLockedBuffer();
    // A flush marks a stream boundary. A software fallback applies to the
    // stream that caused it; the next stream starts in hardware again.
    if (m_hardware_preferred && !m_hardware_allowed) {
        close();
        m_hardware_allowed = true;
        m_parameter_sets.clear();
        if (open(true)) {
            blog(LOG_INFO, "[DECODE] new stream: hardware decoding enabled again");
        }
        return;
    }
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
    std::vector<uint8_t> found;
    bool has_sequence_set = false;
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
        size_t next_length = 0;
        const size_t nal_end = next_start(nal_start, &next_length);
        const uint8_t header = data[nal_start];
        if (is_slice(m_codec_id, header)) {
            break;
        }
        if (is_parameter_set(m_codec_id, header)) {
            static const uint8_t start_code[] = {0, 0, 0, 1};
            found.insert(found.end(), start_code, start_code + sizeof(start_code));
            found.insert(found.end(), data + nal_start, data + nal_end);
            has_sequence_set = has_sequence_set || is_sequence_parameter_set(m_codec_id, header);
        }
        position = nal_end;
        code_length = next_length;
    }
    if (has_sequence_set) {
        m_parameter_sets.swap(found);
    }
}

bool H264Decoder::fallBackToSoftware(const char* reason)
{
    blog(LOG_WARNING, "[DECODE] hardware decoding stopped (%s); continuing in software",
         reason);
    close();
    m_hardware_allowed = false;
    if (!open(false)) {
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
    if (frame->format != AV_PIX_FMT_YUV420P && !nv12) {
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
    out_frame.full_range = nv12 && frame->color_range == AVCOL_RANGE_JPEG;
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

bool H264Decoder::receiveFrame(DecodedVideoFrame& out_frame, int* error)
{
    const int ret = avcodec_receive_frame(m_codec_context, m_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    }
    if (ret < 0) {
        *error = ret;
        return false;
    }

    const bool hardware = m_frame->format == AV_PIX_FMT_VIDEOTOOLBOX;
    const bool produced = hardware ? outputHardwareFrame(out_frame)
                                   : outputSoftwareFrame(m_frame, out_frame);
    if (!produced) {
        *error = AVERROR_EXTERNAL;
        return false;
    }
    noteMode(hardware, out_frame.width, out_frame.height, out_frame.nv12, out_frame.full_range);
    return true;
}

bool H264Decoder::decodeToI420(const uint8_t* data, size_t size, DecodedVideoFrame& out_frame)
{
    if (!m_codec_context || !m_frame || !m_frame_i420 || !m_frame_transfer || !m_packet) {
        return false;
    }

    // The previous frame's planes are no longer needed by the caller.
    releaseLockedBuffer();
    rememberParameterSets(data, size);

    const auto send = [&]() -> int {
        av_packet_unref(m_packet);
        if (av_new_packet(m_packet, static_cast<int>(size)) < 0) {
            blog(LOG_ERROR, "Failed to allocate video packet buffer");
            return AVERROR(ENOMEM);
        }
        memcpy(m_packet->data, data, size);
        return avcodec_send_packet(m_codec_context, m_packet);
    };

    int ret = send();
    if (ret < 0 && m_hardware_open && ++m_hardware_errors >= kMaxHardwareErrors) {
        if (!fallBackToSoftware("repeated errors sending packets")) {
            return false;
        }
        ret = send();
    }
    if (ret < 0) {
        blog(LOG_ERROR, "Error sending packet to decoder (%d)", ret);
        return false;
    }

    int error = 0;
    if (receiveFrame(out_frame, &error)) {
        m_hardware_errors = 0;
        return true;
    }
    if (error < 0) {
        blog(LOG_ERROR, "Error decoding frame (%d)", error);
        if (m_hardware_open && ++m_hardware_errors >= kMaxHardwareErrors) {
            fallBackToSoftware("repeated decode errors");
        }
    }
    return false;
}
