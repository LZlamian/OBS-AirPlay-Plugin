// SPDX-License-Identifier: GPL-3.0-or-later
// HLS multivariant playlists from the network (src/hls-playlist.cpp).
#include "hls-playlist.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string playlist(reinterpret_cast<const char*>(data), size);
    const auto variants = hls::parseVariants(playlist);
    if (const hls::Variant* chosen = hls::chooseVariant(variants)) {
        hls::resolveUrl("http://host:8080/a/b/master.m3u8?t=1", chosen->uri);
    }
    // The input as both the reference and the base.
    hls::resolveUrl("https://host/x/y.m3u8", playlist);
    hls::resolveUrl(playlist, "../z.m3u8");
    return 0;
}
