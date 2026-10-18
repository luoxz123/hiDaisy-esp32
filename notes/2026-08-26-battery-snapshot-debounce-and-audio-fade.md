# 电池快照一致性、低电确认与按键淡出

## 现象

屏幕出现 `LOW BATTERY, Please charge` 时，电池图标仍显示四格，说明 UI 与提醒没有使用同一份稳定的电量数据。按键打断播放时还会立即清空队列并关闭 I2S，导致句子硬截断。

## 排查过程

- 原电池任务启动后立即发布第一次 ADC 结果，启动瞬态可直接进入 UI。
- UI 每 10 秒读取一次百分比，低电事件处理时又重新读取电池对象，图标与 MQTT 可能来自不同采样时刻。
- 原逻辑只按一次 `<=20%` 判断，没有连续采样确认。
- ADC 结果没有 2500～5000mV 合理范围检查。
- Overlay 发送前只检查 Idle 和未播放，没有检查录音状态。
- 状态机不允许 `idle -> thinking/speaking`，服务器主动系统通知可能被拒绝。
- 按键打断顺序为 abort、清队列、StopPlayback，必然产生突然截断；服务器提前报 Idle 但扬声器仍排空时，还会错误走普通 Idle 唤醒分支。

## 根因

电池采样、图标刷新、Overlay 和 MQTT 上报缺少统一且经过确认的快照；音频打断也没有由播放任务协调的淡出阶段。

## 解决方案

- ADC 启动后丢弃前 5 个有效样本，再收满 5 点窗口并取中值。
- 2500～5000mV 之外的读数直接丢弃，不更新 UI、不累计低电次数、不触发提醒。
- 使用带 sequence 的一致性快照，同时携带电压、百分比、有效标志和低电确认标志。
- 连续 3 个有效中值快照均 `<=20%` 后才确认低电。
- 图标、待显示 Overlay 和 MQTT 使用同一快照；MQTT 增加 `snapshot_sequence` 便于端到端核对。
- 发送前再次检查 `idle && !speaker_playing && !capture_active && mqtt_connected`；忙碌时保留 pending，稍后用最新有效快照重试。
- 进入 Listening/Thinking/Speaking 或按键时隐藏低电 Overlay，避免遮挡交互。
- 状态机增加 `idle -> thinking` 和 `idle -> speaking`。
- PlaybackTask 收到淡出请求后，将当前 60ms 音频帧线性衰减到零，再停 I2S；随后才清理剩余队列并进入 Listening。
- 即使服务器逻辑状态已经是 Idle，只要物理扬声器还在播放，按键也走淡出打断路径。

## 验证

- ESP-IDF 增量编译、链接和分区检查成功。
- 最终固件 `build/hilily-esp32.bin` 大小 `0x23a300`。
- 应用分区剩余 `0xc5d00`（26%）。
- 最终构建没有本次改动引入的编译警告。

## 预期日志

```text
battery: Battery ADC warm-up: ...
battery: Battery median window: 1/5
battery: Battery snapshot #...: ...mV, ...%, low=... (.../3)
app: Low-battery reminder sent: ...%, ...mV, snapshot=...
audio: Playback faded out over one audio frame
```

## 教训

- UI 与行为决策必须消费同一个不可分割的传感器快照，不能各自查询实时值。
- 阈值类告警需要范围校验、启动预热、滤波和连续命中确认。
- 音频淡出必须在唯一的 PlaybackTask 内执行，其他任务不能同时操作 I2S 输出。
