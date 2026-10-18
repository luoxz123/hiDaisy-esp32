# HiLily 设备通信协议规范 (API & WebSocket)

> 版本: v0.1.0-alpha
> 最后更新: 2026-03-01

本文档规范了 ESP32 硬件端与 Server 服务端之间的双轨通信协议，包含 HTTP 鉴权层、WebSocket 实时流媒体控制层、以及 Admin 管理接口。

---

## 1. 鉴权与接入 (HTTP API)

硬件设备在开机或重新配网后，必须通过此接口换取长连接凭证。

### 1.1 设备认证 — 获取 WebSocket Ticket

- **URL**: `POST /api/v1/device/auth`
- **Content-Type**: `application/json`
- **速率限制**: 5 次/分钟 (per MAC)

**请求载荷:**
```json
{
  "mac_address": "84:CC:A8:XX:XX:XX",
  "device_token": "your-64-char-device-token"
}
```

**响应 200 OK:**
```json
{
  "ticket": "eyJhbGciOiJIUzI1NiJ9...",
  "ws_url": "ws://4.197.40.220:8000/ws/v1/device/84:CC:A8:XX:XX:XX",
  "expires_in": 30,
  "device_name": "客厅设备"
}
```

> 注: `ticket` 为 30 秒一次性 JWT Ticket，用于 WebSocket 连接。鉴权成功后自动初始化 DeviceWorkspace。

**错误响应:**
| 状态码 | 场景 |
|--------|------|
| 401 | Token 不匹配 |
| 403 | 设备已禁用 |
| 404 | 设备未注册 |
| 429 | 速率限制 (5/min 超限) |

### 1.2 Token 刷新

- **URL**: `POST /api/v1/device/token/refresh`
- **Content-Type**: `application/json`

**请求载荷:**
```json
{
  "mac_address": "84:CC:A8:XX:XX:XX",
  "pin_code": "000000"
}
```

**响应:** 新 64 字符 Token（仅显示一次）。

---

## 2. WebSocket 双轨长连接 (WSS)

### 2.0 连接端点

```
ws://4.197.40.220:8000/ws/v1/device/{device_id}
```

**连接限制:**
- 每台设备最多 1 个并发连接 (ConnectionLimiter)
- 超限连接将被拒绝 (code=4003)

### 2.1 连接生命周期

```
Client                                      Server
  |                                           |
  |──── WebSocket Connect ──────────────────▶ |  ConnectionLimiter 检查
  |                                           |
  |──── hello {device_id} ─────────────────▶ |  注册设备 + 恢复/创建 Pipeline
  |◀─── hello_ack ─────────────────────────── |
  |                                           |
  |        ┌─── 心跳循环 (每 30s) ───┐        |
  |◀───── server_ping ───────────────────── |
  |──── client_pong ───────────────────────▶ |
  |        └────────────────────────────┘      |
  |                                           |
  |──── audio_start ───────────────────────▶ |  → LISTENING
  |──── [Binary] PCM chunks ───────────────▶ |  → ASR
  |──── audio_end ─────────────────────────▶ |  → THINKING (垫话播放)
  |                                           |
  |◀─── state_change (speaking) ──────────── |  → SPEAKING
  |◀─── [Binary] TTS chunks ─────────────── |
  |◀─── tts_end ─────────────────────────── |  → IDLE
  |                                           |
  |──── disconnect ────────────────────────▶ |  Pipeline 缓存 5 分钟
```

### 2.2 会话恢复 (v0.1.0-alpha 新增)

设备断线后，Pipeline 被缓存到 `_stale_pipelines`（TTL = 5 分钟）：
- **5 分钟内重连**: 自动恢复对话上下文，对话历史保留
- **超过 5 分钟**: 创建全新 Pipeline

### 2.3 服务端心跳 (v0.1.0-alpha 新增)

```
配置参数:
  HEARTBEAT_INTERVAL = 30s   # 发送间隔
  HEARTBEAT_TIMEOUT  = 10s   # 等待 Pong 超时
  MAX_MISSED_PONGS   = 3     # 最大容忍次数

行为:
  Server 每 30s 发送 server_ping
  Client 收到后必须在 10s 内回复 client_pong
  连续 3 次未回复 → Server 关闭连接 (code=4002, reason="Heartbeat timeout")
```

---

## 3. 消息类型定义

### 3.1 上行 (Client → Server)

| type | 说明 | 必含字段 |
|------|------|---------|
| `hello` | 连接握手 | `device_id` |
| `audio_start` | 开始录音 | — |
| `audio_data` | 音频数据帧 | (Binary Frame) |
| `audio_end` | 结束录音 | — |
| `dialogue_stop` | 用户主动停止 | — |
| `client_pong` | 心跳响应 | — |
| `ping` | 客户端心跳 | `battery`, `wifi_rssi` (可选) |
| `reminder_triggered` | 客户端定时到期触发 | `reminder_id` |
| `low_battery` | 电量不高于 20% 时请求低电量语音提醒（最多每 5 分钟一次） | `battery`, `voltage_mv`, `text` |

### 3.2 下行 (Server → Client)

