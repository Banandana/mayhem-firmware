#!/bin/bash
# Flash script for PortaPack Mayhem firmware (PRALINE/HackRF Pro)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TMP_DIR="/tmp/mayhem_flash_$$"

# Default paths - use built DFU with 4MB flash support for PRALINE
DFU_FILE="${BUILD_DIR}/hackrf/firmware/hackrf_usb/hackrf_usb.dfu"
FALLBACK_DFU_FILE="/home/kitty/Downloads/h4m/mayhem_v2.3.2_FIRMWARE/utils/hackrf_pro_usb.dfu"
KNOWN_GOOD_FW="/home/kitty/Downloads/h4m/mayhem_v2.3.2_FIRMWARE/portapack-mayhem-hackrf-pro.bin"
FIRMWARE="${BUILD_DIR}/firmware/portapack-mayhem-firmware.bin"

# Flash size limit (1MB = 1048576 bytes)
FLASH_LIMIT=1048576
FPGA_OFFSET=1048576  # 0x100000

# Custom hackrf_spiflash with 4MB support (built from hackrf submodule)
CUSTOM_SPIFLASH="/tmp/hackrf_spiflash"
CUSTOM_LIBHACKRF="/tmp/libhackrf.so"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

cleanup() {
    rm -rf "${TMP_DIR}" 2>/dev/null || true
}
trap cleanup EXIT

usage() {
    echo "Usage: $0 [OPTIONS] [FIRMWARE_FILE]"
    echo ""
    echo "Options:"
    echo "  -d, --dfu-only     Only run DFU recovery (don't flash firmware)"
    echo "  -r, --recovery     Flash known-good recovery firmware"
    echo "  -f, --firmware F   Specify firmware file to flash"
    echo "  --dfu-file F       Specify DFU file (default: ${DFU_FILE})"
    echo "  -s, --skip-dfu     Skip DFU step (device already in bootloader)"
    echo "  -h, --help         Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                           # DFU + flash build/firmware/portapack-mayhem-firmware.bin"
    echo "  $0 -r                        # DFU + flash known-good recovery firmware"
    echo "  $0 -d                        # DFU recovery only"
    echo "  $0 -s                        # Flash without DFU (already in bootloader)"
    echo "  $0 -f custom.bin             # Flash custom firmware file"
    echo ""
    echo "Note: Put device in DFU mode first by holding DFU button while connecting USB,"
    echo "      or use the reset button while connected."
    echo ""
    echo "For PRALINE (2MB firmware with FPGA bitstream), the script automatically"
    echo "flashes in two parts: base firmware (1MB) + FPGA bitstream at 0x100000."
}

# Defaults
DFU_ONLY=0
RECOVERY=0
SKIP_DFU=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--dfu-only)
            DFU_ONLY=1
            shift
            ;;
        -r|--recovery)
            RECOVERY=1
            shift
            ;;
        -f|--firmware)
            FIRMWARE="$2"
            shift 2
            ;;
        --dfu-file)
            DFU_FILE="$2"
            shift 2
            ;;
        -s|--skip-dfu)
            SKIP_DFU=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            exit 1
            ;;
        *)
            # Positional argument = firmware file
            FIRMWARE="$1"
            shift
            ;;
    esac
done

# Use recovery firmware if requested
if [[ $RECOVERY -eq 1 ]]; then
    FIRMWARE="${KNOWN_GOOD_FW}"
    echo -e "${YELLOW}Using recovery firmware: ${FIRMWARE}${NC}"
fi

# Validate files exist
if [[ $SKIP_DFU -eq 0 ]]; then
    if [[ ! -f "${DFU_FILE}" ]]; then
        # Try fallback DFU file
        if [[ -f "${FALLBACK_DFU_FILE}" ]]; then
            echo -e "${YELLOW}Built DFU not found, using fallback: ${FALLBACK_DFU_FILE}${NC}"
            echo -e "${YELLOW}WARNING: Fallback DFU may have 1MB flash limit!${NC}"
            DFU_FILE="${FALLBACK_DFU_FILE}"
        else
            echo -e "${RED}DFU file not found: ${DFU_FILE}${NC}"
            echo "Run ./build.sh first to build the DFU with 4MB flash support."
            exit 1
        fi
    else
        echo -e "${GREEN}Using built DFU with 4MB flash support${NC}"
    fi
fi

if [[ $DFU_ONLY -eq 0 ]]; then
    if [[ ! -f "${FIRMWARE}" ]]; then
        echo -e "${RED}Firmware file not found: ${FIRMWARE}${NC}"
        echo "Run ./build.sh first, or specify firmware with -f"
        exit 1
    fi
fi

