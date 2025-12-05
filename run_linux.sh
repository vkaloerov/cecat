#!/bin/bash
# Linux shell script for running EtherCAT CLI
# Must be run with sudo or as root!

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "======================================"
echo "EtherCAT CLI - Linux Startup Script"
echo "======================================"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}[ERROR]${NC} This script must be run as root or with sudo"
    echo "Usage: sudo $0 <interface> [options]"
    exit 1
fi

echo -e "${GREEN}[OK]${NC} Running with root privileges"
echo ""

# List available network interfaces
echo "Available network interfaces:"
echo ""
ip link show | grep -E "^[0-9]+:" | awk '{print $2}' | sed 's/://' | while read iface; do
    state=$(ip link show $iface | grep -o "state [A-Z]*" | awk '{print $2}')
    echo "  - $iface ($state)"
done
echo ""

echo "======================================"
echo ""

# Check if interface argument is provided
if [ -z "$1" ]; then
    echo "Usage: $0 <interface> [options]"
    echo ""
    echo "Examples:"
    echo "  $0 eth0"
    echo "  $0 eth0 -v          # with verbose mode"
    echo "  $0 enp0s31f6"
    echo ""
    exit 1
fi

INTERFACE=$1
shift  # Remove first argument, keep the rest for options

# Check if interface exists
if ! ip link show "$INTERFACE" > /dev/null 2>&1; then
    echo -e "${RED}[ERROR]${NC} Interface '$INTERFACE' does not exist"
    echo ""
    echo "Available interfaces:"
    ip link show | grep -E "^[0-9]+:" | awk '{print "  - " $2}' | sed 's/://'
    exit 1
fi

# Check if interface is up
STATE=$(ip link show "$INTERFACE" | grep -o "state [A-Z]*" | awk '{print $2}')
if [ "$STATE" != "UP" ]; then
    echo -e "${YELLOW}[WARNING]${NC} Interface '$INTERFACE' is $STATE"
    echo "You may need to bring it up: sudo ip link set $INTERFACE up"
    echo ""
fi

echo "Starting EtherCAT CLI on interface: $INTERFACE"
echo "Additional options: $@"
echo ""

# Check if executable exists
if [ ! -f "./build/dummy-ecat-cli" ]; then
    echo -e "${RED}[ERROR]${NC} Executable not found: ./build/dummy-ecat-cli"
    echo ""
    echo "Please build the project first:"
    echo "  mkdir -p build"
    echo "  cd build"
    echo "  cmake .."
    echo "  make"
    echo ""
    exit 1
fi

# Run the application
./build/dummy-ecat-cli -i "$INTERFACE" "$@"

EXIT_CODE=$?

if [ $EXIT_CODE -ne 0 ]; then
    echo ""
    echo -e "${RED}[ERROR]${NC} Application exited with error code $EXIT_CODE"
    echo ""
    echo "Common issues:"
    echo "  - No EtherCAT devices connected"
    echo "  - Wrong network interface"
    echo "  - Firewall blocking raw socket access"
    echo "  - Interface not configured properly"
    echo ""
    echo "Try with verbose mode: sudo $0 $INTERFACE -v"
    echo ""
    exit $EXIT_CODE
fi

exit 0
