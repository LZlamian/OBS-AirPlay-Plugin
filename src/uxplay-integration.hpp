#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Forward declarations
struct obs_source;
typedef struct obs_source obs_source_t;

// Video frame callback
typedef std::function<void(uint8_t** data, int* linesize, int width, int height, uint64_t pts)> VideoFrameCallback;

// Audio data callback  
typedef std::function<void(uint8_t* data, int samples, int channels, int sample_rate, uint64_t pts)> AudioDataCallback;

class UxPlayIntegration {
public:
    UxPlayIntegration();
    ~UxPlayIntegration();
    
    // Start the UxPlay server
    bool start(int port = 7100);
    
    // Stop the server
    void stop();
    
    // Set callbacks for video and audio
    void setVideoCallback(VideoFrameCallback callback);
    void setAudioCallback(AudioDataCallback callback);
    
    // Check if running
    bool isRunning() const { return m_running; }
    
private:
    bool m_running;
    VideoFrameCallback m_video_callback;
    AudioDataCallback m_audio_callback;
    std::mutex m_mutex;
    
    // Thread for handling incoming data
    std::thread* m_worker_thread;
    std::condition_variable m_cv;
    std::atomic<bool> m_should_stop;
    
    // Internal video/audio processing
    void processVideoData(uint8_t* data, size_t len);
    void processAudioData(uint8_t* data, size_t len);
    
    // Worker thread function
    void workerThread();
};
