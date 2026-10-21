// SPDX-License-Identifier: GPL-3.0-or-later
#include "media-player.hpp"
#include "media-url-policy.hpp"
#include "test-timing.hpp"
#include "url-policy-cases.hpp"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/log.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Set when the player logs that it continued a cut-off response (--resilient).
std::atomic<int> g_resumed_responses{0};
// Every log line, for modes that check what the player reported.
std::mutex g_log_mutex;
std::string g_log;

extern "C" void blog(int, const char* format, ...)
{
    char message[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::fprintf(stderr, "media-smoke: %s\n", message);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_log += message;
        g_log += '\n';
    }
    int reopens = 0;
    if (std::sscanf(message, "[MEDIA] media server responses ended early %d", &reopens) == 1) {
        g_resumed_responses.store(reopens);
    }
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
    waitUntil([&] { return !(video_frames.load() == 0); }, 10000, 50);
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
    waitUntil([&] { return !(video_frames.load() == before_paused_start); }, 2000, 20);
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
    waitUntil([&] { return !(video_frames.load() == 0); }, 1000, 5);
    player.setRate(1.0f);
    MediaPlaybackInfo info;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        info = player.getPlaybackInfo();
    } while (!info.ended && std::chrono::steady_clock::now() - started <
                                std::chrono::milliseconds(scaledMs(9000)));
    const double ended_after = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    player.stop();
    // Its last frame (at 4 s) is shown for its 2 s: the item lasts 6 s.
    const bool ok = info.ended && ended_after > 5.7 && video_frames.load() >= 3;
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
    waitUntil([&] { return !(video_frames.load() == 0); }, 4000, 20);
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
    waitUntil([&] { return !(video_frames.load() == 0); }, 6000, 20);
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

