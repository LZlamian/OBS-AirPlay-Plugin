// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The audio format a sender chose in SETUP. Without one, the format every
// iOS device uses so far: 44.1 kHz, 16-bit stereo, the codec's usual frame.
struct AirPlayAudioFormat {
    int sample_rate = 44100;
    int channels = 2;
    int bit_depth = 16;    // ALAC only
    int frame_samples = 0; // samples per packet (spf); 0: the codec's usual
    bool operator==(const AirPlayAudioFormat& other) const
    {
        return sample_rate == other.sample_rate && channels == other.channels &&
               bit_depth == other.bit_depth && frame_samples == other.frame_samples;
    }
    bool operator!=(const AirPlayAudioFormat& other) const { return !(*this == other); }
};

namespace airplay_audio {
// From SETUP's audioFormat (one bit) and spf. Known bits: ALAC
// 44.1/48 kHz at 16/24 bits, AAC-LC and AAC-ELD at 44.1/48 kHz, all stereo
// (bits 18-25). Others keep the defaults; false says so.
bool formatFromSetup(uint64_t audio_format, unsigned spf, AirPlayAudioFormat& out);
// For the log, e.g. "48000 Hz, 24-bit, 2 ch".
std::string describe(const AirPlayAudioFormat& format, uint8_t ct);
// Decoder setup data. AAC: an AudioSpecificConfig for object type 2 (LC)
// or 39 (ELD). ALAC: the 'alac' magic cookie.
std::vector<uint8_t> aacAudioSpecificConfig(int object_type, const AirPlayAudioFormat& format);
std::vector<uint8_t> alacMagicCookie(const AirPlayAudioFormat& format);
} // namespace airplay_audio

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // Decodes one AirPlay audio packet of codec type `ct` into planar float
    // stereo. `left`/`right` are owned by the caller and reused between calls
    // (resized, not reallocated when large enough). Returns false with no
    // output for a packet that does not decode.
    bool decode(const uint8_t* data, size_t size, uint8_t ct,
                std::vector<float>& left, std::vector<float>& right,
                int& sample_rate);
    void flush();
    // The format the sender negotiated for its next stream; the decoder is
    // set up for it at the next packet.
    void setStreamFormat(const AirPlayAudioFormat& format);

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
    AirPlayAudioFormat m_format;
    AirPlayAudioFormat m_opened_format;
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
    // Errors so far, for logging every 60th (per decoder, not per process).
    uint64_t m_alloc_errors = 0;
    uint64_t m_send_errors = 0;
    uint64_t m_decode_errors = 0;
    AVChannelLayout m_input_channel_layout = {};
};
