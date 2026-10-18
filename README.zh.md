# HiDaisy ESP32 终端固件

> HiDaisy / HiLily AI 智能陪伴设备的 ESP32-S3 固件。

**[English README](README.md)** · **[终端项目报告](docs/HILILY_TERMINAL_PROJECT_REPORT.md)** · **[英文用户手册](USER_MANUAL_EN.md)**

## 项目概述

本仓库包含 HiDaisy 智能陪伴终端的设备侧固件，产品主要面向老年用户。ESP32-S3 负责实时音频采集与播放、本地唤醒、显示和物理按键、Wi-Fi 配网、设备激活、提醒、电池监控和 OTA。语音识别、对话推理、内容服务和语音合成运行在服务端。

当前主链路使用 HTTP 完成注册和鉴权，使用 MQTT 传输 JSON 控制信令，使用加密 UDP 传输低延迟 Opus 音频。旧 WebSocket 客户端仍保留在源码中供参考，但没有进入当前硬件构建。

```text
INMP441 麦克风
  -> I2S 采集 -> ESP-SR AEC + 降噪
  -> 本地 Daisy microWakeWord / Opus 编码
  -> AES-CTR UDP 音频 -> HiDaisy 服务端
  -> ASR -> 对话 / 工具 -> TTS
  -> UDP Opus -> 抗抖队列 -> I2S -> MAX98357A 扬声器

控制平面：终端 <-> MQTT <-> 服务端
启动入口：终端 -> HTTP 注册/鉴权/OTA
```

## 当前能力

- ESP32-S3 双核 FreeRTOS 应用，音频和 UI 大任务优先使用 PSRAM。
- 16 kHz 常开麦克风管线，支持 ESP-SR AEC 和 NSNet2 降噪。
- 使用量化 TFLite Micro 模型在本地识别 “Hi Daisy”。
- 16 kHz 单声道 Opus 上下行，每帧 60 ms。
- MQTT 控制通道和 AES-CTR 加密 UDP 音频通道。
- 配合服务端 Silero VAD，在 TTS 播放期间支持语音打断。
- 按键打断会先对当前音频帧淡出，再清空待播放队列。
- 连续对话和长录音分段上传。
- ST7789/LVGL 界面，显示状态、字幕、波形、Wi-Fi、时间、电池、Overlay 和动态表情。
- Captive Portal Wi-Fi 配网，最多保存 3 个热点，支持漫游和保存密码复用。
- 设备注册、激活码 onboarding、持久化设备 token 和动态 MQTT 凭据。
- 本地持久化提醒、闹铃、低电上报、远程设置和双分区 OTA。
- 提供 QEMU mock，可在没有真机时进行设备侧开发。
- `frontend/` 下提供可选的 React DAISY 图书搜索测试界面。

## 硬件与软件

| 项目 | 当前配置 |
|---|---|
| 主控 | ESP32-S3-N16R8，双核 240 MHz |
| 存储 | 16 MB Flash，8 MB Octal PSRAM |
| 板型 | `zhengchen-1.54tft-wifi` |
| 显示屏 | ST7789，1.54 英寸，240 x 240，SPI 80 MHz |
| 麦克风 | INMP441，I2S，16 kHz 单声道 |
| 功放 | MAX98357A，I2S |
| USB 转串口 | CH340K |
| 开发框架 | ESP-IDF 5.3 及以上；最近构建元数据使用 5.5.3 |
| UI | LVGL 8.4 |
| 音频编解码 | `espressif/esp_audio_codec ~2.4.1` |
| 音频前端 | `espressif/esp-sr ~2.3.0` |
| 唤醒推理 | TFLite Micro + ESP-NN + micro-speech-features |
| OTA 上报固件版本 | `1.2.0` |

### 引脚映射

| 功能 | GPIO |
|---|---:|
| LCD MOSI / SCLK / CS / DC / RST / 背光 | 41 / 42 / 21 / 40 / 45 / 20 |
| 扬声器 DOUT / BCLK / LRCK | 7 / 15 / 16 |
| 麦克风 SD / WS / SCK | 6 / 4 / 5 |
| BOOT / 唤醒按键 | 0 |
| 音量加 / 音量减及录音测试 | 10 / 39 |
| 电池 ADC | 1 |

