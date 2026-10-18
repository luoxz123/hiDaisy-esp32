# 2026-06-24 Serial monitor shows ROM download mode

## 现象

Windows 上用 `esp-idf-monitor` 打开 `COM3` 后反复出现：

- `GetOverlappedResult failed (PermissionError(13, '拒绝访问。', None, 5))`
- `boot:0x33 (DOWNLOAD(USB/UART0))`
- `waiting for download`

## 排查过程

- `boot:0x33 (DOWNLOAD(USB/UART0))` 是 ESP32-S3 ROM 下载模式日志，不是应用固件启动日志。
- 板子停在 `waiting for download`，说明 GPIO0/BOOT 在复位时被拉低，或烧录流程后没有正常退出下载模式。
- `GetOverlappedResult` 权限错误通常出现在 Windows 串口断开重连、另一个程序占用串口、或 CH340 复位导致 monitor 句柄失效时。
- 检查 `flash.sh` 发现仍使用 `--after hard_reset`，与本项目“不要 hard reset”的硬件说明冲突。

## 根因

这次日志的直接原因是设备处于 ROM 下载模式，monitor 只能看到 bootloader 等待下载。串口权限错误是 Windows/CH340 串口重连或端口占用的副作用。烧录脚本里的 `--after hard_reset` 也可能加剧复位后串口状态不稳定。

## 解决方案

- 监控前确认没有 VS Code Serial Monitor、ESP-IDF Monitor、Arduino Serial Monitor、其他 `python/esptool` 进程占用 `COM3`。
- 如果只是想运行固件：松开 BOOT，不要按住 BOOT，按一下 RST；仍停在下载模式时，拔插 USB 后只按 RST。
- 如果是准备烧录：停在 `waiting for download` 是正常下载等待状态，此时运行烧录命令即可。
- 烧录完成后手动按 RST 启动固件，不依赖自动 hard reset。
- 已将 `flash.sh` 的 esptool 参数从 `--after hard_reset` 改为 `--after no_reset`。

## 教训

看到 `waiting for download` 时先判断启动模式，不要继续排查应用层网络或音频。此板不支持可靠自动复位，烧录后需要手动 RST，并且脚本不能使用 `--after hard_reset`。
