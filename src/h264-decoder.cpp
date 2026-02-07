#include "h264-decoder.hpp"
#include <obs-module.h>

H264Decoder::H264Decoder(AVCodecID codec_id)
    : m_codec_context(nullptr)
    , m_frame(nullptr)
    , m_frame_i420(nullptr)
    , m_packet(nullptr)
    , m_sws_context(nullptr)
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
    
    if (avcodec_open2(m_codec_context, codec, nullptr) < 0) {
        blog(LOG_ERROR, "Failed to open codec");
        avcodec_free_context(&m_codec_context);
        return;
    }
    
    m_frame = av_frame_alloc();
    m_frame_i420 = av_frame_alloc();
    m_packet = av_packet_alloc();
    
    blog(LOG_INFO, "Video decoder initialized for codec id %d", static_cast<int>(codec_id));
}

H264Decoder::~H264Decoder()
{
    if (m_sws_context) {
        sws_freeContext(m_sws_context);
    }
    
    if (m_frame) {
        av_frame_free(&m_frame);
    }

    if (m_frame_i420) {
        av_frame_free(&m_frame_i420);
    }
    
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    
    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }
}

bool H264Decoder::decodeToI420(const uint8_t* data, size_t size, DecodedVideoFrame& out_frame)
{
    if (!m_codec_context || !m_frame || !m_frame_i420 || !m_packet) {
        return false;
    }
    
    av_packet_unref(m_packet);
    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = static_cast<int>(size);
    
    int ret = avcodec_send_packet(m_codec_context, m_packet);
    if (ret < 0) {
        blog(LOG_ERROR, "Error sending packet to decoder (%d)", ret);
        return false;
    }

    while (true) {
        ret = avcodec_receive_frame(m_codec_context, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return false;
        }
        if (ret < 0) {
            blog(LOG_ERROR, "Error decoding frame (%d)", ret);
            return false;
        }

        AVFrame* src = m_frame;
        if (m_frame->format != AV_PIX_FMT_YUV420P) {
            m_sws_context = sws_getCachedContext(
                m_sws_context,
                m_frame->width,
                m_frame->height,
                static_cast<AVPixelFormat>(m_frame->format),
                m_frame->width,
                m_frame->height,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            if (!m_sws_context) {
                blog(LOG_ERROR, "Failed to create swscale context");
                return false;
            }

            if (m_frame_i420->width != m_frame->width || m_frame_i420->height != m_frame->height ||
                m_frame_i420->format != AV_PIX_FMT_YUV420P) {
                av_frame_unref(m_frame_i420);
                m_frame_i420->format = AV_PIX_FMT_YUV420P;
                m_frame_i420->width = m_frame->width;
                m_frame_i420->height = m_frame->height;
                if (av_frame_get_buffer(m_frame_i420, 32) < 0) {
                    blog(LOG_ERROR, "Failed to allocate I420 frame buffer");
                    return false;
                }
            }

            sws_scale(m_sws_context,
                      m_frame->data,
                      m_frame->linesize,
                      0,
                      m_frame->height,
                      m_frame_i420->data,
                      m_frame_i420->linesize);
            src = m_frame_i420;
        }

        out_frame.width = src->width;
        out_frame.height = src->height;
        out_frame.linesize[0] = src->linesize[0];
        out_frame.linesize[1] = src->linesize[1];
        out_frame.linesize[2] = src->linesize[2];

        const int chroma_h = (src->height + 1) / 2;
        out_frame.plane[0].assign(src->data[0], src->data[0] + src->linesize[0] * src->height);
        out_frame.plane[1].assign(src->data[1], src->data[1] + src->linesize[1] * chroma_h);
        out_frame.plane[2].assign(src->data[2], src->data[2] + src->linesize[2] * chroma_h);
        return true;
    }
}
