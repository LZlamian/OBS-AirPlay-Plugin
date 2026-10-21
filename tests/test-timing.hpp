// SPDX-License-Identifier: GPL-3.0-or-later
// Timing for the smokes. Deadlines are set for a developer Mac;
// OBS_AIRPLAY_TIMING_SCALE (CI sets 3, 5 under sanitizers) widens them.
// Waiting for something is "until it holds or the deadline": a slow
// machine takes longer, a fast one does not wait for nothing.
#pragma once

#include <chrono>
#include <cstdlib>
#include <thread>

inline double timingScale()
{
    static const double scale = [] {
        const char* env = std::getenv("OBS_AIRPLAY_TIMING_SCALE");
        const double value = env ? std::atof(env) : 1.0;
        return value >= 1.0 ? value : 1.0;
    }();
    return scale;
}

// `ms` on a developer Mac, scaled.
inline int scaledMs(int ms)
{
    return static_cast<int>(ms * timingScale());
}

// Polls `done` every `step_ms` until it returns true or scaledMs(timeout_ms)
// pass; returns its last result.
template <typename Predicate> bool waitUntil(Predicate done, int timeout_ms, int step_ms = 10)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(scaledMs(timeout_ms));
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return done();
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return true;
}
