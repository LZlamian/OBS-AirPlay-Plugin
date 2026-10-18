// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay-source.hpp"
#include "airplay-server.hpp"
#include "uxplay-integration.hpp"
#include "media-player.hpp"
#include "module-settings.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>

extern std::shared_ptr<UxPlayIntegration> get_uxplay_integration();

static std::atomic<uint64_t> g_first_frame_queued_ns{0};

AirPlaySource::AirPlaySource(obs_source_t* src)
    : source(src)
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

    // The receiver's settings are shared by all AirPlay sources: this one
    // shows them (or, the first time, provides them).
    adopt_or_show_module_settings(settings, source);

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
    obs_data_set_default_bool(settings, "follow_sender_clock", false);
    obs_data_set_default_bool(settings, "allow_self_signed_media", false);
    obs_data_set_default_string(settings, "airplay_video", "sender");
    obs_data_set_default_bool(settings, "check_for_updates", true);
    obs_data_set_default_bool(settings, "show_advanced", false);
}

// Shows or hides the Advanced group (OBS has no collapsible section).
static bool show_advanced_modified(obs_properties_t* props, obs_property_t* property,
                                   obs_data_t* settings)
{
    UNUSED_PARAMETER(property);
    obs_property_set_visible(obs_properties_get(props, "advanced"),
                             obs_data_get_bool(settings, "show_advanced"));
    return true;
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
    UNUSED_PARAMETER(data);

    obs_properties_t* props = obs_properties_create();

    obs_property_t* name = obs_properties_add_text(props, "server_name",
        obs_module_text("AirPlay.ServerName"),
        OBS_TEXT_DEFAULT);
    obs_property_set_long_description(name, obs_module_text("AirPlay.ServerNameTooltip"));

    obs_properties_add_button(props, "reset_server_name",
        obs_module_text("AirPlay.ResetServerName"),
        reset_server_name_clicked);

    obs_properties_add_text(props, "info", obs_module_text("AirPlay.Info"), OBS_TEXT_INFO);
    
    // Live state; the panel is refreshed when it changes.
    auto uxplay = get_uxplay_integration();
    std::string server_info;
    if (uxplay && uxplay->isRunning()) {
        char line[256];
        snprintf(line, sizeof(line), obs_module_text("AirPlay.Status"),
                 uxplay->connectionState().c_str(), static_cast<int>(uxplay->getActualPort()));
        server_info = line;
    } else {
        server_info = obs_module_text("AirPlay.Status.NotRunning");
    }
    const std::string error = uxplay ? uxplay->lastError() : std::string();
    if (!error.empty()) server_info += "\n" + error;
    else if (!uxplay || !uxplay->isRunning()) {
        server_info += std::string("\n") + obs_module_text("AirPlay.Status.SeeLog");
    }
    const std::string name_error = get_server_name_error();
    if (!name_error.empty()) server_info += "\n" + name_error;
    const std::string refusal = uxplay ? uxplay->lastRefusal() : std::string();
    if (!refusal.empty()) {
        char line[512];
        snprintf(line, sizeof(line), obs_module_text("AirPlay.Status.Refused"), refusal.c_str());
        server_info += std::string("\n") + line;
    }
    obs_property_t* status = obs_properties_add_text(props, "server_info", server_info.c_str(),
                                                     OBS_TEXT_INFO);
    if (!error.empty() || !name_error.empty() || !refusal.empty()) {
        obs_property_text_set_info_type(status, OBS_TEXT_INFO_WARNING);
    }

    // Everything below the status lives in an Advanced group, shown when
    // "Show advanced settings" is ticked.
    obs_property_t* show_advanced = obs_properties_add_bool(props, "show_advanced",
        obs_module_text("AirPlay.ShowAdvanced"));
    obs_property_set_modified_callback(show_advanced, show_advanced_modified);

    obs_properties_t* advanced = obs_properties_create();
    obs_property_t* follow = obs_properties_add_bool(advanced, "follow_sender_clock",
        obs_module_text("AirPlay.FollowSenderClock"));
    obs_property_set_long_description(follow, obs_module_text("AirPlay.FollowSenderClock.Tooltip"));

    obs_property_t* self_signed = obs_properties_add_bool(advanced, "allow_self_signed_media",
        obs_module_text("AirPlay.AllowSelfSigned"));
    obs_property_set_long_description(self_signed, obs_module_text("AirPlay.AllowSelfSigned.Tooltip"));

    obs_property_t* telemetry = obs_properties_add_bool(advanced, "log_latency_telemetry",
        obs_module_text("AirPlay.LatencyTelemetry"));
    obs_property_set_long_description(telemetry, obs_module_text("AirPlay.LatencyTelemetry.Tooltip"));

    obs_property_t* updates = obs_properties_add_bool(advanced, "check_for_updates",
        obs_module_text("AirPlay.CheckForUpdates"));
    obs_property_set_long_description(updates, obs_module_text("AirPlay.CheckForUpdates.Tooltip"));

    // One choice with three answers (the settings behind it are "accept
    // AirPlay video" and "only from the sending device").
    obs_property_t* video = obs_properties_add_list(advanced, "airplay_video",
        obs_module_text("AirPlay.Video"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    // Least to most permissive; "sender" is the default. Last in the list:
    // its tooltip is long, and below it there is no help icon to cover.
    obs_property_list_add_string(video, obs_module_text("AirPlay.Video.Off"), "off");
    obs_property_list_add_string(video, obs_module_text("AirPlay.Video.SenderOnly"), "sender");
    obs_property_list_add_string(video, obs_module_text("AirPlay.Video.Anywhere"), "anywhere");
    obs_property_set_long_description(video, obs_module_text("AirPlay.Video.Tooltip"));

    obs_properties_add_group(props, "advanced", obs_module_text("AirPlay.Advanced"),
                             OBS_GROUP_NORMAL, advanced);

    char version[64];
    snprintf(version, sizeof(version), obs_module_text("AirPlay.Version"), PLUGIN_VERSION);
    obs_properties_add_text(props, "plugin_version", version, OBS_TEXT_INFO);

    return props;
}

void airplay_source_update(void* data, obs_data_t* settings)
{
    auto* airplay_src = static_cast<AirPlaySource*>(data);
    // The name goes through update_server_name's debounce; the other values
    // apply at once. All AirPlay sources then show the same values.
    set_module_settings(read_source_settings(settings),
                        airplay_src ? airplay_src->source : nullptr);
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

    // Only observe: obs_source_get_frame() would take the source's current
    // async frame away from the renderer before it is uploaded, so a single
    // frame (a paused item, a still) would never be drawn.
    const uint32_t width = obs_source_get_width(airplay_source->source);
    const uint32_t height = obs_source_get_height(airplay_source->source);
    if (!width || !height) {
        return;
    }

    const uint64_t now = os_gettime_ns();
    blog(LOG_INFO,
         "[DISPLAY] first AirPlay frame reached the OBS video tick %.2fms after queue (%ux%u)",
         now >= queued_ns ? (now - queued_ns) / 1e6 : 0.0, width, height);
    g_first_frame_queued_ns.store(0, std::memory_order_release);
}