| type | 说明 | 关键字段 |
|------|------|---------|
| `hello_ack` | 握手确认 | `session_id` |
| `state_change` | 状态流转 | `state` (idle/listening/thinking/speaking) |
| `asr_result` | ASR 结果 | `text`, `is_final` |
| `tts_data` | TTS 音频帧 | (Binary Frame) |
| `tts_end` | TTS 结束 | — |
| `dialogue_finish` | 对话轮结束 | `text` (完整回复) |
| `emotion_update` | 情绪更新 | `emotion`, `intensity` |
| `reminder_created` | 提醒创建确认 | `reminder_id`, `time`, `content` |
| `reminder_cancelled` | 提醒取消确认 | `reminder_id` |
| `reminder_notify` | 提醒触发通知（服务端备用） | `reminder_id`, `content` |
| `reminder_sync` | 重连时同步活跃提醒 | `reminders` |
| `end_dialogue` | 对话结束指令 | `reason` |
| `server_ping` | 服务端心跳 | `timestamp` |
| `error` | 错误通知 | `code`, `message` |
| `pong` | 心跳响应 | — |

### 3.3 emotion_update 详细说明

```json
{
  "type": "emotion_update",
  "emotion": "happy",
  "intensity": 0.8
}
```

| emotion | 含义 | 典型场景 |
|---------|------|---------|
| `neutral` | 默认/空闲 | 无明显情绪 |
| `happy` | 高兴 | 用户分享好消息、问候 |
| `sad` | 伤心 | 用户表达悲伤、失落 |
| `comfort` | 安慰 | 安慰用户、温暖鼓励 |
| `surprise` | 惊奇 | 用户说了意外的事 |
| `thinking` | 思考 | LLM 推理中 |

`intensity`: 0.0 ~ 1.0，表示情绪强度。

### 3.4 提醒消息详细说明 (Issue #289)

#### 上行: `reminder_triggered`

客户端本地定时器到期后发送，请求服务端合成 TTS 语音并推送：

```json
{
  "type": "reminder_triggered",
  "reminder_id": "rem_abc123"
}
```

服务端收到后：
1. 根据 `reminder_id` 查询提醒内容
2. 立即标记为 `triggered`（防止 ReminderScheduler 重复推送）
3. 合成 TTS 语音并流式推送 PCM 音频帧
4. 完成后标记为 `completed`

若 `reminder_id` 不存在，返回 `error` 消息 (code: 404)。

#### 下行: `reminder_created`

LLM 创建提醒后推送，客户端据此启动本地定时器：

```json
{
  "type": "reminder_created",
  "reminder_id": "rem_abc123",
  "time": 1741660800,
  "content": "吃药"
}
```

- `time`: Unix 时间戳（秒）

#### 下行: `reminder_cancelled`

提醒被取消后推送，客户端据此取消本地定时器：

```json
{
  "type": "reminder_cancelled",
  "reminder_id": "rem_abc123"
}
```

#### 下行: `reminder_sync`

WebSocket 重连后推送所有活跃提醒，客户端据此恢复本地定时器：

```json
{
  "type": "reminder_sync",
  "reminders": [
    {"reminder_id": "rem_abc123", "time": 1741660800, "content": "吃药"},
    {"reminder_id": "rem_def456", "time": 1741664400, "content": "散步"}
  ]
}
```

空列表也会推送（表示无活跃提醒）。

#### 提醒触发优先级

1. **客户端触发为主**: 客户端本地定时器到期后发送 `reminder_triggered`
2. **服务端备用**: 提醒到期超过 2 分钟且客户端未触发时，ReminderScheduler 自动推送 TTS

#### 下行: `end_dialogue`

用户表达告别意图（如"再见"、"拜拜"、"就这样吧"）后，服务端在发送告别 TTS 后下发此命令。客户端收到后停止 continuous listening 模式，在 TTS 播放完毕后回到 Idle 状态：

```json
{
  "type": "end_dialogue",
  "reason": "user_farewell"
}
```

**reason 取值:**
| reason | 说明 |
|--------|------|
| `user_farewell` | 用户主动告别 |
| `timeout` | 对话超时 |
| `server_close` | 服务端主动关闭 |

---

### 3.5 低电量提醒

电池电量不高于 20% 时，客户端在设备空闲且扬声器未播放时发送；成功发送后至少等待 5 分钟才再次发送：

```json
{
  "type": "low_battery",
  "battery": 20,
  "voltage_mv": 3400,
  "snapshot_sequence": 17,
  "text": "Low battery, please charge as soon as possible."
}
```

服务端收到后应跳过 ASR/LLM，直接使用 `text` 合成 TTS，并复用常规
`state_change(speaking)` → 音频帧 → `tts(end)` → `state_change(idle)` 下行链路。
服务端应校验 `battery <= 20`，并按设备 ID 做至少 5 分钟幂等限流，防止设备重连或重复上报造成连续播报。
`battery`、`voltage_mv` 和 `snapshot_sequence` 来自终端同一次经过验证的 ADC 快照；服务端日志应保留该序号，以便核对 UI 与提醒是否使用了同一数据。

