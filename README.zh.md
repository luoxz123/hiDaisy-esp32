# HiLily ESP32 设备端固件 (HiLily Device Firmware)

> **HiLily** — 用 AI 温暖每一段晚年时光。
> 本仓库为 HiLily 智能陪伴设备的硬件端 (ESP32) 固件代码，采用 FreeRTOS + C++ 架构开发。

**[English Documentation → README.md](README.md)**

---

## 核心产品愿景与架构路线

### 现状与挑战

当前业界的主流方案多为"语音 → 文本 → 大模型 → 文本 → 语音"的级联架构 (Cascade Pipeline)。这导致硬件端必须等待服务端完成一系列串行处理，网络 I/O 的叠加极易突破 500ms 的拟真交互延迟极限。

### 我们的目标

HiLily 的终极目标是实现**极低延迟、全双工、拟真自然**的语音对讲。面向独居老人用户群体，我们紧跟前沿技术（如 **GPT-4o Realtime**），积极探索**端到端原生多模态大模型 (E2E Audio-in/Audio-out)**，旨在跳过文本的编解码损耗，直接用音频流驱动情绪与对话。

### 未来方向：声学情绪感知

当前的情绪识别依赖文本 — LLM 从转录内容推断情感状态。我们的路线图目标是**声学情绪感知**，使 HiLily 能够：

- 检测**呻吟声或呼救声** — 独居老人可能的疼痛或不适信号
- 识别**声音颤抖或不稳定** — 焦虑、恐惧或身体虚弱的指标
- 感知**说话节奏与能量的变化** — 疲劳、激动或突发困境的迹象

这些声学信号对人类听者来说显而易见，但在资源受限的 ESP32 硬件上实现仍具挑战性。我们正在探索轻量级端侧模型（如基于 CNN 的 MFCC 分类器或量化 TinyML 模型）与服务端韵律分析的混合方案。

### 硬件架构设计原则

在原生端到端大模型完全成熟前，本固件配合 Server 端基于 **MQTT + UDP + Opus (AES-CTR 加密)** 的通信架构运行：

- **极简数据流**: 硬件端不做复杂音频语义识别，仅作为麦克风采集推流器与喇叭播放器
- **ESP-SR 音频前处理 (AFE)**: NSNet2 神经网络降噪 + VAD 语音活动检测 + WakeNet9 唤醒词检测
- **Always-on 麦克风**: 常开麦克风持续喂入 AFE，支持随时唤醒 ("嗨莉莉") 和实时 VAD 检测
- **MQTT 信令通道**: JSON 控制信令（hello 握手、listen/stop/interrupt 等）通过 MQTT 传输
- **UDP 音频通道**: Opus 编码音频通过 UDP 加密传输，低延迟、低开销
- **全双工打断 (Barge-in)**: 本地高优先级 GPIO 监听 + Jitter Buffer，随时丢弃残余音频并向服务端发送打断指令

---

## 技术栈与硬件

| 项目 | 详情 |
|------|------|
| **主控芯片** | ESP32-S3-N16R8 (双核 240MHz, 16MB Flash, 8MB PSRAM) |
| **操作系统** | FreeRTOS |
| **开发框架** | ESP-IDF v5.x |
| **语言** | C++ |
| **网络通信** | Wi-Fi, MQTT (信令), UDP (音频, AES-CTR 加密) |
| **音频** | I2S 麦克风 (INMP441, 16kHz Mono) + I2S 功放 (MAX98357A) |
| **音频编码** | Opus 上行 (esp\_audio\_codec, 60ms 帧, AES-CTR 加密), Opus 下行解码 |
| **加密** | AES-CTR via mbedTLS (ESP-IDF 内置) |
| **显示屏** | ST7789 TFT 1.54" 240×240 (SPI, 80MHz) |
| **UI 框架** | LVGL v8.x |
| **语音前处理** | ESP-SR AFE (NSNet2 降噪 + VAD 语音活动检测) |
| **语音唤醒** | ESP-SR WakeNet9 (离线, "嗨莉莉" / Hi Lily, 阈值 0.5) |

## 代码结构