// Following the sender's clock (MediaPlayer::setFollowSenderClock): the
// output is where a sender that started at the command, and has played on
// since, would be: after a start mid-file, a jump while playing, and a
// resume. Needs a file of 90 s or more.
int followClock(const char* location)
{
    using clock = std::chrono::steady_clock;
    std::atomic<unsigned int> video_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0) ++video_frames;
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    const auto wait_for = [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
    const auto since = [](clock::time_point from) {
        return std::chrono::duration<double>(clock::now() - from).count();
    };
    // The output may be a frame or two off the sender's clock. Shared CI
    // runners are slower and noisier: OBS_AIRPLAY_TIMING_SCALE widens this.
    const double timing_scale = timingScale();
    const double kTolerance = 0.12 * timing_scale;
    bool ok = true;
    const auto check = [&](const char* what, double position, double expected) {
        const bool good = std::fabs(position - expected) <= kTolerance;
        std::fprintf(stderr, "media-smoke: follow-clock %s: output %.3fs, sender %.3fs (%+.3fs) %s\n",
                     what, position, expected, position - expected, good ? "ok" : "FAILED");
        ok = ok && good;
    };

    MediaPlayer::setFollowSenderClock(true);
    auto anchor = clock::now();
    player.play(location, 21.0);
    waitUntil([&] { return !(video_frames.load() == 0); }, 2000, 10);
    const double first_wait = since(anchor);
    wait_for(1500);
    check("start", player.getPlaybackInfo().position, 21.0 + since(anchor));

    anchor = clock::now();
    player.seek(61.0);
    wait_for(1200);
    check("jump while playing", player.getPlaybackInfo().position, 61.0 + since(anchor));

    player.setRate(0.0f);
    wait_for(600);
    const double paused_at = player.getPlaybackInfo().position;
    anchor = clock::now();
    player.setRate(1.0f);
    wait_for(1200);
    check("resume", player.getPlaybackInfo().position, paused_at + since(anchor));

    // A jump while paused shows that frame; playing then starts from it.
    player.setRate(0.0f);
    player.seek(41.0);
    wait_for(800);
    check("jump while paused", player.getPlaybackInfo().position, 41.0);
    anchor = clock::now();
    player.setRate(1.0f);
    wait_for(1200);
    check("resume after a paused jump", player.getPlaybackInfo().position, 41.0 + since(anchor));
    player.stop();

    // A slow open (0.6 s here), as over a network: still in step, and the
    // frames the sender has passed are skipped, not shown in a burst. With a
    // sound track too, where the audio reaches the sender's position first.
    MediaPlayer::setOpenDelayForTesting(600);
    video_frames.store(0);
    anchor = clock::now();
    player.play(location, 21.0);
    waitUntil([&] { return !(video_frames.load() == 0); }, 2000, 5);
    wait_for(300);
    const unsigned int after_start = video_frames.load();
    wait_for(900);
    check("slow open, start", player.getPlaybackInfo().position, 21.0 + since(anchor));
    MediaPlayer::setOpenDelayForTesting(0);
    video_frames.store(0);
    anchor = clock::now();
    player.seek(61.0);
    waitUntil([&] { return !(video_frames.load() == 0); }, 2000, 5);
    wait_for(300);
    const unsigned int after_jump = video_frames.load();
    player.stop();
    // 0.3 s of a 30 fps video is 9 or 10 pictures; a burst of skipped ones
    // would be many more.
    const unsigned int max_pictures = static_cast<unsigned int>(9 + 5 * timing_scale);
    const bool no_burst = after_start >= 1 && after_start <= max_pictures && after_jump >= 1 &&
        after_jump <= max_pictures;
    std::fprintf(stderr, "media-smoke: follow-clock pictures in the first 0.3 s: %u after a slow start, %u after a jump %s\n",
                 after_start, after_jump, no_burst ? "ok" : "FAILED");
    ok = ok && no_burst;

    // Off: the start waits for the first picture and begins at the position named.
    MediaPlayer::setFollowSenderClock(false);
    video_frames.store(0);
    player.play(location, 21.0);
    waitUntil([&] { return !(video_frames.load() == 0); }, 2000, 10);
    const auto first_frame = clock::now();
    wait_for(1000);
    check("off, start", player.getPlaybackInfo().position, 21.0 + since(first_frame));
    player.stop();

    std::fprintf(stderr, "media-smoke: follow-clock first picture %.0f ms after the start; %s\n",
                 first_wait * 1000.0, ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}

// Following the sender's clock, an item shorter than the time it took to
// open (a 2 s still clip over a slow network): by then the sender is past
// its end, and it must still show a picture: also a still of one frame a
// second, or of a single frame, whose last frame starts long before the end.
// CLIP is 3 s or shorter. With "quick", the file opens at once: the frame at
// the start is the one on screen and must be shown without waiting for the next.
int followClockShortItem(const char* location, bool quick = false)
{
    std::atomic<unsigned int> video_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0) ++video_frames;
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    MediaPlayer::setFollowSenderClock(true);
    MediaPlayer::setOpenDelayForTesting(quick ? 300 : 3500);
    player.play(location, 0.0);
    // (quick: within 0.7 s, before a second frame a second in could arrive)
    for (int i = 0; i < (quick ? 14 : 120) && video_frames.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!quick) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const unsigned int shown = video_frames.load();
    player.stop();
    MediaPlayer::setOpenDelayForTesting(0);
    MediaPlayer::setFollowSenderClock(false);
    std::fprintf(stderr, "media-smoke: follow-clock short item opened %s: %u picture(s) -> %s\n",
                 quick ? "in 0.3 s" : "late", shown, shown > 0 ? "ok" : "FAILED");
    return shown > 0 ? 0 : 1;
}

// stop() racing a paused worker and concurrent play()/stop() callers (the
// HTTP, mirror, deferred-clear and OBS threads all reach the player). A lost
// wake-up hangs stop(); unsynchronised `worker` access ends in std::terminate.
int stopRace(const char* location, int iterations)
{
    std::atomic<unsigned int> progress{0};
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        unsigned int last = progress.load();
        auto last_change = std::chrono::steady_clock::now();
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const unsigned int now_progress = progress.load();
            if (now_progress != last) {
                last = now_progress;
                last_change = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - last_change > std::chrono::seconds(10)) {
                std::fprintf(stderr, "media-smoke: stop-race hung after %u steps -> FAILED\n",
                             now_progress);
                std::fflush(stderr);
                std::_Exit(1);
            }
        }
    });

    MediaPlayer player;
    player.setVideoCallback([](const MediaVideoFrame&) {});
    player.setAudioCallback([](const MediaAudioFrame&) {});
    for (int i = 0; i < iterations; ++i) {
        player.play(location, 0.0);
        player.setRate(0.0f);
        // Vary where stop() lands relative to the worker reaching its paused wait.
        if (i % 4) std::this_thread::sleep_for(std::chrono::microseconds((i % 7) * 400));
        player.stop();
        ++progress;
    }
    const auto paused_steps = progress.load();

    std::thread player_thread([&] {
        for (int i = 0; i < iterations; ++i) {
            player.play(location, 0.0);
            if (i % 3 == 0) player.setRate(0.0f);
            ++progress;
        }
    });
    std::thread stop_thread([&] {
        for (int i = 0; i < iterations; ++i) {
            player.stop();
            if (i % 5) std::this_thread::sleep_for(std::chrono::microseconds((i % 5) * 300));
            ++progress;
        }
    });
    player_thread.join();
    stop_thread.join();
    player.stop();
    done.store(true);
    watchdog.join();
    std::fprintf(stderr, "media-smoke: stop-race paused=%u concurrent=%u -> ok\n",
                 paused_steps, progress.load() - paused_steps);
    return 0;
}

