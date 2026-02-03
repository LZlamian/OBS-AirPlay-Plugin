#include <obs-module.h>
#include "airplay-source.hpp"
#include "airplay-server.hpp"
#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-airplay", "en-US")

static std::shared_ptr<AirPlayServer> g_airplay_server;

bool obs_module_load(void)
{
    blog(LOG_INFO, "OBS AirPlay Plugin loaded (version 1.0.0)");
    
    // Register the AirPlay source
    obs_source_info airplay_source_info = {};
    airplay_source_info.id = "airplay_source";
    airplay_source_info.type = OBS_SOURCE_TYPE_INPUT;
    airplay_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
    airplay_source_info.get_name = airplay_source_get_name;
    airplay_source_info.create = airplay_source_create;
    airplay_source_info.destroy = airplay_source_destroy;
    airplay_source_info.get_defaults = airplay_source_get_defaults;
    airplay_source_info.get_properties = airplay_source_get_properties;
    airplay_source_info.update = airplay_source_update;
    airplay_source_info.show = airplay_source_show;
    airplay_source_info.hide = airplay_source_hide;
    airplay_source_info.icon_type = OBS_ICON_TYPE_MEDIA;
    
    obs_register_source(&airplay_source_info);
    
    // Start the global AirPlay server
    try {
        g_airplay_server = std::make_shared<AirPlayServer>();
        bool started = g_airplay_server->start();
        if (started) {
            blog(LOG_INFO, "AirPlay server started successfully");
        } else {
            blog(LOG_ERROR, "Failed to start AirPlay server - start() returned false");
            return false;
        }
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "Failed to start AirPlay server: %s", e.what());
        return false;
    }
    
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "OBS AirPlay Plugin unloaded");
    
    // Stop the server
    if (g_airplay_server) {
        g_airplay_server->stop();
        g_airplay_server.reset();
    }
}

std::shared_ptr<AirPlayServer> get_airplay_server()
{
    return g_airplay_server;
}
