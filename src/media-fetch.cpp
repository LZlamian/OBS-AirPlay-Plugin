// SPDX-License-Identifier: GPL-3.0-or-later
#include "media-fetch.hpp"
#include "hls-playlist.hpp"
#include "media-url-policy.hpp"

#include <obs-module.h>

#include <strings.h>
#if defined(_WIN32)
#include <io.h>
#define access _access
#define R_OK 4
#else
#include <unistd.h>
#endif

#include <cctype>
#include <cstring>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace media_fetch {
namespace {

// scheme://[userinfo@]host[:port]rest, with the host as written (IPv6 in
// brackets) and the port as written (may be empty).
struct UrlView {
    std::string scheme; // lower case
    std::string userinfo;
    std::string host;
    std::string port;
    std::string rest;
};

bool parse(const std::string& url, UrlView& view)
{
    const size_t separator = url.find("://");
    if (separator == std::string::npos) return false;
    view.scheme = url.substr(0, separator);
    for (char& c : view.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (view.scheme != "http" && view.scheme != "https") return false;
    const size_t start = separator + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    std::string authority = url.substr(start, end - start);
    view.rest = url.substr(end);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        view.userinfo = authority.substr(0, at);
        authority.erase(0, at + 1);
    }
    if (authority.empty()) return false;
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        view.host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            view.port = authority.substr(close + 2);
        }
    } else {
        const size_t colon = authority.find(':');
        view.host = authority.substr(0, colon);
        if (colon != std::string::npos) view.port = authority.substr(colon + 1);
    }
    return !view.host.empty();
}

int portNumber(const UrlView& view)
{
    if (view.port.empty()) return view.scheme == "https" ? 443 : 80;
    return std::atoi(view.port.c_str());
}

// The host for a URL or "host:port" string: IPv6 in brackets.
std::string bracketed(const std::string& address)
{
    return address.find(':') != std::string::npos ? "[" + address + "]" : address;
}

std::string pinKey(const std::string& scheme, const std::string& host, int port)
{
    return scheme + "://" + host + ":" + std::to_string(port);
}

bool startsWithCaseInsensitive(const std::string& value, const char* prefix)
{
    const size_t length = std::strlen(prefix);
    return value.size() >= length && strncasecmp(value.c_str(), prefix, length) == 0;
}

} // namespace

void setTlsOptions(AVDictionary** options, bool allow_insecure)
{
    if (allow_insecure) {
        av_dict_set(options, "tls_verify", "0", 0);
        return;
    }
    av_dict_set(options, "tls_verify", "1", 0);
    // The release build's OpenSSL has no default trust store of its own.
    static const bool system_bundle = access("/etc/ssl/cert.pem", R_OK) == 0;
    if (system_bundle) av_dict_set(options, "ca_file", "/etc/ssl/cert.pem", 0);
}

std::string safeForLog(std::string url)
{
    // Media URLs frequently carry short-lived authentication tokens. Keep the
    // useful scheme/host/path diagnostic without writing those credentials to
    // the OBS log.
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        const size_t authority_start = scheme + 3;
        const size_t authority_end = url.find('/', authority_start);
        const size_t at = url.find('@', authority_start);
        if (at != std::string::npos && (authority_end == std::string::npos || at < authority_end)) {
            url.replace(authority_start, at - authority_start + 1, "[redacted]@");
        }
    }
    const size_t sensitive = url.find_first_of("?#");
    if (sensitive != std::string::npos) {
        url.erase(sensitive);
        url += "?[redacted]";
    }
    return url;
}

Session::Session(std::string item_url, bool allow_insecure_tls,
                 std::function<bool()> interrupted,
                 std::function<void(const std::string&)> refused)
    : m_item_url(std::move(item_url)),
      m_allow_insecure_tls(allow_insecure_tls),
      m_interrupted(std::move(interrupted)),
      m_refused(std::move(refused))
{
}

int Session::interruptCallback(void* opaque)
{
    const auto* self = static_cast<const Session*>(opaque);
    return self->m_interrupted && self->m_interrupted() ? 1 : 0;
}

bool Session::lookUp(const std::string& host, std::vector<sockaddr_storage>& addresses)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto cached = m_lookups.find(host);
        if (cached != m_lookups.end()) {
            addresses = cached->second;
            return true;
        }
    }
    // One answer per item: every later fetch from this host connects to
    // the address checked here.
    if (!media_url_policy::resolveInterruptible(host, addresses, m_interrupted, kLookupTimeoutMs)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lookups.emplace(host, addresses);
    return true;
}

std::string Session::unpin(const std::string& url) const
{
    UrlView view;
    if (!parse(url, view)) return url;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto pinned = m_pinned.find(pinKey(view.scheme, view.host, portNumber(view)));
    if (pinned == m_pinned.end()) return url;
    return view.scheme + "://" + (view.userinfo.empty() ? "" : view.userinfo + "@") +
        pinned->second + view.rest;
}

std::string Session::check(const std::string& url, bool item)
{
    const media_url_policy::Resolver resolve =
        [this](const std::string& host, std::vector<sockaddr_storage>& addresses) {
            return lookUp(host, addresses);
        };
    const std::string name_url = unpin(url);
    return item ? media_url_policy::decideItem(name_url, resolve).refused
                : media_url_policy::decideNested(name_url, m_item_url, resolve).refused;
}

