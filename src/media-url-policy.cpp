// SPDX-License-Identifier: GPL-3.0-or-later
#include "media-url-policy.hpp"

#include "../uxplay/lib/media_url.h"  // the receiver's own parser and address classes

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace media_url_policy {
namespace {

std::mutex g_ports_mutex;
std::vector<unsigned short> g_ports{80, 443, 8080, 8443};
std::atomic<bool> g_loopback_for_testing{false};
std::atomic<bool> g_local_files_for_testing{false};
std::atomic<bool> g_sender_only{false};
std::mutex g_resolver_mutex; // leaf
Resolver g_resolver_for_testing;

constexpr const char* kNotSender = "host is not the sending device (only the sending device is allowed)";

struct UrlParts {
    std::string host; // lower case, no brackets or IPv6 zone
    int port = 0;
};

bool split(const std::string& url, UrlParts& parts)
{
    char host[MEDIA_URL_HOST_BYTES];
    long port = 0;
    if (!media_url_split(url.c_str(), host, sizeof(host), &port)) return false;
    parts.host = host;
    parts.port = static_cast<int>(port);
    return true;
}

// Empty for an address any item may use, otherwise what it is.
std::string describe(media_address_class_t address_class)
{
    switch (address_class) {
    case MEDIA_ADDRESS_ROUTABLE: return "";
    case MEDIA_ADDRESS_LOOPBACK: return g_loopback_for_testing ? "" : "loopback address";
    case MEDIA_ADDRESS_LINK_LOCAL: return "link-local address";
    case MEDIA_ADDRESS_MULTICAST: return "multicast address";
    case MEDIA_ADDRESS_UNSPECIFIED: return "unspecified address";
    case MEDIA_ADDRESS_RESERVED: return "reserved address";
    }
    return "reserved address";
}

std::string classifyIpv4(uint32_t address)
{
    return describe(media_classify_ipv4(address));
}

std::string classifyIpv6(const unsigned char* address)
{
    return describe(media_classify_ipv6(address));
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

std::string numericAddress(const sockaddr_storage& address)
{
    char text[INET6_ADDRSTRLEN] = {};
    if (address.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in&>(address).sin_addr, text,
                  sizeof(text));
    } else if (address.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6&>(address).sin6_addr, text,
                  sizeof(text));
    }
    return text;
}

// Literal addresses are classified directly (inet_aton also takes the short
// and numeric forms resolvers accept); names are resolved and every address
// must pass. For a name, `address` gets the first one, which is where the
// fetch must connect (another lookup may answer differently).
Decision checkHost(const std::string& host, const Resolver& resolve)
{
    int family = 0;
    unsigned char literal[16] = {};
    if (media_parse_literal(host.c_str(), &family, literal)) {
        if (family == AF_INET6) return {classifyIpv6(literal), ""};
        uint32_t v4 = 0;
        std::memcpy(&v4, literal, sizeof(v4));
        return {classifyIpv4(ntohl(v4)), ""};
    }
    if (isLocalhostName(host) && !g_loopback_for_testing) return {"loopback host", ""};

    std::vector<sockaddr_storage> addresses;
    if (!resolve(host, addresses) || addresses.empty()) {
        // Refused, not left to FFmpeg: its own lookup may get a different
        // answer (a resolver that times out here, a name that rebinds).
        return {"host name could not be resolved", ""};
    }
    std::string refused;
    for (const sockaddr_storage& address : addresses) {
        refused = classify(reinterpret_cast<const sockaddr*>(&address));
        if (!refused.empty()) return {"host resolves to a " + refused, ""};
    }
    const std::string first = numericAddress(addresses.front());
    if (first.empty()) return {"host resolves to an unsupported address", ""};
    return {"", first};
}

bool blockingResolve(const std::string& host, std::vector<sockaddr_storage>& addresses)
{
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || !results) {
        if (results) freeaddrinfo(results);
        return false;
    }
    for (const addrinfo* entry = results; entry; entry = entry->ai_next) {
        if (!entry->ai_addr || entry->ai_addrlen > sizeof(sockaddr_storage)) continue;
        sockaddr_storage address = {};
        std::memcpy(&address, entry->ai_addr, entry->ai_addrlen);
        addresses.push_back(address);
    }
    freeaddrinfo(results);
    return !addresses.empty();
}

// A literal address as 16 bytes (IPv4 as IPv4-mapped IPv6).
bool literalAddress(const std::string& host, unsigned char out[16])
{
    int family = 0;
    unsigned char literal[16] = {};
    if (!media_parse_literal(host.c_str(), &family, literal)) return false;
    if (family == AF_INET6) {
        std::memcpy(out, literal, 16);
    } else {
        static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        std::memcpy(out, mapped, 12);
        std::memcpy(out + 12, literal, 4);
    }
    return true;
}

