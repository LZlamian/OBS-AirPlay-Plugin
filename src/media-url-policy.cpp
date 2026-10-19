// SPDX-License-Identifier: GPL-3.0-or-later
#include "media-url-policy.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <aclapi.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>

namespace media_url_policy {
namespace {

std::mutex g_ports_mutex;
std::vector<unsigned short> g_ports{80, 443, 8080, 8443};
std::atomic<bool> g_loopback_for_testing{false};
std::atomic<bool> g_local_files_for_testing{false};
std::atomic<bool> g_sender_only{false};

constexpr const char* kNotSender = "host is not the sending device (only the sending device is allowed)";

#if defined(_WIN32)
// Windows has no inet_aton. Its own lenient IPv4 parser (what its resolver
// uses for numeric hosts) takes the same short, octal and hex forms.
int inet_aton(const char* text, in_addr* address)
{
#if defined(__MINGW32__)
    LPSTR end = nullptr;
#else
    PCSTR end = nullptr;
#endif
    return RtlIpv4StringToAddressA(text, FALSE, &end, address) == 0 && end && *end == '\0'
        ? 1 : 0;
}
#endif

struct UrlParts {
    std::string host; // lower case, no brackets or IPv6 zone
    int port = 0;
};

bool split(const std::string& url, UrlParts& parts)
{
    size_t authority_start;
    int default_port;
    if (url.size() > 7 && strncasecmp(url.c_str(), "http://", 7) == 0) {
        authority_start = 7;
        default_port = 80;
    } else if (url.size() > 8 && strncasecmp(url.c_str(), "https://", 8) == 0) {
        authority_start = 8;
        default_port = 443;
    } else {
        return false;
    }
    size_t end = url.find_first_of("/?#", authority_start);
    if (end == std::string::npos) end = url.size();
    std::string authority = url.substr(authority_start, end - authority_start);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    if (authority.empty()) return false;

    std::string host;
    std::string port;
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(1, close - 1);
        const size_t zone = host.find('%');
        if (zone != std::string::npos) host.erase(zone);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port = authority.substr(close + 2);
        }
    } else {
        const size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    while (host.size() > 1 && host.back() == '.') host.pop_back();
    if (host.empty()) return false;
    for (char& c : host) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return false;
        c = static_cast<char>(std::tolower(u));
    }
    parts.host = host;
    parts.port = default_port;
    if (!port.empty()) {
        long value = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
            if (value > 65535) return false;
        }
        if (value == 0) return false;
        parts.port = static_cast<int>(value);
    }
    return true;
}

// Empty for an address any item may use, otherwise what it is.
std::string classifyIpv4(uint32_t address)
{
    const uint32_t first = address >> 24;
    if (first == 0) return "unspecified address";
    if (first == 127) return g_loopback_for_testing ? "" : "loopback address";
    if ((address & 0xffff0000U) == 0xa9fe0000U) return "link-local address";
    if (first >= 224 && first <= 239) return "multicast address";
    if (first >= 240) return "reserved address";
    return "";
}

std::string classifyIpv6(const unsigned char* a)
{
    static const unsigned char zero[16] = {};
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    static const unsigned char nat64[12] = {0x00, 0x64, 0xff, 0x9b};
    if (std::memcmp(a, zero, 16) == 0) return "unspecified address";
    if (std::memcmp(a, zero, 15) == 0 && a[15] == 1) {
        return g_loopback_for_testing ? "" : "loopback address";
    }
    if (std::memcmp(a, mapped, 12) == 0 || std::memcmp(a, nat64, 12) == 0 ||
        std::memcmp(a, zero, 12) == 0) {
        return classifyIpv4((static_cast<uint32_t>(a[12]) << 24) |
                            (static_cast<uint32_t>(a[13]) << 16) |
                            (static_cast<uint32_t>(a[14]) << 8) | a[15]);
    }
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return "link-local address";
    if (a[0] == 0xff) return "multicast address";
    return "";
}

std::string classify(const sockaddr* address)
{
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        return classifyIpv4(ntohl(v4->sin_addr.s_addr));
    }
    if (address->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        return classifyIpv6(v6->sin6_addr.s6_addr);
    }
    return "unsupported address family";
}

bool isLocalhostName(const std::string& host)
{
    return host == "localhost" ||
        (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0);
}

// Literal addresses are classified directly (inet_aton also takes the short
// and numeric forms resolvers accept); names are resolved and every address
// must pass.
std::string checkHost(const std::string& host)
{
    in6_addr v6 = {};
    in_addr v4 = {};
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) return classifyIpv6(v6.s6_addr);
    if (inet_aton(host.c_str(), &v4) == 1) return classifyIpv4(ntohl(v4.s_addr));
    if (isLocalhostName(host) && !g_loopback_for_testing) return "loopback host";

#if defined(_WIN32)
    // getaddrinfo fails before Winsock is started (the receiver starts it,
    // a test may not); a failed lookup would let the URL through.
    static const bool winsock_started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)winsock_started;
#endif
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int error = getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if (error != 0 || !results) {
        // Nothing to fetch from; FFmpeg reports the failure itself.
        if (results) freeaddrinfo(results);
        return "";
    }
    std::string refused;
    for (const addrinfo* entry = results; entry && refused.empty(); entry = entry->ai_next) {
        if (entry->ai_addr) refused = classify(entry->ai_addr);
    }
    freeaddrinfo(results);
    return refused.empty() ? "" : "host resolves to a " + refused;
}

// A literal address as 16 bytes (IPv4 as IPv4-mapped IPv6).
bool literalAddress(const std::string& host, unsigned char out[16])
{
    in6_addr v6 = {};
    in_addr v4 = {};
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        std::memcpy(out, v6.s6_addr, 16);
        return true;
    }
    if (inet_aton(host.c_str(), &v4) == 1) {
        static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        std::memcpy(out, mapped, 12);
        std::memcpy(out + 12, &v4.s_addr, 4);
        return true;
    }
    return false;
}

