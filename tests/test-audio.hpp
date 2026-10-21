// SPDX-License-Identifier: GPL-3.0-or-later
// Test input shared by unit-tests and airplay-server-tests.
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <cstdint>
#include <vector>

// Encodes `seconds` of a 440 Hz stereo sine as raw AAC-LC access units
// (44.1 kHz, as AirPlay sends them: no ADTS headers).
inline std::vector<std::vector<uint8_t>> encodeAacLc(double seconds, int sample_rate = 44100)
{
    std::vector<std::vector<uint8_t>> packets;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return packets;
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    encoder->sample_rate = sample_rate;
    encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
    encoder->bit_rate = 128000;
    encoder->profile = AV_PROFILE_AAC_LOW;
    av_channel_layout_default(&encoder->ch_layout, 2);
    if (avcodec_open2(encoder, codec, nullptr) < 0) {
        avcodec_free_context(&encoder);
        return packets;
    }
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    frame->format = encoder->sample_fmt;
    frame->sample_rate = encoder->sample_rate;
    frame->nb_samples = encoder->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
    av_frame_get_buffer(frame, 0);

    auto drain = [&] {
        while (avcodec_receive_packet(encoder, packet) == 0) {
            packets.emplace_back(packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }
    };
    const int total = static_cast<int>(seconds * encoder->sample_rate);
    for (int sample = 0; sample < total; sample += frame->nb_samples) {
        av_frame_make_writable(frame);
        for (int ch = 0; ch < 2; ++ch) {
            auto* out = reinterpret_cast<float*>(frame->data[ch]);
            for (int i = 0; i < frame->nb_samples; ++i) {
                out[i] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 440.0 * (sample + i) /
                                                            encoder->sample_rate));
            }
        }
        frame->pts = sample;
        if (avcodec_send_frame(encoder, frame) == 0) drain();
    }
    avcodec_send_frame(encoder, nullptr);
    drain();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    return packets;
}

