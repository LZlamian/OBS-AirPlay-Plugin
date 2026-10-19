// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// Keeps mirrored audio from reaching OBS late.
//
// OBS treats audio whose timestamp is already in the past as late and raises
// its audio buffering to cover it, for every source, up to 960 ms, and never
// lowers it again while OBS runs. AirPlay mirror audio has two sources of
// late timestamps:
// - the start of a stream: packets queued before the first RTP sync are
//   released in a burst, with play-at times that have passed;
// - the first real RTP sync: until it arrives UxPlay estimates the stream's
//   clock from the video, and the real sync then moves every play-at time,
//   often back by about a second.
// Timestamps are therefore kept continuous (each packet follows the previous
// one) and follow the sender's play-at times only while those are on time:
// a step back that would make audio late is ridden over, a startup backlog
// is dropped, and after a real stall (Wi-Fi) the stream restarts at now.
//
// Not thread-safe: used on the audio thread (under the audio decoder lock).
class AudioRetimer {
public:
    // Audio this far behind the OBS clock is late.
    static constexpr uint64_t kLateNs = 20'000'000;
    // From the first packet of a stream: late packets are its startup backlog.
    static constexpr uint64_t kStartupNs = 2'000'000'000;
    // Play-at times this close to the continuous timeline are jitter (OBS's
    // own smoothing threshold is 70 ms).
    static constexpr uint64_t kJitterNs = 70'000'000;
    // While play-at times are ridden over, the timeline is kept at most this
    // far ahead of now: a burst after a stall, or a sender clock slightly
    // faster than OBS's, would otherwise add delay for the rest of the stream.
    // Enough to absorb Wi-Fi jitter.
    static constexpr uint64_t kMaxLeadNs = 150'000'000;

    struct Result {
        bool drop = false;
        uint64_t timestamp = 0;
    };

    // `timestamp`: the packet's play-at time on the OBS clock; `duration`:
    // its length in ns; `now`: the OBS clock (os_gettime_ns()).
    Result process(uint64_t timestamp, uint64_t duration, uint64_t now)
    {
        Result result;
        if (!m_started) {
            m_started = true;
            m_stream_start = now;
        }
        const bool have_next = m_next != 0;
        uint64_t out = timestamp;
        const bool on_timeline = have_next && distance(timestamp, m_next) <= kJitterNs;
        if (on_timeline) {
            out = m_next;
        } else if (have_next && m_following) {
            ++m_steps;  // play-at times moved away from the timeline
            m_last_step_ns = static_cast<int64_t>(timestamp - m_next);
        }
        m_following = !have_next || on_timeline;
        if (late(out, now)) {
            if (have_next && !late(m_next, now)) {
                if (m_next > now + kMaxLeadNs) {
                    ++m_trimmed;
                    result.drop = true;
                    return result;
                }
                out = m_next;  // a step back: stay continuous and on time
                ++m_held;
            } else if (now - m_stream_start < kStartupNs) {
                ++m_dropped;
                const uint64_t behind = now - out;
                if (behind > m_worst_dropped) m_worst_dropped = behind;
                result.drop = true;
                return result;
            } else {
                out = now;  // fell behind (a stall): restart at now
                ++m_resyncs;
            }
        }
        m_next = out + duration;
        result.timestamp = out;
        return result;
    }

    // A new stream (SETUP, flush, reconnect): start over.
    void reset() { *this = AudioRetimer{}; }

    // True once a packet past the startup window has been seen (for a summary).
    bool pastStartup(uint64_t now) const
    {
        return m_started && now - m_stream_start >= kStartupNs;
    }

    uint64_t dropped() const { return m_dropped; }
    uint64_t worstDroppedNs() const { return m_worst_dropped; }
    // Times the play-at times moved off the timeline (beyond jitter) and by
    // how much the last time, packets held on the timeline because of such a
    // move, and restarts after a stall.
    uint64_t steps() const { return m_steps; }
    int64_t lastStepNs() const { return m_last_step_ns; }
    uint64_t held() const { return m_held; }
    uint64_t resyncs() const { return m_resyncs; }
    uint64_t trimmed() const { return m_trimmed; }

private:
    static uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }
    static bool late(uint64_t timestamp, uint64_t now) { return timestamp + kLateNs < now; }

    bool m_started = false;
    uint64_t m_stream_start = 0;
    uint64_t m_next = 0;
    uint64_t m_dropped = 0;
    uint64_t m_worst_dropped = 0;
    bool m_following = true;
    uint64_t m_steps = 0;
    int64_t m_last_step_ns = 0;
    uint64_t m_held = 0;
    uint64_t m_resyncs = 0;
    uint64_t m_trimmed = 0;
};
