// SPDX-License-Identifier: GPL-3.0-or-later
#include <obs-module.h>
#include "airplay-source.hpp"
#include "airplay-server.hpp"
#include "uxplay-integration.hpp"
#include "airplay-ble-helper.hpp"
#include "media-player.hpp"
#include "module-settings.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdio>
#include <cstring>
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
// g_server_name is read by OBS threads (source create/update) and written by
// the debounce thread.
static std::mutex g_server_name_mutex;

// The receiver's settings, shared by every AirPlay source (module-settings.hpp).
static std::mutex g_module_settings_mutex;
static ModuleSettings g_module_settings;
// False until settings.json exists or the first source's settings were taken.
static bool g_module_settings_known = false;
static std::string g_server_name_error;

// Status thread: refreshes open AirPlay source panels after the receiver's
// state changed (at most once a second, from a thread of its own, since the
// change is reported on UxPlay's threads).
static std::mutex              g_status_mutex;
static std::condition_variable g_status_cv;
static bool                    g_status_quit = false;
static std::atomic<bool>       g_status_dirty{false};
static std::thread             g_status_thread;

static void refresh_airplay_source_panels()
{
    obs_enum_sources([](void*, obs_source_t* source) {
        const char* id = obs_source_get_unversioned_id(source);
        if (id && strcmp(id, "airplay_source") == 0) {
            obs_source_update_properties(source);
        }
        return true;
    }, nullptr);
}

// Streaming, recording, a replay buffer or the virtual camera.
static bool any_output_active()
{
    bool active = false;
    obs_enum_outputs([](void* param, obs_output_t* output) {
        if (obs_output_active(output)) {
            *static_cast<bool*>(param) = true;
            return false;
        }
        return true;
    }, &active);
    return active;
}

static void status_thread_func()
{
    // The update helper never prompts while an output is active.
    int outputs_active = -1;
    std::unique_lock<std::mutex> lock(g_status_mutex);
    while (!g_status_quit) {
        g_status_cv.wait_for(lock, std::chrono::seconds(1));
        if (g_status_quit) break;
        lock.unlock();
        if (g_status_dirty.exchange(false)) {
            refresh_airplay_source_panels();
        }
        const int active = any_output_active() ? 1 : 0;
        if (active != outputs_active) {
            outputs_active = active;
            set_helper_obs_outputs_active(active != 0);
        }
        lock.lock();
    }
    set_helper_obs_outputs_active(false);
}

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
    {
        std::lock_guard<std::mutex> lock(g_server_name_mutex);
        // The user reverted to the advertised name before the debounce fired.
        if (name == g_server_name)
            return;
        g_server_name = name;
    }
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

    std::lock_guard<std::mutex> lock(g_debounce_mutex);
    if (g_debounce_gen != 0) {
        // A change is in flight: compare with the name it will apply, so
        // reverting to the advertised name replaces the pending one.
        if (resolved == g_debounce_pending)
            return;
    } else {
        std::lock_guard<std::mutex> name_lock(g_server_name_mutex);
        if (resolved == g_server_name)
            return;
    }
    g_debounce_pending = resolved;
    ++g_debounce_gen;
    g_debounce_cv.notify_one();
}

static void apply_module_runtime(const ModuleSettings& settings)
{
    g_latency_telemetry_enabled.store(settings.log_latency_telemetry, std::memory_order_relaxed);
    MediaPlayer::setFollowSenderClock(settings.follow_sender_clock);
    MediaPlayer::setAllowInsecureTls(settings.allow_self_signed_media);
    set_helper_update_checks_enabled(settings.check_for_updates);
}

static void write_module_settings(obs_data_t* data, const ModuleSettings& settings)
{
    obs_data_set_string(data, "server_name", settings.server_name.c_str());
    obs_data_set_bool(data, "follow_sender_clock", settings.follow_sender_clock);
    obs_data_set_bool(data, "log_latency_telemetry", settings.log_latency_telemetry);
    obs_data_set_bool(data, "allow_self_signed_media", settings.allow_self_signed_media);
    obs_data_set_bool(data, "check_for_updates", settings.check_for_updates);
}

ModuleSettings read_source_settings(obs_data_t* data)
{
    ModuleSettings settings;
    const char* name = obs_data_get_string(data, "server_name");
    settings.server_name = name && *name ? name : "OBS AirPlay";
    settings.follow_sender_clock = obs_data_get_bool(data, "follow_sender_clock");
    settings.log_latency_telemetry = obs_data_get_bool(data, "log_latency_telemetry");
    settings.allow_self_signed_media = obs_data_get_bool(data, "allow_self_signed_media");
    settings.check_for_updates = obs_data_get_bool(data, "check_for_updates");
    return settings;
}

ModuleSettings get_module_settings()
{
    std::lock_guard<std::mutex> lock(g_module_settings_mutex);
    return g_module_settings;
}

std::string get_server_name_error()
{
    std::lock_guard<std::mutex> lock(g_module_settings_mutex);
    return g_server_name_error;
}

// Shows the receiver's settings in every AirPlay source except `except`
// (their panels read the source's own settings). Does not call update.
static void show_module_settings_in_sources(const ModuleSettings& settings, obs_source_t* except)
{
    struct Context {
        const ModuleSettings* settings;
        obs_source_t* except;
    } context = {&settings, except};
    obs_enum_sources([](void* param, obs_source_t* source) {
        auto* ctx = static_cast<Context*>(param);
        const char* id = obs_source_get_unversioned_id(source);
        if (source != ctx->except && id && strcmp(id, "airplay_source") == 0) {
            obs_data_t* data = obs_source_get_settings(source);
            write_module_settings(data, *ctx->settings);
            obs_data_release(data);
        }
        return true;
    }, &context);
}