bool isLiteral(const std::string& host)
{
    int family = 0;
    unsigned char literal[16];
    return media_parse_literal(host.c_str(), &family, literal);
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

Decision decideItem(const std::string& url, const Resolver& resolve)
{
    UrlParts parts;
    if (!split(url, parts)) return {"malformed URL", ""};
    if (isLiteral(parts.host)) return {};
    if (parts.host == "localhost") return {};
    if (g_sender_only) return {kNotSender, ""};
    return checkHost(parts.host, resolve);
}

Decision decideNested(const std::string& url, const std::string& item_url,
                      const Resolver& resolve)
{
    UrlParts parts;
    if (!split(url, parts)) return {"not an HTTP(S) URL", ""};
    UrlParts item;
    if (split(item_url, item) && item.host == parts.host && item.port == parts.port) {
        // The item's own host was checked with the item (a literal one by
        // the receiver, with the sender's address in mind). A name is looked
        // up again for its address, which the caller's resolver keeps stable.
        if (item.host == "localhost" || isLiteral(parts.host)) return {};
        return checkHost(parts.host, resolve);
    }
    if (g_sender_only) {
        // The item's address is the sender's (the receiver checked it); its
        // other ports are the sender's too.
        unsigned char wanted[16];
        unsigned char got[16];
        const bool same_address = !item.host.empty() && item.host != "localhost" &&
            literalAddress(item.host, wanted) && literalAddress(parts.host, got) &&
            std::memcmp(wanted, got, 16) == 0;
        return {same_address ? "" : kNotSender, ""};
    }
    if (!portAllowed(parts.port) && !g_loopback_for_testing) {
        return {"port " + std::to_string(parts.port) + " not allowed", ""};
    }
    return checkHost(parts.host, resolve);
}

namespace {
// getaddrinfo, or the test hook.
Resolver currentResolver()
{
    std::lock_guard<std::mutex> lock(g_resolver_mutex);
    return g_resolver_for_testing ? g_resolver_for_testing : Resolver(blockingResolve);
}
} // namespace

std::string checkItem(const std::string& url)
{
    return decideItem(url, currentResolver()).refused;
}

std::string checkNested(const std::string& url, const std::string& item_url)
{
    return decideNested(url, item_url, currentResolver()).refused;
}

void setResolverForTesting(Resolver resolver)
{
    std::lock_guard<std::mutex> lock(g_resolver_mutex);
    g_resolver_for_testing = std::move(resolver);
}

bool resolveInterruptible(const std::string& host, std::vector<sockaddr_storage>& addresses,
                          const std::function<bool()>& interrupted, int timeout_ms)
{
    const Resolver resolver = currentResolver();
    // getaddrinfo cannot be cancelled: it runs on a thread of its own, which
    // is left to finish when the caller stops waiting. (The plugin bundle
    // holds Objective-C code, so dyld never unloads it under that thread.)
    struct Lookup {
        std::mutex mutex;
        std::condition_variable done_cv;
        bool done = false;
        bool resolved = false;
        std::vector<sockaddr_storage> addresses;
    };
    auto lookup = std::make_shared<Lookup>();
    try {
        std::thread([lookup, host, resolver] {
            std::vector<sockaddr_storage> found;
            const bool resolved = resolver(host, found);
            std::lock_guard<std::mutex> lock(lookup->mutex);
            lookup->resolved = resolved;
            lookup->addresses = std::move(found);
            lookup->done = true;
            lookup->done_cv.notify_all();
        }).detach();
    } catch (const std::system_error&) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(lookup->mutex);
    while (!lookup->done) {
        if ((interrupted && interrupted()) || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        lookup->done_cv.wait_for(lock, std::chrono::milliseconds(10));
    }
    addresses = std::move(lookup->addresses);
    return lookup->resolved;
}

std::string checkLocalFile(const std::string& location)
{
    if (g_local_files_for_testing) return "";
    if (location.empty() || location[0] != '/') return "not an absolute path";
    if (location.find("/../") != std::string::npos || location.find("/./") != std::string::npos) {
        return "path is not canonical";
    }
    const std::string name = location.substr(location.rfind('/') + 1);
    if (name.rfind(MEDIA_SPOOL_FILE_PREFIX, 0) != 0) return "not a receiver spool file";
    struct stat info = {};
    if (lstat(location.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid()) {
        return "spool file is not a regular file owned by this user";
    }
    return "";
}

} // namespace media_url_policy