```
hilily-esp32/
├── main/
│   ├── main.cc               # 入口: app_main()
│   ├── application.cc/h      # 应用框架: FreeRTOS 任务启动、事件循环、配网/状态协调
│   ├── device_state.h        # 设备状态 & 情绪枚举定义
│   ├── state_machine.cc/h    # 状态机: 状态流转 + 情绪回调 (含 intensity)
│   ├── msg_router.cc/h       # MQTT JSON 消息路由与分发
│   │
│   ├── mqtt_client.cc/h      # MQTT 客户端: HTTP 认证、broker 连接、JSON 发布/订阅
│   ├── udp_client.cc/h       # UDP 音频客户端: AES-CTR 加解密 + FreeRTOS 接收任务
│   ├── ws_client.cc/h        # WebSocket 客户端 (遗留，保留供参考)
│   │
│   ├── display_manager.cc/h  # LVGL 初始化、UI 任务、状态栏、开机/配网浮层
│   ├── face_renderer.cc/h    # 参数化机器人表情: 渲染引擎 + 30fps 动画
│   │
│   ├── audio_service.cc/h    # I2S 采集/播放、Opus 编解码 (esp_audio_codec)、Jitter Buffer
│   ├── afe_processor.cc/h    # ESP-SR AFE 音频前处理: NSNet2 降噪 + VAD + WakeNet9 唤醒词
│   ├── wifi_manager.cc/h     # Wi-Fi 连接管理、认证页配网、网页配置、NTP 时间同步
│   │
│   ├── button_manager.cc/h   # 物理按键 (GPIO0) 管理与防抖
│   ├── battery.cc/h          # 电池 ADC 读取与电量监控
│   ├── wake_word.cc/h        # ESP-SR 唤醒词检测 (遗留，已由 afe_processor 替代)
│   ├── reminder_manager.cc/h # 定时提醒管理器
│   │
│   ├── board.cc/h            # 硬件 BSP: SPI 总线、外设初始化
│   ├── pin_config.h          # GPIO 引脚定义
│   └── CMakeLists.txt        # 编译配置
│
├── docs/
│   ├── PRD_ESP32_Architecture.md   # 产品需求文档
│   └── API_SPECIFICATION.md        # 设备-服务端通信协议
│
├── partitions.csv            # 分区表 (NVS, OTA, app, model SPIFFS)
├── sdkconfig.defaults        # 核心配置 (PSRAM, LVGL, ESP-SR NSNet2 等)
└── tests/                    # 测试
```

## 通信架构

### 连接流程 (MQTT + UDP)

```
Wi-Fi 连接
  → HTTP POST /api/v1/device/auth        获取 MQTT Broker 凭据
  → MQTT Connect (broker 地址/用户/密码)
  → MQTT Publish {"type":"hello"}         触发服务端会话协商
  → MQTT Receive hello reply (UDP 地址 + AES 密钥)
  → UDP Connect + AES-CTR 初始化
音频上行: I2S 采集 → AFE 降噪 (NSNet2) → Opus 编码 (60ms/帧) → AES-CTR 加密 → UDP 发送
音频下行: UDP 收包 → AES-CTR 解密 → Opus 解码 (16kHz) → Jitter Buffer → I2S 播放
控制信令: MQTT device/{mac}/up (上行) / device/{mac}/down (下行)
```

### 消息类型

**MQTT 上行 (device → server)**:
`hello` · `start_dialogue` · `stop_dialogue` · `interrupt` · `client_pong`

**MQTT 下行 (server → device)**:
`hello` (含 UDP 参数) · `state_change` · `asr_result` · `emotion_update` · `reminder_notify` · `server_ping` · `error`

**UDP 音频包格式** (16 字节头 + AES-CTR 加密 Opus 载荷):
```
| type 1B | flags 1B | payload_len 2B | ssrc 4B | timestamp 4B | seq 4B | [encrypted opus] |
```

## 动态表情系统

设备端通过 `FaceRenderer` 实现完全计算生成的参数化机器人表情，无需任何图片资源：

- **造型**: 圆角矩形眼睛 + 嘴巴，女性化 kawaii 风格 (睫毛 + 圆形腮红)
- **6 种情绪**: neutral / happy / sad / comfort / surprise / thinking
- **情绪强度**: 服务端通过 MQTT `emotion_update` 消息下发情绪类型 + `intensity` (0.0–1.0)，支持渐变混合
- **30fps 动画**: 眨眼 (随机间隔)、呼吸 (正弦波微移)、水平漂移 (随机目标)
- **平滑过渡**: 所有参数变化通过 lerp 插值 (系数 0.18)，约 500ms 到达目标值

情绪数据流:
```
Server → MQTT → MsgRouter → StateMachine → DisplayManager → FaceRenderer
         (emotion_update JSON)              (SetEmotion)    (SetTargetEmotion)
```

