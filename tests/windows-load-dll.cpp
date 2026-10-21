// SPDX-License-Identifier: GPL-3.0-or-later
// Windows only: loads the finished obs-airplay.dll as OBS does, with OBS's
// bin\64bit searched for obs.dll, then unloads it. No other test loads the
// DLL itself (they link UxPlay, FFmpeg and OpenSSL into their own
// executables), so this is what finds a static initialiser that crashes, a
// static runtime failing at DLL load or unload, or an import missing at the
// symbol level. Checks every export libobs looks up and the libobs version
// the module reports.
//   obs-airplay-load-dll PLUGIN_DLL OBS_BIN_DIR
#include <windows.h>

#include <obs-config.h>

#include <cstdint>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PLUGIN_DLL OBS_BIN_DIR\n", argv[0]);
        return 2;
    }
    if (!SetDllDirectoryA(argv[2])) {
        std::printf("load-dll: SetDllDirectory failed (%lu)\n", GetLastError());
        return 1;
    }
    HMODULE module = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        std::printf("load-dll: LoadLibrary(%s) failed (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    int failures = 0;
    const char* const exports[] = {
        "obs_module_load",       "obs_module_unload",      "obs_module_set_pointer",
        "obs_module_ver",        "obs_module_set_locale",  "obs_module_free_locale",
        "obs_module_get_string",
    };
    for (const char* name : exports) {
        if (!GetProcAddress(module, name)) {
            std::printf("load-dll: missing export %s\n", name);
            ++failures;
        }
    }
    using version_function = uint32_t (*)(void);
    const auto version = reinterpret_cast<version_function>(
        reinterpret_cast<void*>(GetProcAddress(module, "obs_module_ver")));
    const uint32_t reported = version ? version() : 0;
    if (reported != LIBOBS_API_VER) {
        std::printf("load-dll: obs_module_ver %u.%u.%u, expected %u.%u.%u\n", reported >> 24,
                    (reported >> 16) & 0xff, reported & 0xffff, LIBOBS_API_MAJOR_VER,
                    LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER);
        ++failures;
    }
    if (!FreeLibrary(module)) {
        std::printf("load-dll: FreeLibrary failed (%lu)\n", GetLastError());
        ++failures;
    }
    std::printf("load-dll: libobs %u.%u.%u -> %s\n", reported >> 24, (reported >> 16) & 0xff,
                reported & 0xffff, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
