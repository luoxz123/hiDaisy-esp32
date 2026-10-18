# 音频播放速度异常修复

> 日期: 2026-03-04 | PR: #155 (ESP32) / #243 (Server)

## Bug 1: AudioTest 录音回放加速
- **根因**: AudioTest 与 CaptureTask 竞争同一 I2S DMA，AudioTest 只录到部分帧
- **修复**: `SetMicExclusive(true)` — AudioTest 录音时 CaptureTask 暂停读取
- **禁止**: AudioTest 不可自行 enable/disable I2S channel

## Bug 2: TTS 播放 1.67x 加速
- **根因**: 服务端 Opus 编码丢弃余量 40%（同坑3，见 2026-03-03-audio-pipeline-debugging）
- **修复**: `_opus_encode_leftover` carry-over buffer + 结束时 flush

## 附加: AUDIO_OUTPUT_SAMPLE_RATE 必须为 16000
- 从 24000 改为 **16000**（与服务端 Opus 编码率匹配）
- 所有 silence buffer 必须由 `AUDIO_OUTPUT_SAMPLE_RATE` 计算，**禁止硬编码**
