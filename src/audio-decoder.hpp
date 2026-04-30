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
    bool inputMatchesOutput(const AVFrame* frame) const;

    AVCodecContext* m_codec_context;
    AVFrame* m_frame;
    AVPacket* m_packet;
    SwrContext* m_swr_context;
    uint8_t m_codec_type;
    int m_output_sample_rate;

    // Cached resampler input description so we can detect a no-op conversion
    // and reconfigure only when the source format actually changes.
    int m_swr_in_rate;
    int m_swr_in_format;
    int m_swr_in_channels;

    // Reused planar output buffer for swr_convert. Sized once to a power-of-2
    // upper bound on per-packet sample counts, then reallocated only when a
    // larger frame appears.
    std::vector<uint8_t> m_swr_buf_left;
    std::vector<uint8_t> m_swr_buf_right;
    int m_swr_buf_capacity;
};
