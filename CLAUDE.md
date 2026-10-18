# HiLily ESP32 项目指南

## 项目概述
HiLily 是一款 AI 智能陪伴设备，面向老年用户。ESP32 硬件端作为"耳目口鼻"，负责音频采集/播放、UI 渲染、WebSocket 通信，云端负责 ASR → LLM → TTS 处理。
- 语言: C/C++
- 框架: ESP-IDF (v5.x) + FreeRTOS
- 主控: ESP32-S3-N16R8 (双核 240MHz, 16MB Flash, 8MB PSRAM)

## 重要
- 请遇到任何技术问题，例如音频，网络传输等技术问题，先参考 `/Users/maxazure/projects/esp32-xiaozhi/xiaozhi-esp32` 的解决方案。
- 我们当前的主板配置为，请你参考： /Users/maxazure/projects/esp32-xiaozhi/xiaozhi-esp32/main/boards/zhengchen-1.54tft-wifi/
- notes 目录下有我们已知踩过的坑，遇到问题你需要先看一下之前是不是遇到过。

## ⚠️ 音频问题排查清单（高频踩坑）

> **音频链路是本项目最易出错的部分。** 每次修改音频相关代码后，必须逐条检查以下项目。

### 1. 采样率一致性
- ESP32 输入: `AUDIO_INPUT_SAMPLE_RATE` (16kHz) — INMP441 麦克风
- ESP32 输出: `AUDIO_OUTPUT_SAMPLE_RATE` (16kHz) — MAX98357A 功放
- 服务端 Opus: `_OPUS_RATE` (16kHz) — pipeline.py
- **三者必须匹配**，不一致会导致播放变快/变慢

### 2. Opus 帧对齐（Frame Alignment）
- Opus 帧大小 = 960 样本 (60ms @ 16kHz) = 1920 字节
- TTS chunk 大小可能不是 1920 的整数倍（如 3200 字节 = 100ms）
- **必须用 carry-over buffer 保存尾部数据**，否则 40% 音频会被丢弃
- TTS 结束时必须 flush 剩余 buffer（补零凑满最后一帧）

### 3. I2S 通道互斥
- `CaptureTask` 始终读取麦克风 I2S（AFE/唤醒词需要）
- 其他任务（如 AudioTest）不可同时读取同一 I2S 通道
- 使用 `AudioService::SetMicExclusive(true)` 获取独占访问
- **绝对不要在非主音频流中调用 `i2s_channel_enable/disable`**

### 4. 32-bit ↔ 16-bit 转换
- INMP441 输出 32-bit I2S → 右移 12 位 + 饱和裁剪 → 16-bit PCM
- 播放时 16-bit → 左移 16 位 → 32-bit I2S (`Mono16ToMono32`)
- 移位方向错误会导致极轻/爆音/静默

### 5. silence buffer 大小
- 所有 silence buffer 必须用 `AUDIO_OUTPUT_SAMPLE_RATE` 计算，**禁止硬编码**
- 示例: `int32_t silence[AUDIO_OUTPUT_SAMPLE_RATE * 80 / 1000]` (80ms)

## 核心文档索引

### 本仓库文档
| 文档 | 路径 | 说明 |
|------|------|------|
| 产品需求 (PRD) | `docs/PRD_ESP32_Architecture.md` | 产品愿景、用户场景、核心模块、非功能性需求 |
| 本地 API 规范 | `docs/API_SPECIFICATION.md` | 设备与服务端通信协议 (HTTP 鉴权 + WebSocket 双轨) |

### 服务端文档 (权威版本，私有仓库)
| 文档 | 获取方式 | 说明 |
|------|----------|------|
| **服务端 API 规范** | `gh api repos/sarahliuakl/hilily-server/contents/docs/API_SPECIFICATION.md --jq '.content' \| base64 -d` | **最新版本**，包含 `emotion_update` 消息类型 |