bool portAllowed(int port)
{
    std::lock_guard<std::mutex> lock(g_ports_mutex);
    return std::find(g_ports.begin(), g_ports.end(), port) != g_ports.end();
}

} // namespace

void setAllowedPorts(std::vector<unsigned short> ports)
{
    std::lock_guard<std::mutex> lock(g_ports_mutex);
    g_ports = std::move(ports);
}

std::vector<unsigned short> allowedPorts()
{
    std::lock_guard<std::mutex> lock(g_ports_mutex);
    return g_ports;
}

std::vector<unsigned short> parsePortList(const std::string& text)
{
    std::vector<unsigned short> ports;
    long value = -1;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if (c >= '0' && c <= '9') {
            value = (value < 0 ? 0 : value) * 10 + (c - '0');
            if (value > 65535) value = 65536;
        } else if (c == ',' || c == ' ' || c == '\t') {
            if (value > 0 && value <= 65535 &&
                std::find(ports.begin(), ports.end(), value) == ports.end()) {
                ports.push_back(static_cast<unsigned short>(value));
            }
            value = -1;
        } else {
            return {};
        }
    }
    return ports;
}

void setSenderOnly(bool sender_only)
{
    g_sender_only.store(sender_only);
}

bool senderOnly()
{
    return g_sender_only.load();
}

void setLoopbackAllowedForTesting(bool allowed)
{
    g_loopback_for_testing.store(allowed);
}

void setLocalFilesAllowedForTesting(bool allowed)
{
    g_local_files_for_testing.store(allowed);
}

std::string checkItem(const std::string& url)
{
    UrlParts parts;
    if (!split(url, parts)) return "malformed URL";
    in6_addr v6 = {};
    in_addr v4 = {};
    if (inet_pton(AF_INET6, parts.host.c_str(), &v6) == 1 ||
        inet_aton(parts.host.c_str(), &v4) == 1) {
        return "";
    }
    if (parts.host == "localhost") return "";
    if (g_sender_only) return kNotSender;
    return checkHost(parts.host);
}

std::string checkNested(const std::string& url, const std::string& item_url)
{
    UrlParts parts;
    if (!split(url, parts)) return "not an HTTP(S) URL";
    UrlParts item;
    if (split(item_url, item) && item.host == parts.host && item.port == parts.port) {
        return "";
    }
    if (g_sender_only) {
        // The item's address is the sender's (the receiver checked it); its
        // other ports are the sender's too.
        unsigned char wanted[16];
        unsigned char got[16];
        const bool same_address = !item.host.empty() && item.host != "localhost" &&
            literalAddress(item.host, wanted) && literalAddress(parts.host, got) &&
            std::memcmp(wanted, got, 16) == 0;
        return same_address ? "" : kNotSender;
    }
    if (!portAllowed(parts.port) && !g_loopback_for_testing) {
        return "port " + std::to_string(parts.port) + " not allowed";
    }
    return checkHost(parts.host);
}

#if defined(_WIN32)
namespace {

std::wstring widen(const std::string& text)
{
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

// The file's owner is this process's user (or the owner it gives new files,
// e.g. Administrators when elevated).
bool ownedByThisUser(const std::wstring& path)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                              nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    bool owned = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        BYTE default_owner[sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE];
        DWORD size = 0;
        owned = (GetTokenInformation(token, TokenUser, user, sizeof(user), &size) &&
                 EqualSid(owner, reinterpret_cast<TOKEN_USER*>(user)->User.Sid)) ||
                (GetTokenInformation(token, TokenOwner, default_owner, sizeof(default_owner),
                                     &size) &&
                 EqualSid(owner, reinterpret_cast<TOKEN_OWNER*>(default_owner)->Owner));
        CloseHandle(token);
    }
    LocalFree(descriptor);
    return owned;
}

} // namespace

// UxPlay spools into the user's temporary folder (uxplay/lib/spool_win32.c).
std::string checkLocalFile(const std::string& location)
{
    if (g_local_files_for_testing) return "";
    const std::wstring path = widen(location);
    if (path.empty()) return "not a valid path";
    wchar_t directory[MAX_PATH + 1];
    const DWORD directory_length = GetTempPathW(MAX_PATH + 1, directory);
    if (!directory_length || directory_length > MAX_PATH) return "no temporary folder";
    if (path.size() <= directory_length ||
        CompareStringOrdinal(path.c_str(), static_cast<int>(directory_length), directory,
                             static_cast<int>(directory_length), TRUE) != CSTR_EQUAL) {
        return "not in the receiver's spool folder";
    }
    const std::wstring name = path.substr(directory_length);
    if (name.find_first_of(L"\\/:") != std::wstring::npos) return "path is not canonical";
    if (name.rfind(L"obs-airplay-fcup-", 0) != 0) return "not a receiver spool file";
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !ownedByThisUser(path)) {
        return "spool file is not a regular file owned by this user";
    }
    return "";
}
#else
std::string checkLocalFile(const std::string& location)
{
    if (g_local_files_for_testing) return "";
    if (location.empty() || location[0] != '/') return "not an absolute path";
    if (location.find("/../") != std::string::npos || location.find("/./") != std::string::npos) {
        return "path is not canonical";
    }
    const std::string name = location.substr(location.rfind('/') + 1);
    if (name.rfind("obs-airplay-fcup-", 0) != 0) return "not a receiver spool file";
    struct stat info = {};
    if (lstat(location.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid()) {
        return "spool file is not a regular file owned by this user";
    }
    return "";
}
#endif

} // namespace media_url_policy
