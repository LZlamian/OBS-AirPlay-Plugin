#!/bin/bash

# OBS AirPlay Plugin Build Script
# This script automates the build and installation process

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== OBS AirPlay Plugin Build Script ===${NC}\n"

# Check if Homebrew is installed
if ! command -v brew &> /dev/null; then
    echo -e "${RED}Error: Homebrew is not installed${NC}"
    echo "Install it from: https://brew.sh"
    exit 1
fi

# Check if OBS is installed
if [ ! -d "/Applications/OBS.app" ]; then
    echo -e "${RED}Error: OBS Studio is not installed${NC}"
    echo "Download it from: https://obsproject.com"
    exit 1
fi

# Get OBS version
OBS_VERSION=$(defaults read /Applications/OBS.app/Contents/Info.plist CFBundleShortVersionString 2>/dev/null || echo "unknown")
echo -e "${GREEN}Found OBS Studio version: ${OBS_VERSION}${NC}\n"

# Check for required dependencies
echo -e "${YELLOW}Checking dependencies...${NC}"

MISSING_DEPS=()

if ! brew list cmake &> /dev/null; then
    MISSING_DEPS+=("cmake")
fi

if ! brew list pkg-config &> /dev/null; then
    MISSING_DEPS+=("pkg-config")
fi

if ! brew list ffmpeg &> /dev/null; then
    MISSING_DEPS+=("ffmpeg")
fi

if ! brew list libplist &> /dev/null; then
    MISSING_DEPS+=("libplist")
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo -e "${YELLOW}Installing missing dependencies: ${MISSING_DEPS[*]}${NC}"
    brew install "${MISSING_DEPS[@]}"
fi

echo -e "${GREEN}All dependencies satisfied!${NC}\n"

# Create build directory
echo -e "${YELLOW}Creating build directory...${NC}"
rm -rf build
mkdir -p build
cd build

# Configure CMake
echo -e "${YELLOW}Configuring CMake...${NC}"
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"

# Build
echo -e "${YELLOW}Building plugin...${NC}"
cmake --build . --config Release -j$(sysctl -n hw.ncpu)

echo -e "${GREEN}Build complete!${NC}\n"

# Check if plugin was built
if [ ! -d "obs-airplay.plugin" ]; then
    echo -e "${RED}Error: Plugin bundle not found${NC}"
    exit 1
fi

# Install plugin
echo -e "${YELLOW}Installing plugin...${NC}"
PLUGIN_DIR="$HOME/Library/Application Support/obs-studio/plugins"
mkdir -p "$PLUGIN_DIR"

# Remove old version if exists
if [ -d "$PLUGIN_DIR/obs-airplay.plugin" ]; then
    echo -e "${YELLOW}Removing old version...${NC}"
    rm -rf "$PLUGIN_DIR/obs-airplay.plugin"
fi

# Copy new version
cp -r obs-airplay.plugin "$PLUGIN_DIR/"

echo -e "${GREEN}Plugin installed successfully!${NC}\n"

echo ""
echo -e "${GREEN}=== Installation Complete ===${NC}"
echo ""
echo "Next steps:"
echo "1. ${YELLOW}Restart OBS Studio${NC}"
echo "2. ${YELLOW}Add an 'AirPlay' source in OBS${NC}"
echo "3. ${YELLOW}Open Control Center on your iOS device${NC}"
echo "4. ${YELLOW}Tap 'Screen Mirroring'${NC}"
echo "5. ${YELLOW}Select 'OBS AirPlay'${NC}"
echo ""
echo "Troubleshooting:"
echo "- Check OBS logs: Help -> Log Files -> View Current Log"
echo "- Verify mDNS: ${GREEN}dns-sd -B _airplay._tcp${NC}"
echo "- Check ports: ${GREEN}lsof -i :7000${NC} and ${GREEN}lsof -i :5000${NC}"
echo ""
echo -e "${GREEN}Happy streaming!${NC}"
