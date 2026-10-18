cmake_minimum_required(VERSION 3.16...3.26)

if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
        "Oldest macOS version supported by the packaged plugin")
endif()

project(obs-airplay VERSION 2.6.0)

# A plain `cmake ..` used to build without optimisation.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No build type given; using Release")
endif()

# The only place the version is set: both bundles' Info.plists are generated
# from it, and scripts/package-macos.sh reads it and checks the result.
set(OBS_AIRPLAY_COPYRIGHT "Copyright © 2025-2026 LZlamian and contributors. GPL-3.0-or-later.")
configure_file("${CMAKE_SOURCE_DIR}/Info.plist.in" "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/airplay-ble-discovery-Info.plist.in"
               "${CMAKE_BINARY_DIR}/airplay-ble-discovery-Info.plist" @ONLY)

set(PLUGIN_AUTHOR "LZlamian")
set(MACOS_BUNDLEID "com.lzlamian.obs-airplay")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

option(OBS_AIRPLAY_FAST_CONNECT
    "Use AirServer-style initial GET /info with a direct pair-verify fallback"
    ON)

# UxPlay otherwise enables -march=native on Intel, which can produce a package
# that crashes on an older x86_64 CPU than the build machine.
set(NO_MARCH_NATIVE ON CACHE BOOL
    "Build UxPlay without host-specific CPU instructions")