> ✅ 本仓库 `docs/API_SPECIFICATION.md` 已与服务端版本同步（含 `emotion_update` 消息类型）。
> 权威来源: `gh api repos/sarahliuakl/hilily-server/contents/docs/API_SPECIFICATION.md --jq '.content' | base64 -d`
> 服务端部署在 `maxazure@192.168.31.205`, 项目路径 `~/projects/hilily-server/`



## 通信协议速查

### HTTP 鉴权
- `POST /api/v1/device/auth` → 用 device_id + secret_key 换取 JWT token 和 ws_url

### WebSocket 指令 (Text Frame JSON)
| 方向 | type | 用途 |
|------|------|------|
| ESP32 → Server | `start_dialogue` | 唤醒/开始录音 (trigger: button/vad/wake_word) |
| ESP32 → Server | `ping` | 心跳保活 (每 30s, 附带 battery/wifi_rssi) |
| Server → ESP32 | `status_update` | 状态机流转 (idle/listening/thinking/speaking) |
| Server → ESP32 | `emotion_update` | 表情更新 (neutral/happy/sad/comfort/surprise/thinking) |
| Server → ESP32 | `audio_meta` | 音频格式预告 (format/sample_rate/channels/bit_depth) |

### 音频流 (Binary Frame)
- 上行: 16kHz, 16-bit, Mono PCM, 每帧 100-200ms
- 下行: PCM/Opus 流, 推入 RingBuffer 消费

## 固件架构 (目标结构)
```
hilily-esp32/
├── components/
│   ├── audio_system/     # I2S 采集、播放与 RingBuffer (Jitter Buffer)
│   ├── network_manager/  # Wi-Fi 配网、重连与 NTP 时钟
│   ├── websocket_client/ # WSS 双轨连接、鉴权与心跳保活
│   ├── state_machine/    # 设备状态机 (Idle/Listening/Thinking/Speaking)
│   └── ui_engine/        # LVGL 界面、动态表情渲染
├── main/                 # 应用层主入口 (FreeRTOS Tasks)
│   ├── main.c            # 系统初始化与 Task 启动
│   ├── task_audio.c      # 音频专职线程
│   ├── task_net.c        # 网络收发专职线程
│   └── task_ui.c         # 屏幕渲染专职线程
├── docs/                 # 产品文档与 API 规范
├── partitions.csv        # NVS, OTA 固件分区表
└── sdkconfig.defaults    # 核心系统配置
```

## 设备硬件信息
| 属性 | 值 |
|------|-----|
| 芯片 | ESP32-S3-N16R8 (QFN56, rev v0.2) |
| MAC | dc:b4:d9:24:3e:f0 |
| 显示屏 | ST7789 TFT 1.54" 240×240 (SPI3_HOST, 80MHz) |
| USB 转串口 | CH340K (VID:1A86 PID:7523) |
| 烧录串口 | `/dev/cu.wchusbserial1130` |
| 监控串口 | `/dev/tty.wchusbserial1130` |
| 板型 | zhengchen-1.54tft-wifi (征辰科技) |
| 麦克风 | INMP441 (I2S, 16kHz Mono) |
| 功放 | MAX98357A (I2S) |

### GPIO 引脚
| 功能 | GPIO |
|------|------|
| LCD MOSI | 41 |
| LCD SCLK | 42 |
| LCD CS | 21 |
| LCD DC | 40 |
| LCD RST | 45 |
| LCD 背光 | 20 |
| Boot 按钮 | 0 |
| I2S SPK DOUT | 7 |
| I2S SPK BCLK | 15 |
| I2S SPK LRCK | 16 |
| I2S MIC SD | 6 |
| I2S MIC WS | 4 |
| I2S MIC SCK | 5 |

## 开发环境
| 属性 | 值 |
|------|-----|
| ESP-IDF (VS Code 扩展) | `/Users/maxazure/.espressif/v5.5.3/esp-idf` |
| ESP-IDF (命令行, v6.1-dev) | `/Users/maxazure/projects/esp32-xiaozhi/esp-idf` |
| Python 环境 | `/Users/maxazure/.espressif/python_env/idf6.1_py3.12_env` |
| 测试 Wi-Fi | SSID: `Lucia-home`, 密码: `1234567890` |

