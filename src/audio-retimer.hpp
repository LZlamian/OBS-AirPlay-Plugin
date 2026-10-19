// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// Keeps mirrored audio from reaching OBS late.
//
// OBS treats audio whose timestamp is already in the past as late and raises
// its audio buffering to cover it, for every source, up to 960 ms, and never
// lowers it again while OBS runs. An AirPlay audio stream starts with a
// burst: packets queued before the first RTP sync are released at once, with
// play-at times that have passed. Those are dropped here. A stream that falls
// behind later (a Wi-Fi stall) is shifted forward just enough to be on time
// again, rather than letting OBS add its buffering for good.
//
// Not thread-safe: used on the audio thread (under the audio decoder lock).
class AudioRetimer {
public:
    // Audio this far behind the OBS clock is late.
    static constexpr uint64_t kLateNs = 20'000'000;
    // From the first packet of a stream: late packets are its startup backlog.
    static constexpr uint64_t kStartupNs = 2'000'000'000;
    // The most a stream is shifted in total (about OBS's own limit).
    static constexpr uint64_t kMaxShiftNs = 1'000'000'000;

    struct Result {
        bool drop = false;
        uint64_t timestamp = 0;
    };

    // `timestamp`: the packet's play-at time on the OBS clock; `now`: the OBS
    // clock (os_gettime_ns()) when it is output.
    Result process(uint64_t timestamp, uint64_t now)
    {
        Result result;
        if (!m_started) {
            m_started = true;
            m_stream_start = now;
        }
        uint64_t shifted = timestamp + m_shift;
        if (shifted + kLateNs < now) {
            if (now - m_stream_start < kStartupNs) {
                ++m_dropped;
                const uint64_t late = now - shifted;
                if (late > m_worst_dropped) m_worst_dropped = late;
                result.drop = true;
                return result;
            }
            const uint64_t needed = now - shifted;
            if (m_shift + needed <= kMaxShiftNs) {
                m_shift += needed;
                shifted = now;
                ++m_shifts;
            }
        }
        result.timestamp = shifted;
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
    uint64_t shiftNs() const { return m_shift; }
    uint64_t shifts() const { return m_shifts; }

private:
    bool m_started = false;
    uint64_t m_stream_start = 0;
    uint64_t m_shift = 0;
    uint64_t m_dropped = 0;
    uint64_t m_worst_dropped = 0;
    uint64_t m_shifts = 0;
};
