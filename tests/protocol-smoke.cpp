// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <plist/plist.h>

#include "test-timing.hpp"
#include "url-policy-cases.hpp"

#include <chrono>
#include <algorithm>
#include <thread>
#include <atomic>
#include <csignal>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "dnssd.h"
#include "raop.h"
}

// {"text of the condition", its value}
#define PROTOCOL_CONDITION(x) {#x, static_cast<bool>(x)}

namespace {

#if defined(_WIN32)
// Winsock sockets are handles, closed with closesocket(); the receiver
// (raop_init) has started Winsock.
void closeSocket(int fd) { closesocket(static_cast<SOCKET>(fd)); }
void setTimeout(int fd, int option, int ms)
{
    const DWORD timeout = static_cast<DWORD>(ms);
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, option,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
void setIntOption(int fd, int option, int value)
{
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, option,
               reinterpret_cast<const char*>(&value), sizeof(value));
}
#else
void closeSocket(int fd) { close(fd); }
void setTimeout(int fd, int option, int ms)
{
    timeval timeout = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}
void setIntOption(int fd, int option, int value)
{
    setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value));
}
#endif

// POSIX: the spool file is 0600. Windows has no such modes (its DACL
// grants this user and SYSTEM only: uxplay/lib/spool_win32.c).
bool spoolModePrivate(unsigned mode)
{
#if defined(_WIN32)
    (void)mode;
    return true;
#else
    return mode == 0600;
#endif
}

void receiverLog(void*, int level, const char* message)
{
    if (message && (level <= LOGGER_WARNING || std::strstr(message, "[MEDIA]") ||
                    std::strstr(message, "[MEDIA-TRACE]")))
        std::fprintf(stderr, "[receiver:%d] %s\n", level, message);
}

struct TestState {
    std::mutex mutex;
    std::condition_variable cv;
    std::string location;
    std::string first_location;
    std::string spooled_location;
    std::string spooled_bytes;
    mode_t spooled_mode = 0;
    off_t spooled_size = 0;
    float start_position = -1.0f;
    float scrub_position = -1.0f;
    float playback_rate = -1.0f;
    int rate_calls = 0;
    int stop_count = 0;
    int playback_info_count = 0;
    int play_count = 0;
    int refused_count = 0;
    std::string refused_reason;
};

void audioProcess(void*, raop_ntp_t*, audio_decode_struct*) {}
void videoProcess(void*, raop_ntp_t*, video_decode_struct*) {}
void reset(void*, reset_type_t) {}
void connectionReset(void*, int) {}
void stop(void* cls)
{
    auto* state = static_cast<TestState*>(cls);
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->stop_count;
}

void refused(void* cls, const char* reason)
{
    auto* state = static_cast<TestState*>(cls);
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->refused_count;
    state->refused_reason = reason ? reason : "";
}

void scrub(void* cls, float position)
{
    auto* state = static_cast<TestState*>(cls);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->scrub_position = position;
}

void rate(void* cls, float value)
{
    auto* state = static_cast<TestState*>(cls);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->playback_rate = value;
    ++state->rate_calls;
}
float playlistRemove(void*) { return 0.0f; }

void playbackInfo(void* cls, playback_info_t* info)
{
    auto* state = static_cast<TestState*>(cls);
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->playback_info_count;
    std::memset(info, 0, sizeof(*info));
    info->duration = 60.0;
    info->position = 1.25;
    info->seek_duration = 60.0;
    info->rate = 1.0f;
    info->ready_to_play = true;
    info->playback_buffer_full = true;
    info->playback_likely_to_keep_up = true;
}

void play(void* cls, const char* location, float start_position)
{
    auto* state = static_cast<TestState*>(cls);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->location = location ? location : "";
    if (state->play_count == 0) state->first_location = state->location;
    const size_t spool_name = state->location.find("obs-airplay-fcup-");
    if (spool_name != std::string::npos && spool_name > 0 &&
        (state->location[spool_name - 1] == '/' || state->location[spool_name - 1] == '\\')) {
        state->spooled_location = state->location;
        FILE* file = std::fopen(state->location.c_str(), "rb");
        if (file) {
            char bytes[64] = {};
            const size_t read = std::fread(bytes, 1, sizeof(bytes), file);
            state->spooled_bytes.assign(bytes, read);
            std::fclose(file);
        }
        struct stat info = {};
        if (stat(state->location.c_str(), &info) == 0) {
            state->spooled_mode = info.st_mode & 0777;
            state->spooled_size = info.st_size;
        }
    }
    state->start_position = start_position;
    ++state->play_count;
    state->cv.notify_all();
    state->cv.notify_all();
}

// Reverse-fetched media is written to its spool file off the receiver's
// network thread; playback starts once it is. Waits for the play callback
// that names a spool file other than `previous`.
bool waitForNewSpool(TestState& state, const std::string& previous, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return !state.spooled_location.empty() && state.spooled_location != previous;
    });
}

int connectLocal(unsigned short port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        closeSocket(fd);
        return -1;
    }
    setTimeout(fd, SO_RCVTIMEO, 10000);
    return fd;
}

// The same receiver seen from a different peer address (::1).
int connectLocal6(unsigned short port)
{
    const int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in6 address = {};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_loopback;
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        closeSocket(fd);
        return -1;
    }
    setTimeout(fd, SO_RCVTIMEO, 10000);
    return fd;
}

bool sendAll(int fd, const void* data, size_t size)
{
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t sent = send(fd, bytes, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (sent <= 0) return false;
        bytes += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

std::string receiveResponse(int fd)
{
    char buffer[4096] = {};
    const ssize_t received = recv(fd, buffer, sizeof(buffer) - 1, 0);
    return received > 0 ? std::string(buffer, static_cast<size_t>(received)) : std::string();
}

std::string receiveUntil(int fd, const char* marker)
{
    std::string result;
    char buffer[4096] = {};
    while (result.find(marker) == std::string::npos) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        result.append(buffer, static_cast<size_t>(received));
    }
    return result;
}

std::string httpExchange(unsigned short port, const std::string& request)
{
    const int fd = connectLocal(port);
    if (fd < 0 || !sendAll(fd, request.data(), request.size())) {
        if (fd >= 0) closeSocket(fd);
        return {};
    }
    const std::string response = receiveResponse(fd);
    closeSocket(fd);
    return response;
}

std::string binaryPlistExchange(unsigned short port, const char* path,
                                const char* session, plist_t root)
{
    char* body = nullptr;
    uint32_t body_size = 0;
    plist_to_bin(root, &body, &body_size);
    std::string request = std::string("POST ") + path + " HTTP/1.1\r\n" +
        "Host: 127.0.0.1\r\n";
    if (session) {
        request += std::string("X-Apple-Session-ID: ") + session + "\r\n";
    }
    request += "Content-Type: application/x-apple-binary-plist\r\n";
    request += "Content-Length: " + std::to_string(body_size) + "\r\n\r\n";

    const int fd = connectLocal(port);
    bool sent = fd >= 0 && sendAll(fd, request.data(), request.size()) &&
        sendAll(fd, body, body_size);
    free(body);
    const std::string response = sent ? receiveResponse(fd) : std::string();
    if (fd >= 0) closeSocket(fd);
    return response;
}

} // namespace

