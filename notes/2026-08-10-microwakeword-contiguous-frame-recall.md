# microWakeWord 待机启用但无法命中

## 现象

设备不按键、处于 idle 状态时多次呼叫 `Hi Daisy` 没有反应。启动日志显示模型、AFE 和推理任务均成功初始化，且 `wifi_connecting -> idle` 后明确打印 `Daisy inference enabled`。

## 排查过程

- AFE 管线为 16 kHz、1MIC+1 playback、AEC+NS，持续运行且没有 ringbuffer 溢出。
- microWakeWord 模型张量初始化成功，推理任务运行在 Core 0。
- 网络鉴权、MQTT、UDP 正常，但没有 `Hi Daisy detected` 或 `Wake word detected`；服务器没有收到本地唤醒事件，所以不是服务器拒绝或下行故障。
- 回看上一轮 CPU 修复，音频队列被改成长度 1，并使用 `xQueueOverwrite`。推理期间到达的新 10 ms 帧会覆盖旧帧，导致送入流式模型的特征时间轴不连续。

## 根因

Daisy 是带内部状态的流式模型，输入 `int8[1,3,40]` 依赖连续的 10 ms 特征切片。单帧最新值邮箱虽然消除了积压，却可能频繁跳帧，破坏训练时假设的连续特征序列，显著降低召回率。

## 解决方案

- 恢复 8 个 10 ms 帧的短有界队列，正常情况下保留连续音频。
- 队列满时只淘汰最旧的一帧再写入最新帧，最大排队延迟约 80 ms，不允许秒级历史积压。
- 推理仍只在 idle 状态运行，避免与 Listening 状态的 Opus 编码竞争。
- 推理任务优先级由 1 调整为 2，并保留每次处理后的主动让出，改善待机召回同时给 IDLE0 留出调度机会。

## 验证

- ESP-IDF 5.5.3 构建、链接和分区检查通过。
- 固件 `build/hilily-esp32.bin` 大小 `0x214d30`，app 分区剩余 31%。
- 仍需真机验证 `Daisy inference enabled -> Hi Daisy detected -> Wake word detected!`，并观察是否出现 `Inference lag, evicted ...`。

## 教训

- 流式唤醒模型的实时策略不能简单等同于普通“只取最新值”传感器；特征连续性是模型输入契约的一部分。
- 应先通过应用状态门控减少无意义负载，再用短队列限制延迟；不能以持续跳帧作为主要降载手段。
- 若仍不能命中，应取得训练 manifest 和真实音频样本，核对特征前端参数、阈值及训练数据，而不是继续猜测服务器行为。
