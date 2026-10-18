// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct DecodedVideoFrame {
    int width = 0;
    int height = 0;
    int linesize[3] = {0, 0, 0};
    // Pointers reference data owned by the decoder (its AVFrame, or a locked
    // VideoToolbox pixel buffer). Valid only until the next decode()/flush()
    // call on the same decoder.
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    // false: I420 (three planes). true: NV12 (Y plane + interleaved UV plane),
    // as produced by hardware decoding.
    bool nv12 = false;
    bool full_range = false;
    // BT.601 colour matrix (the stream says so); otherwise BT.709.
    bool bt601 = false;
    // The pts passed with the packet this picture was decoded from.
    int64_t pts = AV_NOPTS_VALUE;
};

// Decoder for the AirPlay mirroring stream (H.264 or HEVC, Annex B).
//
// Uses VideoToolbox hardware decoding when the linked FFmpeg provides it and
// falls back to FFmpeg's software decoder automatically: when hardware
// decoding is unavailable, when a stream cannot be decoded in hardware, or
// after repeated hardware errors. Set OBS_AIRPLAY_HW_DECODE=0 to force
// software decoding.
class H264Decoder {
public:
    enum class Mode { Auto, SoftwareOnly };

    explicit H264Decoder(AVCodecID codec_id = AV_CODEC_ID_H264, Mode mode = Mode::Auto);
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Decodes one packet (Annex B). When it holds more than one picture,
    // the newest is returned. `pts` comes back in out_frame.pts.
    bool decodeToI420(const uint8_t* data, size_t size, DecodedVideoFrame& out_frame,
                      int64_t pts = AV_NOPTS_VALUE);
    void flush();
    // The caller is done with the last frame's planes (OBS has copied them):
    // a locked VideoToolbox buffer goes back to the decoder's pool at once
    // instead of at the next decode.
    void releaseFrame();

    // True while the most recent frame came from the hardware decoder.
    bool usingHardware() const { return m_last_frame_hardware; }

private:
    bool open(bool allow_hardware);
    bool reopen();
    bool noteError(int error);
    void close();
    void releaseLockedBuffer();
    void rememberParameterSets(const uint8_t* data, size_t size);
    bool fallBackToSoftware(const char* reason);
    bool collectFrames(int* error);
    bool produceFrame(DecodedVideoFrame& out_frame, int* error);
    bool outputHardwareFrame(DecodedVideoFrame& out_frame);
    bool outputSoftwareFrame(AVFrame* frame, DecodedVideoFrame& out_frame);
    void noteMode(bool hardware, int width, int height, bool nv12, bool full_range);

    static enum AVPixelFormat chooseFormat(AVCodecContext* context,
                                           const enum AVPixelFormat* formats);

    AVCodecID m_codec_id;
    // Whether hardware decoding is wanted at all, and whether it is still
    // allowed for the current stream (cleared by a fallback, restored at the
    // next stream boundary).
    bool m_hardware_preferred;
    bool m_hardware_allowed;
    bool m_hardware_open = false;
    // Set while decoding when FFmpeg offers no hardware format for the stream.
    bool m_hardware_refused = false;
    AVCodecContext* m_codec_context;
    AVBufferRef* m_hw_device = nullptr;
    AVFrame* m_frame;
    AVFrame* m_frame_next = nullptr;  // receives while m_frame holds the newest
    AVFrame* m_frame_transfer = nullptr;
    AVFrame* m_frame_i420;
    AVPacket* m_packet;
    struct SwsContext* m_sws_context;

    // A VideoToolbox pixel buffer locked for the caller (CVPixelBufferRef).
    void* m_locked_buffer = nullptr;

    // The stream's latest parameter set of each kind (VPS/SPS/PPS, by NAL
    // type, so a PPS-only update is kept too), and all of them as Annex B,
    // replayed into a freshly opened decoder when falling back to software.
    std::map<int, std::vector<uint8_t>> m_parameter_set_units;
    std::vector<uint8_t> m_parameter_sets;

    // Consecutive errors since the last decoded picture: hardware failures,
    // "invalid data" (which a damaged stream causes as well), and the count
    // behind the rate-limited error log line.
    int m_hardware_errors = 0;
    int m_stream_errors = 0;
    int m_errors_logged = 0;
    bool m_last_frame_hardware = false;
    bool m_mode_logged = false;
    bool m_logged_hardware = false;
    int m_logged_width = 0;
    int m_logged_height = 0;
    bool m_logged_full_range = false;
};
