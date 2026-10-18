// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the parts that need no network or OBS: the HLS playlist
// reader, the media port list, and the AirPlay audio decoder.
#include "audio-decoder.hpp"
#include "hls-playlist.hpp"
#include "media-url-policy.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" void blog(int, const char* format, ...)
{
    std::fputs("unit-tests: ", stderr);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

namespace {

int g_failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #condition);                                             \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        const auto& a_ = (actual);                                                    \
        const auto& e_ = (expected);                                                  \
        if (!(a_ == e_)) {                                                            \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s\n", __FILE__, __LINE__, \
                         #actual " == " #expected);                                   \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

void testHlsParse()
{
    const std::string playlist =
        "#EXTM3U\r\n"
        "#EXT-X-VERSION:6\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\"\r\n"
        "low/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1400000,CODECS=\"avc1.4d401f,mp4a.40.2\"\r\n"
        "\r\n"
        "  mid/index.m3u8  \r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=6000000,CODECS=\"hvc1.2.4.L123.B0,mp4a.40.2\"\n"
        "https://cdn.example.com/high/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1200000,CODECS=\"vp09.00.10.08\"\n"
        "vp9/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000\n"
        "#EXT-X-SOME-TAG\n"
        "nocodecs.m3u8";  // last line without a newline
    const std::vector<hls::Variant> variants = hls::parseVariants(playlist);
    CHECK_EQ(variants.size(), size_t(5));
    if (variants.size() == 5) {
        CHECK_EQ(variants[0].uri, std::string("low/index.m3u8"));
        CHECK_EQ(variants[0].bandwidth, int64_t(800000));
        CHECK(variants[0].supported_video);
        CHECK_EQ(variants[1].uri, std::string("mid/index.m3u8"));  // blank line and spaces skipped
        CHECK(variants[2].supported_video);  // HEVC
        CHECK(!variants[3].supported_video);  // VP9
        CHECK_EQ(variants[4].uri, std::string("nocodecs.m3u8"));  // tag lines between skipped
        CHECK(variants[4].supported_video);  // no CODECS: assumed playable
    }

    // The best rendition at or under the target, not the largest one.
    const hls::Variant* chosen = hls::chooseVariant(variants);
    CHECK(chosen && chosen->uri == "mid/index.m3u8");

    // A media playlist has no renditions.
    CHECK(hls::parseVariants("#EXTM3U\n#EXTINF:2.0,\nseg0.ts\n").empty());
    CHECK(hls::chooseVariant({}) == nullptr);
}

void testHlsChoose()
{
    // Everything over the target: the smallest one.
    std::vector<hls::Variant> over = {
        {"a", 9000000, true}, {"b", 3000000, true}, {"c", 5000000, true}};
    const hls::Variant* chosen = hls::chooseVariant(over);
    CHECK(chosen && chosen->uri == "b");

    // Unsupported renditions are never chosen, even the only one.
    std::vector<hls::Variant> unsupported = {{"vp9", 500000, false}};
    CHECK(hls::chooseVariant(unsupported) == nullptr);

    // No bandwidth given: taken when nothing under the target is known.
    std::vector<hls::Variant> unknown = {{"x", 0, true}, {"big", 8000000, true}};
    chosen = hls::chooseVariant(unknown);
    CHECK(chosen && chosen->uri == "x");
    std::vector<hls::Variant> known_first = {{"ok", 1000000, true}, {"x", 0, true}};
    chosen = hls::chooseVariant(known_first);
    CHECK(chosen && chosen->uri == "ok");

    // Exactly at the target counts as under it.
    std::vector<hls::Variant> exact = {{"at", hls::kTargetBitrate, true},
                                       {"over", hls::kTargetBitrate + 1, true}};
    chosen = hls::chooseVariant(exact);
    CHECK(chosen && chosen->uri == "at");
}

void testHlsResolve()
{
    const std::string base = "http://host:8080/path/to/master.m3u8?token=abc#frag";
    CHECK_EQ(hls::resolveUrl(base, "low/index.m3u8"),
             std::string("http://host:8080/path/to/low/index.m3u8"));
    CHECK_EQ(hls::resolveUrl(base, "../other/index.m3u8"),
             std::string("http://host:8080/path/other/index.m3u8"));
    CHECK_EQ(hls::resolveUrl(base, "./a/./b/../c.m3u8"),
             std::string("http://host:8080/path/to/a/c.m3u8"));
    CHECK_EQ(hls::resolveUrl(base, "/root.m3u8?x=1"),
             std::string("http://host:8080/root.m3u8?x=1"));
    CHECK_EQ(hls::resolveUrl(base, "//cdn.example.com/v.m3u8"),
             std::string("http://cdn.example.com/v.m3u8"));
    CHECK_EQ(hls::resolveUrl(base, "HTTPS://Other/x.m3u8"), std::string("HTTPS://Other/x.m3u8"));
    // ".." never climbs above the root.
    CHECK_EQ(hls::resolveUrl("http://h/a.m3u8", "../../../x.m3u8"), std::string("http://h/x.m3u8"));
    // No base to resolve against.
    CHECK(hls::resolveUrl("not a url", "x.m3u8").empty());
    CHECK(hls::resolveUrl("http://hostonly", "x.m3u8").empty());
}

void testPortList()
{
    using media_url_policy::parsePortList;
    CHECK((parsePortList("80, 443,8080\t8443") ==
           std::vector<unsigned short>{80, 443, 8080, 8443}));
    CHECK((parsePortList("443,443, 80") == std::vector<unsigned short>{443, 80}));
    CHECK((parsePortList("0,65535,65536,99999") == std::vector<unsigned short>{65535}));
    CHECK(parsePortList("80;443").empty());
    CHECK(parsePortList("http").empty());
    CHECK(parsePortList("").empty());
}

// Encodes `seconds` of a 440 Hz stereo sine as raw AAC-LC access units
// (44.1 kHz, as AirPlay sends them: no ADTS headers).
std::vector<std::vector<uint8_t>> encodeAacLc(double seconds)
{
    std::vector<std::vector<uint8_t>> packets;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return packets;
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    encoder->sample_rate = 44100;
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

void testAudioDecoder()
{
    const std::vector<std::vector<uint8_t>> packets = encodeAacLc(1.0);
    CHECK(packets.size() > 40);
    if (packets.empty()) return;

    AudioDecoder decoder;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> all_left;
    int sample_rate = 0;
    int decoded_packets = 0;
    for (const auto& packet : packets) {
        if (decoder.decode(packet.data(), packet.size(), 4, left, right, sample_rate)) {
            ++decoded_packets;
            CHECK_EQ(left.size(), right.size());
            all_left.insert(all_left.end(), left.begin(), left.end());
        }
    }
    // The encoder's priming packet may produce nothing; everything else must.
    CHECK(decoded_packets >= static_cast<int>(packets.size()) - 2);
    CHECK_EQ(sample_rate, 44100);
    CHECK(all_left.size() > 40000);

    // Past the codec's start-up: a 440 Hz sine at half scale.
    if (all_left.size() > 30000) {
        double sum_squares = 0.0;
        int crossings = 0;
        const size_t begin = 4096;
        const size_t end = begin + 22050;  // half a second
        for (size_t i = begin; i < end; ++i) {
            sum_squares += static_cast<double>(all_left[i]) * all_left[i];
            if ((all_left[i - 1] < 0.0f) != (all_left[i] < 0.0f)) ++crossings;
        }
        const double rms = std::sqrt(sum_squares / static_cast<double>(end - begin));
        std::printf("audio: %d/%zu packets, %zu samples, rms=%.3f, %d zero crossings in 0.5 s\n",
                    decoded_packets, packets.size(), all_left.size(), rms, crossings);
        CHECK(std::fabs(rms - 0.5 / std::sqrt(2.0)) < 0.05);
        CHECK(crossings > 420 && crossings < 460);  // 440 Hz: 440 crossings
    }

    // Garbage and unsupported codec types fail without output or a crash,
    // and the decoder still works afterwards.
    std::vector<uint8_t> garbage(300);
    for (size_t i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<uint8_t>(i * 37 + 11);
    decoder.decode(garbage.data(), garbage.size(), 4, left, right, sample_rate);
    CHECK(!decoder.decode(packets[5].data(), packets[5].size(), 99, left, right, sample_rate));
    CHECK(!decoder.decode(nullptr, 0, 4, left, right, sample_rate));
    decoder.flush();
    int recovered = 0;
    for (size_t i = 10; i < 20; ++i) {
        if (decoder.decode(packets[i].data(), packets[i].size(), 4, left, right, sample_rate) &&
            !left.empty()) {
            ++recovered;
        }
    }
    CHECK(recovered >= 9);
}

} // namespace

int main()
{
    testHlsParse();
    testHlsChoose();
    testHlsResolve();
    testPortList();
    testAudioDecoder();
    if (g_failures) {
        std::fprintf(stderr, "unit-tests: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("unit-tests: all checks passed\n");
    return 0;
}
