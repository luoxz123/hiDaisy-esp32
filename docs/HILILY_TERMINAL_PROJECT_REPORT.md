# HiLily 智能终端项目报告

> 报告版本：1.0  
> 基线日期：2026-10-06  
> 终端项目：`hilily-esp32`  
> 目标硬件：ESP32-S3-N16R8 / zhengchen-1.54tft-wifi  
> 当前 OTA 固件版本：`1.2.0`

## 1. 报告目的与范围

本报告描述 HiLily 智能陪伴终端当前已经实现的硬件、固件、网络、音频、交互和运维能力，并定义终端与服务端合并时需要统一的接口契约、联调步骤和验收标准。

报告内容以当前仓库代码为主要事实来源，同时参考 `README.zh.md`、`docs/` 和 `notes/`。由于本地部分早期文档仍描述 WebSocket、WakeNet 和本地 VAD，本报告在相关冲突处以当前参与构建的代码为准，并单独列出文档差异。

本报告不展开服务端内部 ASR、LLM、TTS、文本翻译、内容检索和用户管理的具体实现。Azure AI Service、Speech 和 Translator endpoint 属于服务端内部依赖，不应由终端直接调用或保存其密钥。

## 2. 项目摘要

HiLily 终端面向老年用户，是云端智能服务的语音和视觉交互入口。终端负责采集、前处理、编码和上传语音，播放服务端返回的音频，显示状态与情绪，管理 Wi-Fi、激活凭证、本地提醒、电池状态和 OTA。复杂语义理解、内容检索和语音生成保留在服务端。

当前运行主链路为：

```text
终端 Wi-Fi
  -> HTTP 注册/激活/鉴权
  -> MQTT 控制信令
  -> hello 协商 UDP 地址和临时 AES 参数
  -> UDP 双向传输 Opus 音频
  -> 服务端 ASR / LLM / TTS
```

当前固件已经具备端到端联调所需的主要能力，但仍存在协议文档滞后、自动化测试滞后、明文公网入口和 OTA 完整性校验不足等问题。因此当前状态适合持续集成测试和功能演示，尚不能视为完成生产安全加固。

## 3. 系统边界与职责

| 能力 | 终端职责 | 服务端职责 |
|---|---|---|
| 设备身份 | 读取 STA MAC、保存 `device_token` | 注册、激活、签发 token 和临时 MQTT 凭据 |
| 网络接入 | Wi-Fi 连接、配网、漫游、断线重连 | 提供公网 HTTP、MQTT、UDP 入口 |
| 语音采集 | I2S 采集、AEC、降噪、Opus 编码 | VAD、ASR、对话编排 |
| 语音播放 | UDP 接收、Opus 解码、缓冲、音量、I2S 播放 | TTS 流生成和发送 |
| 唤醒 | 本地 Daisy microWakeWord | 接收 `listen.detect` 并建立会话 |
| 打断 | 按键淡出、清缓冲、发送 `abort`；持续上传 AEC 音频 | Silero VAD 检测语音打断并停止 LLM/TTS |
| UI | 状态、字幕、情绪、波形、电量、配网/错误提示 | 下发状态、情绪、STT 文本和配置 |
| 提醒 | NVS 缓存、时间扫描、蜂鸣和触发上报 | 创建、取消、同步提醒并生成播报内容 |
| OTA | 检查、下载、写入备用分区、回滚保护 | 固件版本管理、发布和下载接口 |
| 内容能力 | 播放服务端结果 | 图书、DAISY、新闻、播客、天气和翻译等业务逻辑 |

明确边界：

- 终端不直接调用 Azure OpenAI、Azure AI Service、Speech 或 Translator。
- 终端不保存 Azure 服务密钥。
- MQTT broker 地址和 UDP 参数由服务端动态返回，终端不硬编码这两类运行时参数。
- `main/ws_client.cc` 是遗留参考代码，没有加入当前硬件构建源文件，不属于现行主链路。
- `frontend/` 是 DAISY 图书搜索的辅助测试界面，不属于 ESP32 固件运行时。

## 4. 硬件基线

### 4.1 核心硬件

| 项目 | 配置 |
|---|---|
| 主控 | ESP32-S3-N16R8，双核 240 MHz |
| Flash | 16 MB |
| PSRAM | 8 MB Octal PSRAM，80 MHz |
| 屏幕 | ST7789，1.54 英寸，240 x 240，SPI 80 MHz |
| 麦克风 | INMP441，I2S，16 kHz，单声道 |
| 功放 | MAX98357A，I2S |
| USB 转串口 | CH340K，VID `1A86`，PID `7523` |
| 主按键 | BOOT / GPIO0，支持短按和 3 秒长按 |
| 辅助按键 | 音量加 GPIO10，音量减/音频测试 GPIO39 |
| 电池检测 | GPIO1，通过 ADC1 通道映射读取 |

