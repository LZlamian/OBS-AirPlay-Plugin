// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

// Settings that belong to the one AirPlay receiver in the process rather
// than to a source: every AirPlay source shows and edits the same values.
// Kept in the plugin's config folder (settings.json) so they are known when
// the receiver starts, before any scene collection is loaded.
struct ModuleSettings {
    std::string server_name = "OBS AirPlay";
    bool follow_sender_clock = false;
    bool log_latency_telemetry = false;
    bool allow_self_signed_media = false;
    // AirPlay video: by default only from the sending device.
    bool accept_airplay_video = true;
    bool media_sender_only = true;
    bool check_for_updates = true;

    bool operator==(const ModuleSettings& other) const;
    bool operator!=(const ModuleSettings& other) const { return !(*this == other); }
};

// The longest advertised name mDNS allows (one DNS label, in UTF-8 bytes).
constexpr size_t kMaxServerNameBytes = 63;

// Loads settings.json. `found` tells whether the file existed (before the
// first save, settings are taken from the first source, as older versions
// stored them per source).
ModuleSettings module_settings_load(bool* found);
void module_settings_save(const ModuleSettings& settings);
