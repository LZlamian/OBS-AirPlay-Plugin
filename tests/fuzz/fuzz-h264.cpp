// SPDX-License-Identifier: GPL-3.0-or-later
// Mirror stream access units (Annex B) into the software decoder, which
// parses and remembers parameter sets itself (src/h264-decoder.cpp). The
// first byte picks H.264 or HEVC.
#include "h264-decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

extern "C" void blog(int, const char*, ...) {}
extern "C" uint64_t os_gettime_ns(void) { return 0; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1) return 0;
    const AVCodecID codec = (data[0] & 1) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    static H264Decoder h264(AV_CODEC_ID_H264, H264Decoder::Mode::SoftwareOnly);
    static H264Decoder hevc(AV_CODEC_ID_HEVC, H264Decoder::Mode::SoftwareOnly);
    H264Decoder& decoder = codec == AV_CODEC_ID_HEVC ? hevc : h264;
    DecodedVideoFrame frame;
    // Whole input as one access unit, then split in two (the decoder keeps
    // state between packets).
    decoder.decode(data + 1, size - 1, frame);
    const size_t half = (size - 1) / 2;
    decoder.decode(data + 1, half, frame);
    decoder.decode(data + 1 + half, size - 1 - half, frame);
    decoder.releaseFrame();
    return 0;
}
