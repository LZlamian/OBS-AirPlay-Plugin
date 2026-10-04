#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

// Hardware (VideoToolbox) decoding is on by default. OBS_AIRPLAY_HW_DECODE=0,
// or the file
// ~/Library/Application Support/obs-studio/obs-airplay-software-decode,
// forces software decoding (for troubleshooting).
inline bool hardware_decode_requested()
{
    if (const char* env = std::getenv("OBS_AIRPLAY_HW_DECODE")) {
        return std::strcmp(env, "0") != 0;
    }
    if (const char* home = std::getenv("HOME")) {
        const std::string flag = std::string(home) +
            "/Library/Application Support/obs-studio/obs-airplay-software-decode";
        struct stat st {};
        if (stat(flag.c_str(), &st) == 0) {
            return false;
        }
    }
    return true;
}