### 4.2 GPIO 映射

| 功能 | GPIO |
|---|---:|
| LCD MOSI | 41 |
| LCD SCLK | 42 |
| LCD CS | 21 |
| LCD DC | 40 |
| LCD RST | 45 |
| LCD 背光 | 20 |
| Speaker DOUT | 7 |
| Speaker BCLK | 15 |
| Speaker LRCK | 16 |
| Microphone SD | 6 |
| Microphone WS | 4 |
| Microphone SCK | 5 |
| BOOT 按键 | 0 |
| 音量加 | 10 |
| 音量减/音频测试 | 39 |
| 电池 ADC | 1 |

## 5. 固件技术基线

| 项目 | 当前值 |
|---|---|
| 语言 | C++ |
| 框架 | ESP-IDF 5.x；最近构建元数据为 5.5.3 |
| 操作系统 | FreeRTOS |
| UI | LVGL 8.4 |
| 音频编解码 | `espressif/esp_audio_codec ~2.4.1` |
| 音频前端 | `espressif/esp-sr ~2.3.0` |
| 唤醒推理 | TFLite Micro 1.3.3、ESP-NN、micro-speech-features |
| 控制通道 | ESP-MQTT |
| 音频通道 | UDP + AES-CTR + Opus |
| 配置存储 | NVS |
| 固件升级 | 双 OTA 分区 + rollback |
| 硬件模拟 | QEMU mock，可替换硬件和网络实现 |

最近保留的构建产物为 `build/hilily-esp32.bin`，大小 2,336,608 字节。每个应用分区为 3 MB，按该产物计算约剩余 809,120 字节，即约 25.7%。该构建产物时间为 2026-08-28，只能证明当时构建完成，不能替代本次报告基线的重新编译和真机回归。

### 5.1 Flash 分区

| 分区 | 类型 | 偏移 | 大小 | 用途 |
|---|---|---:|---:|---|
| `nvs` | data/nvs | `0x9000` | 16 KB | Wi-Fi、token、设置、提醒 |
| `otadata` | data/ota | `0xd000` | 8 KB | OTA 状态 |
| `phy_init` | data/phy | `0xf000` | 4 KB | RF 校准数据 |
| `ota_0` | app/ota_0 | `0x10000` | 3 MB | 应用槽 A |
| `ota_1` | app/ota_1 | `0x310000` | 3 MB | 应用槽 B |
| `model` | data/spiffs | `0x610000` | 1 MB | ESP-SR 模型分区 |
| `storage` | data/spiffs | `0x710000` | 约 8.94 MB | 预留存储 |

## 6. 软件架构

### 6.1 主要模块

| 模块 | 文件 | 主要职责 |
|---|---|---|
| 应用协调 | `main/application.*` | 初始化、事件循环、状态和各模块编排 |
| 状态机 | `main/state_machine.*` | 设备状态和情绪状态的合法流转 |
| Wi-Fi | `main/wifi_manager.*` | 多热点、配网、漫游、NTP、重连 |
| HTTP/MQTT | `main/hilily_mqtt_client.*` | 注册、激活、鉴权、MQTT 发布订阅 |
| UDP | `main/udp_client.*` | Opus 包封装、AES-CTR、收发和重放序号检查 |
| 消息路由 | `main/msg_router.*` | MQTT JSON 解析、UDP 音频转发 |
| 音频服务 | `main/audio_service.*` | I2S、Opus、缓冲、播放、淡出、AEC 参考 |
| AFE | `main/afe_processor.*` | AEC、NS、RMS 语音/静音回退检测 |
| Daisy 唤醒 | `main/micro_wake_word.*` | 本地量化模型特征提取和流式推理 |
| 显示 | `main/display_manager.*` | LVGL、状态栏、字幕、波形、Overlay |
| 表情 | `main/face_renderer.*` | 参数化表情和动画 |
| 电池 | `main/battery.*` | ADC、预热、中值滤波、低电确认 |
| 提醒 | `main/reminder_manager.*` | NVS 持久化、到期扫描和触发 |
| OTA | `main/ota_manager.*` | 版本检查、下载、分区切换、回滚 |
| 设置 | `main/settings.*` | 音量、亮度、时区的 NVS 持久化 |

### 6.2 任务和队列