## 运行架构

### 启动与联网流程

```text
上电
  -> 初始化 NVS、设置和 OTA 状态
  -> 初始化板级硬件、显示、电池、音频、按键和提醒
  -> 先初始化 Wi-Fi，为 AFE 保留足够内部 RAM
  -> 初始化 AEC/NS 和 Daisy 唤醒模型
  -> 连接已保存 Wi-Fi，或进入配网
  -> 探测 HTTP 服务端
  -> 没有 device_token 时执行注册和激活
  -> 鉴权并取得 MQTT 凭据
  -> 连接并订阅 MQTT
  -> 发送 hello(features.aec=true)
  -> 接收 UDP 地址、AES key/nonce、时区和音频参数
  -> 进入 idle 并启用本地唤醒
```

应用只有在 MQTT 真正连接后才进入 `idle`，避免控制通道未就绪时启动录音并导致 `listen` 消息发送失败。

### 设备状态

状态机包括 `starting`、`wifi_connecting`、`idle`、`listening`、`thinking`、`speaking`、`reminding` 和 `error`。服务端通过 `state_change` 驱动对话状态，终端在每次状态转换时应用对应的音频和 UI 行为。

支持的表情包括 `neutral`、`happy`、`sad`、`comfort`、`surprise`、`thinking`、`listening` 和 `reminding`。服务端可同时下发 0.0 到 1.0 的 `intensity`。

### 主要任务

| 任务 | Core | 优先级 | 职责 |
|---|---:|---:|---|
| `audio_in` | 1 | 10 | 常开麦克风采集 |
| `afe_proc` | 1 | 8 | AEC 和降噪 |
| `audio_out` | 1 | 10 | 音频解码和扬声器输出 |
| `opus_enc` | 0 | 2 | 60 ms Opus 编码 |
| `mww_daisy` | 0 | 2 | 本地唤醒推理 |
| `udp_rx` | 0 | 11 | UDP 接收和解密 |
| `audio_wr` | 0 | 8 | 下行音频队列转发 |
| `msg_router` | 0 | 5 | MQTT JSON 路由 |
| `ui` | 0 | 5 | LVGL 渲染和动画 |

大任务栈和音频队列使用 PSRAM，尽量为 Wi-Fi 和实时驱动保留内部 SRAM。

## 音频管线

### 上行链路

```text
INMP441 32-bit I2S
  -> 右移 12 位并饱和转换为有符号 16-bit PCM
  -> ESP-SR AFE MR 模式：麦克风 + 软件扬声器参考
  -> AEC_MODE_VOIP_LOW_COST
  -> NSNet2，缺失时回退 WebRTC NS
  -> 16 kHz 单声道 Opus，960 samples / 60 ms，VBR，关闭 DTX
  -> UDP 包头 -> AES-CTR 加密载荷 -> 服务端
```

待机时麦克风 I2S 仍保持工作，使唤醒模型可以持续接收音频。当前实现关闭了 ESP-SR VAD 和 WakeNet。本地使用有界 RMS 检测辅助自动停止录音，服务端使用 Silero VAD 检测播放期间的语音打断。

### 唤醒词

固件嵌入的模型为 `main/models/daisy_stream_state_internal_quant.tflite`。模型通过短连续队列接收 AEC/NS 处理后的 16 kHz PCM。当前部署参数为：

- Tensor 输入/输出：`int8[1,3,40]` 到 `uint8[1,1]`。
- 30 ms 特征窗口，10 ms 步长。
- 5 次结果滑动窗口。
- 概率阈值：0.97。
- 只有应用真正 idle 且扬声器排空后才启用推理。

模型没有随附权威 microWakeWord manifest，因此量产前仍需在真实外壳和使用环境中测量 FAR/FRR 并完成参数校准。

### 下行链路

```text
UDP 数据包
  -> 序号检查 -> AES-CTR 解密
  -> PSRAM 消息路由和下行队列
  -> 5 帧启动水位
  -> Opus 解码或 PCM 路径
  -> 二次曲线软件音量
  -> 16-bit 单声道转 32-bit I2S
  -> MAX98357A 扬声器
```