## 语音处理管线

设备端通过 ESP-SR AFE (Audio Front-End) 框架构建统一的音频前处理管线：

### 上行链路 (采集 → 服务端)
```
INMP441 麦克风 (I2S, 16kHz/32-bit Mono)
  → CaptureTask: 32→16bit 转换 (>>12, 饱和裁剪)
  → AfeProcessor: AFE(VC) 管线
    ├─ NSNet2 神经网络降噪 (单通道, 无 AEC)
    ├─ VAD 语音活动检测 → Application 回调
    └─ WakeNet9 唤醒词检测 ("嗨莉莉", 阈值 0.5)
  → OpusEncodeTask: esp_audio_codec Opus 编码 (60ms 帧, 16kHz)
  → UdpClient: AES-CTR 加密 → UDP 发送
```

### 下行链路 (服务端 → 播放)
```
UDP 收包 → AES-CTR 解密
  → MsgRouter: 音频包分发至下行队列
  → PlaybackTask: Jitter Buffer (预缓冲 5 帧)
    → esp_audio_codec Opus 解码 (16kHz/60ms)
    → I2S 输出 (MAX98357A 功放)
```

### 双监听模式

| 模式 | 触发方式 | 停止条件 |
|------|---------|---------|
| **自动 (唤醒词)** | "嗨莉莉" 离线检测 | VAD 静音 ≥1.5s 自动停止，无需按键 |
| **手动 (按键)** | GPIO0 按键 | 下一次按键停止 — 适合长时间独白 |

### 分段上传 (长对话)

对于较长的录音（如按键模式长时间讲话），固件以分段方式发送音频而非一次性发送全部数据：

- 超过 30 秒后，遇到 ≥1s VAD 静音时触发**分段边界**
- ESP32 通过 MQTT 发送 `{"type":"listen","state":"segment","segment":N}`
- 服务端对每段分别执行 ASR，合并转录结果
- 最终 `listen(stop)` 将完整多段文本统一发给 LLM 处理
- 防止 Groq ASR 60s 超时，同时保留完整对话上下文

### 唤醒词与 VAD
- **Always-on 麦克风**: CaptureTask 持续读取 I2S 并喂入 AFE，支持随时唤醒
- **WakeNet9 模型**: `wn9_hilili_tts` (嗨莉莉)，DET_MODE_90 + 阈值 0.5，适配多种口音
- **VAD 回调**: 实时通知 Application 语音/静音状态，用于自动停止录音
- **模型存储**: SPIFFS 分区 (`model`, 1MB @ 0x610000)

## 核心特性

1. **FreeRTOS 多任务架构**
   - Audio 采集/播放 (Core 1)、AFE 处理 (Core 1)、Opus 编码 (Core 0)、UI 渲染 (Core 0)、网络通信等各跑独立 Task
   - 任务间严禁直接调用，通过 FreeRTOS Queue 和 Mutex 通信

2. **MQTT + UDP 双通道通信**
   - MQTT: JSON 控制信令 (`hello`, `start_dialogue`, `emotion_update`, `server_ping` 等)
   - UDP: AES-CTR 加密 Opus 音频流 (低延迟、低开销)

3. **ESP-SR 音频前处理 (AFE)**
   - NSNet2 神经网络降噪: 单麦克风 (M 格式)，无 AEC (本板无硬件参考回路)
   - VAD 语音活动检测: 实时语音/静音状态回调
   - WakeNet9 唤醒词: "嗨莉莉" 离线检测 (阈值 0.5，适配多种口音)
   - Always-on 麦克风: 全时段监听，支持随时唤醒

4. **全双工打断 (Barge-in)**
   - 按键/唤醒词触发后 100ms 内清空音频 Buffer 停止发声
   - 立即通过 MQTT 上报 `interrupt` 指令

5. **流式音频抗抖动 (Jitter Buffer)**
   - 65KB 环形缓冲区，预缓冲 5 帧后开始播放，抵抗弱网环境下的音频撕裂

6. **Opus 编解码 (esp\_audio\_codec)**
   - 上行: 16kHz/60ms 帧/自动码率/DTX+VBR，替代已弃用的 micro-opus
   - 下行: 16kHz/60ms 帧，服务端 TTS 流式解码（含 carry-over buffer 防帧对齐丢帧）

7. **双监听模式**
   - 唤醒词模式: VAD 静音自动停止
   - 按键模式: 两次按键之间持续录音，支持分段上传

