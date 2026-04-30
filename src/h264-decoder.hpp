#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// View of a decoded video frame. Pointers reference internal decoder buffers
// and are only valid for the duration of the FrameCallback invocation.
//   format == 1 (NV12):  data[0]=Y plane, data[1]=interleaved UV, data[2] unused
//   format == 2 (I420):  data[0]=Y plane, data[1]=U plane, data[2]=V plane
struct DecodedVideoFrame {
    int width = 0;
    int height = 0;
    int linesize[3] = {0, 0, 0};
    const uint8_t* data[3] = {nullptr, nullptr, nullptr};
    // Planar YUV layout of the data:
    //   1 = NV12 (data[0]=Y, data[1]=interleaved UV, data[2]=nullptr)
    //   2 = I420 (data[0]=Y, data[1]=U, data[2]=V)
    int format = 0;
};

class H264Decoder {
public:
    using FrameCallback = std::function<void(const DecodedVideoFrame&)>;

    explicit H264Decoder(AVCodecID codec_id = AV_CODEC_ID_H264);
    ~H264Decoder();

    // Submit a compressed access unit. Drains and invokes `cb` once per
    // decoded frame produced. Returns the number of frames emitted, or
    // a negative value on a fatal decoder error.
    int decode(const uint8_t* data, size_t size, const FrameCallback& cb);

private:
    bool ensureSwsContext(int src_w, int src_h, AVPixelFormat src_fmt);

    AVCodecContext* m_codec_context;
    AVFrame* m_frame;
    AVFrame* m_frame_sw;        // staging frame for sws output (I420)
    AVPacket* m_packet;
    struct SwsContext* m_sws_context;
    int m_sws_src_w;
    int m_sws_src_h;
    AVPixelFormat m_sws_src_fmt;
};