收到 `tts.end` 后，终端会排空队列，而不是立即停止播放。每个新的 `tts.start` 都会重置结束标志，避免多段回复被前一段的 drain 提前截断。播放时，经过音量缩放的 PCM 还会写入软件 AEC 参考通道。

### 对话与打断

- Daisy 唤醒后先播放本地语音确认，发送 `listen.detect`，再进入自动连续对话。
- idle 状态短按 BOOT 会启动自动连续对话。
- 检测到有效语音后，本地 RMS 静音持续 1.5 秒会发送 `listen.stop`。
- TTS 完全排空后等待 500 ms，让扬声器余振衰减，再重新监听。
- 录音超过 30 秒时，可在自然停顿处发送 `listen.segment`。
- 播放期间按 BOOT 会发送 `abort`，淡出当前帧、清空待播放音频并进入 listening。
- speaking 状态仍持续上传 AEC 后音频，由服务端判断语音打断。

## 网络协议

### 当前地址

| 通道 | 当前值 |
|---|---|
| HTTP 启动入口/OTA | `http://4.197.40.220:8000` |
| MQTT | 由 `/api/v1/device/auth` 返回；当前部署预期端口 `1884` |
| UDP 音频 | 由 MQTT `hello` 返回；当前部署预期端口 `8765` |
| 本地配网 | `http://192.168.4.1` |

ESP32 不直接访问 Azure OpenAI、Azure Speech 或 Translator。这些 provider 和密钥只属于服务端部署。

### HTTP 启动流程

1. `POST /api/v1/device/register`，提交小写 `mac_address`。
2. `GET /api/v1/device/register/status?mac=...`，直到激活并取得 `device_token`。
3. `POST /api/v1/device/auth`，提交 `mac_address` 和 `device_token`。
4. 鉴权后以及每小时调用 `GET /api/v1/ota/check?device_id=...&current_version=1.2.0`。

鉴权响应包含 broker URI、client ID、用户名/密码、上下行 topic 和过期信息。MQTT topic 通常为 `device/{mac}/up` 和 `device/{mac}/down`。

### MQTT 消息

终端发往服务端：

| Type | 用途 |
|---|---|
| `hello` | 协商能力和 UDP 通道 |
| `listen` | `start`、`detect`、`segment` 和 `stop` 生命周期 |
| `abort` | 按键打断 |
| `goodbye` | 结束对话 |
| `client_pong` | 回复服务端心跳 |
| `reminder_triggered` | 本地提醒已触发 |
| `low_battery` | 已验证的低电快照 |

服务端发往终端：

| Type | 用途 |
|---|---|
| `hello` | UDP 会话、音频参数和时区 |
| `state_change` | 对话状态；这是当前唯一有效的状态消息名 |
| `emotion_update` | 表情和强度 |
| `tts` | TTS `start` 和 `end` 生命周期 |
| `audio_meta` | PCM/Opus 格式元数据 |
| `stt` | 显示在屏幕上的识别文本 |
| `server_ping` | 心跳请求 |
| `config` | 音量、亮度和时区 |
| `ota` | 远程固件更新请求 |
| `reminder_created`、`reminder_cancelled`、`reminder_notify` | 提醒同步 |
| `end_dialogue` | 关闭连续对话 |
| `error` | 服务端错误信息 |

### UDP 数据包

```text
| type 1 B | flags 1 B | payload_len 2 B | ssrc 4 B |
| timestamp 4 B | sequence 4 B | 加密 Opus 载荷 |
```

16 字节网络字节序包头同时作为每包 AES-CTR counter 输入。sequence 提供基础旧包过滤。AES-CTR 不验证载荷完整性，相关风险见“当前限制”。

## Wi-Fi 配网

终端在 NVS 中最多保存 3 组 SSID 和密码。

