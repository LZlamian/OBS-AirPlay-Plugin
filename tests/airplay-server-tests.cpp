// SPDX-License-Identifier: GPL-3.0-or-later
// AirPlayServer (src/airplay-server.cpp) without OBS: the libobs calls it
// makes are stubbed below and record what reaches the "source". Covers the
// mirror decode thread (every payload in order, the backlog dropped when it
// falls behind and decoding resumed at the next keyframe), the shared clock
// anchor between mirror video and audio, the audio re-timer wiring, and
// resetDecoders clearing the output.
//   obs-airplay-airplay-server-tests H264_ANNEX_B_FILE
#include "airplay-server.hpp"
#include "test-audio.hpp"
#include "test-timing.hpp"

#include <obs.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex g_mutex;
std::vector<uint64_t> g_video_timestamps;  // one per frame; 0 for a clear
std::vector<uint64_t> g_audio_timestamps;
std::atomic<int> g_output_delay_ms{0};
std::atomic<int> g_fell_behind{0};
int g_failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%-62s %s\n", what, condition ? "ok" : "FAILED");
    if (!condition) ++g_failures;
}

uint64_t now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Access units of an Annex B H.264 file, split by FFmpeg's parser.
std::vector<std::vector<uint8_t>> accessUnits(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    std::vector<std::vector<uint8_t>> units;
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext* context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    size_t at = 0;
    while (parser && context) {
        uint8_t* unit = nullptr;
        int unit_size = 0;
        const int remaining = static_cast<int>(bytes.size() - at);
        const int used = av_parser_parse2(parser, context, &unit, &unit_size,
                                          remaining ? bytes.data() + at : nullptr, remaining,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) break;
        at += static_cast<size_t>(used);
        if (unit_size > 0) units.emplace_back(unit, unit + unit_size);
        if (remaining == 0) break;
    }
    if (parser) av_parser_close(parser);
    avcodec_free_context(&context);
    return units;
}

size_t videoFrames()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    size_t frames = 0;
    for (uint64_t timestamp : g_video_timestamps) frames += timestamp != 0;
    return frames;
}

void reset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_video_timestamps.clear();
    g_audio_timestamps.clear();
}

} // namespace

