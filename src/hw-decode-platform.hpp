// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

// The platform's hardware video decoder. macOS: VideoToolbox, whose NV12
// pixel buffers are read in place. Windows: D3D11VA, whose surfaces are
// copied to memory with av_hwframe_transfer_data (the path other layouts
// take on macOS). Elsewhere: none, so device creation fails and decoding
// stays in software.
#if defined(__APPLE__)
inline constexpr AVHWDeviceType kHardwareDeviceType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
inline constexpr AVPixelFormat kHardwarePixelFormat = AV_PIX_FMT_VIDEOTOOLBOX;
inline constexpr const char* kHardwareDecoderName = "VideoToolbox";
#elif defined(_WIN32)
inline constexpr AVHWDeviceType kHardwareDeviceType = AV_HWDEVICE_TYPE_D3D11VA;
inline constexpr AVPixelFormat kHardwarePixelFormat = AV_PIX_FMT_D3D11;
inline constexpr const char* kHardwareDecoderName = "D3D11VA";
#else
inline constexpr AVHWDeviceType kHardwareDeviceType = AV_HWDEVICE_TYPE_NONE;
inline constexpr AVPixelFormat kHardwarePixelFormat = AV_PIX_FMT_NONE;
inline constexpr const char* kHardwareDecoderName = "hardware";
#endif
