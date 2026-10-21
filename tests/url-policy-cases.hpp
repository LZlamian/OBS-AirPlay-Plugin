// SPDX-License-Identifier: GPL-3.0-or-later
// URLs both URL policies decide the same way: the receiver's POST /play
// check (uxplay/lib/media_url_policy.h, sender at 127.0.0.1) and the
// player's checkNested for a host other than the item's
// (src/media-url-policy.cpp, loopback not allowed). Literal addresses and
// textual cases only, so neither looks anything up. Used by protocol-smoke
// and media-player-smoke (--url-policy).
#pragma once

struct SharedPolicyCase {
    const char* url;
    bool allowed;
};

inline const SharedPolicyCase kSharedPolicyCases[] = {
    {"http://127.0.0.1/x.mp4", false},
    {"http://127.0.0.1:631/x.mp4", false},
    {"http://localhost/x.mp4", false},
    {"http://foo.localhost/x.mp4", false},
    {"http://169.254.169.254/latest/meta-data", false},
    {"http://[::1]/x.mp4", false},
    {"http://[::ffff:127.0.0.1]/x.mp4", false},
    {"http://[fe80::1%25en0]/x.mp4", false},
    {"http://2130706433/x.mp4", false},
    {"http://0x7f.1/x.mp4", false},
    {"http://224.0.0.251/x.mp4", false},
    {"http://0.0.0.0/x.mp4", false},
    {"http://10.1.2.3:22/x.mp4", false},
    {"file:///etc/passwd", false},
    {"http://10.1.2.3/x.mp4", true},
    {"http://user@10.0.0.1:8443/x.mp4", true},
};
