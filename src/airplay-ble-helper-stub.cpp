// SPDX-License-Identifier: GPL-3.0-or-later
// Platforms without the macOS discovery helper (Windows): no Bluetooth LE
// advertisement and no self-updater. Senders still find the receiver over
// Bonjour (mDNS), which is all UxPlay uses on any platform.
#include "airplay-ble-helper.hpp"

bool start_airplay_ble_helper()
{
    return false;
}

void stop_airplay_ble_helper() {}

void set_helper_update_checks_enabled(bool) {}

void set_helper_obs_outputs_active(bool) {}

std::string read_helper_bluetooth_state()
{
    return {};
}