| 任务 | Core | 优先级 | 栈 | 说明 |
|---|---:|---:|---:|---|
| `audio_in` | 1 | 10 | 8 KB PSRAM | 常开麦克风采集 |
| `afe_proc` | 1 | 8 | 8 KB PSRAM | AEC/NS 处理 |
| `audio_out` | 1 | 10 | 16 KB PSRAM | Opus/PCM 播放 |
| `opus_enc` | 0 | 2 | 24 KB PSRAM | 60 ms Opus 编码 |
| `mww_daisy` | 0 | 2 | 4 KB PSRAM | Daisy 唤醒推理 |
| `udp_rx` | 0 | 11 | 8 KB PSRAM | UDP 接收和解密 |
| `audio_wr` | 0 | 8 | 4 KB | 下行音频队列转发 |
| `msg_router` | 0 | 5 | 4 KB | MQTT JSON 路由 |
| `ui` | 0 | 5 | 16 KB PSRAM | LVGL 刷新和动画 |
| `button` | 0 | 5 | 2 KB | 按键事件和防抖 |
| `reminder` | 0 | 1 | 4 KB | 每分钟扫描提醒 |
| `battery` | 0 | 1 | 6 KB PSRAM | 每秒读取电池快照 |

关键资源：

- 采集到编码使用 32 KB PSRAM StreamBuffer。
- 扬声器到 AEC 使用 12.8 KB 参考 StreamBuffer，约 400 ms。
- 下行音频队列最多 4096 个 Opus chunk，播放前预缓冲 5 帧。
- `MsgRouter` 使用 16 项 JSON 队列和 2048 项 PSRAM 音频写队列。
- microWakeWord 使用 8 个 10 ms PCM 帧的短队列，满时只淘汰最旧帧。

## 7. 启动与连接流程

```text
上电
  -> 初始化 NVS、设置、OTA 状态
  -> 初始化板级硬件、显示、电池、音频、按键、提醒
  -> 优先初始化 Wi-Fi，避免 AFE/AEC 抢占内部 RAM
  -> 初始化 AFE 和 Daisy microWakeWord
  -> 进入 wifi_connecting
  -> 尝试最多 3 个已保存热点
      -> 成功：NTP + TCP 探测 4.197.40.220:8000
      -> 全部失败：重启进入 HiLily-XXXX 配网热点
  -> 无 device_token：注册并显示激活码，轮询激活状态
  -> POST /api/v1/device/auth
  -> 使用返回值连接 MQTT
  -> MQTT connected 后发送 hello(features.aec=true)
  -> 收到 hello 后建立 UDP 会话
  -> 进入 idle，启用 Daisy 唤醒
```

Wi-Fi 已连接不等于对话链路可用。固件只有在 MQTT connected 回调到达后才进入 `idle`，从而避免在 MQTT 未就绪时启动录音并出现 `listen(start) send failed`。

## 8. 网络和云端配置

### 8.1 终端当前入口

| 通道 | 地址/来源 | 用途 |
|---|---|---|
| HTTP | `http://4.197.40.220:8000`，固件硬编码 | 健康、注册、激活、鉴权、OTA |
| MQTT | 鉴权响应 `mqtt_broker` | 控制信令；当前部署预期 `mqtt://4.197.40.220:1884` |
| UDP | MQTT `hello.udp` | 音频；当前部署预期 `4.197.40.220:8765` |
| 配网页 | `http://192.168.4.1` | 本地 Wi-Fi 配置 |

终端启动时会对 `4.197.40.220:8000` 做最大 5 秒的 TCP 探测。失败后保持 `wifi_connecting`，显示提示并每 30 秒重试；用户也可在该状态短按按键立即重试。

### 8.2 服务端内部 Azure 依赖

以下 endpoint 应配置在服务端，不进入终端固件：

| 服务 | Endpoint |
|---|---|
| Azure AI Service | `https://realtime-tts.services.ai.azure.com/` |
| Azure OpenAI | `https://realtime-tts.openai.azure.com/` |
| Speech to Text | `https://australiaeast.stt.speech.microsoft.com` |
| Text to Speech | `https://australiaeast.tts.speech.microsoft.com` |
| Text Translation | `https://api.cognitive.microsofttranslator.com/` |

服务端合并报告应补充每项服务的 deployment/model、API version、区域、超时、重试、配额和降级策略，但不得记录实际密钥。

## 9. HTTP 接口契约

### 9.1 首次注册

```http
POST /api/v1/device/register
Content-Type: application/json

{"mac_address":"dc:b4:d9:24:3e:f0"}
```

终端期望 `200`：

```json
{
  "activation_code": "123456",
  "expires_in": 86400
}
```

`409` 表示设备已经注册，终端随后直接查询激活状态。

### 9.2 激活状态

```http
GET /api/v1/device/register/status?mac=dc:b4:d9:24:3e:f0
```

终端读取：

