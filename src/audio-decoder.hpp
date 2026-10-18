// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

// The AVChannelLayout API (FFmpeg 5.1: libavutil 57.24.100, with the
// matching libavcodec). One gate for the decoder and the resampler.
#define OBS_AIRPLAY_HAVE_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100))

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
    void flush();

private:
    bool configureDecoder(uint8_t ct);
    bool openDecoder(uint8_t ct);
    bool configureResampler(const AVFrame* frame);
    void resetResampler();

    AVCodecContext* m_codec_context;
    AVFrame* m_frame;
    AVPacket* m_packet;
    SwrContext* m_swr_context;
    uint8_t m_codec_type;
    int m_output_sample_rate;
    int m_input_sample_rate;
    AVSampleFormat m_input_sample_format;
    bool m_resampler_configured;
    // Codec type whose decoder failed to open, and calls since: retried
    // only now and then, not on every packet.
    uint8_t* m_convert[2] = {nullptr, nullptr};  // resampler output, reused
    int m_convert_capacity = 0;
    uint8_t m_failed_codec_type = 0;
    unsigned m_failed_calls = 0;
#if OBS_AIRPLAY_HAVE_CH_LAYOUT
    AVChannelLayout m_input_channel_layout = {};
#else
    uint64_t m_input_channel_layout;
#endif
};
