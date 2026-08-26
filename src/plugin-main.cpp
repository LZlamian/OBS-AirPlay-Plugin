#include <obs-module.h>
#include "airplay-source.hpp"
#include "airplay-server.hpp"
#include "uxplay-integration.hpp"
#include "airplay-ble-helper.hpp"
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdio>
#include <util/platform.h>

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
static std::mutex g_server_mutex;
static std::shared_ptr<UxPlayIntegration> g_uxplay_integration;
static std::mutex g_uxplay_mutex;
static std::string g_server_name = "OBS AirPlay";

// Debounce: defer mDNS name updates until the user stops typing for 800ms.
static std::mutex              g_debounce_mutex;
static std::condition_variable g_debounce_cv;
static std::string             g_debounce_pending;
static uint64_t                g_debounce_gen  = 0;
static bool                    g_debounce_quit = false;
static std::thread             g_debounce_thread;

// Persist the receiver identity so Bonjour caches and the advertised public
// key remain stable across OBS restarts.
static std::string load_or_persist_mac(const std::string& generated_mac)
{
    char* path = obs_module_config_path("mac_address.txt");
    if (!path) {
        return generated_mac;
    }

    // Try to load existing MAC
    FILE* f = fopen(path, "r");
    if (f) {
        char buf[32] = {};
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            std::string mac(buf);
            while (!mac.empty() && (mac.back() == '\n' || mac.back() == '\r'))
                mac.pop_back();
            if (mac.size() == 17) {
                bfree(path);
                blog(LOG_INFO, "Loaded persisted AirPlay MAC: %s", mac.c_str());
                return mac;
            }
        } else {
            fclose(f);
        }
    }

    // First launch — create the config directory and save
    char* dir = obs_module_config_path("");
    if (dir) {
        os_mkdirs(dir);
        bfree(dir);
    }
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", generated_mac.c_str());
        fclose(f);
        blog(LOG_INFO, "Saved new AirPlay MAC: %s", generated_mac.c_str());
    }
    bfree(path);
    return generated_mac;
}

// Applies a server name change to the live mDNS advertisement.
static void apply_server_name(const std::string& name)
{
    g_server_name = name;
    blog(LOG_INFO, "Updating AirPlay server name to: %s", name.c_str());

    std::shared_ptr<UxPlayIntegration> uxplay;
    {
        std::lock_guard<std::mutex> lock(g_uxplay_mutex);
        uxplay = g_uxplay_integration;
    }
    if (uxplay) {
        uxplay->updateServerName(name);
        blog(LOG_INFO, "mDNS restarted with new server name: %s", name.c_str());
    }
}

// Debounce thread: waits 800ms after the last scheduled name change, then applies it.
// This prevents mDNS re-registration on every keystroke in the preferences panel.
static void name_debounce_thread_func()
{
    for (;;) {
        uint64_t my_gen;
        std::string my_name;

        // Wait until a name change is queued.
        {
            std::unique_lock<std::mutex> lock(g_debounce_mutex);
            g_debounce_cv.wait(lock, [] {
                return g_debounce_gen > 0 || g_debounce_quit;
            });
            if (g_debounce_quit) break;
            my_gen  = g_debounce_gen;
            my_name = g_debounce_pending;
        }

        // Wait 800ms. If a newer change arrives, restart the wait.
        {
            std::unique_lock<std::mutex> lock(g_debounce_mutex);
            bool superseded = g_debounce_cv.wait_for(
                lock,
                std::chrono::milliseconds(800),
                [my_gen] { return g_debounce_gen != my_gen || g_debounce_quit; });
            if (g_debounce_quit) break;
            if (superseded) continue; // Newer change came in — loop back and re-wait.
        }

        // No newer change in 800ms — apply.
        apply_server_name(my_name);

        // Reset gen so the thread blocks until the next change.
        {
            std::lock_guard<std::mutex> lock(g_debounce_mutex);
            if (g_debounce_gen == my_gen) g_debounce_gen = 0;
        }
    }
}