## 烧录指南
1. **构建**: VS Code ESP-IDF 扩展 → Set Target (esp32s3) → Build
2. **此板子不支持自动进入下载模式** (DTR 未连接 GPIO0)
3. **手动进入下载模式**: 按住 BOOT → 按一下 RST → 松开 BOOT
4. **推荐**: 先启动重试脚本，再按 BOOT + RST

## 参考项目

### 1. 小智 ESP32 (主要参考)
- **路径**: `/Users/maxazure/projects/esp32-xiaozhi/xiaozhi-esp32`
- **用途**: 核心架构、设计模式、组件实现的主要参考
- **关键模块与文件**:

| 模块 | 路径 | 对应 HiLily 功能 |
|------|------|------------------|
| 应用主框架 | `main/application.{h,cc}` | FreeRTOS 事件驱动主循环, 状态机整合 |
| 设备状态枚举 | `main/device_state.h` | 状态定义 (Starting/Idle/Listening/Speaking 等) |
| 设备状态机 | `main/device_state_machine.{h,cc}` | 状态流转逻辑 |
| WebSocket 协议 | `main/protocols/websocket_protocol.{h,cc}` | WSS 双轨通信实现 |
| 协议基类 | `main/protocols/protocol.{h,cc}` | 协议抽象层 |
| 音频服务 | `main/audio/audio_service.{h,cc}` | 音频采集/播放/编解码管理 |
| 音频编解码 | `main/audio/audio_codec.{h,cc}` | PCM/Opus 编解码 |
| 音频编解码器 | `main/audio/codecs/` | 具体编解码实现 |
| 音频处理器 | `main/audio/processors/` | 音频前处理 (降噪等) |
| 唤醒词 | `main/audio/wake_words/` | 离线唤醒词模型 |
| 显示抽象层 | `main/display/display.{h,cc}` | 屏幕驱动抽象 |
| LCD 显示 | `main/display/lcd_display.{h,cc}` | LCD 具体实现 |
| 表情显示 | `main/display/emote_display.{h,cc}` | 动态表情渲染 |
| LVGL 显示 | `main/display/lvgl_display/` | LVGL 集成 |
| LED 控制 | `main/led/` | 呼吸灯/状态灯 |
| OTA 升级 | `main/ota.{h,cc}` | 固件升级 |
| 系统信息 | `main/system_info.{h,cc}` | 芯片/内存信息 |
| 板级配置 | `main/boards/` | 不同硬件板型的 BSP 配置 |
| 资源文件 | `main/assets/` | 内置音频/图片资源 |

**关键设计模式**:
- **单例模式**: `Application::GetInstance()` 全局入口
- **事件驱动**: 通过 `EventGroupHandle_t` 的事件位触发状态切换
- **状态机模式**: `DeviceStateMachine` 管理设备生命周期
- **任务隔离**: Audio/Net/UI 各跑独立 FreeRTOS Task，通过 Queue 通信

### 2. ESP32-Bot (硬件验证项目)
- **路径**: `/Users/maxazure/projects/esp32-bot`
- **用途**: 同一硬件板的验证项目，已成功烧录运行
- **关键参考**: `CLAUDE.md` (设备信息) / `ESP_DEVICE.md` (烧录指南) / `main/lilywarp/` (唤醒词引擎)
- **LilyWarp 唤醒词引擎**: 基于 DTW (动态时间规整) 的轻量级离线唤醒词检测 (16kHz, 特征维度 10)

## Issue 追踪

> 所有 Sprint 1-6 的 issue 及 bug fix issue 均已关闭。当前无 open issue。
> 使用 `gh issue list --state open | cat` 查看最新状态。

### 议题写作规范

**核心原则：所有议题必须包含验收标准 (Acceptance Criteria)，缺少验收标准的议题不得开始实现。**

每条验收标准必须是**可测量、可验证**的具体条件，严禁出现"正确运行"、"正常工作"等模糊表述。

