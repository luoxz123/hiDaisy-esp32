#!/bin/bash
# HiLily ESP32-S3 烧录脚本
# 用法: ./flash.sh
# 注意: 此板子不支持自动进入下载模式，需要手动操作:
#   1. 按住 BOOT 按钮
#   2. 按一下 RST 按钮
#   3. 松开 BOOT 按钮
#   4. 然后运行此脚本（或先运行脚本再按 BOOT+RST）

set -e

PORT="${1:-/dev/cu.wchusbserial1130}"
BAUD=460800
BUILD_DIR="$(cd "$(dirname "$0")" && pwd)/build"
IDF_PATH="/Users/maxazure/.espressif/v5.5.3/esp-idf"
ERASE_USER_DATA="${ERASE_USER_DATA:-0}"

# 检查 build 目录
if [ ! -f "$BUILD_DIR/hilily-esp32.bin" ]; then
    echo "错误: 未找到 $BUILD_DIR/hilily-esp32.bin"
    echo "请先编译项目"
    exit 1
fi

# 检查串口
if [ ! -e "$PORT" ]; then
    echo "错误: 串口 $PORT 不存在"
    echo "请检查 USB 连接，或指定其他串口: ./flash.sh /dev/cu.xxx"
    exit 1
fi

echo "=== HiLily ESP32-S3 烧录 ==="
echo "串口: $PORT"
echo "波特率: $BAUD"
if [ "$ERASE_USER_DATA" = "1" ]; then
    echo "用户数据: 将擦除 nvs 和 storage 分区"
fi
echo ""
echo "如果还没进入下载模式，请现在操作:"
echo "  按住 BOOT → 按 RST → 松开 BOOT"
echo ""

# Source ESP-IDF 环境并烧录
bash -c "set -e; . '$IDF_PATH/export.sh' > /dev/null 2>&1; \
    ERASE_ALL_FLAG=''; \
    if [ '$ERASE_USER_DATA' = '1' ]; then \
        ERASE_ALL_FLAG='--erase-all'; \
    fi; \
    python -m esptool --chip esp32s3 -p '$PORT' -b $BAUD \
    --before no_reset \
    --after no_reset \
    $ERASE_ALL_FLAG \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 '$BUILD_DIR/bootloader/bootloader.bin' \
    0x10000 '$BUILD_DIR/hilily-esp32.bin' \
    0x8000 '$BUILD_DIR/partition_table/partition-table.bin' \
    0xd000 '$BUILD_DIR/ota_data_initial.bin' \
    0x610000 '$BUILD_DIR/srmodels/srmodels.bin'"

echo ""
echo "=== 烧录完成！请按 RST 重启设备 ==="
