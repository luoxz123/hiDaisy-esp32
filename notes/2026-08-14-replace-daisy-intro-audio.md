# 替换 Daisy 本地唤醒欢迎语音色

## 现象

终端本地播放的 `I am Daisy. How can I help you?` 与服务器 TTS 使用的音色不同，造成一轮对话中音色不一致。

## 排查过程

- 找到用户提供的项目根目录文件 `daisy_intro.wav`。
- 检查 WAV 参数：16 kHz、16-bit、单声道、未压缩 PCM，时长约 2.362 秒。
- 参数与终端 MAX98357A 播放链路要求一致，因此无需重采样或格式转换。

## 根因

固件仍嵌入并引用旧的 `main/assets/daisy_greeting.wav`，该文件使用了与服务器不同的合成音色。

## 解决方案

- 将新文件复制为 `main/assets/daisy_intro.wav`。
- 将 `main/CMakeLists.txt` 的嵌入资源改为 `daisy_intro.wav`。
- 将 `audio_service.cc` 中的链接器资源符号切换为 `_binary_daisy_intro_wav_start/end`。
- 保持 `PlayWakeGreeting()` 及唤醒事件流程不变，只替换播放内容。

## 验证

- 完整构建和链接成功，构建日志确认生成 `daisy_intro.wav.S.obj`。
- `build/hilily-esp32.bin` 大小为 `0x2277c0`。
- 最小应用分区为 `0x300000`，剩余 `0xd8840`（28%）。

## 教训

替换本地提示音前应先确认采样率、位深、声道数和 PCM 编码。格式匹配时应直接嵌入原始资源，避免不必要的转码和音质损失。
