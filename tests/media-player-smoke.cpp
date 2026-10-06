#include "media-player.hpp"

extern "C" {
#include <libavutil/log.h>
}

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
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

// AirPlay video controls: start mid-file, pause, seek while paused, resume.
// Needs a >= 30 s file whose keyframes are ~10 s apart so pre-roll matters.
int seekSync(const char* location)
{
    std::atomic<unsigned int> video_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0) ++video_frames;
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    const auto wait_for = [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };

    player.play(location, 20.0);
    for (int i = 0; i < 200 && video_frames.load() == 0; ++i) wait_for(50);
    wait_for(1500);
    // Without pre-roll discard the frames from the keyframe at 10 s play out
    // first and the position would still be pinned at 20 s.
    const double started = player.getPlaybackInfo().position;
    bool start_ok = video_frames.load() > 0 && started > 20.8 && started < 23.0;

    // A small corrective scrub while playing is ignored (no re-seek hitch):
    // the output keeps running from where it is.
    const double before_correction = player.getPlaybackInfo().position;
    player.seek(before_correction + 0.1);
    wait_for(400);
    const double after_correction = player.getPlaybackInfo().position;
    const bool correction_ok = after_correction > before_correction + 0.25;
    start_ok = start_ok && correction_ok;
    std::printf("small-correction before=%.3f after=%.3f -> %s\n", before_correction,
                after_correction, correction_ok ? "ok" : "FAILED");

    player.setRate(0.0f);
    wait_for(300);
    const unsigned int before_seek = video_frames.load();
    player.seek(5.0);
    wait_for(1500);
    const unsigned int after_seek = video_frames.load();
    const double paused_position = player.getPlaybackInfo().position;
    wait_for(500);
    const bool preview_ok = after_seek == before_seek + 1 &&
        video_frames.load() == after_seek &&
        paused_position >= 4.98 && paused_position < 5.2;

    player.setRate(1.0f);
    wait_for(1200);
    const double resumed = player.getPlaybackInfo().position;
    const bool resume_ok = resumed > 5.6 && resumed < 7.5;
    player.stop();

    // A new item that starts paused (still clip) must show exactly one frame
    // at its start position, and replace the previous item promptly.
    const unsigned int before_paused_start = video_frames.load();
    const auto replace_started = std::chrono::steady_clock::now();
    player.play(location, 3.0);
    player.setRate(0.0f);
    for (int i = 0; i < 100 && video_frames.load() == before_paused_start; ++i) wait_for(20);
    const double replace_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - replace_started).count();
    wait_for(700);
    const unsigned int paused_start_frames = video_frames.load() - before_paused_start;
    const double paused_start_position = player.getPlaybackInfo().position;
    player.stop();
    const bool paused_start_ok = paused_start_frames == 1 &&
        paused_start_position >= 2.98 && paused_start_position < 3.2 && replace_ms < 2000.0;
    std::printf("paused-start frames=%u position=%.3f first_frame_ms=%.1f -> %s\n",
                paused_start_frames, paused_start_position, replace_ms,
                paused_start_ok ? "ok" : "FAILED");
    start_ok = start_ok && paused_start_ok;

    std::printf("seek-sync start=%.3f preview_frames=%u paused=%.3f resumed=%.3f -> %s\n",
                started, after_seek - before_seek, paused_position, resumed,
                start_ok && preview_ok && resume_ok ? "ok" : "FAILED");
    return start_ok && preview_ok && resume_ok ? 0 : 1;
}