# Find OBS Studio installation
if(APPLE)
    # Try to find OBS in common locations
    set(OBS_BUNDLE_PATH "/Applications/OBS.app" CACHE PATH "Path to OBS.app bundle")
    
    if(NOT EXISTS "${OBS_BUNDLE_PATH}")
        message(FATAL_ERROR "OBS not found at ${OBS_BUNDLE_PATH}. Please install OBS or set OBS_BUNDLE_PATH.")
    endif()
    
    # Get OBS version
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString"
                "${OBS_BUNDLE_PATH}/Contents/Info.plist"
        OUTPUT_VARIABLE OBS_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    
    if(NOT OBS_VERSION)
        set(OBS_VERSION "30.0.0")
        message(WARNING "Could not detect OBS version, assuming ${OBS_VERSION}")
    endif()
    
    message(STATUS "Detected OBS version: ${OBS_VERSION}")
    
    # Try multiple possible locations for headers
    set(POSSIBLE_INCLUDE_DIRS
        "${OBS_BUNDLE_PATH}/Contents/Resources/include"
        "${OBS_BUNDLE_PATH}/Contents/Frameworks/obs.framework/Headers"
        "${OBS_BUNDLE_PATH}/Contents/Headers"
    )
    
    foreach(DIR ${POSSIBLE_INCLUDE_DIRS})
        if(EXISTS "${DIR}/obs-module.h")
            set(OBS_INCLUDE_DIR "${DIR}")
            break()
        endif()
    endforeach()
    
    # The headers are vendored (obs-headers/PROVENANCE.md); configure never
    # downloads anything or writes into the source tree.
    if(NOT OBS_INCLUDE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/obs-headers/libobs/obs-module.h")
        set(OBS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/obs-headers/libobs")
    endif()
    if(NOT OBS_INCLUDE_DIR)
        message(FATAL_ERROR
            "OBS headers not found: obs-headers/libobs is missing from the checkout.")
    endif()
    message(STATUS "OBS headers: ${OBS_INCLUDE_DIR}")
    if(EXISTS "${CMAKE_SOURCE_DIR}/obs-headers/simde/simde")
        include_directories("${CMAKE_SOURCE_DIR}/obs-headers/simde")
    else()
        message(FATAL_ERROR "SIMDe headers not found at obs-headers/simde")
    endif()

    # Try multiple possible locations for libraries
    set(POSSIBLE_LIB_DIRS
        "${OBS_BUNDLE_PATH}/Contents/Frameworks"
        "${OBS_BUNDLE_PATH}/Contents/MacOS"
        "${OBS_BUNDLE_PATH}/Contents/Resources/bin"
    )
    
    # Try to find libobs as a framework first, then as a dylib
    find_library(LIBOBS_LIB 
        NAMES obs libobs obs.framework
        PATHS ${POSSIBLE_LIB_DIRS}
        NO_DEFAULT_PATH
    )
    
    # If not found as framework, try direct dylib
    if(NOT LIBOBS_LIB)
        foreach(DIR ${POSSIBLE_LIB_DIRS})
            if(EXISTS "${DIR}/libobs.dylib")
                set(LIBOBS_LIB "${DIR}/libobs.dylib")
                set(OBS_LIB_DIR "${DIR}")
                break()
            elseif(EXISTS "${DIR}/libobs.0.dylib")
                set(LIBOBS_LIB "${DIR}/libobs.0.dylib")
                set(OBS_LIB_DIR "${DIR}")
                break()
            endif()
        endforeach()
    endif()
    
    if(NOT LIBOBS_LIB)
        message(STATUS "Searched for libobs in:")
        foreach(DIR ${POSSIBLE_LIB_DIRS})
            message(STATUS "  - ${DIR}")
            # List what's actually in each directory
            if(EXISTS "${DIR}")
                file(GLOB DIR_CONTENTS "${DIR}/*obs*")
                foreach(FILE ${DIR_CONTENTS})
                    message(STATUS "    Found: ${FILE}")
                endforeach()
            endif()
        endforeach()
        message(FATAL_ERROR "Could not find libobs library. Please check the above paths.")
    endif()
    
    # Add OBS include directory
    include_directories(${OBS_INCLUDE_DIR})
    
    message(STATUS "=== OBS Configuration ===")
    message(STATUS "OBS Bundle: ${OBS_BUNDLE_PATH}")
    message(STATUS "OBS Version: ${OBS_VERSION}")
    message(STATUS "OBS Headers: ${OBS_INCLUDE_DIR}")
    message(STATUS "OBS Library: ${LIBOBS_LIB}")
    message(STATUS "========================")
endif()

# Find required libraries
find_package(PkgConfig REQUIRED)

# Find FFmpeg components using pkg-config
pkg_check_modules(FFMPEG REQUIRED 
    libavformat
    libavcodec 
    libavutil 
    libswscale 
    libswresample
)

# Add UxPlay as subdirectory
# The UxPlay fork is a git submodule; a plain clone or a release source
# archive does not include it.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/uxplay/lib/CMakeLists.txt")
    message(FATAL_ERROR
        "The uxplay submodule is missing. Run:\n"
        "  git submodule update --init --recursive\n"
        "(or clone with: git clone --recursive https://github.com/LZlamian/OBS-AirPlay-Plugin.git)")
endif()
add_subdirectory(uxplay/lib)

# Plugin source files
set(obs-airplay_SOURCES
    src/plugin-main.cpp
    src/airplay-ble-helper.cpp
    src/airplay-source.cpp
    src/airplay-server.cpp
    src/uxplay-integration.cpp
    src/h264-decoder.cpp
    src/audio-decoder.cpp
    src/hls-playlist.cpp
    src/media-player.cpp
    src/media-url-policy.cpp
    src/module-settings.cpp
)

set(obs-airplay_HEADERS
    src/airplay-ble-helper.hpp
    src/airplay-source.hpp
    src/airplay-server.hpp
    src/uxplay-integration.hpp
    src/h264-decoder.hpp
    src/audio-decoder.hpp
    src/hls-playlist.hpp
    src/media-player.hpp
)

# Create the plugin library
add_library(obs-airplay MODULE
    ${obs-airplay_SOURCES}
    ${obs-airplay_HEADERS}
)

# Include directories
target_include_directories(obs-airplay PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
)

target_compile_definitions(obs-airplay PRIVATE
    PLUGIN_VERSION="${CMAKE_PROJECT_VERSION}"
)

if(OBS_AIRPLAY_FAST_CONNECT)
    target_compile_definitions(obs-airplay PRIVATE OBS_AIRPLAY_FAST_CONNECT=1)
else()
    target_compile_definitions(obs-airplay PRIVATE OBS_AIRPLAY_FAST_CONNECT=0)
endif()

# Link libraries
target_link_libraries(obs-airplay PRIVATE
    ${LIBOBS_LIB}
    ${FFMPEG_LIBRARIES}
    airplay  # UxPlay library
)

# Add library directories
target_link_directories(obs-airplay PRIVATE
    ${FFMPEG_LIBRARY_DIRS}
)

# macOS-specific settings
if(APPLE)
    enable_language(OBJCXX)

    # Get the macOS SDK path
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    
    if(MACOS_SDK_PATH)
        message(STATUS "macOS SDK path: ${MACOS_SDK_PATH}")
        include_directories("${MACOS_SDK_PATH}/usr/include")
    endif()
    
    # On modern macOS, dns_sd is part of the System framework
    # We need to link against System.framework which contains it
    find_library(SYSTEM_LIBRARY System REQUIRED)
    find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(CORE_BLUETOOTH_LIBRARY CoreBluetooth REQUIRED)
    find_library(USER_NOTIFICATIONS_LIBRARY UserNotifications REQUIRED)

    # Update signatures are checked with CryptoKit, which has only a Swift
    # API. The one Swift file is compiled to an object here so any generator
    # (including Unix Makefiles) can link it; the Swift runtime is part of
    # macOS 12.
    if(CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES OBS_AIRPLAY_ARCH_COUNT)
        if(OBS_AIRPLAY_ARCH_COUNT GREATER 1)
            message(FATAL_ERROR "Build one architecture at a time (CMAKE_OSX_ARCHITECTURES)")
        endif()
        set(OBS_AIRPLAY_SWIFT_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(OBS_AIRPLAY_SWIFT_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    find_program(XCRUN xcrun REQUIRED)
    set(OBS_AIRPLAY_SIGNATURE_OBJECT "${CMAKE_BINARY_DIR}/update-signature.o")
    add_custom_command(
        OUTPUT "${OBS_AIRPLAY_SIGNATURE_OBJECT}"
        COMMAND ${XCRUN} swiftc -parse-as-library -O -whole-module-optimization
                -target "${OBS_AIRPLAY_SWIFT_ARCH}-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}"
                -module-name OBSAirPlayUpdateSignature
                -emit-object "${CMAKE_SOURCE_DIR}/src/update-signature.swift"
                -o "${OBS_AIRPLAY_SIGNATURE_OBJECT}"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/update-signature.swift"
        COMMENT "Compiling update-signature.swift"
        VERBATIM
    )
    set_source_files_properties("${OBS_AIRPLAY_SIGNATURE_OBJECT}" PROPERTIES
        EXTERNAL_OBJECT TRUE GENERATED TRUE)
    execute_process(COMMAND ${XCRUN} --show-sdk-path
                    OUTPUT_VARIABLE OBS_AIRPLAY_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${XCRUN} -f swiftc
                    OUTPUT_VARIABLE OBS_AIRPLAY_SWIFTC OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(OBS_AIRPLAY_SWIFT_TOOLCHAIN "${OBS_AIRPLAY_SWIFTC}" DIRECTORY)
    set(OBS_AIRPLAY_SWIFT_LINK_OPTIONS
        "-L${OBS_AIRPLAY_SDK_PATH}/usr/lib/swift"
        "-L${OBS_AIRPLAY_SWIFT_TOOLCHAIN}/../lib/swift/macosx"
        "-Wl,-rpath,/usr/lib/swift")

    add_executable(obs-airplay-ble-discovery MACOSX_BUNDLE
        src/airplay-ble-discovery.mm
        src/plugin-updater.mm
        "${OBS_AIRPLAY_SIGNATURE_OBJECT}"
    )
    target_link_options(obs-airplay-ble-discovery PRIVATE ${OBS_AIRPLAY_SWIFT_LINK_OPTIONS})
    target_compile_definitions(obs-airplay-ble-discovery PRIVATE
        PLUGIN_VERSION="${CMAKE_PROJECT_VERSION}"
    )
    target_compile_options(obs-airplay-ble-discovery PRIVATE -fobjc-arc)
    target_link_libraries(obs-airplay-ble-discovery PRIVATE
        ${COCOA_LIBRARY}
        ${CORE_BLUETOOTH_LIBRARY}
        ${USER_NOTIFICATIONS_LIBRARY}
    )
    set_target_properties(obs-airplay-ble-discovery PROPERTIES
        OUTPUT_NAME "OBS AirPlay Discovery"
        MACOSX_BUNDLE_BUNDLE_NAME "OBS AirPlay Discovery"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.lzlamian.obs-airplay.discovery"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/airplay-ble-discovery-Info.plist"
    )
    add_dependencies(obs-airplay obs-airplay-ble-discovery)
    
    if(NOT SYSTEM_LIBRARY)
        message(FATAL_ERROR "System framework not found")
    endif()
    
    message(STATUS "System: ${SYSTEM_LIBRARY}")
    message(STATUS "CoreFoundation: ${CORE_FOUNDATION}")
    message(STATUS "Cocoa: ${COCOA_LIBRARY}")
    
    # Link frameworks - dns_sd is provided by System.framework
    # CoreVideo: the mirror decoder reads VideoToolbox pixel buffers directly.
    find_library(CORE_VIDEO_LIBRARY CoreVideo REQUIRED)
    target_link_libraries(obs-airplay PRIVATE 
        ${SYSTEM_LIBRARY}
        ${CORE_FOUNDATION}
        ${COCOA_LIBRARY}
        ${CORE_VIDEO_LIBRARY}
    )
    
    # Export only what libobs looks up (obs_module_*): the statically
    # linked UxPlay, OpenSSL and libplist symbols stay private, and unused
    # code is stripped.
    target_compile_options(obs-airplay PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    target_link_options(obs-airplay PRIVATE
        -Wl,-dead_strip
        "-Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/src/obs-airplay.exports")
    set_property(TARGET obs-airplay APPEND PROPERTY
        LINK_DEPENDS "${CMAKE_SOURCE_DIR}/src/obs-airplay.exports")

    # Set macOS bundle properties
    set_target_properties(obs-airplay PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        OUTPUT_NAME "obs-airplay"
    )
    
    # Create plugin bundle structure
    set(PLUGIN_BUNDLE_DIR "${CMAKE_BINARY_DIR}/obs-airplay.plugin")
    
    add_custom_command(TARGET obs-airplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_BUNDLE_DIR}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_BUNDLE_DIR}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:obs-airplay>" "${PLUGIN_BUNDLE_DIR}/Contents/MacOS/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:obs-airplay-ble-discovery>"
                "${PLUGIN_BUNDLE_DIR}/Contents/Resources/OBS AirPlay Discovery.app"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE"
                "${PLUGIN_BUNDLE_DIR}/Contents/Resources/LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/uxplay/LICENSE"
                "${PLUGIN_BUNDLE_DIR}/Contents/Resources/LICENSE-UxPlay-GPL-3.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
                "${PLUGIN_BUNDLE_DIR}/Contents/Resources/THIRD_PARTY_NOTICES.md"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/licenses"
                "${PLUGIN_BUNDLE_DIR}/Contents/Resources/licenses"
        
        # Copy Info.plist
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/Info.plist" "${PLUGIN_BUNDLE_DIR}/Contents/Info.plist"
        
        COMMENT "Creating plugin bundle structure"
    )
    
    # The plugin's POST_BUILD step only runs when the plugin itself is linked
    # again; a change to the helper alone must reach the bundle too.
    add_custom_command(TARGET obs-airplay-ble-discovery POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_BUNDLE_DIR}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:obs-airplay-ble-discovery>"
                "${PLUGIN_BUNDLE_DIR}/Contents/Resources/OBS AirPlay Discovery.app"
        COMMENT "Copying the discovery helper into the plugin bundle"
    )

    # Install data files if they exist
    if(EXISTS "${CMAKE_SOURCE_DIR}/data")
        add_custom_command(TARGET obs-airplay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory 
                "${CMAKE_SOURCE_DIR}/data" 
                "${PLUGIN_BUNDLE_DIR}/Contents/Resources"
        )
    endif()
endif()

# Installation
install(DIRECTORY "${CMAKE_BINARY_DIR}/obs-airplay.plugin"
        DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins"
        USE_SOURCE_PERMISSIONS)

# Warnings for this project's own code (UxPlay keeps its upstream flags).
set(OBS_AIRPLAY_WARNING_FLAGS -Wall -Wextra)
foreach(target obs-airplay obs-airplay-ble-discovery)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE ${OBS_AIRPLAY_WARNING_FLAGS})
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    pkg_check_modules(PLIST_TEST REQUIRED libplist-2.0)

    add_executable(obs-airplay-media-smoke
        tests/media-player-smoke.cpp
        src/hls-playlist.cpp
        src/media-player.cpp
        src/media-url-policy.cpp
    )
    target_include_directories(obs-airplay-media-smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${OBS_INCLUDE_DIR}
        ${FFMPEG_INCLUDE_DIRS}
    )
    target_compile_definitions(obs-airplay-media-smoke PRIVATE
        PLUGIN_VERSION="${CMAKE_PROJECT_VERSION}"
    )
    target_link_directories(obs-airplay-media-smoke PRIVATE
        ${FFMPEG_LIBRARY_DIRS}
    )
    target_link_libraries(obs-airplay-media-smoke PRIVATE
        ${FFMPEG_LIBRARIES}
    )
    if(APPLE)
        target_link_libraries(obs-airplay-media-smoke PRIVATE "-framework CoreVideo")
    endif()

    # Mirror decoder: hardware vs software output, size changes, recovery.
    add_executable(obs-airplay-decoder-smoke
        tests/video-decoder-smoke.cpp
        src/h264-decoder.cpp
    )
    target_include_directories(obs-airplay-decoder-smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${OBS_INCLUDE_DIR}
        ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_directories(obs-airplay-decoder-smoke PRIVATE
        ${FFMPEG_LIBRARY_DIRS}
    )
    target_link_libraries(obs-airplay-decoder-smoke PRIVATE
        ${FFMPEG_LIBRARIES}
    )
    if(APPLE)
        target_link_libraries(obs-airplay-decoder-smoke PRIVATE "-framework CoreVideo")
    endif()

    if(APPLE)
        # Update download checks, staging and the swap, on scratch bundles.
        add_executable(obs-airplay-updater-smoke
            tests/updater-smoke.mm
            src/plugin-updater.mm
            "${OBS_AIRPLAY_SIGNATURE_OBJECT}"
        )
        target_link_options(obs-airplay-updater-smoke PRIVATE ${OBS_AIRPLAY_SWIFT_LINK_OPTIONS})
        target_include_directories(obs-airplay-updater-smoke PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_options(obs-airplay-updater-smoke PRIVATE -fobjc-arc)
        target_link_libraries(obs-airplay-updater-smoke PRIVATE "-framework Foundation")
    endif()

    add_executable(obs-airplay-protocol-smoke
        tests/protocol-smoke.cpp
    )

    # HLS playlist reader, media port list, audio decoder.
    add_executable(obs-airplay-unit-tests
        tests/unit-tests.cpp
        src/audio-decoder.cpp
        src/hls-playlist.cpp
        src/media-url-policy.cpp
    )
    target_include_directories(obs-airplay-unit-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${OBS_INCLUDE_DIR}
        ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_directories(obs-airplay-unit-tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(obs-airplay-unit-tests PRIVATE ${FFMPEG_LIBRARIES})

    foreach(target obs-airplay-media-smoke obs-airplay-decoder-smoke obs-airplay-updater-smoke
                   obs-airplay-protocol-smoke obs-airplay-unit-tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE ${OBS_AIRPLAY_WARNING_FLAGS})
        endif()
    endforeach()
    target_include_directories(obs-airplay-protocol-smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/uxplay/lib
        ${PLIST_TEST_INCLUDE_DIRS}
    )
    target_link_libraries(obs-airplay-protocol-smoke PRIVATE
        airplay
    )
    # Fuzz targets for input from the network: HLS playlists, mirror access
    # units, HTTP requests. With libFuzzer's runtime they are libFuzzer
    # binaries; otherwise (Apple's clang) tests/fuzz/fuzz-driver.cpp runs
    # seeded random mutations. Configure with the asan preset so bad
    # inputs are caught.
    option(OBS_AIRPLAY_FUZZ "Build the fuzz targets and register short fuzz runs" OFF)
    if(OBS_AIRPLAY_FUZZ)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
        check_cxx_source_compiles(
            "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
            OBS_AIRPLAY_HAVE_LIBFUZZER)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
        function(obs_airplay_fuzz_target name)
            add_executable(${name} ${ARGN})
            if(OBS_AIRPLAY_HAVE_LIBFUZZER)
                target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
                target_link_options(${name} PRIVATE -fsanitize=fuzzer)
            else()
                target_sources(${name} PRIVATE tests/fuzz/fuzz-driver.cpp)
            endif()
            target_compile_options(${name} PRIVATE ${OBS_AIRPLAY_WARNING_FLAGS})
        endfunction()

        obs_airplay_fuzz_target(obs-airplay-fuzz-hls tests/fuzz/fuzz-hls.cpp src/hls-playlist.cpp)
        target_include_directories(obs-airplay-fuzz-hls PRIVATE ${CMAKE_SOURCE_DIR}/src)

        obs_airplay_fuzz_target(obs-airplay-fuzz-h264 tests/fuzz/fuzz-h264.cpp src/h264-decoder.cpp)
        target_include_directories(obs-airplay-fuzz-h264 PRIVATE
            ${CMAKE_SOURCE_DIR}/src ${OBS_INCLUDE_DIR} ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(obs-airplay-fuzz-h264 PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(obs-airplay-fuzz-h264 PRIVATE ${FFMPEG_LIBRARIES})
        if(APPLE)
            target_link_libraries(obs-airplay-fuzz-h264 PRIVATE "-framework CoreVideo")
        endif()

        obs_airplay_fuzz_target(obs-airplay-fuzz-http-request tests/fuzz/fuzz-http-request.c)
        target_include_directories(obs-airplay-fuzz-http-request PRIVATE ${CMAKE_SOURCE_DIR}/uxplay/lib)
        target_link_libraries(obs-airplay-fuzz-http-request PRIVATE airplay)

        set(FUZZ_SEEDS "${CMAKE_SOURCE_DIR}/tests/fuzz/seeds")
        if(OBS_AIRPLAY_HAVE_LIBFUZZER)
            set(FUZZ_RUNS -runs=20000)
        else()
            set(FUZZ_RUNS -runs=20000 -seed=1)
        endif()
        add_test(NAME fuzz-hls COMMAND obs-airplay-fuzz-hls ${FUZZ_RUNS} "${FUZZ_SEEDS}/master.m3u8")
        add_test(NAME fuzz-http-request COMMAND obs-airplay-fuzz-http-request ${FUZZ_RUNS}
                 "${FUZZ_SEEDS}/play-request.bin" "${FUZZ_SEEDS}/info-request.bin")
        set_tests_properties(fuzz-hls fuzz-http-request PROPERTIES TIMEOUT 300)
    endif()

    # ---- CTest: every smoke, with media generated at test time ----------
    add_test(NAME unit COMMAND obs-airplay-unit-tests)
    add_test(NAME protocol COMMAND obs-airplay-protocol-smoke)
    add_test(NAME protocol-large-blob COMMAND obs-airplay-protocol-smoke --large-blob)

    find_program(OBS_AIRPLAY_FFMPEG ffmpeg)
    set(OBS_AIRPLAY_FIXTURES "${CMAKE_BINARY_DIR}/test-fixtures")
    if(OBS_AIRPLAY_FFMPEG)
        add_test(NAME fixtures
            COMMAND "${CMAKE_SOURCE_DIR}/tests/make-fixtures.sh" "${OBS_AIRPLAY_FFMPEG}"
                    "${OBS_AIRPLAY_FIXTURES}")
        set_tests_properties(fixtures PROPERTIES FIXTURES_SETUP media TIMEOUT 600)

        set(F "file://${OBS_AIRPLAY_FIXTURES}")
        set(OBS_AIRPLAY_MEDIA_TESTS
            "media-play|${F}/clip8.mp4"
            "media-rotated|${F}/rot90.mp4"
            "media-require-audio|--require-audio^${F}/clip8.mp4"
            "media-startup|--startup^${F}/clip.mp4"
            "media-seek-sync|--seek-sync^${F}/clip.mp4"
            "media-seek-sync-preroll|--seek-sync^${F}/clip-g300.mp4"
            "media-scrub-drag|--scrub-drag^${F}/clip.mp4"
            "media-scrub-latency|--scrub-latency^${F}/clip.mp4"
            "media-paused-audio|--paused-audio^${F}/clip.mp4"
            "media-sparse-clip|--sparse-clip^${F}/sparse.mp4"
            "media-follow-clock|--follow-clock^${F}/clip90.mp4"
            "media-follow-clock-short|--follow-clock-short^${F}/still2.mp4"
            "media-follow-clock-quick|--follow-clock-short-quick^${F}/still2.mp4"
            "media-stop-race|--stop-race^${F}/clip.mp4"
            "media-ts-seek|--ts-seek^${F}/clip-ts.ts"
            "media-after-end|--after-end^${F}/clip8.mp4")
        foreach(entry ${OBS_AIRPLAY_MEDIA_TESTS})
            # name|arg^arg (';' would split the list itself)
            string(REPLACE "|" ";" parts "${entry}")
            string(REPLACE "^" ";" parts "${parts}")
            list(GET parts 0 name)
            list(REMOVE_AT parts 0)
            add_test(NAME ${name} COMMAND obs-airplay-media-smoke ${parts})
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED media TIMEOUT 180)
        endforeach()
        add_test(NAME media-url-policy COMMAND obs-airplay-media-smoke --url-policy)

        # Local HTTP(S) servers on fixed ports: one test at a time.
        add_test(NAME media-nested-policy
            COMMAND "${CMAKE_SOURCE_DIR}/tests/with-media-servers.sh" "${OBS_AIRPLAY_FIXTURES}"
                    "$<TARGET_FILE:obs-airplay-media-smoke>" --nested-policy
                    http://127.0.0.1:18901/same.m3u8 http://127.0.0.1:18901/cross.m3u8)
        add_test(NAME media-tls-verify
            COMMAND "${CMAKE_SOURCE_DIR}/tests/with-media-servers.sh" "${OBS_AIRPLAY_FIXTURES}"
                    "$<TARGET_FILE:obs-airplay-media-smoke>" --tls-verify
                    https://localhost:18443/clip.mp4)
        add_test(NAME media-resilient-http
            COMMAND "${CMAKE_SOURCE_DIR}/tests/with-media-servers.sh" "${OBS_AIRPLAY_FIXTURES}"
                    "$<TARGET_FILE:obs-airplay-media-smoke>" --resilient
                    http://127.0.0.1:18903/clip8.mp4)
        set_tests_properties(media-nested-policy media-tls-verify media-resilient-http PROPERTIES
            FIXTURES_REQUIRED media RESOURCE_LOCK media-servers TIMEOUT 120)

        add_test(NAME decoder
            COMMAND obs-airplay-decoder-smoke "${OBS_AIRPLAY_FIXTURES}/clip.mp4"
                    "${OBS_AIRPLAY_FIXTURES}/clip709.mp4")
        add_test(NAME decoder-two-au
            COMMAND obs-airplay-decoder-smoke --two-au "${OBS_AIRPLAY_FIXTURES}/clip709.mp4")
        add_test(NAME decoder-colorspace
            COMMAND obs-airplay-decoder-smoke --colorspace "${OBS_AIRPLAY_FIXTURES}/clip601.mp4"
                    "${OBS_AIRPLAY_FIXTURES}/clip709.mp4")
        set_tests_properties(decoder decoder-two-au decoder-colorspace PROPERTIES
            FIXTURES_REQUIRED media TIMEOUT 300)
        if(OBS_AIRPLAY_FUZZ)
            add_test(NAME fuzz-h264
                COMMAND obs-airplay-fuzz-h264 -runs=3000 "${OBS_AIRPLAY_FIXTURES}/fuzz-h264.bin"
                        "${OBS_AIRPLAY_FIXTURES}/fuzz-hevc.bin")
            set_tests_properties(fuzz-h264 PROPERTIES FIXTURES_REQUIRED media TIMEOUT 600)
        endif()
    else()
        message(WARNING "ffmpeg not found: media and decoder tests are not registered")
    endif()

    if(TARGET obs-airplay-updater-smoke)
        # Test signing keys need OpenSSL 3 (macOS's /usr/bin/openssl is LibreSSL).
        find_program(OBS_AIRPLAY_OPENSSL3 openssl
            PATHS /opt/homebrew/opt/openssl@3/bin /usr/local/opt/openssl@3/bin NO_DEFAULT_PATH)
        add_test(NAME updater
            COMMAND obs-airplay-updater-smoke "${CMAKE_BINARY_DIR}/obs-airplay.plugin"
                    "${CMAKE_BINARY_DIR}/test-scratch")
        if(OBS_AIRPLAY_OPENSSL3)
            set_tests_properties(updater PROPERTIES ENVIRONMENT "OPENSSL=${OBS_AIRPLAY_OPENSSL3}")
        endif()
        set_tests_properties(updater PROPERTIES TIMEOUT 300)
    endif()
endif()
