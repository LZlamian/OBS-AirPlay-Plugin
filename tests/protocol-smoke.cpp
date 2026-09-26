#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <plist/plist.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "raop.h"
}

namespace {

void receiverLog(void*, int level, const char* message)
{
    if (message && (level <= LOGGER_WARNING || std::strstr(message, "[MEDIA]")))
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
    int stop_count = 0;
    int playback_info_count = 0;
    int play_count = 0;
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
    if (state->location.find("/obs-airplay-fcup-") != std::string::npos) {
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
        close(fd);
        return -1;
    }
    timeval timeout = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

bool sendAll(int fd, const void* data, size_t size)
{
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t sent = send(fd, bytes, size, 0);
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
        if (fd >= 0) close(fd);
        return {};
    }
    const std::string response = receiveResponse(fd);
    close(fd);
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
    if (fd >= 0) close(fd);
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

    raop_t* raop = raop_init(&callbacks);
    if (!raop || raop_init2(raop, 1, "02:00:00:00:00:01", "") != 0) {
        std::fprintf(stderr, "failed to initialize receiver\n");
        return 1;
    }
    raop_set_log_callback(raop, receiverLog, nullptr);
    raop_set_log_level(raop, LOGGER_INFO);
    if (raop_set_plist(raop, "hls", 1) != 0) {
        std::fprintf(stderr, "failed to enable HLS receiver mode\n");
        raop_destroy(raop);
        return 1;
    }

    unsigned short port = 0;
    if (raop_start_httpd(raop, &port) < 0) {
        std::fprintf(stderr, "failed to start protocol smoke receiver\n");
        raop_destroy(raop);
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
                        plist_new_string("http://127.0.0.1:18765/sample.mp4"));
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
    if (stateless_fd >= 0) close(stateless_fd);

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
    if (missing_fd >= 0) close(missing_fd);

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
    const std::string pending_action_response =
        binaryPlistExchange(port, "/action", session, pending_action);
    plist_free(pending_action);

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
    const std::string blob_action_response =
        binaryPlistExchange(port, "/action", session, blob_action);
    plist_free(blob_action);

    bool blob_spooled = false;
    std::string blob_spooled_location;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        blob_spooled = state.spooled_bytes.size() >= sizeof(mp4_header) &&
            !std::memcmp(state.spooled_bytes.data(), mp4_header, sizeof(mp4_header)) &&
            state.spooled_size == static_cast<off_t>(mp4_payload.size()) &&
            state.spooled_mode == 0600;
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
    const std::string file_action_response =
        binaryPlistExchange(port, "/action", session, file_action);
    plist_free(file_action);

    bool file_spooled = false;
    std::string file_spooled_location;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        file_spooled = state.spooled_bytes.size() >= sizeof(mp4_header) &&
            !std::memcmp(state.spooled_bytes.data(), mp4_header, sizeof(mp4_header)) &&
            state.spooled_size == static_cast<off_t>(mp4_payload.size()) &&
            state.spooled_mode == 0600;
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
    if (selected_fd >= 0) close(selected_fd);

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
    const bool passed = native_response.find("200 OK") != std::string::npos &&
        native_response.find("CSeq: 1") != std::string::npos &&
        reverse_response.find("101 Switching Protocols") != std::string::npos &&
        play_response.find("200 OK") != std::string::npos &&
        playback_response.find("200 OK") != std::string::npos &&
        playback_response.find("readyToPlay") != std::string::npos &&
        rate_response.find("200 OK") != std::string::npos &&
        scrub_response.find("200 OK") != std::string::npos &&
        stop_response.find("200 OK") != std::string::npos &&
        stateless_response.find("200 OK") != std::string::npos &&
        classic_response.find("200 OK") != std::string::npos &&
        unsafe_response.find("400 Bad Request") != std::string::npos &&
        missing_response.find("400 Bad Request") != std::string::npos &&
        missing_response.find("Connection: close") == std::string::npos &&
        after_reject_response.find("200 OK") != std::string::npos &&
        hosted_ok && insert_ok &&
        pending_response.find("200 OK") != std::string::npos &&
        pending_fcup_request.find(pending_blob_url) != std::string::npos &&
        pending_action_response.find("200 OK") != std::string::npos &&
        blob_response.find("200 OK") != std::string::npos &&
        blob_fcup_request.find(blob_url) != std::string::npos &&
        blob_action_response.find("200 OK") != std::string::npos &&
        blob_spooled && blob_spool_replaced &&
        file_response.find("200 OK") != std::string::npos &&
        file_fcup_request.find(file_url) != std::string::npos &&
        file_action_response.find("200 OK") != std::string::npos &&
        file_spooled && file_spool_cleaned &&
        !selected_response.empty() &&
        action_without_session.find("400 Bad Request") != std::string::npos &&
        action_without_play_1.find("400 Bad Request") != std::string::npos &&
        action_without_play_2.find("400 Bad Request") != std::string::npos &&
        recovery_response.find("200 OK") != std::string::npos &&
        action_missing_type_1.find("400 Bad Request") != std::string::npos &&
        action_missing_type_2.find("400 Bad Request") != std::string::npos &&
        action_wrong_session.find("400 Bad Request") != std::string::npos &&
        invalid_action_response.find("400 Bad Request") != std::string::npos &&
        state.first_location == "http://127.0.0.1:18765/sample.mp4" &&
        state.location == "https://example.test/recovery.mp4" &&
        std::fabs(state.start_position) < 0.001f &&
        std::fabs(state.playback_rate - 0.5f) < 0.001f &&
        std::fabs(state.scrub_position - 80.5f) < 0.001f &&
        state.stop_count == 6 && state.playback_info_count == 2 &&
        state.play_count == 10;
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

    // Closing the last AirPlay video connection without POST /stop must stop
    // URL playback so the output does not keep a stale frame.
    int stops_before_close = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        stops_before_close = state.stop_count;
    }
    if (play_fd >= 0) close(play_fd);
    int stops_after_close = stops_before_close;
    for (int i = 0; i < 40 && stops_after_close == stops_before_close; ++i) {
        usleep(50000);
        std::lock_guard<std::mutex> lock(state.mutex);
        stops_after_close = state.stop_count;
    }
    const bool close_stops = stops_after_close == stops_before_close + 1;
    std::printf("close_without_stop=%s\n", close_stops ? "stopped" : "FAILED");
    if (reverse_fd >= 0) close(reverse_fd);
    raop_stop_httpd(raop);
    raop_destroy(raop);
    return passed && close_stops ? 0 : 1;
}
