# Daisy 本地唤醒确认语

## 现象

本地 `Hi Daisy` 唤醒已经成功，但用户仅说唤醒词时，终端立即打开 ASR 录音窗口；没有后续问题便会上传静音，服务器回复 `I can't catch that`。产品目标是唤醒后先回应身份并询问是否需要帮助。

## 排查过程

- 现有固件只有上升/下降提示音，没有可播放的本地语音资源。
- 当前 wake event 发送 `listen(detect)` 后立即切换 listening；状态回调播放提示音，然后启动 Capture。
- 服务端协议没有已确认可用的独立 TTS-only 请求，因此不能安全地假定云端可以先合成欢迎语再打开录音。
- 使用本机 Microsoft Zira 生成 16 kHz、16-bit、mono PCM WAV：`I am Daisy. How can I help you?`。

## 根因

此前交互缺少“本地唤醒确认”阶段，把唤醒成功直接等同于开始 ASR。只有唤醒短语而没有命令时，ASR 空输入提示属于预期后果。

## 解决方案

- 将 `assets/daisy_greeting.wav` 作为固件二进制资源嵌入。
- 新增 `AudioService::PlayWakeGreeting()`，校验 WAV 头后使用现有 I2S 和 AEC reference 路径阻塞播放。
- Daisy 命中后先暂停本地推理，播放 `I am Daisy. How can I help you?`，再发送 `listen(detect)` 并进入 listening。
- 欢迎语替代该次上升提示音；按键唤醒仍使用原提示音，不改变按键行为。
- 发布 `listen(detect)` 失败时恢复 idle Daisy 推理。

## 验证

- ESP-IDF 5.5.3 全量构建和链接通过。
- 新固件 `build/hilily-esp32.bin` 大小 `0x2306b0`，最小 app 分区剩余 27%。
- 仍需真机确认语音音量、播放清晰度，以及欢迎语结束后 Capture 能立即接收问题。

## 教训

- 唤醒确认语适合放在终端本地，延迟稳定且不依赖服务器；开放式回答继续交给云端。
- 本地语音播放必须复用扬声器 I2S 和 AEC reference 路径，不能创建第二套音频通道。
- 应在欢迎语播放结束后才打开 ASR 窗口，避免把本机播音或纯静音上传服务器。
