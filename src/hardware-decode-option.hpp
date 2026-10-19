// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>

// Troubleshooting switches are flag files in the plugin's OBS config folder
// (obs_module_config_path, so portable OBS installs work), set at module
// load. The folder used before v2.6.0, ~/Library/Application
// Support/obs-studio/, is still honoured.
inline std::mutex g_flag_dir_mutex;
inline std::string g_flag_dir;

inline void set_flag_file_directory(const std::string& directory)
{
    std::lock_guard<std::mutex> lock(g_flag_dir_mutex);
    g_flag_dir = directory;
}

inline bool flag_file_exists(const char* name)
{
    std::string directory;
    {
        std::lock_guard<std::mutex> lock(g_flag_dir_mutex);
        directory = g_flag_dir;
    }
    struct stat st {};
    if (!directory.empty()) {
        if (directory.back() != '/') directory += '/';
        if (stat((directory + name).c_str(), &st) == 0) return true;
    }
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        const std::string legacy =
            std::string(home) + "/Library/Application Support/obs-studio/" + name;
        if (stat(legacy.c_str(), &st) == 0) return true;
    }
#endif
    return false;
}

// Hardware (VideoToolbox, D3D11VA on Windows) decoding is on by default. OBS_AIRPLAY_HW_DECODE=0,
// or a file named obs-airplay-software-decode in the plugin's config folder,
// forces software decoding (for troubleshooting). Read when a decoder is
// created: it takes effect for the next stream; restart OBS to be sure.
inline bool hardware_decode_requested()
{
    if (const char* env = std::getenv("OBS_AIRPLAY_HW_DECODE")) {
        return std::strcmp(env, "0") != 0;
    }
    return !flag_file_exists("obs-airplay-software-decode");
}