// Frames delivered within timeout_ms of play(), then stop().
unsigned int framesWithin(const char* location, int timeout_ms)
{
    std::atomic<unsigned int> frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0) ++frames;
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    player.play(location, 0.0);
    waitUntil([&] { return !(frames.load() == 0); }, timeout_ms, 50);
    player.stop();
    return frames.load();
}

// URL policy decisions for the resolved/nested checks (no network needed).
int urlPolicy()
{
    using namespace media_url_policy;
    setLoopbackAllowedForTesting(false);
    setLocalFilesAllowedForTesting(false);
    // Names answer from a table, not the network: cdn.example.test is a
    // public address, rebind.example.test a loopback one, others none.
    setResolverForTesting([](const std::string& host, std::vector<sockaddr_storage>& out) {
        sockaddr_storage address = {};
        auto& v4 = reinterpret_cast<sockaddr_in&>(address);
        v4.sin_family = AF_INET;
        if (host == "cdn.example.test") {
            inet_pton(AF_INET, "93.184.216.34", &v4.sin_addr);
        } else if (host == "rebind.example.test") {
            inet_pton(AF_INET, "127.0.0.1", &v4.sin_addr);
        } else {
            return false;
        }
        out.push_back(address);
        return true;
    });
    const char* item = "https://cdn.example.test/master.m3u8";
    struct Case { const char* url; bool allowed; };
    const Case nested[] = {
        {"https://cdn.example.test/seg1.ts", true},
        {"https://cdn.example.test:443/key", true},
        {"http://cdn.example.test:8080/seg.ts", true},
        // A name that does not resolve is refused (fail closed), and one
        // that resolves to a refused address.
        {"http://segments.invalid/seg.ts", false},
        {"http://rebind.example.test/seg.ts", false},
        {"http://localhost:8080/x", false},
    };
    int failures = 0;
    for (const SharedPolicyCase& c : kSharedPolicyCases) {
        const std::string refused = checkNested(c.url, item);
        if (refused.empty() != c.allowed) {
            std::fprintf(stderr, "media-smoke: url-policy shared %s -> %s (expected %s)\n", c.url,
                         refused.empty() ? "allowed" : refused.c_str(),
                         c.allowed ? "allowed" : "refused");
            ++failures;
        }
    }
    for (const Case& c : nested) {
        const std::string refused = checkNested(c.url, item);
        if (refused.empty() != c.allowed) {
            std::fprintf(stderr, "media-smoke: url-policy nested %s -> %s (expected %s)\n", c.url,
                         refused.empty() ? "allowed" : refused.c_str(),
                         c.allowed ? "allowed" : "refused");
            ++failures;
        }
    }
    // The receiver vetted literal item URLs with the sender's address;
    // "localhost" is only its own HLS proxy.
    if (!checkItem("http://10.0.0.5:7001/1/x.mp4").empty()) ++failures;
    if (!checkItem("http://localhost:7000/master.m3u8").empty()) ++failures;
    if (checkItem("http://foo.localhost/x.mp4").empty()) ++failures;
    if (checkItem("ftp://example.test/x").empty()) ++failures;
    if (checkLocalFile("/etc/passwd").empty()) ++failures;
    if (checkLocalFile("/tmp/../etc/obs-airplay-fcup-x").empty()) ++failures;
    if (checkLocalFile("/tmp/obs-airplay-fcup-missing").empty()) ++failures;
    // "AirPlay video: Only from the sending device": everything an item
    // fetches must be on the item's (the sender's) address, any port; a
    // named item host is refused. The HLS proxy's playlists stay allowed,
    // but not the hosts they point at.
    setSenderOnly(true);
    const char* sender_item = "http://192.168.1.20:7001/1/x.m3u8";
    const Case sender_nested[] = {
        {"http://192.168.1.20:7001/1/seg1.ts", true},
        {"http://192.168.1.20:8080/seg.ts", true},
        {"https://192.168.1.20/key", true},
        {"http://[::ffff:192.168.1.20]:7001/seg.ts", true},
        {"http://192.168.1.21/seg.ts", false},
        {"https://cdn.example.test/seg1.ts", false},
        {"http://10.0.0.1/x", false},
        {"http://127.0.0.1/x", false},
    };
    for (const Case& c : sender_nested) {
        const std::string refused = checkNested(c.url, sender_item);
        if (refused.empty() != c.allowed) {
            std::fprintf(stderr, "media-smoke: url-policy sender-only %s -> %s (expected %s)\n",
                         c.url, refused.empty() ? "allowed" : refused.c_str(),
                         c.allowed ? "allowed" : "refused");
            ++failures;
        }
    }
    if (checkNested("http://localhost:7000/media.m3u8", "http://localhost:7000/master.m3u8") != "") ++failures;
    if (checkNested("https://cdn.example.test/seg.ts", "http://localhost:7000/master.m3u8").empty()) ++failures;
    if (checkNested("http://cdn.example.test:8080/seg2.ts", item).empty()) ++failures;
    if (!checkItem("http://192.168.1.20:7001/1/x.mp4").empty()) ++failures;
    if (!checkItem("http://localhost:7000/master.m3u8").empty()) ++failures;
    if (checkItem("https://cdn.example.test/x.mp4").find("sending device") == std::string::npos) ++failures;
    setSenderOnly(false);
    if (!checkNested("http://10.0.0.1:8080/seg2.ts", item).empty()) ++failures;
    if (checkNested("http://segments.invalid/seg.ts", item).find("could not be resolved") ==
        std::string::npos) {
        ++failures;
    }
    if (checkItem("https://cdn.invalid/x.mp4").find("could not be resolved") ==
        std::string::npos) {
        ++failures;
    }
    // A name is pinned to its checked address; literals and the receiver's
    // own proxy are not.
    const Resolver table = [](const std::string& host, std::vector<sockaddr_storage>& out) {
        if (host != "cdn.example.test") return false;
        sockaddr_storage address = {};
        auto& v6 = reinterpret_cast<sockaddr_in6&>(address);
        v6.sin6_family = AF_INET6;
        inet_pton(AF_INET6, "2001:db8::7", &v6.sin6_addr);
        out.push_back(address);
        return true;
    };
    if (decideItem("http://cdn.example.test/x.mp4", table).address != "2001:db8::7") ++failures;
    if (decideNested("http://cdn.example.test/seg.ts", item, table).address != "2001:db8::7") {
        ++failures;
    }
    if (!decideItem("http://10.0.0.5:7001/x.mp4", table).address.empty()) ++failures;
    if (!decideNested("http://10.0.0.5:7001/seg.ts", "http://10.0.0.5:7001/x.mp4", table)
             .refused.empty()) {
        ++failures;
    }
    if (!decideNested("http://[fe80::1]:7001/seg.ts", "http://[fe80::1]:7001/x.m3u8", table)
             .refused.empty()) {
        ++failures;  // a link-local sender's own segments
    }
    setResolverForTesting(nullptr);

    const auto ports = parsePortList("80, 443,8080");
    if (ports.size() != 3 || ports[2] != 8080 || !parsePortList("80,http").empty()) ++failures;
    std::fprintf(stderr, "media-smoke: url-policy failures=%d -> %s\n", failures,
                 failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// HLS resources go through the policy: a playlist whose segments are on its
// own host and port plays; one whose segments point at another loopback port
// is refused (loopback is only allowed as the item's own address here).
// ResilientHttpIo: a server that closes every response after 64 KiB
// (tests/truncating-server.py). The whole clip still plays, every frame of
// it, by way of fresh range requests.
int resilient(const char* location)
{
    std::atomic<unsigned int> video_frames{0};
    std::atomic<unsigned int> audio_frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0]) ++video_frames;
    });
    player.setAudioCallback([&](const MediaAudioFrame& frame) {
        if (frame.frames > 0) ++audio_frames;
    });
    player.play(location, 0.0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    MediaPlaybackInfo info;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        info = player.getPlaybackInfo();
    } while (!info.ended && std::chrono::steady_clock::now() < deadline);
    player.stop();
    // clip8.mp4: 8 s at 30 fps.
    const unsigned int expected_frames = 240;
    const bool passed = info.ended && video_frames.load() >= expected_frames - 2 &&
        audio_frames.load() > 0 && g_resumed_responses.load() > 0;
    std::printf("resilient: ended=%d video_frames=%u (expected %u) audio_frames=%u "
                "resumed_responses=%d\n", info.ended, video_frames.load(), expected_frames,
                audio_frames.load(), g_resumed_responses.load());
    return passed ? 0 : 1;
}

