cmake_minimum_required(VERSION 3.16...3.26)

project(obs-airplay VERSION 1.2.0)

set(PLUGIN_AUTHOR "Your Name")
set(MACOS_BUNDLEID "com.yourname.obs-airplay")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

# Find OBS Studio installation
if(APPLE)
    # Try to find OBS in common locations
    set(OBS_BUNDLE_PATH "/Applications/OBS.app" CACHE PATH "Path to OBS.app bundle")
    
    if(NOT EXISTS "${OBS_BUNDLE_PATH}")
        message(FATAL_ERROR "OBS not found at ${OBS_BUNDLE_PATH}. Please install OBS or set OBS_BUNDLE_PATH.")
    endif()
    
    # Get OBS version
    execute_process(
        COMMAND defaults read "${OBS_BUNDLE_PATH}/Contents/Info.plist" CFBundleShortVersionString
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
        "${CMAKE_SOURCE_DIR}/obs-headers"
    )
    
    foreach(DIR ${POSSIBLE_INCLUDE_DIRS})
        if(EXISTS "${DIR}/obs-module.h")
            set(OBS_INCLUDE_DIR "${DIR}")
            break()
        endif()
    endforeach()
    
    # If headers not found, download them
    if(NOT OBS_INCLUDE_DIR)
        message(STATUS "OBS headers not found in installation. Downloading from GitHub...")
        
        set(OBS_HEADERS_DIR "${CMAKE_SOURCE_DIR}/obs-headers")
        
        if(NOT EXISTS "${OBS_HEADERS_DIR}")
            # Download OBS headers matching the installed version
            set(OBS_REPO_URL "https://github.com/obsproject/obs-studio/archive/refs/tags/${OBS_VERSION}.tar.gz")
            set(OBS_DOWNLOAD_FILE "${CMAKE_BINARY_DIR}/obs-studio.tar.gz")
            
            message(STATUS "Downloading OBS ${OBS_VERSION} headers...")
            file(DOWNLOAD 
                "${OBS_REPO_URL}"
                "${OBS_DOWNLOAD_FILE}"
                STATUS DOWNLOAD_STATUS
                TIMEOUT 60
            )
            
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                # Try without patch version (e.g., 30.0 instead of 30.0.0)
                string(REGEX REPLACE "\\.[0-9]+$" "" OBS_VERSION_SHORT "${OBS_VERSION}")
                set(OBS_REPO_URL "https://github.com/obsproject/obs-studio/archive/refs/tags/${OBS_VERSION_SHORT}.tar.gz")
                message(STATUS "Trying alternative URL: ${OBS_REPO_URL}")
                
                file(DOWNLOAD 
                    "${OBS_REPO_URL}"
                    "${OBS_DOWNLOAD_FILE}"
                    STATUS DOWNLOAD_STATUS
                    TIMEOUT 60
                )
                
                list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
                if(NOT STATUS_CODE EQUAL 0)
                    message(FATAL_ERROR "Failed to download OBS headers. Please install OBS development files or clone obs-studio source.")
                endif()
            endif()
            
            # Extract headers
            message(STATUS "Extracting headers...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E tar xzf "${OBS_DOWNLOAD_FILE}"
                WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                RESULT_VARIABLE EXTRACT_RESULT
            )
            
            if(NOT EXTRACT_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to extract OBS headers")
            endif()
            
            # Find extracted directory
            file(GLOB EXTRACTED_DIR "${CMAKE_BINARY_DIR}/obs-studio-*")
            
            # Copy headers to a known location
            file(MAKE_DIRECTORY "${OBS_HEADERS_DIR}")
            file(COPY "${EXTRACTED_DIR}/libobs" DESTINATION "${OBS_HEADERS_DIR}")
            
            # Download and extract simde separately (it's a submodule in OBS)
            message(STATUS "Downloading simde library...")
            set(SIMDE_URL "https://github.com/simd-everywhere/simde/archive/refs/heads/master.tar.gz")
            set(SIMDE_DOWNLOAD "${CMAKE_BINARY_DIR}/simde.tar.gz")
            
            file(DOWNLOAD "${SIMDE_URL}" "${SIMDE_DOWNLOAD}" STATUS SIMDE_STATUS TIMEOUT 30)
            list(GET SIMDE_STATUS 0 SIMDE_CODE)
            
            if(SIMDE_CODE EQUAL 0)
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E tar xzf "${SIMDE_DOWNLOAD}"
                    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                )
                
                file(GLOB SIMDE_EXTRACTED "${CMAKE_BINARY_DIR}/simde-*")
                if(SIMDE_EXTRACTED)
                    file(COPY "${SIMDE_EXTRACTED}/" DESTINATION "${OBS_HEADERS_DIR}/simde")
                    file(REMOVE_RECURSE "${SIMDE_EXTRACTED}")
                    message(STATUS "Simde library downloaded successfully")
                endif()
                
                file(REMOVE "${SIMDE_DOWNLOAD}")
            else()
                message(WARNING "Failed to download simde library, build may fail")
            endif()
            
            # Clean up OBS archive
            file(REMOVE_RECURSE "${EXTRACTED_DIR}")
            file(REMOVE "${OBS_DOWNLOAD_FILE}")
        endif()
        
        set(OBS_INCLUDE_DIR "${OBS_HEADERS_DIR}/libobs")
        
        if(NOT EXISTS "${OBS_INCLUDE_DIR}/obs-module.h")
            message(FATAL_ERROR "Failed to set up OBS headers at ${OBS_INCLUDE_DIR}")
        endif()
        
        message(STATUS "Using downloaded headers at: ${OBS_INCLUDE_DIR}")
    endif()
    
    # Always add simde to include path if it exists in our headers directory
    if(EXISTS "${CMAKE_SOURCE_DIR}/obs-headers/simde")
        include_directories("${CMAKE_SOURCE_DIR}/obs-headers/simde")
        message(STATUS "Added simde include directory: ${CMAKE_SOURCE_DIR}/obs-headers/simde")
    else()
        message(WARNING "Simde headers not found at ${CMAKE_SOURCE_DIR}/obs-headers/simde")
    endif()
    
    # Copy obsconfig.h to the headers directory if it doesn't exist
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/obs-headers/libobs/obsconfig.h")
        if(EXISTS "${CMAKE_SOURCE_DIR}/obsconfig.h")
            file(COPY "${CMAKE_SOURCE_DIR}/obsconfig.h" 
                 DESTINATION "${CMAKE_SOURCE_DIR}/obs-headers/libobs/")
            message(STATUS "Copied obsconfig.h to headers directory")
        else()
            message(WARNING "obsconfig.h not found - creating minimal version")
            # Create a minimal obsconfig.h
            file(WRITE "${CMAKE_SOURCE_DIR}/obs-headers/libobs/obsconfig.h"
                "#pragma once\n"
                "#define OBS_VERSION \"30.0.0\"\n"
                "#define OBS_VERSION_CANONICAL \"30.0.0\"\n"
            )
        endif()
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
    libavcodec 
    libavformat 
    libavutil 
    libswscale 
    libswresample
)

