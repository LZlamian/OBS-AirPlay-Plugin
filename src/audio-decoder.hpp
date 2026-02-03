#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    
    bool decode(const uint8_t* data, size_t size);
    
private:
    AVCodecContext* m_codec_context;
    AVFrame* m_frame;
    AVPacket* m_packet;
    struct SwrContext* m_swr_context;
};
