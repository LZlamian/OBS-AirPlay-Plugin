#pragma once

#include "rtp-receiver.hpp"
#include "h264-decoder.hpp"
#include "audio-decoder.hpp"
#include "crypto-utils.hpp"
#include <obs-module.h>
#include <memory>
#include <thread>
#include <atomic>
#include <plist/plist.h>

struct obs_source;
typedef struct obs_source obs_source_t;

struct StreamConfig {
    // Video
    int video_width;
    int video_height;
    int video_fps;
    std::string video_codec; // "H264"
    
    // Audio  
    int audio_sample_rate;
    int audio_channels;
    std::string audio_codec; // "AAC" or "ALAC"
    
    // Encryption
    bool encrypted;
    std::vector<uint8_t> aes_key;
    std::vector<uint8_t> aes_iv;
    uint64_t stream_connection_id;
    
    // RTP ports
    int video_data_port;
    int video_control_port;
    int audio_data_port;
    int audio_control_port;
};

class AirPlayStream {
public:
    AirPlayStream(obs_source_t* source);
    ~AirPlayStream();
    
    // Parse stream configuration from plist
    bool parseConfig(const std::string& plist_xml);
    
    // Start streaming
    bool start();
    
    // Stop streaming
    void stop();
    
    // Check if streaming
    bool isStreaming() const { return m_streaming; }
    
    // Get configuration
    const StreamConfig& getConfig() const { return m_config; }
    
private:
    obs_source_t* m_source;
    StreamConfig m_config;
    std::atomic<bool> m_streaming;
    
    // RTP receivers
    std::unique_ptr<RTPReceiver> m_video_receiver;
    std::unique_ptr<RTPReceiver> m_audio_receiver;
    
    // Decoders
    std::unique_ptr<H264Decoder> m_video_decoder;
    std::unique_ptr<AudioDecoder> m_audio_decoder;
    
    // Decryption
    std::unique_ptr<AESDecryptor> m_video_decryptor;
    std::unique_ptr<AESDecryptor> m_audio_decryptor;
    
    // SPS/PPS buffering (for NAL prepending)
    std::vector<uint8_t> m_sps_pps_buffer;
    bool m_pending_sps_pps;
    
    // Receiver threads
    std::thread m_video_thread;
    std::thread m_audio_thread;
    
    // Thread functions
    void videoReceiverThread();
    void audioReceiverThread();
    
    // Process received data
    void processVideoPacket(const std::vector<uint8_t>& data);
    void processAudioPacket(const std::vector<uint8_t>& data);
    
    // Decryption
    bool decryptVideoData(uint8_t* data, size_t length);
    bool decryptAudioData(uint8_t* data, size_t length);
    
    // NAL unit processing
    void processNALUnits(uint8_t* data, size_t length);
    void handleUnencryptedSPSPPS(uint8_t* data, size_t length);
    
    // Output to OBS
    void outputVideoFrame(uint8_t** data, int* linesize, int width, int height, uint64_t pts);
    void outputAudioData(uint8_t* data, int samples, int channels, int sample_rate, uint64_t pts);
};
