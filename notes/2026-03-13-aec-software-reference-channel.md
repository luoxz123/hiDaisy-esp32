# AEC 软件参考通道实现

**日期**: 2026-03-13
**Issues**: #194, #195, #196
**PRs**: #197, #198, #199

## 背景

设备端 AEC 一直无法正常工作，原因是我们的板子（zhengchen-1.54tft-wifi）使用独立 I2S 总线连接麦克风和喇叭，没有硬件回环通道。之前直接把 `aec_init` 设为 false 绕过了问题，但这导致 TTS 播放时喇叭声被麦克风采集，干扰 ASR 识别。

## 解决方案：软件参考通道

参考小智 esp-box 等板子的做法，通过软件构造 AEC 参考通道：

### 架构

```
Speaker PCM ──┬──→ I2S Output (喇叭)
              └──→ Speaker Ref StreamBuffer (12800B / 400ms)
                        │
Mic I2S ────────────────┼──→ Interleave [mic, ref] → AFE ("MR", AEC+NS)
                                                          │
                                                    AEC-cleaned PCM
                                                    ↙          ↘
                                              WakeNet        Opus → Server
                                                                   ↓
                                                            Silero VAD (barge-in)
```

### 三步实现

1. **Speaker Reference RingBuffer (#194)**: 在 AudioService 中添加 StreamBuffer，PlaybackTask 写 I2S 时同步写入 volume-adjusted 16-bit PCM
2. **AFE AEC 模式 (#195)**: AfeProcessor 从 `"M"` 改为 `"MR"` 双通道，启用 AEC (`AEC_MODE_VOIP_LOW_COST`，原 HIGH_PERF 因 OOM 降级)，禁用本地 VAD
3. **端到端集成 (#196)**: Hello 消息添加 `features.aec:true`，SPEAKING 状态保持 capture 运行（音频持续上传供服务端 barge-in 检测）

## 关键设计决策

### 为什么用 StreamBuffer 而非 RingBuffer

StreamBuffer 是 FreeRTOS 提供的线程安全 FIFO，适合单生产者-单消费者场景（PlaybackTask 写、CaptureTask 读）。PSRAM 分配避免占用 WiFi 所需的内部 RAM。

### 为什么禁用本地 VAD

ESP-SR AFE 的 AEC 和 VAD 是互斥的（参考小智）。我们的架构是：
- **本地**: 只做 AEC (消除回声) + NS (降噪) + WakeNet (唤醒词)
- **服务端**: Silero VAD 做语音检测 + barge-in 判断

本地保留 RMS-based 语音检测作为 auto-stop 回退机制。

### SPEAKING 状态不停止 capture

之前 SPEAKING 时调 `StopCapture()` 停止上传音频。现在保持 capture 运行，AEC-cleaned 音频持续发送到服务端，服务端 Silero VAD 在此基础上检测用户打断意图。

### 参考数据是 post-volume

RingBuffer 中存储的是经过 `ApplyVolume()` 处理后的 PCM。这样参考数据与喇叭实际输出匹配，AEC 效果更好。

## 踩坑：AEC 导致 WiFi OOM Boot Loop (#200, PR #201)

### 现象
启用 AEC 后设备进入无限重启循环:
```
W (2006) wifi:malloc buffer fail
E (2017) wifi:Expected to init 10 rx buffer, actual is 2
ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM) at esp_wifi_init()
```

### 根因
两个问题叠加:
1. **初始化顺序错误**: `AfeProcessor::Init()` 在 `WifiManager::Init()` 之前执行，AEC 算法消耗大量内部 RAM
2. **AEC 模式过重**: `AEC_MODE_VOIP_HIGH_PERF` 即使设置了 `AFE_MEMORY_ALLOC_MORE_PSRAM`，部分核心分配仍使用内部 RAM，导致 WiFi 所需的 ~50-70KB 内部 RAM 不足

### 解决方案
1. 将 WiFi 初始化移到 AFE 初始化之前（WiFi 优先获取内部 RAM）
2. AEC 模式从 `AEC_MODE_VOIP_HIGH_PERF` 改为 `AEC_MODE_VOIP_LOW_COST`

### 修复后内存状况
- Internal RAM free: ~27KB（足够 WiFi 稳定运行）
- PSRAM free: ~6.9MB
- AFE Pipeline: `AEC(VOIP_LOW_COST)` + `NS(nsnet2)` 正常运行

### 教训
- **ESP-SR AFE 的 `AFE_MEMORY_ALLOC_MORE_PSRAM` 不是万能的** — AEC 核心算法仍有部分分配走内部 RAM
- **WiFi 必须在任何大内存消费者之前初始化**（已知踩坑: Opus 编解码、AEC 都会抢内部 RAM）
- ESP-SR 提供 4 种 AEC 模式，按内存从低到高: `SR_LOW_COST` < `SR_HIGH_PERF` < `VOIP_LOW_COST` < `VOIP_HIGH_PERF`

## 待验证项

- [ ] AEC 消除效果：TTS 播放时说话，服务端 ASR 能否正确识别
- [ ] 延迟对齐：I2S 写入到喇叭物理输出的延迟是否影响 AEC（通常 AEC 算法可容忍 ~200ms 偏移）
- [x] ~~内存消耗：AEC 模式下 AFE 使用更多 PSRAM，验证不会 OOM~~ → 已解决 (#200)
- [ ] RMS 阈值 200 是否合适：可能需要根据实际 AEC 后信号幅度调整

## 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `main/audio_service.h` | 添加 `speaker_ref_buf_`、`ReadReferenceData()` |
| `main/audio_service.cc` | RingBuffer 创建/写入/读取、CaptureTask 双通道喂入 |
| `main/afe_processor.h` | Feed() 签名改为 `(mic, ref, count)`，添加 `kInputChannels` |
| `main/afe_processor.cc` | AFE 配置 "MR" + AEC，交织 feed，RMS-based 语音检测 |
| `main/application.cc` | Hello 添加 features.aec，SPEAKING 不停 capture |
| `main/qemu_mock/afe_processor_mock.cc` | Feed() 签名同步 |
