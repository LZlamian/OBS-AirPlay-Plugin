// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

// AirServer keeps a CoreBluetooth peripheral advertiser alive while its
// receiver is available. macOS privacy checks belong to the containing app,
// so the plugin starts a small bundled helper with its own usage description.
bool start_airplay_ble_helper();
void stop_airplay_ble_helper();

// The helper also checks for plugin updates. These are passed to it through
// its preferences domain (com.lzlamian.obs-airplay.discovery), which it
// reads before each check and before showing a prompt.
void set_helper_update_checks_enabled(bool enabled);  // the "Check for updates" option
void set_helper_obs_outputs_active(bool active);      // streaming or recording: no prompts
// The helper's Bluetooth state ("on", "off", "unauthorized", ...), or empty.
std::string read_helper_bluetooth_state();
