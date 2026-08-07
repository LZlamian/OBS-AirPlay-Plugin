#include "airplay-ble-helper.hpp"

#include <obs-module.h>

#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
bool g_helper_started = false;
}

bool start_airplay_ble_helper()
{
    if (g_helper_started)
        return true;

    char *resolved_path = obs_module_file("OBS AirPlay Discovery.app");
    if (!resolved_path) {
        blog(LOG_WARNING, "[BLE] Could not resolve the discovery helper path");
        return false;
    }

    const std::string app_path(resolved_path);
    bfree(resolved_path);
    if (access(app_path.c_str(), R_OK) != 0) {
        blog(LOG_WARNING, "[BLE] Discovery helper app is missing: %s",
             app_path.c_str());
        return false;
    }

    const std::string parent_pid = std::to_string(getpid());
    char *const argv[] = {
        const_cast<char *>("/usr/bin/open"),
        const_cast<char *>("-gj"),
        const_cast<char *>("-n"),
        const_cast<char *>(app_path.c_str()),
        const_cast<char *>("--args"),
        const_cast<char *>(parent_pid.c_str()),
        nullptr,
    };

    // LaunchServices must start the .app. Executing its binary directly causes
    // TCC to miss the app's Bluetooth usage description and terminate it.
    pid_t launcher_pid = -1;
    const int result = posix_spawn(&launcher_pid, "/usr/bin/open", nullptr, nullptr,
                                   argv, environ);
    if (result != 0) {
        blog(LOG_WARNING, "[BLE] Failed to launch discovery helper: %s",
             std::strerror(result));
        return false;
    }

    int launcher_status = 0;
    if (waitpid(launcher_pid, &launcher_status, 0) < 0 ||
        !WIFEXITED(launcher_status) || WEXITSTATUS(launcher_status) != 0) {
        blog(LOG_WARNING, "[BLE] LaunchServices could not start the discovery helper");
        return false;
    }

    g_helper_started = true;
    blog(LOG_INFO, "[BLE] Discovery helper launched through LaunchServices");
    return true;
}

void stop_airplay_ble_helper()
{
    if (!g_helper_started)
        return;

    // The helper watches the OBS PID and exits within one second of its parent.
    g_helper_started = false;
    blog(LOG_INFO, "[BLE] Discovery helper will stop with OBS");
}
