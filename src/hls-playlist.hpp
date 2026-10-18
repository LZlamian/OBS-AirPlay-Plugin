// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The parts of an HLS multivariant playlist the media player reads itself,
// before FFmpeg opens the item: the renditions, which one to play, and how
// its URI resolves against the playlist's.
namespace hls {

// One rendition is used for a whole HLS item (no adaptation), so prefer a
// quick start over the largest one: the best rendition at or under this
// bandwidth, otherwise the smallest one over it.
constexpr int64_t kTargetBitrate = 1500000;

struct Variant {
    std::string uri;
    int64_t bandwidth = 0;  // 0 when the playlist gives none
    bool supported_video = false;  // H.264 or HEVC, or no CODECS attribute
};

// The #EXT-X-STREAM-INF entries, in playlist order. Empty for a media
// playlist (one without renditions).
std::vector<Variant> parseVariants(const std::string& playlist);
// The rendition to play (see kTargetBitrate), or nullptr when none has
// supported video. Points into `variants`.
const Variant* chooseVariant(const std::vector<Variant>& variants);
// `reference` resolved against the playlist URL `base` (absolute, scheme-
// relative, root-relative or relative; "." and ".." segments removed).
// Empty when `base` is not an absolute URL.
std::string resolveUrl(const std::string& base, const std::string& reference);

} // namespace hls
