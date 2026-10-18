// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <obs-module.h>
#include <memory>
#include <string>

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

// The receiver's settings, shared by all AirPlay sources (plugin-main.cpp).
struct ModuleSettings;
ModuleSettings get_module_settings();
ModuleSettings read_source_settings(obs_data_t* data);
void set_module_settings(ModuleSettings settings, obs_source_t* origin);
void adopt_or_show_module_settings(obs_data_t* source_settings, obs_source_t* source);
// Why the last server name was not used, or empty.
std::string get_server_name_error();

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