- 手动配网前始终先尝试已保存网络。
- 初次启动时每个热点快速重试 2 次，所有已保存网络都失败后才进入配网。
- 运行时断线使用更长的指数退避重连。
- 每 10 秒检查 RSSI；连续 3 次低于 -75 dBm 后扫描，候选热点至少强 10 dB 才漫游。
- 配网会建立开放热点 `HiLily-XXXX`，Captive Portal 地址为 `192.168.4.1`。
- 配网页不要求 PIN。
- 对已有 SSID 提交空密码会复用保存的密码。
- 只有保存密码失效或用户主动修改时才需要输入新密码。
- 已连接时长按 BOOT 只显示设备 IP，不会重新打开配网。
- 未连接时长按 BOOT 会先重试已保存网络，仍失败后才进入设置。

首次配网步骤：

1. 手机连接 `HiLily-XXXX`。
2. 使用自动弹出的配网页，或打开 `http://192.168.4.1`。
3. 扫描新网络或选择已保存项目。
4. 保存凭据，并在页面提示后重启。

## 显示、电池、提醒与 OTA

- LVGL 显示动态表情、监听波形、字幕、时间、Wi-Fi、五格电池以及全屏状态 Overlay。
- 电池读数不在 2500-5000 mV 范围时直接丢弃。监控任务丢弃 5 个预热样本，使用 5 点中值，并要求连续 3 次低电才上报。
- 当前电量曲线将 3000 mV 映射为 0%，4200 mV 映射为 100%；最终分压比例和曲线仍需真机校准。
- 最多 10 条提醒保存到 NVS，每分钟扫描一次。到期后播放本地闹铃并通知服务端。
- OTA 在鉴权后检查一次，之后每小时检查。固件写入非当前的 3 MB 应用分区，并支持回滚。

## 仓库结构

```text
hiDaisy-esp32/
├── main/
│   ├── application.*              应用生命周期和事件循环
│   ├── audio_service.*            I2S、Opus、缓冲、播放、AEC 参考
│   ├── afe_processor.*            ESP-SR AEC 和降噪
│   ├── micro_wake_word.*          Daisy TFLite Micro 推理
│   ├── hilily_mqtt_client.*       HTTP onboarding/鉴权和 MQTT 客户端
│   ├── udp_client.*               加密 UDP 音频通道
│   ├── msg_router.*               MQTT JSON 和 UDP 音频路由
│   ├── wifi_manager.*             保存网络、漫游、配网页和 NTP
│   ├── display_manager.*          LVGL 显示和 Overlay
│   ├── face_renderer.*            参数化动态表情
│   ├── battery.*                  ADC 滤波和低电快照
│   ├── reminder_manager.*         持久化提醒调度
│   ├── ota_manager.*              双分区 OTA
│   ├── qemu_mock/                 硬件和网络 mock
│   ├── models/                    嵌入式 Daisy 唤醒模型
│   └── assets/                    嵌入式本地音频资源
├── docs/                           协议、架构、OTA 和项目报告
├── notes/                          技术排查和已解决问题
├── tests/                          固件源码级检查
├── scripts/qemu_run.sh             独立 QEMU 构建和运行脚本
├── frontend/                       DAISY 图书搜索测试界面
├── partitions.csv                  16 MB Flash 分区表
├── sdkconfig.defaults              真机默认配置
├── sdkconfig.defaults.qemu         QEMU 默认配置
├── flash.sh                        适配手动复位的烧录脚本
└── monitor.sh                      自动重连串口监控和日志脚本
```

## 编译

### 环境要求

- ESP-IDF 5.3 及以上；当前记录的构建环境为 5.5.3。
- ESP-IDF Python 环境，并允许下载 managed components。
- ESP-IDF 安装提供的 Git、CMake 和 Ninja。

### 真机固件

```bash
cd hiDaisy-esp32
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
```

生成的应用固件为 `build/hilily-esp32.bin`。`main/CMakeLists.txt` 会将 Daisy TFLite 模型和本地唤醒确认音频嵌入固件。

### QEMU

```bash
./scripts/qemu_run.sh build
./scripts/qemu_run.sh run
```

需要虚拟 framebuffer 时将 `run` 改为 `graphics`。QEMU 使用 board、Wi-Fi、电池、音频、显示、MQTT 和 UDP mock，不能复现真实 AEC、射频、ADC 和声学效果。

