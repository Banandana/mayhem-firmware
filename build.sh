#!/bin/bash
# Build script for PortaPack Mayhem firmware (PRALINE/HackRF Pro)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DOCKER_IMAGE="mayhem-gcc9"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -c, --clean      Clean build (remove build directory first)"
    echo "  -b, --board NAME Board type: PRALINE (default), HACKRF_ONE, RAD1O"
    echo "  -j, --jobs N     Number of parallel build jobs (default: $(nproc))"
    echo "  -h, --help       Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                    # Build for PRALINE (HackRF Pro)"
    echo "  $0 -c                 # Clean build for PRALINE"
    echo "  $0 -b HACKRF_ONE      # Build for HackRF One"
    echo "  $0 -c -j 8            # Clean build with 8 parallel jobs"
}

# Defaults
CLEAN=0
BOARD="PRALINE"
JOBS=10

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -b|--board)
            BOARD="$2"
            shift 2
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            exit 1
            ;;
    esac
done

# Validate board
case $BOARD in
    PRALINE|HACKRF_ONE|RAD1O)
        ;;
    *)
        echo -e "${RED}Invalid board: $BOARD${NC}"
        echo "Valid boards: PRALINE, HACKRF_ONE, RAD1O"
        exit 1
        ;;
esac

echo -e "${GREEN}Building for board: ${BOARD}${NC}"
echo -e "${GREEN}Using ${JOBS} parallel jobs${NC}"

# Check if Docker image exists
if ! docker image inspect "${DOCKER_IMAGE}" &>/dev/null; then
    echo -e "${RED}Docker image '${DOCKER_IMAGE}' not found!${NC}"
    echo "Please build or pull the Docker image first."
    exit 1
fi

# Build command
BUILD_CMD="cd /src"

if [[ $CLEAN -eq 1 ]]; then
    echo -e "${YELLOW}Performing clean build...${NC}"
    # Remove build directory (may need sudo if Docker created files as root)
    if [[ -d "${BUILD_DIR}" ]]; then
        rm -rf "${BUILD_DIR}" 2>/dev/null || sudo rm -rf "${BUILD_DIR}"
    fi
    # Clean hackrf firmware artifacts (fpga.o, libopencm3, etc. - they have arch-specific paths)
    echo -e "${YELLOW}Cleaning hackrf firmware artifacts...${NC}"
    cd "${SCRIPT_DIR}/hackrf/firmware" && git clean -fdx 2>/dev/null || true
    cd "${SCRIPT_DIR}"
fi

BUILD_CMD="${BUILD_CMD} && mkdir -p build && cd build"
BUILD_CMD="${BUILD_CMD} && cmake -DBOARD=${BOARD} .."
BUILD_CMD="${BUILD_CMD} && make -j${JOBS} firmware"

# Run Docker build
echo -e "${GREEN}Starting Docker build...${NC}"
docker run --rm \
    -v "${SCRIPT_DIR}:/src" \
    -w /src \
    "${DOCKER_IMAGE}" \
    /bin/bash -c "${BUILD_CMD}"

# Check result
if [[ -f "${BUILD_DIR}/firmware/portapack-mayhem-firmware.bin" ]]; then
    echo ""
    echo -e "${GREEN}Build successful!${NC}"
    echo -e "Firmware: ${BUILD_DIR}/firmware/portapack-mayhem-firmware.bin"
    ls -lh "${BUILD_DIR}/firmware/portapack-mayhem-firmware.bin"
    echo ""
    echo "To flash, run: ./flash.sh"
else
    echo -e "${RED}Build failed - firmware binary not found${NC}"
    exit 1
fi
