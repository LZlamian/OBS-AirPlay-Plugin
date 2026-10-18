// SPDX-License-Identifier: GPL-3.0-or-later
#include "module-settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

bool ModuleSettings::operator==(const ModuleSettings& other) const
{
    return server_name == other.server_name &&
           follow_sender_clock == other.follow_sender_clock &&
           log_latency_telemetry == other.log_latency_telemetry &&
           allow_self_signed_media == other.allow_self_signed_media &&
           accept_airplay_video == other.accept_airplay_video &&
           media_sender_only == other.media_sender_only &&
           check_for_updates == other.check_for_updates;
}

ModuleSettings module_settings_load(bool* found)
{
    ModuleSettings settings;
    if (found) *found = false;
    char* path = obs_module_config_path("settings.json");
    if (!path) return settings;
    obs_data_t* data = os_file_exists(path) ? obs_data_create_from_json_file_safe(path, "bak")
                                            : nullptr;
    bfree(path);
    if (!data) return settings;
    if (found) *found = true;
    const char* name = obs_data_get_string(data, "server_name");
    if (name && *name) settings.server_name = name;
    settings.follow_sender_clock = obs_data_get_bool(data, "follow_sender_clock");
    settings.log_latency_telemetry = obs_data_get_bool(data, "log_latency_telemetry");
    settings.allow_self_signed_media = obs_data_get_bool(data, "allow_self_signed_media");
    settings.accept_airplay_video = obs_data_has_user_value(data, "accept_airplay_video") &&
                                    obs_data_get_bool(data, "accept_airplay_video");
    settings.media_sender_only = obs_data_get_bool(data, "media_sender_only");
    settings.check_for_updates = !obs_data_has_user_value(data, "check_for_updates") ||
                                 obs_data_get_bool(data, "check_for_updates");
    obs_data_release(data);
    return settings;
}

void module_settings_save(const ModuleSettings& settings)
{
    char* dir = obs_module_config_path("");
    if (dir) {
        os_mkdirs(dir);
        bfree(dir);
    }
    char* path = obs_module_config_path("settings.json");
    if (!path) return;
    obs_data_t* data = obs_data_create();
    obs_data_set_string(data, "server_name", settings.server_name.c_str());
    obs_data_set_bool(data, "follow_sender_clock", settings.follow_sender_clock);
    obs_data_set_bool(data, "log_latency_telemetry", settings.log_latency_telemetry);
    obs_data_set_bool(data, "allow_self_signed_media", settings.allow_self_signed_media);
    obs_data_set_bool(data, "accept_airplay_video", settings.accept_airplay_video);
    obs_data_set_bool(data, "media_sender_only", settings.media_sender_only);
    obs_data_set_bool(data, "check_for_updates", settings.check_for_updates);
    if (!obs_data_save_json_safe(data, path, "tmp", "bak")) {
        blog(LOG_WARNING, "[AirPlay] could not save %s", path);
    }
    obs_data_release(data);
    bfree(path);
}