```json
{
  "status": "pending|active",
  "device_token": "returned-when-active"
}
```

激活状态轮询间隔为 10 秒；激活成功后 `device_token` 写入 NVS 的 `device/device_token`。鉴权返回 `device_pending_activation` 时采用另一条 30 秒轮询路径。

### 9.3 MQTT 鉴权

```http
POST /api/v1/device/auth
Content-Type: application/json

{
  "mac_address": "dc:b4:d9:24:3e:f0",
  "device_token": "..."
}
```

终端期望响应：

```json
{
  "mqtt_broker": "mqtt://4.197.40.220:1884",
  "mqtt_client_id": "device-dcb4d9243ef0",
  "mqtt_user": "...",
  "mqtt_password": "...",
  "publish_topic": "device/dc:b4:d9:24:3e:f0/up",
  "subscribe_topic": "device/dc:b4:d9:24:3e:f0/down",
  "expires_in": 300,
  "device_token": "optional-rotated-token"
}
```

注意：`Authenticate()` 的 C++ 签名仍带 `firmware_version` 和 `battery_level`，但当前请求体没有发送这两个字段。服务端不得依赖它们，除非双方同步修改并测试。

### 9.4 OTA 检查

```http
GET /api/v1/ota/check?device_id={MAC}&current_version=1.2.0
```

终端读取 `has_update`、`version` 和 `url`。`size`、`md5` 即使返回，当前固件也没有用于下载后校验。

## 10. MQTT 协议契约

### 10.1 终端上行消息

| `type` | 关键字段 | 触发场景 |
|---|---|---|
| `hello` | `features.aec=true` | MQTT 连接成功 |
| `listen` | `state=start`, `mode=auto|manual` | 按键开始监听 |
| `listen` | `state=detect`, `text=嗨莉莉`, `mode=auto` | Daisy 本地唤醒 |
| `listen` | `state=segment`, `segment=N` | 长录音分段 |
| `listen` | `state=stop`, `segments=N` 可选 | 结束录音 |
| `abort` | `reason=barge-in` | 播放期间按键打断 |
| `goodbye` | 无 | 主动结束会话 |
| `client_pong` | 无 | 回复服务端心跳 |
| `reminder_triggered` | `reminder_id` | 本地提醒到期 |
| `low_battery` | `battery`, `voltage_mv`, `snapshot_sequence` | 确认低电且设备空闲 |

### 10.2 服务端下行消息

| `type` | 终端读取字段 | 终端行为 |
|---|---|---|
| `hello` | `udp`, `timezone_offset`, `audio_params` | 建立 UDP、更新时间和音频格式 |
| `state_change` | `state` | 更新状态机和音频行为 |
| `emotion_update` | `emotion`, `intensity` | 更新参数化表情 |
| `tts` | `state=start|end`, `text` 可选 | 重置或触发播放 drain |
| `server_ping` | 无 | 返回 `client_pong` |
| `audio_meta` | `format`, `sample_rate`, `channels`, `bit_depth` | 设置 PCM/Opus 解码方式 |
| `stt` | `text` | 在屏幕显示识别文本 |
| `config` | `volume`, `brightness`, `timezone_offset` | 写入 NVS 并应用 |
| `ota` | `url`, `version` | 异步启动 OTA |
| `reminder_created` | `reminder_id`, `time`, `content` | 添加或更新本地提醒 |
| `reminder_cancelled` | `reminder_id` | 删除本地提醒 |
| `reminder_notify` | `reminder_id` | 进入提醒状态的备份通道 |
| `end_dialogue` | `reason` | 关闭连续对话模式 |
| `error` | `code`, `message` | 记录服务端错误 |

当前路由只接受 `state_change`，不再兼容旧 `status_update` 或 `status`。服务端合并时必须以 `state_change` 为唯一状态消息名称。

### 10.3 `hello` 的 UDP 协商字段

```json
{
  "type": "hello",
  "udp": {
    "server": "4.197.40.220",
    "port": 8765,
    "key": "32-hex-characters",
    "nonce": "32-hex-characters"
  },
  "timezone_offset": 720,
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1
  }
}
```

`key` 和 `nonce` 必须都是 16 字节对应的 32 位十六进制字符串。UDP 地址必须是终端所在公网能够访问的地址，不能返回 Docker 容器地址或服务器局域网地址。

## 11. UDP 音频协议

每个 UDP 包由 16 字节明文头和 AES-CTR 加密载荷组成：

```text
| type 1B | flags 1B | payload_len 2B | ssrc 4B | timestamp 4B | sequence 4B | ciphertext |
```

字段使用网络字节序。当前定义：