## 4. 二进制音频流 (Binary Frame)

**音频数据无需 JSON 包装，直接作为 Binary 发送，以达到极致性能。**

**上行 (ESP32 → Server):**
- 格式: 16kHz, 16-bit, Mono PCM, Little-Endian
- 建议每帧: 100ms - 200ms

**下行 (Server → ESP32):**
- 格式: TTS 引擎切句合成后的 PCM 流
- 硬件端收到后直接推入 RingBuffer 消费

---

## 5. Admin API

### 5.1 设备管理

| 方法 | 端点 | 说明 | 审计 |
|------|------|------|------|
| POST | `/admin/devices/create` | 注册新设备 | ✅ |
| GET | `/admin/devices/{mac}` | 查看设备详情 | ✅ |
| POST | `/admin/devices/{mac}/update` | 更新设备信息 | ✅ |
| POST | `/admin/devices/{mac}/delete` | 删除设备 | ✅ |
| POST | `/admin/devices/{mac}/regenerate-token` | 重新生成 Token | ✅ |

### 5.2 对话管理 (v0.1.0-alpha 新增)

| 方法 | 端点 | 说明 |
|------|------|------|
| GET | `/admin/conversations` | 对话记录列表 (支持设备/日期筛选) |
| GET | `/admin/conversations/api/stats` | 对话统计 API |

### 5.3 其他管理接口

| 方法 | 端点 | 说明 |
|------|------|------|
| GET | `/admin/dashboard` | 管理面板首页 |
| GET | `/admin/users` | 用户管理 |
| GET | `/admin/prompts` | 系统提示词管理 |
| GET | `/admin/memory` | 记忆配置管理 |
| GET | `/admin/workspace/{mac}` | 设备工作区查看 |
| GET | `/admin/metrics` | 延迟指标 (P50/P95) |

### 5.4 审计日志

所有 Admin 设备操作自动记录到 `storage/audit/admin_audit.jsonl`：

```json
{
  "timestamp": "2026-03-01T08:00:00+00:00",
  "action": "create_device",
  "actor": "admin@hilily.com",
  "target": "AA:BB:CC:DD:EE:FF",
  "details": {"device_name": "客厅设备"},
  "ip": "192.168.1.100"
}
```

---

## 6. 安全机制

### 6.1 速率限制

| 端点类别 | 限制 | 窗口 |
|---------|------|------|
| 设备鉴权 (`/device/auth`) | 5 次 | 60 秒 |
| Admin API | 60 次 | 60 秒 |
| WebSocket 连接 | 10 次 | 60 秒 |
| 默认 | 100 次 | 60 秒 |

### 6.2 连接限制

每台设备最多 1 个并发 WebSocket 连接，防止重复连接导致状态冲突。

### 6.3 紧急呼救检测

ASR 结果经过 `EmergencyDetector` 检测紧急关键词：
- 精确匹配: "救命"、"摔倒了"、"help me" 等
- 模糊匹配: 正则支持间断说法
- 命中后即时 TTS 播放安抚语音

---

## 7. 核心交互时序图

### 7.1 正常对话 (含垫话)

```text
ESP32 (Client)                          Server (Gateway & AI Pipeline)
  |                                                  |
  |── [JSON] hello {device_id} ─────────────────────▶|  注册 + Pipeline 创建/恢复
  |◀─ [JSON] hello_ack ─────────────────────────────|
  |                                                  |
  |── [JSON] audio_start ──────────────────────────▶|  → LISTENING
  |── [Binary] PCM Chunks ─────────────────────────▶|  → ASR
  |── [JSON] audio_end ────────────────────────────▶|
  |                                                  |
  |◀─ [JSON] state_change (thinking) ──────────────|  → THINKING
  |◀─ [Binary] 垫话音频 "让我想想..." ────────────────|  ← FillerAudioManager
  |                                                  |
  |◀─ [JSON] state_change (speaking) ──────────────|  → SPEAKING (LLM 首句到达)
  |◀─ [JSON] emotion_update {happy, 0.8} ──────────|
  |◀─ [Binary] TTS Chunk 1 ────────────────────────|  ← SentenceBuffer eager_mode
  |◀─ [Binary] TTS Chunk 2 ────────────────────────|
  |◀─ [JSON] tts_end ──────────────────────────────|
  |◀─ [JSON] dialogue_finish ──────────────────────|  → IDLE
```

### 7.2 Barge-in 打断

```text
ESP32                                    Server
  |◀─ [Binary] TTS playing... ──────────|
  |── [JSON] audio_start (打断!) ──────▶|  强制中止 TTS + LLM
  |◀─ [JSON] state_change (listening) ──|  立刻回退状态机
```

### 7.3 紧急呼救

```text
ESP32                                    Server
  |── [Binary] "救命啊！" ──────────────▶|  ASR → EmergencyDetector 命中
  |◀─ [Binary] 安抚语音 ────────────────|  ← 即时 TTS (不等 LLM)
  |                                      |  + 并行 LLM 正常回复
  |◀─ [Binary] LLM 回复 ───────────────|
```