# DFU recovery step
if [[ $SKIP_DFU -eq 0 ]]; then
    echo -e "${CYAN}Step 1: DFU Recovery${NC}"
    echo -e "Using DFU file: ${DFU_FILE}"
    echo ""
    echo -e "${YELLOW}Make sure device is in DFU mode (hold DFU button while connecting)${NC}"
    echo ""

    # Capture DFU output - it often returns error even on success (device reboots mid-status-read)
    DFU_OUTPUT=$(sudo dfu-util --device 1fc9:000c --alt 0 --download "${DFU_FILE}" 2>&1) || true
    echo "$DFU_OUTPUT"

    # Check if download actually completed (look for 100% or "Download done")
    if echo "$DFU_OUTPUT" | grep -qE "(100%|Download done)"; then
        echo ""
        echo -e "${GREEN}DFU download completed!${NC}"
    elif echo "$DFU_OUTPUT" | grep -q "No DFU capable USB device"; then
        echo -e "${RED}DFU failed - no device found!${NC}"
        echo "Make sure:"
        echo "  1. Device is in DFU mode (hold DFU button while connecting USB)"
        echo "  2. You have permission to access USB devices"
        exit 1
    else
        echo -e "${RED}DFU failed!${NC}"
        echo "Make sure:"
        echo "  1. Device is in DFU mode (hold DFU button while connecting USB)"
        echo "  2. dfu-util is installed (apt install dfu-util)"
        echo "  3. You have permission to access USB devices"
        exit 1
    fi

    if [[ $DFU_ONLY -eq 1 ]]; then
        echo "DFU-only mode - not flashing firmware"
        exit 0
    fi

    echo ""
    echo -e "${YELLOW}Waiting 5 seconds for device to boot...${NC}"
    sleep 5
fi

# Flash firmware
echo -e "${CYAN}Step 2: Flash Firmware${NC}"
echo -e "Flashing: ${FIRMWARE}"

# Get firmware size
FW_SIZE=$(stat -c%s "${FIRMWARE}")
echo -e "Firmware size: ${FW_SIZE} bytes"

if [[ $FW_SIZE -le $FLASH_LIMIT ]]; then
    # Standard 1MB firmware - flash directly
    echo ""
    if ! sudo hackrf_spiflash -w "${FIRMWARE}" -i; then
        echo -e "${RED}Flash failed!${NC}"
        echo "Make sure:"
        echo "  1. Device booted properly after DFU"
        echo "  2. hackrf_spiflash is installed"
        echo "  3. Device is detected (run 'hackrf_info')"
        exit 1
    fi
else
    # PRALINE firmware (>1MB) - requires custom hackrf_spiflash with 4MB support
    echo -e "${YELLOW}Firmware exceeds 1MB - using custom hackrf_spiflash (PRALINE mode)${NC}"

    # Check if custom hackrf_spiflash exists
    if [[ ! -f "${CUSTOM_SPIFLASH}" ]] || [[ ! -f "${CUSTOM_LIBHACKRF}" ]]; then
        echo -e "${YELLOW}Building custom hackrf_spiflash with 4MB flash support...${NC}"
        cd "${SCRIPT_DIR}/hackrf/host"

        # Compile libhackrf
        gcc -shared -fPIC -o "${CUSTOM_LIBHACKRF}" \
            libhackrf/src/hackrf.c \
            -I libhackrf/src \
            $(pkg-config --cflags --libs libusb-1.0) \
            -lpthread 2>/dev/null

        # Compile hackrf_spiflash
        gcc -o "${CUSTOM_SPIFLASH}" \
            hackrf-tools/src/hackrf_spiflash.c \
            -I libhackrf/src \
            "${CUSTOM_LIBHACKRF}" \
            $(pkg-config --cflags --libs libusb-1.0) \
            -lpthread \
            -Wl,-rpath,/tmp 2>/dev/null

        cd "${SCRIPT_DIR}"

        if [[ ! -f "${CUSTOM_SPIFLASH}" ]]; then
            echo -e "${RED}Failed to build custom hackrf_spiflash!${NC}"
            exit 1
        fi
        echo -e "${GREEN}Custom hackrf_spiflash built successfully!${NC}"
    fi

    # Flash entire firmware at once using custom tool
    echo ""
    echo -e "${CYAN}Flashing ${FW_SIZE} bytes (PRALINE 2MB firmware)...${NC}"
    if ! sudo LD_LIBRARY_PATH=/tmp "${CUSTOM_SPIFLASH}" -w "${FIRMWARE}" -i; then
        echo -e "${RED}Flash failed!${NC}"
        echo "Make sure:"
        echo "  1. Device booted properly after DFU"
        echo "  2. Device is detected (run 'hackrf_info')"
        exit 1
    fi
fi

echo ""
echo -e "${GREEN}Flash successful!${NC}"
echo ""
echo -e "${CYAN}Device should now reboot with new firmware.${NC}"
echo "If it doesn't boot, use: $0 -r  (to flash recovery firmware)"
