#!/bin/bash
# Build and run HiLily firmware in QEMU emulator
#
# Usage:
#   ./scripts/qemu_run.sh [build|run|monitor|clean]
#
# Default: build + run with monitor

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEFAULT_IDF_PATH="/Users/maxazure/.espressif/v5.5.3/esp-idf"
IDF_PATH="$DEFAULT_IDF_PATH"
BUILD_DIR="${PROJECT_DIR}/build_qemu"
QEMU_SDKCONFIG="${BUILD_DIR}/sdkconfig.qemu"

# Source project-pinned ESP-IDF v5.5.3
source "$IDF_PATH/export.sh" > /dev/null 2>&1

cd "$PROJECT_DIR"

qemu_build() {
    echo "=== Building for QEMU ==="

    # Keep QEMU config isolated from project sdkconfig
    if [ ! -f "$QEMU_SDKCONFIG" ]; then
        idf.py -B "$BUILD_DIR" \
            -DSDKCONFIG="$QEMU_SDKCONFIG" \
            -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.qemu" \
            set-target esp32s3
    fi

    idf.py -B "$BUILD_DIR" \
        -DSDKCONFIG="$QEMU_SDKCONFIG" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.qemu" \
        build

    echo "=== QEMU Build Complete ==="
}

qemu_run() {
    echo "=== Running in QEMU ==="

    # Generate flash image
    local FLASH_IMG="$BUILD_DIR/qemu_flash.bin"
    local EFUSE_IMG="$BUILD_DIR/qemu_efuse.bin"

    python -m esptool --chip=esp32s3 merge_bin \
        --output="$FLASH_IMG" \
        --fill-flash-size=16MB \
        --flash_mode dio --flash_freq 80m --flash_size 16MB \
        0x0 "$BUILD_DIR/bootloader/bootloader.bin" \
        0x10000 "$BUILD_DIR/hilily-esp32.bin" \
        0x8000 "$BUILD_DIR/partition_table/partition-table.bin" \
        0xd000 "$BUILD_DIR/ota_data_initial.bin"

    # Create efuse file if missing
    [ -f "$EFUSE_IMG" ] || dd if=/dev/zero of="$EFUSE_IMG" bs=1024 count=4 2>/dev/null

    QEMU_BIN=$(command -v qemu-system-xtensa 2>/dev/null)
    if [ -z "$QEMU_BIN" ]; then
        echo "Error: qemu-system-xtensa not found. Run: idf_tools.py install qemu-xtensa"
        exit 1
    fi

    echo "Starting QEMU... (Ctrl+A X to quit)"
    "$QEMU_BIN" -M esp32s3 -m 32M \
        -drive file="$FLASH_IMG",if=mtd,format=raw \
        -drive file="$EFUSE_IMG",if=none,format=raw,id=efuse \
        -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -nic user,model=open_eth \
        -nographic
}

qemu_run_graphics() {
    echo "=== Running in QEMU (with virtual framebuffer) ==="
    idf.py -B "$BUILD_DIR" qemu --graphics monitor
}

qemu_clean() {
    echo "=== Cleaning QEMU build ==="
    rm -rf "$BUILD_DIR"
    echo "Done"
}

case "${1:-all}" in
    build)
        qemu_build
        ;;
    run)
        qemu_run
        ;;
    graphics)
        qemu_run_graphics
        ;;
    clean)
        qemu_clean
        ;;
    all)
        qemu_build
        qemu_run
        ;;
    *)
        echo "Usage: $0 [build|run|graphics|clean|all]"
        echo "  build    - Build firmware for QEMU"
        echo "  run      - Run in QEMU with monitor"
        echo "  graphics - Run in QEMU with virtual framebuffer"
        echo "  clean    - Remove QEMU build directory"
        echo "  all      - Build + run (default)"
        exit 1
        ;;
esac
