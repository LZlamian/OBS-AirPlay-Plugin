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
// is dropped, and after a real stall (Wi-Fi) the stream restarts.
//
// The timeline is the sender's play-at times plus an offset: 0 while they
// are followed; when a step back is ridden over, or the stream restarts, the
// offset is set so the sender's later times continue the timeline, which
// therefore keeps following the sender's clock (UxPlay converts it to the
// local one) instead of running on packet durations alone. Small
// differences between the two (a sender sample clock a few ppm off) are
// absorbed by moving each packet by at most 1/1024 of its duration, so they
// never add up to a step.
//
// A stream starts (and restarts) at least kStartLeadNs ahead of now: when
// audio anchors the clock its first packet is due exactly now, and arrival
// jitter would otherwise make the next ones late.
//
// Not thread-safe: used on the audio thread (under the audio decoder lock).
class AudioRetimer {
public:
    // Audio this far behind the OBS clock is late.
    static constexpr uint64_t kLateNs = 20'000'000;
    // From the first packet of a stream: late packets are its startup backlog.
    static constexpr uint64_t kStartupNs = 2'000'000'000;
    // Play-at times this close to the continuous timeline are jitter (OBS's
    // own smoothing threshold is 70 ms). In the startup window, packets late
    // by less than this are kept (the stream restarts), not dropped.
    static constexpr uint64_t kJitterNs = 70'000'000;
    // After a step back was ridden over, or a restart, the timeline is kept
    // at most this far ahead of now: a burst after a stall, or a sender clock
    // slightly faster than OBS's, would otherwise add delay for the rest of
    // the stream. Enough to absorb Wi-Fi jitter.
    static constexpr uint64_t kMaxLeadNs = 150'000'000;
    // The least lead a stream starts or restarts with: with kLateNs, packets
    // may arrive 60 ms behind their play-at times before they are late.
    static constexpr uint64_t kStartLeadNs = 40'000'000;

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
        const bool in_startup = now - m_stream_start < kStartupNs;
        if (m_next == 0) {
            // The first packet of the timeline.
            if (late(timestamp, now)) {
                if (in_startup) return dropBacklog(timestamp, now);
                ++m_resyncs;
            }
            return start(timestamp, duration, now);
        }

        const uint64_t expected = shifted(timestamp);
        const bool on_timeline = distance(expected, m_next) <= kJitterNs;
        if (!on_timeline && m_following) {
            ++m_steps;  // play-at times moved away from the timeline
            m_last_step_ns = static_cast<int64_t>(expected - m_next);
        }
        m_following = on_timeline;

        uint64_t out = 0;
        if (on_timeline) {
            out = m_next + slew(expected, duration);
        } else if (!late(timestamp, now)) {
            // On time off the timeline (a step forward): follow the sender.
            m_offset = 0;
            m_anchored = false;
            out = timestamp;
        }
        if (out == 0 || late(out, now)) {
            if (!late(m_next, now)) {
                // A step back: stay continuous and on time, and continue the
                // timeline from the sender's later times.
                m_offset = static_cast<int64_t>(m_next - timestamp);
                m_anchored = true;
                m_following = true;
                out = m_next;
                ++m_held;
            } else if (in_startup && now - (on_timeline ? out : timestamp) > kJitterNs) {
                return dropBacklog(on_timeline ? out : timestamp, now);
            } else {
                // Fell behind (a stall, or startup jitter): restart.
                ++m_resyncs;
                return start(timestamp, duration, now);
            }
        }
        if (m_anchored && out > now + kMaxLeadNs) {
            // Running ahead: drop this packet and let the next one take its
            // place on the timeline.
            ++m_trimmed;
            m_offset = static_cast<int64_t>(m_next - timestamp - duration);
            result.drop = true;
            return result;
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

    uint64_t shifted(uint64_t timestamp) const
    {
        return static_cast<uint64_t>(static_cast<int64_t>(timestamp) + m_offset);
    }

    // How far to move a packet on the timeline toward the sender's time.
    static int64_t slewBy(int64_t difference, uint64_t duration)
    {
        const int64_t limit = static_cast<int64_t>(duration / 1024);
        return difference > limit ? limit : difference < -limit ? -limit : difference;
    }
    int64_t slew(uint64_t expected, uint64_t duration) const
    {
        return slewBy(static_cast<int64_t>(expected - m_next), duration);
    }

    // Starts the timeline at this packet, at least kStartLeadNs ahead of now.
    Result start(uint64_t timestamp, uint64_t duration, uint64_t now)
    {
        const uint64_t out = timestamp < now + kStartLeadNs ? now + kStartLeadNs : timestamp;
        m_offset = static_cast<int64_t>(out - timestamp);
        // A restart moved the timeline: keep it from running ahead.
        m_anchored = m_resyncs > 0;
        m_following = true;
        m_next = out + duration;
        Result result;
        result.timestamp = out;
        return result;
    }

    Result dropBacklog(uint64_t timestamp, uint64_t now)
    {
        ++m_dropped;
        const uint64_t behind = now - timestamp;
        if (behind > m_worst_dropped) m_worst_dropped = behind;
        Result result;
        result.drop = true;
        return result;
    }

    bool m_started = false;
    uint64_t m_stream_start = 0;
    uint64_t m_next = 0;
    // Added to the sender's play-at times to place them on the timeline.
    int64_t m_offset = 0;
    // The offset was set by a ride-over or a restart (the lead is capped).
    bool m_anchored = false;
    uint64_t m_dropped = 0;
    uint64_t m_worst_dropped = 0;
    bool m_following = true;
    uint64_t m_steps = 0;
    int64_t m_last_step_ns = 0;
    uint64_t m_held = 0;
    uint64_t m_resyncs = 0;
    uint64_t m_trimmed = 0;
};