int nestedPolicy(const char* same_origin, const char* cross_origin)
{
    media_url_policy::setLoopbackAllowedForTesting(false);
    const unsigned int good = framesWithin(same_origin, 8000);
    const unsigned int bad = framesWithin(cross_origin, 4000);
    media_url_policy::setLoopbackAllowedForTesting(true);
    const bool ok = good > 0 && bad == 0;
    std::fprintf(stderr, "media-smoke: nested-policy same_origin_frames=%u cross_origin_frames=%u -> %s\n",
                 good, bad, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// An HTTPS server with a self-signed certificate is refused unless the user
// allowed it.
int tlsVerify(const char* location)
{
    MediaPlayer::setAllowInsecureTls(false);
    const unsigned int verified = framesWithin(location, 4000);
    MediaPlayer::setAllowInsecureTls(true);
    const unsigned int insecure = framesWithin(location, 8000);
    MediaPlayer::setAllowInsecureTls(false);
    const bool ok = verified == 0 && insecure > 0;
    std::fprintf(stderr, "media-smoke: tls-verify verified_frames=%u allowed_frames=%u -> %s\n",
                 verified, insecure, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// A stream that does not start at 0 (MPEG-TS with -output_ts_offset 10):
// a paused seek must land on its target without decoding from a point
// start_time too early.
// `race`: the worker and presenter are slowed around each seek (as on a
// loaded CI runner), so the presenter checks frames queued before the seek
// while the worker carries it out. None of them may be shown, or clear the
// discard for the new target (which then showed the pre-roll keyframe
// before it). Only the position is checked: the delays make previews slow.
int tsSeek(const char* location, bool race = false)
{
    std::atomic<unsigned int> frames{0};
    std::atomic<int64_t> last_frame_ns{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0) {
            ++frames;
            last_frame_ns.store(static_cast<int64_t>(os_gettime_ns()));
        }
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    player.play(location, 0.0);
    waitUntil([&] { return !(frames.load() == 0); }, 5000, 25);
    player.setRate(0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (race) MediaPlayer::setSeekDelaysForTesting(30, 15);
    double worst_ms = 0.0;
    double worst_error = 0.0;
    bool all_shown = true;
    for (const double target : {20.0, 5.0, 25.0}) {
        const unsigned int before = frames.load();
        const auto started = std::chrono::steady_clock::now();
        player.seek(target);
        while (frames.load() == before &&
               std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const double position = player.getPlaybackInfo().position;
        all_shown = all_shown && frames.load() > before;
        worst_ms = std::max(worst_ms, ms);
        worst_error = std::max(worst_error, std::fabs(position - target));
        std::fprintf(stderr, "media-smoke: ts-seek target=%.3f position=%.3f preview_ms=%.1f\n",
                     target, position, ms);
    }
    player.stop();
    MediaPlayer::setSeekDelaysForTesting(0, 0);
    // One frame at 30 fps; a seek that starts 10 s early decodes 300 frames
    // of 720p before the preview and takes several times longer.
    const bool ok = all_shown && worst_error <= 1.0 / 30.0 + 1e-6 &&
                    (race || worst_ms < 400.0);
    std::fprintf(stderr, "media-smoke: ts-seek worst_error=%.4f worst_preview_ms=%.1f -> %s\n",
                 worst_error, worst_ms, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// After the end of an item, a resume is ignored and a scrub reopens the item
// at that position (paused), instead of reporting a position nothing shows.
int afterEnd(const char* location)
{
    std::atomic<unsigned int> frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0] && frame.width > 0) ++frames;
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    player.play(location, 0.0);
    MediaPlaybackInfo info;
    for (int i = 0; i < 400; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        info = player.getPlaybackInfo();
        if (info.ended) break;
    }
    const bool ended = info.ended;
    player.setRate(1.0f);
    const bool resume_ignored = player.getPlaybackInfo().rate == 0.0f &&
                                player.getPlaybackInfo().ended;
    const unsigned int before = frames.load();
    player.seek(2.0);
    waitUntil([&] { return !(frames.load() == before); }, 5000, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    info = player.getPlaybackInfo();
    const bool reopened = frames.load() > before && !info.ended && info.rate == 0.0f &&
                          std::fabs(info.position - 2.0) < 0.1;
    // Non-finite values from a sender are refused.
    player.seek(std::numeric_limits<double>::infinity());
    const bool finite = std::isfinite(player.getPlaybackInfo().position);
    player.stop();
    const bool ok = ended && resume_ignored && reopened && finite;
    std::fprintf(stderr, "media-smoke: after-end ended=%d resume_ignored=%d reopened=%d "
                 "position=%.3f finite=%d -> %s\n", ended, resume_ignored, reopened,
                 info.position, finite, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// The first picture of an item: size and the luma at a point in each
// quadrant (top-left, top-right, bottom-left, bottom-right).
struct FirstPicture {
    int width = 0;
    int height = 0;
    int luma[4] = {};
};

FirstPicture firstPicture(const char* location)
{
    std::mutex mutex;
    FirstPicture picture;
    std::atomic<bool> taken{false};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (!frame.data[0] || frame.width <= 0 || taken.exchange(true)) return;
        std::lock_guard<std::mutex> lock(mutex);
        picture.width = frame.width;
        picture.height = frame.height;
        if (const char* dump = std::getenv("MEDIA_SMOKE_DUMP_PGM")) {
            // The luma plane as a PGM image, to look at by hand.
            const std::string path = std::string(dump) + "-" + std::to_string(frame.width) + "x" +
                std::to_string(frame.height) + ".pgm";
            if (FILE* file = std::fopen(path.c_str(), "wb")) {
                std::fprintf(file, "P5\n%d %d\n255\n", frame.width, frame.height);
                for (int y = 0; y < frame.height; ++y) {
                    std::fwrite(frame.data[0] + y * frame.linesize[0], 1, frame.width, file);
                }
                std::fclose(file);
            }
        }
        // At 3/16 and 13/16 of each side: points a quarter turn maps onto
        // each other, inside testsrc's stripes rather than on their edges.
        for (int q = 0; q < 4; ++q) {
            const int x = frame.width * (q % 2 ? 13 : 3) / 16;
            const int y = frame.height * (q / 2 ? 13 : 3) / 16;
            picture.luma[q] = frame.data[0][y * frame.linesize[0] + x];
        }
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    player.play(location, 0.0);
    waitUntil([&] { return taken.load(); }, 5000);
    player.stop();
    std::lock_guard<std::mutex> lock(mutex);
    return picture;
}

// A recording with a 90-degree display rotation (rot90.mp4: clip8.mp4's
// pixels, shown turned 90 degrees counter-clockwise) comes out upright: its
// size swapped and its quadrants those of the reference turned the same way.
int rotation(const char* rotated, const char* reference)
{
    const FirstPicture turned = firstPicture(rotated);
    const FirstPicture upright = firstPicture(reference);
    // Counter-clockwise: top-left <- top-right, top-right <- bottom-right,
    // bottom-left <- top-left, bottom-right <- bottom-left.
    const int from[4] = {1, 3, 0, 2};
    bool quadrants = upright.width > 0;
    for (int q = 0; q < 4; ++q) {
        quadrants = quadrants && std::abs(turned.luma[q] - upright.luma[from[q]]) <= 12;
    }
    const bool size = turned.width == upright.height && turned.height == upright.width;
    // The quadrants must differ, or the check proves nothing.
    const bool distinct = upright.luma[0] != upright.luma[1] || upright.luma[1] != upright.luma[3];
    const bool ok = size && quadrants && distinct;
    std::fprintf(stderr,
                 "media-smoke: rotation %dx%d -> %dx%d, quadrants %d,%d,%d,%d from %d,%d,%d,%d "
                 "-> %s\n",
                 upright.width, upright.height, turned.width, turned.height, turned.luma[0],
                 turned.luma[1], turned.luma[2], turned.luma[3], upright.luma[0], upright.luma[1],
                 upright.luma[2], upright.luma[3], ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// A live HLS playlist (no end): pictures, no known duration, and stop()
// returns promptly although the demuxer waits for the playlist to grow.
int live(const char* location)
{
    std::atomic<unsigned int> frames{0};
    MediaPlayer player;
    player.setVideoCallback([&](const MediaVideoFrame& frame) {
        if (frame.data[0]) ++frames;
    });
    player.setAudioCallback([](const MediaAudioFrame&) {});
    player.play(location, 0.0);
    const bool shown = waitUntil([&] { return frames.load() > 0; }, 8000);
    const MediaPlaybackInfo info = player.getPlaybackInfo();
    const auto started = std::chrono::steady_clock::now();
    player.stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    const bool ok = shown && !info.duration_known && stop_ms < scaledMs(1000);
    std::fprintf(stderr, "media-smoke: live frames=%u duration_known=%d stop_ms=%.1f -> %s\n",
                 frames.load(), info.duration_known, stop_ms, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// A multivariant playlist: the rendition with `bandwidth` is the one played
// (the preflight's choice for HTTP, select_hls_program's for a file).
int rendition(const char* location, const char* bandwidth)
{
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_log.clear();
    }
    const unsigned int frames = framesWithin(location, 8000);
    const std::string wanted = std::string("bandwidth=") + bandwidth + " bps";
    bool chosen = false;
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        chosen = g_log.find(wanted) != std::string::npos;
    }
    const bool ok = frames > 0 && chosen;
    std::fprintf(stderr, "media-smoke: rendition frames=%u chosen(%s)=%d -> %s\n", frames,
                 wanted.c_str(), chosen, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// GET over FFmpeg's HTTP input, or empty.
std::string httpGet(const std::string& url)
{
    AVIOContext* io = nullptr;
    if (avio_open2(&io, url.c_str(), AVIO_FLAG_READ, nullptr, nullptr) < 0) return {};
    std::string body;
    unsigned char buffer[4096];
    int got = 0;
    while ((got = avio_read(io, buffer, sizeof(buffer))) > 0) {
        body.append(reinterpret_cast<const char*>(buffer), static_cast<size_t>(got));
    }
    avio_closep(&io);
    return body;
}

// Fetches go where the policy checked (tests/redirect-server.py on base+4):
// a name is connected to by its checked address with the name in the Host
// header (pinned.invalid exists only in the test lookup, so FFmpeg could not
// have found it), also for an HLS playlist's relative segments; redirects
// are followed by hand, and a hop the policy refuses is never requested and
// is reported once, not once per retry.
int fetchPinning()
{
    using namespace media_url_policy;
    const char* base_env = std::getenv("OBS_AIRPLAY_TEST_PORT_BASE");
    const std::string port = std::to_string((base_env ? std::atoi(base_env) : 18900) + 4);
    const auto url = [&](const char* host, const char* path) {
        return std::string("http://") + host + ":" + port + path;
    };
    setResolverForTesting([](const std::string& host, std::vector<sockaddr_storage>& out) {
        if (host != "pinned.invalid") return false;
        sockaddr_storage address = {};
        auto& v4 = reinterpret_cast<sockaddr_in&>(address);
        v4.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &v4.sin_addr);
        out.push_back(address);
        return true;
    });
    std::mutex refusals_mutex;
    std::vector<std::string> refusals;
    MediaPlayer::setRefusalCallback([&](const std::string& reason) {
        std::lock_guard<std::mutex> lock(refusals_mutex);
        refusals.push_back(reason);
    });
    const unsigned int pinned_mp4 = framesWithin(url("pinned.invalid", "/files/clip8.mp4").c_str(), 8000);
    const unsigned int pinned_hls = framesWithin(url("pinned.invalid", "/hls/same.m3u8").c_str(), 8000);
    // Loopback is refused from here on, except the item's own literal address.
    setLoopbackAllowedForTesting(false);
    const unsigned int redirected_mp4 =
        framesWithin(url("127.0.0.1", "/redirect/clip8.mp4").c_str(), 8000);
    const unsigned int redirected_hls =
        framesWithin(url("127.0.0.1", "/redirect-hls/same.m3u8").c_str(), 8000);
    size_t refusals_before = 0;
    {
        std::lock_guard<std::mutex> lock(refusals_mutex);
        refusals_before = refusals.size();
    }
    const unsigned int refused_frames =
        framesWithin(url("127.0.0.1", "/redirect-out/clip8.mp4").c_str(), 3000);
    size_t refused_reports = 0;
    {
        std::lock_guard<std::mutex> lock(refusals_mutex);
        refused_reports = refusals.size() - refusals_before;
    }
    setLoopbackAllowedForTesting(true);
    MediaPlayer::setRefusalCallback(nullptr);
    setResolverForTesting(nullptr);

    const std::string log = httpGet(url("127.0.0.1", "/log"));
    const bool host_kept = log.find("pinned.invalid:" + port + " /files/clip8.mp4") != std::string::npos &&
        log.find("pinned.invalid:" + port + " /hls/seg0.ts") != std::string::npos;
    const bool redirect_followed = log.find("127.0.0.1:" + port + " /redirect/clip8.mp4") !=
        std::string::npos;
    const bool secret_untouched = log.find("/secret/") == std::string::npos;
    const bool ok = pinned_mp4 > 0 && pinned_hls > 0 && redirected_mp4 > 0 &&
        redirected_hls > 0 && redirect_followed && host_kept && refused_frames == 0 &&
        secret_untouched && refused_reports == 1;
    std::fprintf(stderr,
                 "media-smoke: fetch-pinning pinned_mp4=%u pinned_hls=%u host_kept=%d "
                 "redirected_mp4=%u redirected_hls=%u refused_frames=%u secret_untouched=%d "
                 "refused_reports=%zu -> %s\n",
                 pinned_mp4, pinned_hls, host_kept, redirected_mp4, redirected_hls,
                 refused_frames, secret_untouched, refused_reports, ok ? "ok" : "FAILED");
    if (!ok) std::fprintf(stderr, "media-smoke: server log:\n%s", log.c_str());
    return ok ? 0 : 1;
}

// stop() while the item's host name is still being looked up returns at
// once: the lookup runs on a thread nobody waits for (an unreachable
// resolver used to hold stop(), and with it UxPlay's HTTP thread or OBS's
// exit, for the resolver's whole retry budget).
int slowLookup()
{
    using namespace media_url_policy;
    setResolverForTesting([](const std::string&, std::vector<sockaddr_storage>&) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return false;
    });
    MediaPlayer player;
    player.setVideoCallback([](const MediaVideoFrame&) {});
    player.setAudioCallback([](const MediaAudioFrame&) {});
    player.play("http://slow-resolver.invalid/clip.mp4", 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto started = std::chrono::steady_clock::now();
    player.stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    setResolverForTesting(nullptr);
    const bool ok = stop_ms < 500.0;
    std::fprintf(stderr, "media-smoke: slow-lookup stop_ms=%.1f -> %s\n", stop_ms,
                 ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    // The smokes play local files and loopback servers; the policy itself is
    // exercised by --url-policy and --nested-policy.
    media_url_policy::setLocalFilesAllowedForTesting(true);
    media_url_policy::setLoopbackAllowedForTesting(true);
    if (argc == 2 && std::strcmp(argv[1], "--url-policy") == 0) {
        return urlPolicy();
    }
    if (argc == 4 && std::strcmp(argv[1], "--nested-policy") == 0) {
        return nestedPolicy(argv[2], argv[3]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--resilient") == 0) {
        return resilient(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--tls-verify") == 0) {
        return tlsVerify(argv[2]);
    }
    if (argc == 2 && std::strcmp(argv[1], "--fetch-pinning") == 0) {
        return fetchPinning();
    }
    if (argc == 2 && std::strcmp(argv[1], "--slow-lookup") == 0) {
        return slowLookup();
    }
    if (argc == 4 && std::strcmp(argv[1], "--rotation") == 0) {
        return rotation(argv[2], argv[3]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--live") == 0) {
        return live(argv[2]);
    }
    if (argc == 4 && std::strcmp(argv[1], "--rendition") == 0) {
        return rendition(argv[2], argv[3]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--ts-seek") == 0) {
        return tsSeek(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--ts-seek-race") == 0) {
        return tsSeek(argv[2], true);
    }
    if (argc == 3 && std::strcmp(argv[1], "--after-end") == 0) {
        return afterEnd(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--follow-clock") == 0) {
        return followClock(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--follow-clock-short") == 0) {
        return followClockShortItem(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--follow-clock-short-quick") == 0) {
        return followClockShortItem(argv[2], true);
    }
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
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "--stop-race") == 0) {
        return stopRace(argv[2], argc == 4 ? std::max(1, std::atoi(argv[3])) : 1000);
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
        std::fprintf(stderr, "usage: %s [--startup] [--require-audio] MEDIA_URL | --seek-sync MEDIA_URL | --sparse-clip MEDIA_URL | --scrub-drag MEDIA_URL | --paused-audio MEDIA_URL | --stop-race MEDIA_URL [iterations] | --url-policy | --nested-policy SAME_ORIGIN_M3U8 CROSS_ORIGIN_M3U8 | --tls-verify HTTPS_URL | --resilient TRUNCATING_URL | --fetch-pinning | --slow-lookup | --rotation ROTATED REFERENCE | --live M3U8 | --rendition M3U8 BANDWIDTH | --switch-to-still VIDEO STILL | --ts-seek TS_URL | --ts-seek-race TS_URL | --after-end SHORT_URL\n", argv[0]);
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
