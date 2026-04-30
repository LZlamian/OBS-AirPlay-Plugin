#include "h264-decoder.hpp"
#include <obs-module.h>

namespace {

// Map an FFmpeg pixel format that OBS can consume natively (no conversion).
// Returns the internal DecodedVideoFrame::format code (1=NV12, 2=I420), or 0
// if the format requires conversion via swscale.
int native_obs_format_for(AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_NV12:
        return 1;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return 2;
    default:
        return 0;
    }
}

} // namespace

H264Decoder::H264Decoder(AVCodecID codec_id)
    : m_codec_context(nullptr)
    , m_frame(nullptr)
    , m_frame_sw(nullptr)
    , m_packet(nullptr)
    , m_sws_context(nullptr)
    , m_sws_src_w(0)
    , m_sws_src_h(0)
    , m_sws_src_fmt(AV_PIX_FMT_NONE)
{
    const AVCodec* codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        blog(LOG_ERROR, "Video codec not found (%d)", static_cast<int>(codec_id));
        return;
    }

    m_codec_context = avcodec_alloc_context3(codec);
    if (!m_codec_context) {
        blog(LOG_ERROR, "Failed to allocate codec context");
        return;
    }

    // Low-latency decoder configuration:
    //  - LOW_DELAY: tell the decoder it can emit frames as soon as decode order
    //    matches output order (no B-frame reordering buffer).
    //  - FAST: enable speed-over-strict-spec compromises in the decoder.
    //  - CHUNKS: allow incomplete frames; we feed one access unit at a time so
    //    the decoder shouldn't sit on partial data waiting for the next packet.
    //  - Slice threading instead of frame threading: frame threading buffers
    //    one full frame of latency, which is exactly what we want to avoid.
    m_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codec_context->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_CHUNKS;
    m_codec_context->thread_type = FF_THREAD_SLICE;
    m_codec_context->thread_count = 0; // auto

    if (avcodec_open2(m_codec_context, codec, nullptr) < 0) {
        blog(LOG_ERROR, "Failed to open codec");
        avcodec_free_context(&m_codec_context);
        return;
    }

    m_frame = av_frame_alloc();
    m_frame_sw = av_frame_alloc();
    m_packet = av_packet_alloc();

    blog(LOG_INFO, "Video decoder initialized for codec id %d (low-latency)",
         static_cast<int>(codec_id));
}

H264Decoder::~H264Decoder()
{
    if (m_sws_context) {
        sws_freeContext(m_sws_context);
    }

    if (m_frame) {
        av_frame_free(&m_frame);
    }

    if (m_frame_sw) {
        av_frame_free(&m_frame_sw);
    }

    if (m_packet) {
        av_packet_free(&m_packet);
    }

    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }
}

bool H264Decoder::ensureSwsContext(int src_w, int src_h, AVPixelFormat src_fmt)
{
    if (m_sws_context && m_sws_src_w == src_w && m_sws_src_h == src_h &&
        m_sws_src_fmt == src_fmt) {
        return true;
    }

    m_sws_context = sws_getCachedContext(
        m_sws_context,
        src_w, src_h, src_fmt,
        src_w, src_h, AV_PIX_FMT_YUV420P,
        // SWS_FAST_BILINEAR is a few times faster than SWS_BILINEAR with
        // negligible quality loss for color conversion at native size.
        SWS_FAST_BILINEAR,
        nullptr, nullptr, nullptr);
    if (!m_sws_context) {
        blog(LOG_ERROR, "Failed to create swscale context");
        return false;
    }

    m_sws_src_w = src_w;
    m_sws_src_h = src_h;
    m_sws_src_fmt = src_fmt;

    if (m_frame_sw->width != src_w || m_frame_sw->height != src_h ||
        m_frame_sw->format != AV_PIX_FMT_YUV420P) {
        av_frame_unref(m_frame_sw);
        m_frame_sw->format = AV_PIX_FMT_YUV420P;
        m_frame_sw->width = src_w;
        m_frame_sw->height = src_h;
        if (av_frame_get_buffer(m_frame_sw, 32) < 0) {
            blog(LOG_ERROR, "Failed to allocate I420 staging frame");
            return false;
        }
    }

    return true;
}

int H264Decoder::decode(const uint8_t* data, size_t size, const FrameCallback& cb)
{
    if (!m_codec_context || !m_frame || !m_frame_sw || !m_packet) {
        return -1;
    }

    av_packet_unref(m_packet);
    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = static_cast<int>(size);

    int ret = avcodec_send_packet(m_codec_context, m_packet);
    if (ret < 0) {
        // EAGAIN here means the decoder has output to be drained first; that's
        // not actually an error. Anything else is logged sparsely.
        if (ret != AVERROR(EAGAIN)) {
            static uint64_t logged = 0;
            if ((logged++ % 60) == 0) {
                blog(LOG_ERROR, "Error sending packet to decoder (%d)", ret);
            }
            return -1;
        }
    }

    int frames_emitted = 0;
    while (true) {
        ret = avcodec_receive_frame(m_codec_context, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            static uint64_t logged = 0;
            if ((logged++ % 60) == 0) {
                blog(LOG_ERROR, "Error decoding frame (%d)", ret);
            }
            break;
        }

        const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(m_frame->format);
        const int native = native_obs_format_for(src_fmt);

        DecodedVideoFrame view;
        view.width = m_frame->width;
        view.height = m_frame->height;

        if (native != 0) {
            view.format = native;
            view.linesize[0] = m_frame->linesize[0];
            view.linesize[1] = m_frame->linesize[1];
            view.linesize[2] = m_frame->linesize[2];
            view.data[0] = m_frame->data[0];
            view.data[1] = m_frame->data[1];
            view.data[2] = m_frame->data[2];
        } else {
            // Fallback: convert to I420 once into a reused staging frame.
            if (!ensureSwsContext(m_frame->width, m_frame->height, src_fmt)) {
                av_frame_unref(m_frame);
                continue;
            }

            sws_scale(m_sws_context,
                      m_frame->data, m_frame->linesize,
                      0, m_frame->height,
                      m_frame_sw->data, m_frame_sw->linesize);

            view.format = 2; // I420
            view.linesize[0] = m_frame_sw->linesize[0];
            view.linesize[1] = m_frame_sw->linesize[1];
            view.linesize[2] = m_frame_sw->linesize[2];
            view.data[0] = m_frame_sw->data[0];
            view.data[1] = m_frame_sw->data[1];
            view.data[2] = m_frame_sw->data[2];
        }

        if (cb) {
            cb(view);
        }
        ++frames_emitted;

        av_frame_unref(m_frame);
    }

    return frames_emitted;
}