// ---- libobs and plugin functions AirPlayServer calls ----
extern "C" {
void blog(int, const char* format, ...)
{
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (std::strstr(message, "fell")) ++g_fell_behind;
    if (std::getenv("SERVER_TESTS_VERBOSE")) std::fprintf(stderr, "blog: %s\n", message);
}
uint64_t os_gettime_ns(void)
{
    return now_ns();
}
obs_weak_source_t* obs_source_get_weak_source(obs_source_t* source)
{
    return reinterpret_cast<obs_weak_source_t*>(source);
}
obs_source_t* obs_weak_source_get_source(obs_weak_source_t* weak)
{
    return reinterpret_cast<obs_source_t*>(weak);
}
void obs_weak_source_release(obs_weak_source_t*) {}
void obs_source_release(obs_source_t*) {}
void obs_source_output_video(obs_source_t*, const struct obs_source_frame* frame)
{
    if (frame && g_output_delay_ms.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_output_delay_ms.load()));
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_video_timestamps.push_back(frame ? frame->timestamp : 0);
}
void obs_source_output_audio(obs_source_t*, const struct obs_source_audio* audio)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_audio_timestamps.push_back(audio->timestamp);
}
bool video_format_get_parameters_for_format(enum video_colorspace, enum video_range_type,
                                            enum video_format, float matrix[16],
                                            float min_range[3], float max_range[3])
{
    for (int i = 0; i < 16; ++i) matrix[i] = i % 5 == 0 ? 1.0f : 0.0f;
    for (int i = 0; i < 3; ++i) {
        min_range[i] = 0.0f;
        max_range[i] = 1.0f;
    }
    return true;
}
}
void airplay_source_notify_frame_queued(uint64_t) {}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s H264_ANNEX_B_FILE\n", argv[0]);
        return 2;
    }
    const std::vector<std::vector<uint8_t>> units = accessUnits(argv[1]);
    if (units.size() < 60) {
        std::fprintf(stderr, "airplay-server-tests: %zu access units in %s\n", units.size(), argv[1]);
        return 2;
    }
    int fake_source = 0;
    obs_source_t* source = reinterpret_cast<obs_source_t*>(&fake_source);
    constexpr uint64_t kFrameNs = 1000000000ULL / 30;
    constexpr uint64_t ms = 1000000;

    // Every payload decoded and output in order, timed from the first one.
    // Sent at three times the sender's pace (all at once would overflow the
    // backlog while VideoToolbox starts, as the second case shows).
    {
        AirPlayServer server;
        server.registerSource(source);
        const uint64_t pts0 = 5000 * ms;
        const uint64_t started = now_ns();
        for (size_t i = 0; i < units.size(); ++i) {
            server.ingestVideoBitstream(units[i].data(), units[i].size(), pts0 + i * kFrameNs,
                                        false);
            std::this_thread::sleep_for(std::chrono::milliseconds(11));
        }
        waitUntil([&] { return videoFrames() >= units.size() - 2; }, 10000);
        std::vector<uint64_t> timestamps;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            timestamps = g_video_timestamps;
        }
        bool increasing = !timestamps.empty();
        for (size_t i = 1; i < timestamps.size(); ++i) {
            increasing = increasing && timestamps[i] > timestamps[i - 1];
        }
        check(timestamps.size() >= units.size() - 2, "mirror payloads decoded and output");
        check(increasing, "frame timestamps increase");
        check(!timestamps.empty() && timestamps[0] >= started && timestamps[0] < started + scaledMs(200) * ms,
              "the first frame is anchored at its arrival");
        check(timestamps.size() >= 31 &&
                  timestamps[30] - timestamps[0] > 30 * kFrameNs - ms &&
                  timestamps[30] - timestamps[0] < 30 * kFrameNs + ms,
              "later frames keep the sender's spacing");
        server.resetDecoders(true, true);
        bool cleared = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            cleared = !g_video_timestamps.empty() && g_video_timestamps.back() == 0;
        }
        check(cleared, "resetDecoders clears the output");
        server.stop();
    }

    // A decoder that falls behind: the backlog is dropped and decoding goes
    // on from the next keyframe. The stream (keyframes 30 frames apart) is
    // sent at once while OBS takes 25 ms per frame, then again at a pace the
    // decoder keeps up with: the second pass is decoded from its first frame.
    {
        reset();
        g_fell_behind = 0;
        g_output_delay_ms = 25;
        AirPlayServer server;
        server.registerSource(source);
        uint64_t pts = 9000 * ms;
        for (const auto& unit : units) {
            server.ingestVideoBitstream(unit.data(), unit.size(), pts, false);
            pts += kFrameNs;
        }
        const bool dropped = g_fell_behind.load() > 0;
        // Whatever was queued before the drop finishes; nothing after it is
        // queued until a keyframe arrives.
        waitUntil([&] { return videoFrames() > 0; }, 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(scaledMs(300)));
        g_output_delay_ms = 0;
        const size_t first_pass = videoFrames();
        for (const auto& unit : units) {
            server.ingestVideoBitstream(unit.data(), unit.size(), pts, false);
            pts += kFrameNs;
            std::this_thread::sleep_for(std::chrono::milliseconds(11));
        }
        const bool resumed =
            waitUntil([&] { return videoFrames() >= first_pass + units.size() - 2; }, 10000);
        check(dropped, "a backlog of 60 payloads is dropped");
        check(first_pass < units.size() / 2, "the dropped payloads are not decoded");
        check(resumed, "decoding resumes at the next keyframe");
        server.stop();
    }

    // Audio first (audio-only AirPlay, or audio before the first picture):
    // the clock is anchored on it, it starts kStartLeadNs ahead, and video
    // that follows keeps its offset from the audio.
    const std::vector<std::vector<uint8_t>> aac = encodeAacLc(0.5);
    if (aac.empty()) {
        std::printf("no AAC encoder in this FFmpeg: audio checks skipped\n");
    } else {
        reset();
        AirPlayServer server;
        server.registerSource(source);
        const uint64_t pts0 = 20000 * ms;
        const uint64_t started = now_ns();
        constexpr uint64_t kPacketNs = 1024ULL * 1000000000ULL / 44100;
        for (size_t i = 0; i < 20 && i < aac.size(); ++i) {
            server.ingestAudioBitstream(aac[i].data(), aac[i].size(), 4, pts0 + i * kPacketNs);
        }
        server.ingestVideoBitstream(units[0].data(), units[0].size(), pts0 + 100 * ms, false);
        waitUntil([&] { return videoFrames() >= 1; }, 5000);
        std::vector<uint64_t> audio;
        uint64_t video = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            audio = g_audio_timestamps;
            if (!g_video_timestamps.empty()) video = g_video_timestamps.front();
        }
        const uint64_t lead = AudioRetimer::kStartLeadNs;
        check(!audio.empty() && audio[0] >= started + lead && audio[0] < started + lead + scaledMs(50) * ms,
              "audio-first: starts kStartLeadNs ahead of its arrival");
        bool continuous = audio.size() >= 10;
        for (size_t i = 1; i < audio.size(); ++i) {
            const uint64_t step = audio[i] - audio[i - 1];
            continuous = continuous && step > kPacketNs - ms && step < kPacketNs + ms;
        }
        check(continuous, "audio packets follow each other");
        check(video != 0 && video >= started + 100 * ms - 5 * ms &&
                  video < started + 100 * ms + scaledMs(60) * ms,
              "video after audio-first keeps the sender's offset");
        server.stop();
    }

    std::printf("airplay-server-tests -> %s\n", g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
