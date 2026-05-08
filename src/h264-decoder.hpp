#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct DecodedVideoFrame {
    int width = 0;
    int height = 0;
    int linesize[3] = {0, 0, 0};
    // Pointers reference data owned by the decoder's internal AVFrame.
    // Valid only until the next decodeToI420() call on the same decoder.
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
};

class H264Decoder {
public:
    explicit H264Decoder(AVCodecID codec_id = AV_CODEC_ID_H264);
    ~H264Decoder();
    
    bool decodeToI420(const uint8_t* data, size_t size, DecodedVideoFrame& out_frame);
    void flush();
    
private:
    AVCodecContext* m_codec_context;
    AVFrame* m_frame;
    AVFrame* m_frame_i420;
    AVPacket* m_packet;
    struct SwsContext* m_sws_context;
};