---

#### Story 议题模板

```
标题: [动词开头的功能简述，如 "支持 Barge-in 打断 TTS 播放"]
标签: type: story, priority: high/medium/low, component: audio/websocket/ui/...

背景 (Context)
作为 [用户角色]，我希望 [执行某操作]，以便 [获得某种价值]。
[1-3 句话说明功能背景和动机]

功能描述 (Description)
- [具体功能点 1]
- [具体功能点 2，技术层不超过 3-5 条]

验收标准 (Acceptance Criteria)
- [ ] AC1: [具体且可验证的条件，例如: "Barge-in 触发后 100ms 内音频 Buffer 清空"]
- [ ] AC2: [...]
- [ ] AC3: [边界情况或回归条件，例如: "不触发 Barge-in 时，正常播放流程不受影响"]

技术备注 (Technical Notes) [可选]
- 涉及文件/组件: [...]
- 依赖 Issue: #xxx
```

---

#### Bug 议题模板

```
标题: [bug] [现象简述，如 "INMP441 采集到噪声无法识别人声"]
标签: type: bug, priority: critical/high/medium/low, component: ...

环境 (Environment)
- 固件版本 / ESP-IDF 版本: [...]
- 硬件板型 / 串口: [如 zhengchen-1.54tft / /dev/cu.wchusbserial1130]

重现步骤 (Steps to Reproduce)
1. [操作步骤 1]
2. [操作步骤 2]
- 预期结果: [...]
- 实际结果: [...]

日志 / 截图
[粘贴关键串口日志片段]

根本原因分析 (Root Cause) [如已知]
[简述根本原因]

验收标准 (Acceptance Criteria)
- [ ] AC1: [bug 消失的验证方法，例如: "INMP441 输出 RMS > 500 时 ASR 识别率 > 90%"]
- [ ] AC2: [...]
- [ ] AC3: 回归测试通过，不影响其他 I2S 功能（播放）
```

---

#### 优先级定义

| 优先级 | 适用场景 |
|--------|----------|
| `critical` | 固件崩溃 / 无法启动 / 无法联网 |
| `high` | 核心功能不可用，无降级方案 |
| `medium` | 功能异常但有绕行方案 |
| `low` | 体验问题、UI 细节、文档补充 |