// A still clip (frames far apart) must not skip to its end when the sender
// sends rate 1 right after the first frame, and must hold at the end.
int sparseClip(const char* location)
{
    std::atomic<unsigned int> video_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0]) ++video_frames;
    });
    const auto started = std::chrono::steady_clock::now();
    player.play(location, 0.0);
    for (int i = 0; i < 200 && video_frames.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    player.setRate(1.0f);
    MediaPlaybackInfo info;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        info = player.getPlaybackInfo();
    } while (!info.ended && std::chrono::steady_clock::now() - started < std::chrono::seconds(6));
    const double ended_after = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    player.stop();
    const bool ok = info.ended && ended_after > 1.7 && video_frames.load() >= 1;
    std::printf("sparse-clip ended_after=%.3fs frames=%u -> %s\n", ended_after,
                video_frames.load(), ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// Scrubber drag as sent by an AVPlayer relay: rate 0, then /scrub at 10 Hz
// across the file. The player must survive every seek (no false end of
// stream), show the final position, and resume from it.
int scrubDrag(const char* location)
{
    std::atomic<unsigned int> video_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0]) ++video_frames;
    });
    const auto wait_for = [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
    player.play(location, 30.0);
    for (int i = 0; i < 200 && video_frames.load() == 0; ++i) wait_for(20);
    wait_for(500);
    player.setRate(0.0f);
    const double targets[] = {40, 48, 52, 44, 35, 27, 18, 12, 9, 14, 22, 31, 39, 45,
                              50, 55, 51, 42, 33, 26, 20, 23, 28, 31, 30.5};
    for (double target : targets) {
        player.seek(target);
        wait_for(100);
    }
    // A held scrubber re-sends the final position; no extra frames follow.
    wait_for(800);
    const unsigned int frames_before_repeats = video_frames.load();
    for (int i = 0; i < 10; ++i) {
        player.seek(30.5);
        wait_for(50);
    }
    const bool repeats_ignored = video_frames.load() == frames_before_repeats;
    wait_for(1500);
    MediaPlaybackInfo info = player.getPlaybackInfo();
    const bool paused_ok = !info.ended && info.position >= 30.45 && info.position < 30.7 &&
        repeats_ignored;
    const unsigned int frames_before_resume = video_frames.load();
    player.setRate(1.0f);
    wait_for(1500);
    info = player.getPlaybackInfo();
    const bool resume_ok = !info.ended && info.position > 31.2 && info.position < 32.6 &&
        video_frames.load() > frames_before_resume + 10;
    player.stop();
    std::printf("scrub-drag paused_ok=%d resume_ok=%d position=%.3f -> %s\n", paused_ok,
                resume_ok, info.position, paused_ok && resume_ok ? "ok" : "FAILED");
    return paused_ok && resume_ok ? 0 : 1;
}

