#include "airplay-source.hpp"
#include "airplay-server.hpp"
#include "uxplay-integration.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>

extern std::shared_ptr<UxPlayIntegration> get_uxplay_integration();

static std::atomic<uint64_t> g_first_frame_queued_ns{0};

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
    auto airplay_source = new AirPlaySource(source);

    // OBS may defer the 'update' callback to the graphics thread via
    // obs_source_video_tick. Apply the saved name here, synchronously on the
    // main thread, so the server advertises the correct name from the moment
    // the source is loaded — not just after the first frame tick.
    const char* saved_name = obs_data_get_string(settings, "server_name");
    if (saved_name && *saved_name) {
        update_server_name(saved_name);
    }

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
    obs_data_set_default_string(settings, "server_name", "OBS AirPlay");
    obs_data_set_default_bool(settings, "log_latency_telemetry", false);
}

static bool reset_server_name_clicked(obs_properties_t* props, obs_property_t* property,
                                      void* data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);

    auto* airplay_src = static_cast<AirPlaySource*>(data);
    if (!airplay_src || !airplay_src->source)
        return false;

    obs_data_t* settings = obs_source_get_settings(airplay_src->source);
    obs_data_set_string(settings, "server_name", "OBS AirPlay");
    obs_source_update(airplay_src->source, settings);
    obs_data_release(settings);

    return true; // Refresh the properties UI so the text field shows the reset value
}

obs_properties_t* airplay_source_get_properties(void* data)
{
    auto* airplay_src = static_cast<AirPlaySource*>(data);

    obs_properties_t* props = obs_properties_create();

    obs_properties_add_text(props, "server_name",
        obs_module_text("AirPlay.ServerName"),
        OBS_TEXT_DEFAULT);

    obs_properties_add_button(props, "reset_server_name",
        obs_module_text("AirPlay.ResetServerName"),
        reset_server_name_clicked);

    obs_properties_add_text(props, "info", 
        "Add this source to start receiving AirPlay streams.\n"
        "Your device should appear in the screen mirroring list on iOS devices.",
        OBS_TEXT_INFO);
    
    auto uxplay = get_uxplay_integration();
    if (uxplay && uxplay->isRunning()) {
        std::string server_info = "Status: Running\n";
        server_info += "Port: " + std::to_string(uxplay->getActualPort());
        obs_properties_add_text(props, "server_info", server_info.c_str(), OBS_TEXT_INFO);
    } else {
        obs_properties_add_text(props, "server_info",
            "Status: Server not running", OBS_TEXT_INFO);
    }

    obs_properties_add_bool(props, "log_latency_telemetry",
        "Log latency telemetry (decode/output ms, every 120 video / 240 audio frames)");

    obs_properties_add_text(props, "plugin_version",
        "OBS AirPlay Plugin v" PLUGIN_VERSION, OBS_TEXT_INFO);

    return props;
}

void airplay_source_update(void* data, obs_data_t* settings)
{
    UNUSED_PARAMETER(data);

    const char* new_name = obs_data_get_string(settings, "server_name");
    if (new_name && *new_name) {
        // Forward to update_server_name which guards against no-op updates
        // and delegates to UxPlay's dnssd context (the sole mDNS advertiser).
        update_server_name(new_name);
    }

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

void airplay_source_notify_frame_queued(uint64_t queued_ns)
{
    g_first_frame_queued_ns.store(queued_ns, std::memory_order_release);
}

void airplay_source_video_tick(void* data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    const uint64_t queued_ns = g_first_frame_queued_ns.load(std::memory_order_acquire);
    if (!queued_ns) {
        return;
    }

    auto* airplay_source = static_cast<AirPlaySource*>(data);
    if (!airplay_source || !airplay_source->source) {
        return;
    }

    obs_source_frame* frame = obs_source_get_frame(airplay_source->source);
    if (!frame) {
        return;
    }

    const uint64_t now = os_gettime_ns();
    blog(LOG_INFO,
         "[DISPLAY] first AirPlay frame selected by OBS graphics thread %.2fms after queue (%ux%u)",
         now >= queued_ns ? (now - queued_ns) / 1e6 : 0.0,
         frame->width, frame->height);
    obs_source_release_frame(airplay_source->source, frame);
    g_first_frame_queued_ns.store(0, std::memory_order_release);
}
