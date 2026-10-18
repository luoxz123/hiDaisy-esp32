#!/bin/bash
# HiLily ESP32-S3 串口监视脚本
# 用法: ./monitor.sh [秒数]
# 输出同时显示在终端和保存到 logs/ 目录
# 按 Ctrl+C 手动停止

PORT="${1:-/dev/cu.wchusbserial1130}"
DURATION="${2:-0}"  # 0 = 无限制，手动 Ctrl+C 停止
LOG_DIR="$(cd "$(dirname "$0")" && pwd)/logs"
BAUD=115200

mkdir -p "$LOG_DIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="$LOG_DIR/serial_${TIMESTAMP}.log"

echo "=== HiLily ESP32-S3 串口监视 ==="
echo "串口: $PORT"
echo "波特率: $BAUD"
echo "日志文件: $LOG_FILE"
if [ "$DURATION" -gt 0 ] 2>/dev/null; then
    echo "监视时长: ${DURATION}s"
else
    echo "监视时长: 无限制 (Ctrl+C 停止)"
fi
echo "================================="
echo ""

# Python 脚本：自动重连 + 保存日志
python3 - "$PORT" "$BAUD" "$LOG_FILE" "$DURATION" << 'PYEOF'
import serial
import sys
import time
import signal
import os

port = sys.argv[1]
baud = int(sys.argv[2])
log_file = sys.argv[3]
duration = int(sys.argv[4])

running = True
def handler(sig, frame):
    global running
    running = False
signal.signal(signal.SIGINT, handler)
signal.signal(signal.SIGTERM, handler)

start = time.time()
lines = 0

with open(log_file, 'w') as f:
    f.write(f"# Serial log started at {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    f.write(f"# Port: {port} Baud: {baud}\n\n")
    f.flush()

    while running:
        if duration > 0 and (time.time() - start) > duration:
            print(f"\n--- 已达到 {duration}s 时限，停止监视 ---")
            break

        try:
            ser = serial.Serial(port, baud, timeout=1)
            print(f"[已连接 {port}]")
            f.write(f"[CONNECTED {time.strftime('%H:%M:%S')}]\n")
            f.flush()

            while running:
                if duration > 0 and (time.time() - start) > duration:
                    break
                try:
                    line = ser.readline()
                    if line:
                        text = line.decode('utf-8', errors='replace').rstrip('\r\n')
                        print(text)
                        f.write(text + '\n')
                        lines += 1
                        if lines % 50 == 0:
                            f.flush()
                except serial.SerialException:
                    print(f"\n[串口断开，重连中...]")
                    f.write(f"[DISCONNECTED {time.strftime('%H:%M:%S')}]\n")
                    f.flush()
                    break

            ser.close()
        except serial.SerialException:
            time.sleep(1)
        except Exception as e:
            print(f"[错误: {e}]")
            time.sleep(1)

    f.write(f"\n[END {time.strftime('%H:%M:%S')} - {lines} lines]\n")

elapsed = time.time() - start
print(f"\n=== 监视结束: {lines} 行, {elapsed:.0f}s ===")
print(f"日志保存到: {log_file}")
PYEOF