// Item switch as a relay sender does it: a video is paused and seeked
// (preview frame), then the player switches to a two-frame still clip at
// rate 1. The still must show and last its two seconds.
int switchToStill(const char* video, const char* still)
{
    std::atomic<unsigned int> video_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0]) ++video_frames;
    });
    const auto wait_for = [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
    player.play(video, 20.0);
    player.seek(20.0);
    player.setRate(0.0f);
    wait_for(1200);
    player.pauseForPlaylistRemoval();
    const unsigned int before = video_frames.load();
    const auto started = std::chrono::steady_clock::now();
    player.play(still, 0.0);
    player.setRate(1.0f);
    MediaPlaybackInfo info;
    do {
        wait_for(20);
        info = player.getPlaybackInfo();
    } while (!info.ended && std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    const double ended_after = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const unsigned int still_frames = video_frames.load() - before;
    player.stop();
    const bool ok = still_frames >= 1 && ended_after > 1.7;
    std::printf("switch-to-still frames=%u ended_after=%.3fs position=%.3f -> %s\n",
                still_frames, ended_after, info.position, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// An audio-only item paused before it starts must wait, not decode itself
// to the end, and play from the start once resumed.
int pausedAudio(const char* location)
{
    std::atomic<unsigned int> audio_frames{0};
    MediaPlayer player;
    player.setVideoCallback([](const MediaVideoFrame&) {});
    player.setAudioCallback([&](const MediaAudioFrame&) { ++audio_frames; });
    player.play(location, 0.0);
    player.setRate(0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const MediaPlaybackInfo paused = player.getPlaybackInfo();
    const unsigned int paused_frames = audio_frames.load();
    player.setRate(1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const MediaPlaybackInfo resumed = player.getPlaybackInfo();
    player.stop();
    const bool ok = !paused.ended && paused_frames == 0 && paused.position < 0.1 &&
        audio_frames.load() > 0 && resumed.position > 0.5 && resumed.position < 1.5;
    std::printf("paused-audio paused_ended=%d paused_frames=%u resumed_position=%.3f -> %s\n",
                paused.ended, paused_frames, resumed.position, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// How quickly a paused scrub puts a picture on screen, and whether a drag
// (scrubs 50 ms apart, as an AVPlayer relay sends them) shows pictures while
// it is going or only once it stops.
//   --scrub-latency URL [min_drag_frames [drag_gap_ms]]
int scrubLatency(const char* location, int min_drag_frames, int drag_gap_ms)
{
    std::atomic<unsigned int> video_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0]) ++video_frames;
    });
    const auto wait_for = [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto ms_since = [&](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(now() - t).count();
    };

    player.play(location, 5.0);
    for (int i = 0; i < 300 && video_frames.load() == 0; ++i) wait_for(20);
    wait_for(300);
    player.setRate(0.0f);
    wait_for(400);
    const double duration = player.getPlaybackInfo().duration;

    // Single scrubs: time until the first picture, and until pictures stop.
    const double fractions[] = {0.21, 0.47, 0.33, 0.78, 0.52, 0.64};
    double worst_first = 0.0;
    double total_first = 0.0;
    int measured = 0;
    for (double fraction : fractions) {
        const unsigned int before = video_frames.load();
        const auto started = now();
        player.seek(duration * fraction);
        double first = -1.0;
        while (ms_since(started) < 3000.0) {
            if (first < 0.0 && video_frames.load() != before) first = ms_since(started);
            if (first >= 0.0 && ms_since(started) > first + 400.0) break;
            wait_for(2);
        }
        std::printf("scrub to %7.2fs: first picture after %6.1f ms, pictures shown %u\n",
                    duration * fraction, first, video_frames.load() - before);
        if (first >= 0.0) {
            worst_first = std::max(worst_first, first);
            total_first += first;
            ++measured;
        }
    }

    // A drag: 30 scrubs, drag_gap_ms apart, sweeping forward.
    const unsigned int before_drag = video_frames.load();
    const auto drag_started = now();
    for (int i = 0; i < 30; ++i) {
        player.seek(duration * (0.20 + 0.015 * i));
        wait_for(drag_gap_ms);
    }
    const double drag_ms = ms_since(drag_started);
    const unsigned int during_drag = video_frames.load() - before_drag;
    wait_for(1500);
    const unsigned int after_drag = video_frames.load() - before_drag - during_drag;
    const double final_position = player.getPlaybackInfo().position;
    const double final_target = duration * (0.20 + 0.015 * 29);
    player.stop();

    const bool settled = std::fabs(final_position - final_target) < 0.2;
    const bool ok = measured == 6 && settled && static_cast<int>(during_drag) >= min_drag_frames;
    std::printf("scrub-latency: single scrub first picture avg %.1f ms worst %.1f ms; "
                "drag of 30 scrubs in %.0f ms showed %u pictures during, %u after; "
                "settled at %.3fs (target %.3fs) -> %s\n",
                measured ? total_first / measured : -1.0, worst_first, drag_ms, during_drag,
                after_drag, final_position, final_target, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && argc <= 5 && std::strcmp(argv[1], "--scrub-latency") == 0) {
        return scrubLatency(argv[2], argc >= 4 ? std::atoi(argv[3]) : 0,
                            argc == 5 ? std::atoi(argv[4]) : 50);
    }
    if (std::getenv("MEDIA_SMOKE_FFMPEG_VERBOSE")) {
        av_log_set_level(AV_LOG_VERBOSE);
    }
    if (argc == 3 && std::strcmp(argv[1], "--seek-sync") == 0) {
        return seekSync(argv[2]);
    }
    if (argc == 4 && std::strcmp(argv[1], "--switch-to-still") == 0) {
        return switchToStill(argv[2], argv[3]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--scrub-drag") == 0) {
        return scrubDrag(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--paused-audio") == 0) {
        return pausedAudio(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--sparse-clip") == 0) {
        return sparseClip(argv[2]);
    }
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
        std::fprintf(stderr, "usage: %s [--startup] [--require-audio] MEDIA_URL | --seek-sync MEDIA_URL | --sparse-clip MEDIA_URL | --scrub-drag MEDIA_URL | --paused-audio MEDIA_URL\n", argv[0]);
        return 2;
    }

    std::atomic<unsigned int> video_frames{0};
    std::atomic<unsigned int> audio_frames{0};
    // The first picture: size, layout and the luma at the centre of each
    // quadrant (top-left, top-right, bottom-left, bottom-right), to check
    // orientation and range.
    int first_width = 0;
    int first_height = 0;
    bool first_nv12 = false;
    bool first_full_range = false;
    int first_luma[4] = {0, 0, 0, 0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0 && frame.height > 0 && frame.timestamp_ns > 0) {
            if (video_frames.load() == 0) {
                first_width = frame.width;
                first_height = frame.height;
                first_nv12 = frame.nv12;
                first_full_range = frame.full_range;
                for (int q = 0; q < 4; ++q) {
                    const int x = frame.width / 4 + (q % 2) * (frame.width / 2);
                    const int y = frame.height / 4 + (q / 2) * (frame.height / 2);
                    first_luma[q] = frame.data[0][y * frame.linesize[0] + x];
                }
            }
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
    std::printf("first-picture=%dx%d %s %s-range quadrant-luma=%d,%d,%d,%d\n", first_width,
                first_height, first_nv12 ? "nv12" : "i420", first_full_range ? "full" : "video",
                first_luma[0], first_luma[1], first_luma[2], first_luma[3]);
    return passed ? 0 : 1;
}
