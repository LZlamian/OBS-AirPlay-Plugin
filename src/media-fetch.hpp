// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <sys/socket.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct AVDictionary;

// Fetches for one AirPlay video item, made so that what FFmpeg connects to
// is what the URL policy (media-url-policy.hpp) checked:
//
// - Each host name is looked up once per item, on a helper thread that
//   stop() does not wait for. A plain-HTTP fetch connects to the checked
//   address, with the name in the Host header.
// - HTTPS keeps the name. FFmpeg 8 skips certificate host verification and
//   SNI when the URL's host is numeric, and a verified certificate already
//   ties the connection to the name, so a name that rebinds to another
//   address gets no answer. With self-signed servers allowed (no
//   verification), HTTPS is pinned like HTTP.
// - FFmpeg does not follow redirects (max_redirects 0). When a request fails
//   the way a redirect does, the URL is requested once more by hand to read
//   the Location, and each hop is checked before it is fetched.
namespace media_fetch {

// Opens a URL with options: the caller's avio_open2 or a demuxer's io_open.
using OpenFunction = std::function<int(const std::string& url, AVDictionary** options)>;

// TLS options for every HTTPS fetch: verify the server unless self-signed
// servers are allowed.
void setTlsOptions(AVDictionary** options, bool allow_insecure);

// A URL for the log: no userinfo, no query or fragment (tokens).
std::string safeForLog(std::string url);

class Session {
public:
    // `interrupted` is polled during lookups and network waits;
    // `refused` is told every policy refusal (already logged).
    Session(std::string item_url, bool allow_insecure_tls, std::function<bool()> interrupted,
            std::function<void(const std::string&)> refused);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const std::string& itemUrl() const { return m_item_url; }

    // The policy decision for `url` (item: the item's own URL), empty when
    // allowed. Looks names up as open() would.
    std::string check(const std::string& url, bool item);

    // Opens `url`: checks it, connects by the checked address and follows
    // redirects (each hop checked). Returns AVERROR(EPERM) when the policy
    // refused a URL (logged and reported). `final_url` gets the URL that
    // answered, in its name form. `options` keeps what the open left over.
    int open(const std::string& url, bool item, AVDictionary** options, const OpenFunction& opener,
             std::string* final_url = nullptr);

    // A URL FFmpeg built from a pinned one (a playlist's relative URI, the
    // opened location) back in its name form; others are returned as given.
    std::string unpin(const std::string& url) const;

    static constexpr int kLookupTimeoutMs = 10000;
    static constexpr int kMaxRedirects = 8;

private:
    struct Target {
        std::string url;          // what FFmpeg opens
        std::string headers;      // "Host: ...\r\n" when the host was replaced
        std::string connect_host; // for the redirect probe: address or name
        std::string host_header;  // host[:port] as written in the URL
        std::string path;         // path and query, at least "/"
        int port = 0;
        bool tls = false;
    };

    std::string prepare(const std::string& url, bool item, Target& target);
    std::string probeRedirect(const Target& target);
    bool lookUp(const std::string& host, std::vector<sockaddr_storage>& addresses);
    static int interruptCallback(void* opaque);

    std::string m_item_url;
    bool m_allow_insecure_tls;
    std::function<bool()> m_interrupted;
    std::function<void(const std::string&)> m_refused;
    mutable std::mutex m_mutex; // leaf
    std::map<std::string, std::vector<sockaddr_storage>> m_lookups;
    // "http://192.0.2.1:80" -> "example.com" (the authority as written)
    std::map<std::string, std::string> m_pinned;
};

} // namespace media_fetch