## 烧录与串口监控

本板不能可靠地通过 DTR/RTS 自动进入或退出下载模式，必须手动操作：

1. 按住 BOOT。
2. 按下并松开 RST。
3. 松开 BOOT。
4. 使用 `--before no_reset --after no_reset` 开始烧录。
5. 烧录完成后手动按 RST。

macOS 辅助脚本：

```bash
./flash.sh /dev/cu.wchusbserial1130
./monitor.sh /dev/tty.wchusbserial1130 0
```

`flash.sh` 当前包含开发者本机的 ESP-IDF 路径，在其他机器使用前需要修改其中的 `IDF_PATH`。

Windows ESP-IDF 终端示例：

```powershell
Set-Location build
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before no_reset --after no_reset write_flash '@flash_args'
Set-Location ..
```

打开 COM3 前先关闭其他串口监视器。如果 ROM 显示 `DOWNLOAD(USB/UART0) waiting for download`，说明设备仍处于下载模式；成功烧录后松开 BOOT 并按 RST。

## DAISY 图书搜索测试前端

可选 React/Vite 界面调用 `GET /api/books/search`，显示 DAISY 图书卡片，并测试浏览器语音命令和服务端音频响应。

```bash
cd frontend
npm install
npm run dev
```

代理配置和响应 schema 见 [frontend/README.md](frontend/README.md)。

## 文档

- [终端项目报告](docs/HILILY_TERMINAL_PROJECT_REPORT.md)：当前架构、联调契约、风险和验收清单。
- [设备/服务端 API 规范](docs/API_SPECIFICATION.md)：历史共享协议文档，其中部分内容仍描述旧 WebSocket 链路。
- [ESP32 架构 PRD](docs/PRD_ESP32_Architecture.md)：产品和目标架构。
- [OTA 管理员指南](docs/OTA_ADMIN_GUIDE.md)：服务端固件发布流程。
- [英文用户手册](USER_MANUAL_EN.md)：用户操作和安全说明。
- [技术笔记](notes/)：问题排查和实现决策。

## 当前限制

- 当前公网 HTTP 地址和预期 MQTT 部署仍使用明文 `http://` 和 `mqtt://`。生产部署应使用稳定域名、HTTPS、MQTTS 和证书校验。
- UDP 音频使用 AES-CTR，但没有认证 tag。生产加固应使用 AEAD，或增加包含防重放关联数据的 MAC。
- OTA 当前没有校验响应中的 MD5，也没有签名固件 manifest。正式发布前应评估 TLS、SHA-256/签名验证和 Secure Boot。
- 顶层 `CMakeLists.txt` 的项目版本为 `1.0.0`，`OtaManager` 上报 `1.2.0`，应改为同一个版本来源。
- MQTT 凭据的 `expires_in` 已解析，但没有主动刷新调度。
- 唤醒模型缺少权威特征 manifest，仍需进行外壳相关的 FAR/FRR 测试。
- 电池 ADC 连线、分压比例和百分比曲线仍需最终硬件校准。
- `tests/test_firmware.py` 仍包含部分旧 WebSocket 实现的断言，不能完整代表当前 MQTT/UDP 固件质量。

## 服务端联调清单

端到端测试应按以下顺序进行：

1. Wi-Fi、DHCP、NTP 和 HTTP 可达性。
2. 注册、激活、token 持久化和鉴权。
3. MQTT 连接、topic、`hello`、心跳和状态消息。
4. 可公网访问的 UDP 地址、keepalive、AES 参数和 Opus 解码。
5. 多段 TTS 播放和 `tts.start`/`tts.end` 时序。
6. 按键打断和语音打断。
7. 长录音分段合并。
8. 提醒同步和本地触发。
9. OTA 成功、下载中断、无效镜像和回滚。
10. Wi-Fi、MQTT 和 UDP 断开恢复，且不产生重复任务或资源泄漏。

服务端和固件必须严格对齐 JSON schema、16 字节 UDP 包头、网络字节序、16 kHz 单声道音频和 60 ms Opus 帧。