// From a source's update: the receiver takes the new values, saves them and
// shows them in the other sources.
void set_module_settings(ModuleSettings settings, obs_source_t* origin)
{
    std::string accepted_name;
    {
        std::lock_guard<std::mutex> lock(g_module_settings_mutex);
        if (settings.server_name.size() > kMaxServerNameBytes) {
            g_server_name_error = "The name is longer than " +
                std::to_string(kMaxServerNameBytes) +
                " bytes, the most AirPlay devices can show; still advertising \"" +
                g_module_settings.server_name + "\".";
            blog(LOG_WARNING, "[AirPlay] server name too long (%zu bytes); keeping \"%s\"",
                 settings.server_name.size(), g_module_settings.server_name.c_str());
            settings.server_name = g_module_settings.server_name;
        } else {
            g_server_name_error.clear();
        }
        if (g_module_settings_known && settings == g_module_settings) return;
        g_module_settings = settings;
        g_module_settings_known = true;
        accepted_name = settings.server_name;
    }
    apply_module_runtime(settings);
    update_server_name(accepted_name);
    module_settings_save(settings);
    show_module_settings_in_sources(settings, origin);
}

// From a source's create: an existing configuration is shown in it; before
// settings.json exists the first source's (per-source, older versions)
// settings become the receiver's.
void adopt_or_show_module_settings(obs_data_t* source_settings, obs_source_t* source)
{
    bool known = false;
    ModuleSettings current;
    {
        std::lock_guard<std::mutex> lock(g_module_settings_mutex);
        known = g_module_settings_known;
        current = g_module_settings;
    }
    if (!known) {
        blog(LOG_INFO, "[AirPlay] taking receiver settings from the first AirPlay source");
        set_module_settings(read_source_settings(source_settings), source);
    } else {
        write_module_settings(source_settings, current);
    }
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "OBS AirPlay Plugin loaded (version " PLUGIN_VERSION ")");

    {
        bool found = false;
        ModuleSettings settings = module_settings_load(&found);
        std::lock_guard<std::mutex> lock(g_module_settings_mutex);
        g_module_settings = settings;
        g_module_settings_known = found;
        std::lock_guard<std::mutex> name_lock(g_server_name_mutex);
        // The receiver starts with the saved name: no rename once the
        // scene collection loads.
        g_server_name = settings.server_name;
    }
    apply_module_runtime(get_module_settings());

    // Start the name debounce thread before anything else.
    g_debounce_thread = std::thread(name_debounce_thread_func);
    
    try {
        // Register the AirPlay source
        obs_source_info airplay_source_info = {};
        airplay_source_info.id = "airplay_source";
        airplay_source_info.type = OBS_SOURCE_TYPE_INPUT;
        // One receiver per process: a duplicate would only mirror the same
        // picture, so OBS offers a reference instead.
        airplay_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
                                           OBS_SOURCE_DO_NOT_DUPLICATE;
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
        g_uxplay_integration = std::make_shared<UxPlayIntegration>();
        g_uxplay_integration->setStateChangedCallback([] { g_status_dirty.store(true); });
        g_status_thread = std::thread(status_thread_func);
        std::string initial_name;
        {
            std::lock_guard<std::mutex> lock(g_server_name_mutex);
            initial_name = g_server_name;
        }
        if (!g_uxplay_integration->start(mac_address, 7000, initial_name)) {
            blog(LOG_ERROR,
                 "Failed to start UxPlay integration; the AirPlay source will remain unavailable. "
                 "Check whether port 7000 is in use and review the preceding UxPlay log messages.");
            // Keep the module loaded so users can inspect properties and logs.
            // The old hand-written socket fallback did not implement a complete
            // AirPlay handshake and could falsely report that receiving worked.
            return true;
        } else {
            blog(LOG_INFO, "UxPlay integration started successfully");

            // AirServer starts an otherwise empty CoreBluetooth peripheral
            // advertisement before any iPhone connection. Keep the
            // equivalent discovery signal alive while the receiver runs.
            start_airplay_ble_helper();

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

            // Safari's media controls use URL playback, not the encoded
            // type-110 mirroring callbacks above. The media player decodes to
            // OBS-ready I420 video and planar float audio.
            g_uxplay_integration->setMediaVideoCallback([](const MediaVideoFrame& frame) {
                if (g_airplay_server) {
                    g_airplay_server->outputMediaVideoFrame(frame);
                }
            });

            g_uxplay_integration->setMediaAudioCallback([](const MediaAudioFrame& frame) {
                if (g_airplay_server) {
                    g_airplay_server->outputMediaAudioFrame(frame);
                }
            });
            
            g_airplay_server->setMirrorFrameOutputCallback([]() {
                if (g_uxplay_integration) {
                    g_uxplay_integration->onMirrorFrameOutput();
                }
            });

            g_uxplay_integration->setConnectionResetCallback(
                [](bool clear_output, bool flush_decoders) {
                if (g_airplay_server) {
                    g_airplay_server->resetDecoders(clear_output, flush_decoders);
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

    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status_quit = true;
    }
    g_status_cv.notify_all();
    if (g_status_thread.joinable())
        g_status_thread.join();

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
