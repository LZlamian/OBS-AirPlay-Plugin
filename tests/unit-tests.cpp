// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the parts that need no network or OBS: the HLS playlist
// reader, the media port list, and the AirPlay audio decoder.
#include "audio-decoder.hpp"
#include "audio-retimer.hpp"
#include "hls-playlist.hpp"
#include "media-url-policy.hpp"

extern "C" {
#include "plist_checked.h"
}

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <random>
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

    // Attribute names are whole names: AVERAGE-BANDWIDTH first (as in
    // Apple's examples) is not BANDWIDTH, and a quoted comma does not split.
    const std::vector<hls::Variant> averaged = hls::parseVariants(
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=500000,CODECS=\"avc1.4d401f,mp4a.40.2\","
        "BANDWIDTH=900000\n"
        "a.m3u8\n"
        "#EXT-X-STREAM-INF:CODECS=\"avc1.4d401f,BANDWIDTH=7\",BANDWIDTH=300000\n"
        "b.m3u8\n");
    CHECK_EQ(averaged.size(), size_t(2));
    if (averaged.size() == 2) {
        CHECK_EQ(averaged[0].bandwidth, int64_t(900000));
        CHECK_EQ(averaged[1].bandwidth, int64_t(300000));
    }

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
std::vector<std::vector<uint8_t>> encodeAacLc(double seconds, int sample_rate = 44100)
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

