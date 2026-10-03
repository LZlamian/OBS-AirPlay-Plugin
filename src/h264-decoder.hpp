#pragma once
#include <cstddef>
#include <cstdint>
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

    bool decodeToI420(const uint8_t* data, size_t size, DecodedVideoFrame& out_frame);
    void flush();

    // True while the most recent frame came from the hardware decoder.
    bool usingHardware() const { return m_last_frame_hardware; }

private:
    bool open(bool allow_hardware);
    void close();
    void releaseLockedBuffer();
    void rememberParameterSets(const uint8_t* data, size_t size);
    bool fallBackToSoftware(const char* reason);
    bool receiveFrame(DecodedVideoFrame& out_frame, int* error);
    bool outputHardwareFrame(DecodedVideoFrame& out_frame);
    bool outputSoftwareFrame(AVFrame* frame, DecodedVideoFrame& out_frame);
    void noteMode(bool hardware, int width, int height);

    static enum AVPixelFormat chooseFormat(AVCodecContext* context,
                                           const enum AVPixelFormat* formats);

    AVCodecID m_codec_id;
    // Whether hardware decoding is wanted at all, and whether it is still
    // allowed for the current stream (cleared by a fallback, restored at the
    // next stream boundary).
    bool m_hardware_preferred;
    bool m_hardware_allowed;
    bool m_hardware_open = false;
    AVCodecContext* m_codec_context;
    AVBufferRef* m_hw_device = nullptr;
    AVFrame* m_frame;
    AVFrame* m_frame_transfer = nullptr;
    AVFrame* m_frame_i420;
    AVPacket* m_packet;
    struct SwsContext* m_sws_context;

    // A VideoToolbox pixel buffer locked for the caller (CVPixelBufferRef).
    void* m_locked_buffer = nullptr;

    // The stream's latest parameter sets (SPS/PPS/VPS, Annex B), replayed
    // into a freshly opened decoder when falling back to software.
    std::vector<uint8_t> m_parameter_sets;

    int m_hardware_errors = 0;
    bool m_last_frame_hardware = false;
    bool m_mode_logged = false;
    bool m_logged_hardware = false;
    int m_logged_width = 0;
    int m_logged_height = 0;
};
