#include "h264-decoder.hpp"
#include <obs-module.h>

H264Decoder::H264Decoder()
    : m_codec_context(nullptr)
    , m_frame(nullptr)
    , m_packet(nullptr)
    , m_sws_context(nullptr)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        blog(LOG_ERROR, "H264 codec not found");
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
    m_packet = av_packet_alloc();
    
    blog(LOG_INFO, "H264 decoder initialized");
}

H264Decoder::~H264Decoder()
{
    if (m_sws_context) {
        sws_freeContext(m_sws_context);
    }
    
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    
    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }
}

bool H264Decoder::decode(const uint8_t* data, size_t size)
{
    if (!m_codec_context || !m_frame || !m_packet) {
        return false;
    }
    
    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;
    
    int ret = avcodec_send_packet(m_codec_context, m_packet);
    if (ret < 0) {
        blog(LOG_ERROR, "Error sending packet to decoder");
        return false;
    }
    
    ret = avcodec_receive_frame(m_codec_context, m_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    } else if (ret < 0) {
        blog(LOG_ERROR, "Error decoding frame");
        return false;
    }
    
    return true;
}
