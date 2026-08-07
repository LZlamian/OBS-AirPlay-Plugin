#pragma once

#include <obs-module.h>
#include <memory>

// Source callbacks
const char* airplay_source_get_name(void* type_data);
void* airplay_source_create(obs_data_t* settings, obs_source_t* source);
void airplay_source_destroy(void* data);
void airplay_source_get_defaults(obs_data_t* settings);
obs_properties_t* airplay_source_get_properties(void* data);
void airplay_source_update(void* data, obs_data_t* settings);
void airplay_source_show(void* data);
void airplay_source_hide(void* data);
void airplay_source_video_tick(void* data, float seconds);
void airplay_source_notify_frame_queued(uint64_t queued_ns);

// Get the global AirPlay server
std::shared_ptr<class AirPlayServer> get_airplay_server();

// Update the advertised AirPlay server name and restart mDNS
void update_server_name(const std::string& new_name);

struct AirPlaySource {
    obs_source_t* source;
    bool active;
    
    // Video settings
    int width;
    int height;
    enum video_format format;
    
    // Audio settings
    int sample_rate;
    enum audio_format audio_format;
    enum speaker_layout speakers;
    
    AirPlaySource(obs_source_t* source);
    ~AirPlaySource();
};
