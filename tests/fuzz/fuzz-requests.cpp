// SPDX-License-Identifier: GPL-3.0-or-later
// Requests from the network into the receiver's handlers (uxplay/lib):
// /play, /action, playback controls and the RTSP requests, with their binary
// plist bodies, run as a sequence on one connection. An input is a list of
// records: a request kind (kKinds), a 16-bit big-endian body length, the
// body. Valid headers are built around each body, so mutations reach the
// handlers and the plist parser instead of breaking the HTTP framing
// (make-request-seeds.py writes the seeds).
extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"
}

#include <csignal>
#include <cstdio>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char kSession[] = "11111111-2222-3333-4444-555555555555";
const char kRtsp[] = "rtsp://127.0.0.1/1234567890";

// Method and path of each record kind; a query value ends with '=' and takes
// the body as its value; RTSP requests carry a CSeq instead of a session.
struct Kind {
    const char* method;
    const char* path;
    bool rtsp;
};
const Kind kKinds[] = {
    {"POST", "/play", false},
    {"POST", "/action", false},
    {"PUT", "/setProperty?selectedMediaArray", false},
    {"POST", "/getProperty?playbackAccessLog", false},
    {"POST", "/rate?value=", false},
    {"POST", "/scrub?position=", false},
    {"GET", "/playback-info", false},
    {"POST", "/stop", false},
    {"GET", "/info", true},
    {"SETUP", kRtsp, true},
    {"RECORD", kRtsp, true},
    {"TEARDOWN", kRtsp, true},
};

std::string buildRequest(const Kind& kind, const std::string& body, int cseq)
{
    std::string path = kind.path;
    std::string content = body;
    if (!path.empty() && path.back() == '=') {
        // query value: printable bytes of the body, no body
        for (char c : body) {
            if (c > ' ' && c < 127 && c != '&' && c != '#') path += c;
        }
        content.clear();
    }
    std::string request = std::string(kind.method) + " " + path +
        (kind.rtsp ? " RTSP/1.0\r\n" : " HTTP/1.1\r\n");
    if (kind.rtsp) {
        request += "CSeq: " + std::to_string(cseq) + "\r\n";
    } else {
        request += std::string("X-Apple-Session-ID: ") + kSession + "\r\n";
    }
    if (!content.empty()) request += "Content-Type: application/x-apple-binary-plist\r\n";
    request += "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n";
    return request + content;
}

void audioProcess(void*, raop_ntp_t*, audio_decode_struct*) {}
void videoProcess(void*, raop_ntp_t*, video_decode_struct*) {}
void videoReset(void*, reset_type_t) {}
void connReset(void*, int) {}
void play(void*, const char*, float) {}
void stop(void*) {}
void scrub(void*, float) {}
void rate(void*, float) {}
float playlistRemove(void*) { return 0.0f; }
void refused(void*, const char*) {}
void playbackInfo(void*, playback_info_t* info)
{
    info->duration = 60.0;
    info->position = 1.0;
    info->rate = 1.0f;
    info->ready_to_play = true;
    info->playback_likely_to_keep_up = true;
}
// OBS_AIRPLAY_FUZZ_VERBOSE=1 prints the receiver's log (to check what the
// seeds reach); quiet otherwise.
void receiverLog(void*, int, const char* message)
{
    static const bool verbose = std::getenv("OBS_AIRPLAY_FUZZ_VERBOSE") != nullptr;
    if (verbose) std::fprintf(stderr, "[receiver] %s\n", message);
}

raop_t* receiver()
{
    static raop_t* raop = [] {
        signal(SIGPIPE, SIG_IGN);
        static raop_callbacks_t callbacks = {};
        callbacks.audio_process = audioProcess;
        callbacks.video_process = videoProcess;
        callbacks.video_reset = videoReset;
        callbacks.conn_reset = connReset;
        callbacks.on_video_play = play;
        callbacks.on_video_stop = stop;
        callbacks.on_video_scrub = scrub;
        callbacks.on_video_rate = rate;
        callbacks.on_video_playlist_remove = playlistRemove;
        callbacks.on_video_acquire_playback_info = playbackInfo;
        callbacks.on_video_refused = refused;
        raop_t* created = raop_init(&callbacks);
        if (!created || raop_init2(created, 1, "02:00:00:00:00:01", "") != 0) std::abort();
        int error = 0;
        const char hw_addr[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        dnssd_t* dnssd = dnssd_init("Fuzz", 4, hw_addr, sizeof(hw_addr), &error, 0);
        if (!dnssd || error != DNSSD_ERROR_NOERROR) std::abort();
        raop_set_dnssd(created, dnssd);
        raop_set_log_callback(created, receiverLog, nullptr);
        if (std::getenv("OBS_AIRPLAY_FUZZ_VERBOSE")) raop_set_log_level(created, LOGGER_INFO);
        raop_set_plist(created, "hls", 1);
        return created;
    }();
    return raop;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    raop_t* raop = receiver();
    void* connection = raop_test_connection_open(raop);
    if (!connection) return 0;
    size_t at = 0;
    for (int cseq = 1; at + 3 <= size && cseq <= 32; ++cseq) {
        const Kind& kind = kKinds[data[at] % (sizeof(kKinds) / sizeof(kKinds[0]))];
        const size_t length = std::min<size_t>((size_t(data[at + 1]) << 8) | data[at + 2],
                                               size - at - 3);
        const std::string body(reinterpret_cast<const char*>(data + at + 3), length);
        at += 3 + length;
        const std::string request = buildRequest(kind, body, cseq);
        raop_test_connection_request(connection, request.data(), static_cast<int>(request.size()));
    }
    raop_test_connection_close(connection);
    return 0;
}
