#!/bin/bash

# OBS AirPlay Plugin Diagnostic Script
# Run this if your plugin isn't working

set +e  # Don't exit on error

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  OBS AirPlay Plugin Diagnostic Tool           ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════╝${NC}"
echo ""

ERRORS=0
WARNINGS=0

# Check 1: OBS Installation
echo -e "${YELLOW}[1/10]${NC} Checking OBS installation..."
if [ -d "/Applications/OBS.app" ]; then
    OBS_VERSION=$(defaults read /Applications/OBS.app/Contents/Info.plist CFBundleShortVersionString 2>/dev/null)
    echo -e "  ${GREEN}✓${NC} OBS found: version ${OBS_VERSION}"
else
    echo -e "  ${RED}✗${NC} OBS not found at /Applications/OBS.app"
    ((ERRORS++))
fi
echo ""

# Check 2: Plugin Installation
echo -e "${YELLOW}[2/10]${NC} Checking plugin installation..."
PLUGIN_PATH="$HOME/Library/Application Support/obs-studio/plugins/obs-airplay.plugin"
if [ -d "$PLUGIN_PATH" ]; then
    echo -e "  ${GREEN}✓${NC} Plugin installed at: $PLUGIN_PATH"
    
    # Check if dylib exists
    if [ -f "$PLUGIN_PATH/Contents/MacOS/obs-airplay" ]; then
        echo -e "  ${GREEN}✓${NC} Binary found"
    else
        echo -e "  ${RED}✗${NC} Binary not found"
        ((ERRORS++))
    fi
else
    echo -e "  ${RED}✗${NC} Plugin not installed"
    ((ERRORS++))
fi
echo ""

# Check 3: Dependencies
echo -e "${YELLOW}[3/10]${NC} Checking dependencies..."

check_lib() {
    if brew list "$1" &> /dev/null; then
        echo -e "  ${GREEN}✓${NC} $1 installed"
    else
        echo -e "  ${RED}✗${NC} $1 missing (install with: brew install $1)"
        ((ERRORS++))
    fi
}

check_lib "ffmpeg"
check_lib "libplist"
check_lib "pkg-config"
echo ""

# Check 4: Network Configuration
echo -e "${YELLOW}[4/10]${NC} Checking network..."

# Get primary network interface
IFACE=$(route -n get default 2>/dev/null | grep interface | awk '{print $2}')
if [ -n "$IFACE" ]; then
    IP=$(ifconfig "$IFACE" | grep "inet " | awk '{print $2}')
    echo -e "  ${GREEN}✓${NC} Interface: $IFACE"
    echo -e "  ${GREEN}✓${NC} IP Address: $IP"
    
    # Check if on private network
    if [[ $IP == 192.168.* ]] || [[ $IP == 10.* ]] || [[ $IP == 172.16.* ]]; then
        echo -e "  ${GREEN}✓${NC} On private network"
    else
        echo -e "  ${YELLOW}!${NC} Not on typical private network range"
        ((WARNINGS++))
    fi
else
    echo -e "  ${RED}✗${NC} Could not determine network interface"
    ((ERRORS++))
fi
echo ""

# Check 5: Firewall
echo -e "${YELLOW}[5/10]${NC} Checking firewall..."
FIREWALL_STATE=$(sudo /usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate 2>/dev/null)
if echo "$FIREWALL_STATE" | grep -q "enabled"; then
    echo -e "  ${YELLOW}!${NC} Firewall is ENABLED"
    echo -e "    You may need to add OBS to firewall exceptions:"
    echo -e "    ${BLUE}sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add /Applications/OBS.app${NC}"
    echo -e "    ${BLUE}sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblock /Applications/OBS.app${NC}"
    ((WARNINGS++))
else
    echo -e "  ${GREEN}✓${NC} Firewall is disabled"
fi
echo ""

# Check 6: Port Availability
echo -e "${YELLOW}[6/10]${NC} Checking ports..."

check_port() {
    PORT=$1
    if lsof -i :"$PORT" &> /dev/null; then
        PROCESS=$(lsof -i :"$PORT" | tail -1 | awk '{print $1}')
        if [ "$PROCESS" == "OBS" ] || [ "$PROCESS" == "obs" ]; then
            echo -e "  ${GREEN}✓${NC} Port $PORT in use by OBS"
        else
            echo -e "  ${RED}✗${NC} Port $PORT in use by $PROCESS (conflict!)"
            ((ERRORS++))
        fi
    else
        echo -e "  ${YELLOW}!${NC} Port $PORT not in use (OBS may not be running)"
        ((WARNINGS++))
    fi
}

check_port 7000
check_port 5000
echo ""

# Check 7: mDNS Responder
echo -e "${YELLOW}[7/10]${NC} Checking mDNS responder..."
if ps aux | grep -v grep | grep -q "mDNSResponder"; then
    echo -e "  ${GREEN}✓${NC} mDNSResponder is running"
else
    echo -e "  ${RED}✗${NC} mDNSResponder is not running"
    echo -e "    Try: ${BLUE}sudo killall -HUP mDNSResponder${NC}"
    ((ERRORS++))
fi
echo ""

# Check 8: AirPlay Service Advertisement
echo -e "${YELLOW}[8/10]${NC} Checking AirPlay service advertisement..."
echo -e "  ${BLUE}Scanning for 5 seconds...${NC}"

timeout 5 dns-sd -B _airplay._tcp > /tmp/airplay_scan.txt 2>&1 &
SCAN_PID=$!
sleep 5
kill $SCAN_PID 2>/dev/null || true

