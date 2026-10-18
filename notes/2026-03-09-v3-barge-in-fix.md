# V3 Barge-in 打断功能修复

**日期**: 2026-03-09
**分支**: feature/176-v3-hybrid-barge-in

## 现象

用户反馈"打断功能不好用"——在 AI 说话期间尝试打断，设备无反应，TTS 继续播放。

## 排查过程

1. 用 Silero VAD ONNX 分析 `notes/audio_samples/` 的 9 个音频文件 → 所有语音检测概率 < 0.15（阈值 0.5），确认样本不含语音（来自 LISTENING 状态，非打断场景）
2. 全面 review 设备端 + 服务端代码，发现 5 个 Bug

## Bug 清单

### BUG 1（致命）：服务端 drain 阶段音频路由错误

**文件**: `hilily-server/src/services/pipeline.py`

TTS 生成完毕 → `state_machine.transition(IDLE)` → 但设备还在播放 1-5 秒缓冲音频。`feed_audio()` 检查 `current_state == SPEAKING` 才路由到 VAD，drain 阶段 state=IDLE → **音频送进 ASR 队列而非 VAD 检测器**。

用户在最可能打断的时间窗口说话，服务端完全不检测。

### BUG 2（高）：设备端 OnAbort 不切换状态机

**文件**: `main/application.cc`

OnAbort 回调只停止播放，不切换状态机，依赖服务端后续 MQTT 消息。如果 MQTT 延迟/丢失，设备卡在 limbo。还多余调了 `StopCapture()`，随后进 LISTENING 又要 `StartCapture()`。

### BUG 3（中）：VAD 语音计时器过于脆弱

**文件**: `hilily-server/src/services/barge_in_detector.py`

单帧 speech_prob < 0.5 就重置 300ms 计时器。AEC 后音频常有 32ms 概率下降，持续说话也无法触发。

### BUG 4（中）：AEC 后音频信号过弱

MIC_SAMPLE_SHIFT=16 产生 RMS ~3000/32768 的信号，Silero VAD 对弱信号检测能力下降。

### BUG 5（低）：AEC 参考缓冲静音间隙保持旧值

**文件**: `main/audio_service.cc`

TTS 句间静音时，AEC 参考通道被喂入上一个 PCM 样本的非零值（DC），干扰自适应滤波器。

## 解决方案

### Fix 1：服务端 drain 窗口（BUG 1）

在 `pipeline.py` 添加 `_barge_in_window_active` 标志：
- TTS 结束时设 True，启动 5 秒超时 task
- `feed_audio()` 检查 `SPEAKING or _barge_in_window_active` 路由到 VAD
- barge-in 触发或新 dialogue 启动时清除标志

### Fix 2：OnAbort 直接切 LISTENING（BUG 2）

```cpp
MsgRouter::GetInstance().OnAbort([this](const char *reason) {
    MsgRouter::GetInstance().FlushAudioQueue();
    AudioService::GetInstance().ClearBuffers();
    AudioService::GetInstance().StopPlayback();
    // 不调 StopCapture —— LISTENING 需要 capture
    continuous_mode_ = true;
    continuous_relisten_ = true;
    listening_mode_ = kListeningModeAutoStop;
    SetDeviceState(kDeviceStateListening);
});
```

### Fix 3：VAD gap 容忍（BUG 3）

允许最多 3 帧（96ms）低于阈值而不重置 300ms 计时器。新增 `_gap_count` 和 `_max_gap_frames` 参数。

### Fix 4：音频增益补偿（BUG 4）

在 Silero 推理前乘以 4x 增益，补偿 MIC_SAMPLE_SHIFT=16 信号衰减。可通过构造函数 `gain` 参数调节。

### Fix 5：AEC ref 静音填零（BUG 5）

无参考数据时填零而非保持上一个样本值。

## 教训

1. **状态机同步是打断功能的基础**：服务端 state 与设备端播放状态不同步是根本原因，仅靠 state==SPEAKING 判断不够
2. **音频样本必须来自真实场景**：LISTENING 状态的音频不能代表 SPEAKING 状态下的打断场景
3. **VAD 对弱信号敏感**：MIC_SAMPLE_SHIFT 导致的信号衰减直接影响 VAD 准确率，需增益补偿
4. **容错设计**：单帧判断过于脆弱，AEC 后音频不稳定，需 gap 容忍机制

## 涉及文件

| 文件 | 修改 |
|------|------|
| `hilily-server/src/services/pipeline.py` | Fix 1: drain 窗口 |
| `hilily-server/src/services/barge_in_detector.py` | Fix 3 + Fix 4: gap 容忍 + 增益 |
| `main/application.cc` | Fix 2: OnAbort 切 LISTENING |
| `main/audio_service.cc` | Fix 5: AEC ref 填零 |

## 验证方法

1. 部署服务端 → 重启 Docker
2. 烧录 ESP32 固件
3. 端到端测试：触发对话 → TTS 播放中大声说"停" → 检查 abort 流程
4. 观察服务端日志：`[BargeIn]` 前缀的诊断日志确认 VAD 在 drain 阶段仍在运行
