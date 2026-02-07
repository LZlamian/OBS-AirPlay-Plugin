#include <obs-module.h>
#include "airplay-source.hpp"
#include "airplay-server.hpp"
#include "uxplay-integration.hpp"
#include <memory>

// Static initializer to test if plugin loads
static bool plugin_loaded = false;
__attribute__((constructor))
static void plugin_initializer() {
    blog(LOG_INFO, "=== OBS AirPlay Plugin CONSTRUCTOR called ===");
    plugin_loaded = true;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-airplay", "en-US")

static std::shared_ptr<AirPlayServer> g_airplay_server;
static std::shared_ptr<UxPlayIntegration> g_uxplay_integration;

bool obs_module_load(void)
{
    blog(LOG_INFO, "OBS AirPlay Plugin loaded (version 1.0.0)");
    
    try {
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
        
        blog(LOG_INFO, "Registering AirPlay source...");
        obs_register_source(&airplay_source_info);
        blog(LOG_INFO, "AirPlay source registered successfully");
        
        // Initialize AirPlay server first to get a consistent MAC address
        g_airplay_server = std::make_shared<AirPlayServer>();
        std::string mac_address = g_airplay_server->getMACAddress();
        blog(LOG_INFO, "Using consistent MAC address for AirPlay: %s", mac_address.c_str());

        // Start UxPlay integration for proper AirPlay protocol handling
        blog(LOG_INFO, "Starting UxPlay integration with MAC: %s", mac_address.c_str());
        g_uxplay_integration = std::make_shared<UxPlayIntegration>();
        if (!g_uxplay_integration->start(mac_address, 7000)) {
            blog(LOG_WARNING, "Failed to start UxPlay integration - continuing with basic server only");
            // Don't return false - continue with basic server
        } else {
            blog(LOG_INFO, "UxPlay integration started successfully");

            g_uxplay_integration->setVideoCallback([](const uint8_t* data, size_t size, uint64_t pts, bool is_h265) {
                if (!data || size == 0) {
                    return;
                }
                if (g_airplay_server) {
                    g_airplay_server->ingestVideoBitstream(data, size, pts, is_h265);
                }
            });
            
            // Start mDNS advertising for UxPlay on the actual port it's using
            uint16_t actual_port = g_uxplay_integration->getActualPort();
            std::string pk = g_uxplay_integration->getPK();
            
            blog(LOG_INFO, "Starting mDNS advertising for UxPlay on actual port %d with PK: %s", actual_port, pk.c_str());
            
            // Only start mDNS, don't start the basic server sockets
            if (g_airplay_server->startMDNS("OBS AirPlay", actual_port, actual_port, pk)) {
                blog(LOG_INFO, "mDNS advertising started for UxPlay on port %d", actual_port);
            } else {
                blog(LOG_WARNING, "Failed to start mDNS advertising for UxPlay");
            }
            
            // Keep call for compatibility; currently a no-op in integration.
            blog(LOG_INFO, "Finalizing UxPlay integration setup");
            g_uxplay_integration->disableInternalMDNS();
            
            return true;
        }
        
        // Start the basic AirPlay server for mDNS announcement (on different port)
        blog(LOG_INFO, "Starting basic AirPlay server...");
        g_airplay_server = std::make_shared<AirPlayServer>();
        bool started = g_airplay_server->start();
        if (started) {
            blog(LOG_INFO, "AirPlay server started successfully");
        } else {
            blog(LOG_ERROR, "Failed to start AirPlay server - start() returned false");
            // Continue anyway since UxPlay is working
        }
        
        blog(LOG_INFO, "All AirPlay services started successfully");
        return true;
        
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "Failed to start AirPlay server: %s", e.what());
        return false;
    }
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "OBS AirPlay Plugin unloaded");
    
    // Stop UxPlay integration
    if (g_uxplay_integration) {
        g_uxplay_integration->stop();
        g_uxplay_integration.reset();
    }
    
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