if grep -q "OBS AirPlay" /tmp/airplay_scan.txt; then
    echo -e "  ${GREEN}✓${NC} OBS AirPlay service is being advertised!"
else
    echo -e "  ${RED}✗${NC} OBS AirPlay service not found"
    echo -e "    This means the plugin is not advertising via mDNS"
    echo -e "    Check OBS logs for errors"
    ((ERRORS++))
fi

# Show other AirPlay devices found
OTHER_DEVICES=$(grep -v "OBS AirPlay" /tmp/airplay_scan.txt | grep "Add" | wc -l)
if [ "$OTHER_DEVICES" -gt 0 ]; then
    echo -e "  ${BLUE}ℹ${NC} Found $OTHER_DEVICES other AirPlay device(s) on network"
fi
rm -f /tmp/airplay_scan.txt
echo ""

# Check 9: OBS Logs
echo -e "${YELLOW}[9/10]${NC} Checking OBS logs..."
LOG_DIR="$HOME/Library/Application Support/obs-studio/logs"
if [ -d "$LOG_DIR" ]; then
    LATEST_LOG=$(ls -t "$LOG_DIR" | head -1)
    if [ -n "$LATEST_LOG" ]; then
        echo -e "  ${GREEN}✓${NC} Latest log: $LATEST_LOG"
        
        # Check for plugin messages
        if grep -q "AirPlay" "$LOG_DIR/$LATEST_LOG"; then
            echo -e "  ${GREEN}✓${NC} Plugin messages found in log"
            
            # Check for errors
            if grep -i "airplay.*error\|airplay.*failed" "$LOG_DIR/$LATEST_LOG" &> /dev/null; then
                echo -e "  ${RED}✗${NC} Errors found in log:"
                grep -i "airplay.*error\|airplay.*failed" "$LOG_DIR/$LATEST_LOG" | tail -3 | sed 's/^/    /'
                ((ERRORS++))
            fi
            
            # Check for success messages
            if grep -q "mDNS advertising started" "$LOG_DIR/$LATEST_LOG"; then
                echo -e "  ${GREEN}✓${NC} mDNS advertising confirmed in logs"
            else
                echo -e "  ${RED}✗${NC} mDNS advertising not confirmed"
                ((ERRORS++))
            fi
        else
            echo -e "  ${RED}✗${NC} No plugin messages in log (plugin may not have loaded)"
            ((ERRORS++))
        fi
    else
        echo -e "  ${YELLOW}!${NC} No log files found"
        ((WARNINGS++))
    fi
else
    echo -e "  ${YELLOW}!${NC} Log directory not found"
    ((WARNINGS++))
fi
echo ""

# Check 10: System Resources
echo -e "${YELLOW}[10/10]${NC} Checking system resources..."

CPU_USAGE=$(ps aux | grep "OBS" | grep -v grep | awk '{sum+=$3} END {print sum}')
if [ -n "$CPU_USAGE" ]; then
    echo -e "  ${BLUE}ℹ${NC} OBS CPU usage: ${CPU_USAGE}%"
    if (( $(echo "$CPU_USAGE > 80" | bc -l) )); then
        echo -e "  ${YELLOW}!${NC} High CPU usage may affect streaming quality"
        ((WARNINGS++))
    fi
fi

MEM_PRESSURE=$(memory_pressure | grep "System-wide memory free percentage" | awk '{print $5}' | tr -d '%')
if [ -n "$MEM_PRESSURE" ]; then
    echo -e "  ${BLUE}ℹ${NC} Memory pressure: ${MEM_PRESSURE}%"
    if [ "$MEM_PRESSURE" -lt 30 ]; then
        echo -e "  ${YELLOW}!${NC} Low memory available"
        ((WARNINGS++))
    fi
fi
echo ""

# Summary
echo -e "${BLUE}╔════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Diagnostic Summary                            ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════╝${NC}"
echo ""

if [ $ERRORS -eq 0 ] && [ $WARNINGS -eq 0 ]; then
    echo -e "${GREEN}✓ All checks passed!${NC}"
    echo ""
    echo "If your device still doesn't show up:"
    echo "1. Restart OBS"
    echo "2. Restart your iOS device"
    echo "3. Ensure both devices are on the same WiFi network"
    echo "4. Check iOS: Settings -> WiFi -> Your Network -> (i) icon"
    echo "5. Compare IP address ranges (should match)"
else
    if [ $ERRORS -gt 0 ]; then
        echo -e "${RED}✗ Found $ERRORS error(s) that need attention${NC}"
    fi
    
    if [ $WARNINGS -gt 0 ]; then
        echo -e "${YELLOW}! Found $WARNINGS warning(s)${NC}"
    fi
    
    echo ""
    echo "Recommended actions:"
    
    if [ $ERRORS -gt 0 ]; then
        echo "1. Fix the errors listed above"
        echo "2. Rebuild the plugin: ./build.sh"
        echo "3. Restart OBS"
    fi
    
    if grep -q "Firewall is ENABLED" /tmp/diag_output.txt 2>/dev/null; then
        echo "4. Add OBS to firewall exceptions (see firewall section above)"
    fi
fi

echo ""
echo "For more help, see:"
echo "- README.md for detailed documentation"
echo "- QUICKSTART.md for common issues"
echo "- OBS logs: $LOG_DIR"
echo ""
echo -e "${BLUE}Support: https://github.com/yourusername/obs-airplay-plugin/issues${NC}"