- `type=0x01` 表示音频。
- `flags=0x00`。
- `payload_len` 不包含 16 字节头。
- `sequence` 单调递增，终端会丢弃小于已接收序号的旧包。
- 16 字节包头同时作为 AES-CTR 每包 nonce 输入。
- 当前没有消息认证标签，篡改检测能力不足，详见安全风险章节。

音频上行格式为 16 kHz、16-bit、Mono，经 Opus 60 ms 帧编码。下行推荐保持同样参数，并在 `hello.audio_params` 或 `audio_meta` 明确声明。

## 12. 音频处理链路

### 12.1 上行

```text
INMP441 32-bit I2S
  -> 右移 12 位并饱和为 16-bit PCM
  -> AFE 双通道 MR 输入：mic + speaker reference
  -> AEC VOIP_LOW_COST
  -> NSNet2，缺失时回退 WebRTC NS
  -> 录音状态写入编码 StreamBuffer
  -> Opus 16 kHz / Mono / 60 ms / VBR / AUDIO mode
  -> UDP AES-CTR
```

当前 Opus 参数：自动码率、VBR 开启、DTX 关闭、每帧 960 samples。采集任务始终读取 I2S，录音开关仅决定是否把 AFE 输出送入 Opus 编码器，从而保证待机唤醒能力。

### 12.2 下行

```text
UDP 收包
  -> 序号检查
  -> AES-CTR 解密
  -> MsgRouter PSRAM 队列
  -> 5 帧预缓冲
  -> Opus 解码或 PCM 路径
  -> 软件音量曲线
  -> 16-bit 转 32-bit I2S
  -> MAX98357A
```

服务端发送 `tts.end` 后，终端不是立即停止，而是等待队列排空再关闭播放。多段 TTS 每次 `tts.start` 会重置结束标志，避免前一段的 drain 提前截断后一段。

### 12.3 AEC 和打断

播放任务把经过音量缩放的 PCM 写入扬声器参考 StreamBuffer。采集任务将麦克风和参考音频交织送入 AFE。终端在 speaking 状态继续上传 AEC 后音频，服务端使用 Silero VAD 判断用户是否说话。

按键打断流程：

```text
按键
  -> MQTT abort(reason=barge-in)
  -> 当前播放帧线性淡出
  -> 清空待播放队列和缓冲
  -> 进入 listening
```

## 13. 唤醒、监听和连续对话

当前本地唤醒器不是 WakeNet9，而是嵌入固件的 `daisy_stream_state_internal_quant.tflite`：

- 输入：16 kHz、16-bit、Mono，来自 AEC/NS 后的 PCM。
- 模型输入：`int8[1,3,40]`。
- 模型输出：`uint8[1,1]`。
- 特征步长：10 ms，窗口 30 ms。
- 概率阈值：0.97。
- 滑动窗口：5。
- 推理仅在真正 idle 且扬声器停止后启用。
- 模型缺少官方 manifest，以上前端参数属于当前部署假设，仍需真实外壳环境的 FAR/FRR 数据校准。

监听模式当前实际行为：

| 触发 | 模式 | 结束方式 |
|---|---|---|
| Daisy 唤醒 | auto + continuous | RMS 静音 1.5 秒后停止，TTS 播完 500 ms 后重新监听 |
| idle 短按 | auto + continuous | 同上；再次按键可退出 |
| speaking 短按 | barge-in | 淡出当前播放并进入 listening |

当前 AFE VAD 已关闭，服务端 Silero VAD负责打断检测。终端的自动停止使用 AEC/NS 输出的 RMS 阈值回退逻辑，并非 ESP-SR VAD。

长录音超过 30 秒后，会在检测到至少 1 秒自然停顿时发送 `listen.segment`。auto 模式硬超时 10 分钟，manual 安全上限 30 分钟。当前 UI/按键默认路径主要进入 auto continuous，`manual` 代码路径保留但不是常规入口。

## 14. 设备状态机和 UI

设备状态包括：

```text
starting -> wifi_connecting -> idle
idle <-> listening -> thinking -> speaking -> idle
任何主要状态 -> reminding
任何状态 -> error 或 wifi_connecting
```

服务端控制状态时应遵循终端允许的转换。终端允许 speaking 与 thinking/listening 之间切换，以支持多段回复和打断。

UI 提供：

- 启动水波动画。
- Wi-Fi、时间和五格电池状态。
- listening 波形。
- STT 文本显示。
- 全屏配网、激活、服务器错误、OTA 和低电 Overlay。
- neutral、happy、sad、comfort、surprise、thinking、listening、reminding 等参数化表情。
- 表情强度 0.0 到 1.0 和平滑插值动画。

## 15. Wi-Fi 与配网