pkg_check_modules(LIBPLIST REQUIRED libplist-2.0)

# OpenSSL for AirPlay encryption
find_package(OpenSSL REQUIRED)

# FDK-AAC is optional but recommended
pkg_check_modules(FDK_AAC fdk-aac)

if(NOT FDK_AAC_FOUND)
    message(WARNING "FDK-AAC not found. AAC decoding may be limited. Install with: brew install fdk-aac")
endif()

# Add UxPlay as subdirectory
add_subdirectory(uxplay/lib)

# Plugin source files
set(obs-airplay_SOURCES
    src/plugin-main.cpp
    src/airplay-source.cpp
    src/airplay-server.cpp
    src/uxplay-integration.cpp
    src/mdns-publisher.cpp
    src/h264-decoder.cpp
    src/audio-decoder.cpp
)

set(obs-airplay_HEADERS
    src/airplay-source.hpp
    src/airplay-server.hpp
    src/uxplay-integration.hpp
    src/mdns-publisher.hpp
    src/h264-decoder.hpp
    src/audio-decoder.hpp
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
    ${LIBPLIST_INCLUDE_DIRS}
    ${FDK_AAC_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(obs-airplay PRIVATE
    ${LIBOBS_LIB}
    ${FFMPEG_LIBRARIES}
    ${LIBPLIST_LIBRARIES}
    ${FDK_AAC_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    airplay  # UxPlay library
)

# Add library directories
target_link_directories(obs-airplay PRIVATE
    ${FFMPEG_LIBRARY_DIRS}
    ${LIBPLIST_LIBRARY_DIRS}
    ${FDK_AAC_LIBRARY_DIRS}
)

# macOS-specific settings
if(APPLE)
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
    
    if(NOT SYSTEM_LIBRARY)
        message(FATAL_ERROR "System framework not found")
    endif()
    
    message(STATUS "System: ${SYSTEM_LIBRARY}")
    message(STATUS "CoreFoundation: ${CORE_FOUNDATION}")
    message(STATUS "Cocoa: ${COCOA_LIBRARY}")
    
    # Link frameworks - dns_sd is provided by System.framework
    target_link_libraries(obs-airplay PRIVATE 
        ${SYSTEM_LIBRARY}
        ${CORE_FOUNDATION}
        ${COCOA_LIBRARY}
    )
    
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
        
        # Copy Info.plist
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/Info.plist" "${PLUGIN_BUNDLE_DIR}/Contents/Info.plist"
        
        COMMENT "Creating plugin bundle structure"
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