int main(int argc, char** argv)
{
    const bool large_blob = argc == 2 && std::strcmp(argv[1], "--large-blob") == 0;
    if (argc > 2 || (argc == 2 && !large_blob)) {
        std::fprintf(stderr, "usage: %s [--large-blob]\n", argv[0]);
        return 2;
    }
    // A peer the receiver drops must fail the test, not kill it.
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    TestState state;
    raop_callbacks_t callbacks = {};
    callbacks.cls = &state;
    callbacks.audio_process = audioProcess;
    callbacks.video_process = videoProcess;
    callbacks.conn_reset = connectionReset;
    callbacks.video_reset = reset;
    callbacks.on_video_play = play;
    callbacks.on_video_stop = stop;
    callbacks.on_video_scrub = scrub;
    callbacks.on_video_rate = rate;
    callbacks.on_video_playlist_remove = playlistRemove;
    callbacks.on_video_acquire_playback_info = playbackInfo;
    callbacks.on_video_refused = refused;

    raop_t* raop = raop_init(&callbacks);
    if (!raop || raop_init2(raop, 1, "02:00:00:00:00:01", "") != 0) {
        std::fprintf(stderr, "failed to initialize receiver\n");
        return 1;
    }
    // GET /info reads the advertised name and device ID from the DNS-SD
    // context; nothing is registered with mDNSResponder.
    int dnssd_error = 0;
    const char hw_addr[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    dnssd_t* dnssd = dnssd_init("Protocol Smoke", 14, hw_addr, sizeof(hw_addr), &dnssd_error, 0);
    if (!dnssd || dnssd_error != DNSSD_ERROR_NOERROR) {
        std::fprintf(stderr, "failed to initialize dnssd context\n");
        raop_destroy(raop);
        return 1;
    }
    raop_set_dnssd(raop, dnssd);
    raop_set_log_callback(raop, receiverLog, nullptr);
    raop_set_log_level(raop, LOGGER_INFO);
    if (std::getenv("PROTOCOL_SMOKE_MEDIA_TRACE")) {
        // Also run the [MEDIA-TRACE] protocol dump over every exchange.
        raop_set_media_trace_level(raop, LOGGER_INFO);
    }
    if (raop_set_plist(raop, "hls", 1) != 0) {
        std::fprintf(stderr, "failed to enable HLS receiver mode\n");
        raop_destroy(raop);
        dnssd_destroy(dnssd);
        return 1;
    }

    unsigned short port = 0;
    if (raop_start_httpd(raop, &port) < 0) {
        std::fprintf(stderr, "failed to start protocol smoke receiver\n");
        raop_destroy(raop);
        dnssd_destroy(dnssd);
        return 1;
    }
    raop_set_port(raop, port);

    const char* session = "11111111-2222-3333-4444-555555555555";
    const std::string native_response = httpExchange(
        port,
        "OPTIONS * RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Content-Length: 0\r\n\r\n");
    const int reverse_fd = connectLocal(port);

    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "uuid", plist_new_string("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    plist_dict_set_item(root, "Content-Location",
                        plist_new_string("http://media.example.test/sample.mp4"));
    plist_dict_set_item(root, "Start-Position-Seconds", plist_new_real(1.25));
    char* body = nullptr;
    uint32_t body_size = 0;
    plist_to_bin(root, &body, &body_size);
    plist_free(root);

    const int play_fd = connectLocal(port);
    const std::string play_headers =
        std::string("POST /play HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n" +
        "X-Apple-Session-ID: " + session + "\r\n" +
        "Content-Type: application/x-apple-binary-plist\r\n" +
        "Content-Length: " + std::to_string(body_size) + "\r\n\r\n";
    bool play_sent = play_fd >= 0 && sendAll(play_fd, play_headers.data(), play_headers.size());
    play_sent = play_sent && sendAll(play_fd, body, body_size);
    free(body);
    const std::string play_response = play_sent ? receiveResponse(play_fd) : "";

    // A reverse-fetch reply for an item that never asked for one (a plain
    // URL item has no HLS state) must be rejected, not dereferenced.
    const auto unexpected_reply = [&](const char* url) {
        plist_t params = plist_new_dict();
        plist_dict_set_item(params, "FCUP_Response_StatusCode", plist_new_uint(200));
        plist_dict_set_item(params, "FCUP_Response_RequestID", plist_new_uint(1));
        plist_dict_set_item(params, "FCUP_Response_URL", plist_new_string(url));
        const char playlist[] = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nmedia.m3u8\n";
        plist_dict_set_item(params, "FCUP_Response_Data",
                            plist_new_data(playlist, sizeof(playlist) - 1));
        plist_t action = plist_new_dict();
        plist_dict_set_item(action, "type", plist_new_string("unhandledURLResponse"));
        plist_dict_set_item(action, "params", params);
        const std::string response = binaryPlistExchange(port, "/action", session, action);
        plist_free(action);
        return response;
    };
    const std::string unexpected_master_response =
        unexpected_reply("http://media.example.test/master.m3u8");
    const std::string unexpected_media_response =
        unexpected_reply("http://media.example.test/media.m3u8");

    const auto control_request = [&](const char* method, const char* path) {
        return std::string(method) + " " + path + " HTTP/1.1\r\n" +
            "Host: 127.0.0.1\r\n" +
            "X-Apple-Session-ID: " + session + "\r\n" +
            "Content-Length: 0\r\n\r\n";
    };
    const std::string playback_response = httpExchange(
        port, control_request("GET", "/playback-info"));
    const std::string rate_response = httpExchange(
        port, control_request("POST", "/rate?value=0.5"));
    const std::string scrub_response = httpExchange(
        port, control_request("POST", "/scrub?position=7.5"));
    const std::string stop_response = httpExchange(
        port, control_request("POST", "/stop"));

    plist_t stateless_root = plist_new_dict();
    plist_dict_set_item(stateless_root, "Content-Location",
                        plist_new_string("https://example.test/stateless.mp4"));
    plist_dict_set_item(stateless_root, "Start-Position-Seconds", plist_new_real(3.0));
    char* stateless_body = nullptr;
    uint32_t stateless_size = 0;
    plist_to_bin(stateless_root, &stateless_body, &stateless_size);
    plist_free(stateless_root);
    const std::string stateless_headers =
        std::string("POST /play HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n" +
        "X-Apple-Session-ID: " + session + "\r\n" +
        "Content-Type: application/x-apple-binary-plist\r\n" +
        "Content-Length: " + std::to_string(stateless_size) + "\r\n\r\n";
    const int stateless_fd = connectLocal(port);
    bool stateless_sent = stateless_fd >= 0 &&
        sendAll(stateless_fd, stateless_headers.data(), stateless_headers.size()) &&
        sendAll(stateless_fd, stateless_body, stateless_size);
    free(stateless_body);
    const std::string stateless_response = stateless_sent
        ? receiveResponse(stateless_fd) : std::string();
    if (stateless_fd >= 0) closeSocket(stateless_fd);

    const std::string classic_body =
        "Content-Location: https://example.test/classic.mp4\r\n"
        "Start-Position: 2.500000\r\n";
    const std::string classic_request =
        std::string("POST /play HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n"
        "Content-Type: Text/Parameters; charset=UTF-8\r\n"
        "Content-Length: " + std::to_string(classic_body.size()) + "\r\n\r\n" +
        classic_body;
    const std::string classic_response = httpExchange(port, classic_request);

    const std::string unsafe_body =
        "Content-Location: file:///etc/passwd\r\n"
        "Start-Position: 0\r\n";
    const std::string unsafe_request =
        std::string("POST /play HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: " + std::to_string(unsafe_body.size()) + "\r\n\r\n" +
        unsafe_body;
    const std::string unsafe_response = httpExchange(port, unsafe_request);

    plist_t missing_root = plist_new_dict();
    char* missing_body = nullptr;
    uint32_t missing_size = 0;
    plist_to_bin(missing_root, &missing_body, &missing_size);
    plist_free(missing_root);
    const std::string missing_headers =
        std::string("POST /play HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n" +
        "X-Apple-Session-ID: " + session + "\r\n" +
        "Content-Type: application/x-apple-binary-plist\r\n" +
        "Content-Length: " + std::to_string(missing_size) + "\r\n\r\n";
    const int missing_fd = connectLocal(port);
    bool missing_sent = missing_fd >= 0 &&
        sendAll(missing_fd, missing_headers.data(), missing_headers.size()) &&
        sendAll(missing_fd, missing_body, missing_size);
    free(missing_body);
    const std::string missing_response = missing_sent
        ? receiveResponse(missing_fd) : std::string();
    // A rejected /play must not close the connection: iOS abandons the whole
    // AirPlay session when it does. The same socket must keep working.
    const std::string after_reject_request = control_request("GET", "/playback-info");
    const std::string after_reject_response =
        missing_fd >= 0 && sendAll(missing_fd, after_reject_request.data(),
                                   after_reject_request.size())
        ? receiveResponse(missing_fd) : std::string();
    if (missing_fd >= 0) closeSocket(missing_fd);

    // Reproduce the device ordering: a stateful sender-local /play can arrive
    // before the sender upgrades the reverse channel. The receiver must retain
    // the playlist and send FCUP after the 101 response is written.
    const char* pending_blob_url = "blob:https://pwa.example/pending-item";
    plist_t pending_root = plist_new_dict();
    plist_dict_set_item(pending_root, "uuid",
                        plist_new_string("99999999-aaaa-bbbb-cccc-dddddddddddd"));
    plist_dict_set_item(pending_root, "Content-Location",
                        plist_new_string(pending_blob_url));
    const std::string pending_response =
        binaryPlistExchange(port, "/play", session, pending_root);
    plist_free(pending_root);

    const std::string reverse_request =
        std::string("POST /reverse HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n" +
        "X-Apple-Session-ID: " + session + "\r\n" +
        "X-Apple-Purpose: event\r\n" +
        "Connection: Upgrade\r\n" +
        "Upgrade: PTTH/1.0\r\n" +
        "Content-Length: 0\r\n\r\n";
    const bool reverse_sent = reverse_fd >= 0 &&
        sendAll(reverse_fd, reverse_request.data(), reverse_request.size());
    const std::string reverse_response = reverse_sent
        ? receiveUntil(reverse_fd, "101 Switching Protocols") : "";
    const std::string pending_fcup_request = reverse_sent
        ? (reverse_response.find("POST /event") != std::string::npos
            ? reverse_response : receiveUntil(reverse_fd, "POST /event"))
        : "";

    plist_t pending_params = plist_new_dict();
    plist_dict_set_item(pending_params, "FCUP_Response_StatusCode", plist_new_uint(0));
    plist_dict_set_item(pending_params, "FCUP_Response_RequestID", plist_new_uint(1));
    plist_dict_set_item(pending_params, "FCUP_Response_URL", plist_new_string(pending_blob_url));
    const char pending_mp4[] = {
        '\x00', '\x00', '\x00', '\x18', 'f', 't', 'y', 'p',
        'i', 's', 'o', 'm', '\x00', '\x00', '\x02', '\x00',
        'i', 's', 'o', 'm', 'i', 's', 'o', '2'
    };
    plist_dict_set_item(pending_params, "FCUP_Response_Data",
                        plist_new_data(pending_mp4, sizeof(pending_mp4)));
    plist_t pending_action = plist_new_dict();
    plist_dict_set_item(pending_action, "type", plist_new_string("unhandledURLResponse"));
    plist_dict_set_item(pending_action, "params", pending_params);
    std::string spool_before_pending;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        spool_before_pending = state.spooled_location;
    }
    const std::string pending_action_response =
        binaryPlistExchange(port, "/action", session, pending_action);
    plist_free(pending_action);
    waitForNewSpool(state, spool_before_pending, 5000);

    // Reproduce the device path: a stateful blob /play must stop the previous
    // URL player and ask Safari to return the page-local object over PTTH.
    const char* blob_url = "blob:https://pwa.example/offline-item";
    plist_t blob_root = plist_new_dict();
    plist_dict_set_item(blob_root, "uuid",
                        plist_new_string("bbbbbbbb-cccc-dddd-eeee-ffffffffffff"));
    plist_dict_set_item(blob_root, "Content-Location",
                        plist_new_string(blob_url));
    const std::string blob_response = binaryPlistExchange(port, "/play", session, blob_root);
    plist_free(blob_root);
    const std::string blob_fcup_request = receiveResponse(reverse_fd);

    const char mp4_header[] = {
        '\x00', '\x00', '\x00', '\x18', 'f', 't', 'y', 'p',
        'i', 's', 'o', 'm', '\x00', '\x00', '\x02', '\x00',
        'i', 's', 'o', 'm', 'i', 's', 'o', '2'
    };
    const size_t mp4_payload_size = large_blob ? 96168758U : sizeof(mp4_header);
    std::vector<char> mp4_payload(mp4_payload_size, 0);
    std::memcpy(mp4_payload.data(), mp4_header, sizeof(mp4_header));
    plist_t blob_params = plist_new_dict();
    // Safari reports status 0 for a successful page-local blob response.
    plist_dict_set_item(blob_params, "FCUP_Response_StatusCode", plist_new_uint(0));
    plist_dict_set_item(blob_params, "FCUP_Response_RequestID", plist_new_uint(1));
    plist_dict_set_item(blob_params, "FCUP_Response_URL", plist_new_string(blob_url));
    plist_dict_set_item(blob_params, "FCUP_Response_Data",
                        plist_new_data(mp4_payload.data(), mp4_payload.size()));
    plist_t blob_action = plist_new_dict();
    plist_dict_set_item(blob_action, "type", plist_new_string("unhandledURLResponse"));
    plist_dict_set_item(blob_action, "params", blob_params);
    // With --large-blob the reply is sent in 1 MiB pieces (like a sender on
    // Wi-Fi) while another connection polls /playback-info: the receiver's
    // single HTTP thread must keep answering it during and after the transfer.
    std::string blob_action_response;
    std::string spool_before_blob;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        spool_before_blob = state.spooled_location;
    }
    bool large_transfer_ok = true;
    double spool_wait_ms = 0.0;
    double worst_poll_ms = 0.0;
    int polls = 0;
    if (large_blob) {
        char* body = nullptr;
        uint32_t body_size = 0;
        plist_to_bin(blob_action, &body, &body_size);
        std::atomic<bool> transfer_done{false};
        // When the worst poll started, from the start of the upload, so a
        // slow one can be placed (upload, reply, spool write).
        const auto upload_started = std::chrono::steady_clock::now();
        double worst_poll_at_ms = 0.0;
        std::thread poller([&] {
            while (!transfer_done.load()) {
                const auto started = std::chrono::steady_clock::now();
                httpExchange(port, std::string("GET /playback-info HTTP/1.1\r\nHost: 127.0.0.1\r\n") +
                                       "X-Apple-Session-ID: " + session + "\r\nContent-Length: 0\r\n\r\n");
                const double took = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                if (took > worst_poll_ms) {
                    worst_poll_ms = took;
                    worst_poll_at_ms = std::chrono::duration<double, std::milli>(
                        started - upload_started).count();
                }
                ++polls;
                usleep(20000);
            }
        });
        const std::string headers = std::string("POST /action HTTP/1.1\r\nHost: 127.0.0.1\r\n") +
            "X-Apple-Session-ID: " + session + "\r\n" +
            "Content-Type: application/x-apple-binary-plist\r\n" +
            "Content-Length: " + std::to_string(body_size) + "\r\n\r\n";
        // A slow disk: the spool file takes 1.5 s to write. The receiver must
        // keep answering meanwhile (the file is written off its thread).
        raop_set_spool_delay_for_testing(raop, 1500);
        const int fd = connectLocal(port);
        bool sent = fd >= 0 && sendAll(fd, headers.data(), headers.size());
        for (uint32_t offset = 0; sent && offset < body_size; offset += 1U << 20) {
            sent = sendAll(fd, body + offset, std::min<uint32_t>(1U << 20, body_size - offset));
            usleep(10000);
        }
        if (sent) blob_action_response = receiveResponse(fd);
        if (fd >= 0) closeSocket(fd);
        const double reply_at_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - upload_started).count();
        const auto spool_started = std::chrono::steady_clock::now();
        waitForNewSpool(state, spool_before_blob, 30000);
        spool_wait_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - spool_started).count();
        raop_set_spool_delay_for_testing(raop, 0);
        usleep(200000);
        transfer_done.store(true);
        poller.join();
        free(body);
        // The receiver's thread never waits on the disk, so /playback-info
        // stays within 100 ms on a developer Mac through the transfer and the
        // slow (1.5 s) spool write; OBS_AIRPLAY_TIMING_SCALE widens it on
        // slower machines.
        const double budget_ms = 100.0 * timingScale();
        const bool in_budget = worst_poll_ms < budget_ms;
        std::printf("large_blob_transfer: %u bytes, %d polls, worst /playback-info %.1f ms "
                    "(budget %.0f ms) starting %.0f ms into the upload (reply at %.0f ms), "
                    "spooled %.0f ms after the reply -> %s\n", body_size, polls, worst_poll_ms,
                    budget_ms, worst_poll_at_ms, reply_at_ms, spool_wait_ms,
                    in_budget ? "ok" : "SLOW");
        large_transfer_ok = in_budget && polls > 10 && spool_wait_ms >= 1500.0;
    } else {
        blob_action_response = binaryPlistExchange(port, "/action", session, blob_action);
        waitForNewSpool(state, spool_before_blob, 5000);
    }
    plist_free(blob_action);

    bool blob_spooled = false;
    std::string blob_spooled_location;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        blob_spooled = state.spooled_bytes.size() >= sizeof(mp4_header) &&
            !std::memcmp(state.spooled_bytes.data(), mp4_header, sizeof(mp4_header)) &&
            state.spooled_size == static_cast<off_t>(mp4_payload.size()) &&
            spoolModePrivate(state.spooled_mode);
        blob_spooled_location = state.spooled_location;
    }

    // A native media app may report an iPad-local file:// URL instead of a
    // Safari blob:. The receiver must send the same reverse fetch request and
    // never try to open the iPad's path on the Mac.
    const char* file_url =
        "file:///private/var/mobile/Containers/Data/Application/import.mov";
    plist_t file_root = plist_new_dict();
    plist_dict_set_item(file_root, "uuid",
                        plist_new_string("dddddddd-eeee-ffff-0000-111111111111"));
    plist_dict_set_item(file_root, "Content-Location", plist_new_string(file_url));
    const std::string file_response = binaryPlistExchange(port, "/play", session, file_root);
    plist_free(file_root);
    const std::string file_fcup_request = receiveResponse(reverse_fd);

    plist_t file_params = plist_new_dict();
    plist_dict_set_item(file_params, "FCUP_Response_StatusCode", plist_new_uint(0));
    plist_dict_set_item(file_params, "FCUP_Response_RequestID", plist_new_uint(1));
    plist_dict_set_item(file_params, "FCUP_Response_URL", plist_new_string(file_url));
    plist_dict_set_item(file_params, "FCUP_Response_Data",
                        plist_new_data(mp4_payload.data(), mp4_payload.size()));
    plist_t file_action = plist_new_dict();
    plist_dict_set_item(file_action, "type", plist_new_string("unhandledURLResponse"));
    plist_dict_set_item(file_action, "params", file_params);
    std::string spool_before_file;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        spool_before_file = state.spooled_location;
    }
    const std::string file_action_response =
        binaryPlistExchange(port, "/action", session, file_action);
    plist_free(file_action);
    waitForNewSpool(state, spool_before_file, 5000);

    bool file_spooled = false;
    std::string file_spooled_location;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        file_spooled = state.spooled_bytes.size() >= sizeof(mp4_header) &&
            !std::memcmp(state.spooled_bytes.data(), mp4_header, sizeof(mp4_header)) &&
            state.spooled_size == static_cast<off_t>(mp4_payload.size()) &&
            spoolModePrivate(state.spooled_mode);
        file_spooled_location = state.spooled_location;
    }

    plist_t selected_root = plist_new_dict();
    char* selected_body = nullptr;
    uint32_t selected_size = 0;
    plist_to_bin(selected_root, &selected_body, &selected_size);
    plist_free(selected_root);
    const std::string selected_headers =
        std::string("PUT /setProperty?selectedMediaArray HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n" +
        "X-Apple-Session-ID: " + session + "\r\n" +
        "Content-Type: application/x-apple-binary-plist\r\n" +
        "Content-Length: " + std::to_string(selected_size) + "\r\n\r\n";
    const int selected_fd = connectLocal(port);
    bool selected_sent = selected_fd >= 0 &&
        sendAll(selected_fd, selected_headers.data(), selected_headers.size()) &&
        sendAll(selected_fd, selected_body, selected_size);
    free(selected_body);
    const std::string selected_response = selected_sent
        ? receiveResponse(selected_fd) : std::string();
    if (selected_fd >= 0) closeSocket(selected_fd);

    plist_t empty_action = plist_new_dict();
    const std::string action_without_session =
        binaryPlistExchange(port, "/action", nullptr, empty_action);
    const std::string action_without_play_1 =
        binaryPlistExchange(port, "/action", session, empty_action);
    const std::string action_without_play_2 =
        binaryPlistExchange(port, "/action", session, empty_action);

    // iOS AVPlayer AirPlay of an app-local file: no Content-Location; the
    // sender serves the file itself and sends host + path (device capture).
    const auto hosted_play = [&](double start) {
        plist_t hosted_root = plist_new_dict();
        plist_dict_set_item(hosted_root, "uuid",
                            plist_new_string("F17F8872-BD0E-4529-821B-FA2D649C630E"));
        plist_dict_set_item(hosted_root, "mediaType", plist_new_string("file"));
        plist_dict_set_item(hosted_root, "host",
                            plist_new_string("[fe80::1492:202d:7134:1f10]:7001"));
        plist_dict_set_item(hosted_root, "path",
                            plist_new_string("/1/767d07bd-907d-5fa4-872c-e2bb5587025f.mp4"));
        plist_dict_set_item(hosted_root, "Start-Position-Seconds", plist_new_real(start));
        plist_dict_set_item(hosted_root, "Start-Position", plist_new_real(0.635));
        plist_dict_set_item(hosted_root, "rate", plist_new_real(1.0));
        plist_dict_set_item(hosted_root, "streamType", plist_new_uint(0));
        plist_dict_set_item(hosted_root, "clientBundleID", plist_new_string("com.shelfcsl.Shelf"));
        const std::string response = binaryPlistExchange(port, "/play", session, hosted_root);
        plist_free(hosted_root);
        return response;
    };
    const std::string hosted_response = hosted_play(231.865);
    std::string hosted_location;
    float hosted_start = -1.0f;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        hosted_location = state.location;
        hosted_start = state.start_position;
    }
    // A repeated /play for the same item must use the new start position.
    const std::string hosted_repeat_response = hosted_play(12.0);
    float hosted_repeat_start = -1.0f;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        hosted_repeat_start = state.start_position;
    }
    // The URL must point at the connection's address (127.0.0.1 here), not
    // at the advertised host, with the advertised port and path.
    const bool hosted_ok = hosted_response.find("200 OK") != std::string::npos &&
        hosted_repeat_response.find("200 OK") != std::string::npos &&
        hosted_location == "http://127.0.0.1:7001/1/767d07bd-907d-5fa4-872c-e2bb5587025f.mp4" &&
        std::fabs(hosted_start - 231.865f) < 0.01f &&
        std::fabs(hosted_repeat_start - 12.0f) < 0.01f;

    // AVPlayer replacing its current item (device capture): playlistRemove
    // for the old uuid, then playlistInsert with a new item and no /play.
    plist_t remove_item = plist_new_dict();
    plist_dict_set_item(remove_item, "uuid",
                        plist_new_string("F17F8872-BD0E-4529-821B-FA2D649C630E"));
    plist_t remove_params = plist_new_dict();
    plist_dict_set_item(remove_params, "item", remove_item);
    plist_t remove_action = plist_new_dict();
    plist_dict_set_item(remove_action, "type", plist_new_string("playlistRemove"));
    plist_dict_set_item(remove_action, "params", remove_params);
    const std::string remove_response =
        binaryPlistExchange(port, "/action", session, remove_action);
    // A second remove while nothing is current must not fail.
    const std::string remove_again_response =
        binaryPlistExchange(port, "/action", session, remove_action);
    plist_free(remove_action);

    plist_t insert_item = plist_new_dict();
    plist_dict_set_item(insert_item, "uuid",
                        plist_new_string("2669CBEE-7337-4945-8620-5A4FF6263A57"));
    plist_dict_set_item(insert_item, "mediaType", plist_new_string("file"));
    plist_dict_set_item(insert_item, "streamType", plist_new_uint(0));
    plist_dict_set_item(insert_item, "host",
                        plist_new_string("[fe80::1492:202d:7134:1f10]:7001"));
    plist_dict_set_item(insert_item, "path",
                        plist_new_string("/1/2669CBEE-7337-4945-8620-5A4FF6263A57.mp4"));
    plist_dict_set_item(insert_item, "clientProcName", plist_new_string("Shelf"));
    plist_t insert_params = plist_new_dict();
    plist_dict_set_item(insert_params, "item", insert_item);
    plist_t insert_action = plist_new_dict();
    plist_dict_set_item(insert_action, "type", plist_new_string("playlistInsert"));
    plist_dict_set_item(insert_action, "params", insert_params);
    const std::string insert_response =
        binaryPlistExchange(port, "/action", session, insert_action);
    plist_free(insert_action);
    std::string inserted_location;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        inserted_location = state.location;
    }
    // Controls must reach the inserted item.
    const std::string insert_scrub_response = httpExchange(
        port, control_request("POST", "/scrub?position=80.5"));
    float insert_scrub = -1.0f;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        insert_scrub = state.scrub_position;
    }
    const bool insert_ok = remove_response.find("200 OK") != std::string::npos &&
        remove_again_response.find("200 OK") != std::string::npos &&
        insert_response.find("200 OK") != std::string::npos &&
        inserted_location == "http://127.0.0.1:7001/1/2669CBEE-7337-4945-8620-5A4FF6263A57.mp4" &&
        insert_scrub_response.find("200 OK") != std::string::npos &&
        std::fabs(insert_scrub - 80.5f) < 0.01f;
    std::printf("playlist_replace=%s inserted_url=%s\n", insert_ok ? "ok" : "FAILED",
                inserted_location.c_str());

    // Paused item swap (device capture): the sender pauses, removes the
    // item and inserts the next one. iOS sends no /rate for the new item
    // because its player's rate did not change, so the receiver must start
    // the inserted item paused rather than playing.
    const std::string pause_response = httpExchange(
        port, control_request("POST", "/rate?value=0.000000"));
    plist_t paused_remove_item = plist_new_dict();
    plist_dict_set_item(paused_remove_item, "uuid",
                        plist_new_string("2669CBEE-7337-4945-8620-5A4FF6263A57"));
    plist_t paused_remove_params = plist_new_dict();
    plist_dict_set_item(paused_remove_params, "item", paused_remove_item);
    plist_t paused_remove = plist_new_dict();
    plist_dict_set_item(paused_remove, "type", plist_new_string("playlistRemove"));
    plist_dict_set_item(paused_remove, "params", paused_remove_params);
    binaryPlistExchange(port, "/action", session, paused_remove);
    plist_free(paused_remove);
    int rate_calls_before_insert = 0;
    int plays_before_insert = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        rate_calls_before_insert = state.rate_calls;
        plays_before_insert = state.play_count;
    }
    plist_t paused_item = plist_new_dict();
    plist_dict_set_item(paused_item, "uuid",
                        plist_new_string("770AD279-55D4-48D7-8A02-9B3F54612E27"));
    plist_dict_set_item(paused_item, "mediaType", plist_new_string("file"));
    plist_dict_set_item(paused_item, "host",
                        plist_new_string("[fe80::1492:202d:7134:1f10]:7001"));
    plist_dict_set_item(paused_item, "path",
                        plist_new_string("/1/770AD279-55D4-48D7-8A02-9B3F54612E27.mp4"));
    plist_t paused_params = plist_new_dict();
    plist_dict_set_item(paused_params, "item", paused_item);
    plist_t paused_insert = plist_new_dict();
    plist_dict_set_item(paused_insert, "type", plist_new_string("playlistInsert"));
    plist_dict_set_item(paused_insert, "params", paused_params);
    const std::string paused_insert_response =
        binaryPlistExchange(port, "/action", session, paused_insert);
    plist_free(paused_insert);
    bool paused_insert_ok = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        paused_insert_ok = pause_response.find("200 OK") != std::string::npos &&
            paused_insert_response.find("200 OK") != std::string::npos &&
            state.play_count == plays_before_insert + 1 &&
            state.rate_calls == rate_calls_before_insert + 1 &&
            std::fabs(state.playback_rate) < 0.001f;
    }
    std::printf("paused_item_swap=%s\n", paused_insert_ok ? "ok" : "FAILED");
    // Leave the rate as the final assertions expect it.
    httpExchange(port, control_request("POST", "/rate?value=0.5"));

    // Restore valid state, then exercise malformed plist/type/session cleanup
    // paths where type must still be either initialized or safely null.
    plist_t recovery_root = plist_new_dict();
    plist_dict_set_item(recovery_root, "uuid",
                        plist_new_string("cccccccc-dddd-eeee-ffff-000000000000"));
    plist_dict_set_item(recovery_root, "Content-Location",
                        plist_new_string("https://example.test/recovery.mp4"));
    const std::string recovery_response =
        binaryPlistExchange(port, "/play", session, recovery_root);
    plist_free(recovery_root);

    const std::string action_missing_type_1 =
        binaryPlistExchange(port, "/action", session, empty_action);
    const std::string action_missing_type_2 =
        binaryPlistExchange(port, "/action", session, empty_action);
    const std::string action_wrong_session =
        binaryPlistExchange(port, "/action", "wrong-session", empty_action);
    plist_free(empty_action);

    const std::string invalid_action_body = "not-a-binary-plist";
    const std::string invalid_action_request =
        std::string("POST /action HTTP/1.1\r\n") +
        "Host: 127.0.0.1\r\n" +
        "X-Apple-Session-ID: " + session + "\r\n" +
        "Content-Type: application/x-apple-binary-plist\r\n" +
        "Content-Length: " + std::to_string(invalid_action_body.size()) + "\r\n\r\n" +
        invalid_action_body;
    const std::string invalid_action_response = httpExchange(port, invalid_action_request);

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait_for(lock, std::chrono::seconds(2), [&] { return !state.location.empty(); });
    }

    const bool file_spool_cleaned = !file_spooled_location.empty() &&
        access(file_spooled_location.c_str(), F_OK) != 0;
    const bool blob_spool_replaced = !blob_spooled_location.empty() &&
        access(blob_spooled_location.c_str(), F_OK) != 0;
    // Each condition by name, so a failure says which one.
    const std::pair<const char*, bool> conditions[] = {
        PROTOCOL_CONDITION(native_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(native_response.find("CSeq: 1") != std::string::npos),
        PROTOCOL_CONDITION(reverse_response.find("101 Switching Protocols") != std::string::npos),
        PROTOCOL_CONDITION(play_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(unexpected_master_response.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(unexpected_media_response.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(playback_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(playback_response.find("readyToPlay") != std::string::npos),
        PROTOCOL_CONDITION(rate_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(scrub_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(stop_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(stateless_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(classic_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(unsafe_response.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(missing_response.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(missing_response.find("Connection: close") == std::string::npos),
        PROTOCOL_CONDITION(after_reject_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(hosted_ok),
        PROTOCOL_CONDITION(insert_ok),
        PROTOCOL_CONDITION(paused_insert_ok),
        PROTOCOL_CONDITION(pending_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(pending_fcup_request.find(pending_blob_url) != std::string::npos),
        PROTOCOL_CONDITION(pending_action_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(blob_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(blob_fcup_request.find(blob_url) != std::string::npos),
        PROTOCOL_CONDITION(blob_action_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(blob_spooled),
        PROTOCOL_CONDITION(blob_spool_replaced),
        PROTOCOL_CONDITION(file_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(file_fcup_request.find(file_url) != std::string::npos),
        PROTOCOL_CONDITION(file_action_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(file_spooled),
        PROTOCOL_CONDITION(file_spool_cleaned),
        PROTOCOL_CONDITION(!selected_response.empty()),
        PROTOCOL_CONDITION(action_without_session.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(action_without_play_1.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(action_without_play_2.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(recovery_response.find("200 OK") != std::string::npos),
        PROTOCOL_CONDITION(action_missing_type_1.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(action_missing_type_2.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(action_wrong_session.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(invalid_action_response.find("400 Bad Request") != std::string::npos),
        PROTOCOL_CONDITION(state.first_location == "http://media.example.test/sample.mp4"),
        PROTOCOL_CONDITION(state.location == "https://example.test/recovery.mp4"),
        PROTOCOL_CONDITION(std::fabs(state.start_position) < 0.001f),
        PROTOCOL_CONDITION(std::fabs(state.playback_rate - 0.5f) < 0.001f),
        PROTOCOL_CONDITION(std::fabs(state.scrub_position - 80.5f) < 0.001f),
        PROTOCOL_CONDITION(state.stop_count == 6),
        PROTOCOL_CONDITION(state.playback_info_count == 2 + polls),
        PROTOCOL_CONDITION(state.play_count == 11),
    };
    bool passed = true;
    for (const auto& condition : conditions) {
        if (!condition.second) {
            std::printf("FAILED: %s\n", condition.first);
            passed = false;
        }
    }
    std::printf("sender_hosted=%s hosted_url=%s hosted_start=%.3f reject_keeps_connection=%s\n",
                hosted_ok ? "ok" : "failed", hosted_location.c_str(), hosted_start,
                after_reject_response.find("200 OK") != std::string::npos ? "yes" : "no");
    std::printf("native=%s reverse=%s bplist=%s classic=%s unsafe=%s missing_url=%s blob=%s file=%s "
                "spool_size=%lld spool_mode=%03o spool_header=%zu blob_replaced=%d file_cleaned=%d "
                "repeated_actions=%s controls=%s callback_url=%s start=%.2f rate=%.2f "
                "scrub=%.2f stop=%d plays=%d\n",
                native_response.empty() ? "missing" : "ok",
                reverse_response.empty() ? "missing" : "ok",
                play_response.empty() ? "missing" : "ok",
                classic_response.empty() ? "missing" : "ok",
                unsafe_response.find("400 Bad Request") == std::string::npos ? "failed" : "rejected",
                missing_response.find("400 Bad Request") == std::string::npos ? "failed" : "rejected",
                blob_response.find("200 OK") == std::string::npos ||
                        blob_action_response.find("200 OK") == std::string::npos || !blob_spooled
                    ? "failed" : "reverse-spooled",
                file_response.find("200 OK") == std::string::npos ||
                        file_fcup_request.find(file_url) == std::string::npos ||
                        file_action_response.find("200 OK") == std::string::npos || !file_spooled
                    ? "failed" : "reverse-spooled",
                static_cast<long long>(state.spooled_size),
                static_cast<unsigned>(state.spooled_mode),
                state.spooled_bytes.size(), blob_spool_replaced, file_spool_cleaned,
                action_without_play_1.find("400 Bad Request") == std::string::npos ||
                        action_missing_type_2.find("400 Bad Request") == std::string::npos ||
                        invalid_action_response.find("400 Bad Request") == std::string::npos
                    ? "failed" : "rejected",
                playback_response.empty() || rate_response.empty() ||
                        scrub_response.empty() || stop_response.empty() ? "missing" : "ok",
                state.location.c_str(), state.start_position, state.playback_rate,
                state.scrub_position, state.stop_count, state.play_count);

    // POST /stop while a reverse fetch is under way cancels it: the sender's
    // late reply must be rejected and must not start playback.
    const char* cancelled_url = "blob:https://pwa.example/cancelled-item";
    plist_t cancelled_root = plist_new_dict();
    plist_dict_set_item(cancelled_root, "uuid",
                        plist_new_string("dddddddd-eeee-ffff-0000-111111111111"));
    plist_dict_set_item(cancelled_root, "Content-Location", plist_new_string(cancelled_url));
    const std::string cancelled_play_response =
        binaryPlistExchange(port, "/play", session, cancelled_root);
    plist_free(cancelled_root);
    const std::string cancelled_fcup_request = receiveResponse(reverse_fd);
    const std::string cancelled_stop_response = httpExchange(
        port, control_request("POST", "/stop"));
    int plays_before_late_reply = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        plays_before_late_reply = state.play_count;
    }
    plist_t late_params = plist_new_dict();
    plist_dict_set_item(late_params, "FCUP_Response_StatusCode", plist_new_uint(0));
    plist_dict_set_item(late_params, "FCUP_Response_RequestID", plist_new_uint(1));
    plist_dict_set_item(late_params, "FCUP_Response_URL", plist_new_string(cancelled_url));
    plist_dict_set_item(late_params, "FCUP_Response_Data",
                        plist_new_data(mp4_header, sizeof(mp4_header)));
    plist_t late_action = plist_new_dict();
    plist_dict_set_item(late_action, "type", plist_new_string("unhandledURLResponse"));
    plist_dict_set_item(late_action, "params", late_params);
    const std::string late_reply_response =
        binaryPlistExchange(port, "/action", session, late_action);
    plist_free(late_action);
    int plays_after_late_reply = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        plays_after_late_reply = state.play_count;
    }
    const bool stop_cancels_fetch =
        cancelled_play_response.find("200 OK") != std::string::npos &&
        cancelled_fcup_request.find(cancelled_url) != std::string::npos &&
        cancelled_stop_response.find("200 OK") != std::string::npos &&
        late_reply_response.find("400 Bad Request") != std::string::npos &&
        plays_after_late_reply == plays_before_late_reply;
    std::printf("stop_cancels_reverse_fetch=%s\n", stop_cancels_fetch ? "ok" : "FAILED");

    // Master playlists that crashed the receiver (UxPlay's HLS language
    // choice and media URI table, audit 2026-10-21): a LANGUAGE longer than
    // its 6-byte field (NET-1), exactly one entry (count/0, NET-2), no
    // DEFAULT=YES and no preferred-language match (languages[-1], NET-3),
    // and no URI under the item's prefix (a zero-length media store). Each
    // is the reply to the sender's own HLS /play; the receiver must keep
    // answering afterwards.
    const auto media_line = [](const std::string& prefix, const std::string& lang,
                               const char* is_default, const char* name) {
        return "#EXT-X-MEDIA:URI=\"" + prefix + "/" + lang.substr(0, 8) +
            "/index.m3u8\",TYPE=AUDIO,GROUP-ID=\"233\",DEFAULT=" + is_default +
            ",AUTOSELECT=YES,NAME=\"" + name + "\",LANGUAGE=\"" + lang +
            "\",YT-EXT-AUDIO-CONTENT-ID=\"" + lang + ".4\"\n";
    };
    const auto variant_lines = [](const std::string& prefix) {
        return "#EXT-X-STREAM-INF:BANDWIDTH=1176518,CODECS=\"avc1.4d401f,mp4a.40.2\","
               "AUDIO=\"233\"\n" + prefix + "/230/index.m3u8\n";
    };
    const auto hls_prefix = [](const char* label) {
        return std::string("http://media.example.test/") + label;
    };
    const std::string net1 = hls_prefix("net1-long-language");
    const std::string long_language(300, 'A');
    const std::string net1_playlist = "#EXTM3U\n" +
        media_line(net1, long_language, "YES", "English") +
        media_line(net1, long_language, "NO", "French") + variant_lines(net1);
    const std::string net2 = hls_prefix("net2-one-entry");
    const std::string net2_playlist = "#EXTM3U\n" + media_line(net2, "en", "YES", "English") +
        variant_lines(net2);
    const std::string net3 = hls_prefix("net3-no-default");
    const std::string net3_playlist = "#EXTM3U\n" + media_line(net3, "en", "NO", "English") +
        media_line(net3, "fr", "NO", "French") + variant_lines(net3);
    const std::string foreign = hls_prefix("other-host");
    const std::string foreign_playlist = "#EXTM3U\n" +
        media_line(foreign, "en", "YES", "English") + variant_lines(foreign);
    const auto hls_master_reply = [&](const char* label, const char* uuid,
                                      const std::string& playlist) {
        const std::string url = hls_prefix(label) + "/master.m3u8";
        plist_t hls_root = plist_new_dict();
        plist_dict_set_item(hls_root, "uuid", plist_new_string(uuid));
        plist_dict_set_item(hls_root, "Content-Location", plist_new_string(url.c_str()));
        const std::string hls_play_response =
            binaryPlistExchange(port, "/play", session, hls_root);
        plist_free(hls_root);
        // (the previous item's media-playlist requests may come first)
        const std::string hls_fcup_request = receiveUntil(reverse_fd, url.c_str());
        plist_t hls_params = plist_new_dict();
        plist_dict_set_item(hls_params, "FCUP_Response_StatusCode", plist_new_uint(200));
        plist_dict_set_item(hls_params, "FCUP_Response_RequestID", plist_new_uint(1));
        plist_dict_set_item(hls_params, "FCUP_Response_URL", plist_new_string(url.c_str()));
        plist_dict_set_item(hls_params, "FCUP_Response_Data",
                            plist_new_data(playlist.data(), playlist.size()));
        plist_t hls_action = plist_new_dict();
        plist_dict_set_item(hls_action, "type", plist_new_string("unhandledURLResponse"));
        plist_dict_set_item(hls_action, "params", hls_params);
        const std::string hls_action_response =
            binaryPlistExchange(port, "/action", session, hls_action);
        plist_free(hls_action);
        const std::string alive_response =
            httpExchange(port, control_request("GET", "/playback-info"));
        httpExchange(port, control_request("POST", "/stop"));
        const bool ok = hls_play_response.find("200 OK") != std::string::npos &&
            hls_fcup_request.find(url) != std::string::npos &&
            !hls_action_response.empty() &&
            alive_response.find("200 OK") != std::string::npos;
        std::printf("hls_master_%s=%s\n", label, ok ? "ok" : "FAILED");
        return ok;
    };
    const bool hls_master_ok =
        hls_master_reply("net1-long-language", "eeeeeeee-0000-0000-0000-000000000001",
                         net1_playlist) &&
        hls_master_reply("net2-one-entry", "eeeeeeee-0000-0000-0000-000000000002",
                         net2_playlist) &&
        hls_master_reply("net3-no-default", "eeeeeeee-0000-0000-0000-000000000003",
                         net3_playlist) &&
        hls_master_reply("foreign-uris", "eeeeeeee-0000-0000-0000-000000000004",
                         foreign_playlist);

    // Closing the last AirPlay video connection without POST /stop must stop
    // URL playback so the output does not keep a stale frame.
    int stops_before_close = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        stops_before_close = state.stop_count;
    }
    if (play_fd >= 0) closeSocket(play_fd);
    int stops_after_close = stops_before_close;
    for (int i = 0; i < 40 && stops_after_close == stops_before_close; ++i) {
        usleep(50000);
        std::lock_guard<std::mutex> lock(state.mutex);
        stops_after_close = state.stop_count;
    }
    const bool close_stops = stops_after_close == stops_before_close + 1;
    std::printf("close_without_stop=%s\n", close_stops ? "stopped" : "FAILED");
    if (reverse_fd >= 0) closeSocket(reverse_fd);

    const std::string info_request = "GET /info RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";

    // POST /fp-setup2 with no body (or one too short to hold the FairPlay
    // version byte) is answered 400; 2.5.1 dereferenced the missing body.
    const auto fp_setup2 = [&](const std::string& body) {
        return httpExchange(port,
                            std::string("POST /fp-setup2 HTTP/1.1\r\n") +
                                "Host: 127.0.0.1\r\n" +
                                "X-Apple-Session-ID: " + session + "\r\n" +
                                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
                                body);
    };
    const std::string fp_empty_response = fp_setup2("");
    const std::string fp_short_response = fp_setup2("\x46\x50");
    const std::string fp_survived_response = httpExchange(port, info_request);
    const bool fp_setup2_ok =
        fp_empty_response.find("400 Bad Request") != std::string::npos &&
        fp_short_response.find("400 Bad Request") != std::string::npos &&
        fp_survived_response.find("200 OK") != std::string::npos;
    std::printf("fp_setup2_short_body=%s\n", fp_setup2_ok ? "rejected" : "FAILED");

    // A binary plist whose string holds a NUL byte: libplist's getters assert
    // on it (and these tests link a libplist built with asserts), so reading
    // it would abort the receiver. It is refused, and the receiver carries on.
    // (a UTF-16 string, as a sender writes non-ASCII text: libplist keeps
    // the NUL inside the converted string, where an ASCII string is cut)
    static const unsigned char nul_string_play[] = {
        0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd2, 0x01, 0x02, 0x03, 0x04, 0x5f,
        0x10, 0x10, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x4c, 0x6f, 0x63, 0x61,
        0x74, 0x69, 0x6f, 0x6e, 0x54, 0x75, 0x75, 0x69, 0x64, 0x6f, 0x10, 0x19, 0x00, 0x68,
        0x00, 0x74, 0x00, 0x74, 0x00, 0x70, 0x00, 0x3a, 0x00, 0x2f, 0x00, 0x2f, 0x00, 0x31,
        0x00, 0x39, 0x00, 0x32, 0x00, 0x2e, 0x00, 0x30, 0x00, 0x2e, 0x00, 0x32, 0x00, 0x2e,
        0x00, 0x31, 0x00, 0x30, 0x00, 0x2f, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x62, 0x00, 0x2e,
        0x00, 0x6d, 0x00, 0x70, 0x00, 0x34, 0x5f, 0x10, 0x24, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x2d, 0x66, 0x66, 0x66, 0x66, 0x2d, 0x30, 0x30, 0x30, 0x30, 0x2d,
        0x31, 0x31, 0x31, 0x31, 0x2d, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
        0x32, 0x32, 0x32, 0x08, 0x0d, 0x20, 0x25, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81};
    const std::string nul_body(reinterpret_cast<const char*>(nul_string_play),
                               sizeof(nul_string_play));
    const std::string nul_play_response = httpExchange(port,
        std::string("POST /play HTTP/1.1\r\nX-Apple-Session-ID: "
                    "99999999-8888-7777-6666-555555555555\r\n"
                    "Content-Type: application/x-apple-binary-plist\r\nContent-Length: ") +
        std::to_string(nul_body.size()) + "\r\n\r\n" + nul_body);
    const std::string nul_survived_response = httpExchange(port, info_request);
    const bool nul_string_ok =
        nul_play_response.find("400 Bad Request") != std::string::npos &&
        nul_survived_response.find("200 OK") != std::string::npos;
    std::printf("plist_nul_string=%s\n", nul_string_ok ? "rejected" : "FAILED");

    // A connection that sends one byte and stalls must not hold up the single
    // HTTP thread: another connection's GET /info is still answered promptly.
    const int stalled_fd = connectLocal(port);
    const bool stalled_sent = stalled_fd >= 0 && sendAll(stalled_fd, "G", 1);
    usleep(100000);
    const auto info_started = std::chrono::steady_clock::now();
    std::string stalled_info_response;
    const int info_fd = connectLocal(port);
    if (info_fd >= 0) {
        setTimeout(info_fd, SO_RCVTIMEO, 3000);
        if (sendAll(info_fd, info_request.data(), info_request.size())) {
            stalled_info_response = receiveResponse(info_fd);
        }
        closeSocket(info_fd);
    }
    const auto info_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - info_started).count();
    if (stalled_fd >= 0) closeSocket(stalled_fd);
    const bool stall_ok = stalled_sent &&
        stalled_info_response.find("200 OK") != std::string::npos && info_ms < 2000;
    std::printf("one_byte_stall=%s info_ms=%lld\n", stall_ok ? "not-blocking" : "FAILED",
                static_cast<long long>(info_ms));

    // POST /play may not point the receiver at loopback, link-local,
    // multicast or unspecified addresses, or at unusual ports on hosts
    // other than the sender.
    const auto play_status = [&](const char* url) {
        plist_t root = plist_new_dict();
        plist_dict_set_item(root, "Content-Location", plist_new_string(url));
        const std::string response = binaryPlistExchange(port, "/play", session, root);
        plist_free(root);
        return response.find("200 OK") != std::string::npos ? "200"
            : response.find("400 Bad Request") != std::string::npos ? "400" : "other";
    };
    struct PolicyCase { const char* url; const char* expected; };
    std::vector<PolicyCase> policy_cases;
    for (const SharedPolicyCase& c : kSharedPolicyCases) {
        policy_cases.push_back({c.url, c.allowed ? "200" : "400"});
    }
    // A name is checked textually here (the player resolves it later).
    policy_cases.push_back(
        {"https://user:hunter2@media.example.test:8443/x.mp4?token=SECRET123#frag", "200"});
    bool policy_ok = true;
    for (const PolicyCase& c : policy_cases) {
        const char* status = play_status(c.url);
        if (std::strcmp(status, c.expected) != 0) {
            std::fprintf(stderr, "url policy: %s -> %s (expected %s)\n", c.url, status, c.expected);
            policy_ok = false;
        }
    }
    const std::string refused_body = "Content-Location: http://192.168.1.1:23/x\r\n\r\n";
    const std::string classic_refused = httpExchange(
        port,
        std::string("POST /play HTTP/1.1\r\n") +
            "Host: 127.0.0.1\r\n" +
            "X-Apple-Session-ID: " + session + "\r\n" +
            "Content-Type: text/parameters\r\n" +
            "Content-Length: " + std::to_string(refused_body.size()) + "\r\n\r\n" +
            refused_body);
    policy_ok = policy_ok && classic_refused.find("400 Bad Request") != std::string::npos;
    std::printf("url_policy=%s\n", policy_ok ? "enforced" : "FAILED");

    // Receiver setting "AirPlay video": "Only from the sending device"
    // refuses other hosts but not the sender's own media server; "Off"
    // refuses every item, however it arrives. Each refusal reaches the host
    // with its reason.
    const auto refusal = [&]() {
        std::lock_guard<std::mutex> lock(state.mutex);
        return std::make_pair(state.refused_count, state.refused_reason);
    };
    const auto classic_play = [&](const char* url) {
        const std::string body = std::string("Content-Location: ") + url + "\r\n\r\n";
        return httpExchange(
            port,
            std::string("POST /play HTTP/1.1\r\n") +
                "Host: 127.0.0.1\r\n" +
                "X-Apple-Session-ID: " + session + "\r\n" +
                "Content-Type: text/parameters\r\n" +
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    };
    const auto is_400 = [](const std::string& response) {
        return response.find("400 Bad Request") != std::string::npos;
    };
    bool video_settings_ok = true;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "video settings: %s failed\n", what);
            video_settings_ok = false;
        }
    };
    {
        const int before = refusal().first;
        raop_set_media_policy(raop, true, true);
        expect(std::strcmp(play_status("http://10.1.2.3/x.mp4"), "400") == 0,
               "sender-only refuses another host");
        const auto after_other = refusal();
        expect(after_other.first == before + 1 &&
                   after_other.second.find("not the sending device") != std::string::npos,
               "sender-only reports its reason");
        expect(std::strcmp(play_status("https://media.example.test/x.mp4"), "400") == 0,
               "sender-only refuses a host name");
        expect(is_400(classic_play("http://10.1.2.3/x.mp4")), "sender-only refuses classic /play");
        expect(hosted_play(1.0).find("200 OK") != std::string::npos,
               "sender-only accepts the sender's own media server");

        raop_set_media_policy(raop, false, false);
        const int off_before = refusal().first;
        expect(std::strcmp(play_status("http://10.1.2.3/x.mp4"), "400") == 0,
               "video off refuses binary /play");
        expect(is_400(hosted_play(1.0)), "video off refuses sender-hosted /play");
        expect(is_400(classic_play("http://10.1.2.3/x.mp4")), "video off refuses classic /play");
        plist_t off_item = plist_new_dict();
        plist_dict_set_item(off_item, "uuid", plist_new_string("0FF0FF0F-0000-4000-8000-000000000001"));
        plist_dict_set_item(off_item, "Content-Location",
                            plist_new_string("http://10.1.2.3/inserted.mp4"));
        plist_t off_params = plist_new_dict();
        plist_dict_set_item(off_params, "item", off_item);
        plist_t off_action = plist_new_dict();
        plist_dict_set_item(off_action, "type", plist_new_string("playlistInsert"));
        plist_dict_set_item(off_action, "params", off_params);
        expect(is_400(binaryPlistExchange(port, "/action", session, off_action)),
               "video off refuses playlistInsert");
        plist_free(off_action);
        const auto after_off = refusal();
        expect(after_off.first == off_before + 4 &&
                   after_off.second.find("turned off") != std::string::npos,
               "video off reports every refusal");

        raop_set_media_policy(raop, true, false);
        expect(std::strcmp(play_status("http://10.1.2.3/x.mp4"), "200") == 0,
               "defaults accept another host again");
    }
    std::printf("video_settings=%s\n", video_settings_ok ? "enforced" : "FAILED");

    // The session belongs to the peer whose /play started it: another peer
    // (here ::1) cannot stop, pause, scrub or edit it.
    const auto exchange6 = [&](const std::string& request) {
        const int fd = connectLocal6(port);
        std::string response;
        if (fd >= 0 && sendAll(fd, request.data(), request.size())) response = receiveResponse(fd);
        if (fd >= 0) closeSocket(fd);
        return response;
    };
    // The owner keeps its AirPlay connection open, as a sender does.
    const int owner_fd = connectLocal(port);
    std::string owned_response;
    {
        plist_t root = plist_new_dict();
        plist_dict_set_item(root, "Content-Location", plist_new_string("http://10.1.2.3/owned.mp4"));
        char* body = nullptr;
        uint32_t body_size = 0;
        plist_to_bin(root, &body, &body_size);
        plist_free(root);
        const std::string headers = std::string("POST /play HTTP/1.1\r\n") +
            "Host: 127.0.0.1\r\n" + "X-Apple-Session-ID: " + session + "\r\n" +
            "Content-Type: application/x-apple-binary-plist\r\n" +
            "Content-Length: " + std::to_string(body_size) + "\r\n\r\n";
        if (owner_fd >= 0 && sendAll(owner_fd, headers.data(), headers.size()) &&
            sendAll(owner_fd, body, body_size)) {
            owned_response = receiveResponse(owner_fd);
        }
        free(body);
    }
    const char* owned_status = owned_response.find("200 OK") != std::string::npos ? "200" : "other";
    int stops_before_foreign = 0;
    float rate_before_foreign = 0.0f;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        stops_before_foreign = state.stop_count;
        rate_before_foreign = state.playback_rate;
    }
    const std::string foreign_stop = exchange6(control_request("POST", "/stop"));
    const std::string foreign_rate = exchange6(control_request("POST", "/rate?value=0.25"));
    const std::string foreign_scrub = exchange6(control_request("POST", "/scrub?position=3"));
    int stops_after_foreign = 0;
    float rate_after_foreign = 0.0f;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        stops_after_foreign = state.stop_count;
        rate_after_foreign = state.playback_rate;
    }
    std::string owner_stop;
    {
        const std::string request = control_request("POST", "/stop");
        if (owner_fd >= 0 && sendAll(owner_fd, request.data(), request.size())) {
            owner_stop = receiveResponse(owner_fd);
        }
        if (owner_fd >= 0) closeSocket(owner_fd);
        usleep(100000);
    }
    // With the session over, any peer may control again.
    const std::string later_stop = exchange6(control_request("POST", "/stop"));
    const bool binding_ok = std::strcmp(owned_status, "200") == 0 &&
        foreign_stop.find("403") != std::string::npos &&
        foreign_rate.find("403") != std::string::npos &&
        foreign_scrub.find("403") != std::string::npos &&
        stops_after_foreign == stops_before_foreign &&
        rate_after_foreign == rate_before_foreign &&
        owner_stop.find("200 OK") != std::string::npos &&
        later_stop.find("200 OK") != std::string::npos;
    std::printf("session_binding=%s owned=%.12s foreign_stop=%.12s owner_stop=%.12s later_stop=%.12s\n",
                binding_ok ? "enforced" : "FAILED", owned_response.c_str(), foreign_stop.c_str(),
                owner_stop.c_str(), later_stop.c_str());

    // Bodies are capped (16 MiB unless a reverse fetch is pending) and share
    // one buffer budget across connections; over either limit the request
    // is answered 413 and its buffer released.
    const auto post_headers = [&](size_t length) {
        return std::string("POST /fp-setup2 HTTP/1.1\r\n") +
            "Host: 127.0.0.1\r\n" +
            "X-Apple-Session-ID: " + session + "\r\n" +
            "Content-Length: " + std::to_string(length) + "\r\n\r\n";
    };
    const std::string oversized_response =
        httpExchange(port, post_headers(20U * 1024U * 1024U));
    const bool oversized_ok = oversized_response.find("413") != std::string::npos;

    raop_set_plist(raop, "max_inflight_body_mib", 40);
    const size_t declared = 15U * 1024U * 1024U;
    const std::vector<char> chunk(14U * 1024U * 1024U, 'x');
    int held[3] = {-1, -1, -1};
    std::string held_response[3];
    for (int i = 0; i < 3; ++i) {
        held[i] = connectLocal(port);
        const std::string headers = post_headers(declared);
        if (held[i] < 0 || !sendAll(held[i], headers.data(), headers.size()) ||
            !sendAll(held[i], chunk.data(), chunk.size())) {
            break;
        }
        usleep(300000);
    }
    // Only the connection that went over the budget has been answered.
    for (int i = 0; i < 3; ++i) {
        if (held[i] < 0) continue;
        setTimeout(held[i], SO_RCVTIMEO, 300);
        held_response[i] = receiveResponse(held[i]);
    }
    for (int& fd : held) {
        if (fd >= 0) closeSocket(fd);
    }
    usleep(300000);
    // With the held bodies gone, the budget is free again.
    std::string released_response;
    {
        const int fd = connectLocal(port);
        const std::string headers = post_headers(declared);
        const std::vector<char> body(declared, 'y');
        if (fd >= 0 && sendAll(fd, headers.data(), headers.size()) &&
            sendAll(fd, body.data(), body.size())) {
            released_response = receiveResponse(fd);
        }
        if (fd >= 0) closeSocket(fd);
    }
    raop_set_plist(raop, "max_inflight_body_mib", 256);
    const bool budget_ok = held_response[0].empty() && held_response[1].empty() &&
        held_response[2].find("413") != std::string::npos &&
        !released_response.empty() && released_response.find("413") == std::string::npos;
    std::printf("body_limits=%s oversized=%s held=[%s|%s|%s] released=%.12s\n",
                oversized_ok && budget_ok ? "enforced" : "FAILED",
                oversized_ok ? "413" : "missing",
                held_response[0].empty() ? "-" : held_response[0].substr(9, 3).c_str(),
                held_response[1].empty() ? "-" : held_response[1].substr(9, 3).c_str(),
                held_response[2].empty() ? "-" : held_response[2].substr(9, 3).c_str(),
                released_response.c_str());

    // A peer that keeps sending requests but never reads the responses
    // fills the socket buffers; the receiver's send() must time out and drop
    // it instead of blocking every other peer.
    bool nonreader_ok = false;
    long long nonreader_ms = 0;
    {
        const int flood_fd = socket(AF_INET, SOCK_STREAM, 0);
        setIntOption(flood_fd, SO_RCVBUF, 1024);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        int flooded = 0;
        if (connect(flood_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            setTimeout(flood_fd, SO_SNDTIMEO, 200);
#ifdef SO_NOSIGPIPE
            setIntOption(flood_fd, SO_NOSIGPIPE, 1);
#endif
            // ~1.5 KB per /info response; well past the socket buffers.
            for (int i = 0; i < 1500; ++i) {
                if (!sendAll(flood_fd, info_request.data(), info_request.size())) break;
                ++flooded;
                usleep(1000);
            }
        }
        const auto started = std::chrono::steady_clock::now();
        std::string response;
        const int fd = connectLocal(port);
        if (fd >= 0) {
            setTimeout(fd, SO_RCVTIMEO, 6000);
            if (sendAll(fd, info_request.data(), info_request.size())) response = receiveResponse(fd);
            closeSocket(fd);
        }
        nonreader_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        closeSocket(flood_fd);
        nonreader_ok = flooded > 0 && response.find("200 OK") != std::string::npos;
        std::printf("non_reading_peer=%s requests=%d info_ms=%lld\n",
                    nonreader_ok ? "dropped" : "FAILED", flooded, nonreader_ms);
    }

    // With the fast-connect profile, /info reports the DNS-SD context's
    // features, so a bit masked there (FairPlay video) is masked in /info.
    const auto info_features = [&]() -> uint64_t {
        const std::string response = httpExchange(port, info_request);
        const size_t body = response.find("\r\n\r\n");
        if (body == std::string::npos) return 0;
        plist_t root = nullptr;
        plist_from_bin(response.data() + body + 4,
                       static_cast<uint32_t>(response.size() - body - 4), &root);
        uint64_t value = 0;
        plist_t node = root ? plist_dict_get_item(root, "features") : nullptr;
        if (node) plist_get_uint_val(node, &value);
        if (root) plist_free(root);
        return value;
    };
    raop_set_fast_connect(raop, true);
    dnssd_set_airserver_profile(dnssd, true);
    const uint64_t profile_features = info_features();
    dnssd_set_airplay_features(dnssd, 2, 0);
    const uint64_t masked_features = info_features();
    const bool features_ok = profile_features == UINT64_C(0x0E427FFFF7) &&
        masked_features == UINT64_C(0x0E427FFFF3);
    std::printf("fast_connect_features=%s profile=0x%llX masked=0x%llX\n",
                features_ok ? "ok" : "FAILED",
                static_cast<unsigned long long>(profile_features),
                static_cast<unsigned long long>(masked_features));

    raop_stop_httpd(raop);
    raop_destroy(raop);
    dnssd_destroy(dnssd);
    return passed && stop_cancels_fetch && hls_master_ok && close_stops && fp_setup2_ok && nul_string_ok &&
        stall_ok && policy_ok &&
        video_settings_ok &&
        oversized_ok && budget_ok && nonreader_ok && binding_ok && features_ok && large_transfer_ok ? 0 : 1;
}