// Called from airplay_source_update() when the user edits the server name.
// Schedules the mDNS update to fire 800ms after the user stops typing.
void update_server_name(const std::string& new_name)
{
    const std::string resolved = new_name.empty() ? "OBS AirPlay" : new_name;
    if (resolved == g_server_name)
        return;

    std::lock_guard<std::mutex> lock(g_debounce_mutex);
    g_debounce_pending = resolved;
    ++g_debounce_gen;
    g_debounce_cv.notify_one();
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "OBS AirPlay Plugin loaded (version " PLUGIN_VERSION ")");

    // Start the name debounce thread before anything else.
    g_debounce_thread = std::thread(name_debounce_thread_func);
    
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
        airplay_source_info.video_tick = airplay_source_video_tick;
        airplay_source_info.show = airplay_source_show;
        airplay_source_info.hide = airplay_source_hide;
        airplay_source_info.icon_type = OBS_ICON_TYPE_MEDIA;
        
        blog(LOG_INFO, "Registering AirPlay source...");
        obs_register_source(&airplay_source_info);
        blog(LOG_INFO, "AirPlay source registered successfully");
        
        // Initialize AirPlay server first to get a consistent MAC address
        g_airplay_server = std::make_shared<AirPlayServer>();
        // Persist the receiver identity and advertised public key across launches.
        std::string mac_address = load_or_persist_mac(g_airplay_server->getMACAddress());
        g_airplay_server->setMACAddress(mac_address);
        blog(LOG_INFO, "Using consistent MAC address for AirPlay: %s", mac_address.c_str());

        // Start UxPlay integration for proper AirPlay protocol handling
        blog(LOG_INFO, "Starting UxPlay integration with MAC: %s", mac_address.c_str());
        // AirServer starts an otherwise empty CoreBluetooth peripheral
        // advertisement before any iPhone connection. Keep the equivalent
        // discovery signal alive for the lifetime of this plugin.
        start_airplay_ble_helper();
        g_uxplay_integration = std::make_shared<UxPlayIntegration>();
        if (!g_uxplay_integration->start(mac_address, 7000, g_server_name)) {
            blog(LOG_ERROR,
                 "Failed to start UxPlay integration; the AirPlay source will remain unavailable. "
                 "Check whether port 7000 is in use and review the preceding UxPlay log messages.");
            // Keep the module loaded so users can inspect properties and logs.
            // The old hand-written socket fallback did not implement a complete
            // AirPlay handshake and could falsely report that receiving worked.
            return true;
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

            g_uxplay_integration->setAudioCallback([](const uint8_t* data, size_t size, uint8_t codec_type, uint64_t pts) {
                if (!data || size == 0) {
                    return;
                }
                if (g_airplay_server) {
                    g_airplay_server->ingestAudioBitstream(data, size, codec_type, pts);
                }
            });
            
            g_uxplay_integration->setConnectionResetCallback([]() {
                if (g_airplay_server) {
                    g_airplay_server->resetDecoders();
                }
            });

            // UxPlay's dnssd context handles all mDNS advertising internally
            // via dnssd_register_raop/airplay called inside start(). No second
            // MDNSPublisher registration is needed or wanted — duplicate
            // registrations caused mDNSResponder to silently rename one entry,
            // so iOS saw the stale/wrong name in Screen Mirroring.
            blog(LOG_INFO, "UxPlay integration active on port %d", g_uxplay_integration->getActualPort());

            return true;
        }
        
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "Failed to start AirPlay server: %s", e.what());
        return false;
    }
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "OBS AirPlay Plugin unloaded");

    stop_airplay_ble_helper();

    // Shut down the debounce thread first so it doesn't race with cleanup below.
    {
        std::lock_guard<std::mutex> lock(g_debounce_mutex);
        g_debounce_quit = true;
        g_debounce_cv.notify_one();
    }
    if (g_debounce_thread.joinable())
        g_debounce_thread.join();
    
    // Stop UxPlay integration
    {
        std::lock_guard<std::mutex> lock(g_uxplay_mutex);
        if (g_uxplay_integration) {
            g_uxplay_integration->stop();
            g_uxplay_integration.reset();
        }
    }
    
    // Stop and release the server under lock so concurrent get_airplay_server()
    // calls from source create/destroy callbacks see a consistent state.
    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_airplay_server) {
        g_airplay_server->stop();
        g_airplay_server.reset();
    }
}

std::shared_ptr<UxPlayIntegration> get_uxplay_integration()
{
    std::lock_guard<std::mutex> lock(g_uxplay_mutex);
    return g_uxplay_integration;
}

std::shared_ptr<AirPlayServer> get_airplay_server()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_airplay_server;
}