终端最多保存 3 个热点。密码和 SSID 保存于 NVS。启动时按保存顺序连接，并使用全信道扫描和信号排序。连接后每 10 秒检查 RSSI；连续 3 次低于 -75 dBm 时扫描候选热点，只有候选至少强 10 dB 才漫游。

配网规则：

- 没有保存热点时直接进入 `HiLily-XXXX` 开放热点。
- 有保存热点时先尝试所有保存项，每个热点初次启动快速重试 2 次。
- 所有保存热点失败后才重启进入手动配网。
- 已连接时长按 BOOT 只显示当前 IP，不进入配网。
- 未连接但有保存热点时长按 BOOT 会先重启重试保存热点，失败后才进入配网。
- 配网页不要求 PIN。
- 对已有 SSID 提交空密码时复用已保存密码；只有连接失败或用户明确修改时才需重新输入。

配网 AP 当前为开放网络，网页可扫描、增加、删除网络并重启设备。该设计降低了老年用户配置门槛，但需要在部署和安全章节明确物理接近风险。

## 16. 电池、提醒和本地设置

### 16.1 电池

- ADC 有效范围：2500 到 5000 mV，范围外读数不发布。
- 启动丢弃 5 个有效样本，再收集 5 点窗口取中值。
- 3000 mV 映射 0%，4200 mV 映射 100%。
- 连续 3 个有效快照低于或等于 20% 才确认低电。
- UI、Overlay 和 MQTT 使用同一带 sequence 的稳定快照。

当前板卡电池分压比例和 GPIO1 实际连线仍需真机确认。历史日志曾在映射修正后继续出现 62 到 88 mV 的无效值，因此软件会忽略异常值，但不能代替硬件校准。

### 16.2 提醒

- 最多保存 10 条提醒。
- 使用 NVS blob 持久化。
- 每 60 秒扫描一次到期提醒。
- 到期后播放 3 次本地提示音并上报 `reminder_triggered`。
- 服务端可用 `reminder_created`、`reminder_cancelled` 和 `reminder_notify` 同步。

### 16.3 设置

| 设置 | 默认值 | 范围/说明 |
|---|---:|---|
| 音量 | 70 | 0 到 100，二次曲线缩放 |
| 屏幕亮度 | 75 | 0 到 100 |
| 时区偏移 | UTC+8 | -720 到 +840 分钟；hello/config 可更新 |

## 17. OTA 与发布

终端启动鉴权成功后检查一次更新，之后每小时检查。服务端也可以通过 MQTT `ota` 消息主动触发。下载写入非当前 OTA 分区，完成后切换启动分区并重启；新固件启动后调用 `MarkValid()` 取消回滚。

烧录板卡不能可靠自动进入或退出下载模式：

1. 按住 BOOT。
2. 按一下 RST。
3. 松开 BOOT。
4. 使用 `./flash.sh`，参数必须包含 `--before no_reset --after no_reset`。
5. 烧录完成后手动按 RST。

监控使用 `./monitor.sh`。CH340K 在复位时会 USB 重连，脚本会循环重连并保存日志。

## 18. 持久化数据清单

| NVS namespace/key | 数据 | 生命周期 |
|---|---|---|
| `wifi/ssid0..2` | SSID | 配网保存，最多 3 条 |
| `wifi/pass0..2` | Wi-Fi 密码 | 配网保存 |
| `wifi/force_ap` | 下次启动强制配网标志 | 启动消费后删除 |
| `wifi/try_saved` | 下次启动先试保存热点 | 启动消费后删除 |
| `device/device_token` | 设备长期 token | 激活或 token 轮换后更新 |
| `audio/volume` | 音量 | 配置更新 |
| `display/brightness` | 亮度 | 配置更新 |
| `system/tz_offset` | 时区分钟偏移 | hello/config 更新 |
| reminder blob | 最多 10 条提醒 | 创建/取消/触发后更新 |

清除 NVS 会同时清除 Wi-Fi、设备 token、设置和提醒，使设备重新进入配网和激活流程。发布运维时应区分“仅更新固件”和“清除用户数据”。

## 19. 当前完成度

