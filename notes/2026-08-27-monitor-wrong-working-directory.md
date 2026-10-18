# idf.py monitor 在框架目录运行失败

## 现象

PowerShell 当前目录是 `D:\Espressif\frameworks\esp-idf-v5.5.3`，循环运行 `idf.py monitor` 后，CMake 反复报告该目录不可构建。

## 排查过程

检查本地 ESP-IDF 根 CMakeLists.txt：它明确禁止将框架根目录作为应用工程构建。检查 tools/idf.py：`-C/--project-dir` 默认使用当前目录。

## 根因

命令指向框架源码而非 HiLily 工程，失败发生在打开串口之前，与设备固件、电池任务和串口驱动无关。重复运行同一命令不会改变工程目录。

## 解决方案

先切换到 HiLily 项目目录再执行 `idf.py monitor`，或使用 `idf.py -C D:\MASSEYU\imagecup\esp32_main04\hilily-esp32-main monitor` 显式指定工程。需要时用 `-p COM实际编号` 指定串口。

## 教训

区分 ESP-IDF 框架目录和应用工程目录；启动监控时固定工程路径，避免在错误目录反复重试。本次仅诊断并记录，不修改固件，也不删除框架目录下生成的 build 文件。