### 已完成 Sprint 概览
- **Sprint 1**: 基础工程搭建、FreeRTOS 任务骨架、Wi-Fi/NTP、电池监控 (#25 #43 #26 #27)
- **Sprint 2**: WebSocket 长连接与消息路由 (#44 #45)
- **Sprint 3**: I2S 音频采集/播放、Opus 编解码、ESP-SR 唤醒词 (#46 #47 #49 #31)
- **Sprint 4**: 按键交互、状态机、语音联调 (#48 #33 #34)
- **Sprint 5**: LVGL 显示、电池 UI、动态表情渲染 (#28 #29 #30)
- **Sprint 6**: 提醒管理器与打断逻辑 (#35 #36)
- **Bug Fixes**: WebSocket 断连 (#53 #57)、状态机同步 (#58 #59 #61 #63)、音频稳定性 (#54 #55)、INMP441 采集 (#64 #66 #68)、表情系统 (#70 #72 #73)

## 踩坑知识库

> 遇到问题先查这里，详见 `notes/` 目录。

### 网络层
| 坑 | 根因 | 解决 | 笔记 |
|----|------|------|------|
| Docker DNAT 丢失 ESP32 TCP 响应 | iptables DNAT 与 ESP32 lwIP 不兼容 | 用 socat 转发绕过 Docker DNAT | [2026-03-03-audio-pipeline-debugging](notes/2026-03-03-audio-pipeline-debugging.md) |
| ESP32 DNS 解析公网域名失败 | DHCP handler 覆盖自定义 DNS | 开发环境直接用 IP，NVS 存储 IP | 同上 |
| esp_http_client 在 Docker 映射端口失败 | 高层封装掩盖底层问题 | 鉴权等关键路径改用 raw socket | 同上 |
| 服务端返回局域网 IP，外网 ESP32 无法连接 | `.env` 中 `UDP_PUBLIC_IP`/`MQTT_BROKER_URL` 硬编码局域网 IP | `curl -s https://api.ipify.org` 获外网 IP，更新配置后 **`docker compose up -d`** 重建（restart 不生效），路由器配 1885/TCP, 8765/UDP, 8800/TCP 端口转发 | [同上](notes/2026-03-03-audio-pipeline-debugging.md) |

### 内存管理
| 坑 | 根因 | 解决 | 笔记 |
|----|------|------|------|
| FreeRTOS task 创建失败（内部 RAM 不足）| `pvPortMalloc` 硬编码 `MALLOC_CAP_INTERNAL`，不用 SPIRAM | 大 stack 任务用 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` | [2026-03-03-audio-pipeline-debugging](notes/2026-03-03-audio-pipeline-debugging.md) |
| WiFi OOM → boot loop | Opus 编解码器 + 任务栈 + 流缓冲共占 ~56KB 内部 RAM，WiFi 需 50-70KB | `xTaskCreatePinnedToCoreWithCaps` + `xStreamBufferCreateWithCaps` 全用 PSRAM | [20260304_restore_opus_encoding](notes/20260304_restore_opus_encoding.md) |

### 音频链路
| 坑 | 根因 | 解决 | 笔记 |
|----|------|------|------|
| TTS 播放 1.67x 加速 | 服务端 Opus 编码丢弃尾部余量（40% 丢失）| `_send_audio_chunk()` 维护 `_pcm_remainder`，结束时 flush | [2026-03-03-audio-pipeline-debugging](notes/2026-03-03-audio-pipeline-debugging.md) / [20260304_audio_playback_speed_fixes](notes/20260304_audio_playback_speed_fixes.md) |
| AudioTest 录音加速 | AudioTest 与 CaptureTask 竞争同一 I2S DMA | `SetMicExclusive(true)`，禁止在非主流中 enable/disable I2S | [20260304_audio_playback_speed_fixes](notes/20260304_audio_playback_speed_fixes.md) |
| I2S Mono 模式不稳定 | ESP32-S3 I2S Mono 行为与文档不符 | 改 `I2S_SLOT_MODE_STEREO` + 软件 MonoToStereo | [2026-03-03-audio-pipeline-debugging](notes/2026-03-03-audio-pipeline-debugging.md) |
| Opus 编码器 error -5 | `bitrate=-1` 错误；应为 `ESP_OPUS_BITRATE_AUTO=-1000`；DTX 仅支持 VOIP 模式 | 查 `esp_opus_enc.h`；不要用 `esphome/micro-opus`，改用 `espressif/esp_audio_codec ~2.4.1` | [20260304_restore_opus_encoding](notes/20260304_restore_opus_encoding.md) |
| AUDIO_OUTPUT_SAMPLE_RATE 错误 | 误设为 24000（应为 16000）| 改为 16000；silence buffer 禁止硬编码 | [20260304_audio_playback_speed_fixes](notes/20260304_audio_playback_speed_fixes.md) |

### TTS 播放控制
| 坑 | 根因 | 解决 | 笔记 |
|----|------|------|------|
| 长 TTS / 多句回复中途停止 | drain 仅等 1s 空 buffer，`tts_end_callback_` 声明但未注册 | 注册 `OnTtsEnd()`；drain 改为等待 `tts_end` 信号，兜底 30s | [2026-03-03-tts-playback-drain-fix](notes/2026-03-03-tts-playback-drain-fix.md) |
| 多段 TTS 只播第一句 | 状态机缺 `speaking→thinking` 转换；第一句 `tts_end` 触发 drain 后续帧无人消费 | 增加合法转换；每次 `tts_start` 调用 `ResetTtsEnd()` | [2026-03-04-multi-segment-tts-fix](notes/2026-03-04-multi-segment-tts-fix.md) |
| same-state 转换不触发回调 | `TransitionTo()` 对 old==new 直接 return | 不能依赖同态转换触发回调，需主动 reset | [2026-03-04-multi-segment-tts-fix](notes/2026-03-04-multi-segment-tts-fix.md) |
| barge-in 后 `tts_end_received_` 残留 | `StopPlayback` 未重置 flag | `StopPlayback` 中重置 `tts_end_received_ = false` | [2026-03-03-tts-playback-drain-fix](notes/2026-03-03-tts-playback-drain-fix.md) |

### ASR / 语音识别
| 坑 | 根因 | 解决 | 笔记 |
|----|------|------|------|
| Whisper 中文同音字识别错误 | 前导 2.5s 静音干扰 Whisper；无 prompt | 服务端裁剪前导静音（RMS < 150）；添加 Whisper prompt 常见词 | [2026-03-04-asr-diagnostics](notes/2026-03-04-asr-diagnostics.md) |

### Docker / 开发环境
| 坑 | 根因 | 解决 |
|----|------|------|
| `docker compose restart` 不生效 | 只重启容器，**不会重新加载 `.env`** | 配置变更后必须用 `docker compose up -d` 重建容器 |
| 容器写文件权限错误 | 容器以 appuser(1001) 运行，目录是 root 创建的 | `chmod 777` 宿主目录 |
| 新 volume mount 不生效 | `restart` 不会 recreate 容器 | `docker compose down && up -d` |

### LVGL / UI
| 坑 | 根因 | 解决 |
|----|------|------|
| 新对象遮挡旧对象 | 后创建的 LVGL 对象默认在最前层 | `lv_obj_move_foreground()` 调整层级 |
| Face canvas 被波形遮挡 | 光调 z-order 不够 | `SetVisible(false)` + `move_foreground` |

### 硬件/烧录
| 坑 | 根因 | 解决 |
|----|------|------|
| 征辰板无法自动进入下载模式 | DTR 未连 GPIO0 | 手动 BOOT+RST；esptool 用 `--before no_reset`，去掉 `--after hard_reset` |
| CH340K RST 后串口断连丢失 boot 日志 | RST 触发 CH340K USB 重连 | pyserial 自动重连脚本 |
| `volatile++ ` C++20 deprecated | 编译警告 | 改为 `x = x + 1` |

## 重要规则
- 任务间严禁直接调用，全部通过 FreeRTOS Queue 传递消息
- 硬件端不做复杂语义识别，仅作为流媒体搬运工
- Barge-in 打断必须 100ms 内清空音频 Buffer
- 支持 7×24 小时运行，禁止内存泄漏
- 可以使用 sudo 权限
- esptool 请使用参数： -b 460800 --before no_reset  但是不要用 --after hard_reset， 因为它不能hard_reset
- **每轮对话完成后必须写 Note**: 如果在调试或开发过程中遇到了技术问题（坑），在问题解决或对话结束时，必须在 `notes/` 目录下新建或追加一条记录，格式为 `notes/YYYY-MM-DD-<简述>.md`，包含：现象、排查过程、根因、解决方案、教训。这些记录是团队知识库的一部分，帮助避免重复踩坑。

## GitHub 项目管理规范
- **Issue/PR 状态必须保持清洁**：所有已完成的工作必须关闭对应 issue，留下 comment 说明修复的 PR 编号
- **工作流程**：创建 issue → 创建分支 → 实现 → 代码审查 → 创建 PR → 合并 → 关闭 issue
- **PR 合并后**：必须在相关 issue 下留 comment，说明修复内容和 PR 编号
- **过时的 PR**：如果 PR 目标分支已过时或代码不一致，关闭并在新分支上重新修复
- **定期检查**：确保无遗留的 open issue/PR，保持项目追踪的健康状态
- **`gh` 命令注意事项**：使用前需 `gh auth switch --user sarahliuakl`；输出可能卡在 pager，始终 pipe 到 `| cat`


## 调试注意事项

- 刷机使用 ./flash.sh
- 监控输出log请使用 ./monitor.sh
