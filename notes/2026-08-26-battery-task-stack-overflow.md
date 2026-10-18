# 电池监控任务启动即栈溢出

## 现象

烧录电池快照改进固件后，设备在 `Battery monitor started` 后立即触发：

```text
***ERROR*** A stack overflow in task battery has been detected.
```

随后产生损坏回溯并不断软件复位，其他模块无法继续初始化。

## 排查过程

- 崩溃任务名明确为 `battery`，发生在 ADC 初始化成功、监控任务首次运行时。
- 原 `Battery::StartMonitoring()` 仍为任务分配 2048 字节内部 RAM 栈。
- 新增 ADC 校准调用、有效性检查、中值滤波、排序和格式化诊断后，任务峰值栈需求已超过 2KB。
- 回溯末端出现 `0xa5a5a5a5`，符合 FreeRTOS 栈填充值被破坏的表现。

## 根因

功能扩展后没有同步调整电池任务栈大小。2KB 栈无法承载 ADC 校准驱动和新增滤波/日志调用，第一次任务切换时被 FreeRTOS 栈溢出检查捕获。

## 解决方案

- 将电池任务栈从 2048 字节提高到 6144 字节。
- 使用 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` 把非 DMA 栈放入 PSRAM，避免消耗 Wi-Fi 所需的内部 RAM。
- 检查任务创建返回值，失败时输出明确错误。
- 启动日志输出 `uxTaskGetStackHighWaterMark()`，并在调试日志中持续保留栈余量，便于真机确认安全裕量。

## 验证

- ESP-IDF 增量编译、链接和分区检查成功。
- 固件 `build/hilily-esp32.bin` 大小 `0x23a3b0`。
- 应用分区剩余 `0xc5c50`（26%）。
- 仍需烧录真机确认启动日志中 `Battery monitor started (stack free: ...)` 后能够进入 ADC warm-up，而不再复位。

## 教训

- 修改 FreeRTOS 任务内部算法或日志后，必须重新评估栈高水位。
- ESP32-S3 内部 RAM 紧张，非 DMA、非 ISR 的较大任务栈应优先使用 PSRAM。
- 创建任务必须检查返回值；运行后应在调试阶段记录栈高水位，而不是只依赖崩溃后的回溯。