| 子系统 | 状态 | 备注 |
|---|---|---|
| BSP / LCD / I2S / GPIO | 已实现 | 需持续真机回归 |
| Wi-Fi 多热点和无 PIN 配网 | 已实现 | 保存密码复用和失败回退已实现 |
| 首次注册和激活 | 已实现 | 依赖服务端管理页面 |
| HTTP 鉴权 | 已实现 | 当前公网 HTTP 为明文 |
| MQTT 控制 | 已实现 | 依赖 auth 动态配置 |
| UDP Opus 音频 | 已实现 | AES-CTR，无认证标签 |
| AEC + NS | 已实现 | VOIP_LOW_COST，服务端 VAD |
| Daisy 本地唤醒 | 已实现 | 参数需现场数据校准 |
| 连续对话 / 分段上传 | 已实现 | 需服务端状态时序配合 |
| Barge-in | 已实现 | 按键可靠；语音打断依赖服务端 |
| UI / 表情 / 字幕 | 已实现 | LVGL 参数化渲染 |
| 电池 | 软件链路已实现 | 板级 ADC 连线/分压仍需确认 |
| 提醒 | 已实现 | 最多 10 条，本地分钟级扫描 |
| OTA | 已实现 | 完整性和传输安全需增强 |
| QEMU mock | 已实现 | 自动化测试未跟上新协议 |
| DAISY 搜索测试前端 | 已实现辅助界面 | 不属于固件运行时 |

## 20. 已知风险与技术债

### P0：合并前必须确认

1. **协议文档与运行代码不一致**  
   `docs/API_SPECIFICATION.md` 仍以 WebSocket 为主，当前固件是 MQTT + UDP；服务端合并必须先建立唯一权威协议文档。

2. **公网传输仍有明文链路**  
   HTTP base URL 是 `http://`，预期 MQTT 是 `mqtt://`。设备 token、临时 MQTT 凭据和控制消息存在被窃听风险。生产环境应使用域名、HTTPS 和 MQTTS，并进行证书校验。

3. **UDP 只有加密，没有完整性认证**  
   AES-CTR 不检测载荷篡改。建议迁移到 AEAD，如 AES-GCM/ChaCha20-Poly1305，或增加 HMAC，并将 session id、方向、序号纳入认证数据。

4. **OTA 未验证服务端返回的 MD5，也没有签名验证**  
   当前主要依赖 ESP 镜像格式检查和回滚，不足以抵御恶意替换。应启用 HTTPS、固件签名/Secure Boot，并校验 SHA-256 或签名。

### P1：建议在完整项目合并时修复

1. **版本来源不统一**  
   顶层 CMake 项目版本为 `1.0.0`，OTA 管理器报告 `1.2.0`。应生成单一版本头并由构建、OTA、日志和服务端共同使用。

2. **鉴权函数参数与请求体不一致**  
   `firmware_version` 和 `battery_level` 传入 `Authenticate()` 但未发送。应删除无效参数或恢复协议字段。

3. **MQTT 凭据过期处理不完整**  
   `expires_in` 被解析但没有主动刷新调度。服务端若严格过期，长连接重连可能持续使用旧凭据。

4. **消息路由任务存在重复启动风险**  
   MQTT 每次 connected 回调都会调用 `MsgRouter::Start()`，当前没有显式 started guard。重连测试需确认不会重复创建任务。

5. **自动化测试已滞后**  
   `tests/test_firmware.py` 仍断言 WebSocket、`status_update`、旧 RingBuffer 常量，并与当前 `state_change` 和队列实现冲突。测试结果不能代表当前固件质量。

6. **README 音频描述滞后**  
   部分章节仍写 WakeNet9、本地 VAD 和无 AEC；当前代码实际为 microWakeWord + AEC/NS + 服务端 VAD。

7. **服务端地址硬编码为公网 IP**  
   IP 迁移需要重新烧录。建议生产固件使用受控域名，并保留经签名配置或安全 fallback。

### P2：现场验证和体验优化

1. 使用真实外壳、距离、口音和噪声数据测量 Daisy 唤醒 FAR/FRR，补齐模型 manifest。
2. 验证 AEC 参考延迟、RMS 阈值和服务端 Silero VAD 增益。
3. 确认 GPIO1 电池分压和电量曲线，不再依赖通用 3.0 到 4.2 V 线性估算。
4. 评估 4096 项下行队列的最大内存和超长延迟，建立按时长丢弃策略。
5. 对开放配网 AP 增加物理窗口限制、一次性口令或设置超时，同时保持老年用户可用性。

## 21. 终端与服务端合并计划

### 阶段 A：冻结协议

- 以 MQTT + UDP 为唯一当前协议，WebSocket 标记为 legacy。
- 固定 HTTP endpoint、状态码和 JSON schema。
- 固定 MQTT 上下行 `type`、字段必选性和状态时序。
- 固定 UDP 包头、字节序、音频参数、密钥生命周期和重放策略。
- 对每个 schema 增加 `protocol_version`，定义向后兼容策略。

### 阶段 B：配置和安全统一

- 将 HTTP/MQTT 切换到 TLS 域名。
- 明确 Azure endpoint 只在服务端配置，密钥使用 Key Vault 或环境变量。
- 统一 token、MQTT credential、UDP session 和 OTA URL 的过期策略。
- 确定设备撤销、token 轮换、遗失设备和恢复出厂流程。

