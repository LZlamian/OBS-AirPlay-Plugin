#include "media-player.hpp"

#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" void blog(int, const char* format, ...)
{
    std::fputs("media-smoke: ", stderr);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

extern "C" uint64_t os_gettime_ns(void)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int main(int argc, char** argv)
{
    bool startup_only = false;
    bool require_audio = false;
    const char* location = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--startup") == 0) {
            startup_only = true;
        } else if (std::strcmp(argv[i], "--require-audio") == 0) {
            require_audio = true;
        } else if (!location) {
            location = argv[i];
        } else {
            location = nullptr;
            break;
        }
    }
    if (!location) {
        std::fprintf(stderr, "usage: %s [--startup] [--require-audio] MEDIA_URL\n", argv[0]);
        return 2;
    }

    std::atomic<unsigned int> video_frames{0};
    std::atomic<unsigned int> audio_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0 && frame.height > 0 && frame.timestamp_ns > 0) {
            ++video_frames;
        }
    });
    player.setAudioCallback([&](const MediaAudioFrame& frame) {
        if (frame.data[0] && frame.data[1] && frame.frames > 0 &&
            frame.sample_rate > 0 && frame.timestamp_ns > 0) {
            ++audio_frames;
        }
    });

    player.play(location, 0.0);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(startup_only ? 25 : 15);
    MediaPlaybackInfo info;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        info = player.getPlaybackInfo();
        const bool advertised_frames =
            (!info.has_video || video_frames.load() > 0) &&
            (!info.has_audio || audio_frames.load() > 0);
        const bool audio_requirement = !require_audio ||
            (info.has_audio && audio_frames.load() > 0);
        if (startup_only && info.ready_to_play &&
            (info.has_video || info.has_audio) && advertised_frames && audio_requirement) {
            break;
        }
    } while (!info.ended && std::chrono::steady_clock::now() < deadline);
    player.stop();

    const bool advertised_frames =
        (!info.has_video || video_frames.load() > 0) &&
        (!info.has_audio || audio_frames.load() > 0);
    const bool audio_requirement = !require_audio ||
        (info.has_audio && audio_frames.load() > 0);
    const bool passed = (startup_only ? info.ready_to_play : info.ended) &&
        (info.has_video || info.has_audio) && advertised_frames && audio_requirement;
    std::printf("ready=%d ended=%d video=%d audio=%d video_frames=%u audio_frames=%u duration=%.3f position=%.3f mode=%s\n",
                info.ready_to_play, info.ended, info.has_video, info.has_audio,
                video_frames.load(), audio_frames.load(), info.duration, info.position,
                startup_only ? "startup" : "complete");
    return passed ? 0 : 1;
}
