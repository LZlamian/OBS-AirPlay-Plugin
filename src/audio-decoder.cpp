#include "audio-decoder.hpp"
#include <obs-module.h>

AudioDecoder::AudioDecoder()
    : m_codec_context(nullptr)
    , m_frame(nullptr)
    , m_packet(nullptr)
    , m_swr_context(nullptr)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) {
        blog(LOG_ERROR, "AAC codec not found");
        return;
    }
    
    m_codec_context = avcodec_alloc_context3(codec);
    if (!m_codec_context) {
        blog(LOG_ERROR, "Failed to allocate audio codec context");
        return;
    }
    
    m_codec_context->sample_rate = 44100;
    
    // Use new FFmpeg 5.0+ API for channel layout
    #if LIBAVCODEC_VERSION_MAJOR >= 59
        av_channel_layout_default(&m_codec_context->ch_layout, 2);
    #else
        m_codec_context->channels = 2;
        m_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    #endif
    
    if (avcodec_open2(m_codec_context, codec, nullptr) < 0) {
        blog(LOG_ERROR, "Failed to open audio codec");
        avcodec_free_context(&m_codec_context);
        return;
    }
    
    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    
    blog(LOG_INFO, "Audio decoder initialized");
}

AudioDecoder::~AudioDecoder()
{
    if (m_swr_context) {
        swr_free(&m_swr_context);
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

bool AudioDecoder::decode(const uint8_t* data, size_t size)
{
    if (!m_codec_context || !m_frame || !m_packet) {
        return false;
    }
    
    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;
    
    int ret = avcodec_send_packet(m_codec_context, m_packet);
    if (ret < 0) {
        blog(LOG_ERROR, "Error sending audio packet to decoder");
        return false;
    }
    
    ret = avcodec_receive_frame(m_codec_context, m_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    } else if (ret < 0) {
        blog(LOG_ERROR, "Error decoding audio frame");
        return false;
    }
    
    return true;
}