### 阶段 C：端到端联调

按以下顺序测试，前一层失败时不要跳到后一层：

1. Wi-Fi 获取 IP、DNS/NTP。
2. HTTP health 和 5 秒 TCP 探测。
3. 新设备注册、管理端激活、token 持久化。
4. auth 返回完整 MQTT 字段。
5. MQTT 连接、订阅、hello、heartbeat。
6. hello 返回可公网访问的 UDP 参数。
7. 终端上传连续 Opus 帧，服务端成功解码。
8. 服务端 TTS 下行，终端完整播放多段音频。
9. state、emotion、stt 和 end_dialogue 时序。
10. 按键打断和语音打断。
11. 长录音 segment 合并。
12. 提醒创建、取消、离线恢复和到期播报。
13. OTA 升级、断电、失败回滚。
14. Wi-Fi/MQTT/UDP 断开和恢复。

### 阶段 D：验收和发布

- 更新并执行与当前协议一致的单元测试、QEMU 测试和服务端 contract tests。
- 完成至少 24 小时持续对话/重连稳定性测试；量产前执行 7 x 24 小时 soak test。
- 记录首包延迟、ASR 延迟、TTS 首音延迟、端到端延迟、丢包率和内存最低水位。
- 归档固件二进制、版本、commit、sdkconfig、分区表、模型哈希和服务端版本。
- 制作可恢复的 OTA 发布和回滚操作手册。

## 22. 建议验收标准

| 编号 | 验收标准 |
|---|---|
| AC-01 | 已保存热点可见时，设备无需用户输入，在 30 秒内进入 MQTT ready；全部热点失败后才出现配网 AP。 |
| AC-02 | MQTT 未连接时，按键或唤醒不会进入持续录音半启动状态。 |
| AC-03 | `hello` 后 5 秒内完成 UDP keepalive，服务端记录正确公网源地址。 |
| AC-04 | 16 kHz 测试音播放时长误差不超过 2%，无系统性加速或丢帧。 |
| AC-05 | 10 段连续 TTS 均完整播放，句间不会提前停止。 |
| AC-06 | 播放期间按键打断后 100 ms 目标时间内停止主要音频，且下一轮监听可用。 |
| AC-07 | 30 秒以上录音生成 segment，服务端按序合并且最终文本不重复、不丢段。 |
| AC-08 | MQTT 断开后设备退出对话态；恢复后只启动一组 router/audio writer 任务。 |
| AC-09 | 连续 24 小时运行无任务重启、看门狗、socket/heap 持续泄漏；量产前扩展到 7 天。 |
| AC-10 | OTA 成功、下载中断和无效镜像三类场景均可恢复到可启动固件。 |
| AC-11 | 所有公网身份和控制通道使用 TLS；OTA 镜像通过签名或 SHA-256 信任链校验。 |
| AC-12 | Daisy 唤醒在目标环境达到双方约定的 FRR/FAR，并保留按键兜底。 |

## 23. 服务端总报告合并模板

服务端项目报告合并时，建议完整项目采用以下章节顺序：

1. 产品目标和用户场景。
2. 总体部署架构和数据流。
3. ESP32 终端硬件与固件，本报告第 3 到 18 章。
4. 服务端 API、MQTT gateway、UDP gateway 和会话状态机。
5. ASR、LLM、TTS、翻译和内容服务。
6. DAISY 图书、新闻、播客、天气和提醒业务流程。
7. 身份、隐私、安全、日志和监控。
8. 部署、CI/CD、OTA 和灾难恢复。
9. 端到端测试结果和性能指标。
10. 已知风险、发布标准和后续路线图。

两份报告合并时必须共享以下唯一数据表：

- 环境与 endpoint 表。
- HTTP/MQTT/UDP 协议版本表。
- 设备状态和服务端状态映射表。
- 音频格式与时序表。
- 版本兼容矩阵。
- 端到端测试证据表。
- 风险、责任人和目标日期表。

## 24. 本报告验证说明

本次报告已核对当前源文件、构建清单、分区表、组件依赖、现有构建元数据和历史问题记录。本轮没有重新编译或烧录固件；用户将自行编译。当前 Windows 环境也没有可直接调用的 `python` 或 `py` 命令，因此没有执行 `tests/test_firmware.py`，且该测试文件已确认与现行协议不一致。

建议服务端团队在合并前以本报告第 9 到 12 章为起点生成机器可验证的 OpenAPI、JSON Schema 和 UDP protocol fixture，并用同一批 fixture 同时测试服务端和 QEMU 固件。
