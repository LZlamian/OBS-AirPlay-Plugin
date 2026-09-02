#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct MediaVideoFrame {
    const uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int linesize[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
    uint64_t timestamp_ns = 0;
};

struct MediaAudioFrame {
    const float* data[2] = {nullptr, nullptr};
    uint32_t frames = 0;
    uint32_t sample_rate = 0;
    uint64_t timestamp_ns = 0;
};

struct MediaPlaybackInfo {
    double duration = 0.0;
    double position = 0.0;
    double seek_start = 0.0;
    double seek_duration = 0.0;
    float rate = 0.0f;
    bool ready_to_play = false;
    bool playback_buffer_empty = true;
    bool playback_buffer_full = false;
    bool playback_likely_to_keep_up = false;
    bool duration_known = false;
    bool has_video = false;
    bool has_audio = false;
    bool ended = false;
};

using MediaVideoCallback = std::function<void(const MediaVideoFrame&)>;
using MediaAudioCallback = std::function<void(const MediaAudioFrame&)>;

// Demuxes and decodes the URL-based AirPlay media mode used by Safari.
// Screen mirroring remains on UxPlay's RTP path and does not pass through here.
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    void setVideoCallback(MediaVideoCallback callback);
    void setAudioCallback(MediaAudioCallback callback);

    void play(const std::string& location, double start_position);
    void stop();
    void seek(double position);
    void setRate(float rate);
    float pauseForPlaylistRemoval();
    MediaPlaybackInfo getPlaybackInfo() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
