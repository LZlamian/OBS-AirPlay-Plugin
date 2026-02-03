#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    
    bool decode(const uint8_t* data, size_t size);
    
private:
    AVCodecContext* m_codec_context;
    AVFrame* m_frame;
    AVPacket* m_packet;
    struct SwsContext* m_sws_context;
};
