# 恢复 Opus 编码

> 日期: 2026-03-04 | Issue: #148 | PR: #149

## 问题链
1. `esphome/micro-opus` PSRAM 伪栈 → 编码速度仅 50% 实时
2. 临时绕过：跳过 Opus 直接发裸 PCM
3. 服务端 `is_opus=True` → 解码垃圾，ASR 完全失败

## 解决
替换为 `espressif/esp_audio_codec ~2.4.1`（Espressif 官方优化版，与 xiaozhi 对齐）

## 踩坑

### Opus 编码器初始化 (error -5)
- `bitrate=-1` 报错 → 正确值 `ESP_OPUS_BITRATE_AUTO = -1000`（注意与 libopus 原生 API 不同）
- `enable_dtx=true` + AUDIO 模式报错 → DTX 仅支持 VOIP 模式，查 `esp_opus_enc.h`

### WiFi OOM → boot loop
- **根因**: Opus encoder/decoder + 任务栈(24KB) + 流缓冲区(32KB) = ~56KB 内部 RAM，WiFi 需 50-70KB → OOM
- **修复**: 三者全部分配到 PSRAM:
  - `xTaskCreatePinnedToCoreWithCaps(... MALLOC_CAP_SPIRAM)`
  - `xStreamBufferCreateWithCaps(... MALLOC_CAP_SPIRAM)`
- **规则**: ESP32-S3 内部 RAM ~200KB；非 DMA 关键的大型分配必须用 PSRAM

### CH340K 串口 RST 后断连
- **原因**: ESP32 RST 时 CH340K 重连，错过启动日志
- **解决**: 用 pyserial 自动重连脚本捕获 boot 日志