8. **OTA 固件远程升级**
   - 启动时 + 每小时自动检查服务端最新版本 (`GET /api/v1/ota/check`)
   - 语义化版本比较 (如 1.2.0 > 1.1.0)
   - 下载过程中屏幕全屏显示进度 (Overlay + 百分比)
   - 下载失败自动重试 3 次；最终失败恢复正常运行
   - 支持服务端 MQTT 推送 OTA：`{"type": "ota", "url": "...", "version": "..."}`
   - 双 OTA 分区 (ota_0 / ota_1, 各 3MB)，自动回滚保护
   - 管理员固件管理：上传 `POST /api/v1/ota/upload`，下载 `/api/v1/ota/firmware/{version}`

9. **WiFi 热点配网**
   - 首次启动或没有已保存 WiFi 时，会重启进入专用配网热点，而不是在运行中直接切 WiFi 模式
   - 手机连接 `HiLily-XXXX` 后会优先弹出认证页；如果系统没有自动弹出，可手动打开 `http://192.168.4.1`
   - 设备屏幕会持续显示配网引导浮层，包含热点名，直到 WiFi 配置成功
   - 网页支持扫描附近 WiFi、保存/删除设备内已保存网络，设备侧最多保存 3 条记录

---

## WiFi 配网流程

HiLily 对首次配网，以及已保存 WiFi 都连接失败的情况，使用专用 AP 配网启动路径。

### 何时进入配网

- 设备里没有任何已保存 WiFi
- 设备里有已保存 WiFi，但所有已保存热点都连接失败
- 待机时长按 `BOOT` 键 3 秒会先尝试连接已保存 WiFi；只有保存热点连接失败时才进入配网热点

### 用户操作流程

1. 屏幕会显示持续常驻的配网提示，包含热点名。
2. 手机连接热点 `HiLily-XXXX`。
3. 系统通常会自动弹出配网页；如果没有弹出，手动访问 `http://192.168.4.1`。
4. 可以直接使用已保存网络、扫描附近 WiFi，或手动输入 SSID/密码。对已保存 SSID 留空密码会复用设备内保存的密码。
5. 保存网络。首次配网时，保存第一条网络后网页会提示重启。

### 说明

- 配网时使用 `WIFI_MODE_APSTA`，这样热点网页可用的同时，`/scan` 也能主动扫描附近 AP。
- 页面里的 `Saved Networks (stored on device)` 指的是设备内已保存的网络，不是实时扫描结果。
- 设备最多保存 3 条 WiFi 记录；在网页里删除后，对应凭据也会从 NVS 中移除。
- 选择已保存网络会复用保存的密码；只有旧密码失效时才需要输入新密码。
- 设备已经连上 WiFi 时，长按不会打开配网热点。
- 配网页会直接进入 WiFi 设置页面，不再要求输入 PIN。
- 配网页依赖 DNS 劫持和通配 HTTP 重定向，兼容常见手机系统的 captive portal 探测请求。

## 编译与烧录

### 1. 环境准备

安装 [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/index.html)，然后:
```bash
source $HOME/.espressif/v5.5.3/esp-idf/export.sh
```

### 2. 编译

```bash
cd hilily-esp32
idf.py set-target esp32s3
idf.py build
```

### 3. 烧录

> 本开发板不支持自动进入下载模式 (DTR 未连接 GPIO0)，需手动操作：
> **按住 BOOT → 按一下 RST → 松开 BOOT**

```bash
cd build
python -m esptool --chip esp32s3 -p /dev/cu.wchusbserial1130 -b 460800 --before no_reset \
  write_flash @flash_args
```

### 4. 串口监控

```bash
idf.py -p /dev/cu.wchusbserial1130 monitor
```

## 配合 Server 端联调

硬件端开发强依赖于 [HiLily Server 接口规范](https://github.com/sarahliuakl/hilily-server/blob/main/docs/API_SPECIFICATION.md)。在进行 MQTT 握手或 UDP 音频流测试时，请确保两端的 JSON Schema 与 UDP 包头规范完全对齐。

服务端仓库: [sarahliuakl/hilily-server](https://github.com/sarahliuakl/hilily-server)

## 团队

- **Alex (Hardware Lead)**: 前版本硬件方案设计与交付
- **Sarah (Product Lead)**: 产品需求与 API 协议设计、项目管理；当前版本 ESP32 固件全栈开发