std::string Session::prepare(const std::string& url, bool item, Target& target)
{
    const media_url_policy::Resolver resolve =
        [this](const std::string& host, std::vector<sockaddr_storage>& addresses) {
            return lookUp(host, addresses);
        };
    const media_url_policy::Decision decision =
        item ? media_url_policy::decideItem(url, resolve)
             : media_url_policy::decideNested(url, m_item_url, resolve);
    if (!decision.refused.empty()) return decision.refused;

    UrlView view;
    if (!parse(url, view)) return "malformed URL";
    target = Target{};
    target.tls = view.scheme == "https";
    target.port = portNumber(view);
    target.host_header = view.host + (view.port.empty() ? "" : ":" + view.port);
    target.path = view.rest.empty() || view.rest[0] != '/' ? "/" + view.rest : view.rest;
    const size_t fragment = target.path.find('#');
    if (fragment != std::string::npos) target.path.erase(fragment);
    target.url = url;
    target.connect_host = view.host;

    const bool pin = !decision.address.empty() && (!target.tls || m_allow_insecure_tls);
    if (pin) {
        const std::string host = bracketed(decision.address);
        target.connect_host = host;
        target.url = view.scheme + "://" + (view.userinfo.empty() ? "" : view.userinfo + "@") +
            host + ":" + std::to_string(target.port) + view.rest;
        target.headers = "Host: " + target.host_header + "\r\n";
        std::lock_guard<std::mutex> lock(m_mutex);
        // The first name keeps an address shared by several names.
        m_pinned.emplace(pinKey(view.scheme, host, target.port), target.host_header);
    }
    return "";
}

int Session::open(const std::string& url, bool item, AVDictionary** options,
                  const OpenFunction& opener, std::string* final_url)
{
    AVDictionary* base = nullptr;
    if (options && *options) av_dict_copy(&base, *options, 0);
    std::string current = unpin(url);
    int result = AVERROR(EIO);
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        Target target;
        const std::string refused = prepare(current, item && hop == 0, target);
        if (!refused.empty()) {
            blog(LOG_WARNING, "[MEDIA] refused to fetch %s: %s", safeForLog(current).c_str(),
                 refused.c_str());
            if (m_refused) m_refused(refused);
            av_dict_free(&base);
            return AVERROR(EPERM);
        }
        AVDictionary* attempt = nullptr;
        av_dict_copy(&attempt, base, 0);
        av_dict_set(&attempt, "max_redirects", "0", 0);
        // Replaces a Host header the demuxer copied from another request.
        av_dict_set(&attempt, "headers", target.headers.empty() ? nullptr : target.headers.c_str(),
                    0);
        result = opener(target.url, &attempt);
        const bool last = result >= 0 || result != AVERROR(EIO) || interruptCallback(this) ||
            hop == kMaxRedirects;
        std::string location;
        // FFmpeg reports a redirect it may not follow as EIO.
        if (!last) location = probeRedirect(target);
        if (last || location.empty()) {
            if (options) {
                av_dict_free(options);
                *options = attempt;
            } else {
                av_dict_free(&attempt);
            }
            av_dict_free(&base);
            if (result >= 0 && final_url) *final_url = current;
            return result;
        }
        av_dict_free(&attempt);
        const std::string next = hls::resolveUrl(current, location);
        if (next.empty()) break;
        blog(LOG_INFO, "[MEDIA] following redirect to %s", safeForLog(next).c_str());
        current = next;
    }
    av_dict_free(&base);
    return result;
}

std::string Session::probeRedirect(const Target& target)
{
    const std::string endpoint = std::string(target.tls ? "tls://" : "tcp://") +
        target.connect_host + ":" + std::to_string(target.port);
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "tcp,tls", 0);
    av_dict_set(&options, "rw_timeout", "7000000", 0);
    if (target.tls) setTlsOptions(&options, m_allow_insecure_tls);
    AVIOInterruptCB interrupt = {&Session::interruptCallback, this};
    AVIOContext* io = nullptr;
    const int opened = avio_open2(&io, endpoint.c_str(), AVIO_FLAG_READ_WRITE, &interrupt, &options);
    av_dict_free(&options);
    if (opened < 0 || !io) return "";

    const std::string request = "GET " + target.path + " HTTP/1.1\r\nHost: " +
        target.host_header +
        "\r\nUser-Agent: AppleCoreMedia/1.0 OBS-AirPlay/" PLUGIN_VERSION
        "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    avio_write(io, reinterpret_cast<const unsigned char*>(request.data()),
               static_cast<int>(request.size()));
    avio_flush(io);
    constexpr size_t kMaximumHeaderBytes = 16384;
    std::string response;
    unsigned char buffer[4096];
    while (response.size() < kMaximumHeaderBytes && response.find("\r\n\r\n") == std::string::npos) {
        const int got = avio_read_partial(io, buffer, sizeof(buffer));
        if (got <= 0) break;
        response.append(reinterpret_cast<const char*>(buffer), static_cast<size_t>(got));
    }
    avio_closep(&io);

    const size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || !startsWithCaseInsensitive(response, "HTTP/1.")) {
        return "";
    }
    const size_t space = response.find(' ');
    const int status = space == std::string::npos ? 0 : std::atoi(response.c_str() + space + 1);
    if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
        return "";
    }
    size_t line = response.find("\r\n");
    while (line != std::string::npos && line < header_end) {
        const size_t start = line + 2;
        const size_t next = response.find("\r\n", start);
        const std::string field = response.substr(start, next - start);
        if (startsWithCaseInsensitive(field, "location:")) {
            size_t value = std::strlen("location:");
            while (value < field.size() && (field[value] == ' ' || field[value] == '\t')) ++value;
            std::string location = field.substr(value);
            while (!location.empty() && (location.back() == ' ' || location.back() == '\t')) {
                location.pop_back();
            }
            return location;
        }
        line = next;
    }
    return "";
}

} // namespace media_fetch
