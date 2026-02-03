#!/bin/bash

# OBS Installation Detector
# This script helps find where OBS stores its libraries and headers

echo "=== OBS Installation Detector ==="
echo ""

OBS_APP="/Applications/OBS.app"

if [ ! -d "$OBS_APP" ]; then
    echo "ERROR: OBS not found at $OBS_APP"
    echo "Please install OBS Studio from https://obsproject.com"
    exit 1
fi

echo "✓ Found OBS at: $OBS_APP"
echo ""

# Get OBS version
OBS_VERSION=$(defaults read "$OBS_APP/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null || echo "unknown")
echo "OBS Version: $OBS_VERSION"
echo ""

echo "Searching for libobs library..."
echo ""

# Search for libobs in various locations
SEARCH_PATHS=(
    "$OBS_APP/Contents/Frameworks"
    "$OBS_APP/Contents/MacOS"
    "$OBS_APP/Contents/Resources/bin"
)

for path in "${SEARCH_PATHS[@]}"; do
    echo "Checking: $path"
    if [ -d "$path" ]; then
        # Look for any obs-related files
        find "$path" -name "*obs*" -type f 2>/dev/null | while read file; do
            echo "  Found: $(basename $file)"
        done
        
        # Specifically look for libobs
        if [ -f "$path/libobs.dylib" ]; then
            echo "  ✓ FOUND libobs.dylib"
            LIBOBS_PATH="$path/libobs.dylib"
        elif [ -f "$path/libobs.0.dylib" ]; then
            echo "  ✓ FOUND libobs.0.dylib"
            LIBOBS_PATH="$path/libobs.0.dylib"
        fi
    else
        echo "  (directory does not exist)"
    fi
    echo ""
done

echo "Searching for OBS headers..."
echo ""

HEADER_SEARCH_PATHS=(
    "$OBS_APP/Contents/Resources/include"
    "$OBS_APP/Contents/Frameworks/obs.framework/Headers"
    "$OBS_APP/Contents/Headers"
)

for path in "${HEADER_SEARCH_PATHS[@]}"; do
    echo "Checking: $path"
    if [ -d "$path" ]; then
        if [ -f "$path/obs-module.h" ]; then
            echo "  ✓ FOUND obs-module.h"
            HEADER_PATH="$path"
        else
            echo "  (obs-module.h not found)"
        fi
        
        # List some files
        ls "$path" 2>/dev/null | head -5 | while read file; do
            echo "  - $file"
        done
    else
        echo "  (directory does not exist)"
    fi
    echo ""
done

echo "=== Summary ==="
if [ -n "$LIBOBS_PATH" ]; then
    echo "✓ libobs found at: $LIBOBS_PATH"
else
    echo "✗ libobs NOT FOUND"
fi

if [ -n "$HEADER_PATH" ]; then
    echo "✓ Headers found at: $HEADER_PATH"
else
    echo "✗ Headers NOT FOUND"
fi

echo ""

if [ -n "$LIBOBS_PATH" ] && [ -n "$HEADER_PATH" ]; then
    echo "✓ OBS installation looks good!"
    echo ""
    echo "To build the plugin, edit CMakeLists.txt if needed:"
    echo "  OBS_INCLUDE_DIR should be: $HEADER_PATH"
    echo "  LIBOBS_LIB should be: $LIBOBS_PATH"
else
    echo "✗ OBS installation appears incomplete"
    echo ""
    echo "Solutions:"
    echo "1. Reinstall OBS from https://obsproject.com"
    echo "2. Make sure you downloaded the official macOS version"
    echo "3. If you installed via Homebrew, try the official installer instead"
fi