// Encodes `seconds` of a 440 Hz stereo sine as ALAC packets at `sample_rate`
// and `bit_depth` (16 or 24); `frame_samples` gets the encoder's frame size.
std::vector<std::vector<uint8_t>> encodeAlac(double seconds, int sample_rate, int bit_depth,
                                             int& frame_samples)
{
    std::vector<std::vector<uint8_t>> packets;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_ALAC);
    if (!codec) return packets;
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    encoder->sample_rate = sample_rate;
    encoder->sample_fmt = bit_depth > 16 ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S16P;
    encoder->bits_per_raw_sample = bit_depth;
    av_channel_layout_default(&encoder->ch_layout, 2);
    if (avcodec_open2(encoder, codec, nullptr) < 0) {
        avcodec_free_context(&encoder);
        return packets;
    }
    frame_samples = encoder->frame_size;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    frame->format = encoder->sample_fmt;
    frame->sample_rate = sample_rate;
    frame->nb_samples = encoder->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
    av_frame_get_buffer(frame, 0);
    auto drain = [&] {
        while (avcodec_receive_packet(encoder, packet) == 0) {
            packets.emplace_back(packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }
    };
    const int total = static_cast<int>(seconds * sample_rate);
    for (int sample = 0; sample + frame->nb_samples <= total; sample += frame->nb_samples) {
        av_frame_make_writable(frame);
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = 0; i < frame->nb_samples; ++i) {
                const double value =
                    0.5 * std::sin(2.0 * M_PI * 440.0 * (sample + i) / sample_rate);
                if (bit_depth > 16) {
                    // 24-bit samples, left-aligned in 32 bits
                    reinterpret_cast<int32_t*>(frame->data[ch])[i] =
                        static_cast<int32_t>(value * 8388607.0) * 256;
                } else {
                    reinterpret_cast<int16_t*>(frame->data[ch])[i] =
                        static_cast<int16_t>(value * 32767.0);
                }
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

// Decodes packets with a decoder set up for `format` and checks the result
// is the 440 Hz half-scale sine at the format's rate.
void checkDecodedSine(const char* what, const std::vector<std::vector<uint8_t>>& packets,
                      uint8_t ct, const AirPlayAudioFormat& format)
{
    AudioDecoder decoder;
    decoder.setStreamFormat(format);
    std::vector<float> left, right, all;
    int sample_rate = 0;
    int decoded = 0;
    for (const auto& packet : packets) {
        if (decoder.decode(packet.data(), packet.size(), ct, left, right, sample_rate)) {
            ++decoded;
            all.insert(all.end(), left.begin(), left.end());
        }
    }
    const size_t begin = static_cast<size_t>(format.sample_rate / 10);
    const size_t end = begin + static_cast<size_t>(format.sample_rate / 2);
    double sum_squares = 0.0;
    int crossings = 0;
    if (all.size() > end) {
        for (size_t i = begin; i < end; ++i) {
            sum_squares += static_cast<double>(all[i]) * all[i];
            if ((all[i - 1] < 0.0f) != (all[i] < 0.0f)) ++crossings;
        }
    }
    const double rms = all.size() > end ? std::sqrt(sum_squares / static_cast<double>(end - begin)) : 0.0;
    std::printf("audio %s: %d/%zu packets at %d Hz, rms=%.3f, %d zero crossings in 0.5 s\n", what,
                decoded, packets.size(), sample_rate, rms, crossings);
    CHECK(decoded >= static_cast<int>(packets.size()) - 2);
    CHECK_EQ(sample_rate, format.sample_rate);
    CHECK(std::fabs(rms - 0.5 / std::sqrt(2.0)) < 0.05);
    CHECK(crossings > 420 && crossings < 460);
}

// BUG-15: the audio format a sender negotiates sets up the decoder (rate,
// channels, depth, frame length) instead of fixed 44.1 kHz stereo data.
void testAudioFormats()
{
    using airplay_audio::formatFromSetup;
    AirPlayAudioFormat format;
    CHECK(formatFromSetup(0x40000, 352, format) && format.sample_rate == 44100 &&
          format.bit_depth == 16 && format.frame_samples == 352);
    CHECK(formatFromSetup(0x80000, 352, format) && format.sample_rate == 44100 && format.bit_depth == 24);
    CHECK(formatFromSetup(0x100000, 352, format) && format.sample_rate == 48000 && format.bit_depth == 16);
    CHECK(formatFromSetup(0x200000, 352, format) && format.sample_rate == 48000 && format.bit_depth == 24);
    CHECK(formatFromSetup(0x400000, 1024, format) && format.sample_rate == 44100);
    CHECK(formatFromSetup(0x800000, 1024, format) && format.sample_rate == 48000);
    CHECK(formatFromSetup(0x1000000, 480, format) && format.sample_rate == 44100 && format.channels == 2);
    CHECK(formatFromSetup(0x2000000, 480, format) && format.sample_rate == 48000);
    CHECK(!formatFromSetup(0, 0, format) && format == AirPlayAudioFormat{});
    CHECK(!formatFromSetup(0x800, 352, format) && format.sample_rate == 44100);  // PCM: not ours

    // The default setup data is exactly what the decoder always used.
    const AirPlayAudioFormat standard;
    CHECK((airplay_audio::aacAudioSpecificConfig(39, standard) ==
           std::vector<uint8_t>{0xF8, 0xE8, 0x50, 0x00}));
    CHECK((airplay_audio::aacAudioSpecificConfig(2, standard) == std::vector<uint8_t>{0x12, 0x10}));
    CHECK((airplay_audio::alacMagicCookie(standard) == std::vector<uint8_t>{
        0x00, 0x00, 0x00, 0x24, 0x61, 0x6C, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0A, 0x0E, 0x02, 0x00, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAC, 0x44}));
    AirPlayAudioFormat rate48;
    rate48.sample_rate = 48000;
    CHECK((airplay_audio::aacAudioSpecificConfig(39, rate48) ==
           std::vector<uint8_t>{0xF8, 0xE6, 0x50, 0x00}));
    CHECK((airplay_audio::aacAudioSpecificConfig(2, rate48) == std::vector<uint8_t>{0x11, 0x90}));

    // Decoded at the negotiated rate.
    AirPlayAudioFormat lc48;
    lc48.sample_rate = 48000;
    checkDecodedSine("AAC-LC 48 kHz", encodeAacLc(1.0, 48000), 4, lc48);
    for (const int rate : {44100, 48000}) {
        for (const int depth : {16, 24}) {
            int frame_samples = 0;
            const auto packets = encodeAlac(1.0, rate, depth, frame_samples);
            CHECK(!packets.empty());
            AirPlayAudioFormat alac;
            alac.sample_rate = rate;
            alac.bit_depth = depth;
            alac.frame_samples = frame_samples;
            char what[48];
            std::snprintf(what, sizeof(what), "ALAC %d Hz %d-bit", rate, depth);
            checkDecodedSine(what, packets, 2, alac);
        }
    }
}

// Binary plists from the network: a string or key with a NUL byte is
// refused before libplist's asserting getters could see it.
std::vector<uint8_t> toBinary(plist_t plist)
{
    char* data = nullptr;
    uint32_t size = 0;
    plist_to_bin(plist, &data, &size);
    std::vector<uint8_t> bytes(data, data + size);
    plist_mem_free(data);
    plist_free(plist);
    return bytes;
}

bool replaceMarker(std::vector<uint8_t>& bytes, const std::vector<uint8_t>& marker,
                   size_t at)
{
    for (size_t i = 0; i + marker.size() <= bytes.size(); ++i) {
        if (std::equal(marker.begin(), marker.end(), bytes.begin() + static_cast<long>(i))) {
            for (size_t j = 0; j < (marker.size() == 1 ? 1 : 2); ++j) bytes[i + at + j] = 0;
            return true;
        }
    }
    return false;
}

void testPlistChecked()
{
    auto check = [](const std::vector<uint8_t>& bytes) {
        return bplist_strings_have_nul(reinterpret_cast<const char*>(bytes.data()),
                                       static_cast<uint32_t>(bytes.size()));
    };
    plist_t ok = plist_new_dict();
    plist_dict_set_item(ok, "Content-Location", plist_new_string("http://192.0.2.10/item.mp4"));
    plist_dict_set_item(ok, "Title", plist_new_string("caf\xc3\xa9"));  // UTF-16 in bplist
    plist_dict_set_item(ok, "Data", plist_new_data("\0\0\0", 3));      // NUL in data is fine
    const std::vector<uint8_t> clean = toBinary(ok);
    CHECK(!check(clean));

    // ASCII string with a NUL ("aXb" -> "a\0b")
    plist_t ascii = plist_new_dict();
    plist_dict_set_item(ascii, "Content-Location", plist_new_string("http://192.0.2.10/aXb.mp4"));
    std::vector<uint8_t> ascii_nul = toBinary(ascii);
    CHECK(replaceMarker(ascii_nul, {'a', 'X', 'b'}, 1));
    CHECK(check(ascii_nul));

    // UTF-16 string with a zero code unit ("\u00e9X" -> "\u00e9\0")
    plist_t utf16 = plist_new_dict();
    plist_dict_set_item(utf16, "Content-Location", plist_new_string("caf\xc3\xa9X"));
    std::vector<uint8_t> utf16_nul = toBinary(utf16);
    CHECK(replaceMarker(utf16_nul, {0x00, 0xe9, 0x00, 'X'}, 2));
    CHECK(check(utf16_nul));

    // a dictionary key with a NUL
    plist_t key = plist_new_dict();
    plist_dict_set_item(key, "KeyXName", plist_new_string("value"));
    std::vector<uint8_t> key_nul = toBinary(key);
    CHECK(replaceMarker(key_nul, {'y', 'X', 'N'}, 1));
    CHECK(check(key_nul));

    // unreadable structure is refused too
    std::vector<uint8_t> truncated(clean.begin(), clean.begin() + static_cast<long>(clean.size() / 2));
    CHECK(check(truncated));
    std::vector<uint8_t> garbage(64, 0xff);
    std::memcpy(garbage.data(), "bplist00", 8);
    CHECK(check(garbage));
    CHECK(check({}));

    // the checked parse gives NULL, as for a parse failure
    plist_t parsed = nullptr;
    plist_from_bin_checked(reinterpret_cast<const char*>(utf16_nul.data()),
                           static_cast<uint32_t>(utf16_nul.size()), &parsed);
    CHECK(parsed == nullptr);
    plist_from_bin_checked(reinterpret_cast<const char*>(clean.data()),
                           static_cast<uint32_t>(clean.size()), &parsed);
    CHECK(parsed != nullptr);
    if (parsed) plist_free(parsed);
}

// Mirrored audio never reaches OBS late (OBS would raise its buffering for
// good) and its timestamps stay continuous.
namespace {
struct RetimerRun {
    int dropped = 0;
    int late = 0;        // output later than now - kLateNs
    int gaps = 0;        // output not continuous with the previous packet
    int audible_gaps = 0;  // ... by more than 1 ms (a gap or an overlap)
    uint64_t max_lead = 0;  // output ahead of now, most
    uint64_t min_lead = UINT64_MAX;  // after the first packet
};
} // namespace

void testAudioRetimer()
{
    constexpr uint64_t ms = 1000000;
    constexpr uint64_t packet = 480 * 1000000000ULL / 44100;  // AAC-ELD packet, ns
    const uint64_t t0 = 1000000 * ms;

    AudioRetimer retimer;
    uint64_t last_end = 0;
    auto feed = [&](RetimerRun& run, uint64_t play_at, uint64_t now) {
        const auto r = retimer.process(play_at, packet, now);
        if (r.drop) {
            ++run.dropped;
            return r;
        }
        if (r.timestamp + AudioRetimer::kLateNs < now) ++run.late;
        if (last_end != 0 && r.timestamp != last_end) ++run.gaps;
        if (last_end != 0 && (r.timestamp > last_end + ms || r.timestamp + ms < last_end)) {
            ++run.audible_gaps;
        }
        if (r.timestamp > now && r.timestamp - now > run.max_lead) run.max_lead = r.timestamp - now;
        if (last_end != 0) {
            run.min_lead = std::min(run.min_lead, r.timestamp > now ? r.timestamp - now : 0);
        }
        last_end = r.timestamp + packet;
        return r;
    };
    auto restart = [&] {
        retimer.reset();
        last_end = 0;
    };

    // On time (100 ms ahead): the sender's times, unchanged.
    {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        bool same = true;
        for (int i = 0; i < 500; ++i, now += packet) {
            same = same && feed(run, now + 100 * ms, now).timestamp == now + 100 * ms;
        }
        CHECK(same);
        CHECK_EQ(run.dropped, 0);
        CHECK_EQ(run.gaps, 0);
        CHECK_EQ(retimer.steps(), uint64_t(0));
    }

    // Startup burst: 1 s of queued packets released at once (play-at times
    // already passed), then real time. The backlog is dropped and the rest
    // keeps the sender's times, at most kStartLeadNs + kLateNs later (the
    // first kept packet starts the stream kStartLeadNs ahead of now).
    {
        restart();
        RetimerRun burst;
        uint64_t now = t0;
        uint64_t play_at = t0 - 1000 * ms;
        for (int i = 0; i < 120; ++i, play_at += packet, now += ms) feed(burst, play_at, now);
        RetimerRun after;
        bool same = true;
        int64_t shift = -1;
        for (int i = 0; i < 400; ++i, play_at += packet, now += packet) {
            const auto r = feed(after, play_at, now);
            if (shift < 0) shift = static_cast<int64_t>(r.timestamp - play_at);
            same = same && !r.drop && static_cast<int64_t>(r.timestamp - play_at) == shift;
        }
        CHECK(burst.dropped > 80);
        CHECK_EQ(burst.late + after.late, 0);
        CHECK(same);
        CHECK(shift >= 0 && shift <= int64_t(AudioRetimer::kStartLeadNs + AudioRetimer::kLateNs));
        CHECK(retimer.worstDroppedNs() >= 990 * ms);
    }

    // The first real RTP sync 4.4 s in moves every play-at time back by 1 s
    // (seen on a device): the stream stays continuous and on time; the 200 ms
    // lead is trimmed to kMaxLeadNs by dropping a few packets.
    {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        uint64_t play_at = t0 + 200 * ms;
        for (; now < t0 + 4400 * ms; now += packet, play_at += packet) feed(run, play_at, now);
        play_at -= 1000 * ms;
        for (int i = 0; i < 2000; ++i, now += packet, play_at += packet) feed(run, play_at, now);
        CHECK_EQ(run.late, 0);
        CHECK_EQ(run.gaps, 0);
        CHECK(run.dropped <= 6);
        CHECK(run.max_lead <= 210 * ms);
        CHECK_EQ(retimer.steps(), uint64_t(1));
        CHECK(retimer.lastStepNs() < -990 * int64_t(ms) && retimer.lastStepNs() > -1010 * int64_t(ms));
        CHECK_EQ(retimer.resyncs(), uint64_t(0));
    }

    // A 4.7 s gap, then the backlog in a burst (seen on a device): restarted
    // at now, and the burst adds at most kMaxLeadNs of delay.
    {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        uint64_t play_at = t0 + 100 * ms;
        for (int i = 0; i < 300; ++i, now += packet, play_at += packet) feed(run, play_at, now);
        now += 4700 * ms;
        play_at += 1900 * ms;
        for (int i = 0; i < 200; ++i, play_at += packet, now += ms) feed(run, play_at, now);
        uint64_t settled_lead = 0;
        for (int i = 0; i < 400; ++i, now += packet, play_at += packet) {
            const auto r = feed(run, play_at, now);
            if (!r.drop) settled_lead = r.timestamp - now;
        }
        CHECK_EQ(run.late, 0);
        CHECK(run.max_lead <= AudioRetimer::kMaxLeadNs + packet);
        CHECK(settled_lead <= AudioRetimer::kMaxLeadNs + packet);
        CHECK(retimer.trimmed() > 0);
    }

    // A stall after the start (packets 400 ms late): one restart kStartLeadNs
    // ahead of now, then on time again, and the delay does not grow.
    {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        uint64_t play_at = t0 + 50 * ms;
        for (int i = 0; i < 300; ++i, now += packet, play_at += packet) feed(run, play_at, now);
        now += 400 * ms;
        RetimerRun after;
        for (int i = 0; i < 400; ++i, now += packet, play_at += packet) feed(after, play_at, now);
        CHECK_EQ(run.late + after.late, 0);
        CHECK_EQ(after.dropped, 0);
        CHECK_EQ(retimer.resyncs(), uint64_t(1));
        CHECK(after.max_lead <= AudioRetimer::kStartLeadNs + packet);
    }

    // Late from the very start (500 ms): dropped for the startup window, then
    // one restart at now, continuous and on time.
    {
        restart();
        RetimerRun startup;
        uint64_t now = t0;
        uint64_t play_at = t0 - 500 * ms;
        for (; now - t0 < AudioRetimer::kStartupNs; now += packet, play_at += packet) {
            feed(startup, play_at, now);
        }
        RetimerRun run;
        for (int i = 0; i < 400; ++i, now += packet, play_at += packet) feed(run, play_at, now);
        CHECK(startup.dropped > 150);
        CHECK_EQ(run.late, 0);
        CHECK_EQ(run.dropped, 0);
        CHECK_EQ(retimer.resyncs(), uint64_t(1));
    }

    // A sender clock 1% fast while its times are ridden over: the timeline
    // never runs more than kMaxLeadNs ahead.
    {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        uint64_t play_at = t0 + 100 * ms;
        for (int i = 0; i < 100; ++i, now += packet, play_at += packet) feed(run, play_at, now);
        play_at -= 1000 * ms;
        for (int i = 0; i < 5000; ++i, now += packet * 99 / 100, play_at += packet) feed(run, play_at, now);
        CHECK_EQ(run.late, 0);
        CHECK(run.max_lead <= AudioRetimer::kMaxLeadNs + packet);
        CHECK(retimer.trimmed() > 0);
    }

    // A step forward that is on time is followed (keeps A/V sync).
    {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        uint64_t play_at = t0 + 50 * ms;
        for (int i = 0; i < 100; ++i, now += packet, play_at += packet) feed(run, play_at, now);
        play_at += 300 * ms;
        const auto r = feed(run, play_at, now);
        CHECK(!r.drop);
        CHECK_EQ(r.timestamp, play_at);
        CHECK_EQ(retimer.steps(), uint64_t(1));
    }

    // Audio anchors the clock (audio-only AirPlay, or audio first after a
    // codec change): its first packet is due exactly now, and the next ones
    // arrive up to 30 ms either side of their times. None is dropped or late
    // and the stream stays continuous; 2.6.0 dropped those 20 ms late.
    {
        restart();
        RetimerRun run;
        std::mt19937_64 random(12345);
        std::uniform_int_distribution<int64_t> spread(-30 * int64_t(ms), 30 * int64_t(ms));
        const auto jitter = [&]() { return spread(random); };
        for (int i = 0; i < 3000; ++i) {
            const uint64_t play_at = t0 + i * packet;
            const uint64_t now = i == 0 ? t0 : static_cast<uint64_t>(int64_t(play_at) + jitter());
            feed(run, play_at, now);
        }
        CHECK_EQ(run.dropped, 0);
        CHECK_EQ(run.late, 0);
        CHECK_EQ(run.gaps, 0);
        CHECK_EQ(retimer.resyncs(), uint64_t(0));
        CHECK(run.max_lead <= AudioRetimer::kStartLeadNs + 30 * ms + packet);
    }

    // A sender whose sample clock is 50 ppm slow or fast against its play-at
    // times (which UxPlay keeps on the local clock), for an hour, after the
    // first RTP sync stepped them back: the difference is absorbed packet by
    // packet. No drops, restarts, late packets or audible gaps, and the
    // delay stays put (2.6.0 dropped a packet every few minutes or opened a
    // gap of 20 ms or more).
    for (const int ppm : {-50, 50}) {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        double play_at = static_cast<double>(t0 + 100 * ms);
        const double real_packet = static_cast<double>(packet) * (1.0 + ppm * 1e-6);
        for (int i = 0; i < 300; ++i, now += packet, play_at += packet) {
            feed(run, static_cast<uint64_t>(play_at), now);
        }
        play_at -= 1000.0 * ms;
        RetimerRun hour;
        const int packets = static_cast<int>(3600.0 * 1e9 / real_packet);
        double now_exact = static_cast<double>(now);
        for (int i = 0; i < packets; ++i, now_exact += real_packet, play_at += real_packet) {
            feed(hour, static_cast<uint64_t>(play_at), static_cast<uint64_t>(now_exact));
        }
        CHECK_EQ(hour.dropped, 0);
        CHECK_EQ(hour.late, 0);
        CHECK_EQ(hour.audible_gaps, 0);
        CHECK_EQ(retimer.resyncs(), uint64_t(0));
        CHECK_EQ(retimer.steps(), uint64_t(1));
        CHECK(hour.max_lead - hour.min_lead <= 2 * ms);
    }

    // The same drift while the sender's times are followed: they are
    // followed, with no step.
    {
        restart();
        RetimerRun run;
        double play_at = static_cast<double>(t0 + 100 * ms);
        double now_exact = static_cast<double>(t0);
        const double real_packet = static_cast<double>(packet) * (1.0 - 50e-6);
        for (int i = 0; i < 330000; ++i, now_exact += real_packet, play_at += real_packet) {
            feed(run, static_cast<uint64_t>(play_at), static_cast<uint64_t>(now_exact));
        }
        CHECK_EQ(run.dropped, 0);
        CHECK_EQ(run.audible_gaps, 0);
        CHECK_EQ(retimer.steps(), uint64_t(0));
    }

    // Jitter in the startup window with the stream already running late by
    // less than kJitterNs: kept (one restart), not dropped.
    {
        restart();
        RetimerRun run;
        uint64_t now = t0;
        uint64_t play_at = t0;
        feed(run, play_at, now);  // starts kStartLeadNs ahead
        play_at += packet;
        now += packet + AudioRetimer::kStartLeadNs + 50 * ms;  // one packet 90 ms late
        const auto r = feed(run, play_at, now);
        CHECK(!r.drop);
        CHECK(r.timestamp >= now);
        CHECK_EQ(retimer.resyncs(), uint64_t(1));
        CHECK_EQ(retimer.dropped(), uint64_t(0));
    }

    // reset(): a new stream starts over.
    restart();
    CHECK(retimer.process(t0 - 100 * ms, packet, t0).drop);
    CHECK_EQ(retimer.dropped(), uint64_t(1));
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
    testAudioFormats();
    testPlistChecked();
    testAudioRetimer();
    if (g_failures) {
        std::fprintf(stderr, "unit-tests: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("unit-tests: all checks passed\n");
    return 0;
}
