/* This is a minimal obsconfig.h for plugin compilation */
#pragma once

/* OBS Version Information — matches the bundled obs-headers (libobs 32.0.4) */
#define OBS_VERSION "32.0.4"
#define OBS_VERSION_CANONICAL "32.0.4"

/* Platform Detection */
#ifdef __APPLE__
#define __APPLE__ 1
#endif

#ifdef __x86_64__
#define ARCH_X86_64 1
#endif

#ifdef __aarch64__
#define ARCH_ARM64 1
#endif

/* Feature Flags - Conservative defaults for plugin compatibility */
#define ENABLE_SCRIPTING 0
#define ENABLE_WAYLAND 0
#define BUILD_BROWSER 0

/* macOS specific */
#ifdef __APPLE__
#define MACOS_BUNDLEID "com.obsproject.obs-studio"
#endif
