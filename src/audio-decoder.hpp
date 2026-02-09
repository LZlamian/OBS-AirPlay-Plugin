#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <cstddef>
#include <cstdint>
#include <vector>

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    bool decode(const uint8_t* data, size_t size, uint8_t ct,
                std::vector<float>& left, std::vector<float>& right,
                int& sample_rate);

private:
    bool configureDecoder(uint8_t ct);
    bool configureResampler(const AVFrame* frame);

    AVCodecContext* m_codec_context;
    AVFrame* m_frame;
    AVPacket* m_packet;
    SwrContext* m_swr_context;
    uint8_t m_codec_type;
    int m_output_sample_rate;
};
