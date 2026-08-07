#pragma once

// AirServer keeps a CoreBluetooth peripheral advertiser alive while its
// receiver is available. macOS privacy checks belong to the containing app,
// so the plugin starts a small bundled helper with its own usage description.
bool start_airplay_ble_helper();
void stop_airplay_ble_helper();
