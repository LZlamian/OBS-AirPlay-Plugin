#include "airplay-source.hpp"
#include "airplay-server.hpp"
#include <obs-module.h>

AirPlaySource::AirPlaySource(obs_source_t* src)
    : source(src)
    , active(false)
    , width(1920)
    , height(1080)
    , format(VIDEO_FORMAT_NV12)
    , sample_rate(44100)
    , audio_format(AUDIO_FORMAT_FLOAT_PLANAR)
    , speakers(SPEAKERS_STEREO)
{
    // Disable OBS's internal async-source buffering for low-latency live mirroring.
    // Default behaviour buffers several frames before display (adds 100-200ms);
    // unbuffered + decoupled ensures frames render as soon as we hand them off.
    obs_source_set_async_unbuffered(source, true);
    obs_source_set_async_decoupled(source, true);

    // Register with the global server
    auto server = get_airplay_server();
    if (server) {
        server->registerSource(source);
    }
}

AirPlaySource::~AirPlaySource()
{
    // Unregister from the global server
    auto server = get_airplay_server();
    if (server) {
        server->unregisterSource(source);
    }
}

const char* airplay_source_get_name(void* type_data)
{
    UNUSED_PARAMETER(type_data);
    return obs_module_text("AirPlay");
}

void* airplay_source_create(obs_data_t* settings, obs_source_t* source)
{
    UNUSED_PARAMETER(settings);
    
    auto airplay_source = new AirPlaySource(source);
    
    blog(LOG_INFO, "AirPlay source created");
    return airplay_source;
}

void airplay_source_destroy(void* data)
{
    auto airplay_source = static_cast<AirPlaySource*>(data);
    delete airplay_source;
    blog(LOG_INFO, "AirPlay source destroyed");
}

void airplay_source_get_defaults(obs_data_t* settings)
{
    obs_data_set_default_bool(settings, "log_latency_telemetry", false);
}

obs_properties_t* airplay_source_get_properties(void* data)
{
    UNUSED_PARAMETER(data);
    
    obs_properties_t* props = obs_properties_create();
    
    obs_properties_add_text(props, "info", 
        "Add this source to start receiving AirPlay streams.\n"
        "Your device should appear in the screen mirroring list on iOS devices.",
        OBS_TEXT_INFO);
    
    auto server = get_airplay_server();
    if (server && server->isRunning()) {
        std::string server_info = "Server Name: " + server->getServerName() + "\n";
        server_info += "AirPlay Port: " + std::to_string(server->getAirPlayPort()) + "\n";
        server_info += "RAOP Port: " + std::to_string(server->getRAOPPort()) + "\n";
        server_info += "Status: Running";
        
        obs_properties_add_text(props, "server_info", server_info.c_str(), OBS_TEXT_INFO);
    } else {
        obs_properties_add_text(props, "server_info", 
            "Status: Server not running", OBS_TEXT_INFO);
    }

    obs_properties_add_bool(props, "log_latency_telemetry",
        "Log latency telemetry (decode/output ms, every 120 video / 240 audio frames)");

    return props;
}

void airplay_source_update(void* data, obs_data_t* settings)
{
    UNUSED_PARAMETER(data);
    g_latency_telemetry_enabled.store(
        obs_data_get_bool(settings, "log_latency_telemetry"),
        std::memory_order_relaxed);
}

void airplay_source_show(void* data)
{
    auto airplay_source = static_cast<AirPlaySource*>(data);
    airplay_source->active = true;
    blog(LOG_INFO, "AirPlay source shown");
}

void airplay_source_hide(void* data)
{
    auto airplay_source = static_cast<AirPlaySource*>(data);
    airplay_source->active = false;
    blog(LOG_INFO, "AirPlay source hidden");
}
