# microWakeWord 实时积压与 CPU0 看门狗

## 现象

- Daisy 模型能够命中，日志出现 `Hi Daisy detected`，概率最高达到 1.0。
- `mww` 队列累计丢弃超过 1400 个 10 ms 音频帧。
- AFE feed ringbuffer 持续满，出现音频数据丢失。
- Task WDT 连续报告 `IDLE0` 无法运行；回溯分布在 UI、Opus 编码和 TFLite/ESP-NN 推理中。
- 已经命中的唤醒事件因 CPU/队列拥塞延迟到设备回到 idle 后才被处理，造成错误的再次唤醒。

## 排查过程

- 根据时间线确认模型本身已成功初始化和推理，并非模型格式或阈值错误。
- 检查任务布局：UI、Opus 编码、消息路由和 Daisy 推理均位于 Core 0；Daisy 原优先级为 4，并持续处理历史队列。
- 原 8 帧队列在推理赶不上 100 Hz 输入时保存旧音频，既不能解决吞吐不足，还会扩大唤醒延迟。
- Daisy 在 listening/thinking/speaking 状态仍持续推理，与 Opus、UI 和网络处理争抢 CPU，但这些状态本来不需要再次触发唤醒。

## 根因

microWakeWord 被当作无状态消费者持续运行，缺少应用状态门控和实时丢弃策略。CPU0 总工作量超过实时预算后，低优先级 idle task 长期得不到调度，从而触发看门狗；与此同时 AFE fetch 也因系统调度压力无法及时消费。

## 解决方案

- 只在设备 `idle` 状态启用 Daisy，其他状态暂停推理和入队。
- 把 8 帧 FIFO 改为长度 1 的最新帧邮箱，使用 `xQueueOverwrite`，禁止历史音频积压和延迟唤醒。
- Daisy 任务优先级从 4 降为 1，每次处理后 `vTaskDelay(1)`，保证 Core 0 idle task 获得运行机会。
- 每次重新启用时重置声学特征前端、模型输入步幅、滑动概率窗口，并保留约 1 秒 warm-up，避免播放尾音触发。
- 禁用时立即清空邮箱和未满 10 ms PCM carry。

## 验证

- ESP-IDF 5.5.3 全量构建通过。
- 新固件大小 `0x214d00`，最小 app 分区剩余 31%。
- 尚需真机验证：idle 下 Daisy 召回、进入 listening 后日志显示 `Daisy inference paused`、不再出现持续队列丢帧和 `IDLE0` 看门狗。

## 教训

- 唤醒词只应在可接受唤醒事件的状态运行；应用状态和推理生命周期必须绑定。
- 实时识别链路应该优先保证数据新鲜度，落后时丢弃旧帧，不能通过扩大 FIFO 掩盖算力不足。
- ESP32 双核任务规划必须给 idle task 留出明确调度窗口；仅降低任务优先级不足以保证 idle 获得 CPU，长时间循环还需要主动阻塞或让出。
