# HiLily OTA 固件管理指南

> **受众**：系统管理员 / 运维人员  
> **版本**：v1.0 · 2026-03-04

---

## 目录

1. [系统概述](#1-系统概述)
2. [固件文件放置位置](#2-固件文件放置位置)
3. [服务器端配置与部署](#3-服务器端配置与部署)
4. [上传新固件](#4-上传新固件)
5. [版本管理](#5-版本管理)
6. [客户端行为说明](#6-客户端行为说明)
7. [故障排查](#7-故障排查)
8. [API 参考](#8-api-参考)

---

## 1. 系统概述

HiLily OTA 系统采用"服务端推送 + 客户端主动拉取"的双轨模式：

```
设备启动
  │
  ├─ 连接 WiFi & 完成 JWT 鉴权
  │
  ├─ GET /api/v1/ota/check?device_id=<id>&current_version=<ver>
  │       │
  │       ├─ has_update: false → 继续正常运行
  │       │
  │       └─ has_update: true  → 下载新固件 → flash → 重启
  │
  └─ 每小时自动重检（esp_timer）
```

### 关键特性

| 特性 | 说明 |
|------|------|
| 双分区滚动升级 | `ota_0` / `ota_1` 交替使用，升级失败自动回滚 |
| 语义版本比较 | `MAJOR.MINOR.PATCH`，仅在服务端版本 **严格大于** 设备版本时触发升级 |
| 断点重试 | 最多重试 3 次，每次失败打印日志 |
| 进度显示 | 屏幕每 5% 刷新一次进度条 |
| 周期检查 | 每隔 1 小时自动检查，无需重启 |

---

## 2. 固件文件放置位置

### 2.1 目录结构

服务器上的固件文件通过 **Docker Volume** 挂载到宿主机目录：

```
/home/maxazure/projects/hilily-server/
└── storage/                    ← Docker volume: ./storage:/app/storage
    └── firmware/               ← 固件存放目录（自动创建）
        ├── 1.2.0.bin
        ├── 1.3.0.bin
        └── ...
```

### 2.2 文件命名规范

固件文件名格式：`{版本号}.bin`

```
✅ 正确：1.2.0.bin, 1.3.0.bin, 2.0.0.bin
❌ 错误：firmware.bin, hilily-latest.bin, v1.2.0.bin
```

> **注意**：文件名必须与上传时指定的 `version` 字段完全匹配，数据库记录中 `filename` 列存储的就是 `{version}.bin`。

### 2.3 手动放置固件（无需 API）

如需直接在服务器上放置文件（绕过 API 上传），步骤如下：

```bash
# 1. 将固件复制到服务器
scp build/hilily.bin maxazure@192.168.31.205:/home/maxazure/projects/hilily-server/storage/firmware/1.3.0.bin

# 2. 确认文件权限（容器内以 appuser 运行，需可读）
ssh maxazure@192.168.31.205 "chmod 644 /home/maxazure/projects/hilily-server/storage/firmware/1.3.0.bin"

# 3. 计算 MD5（稍后需填入数据库）
ssh maxazure@192.168.31.205 "md5sum /home/maxazure/projects/hilily-server/storage/firmware/1.3.0.bin"
```

> ⚠️ **直接放文件不会自动更新数据库**。必须同时通过 API 注册该版本，否则 `/check` 接口不会返回该版本。推荐使用 [第 4 章](#4-上传新固件) 的 API 上传方式。

---

## 3. 服务器端配置与部署

### 3.1 环境要求

| 组件 | 版本 | 说明 |
|------|------|------|
| Docker | ≥ 20.10 | 容器运行时 |
| Docker Compose | ≥ 2.0 | 服务编排 |
| PostgreSQL | 15 | 版本元数据存储 |
| FastAPI 应用 | hilily-server | OTA API 服务 |

### 3.2 服务器目录结构

```
/home/maxazure/projects/hilily-server/
├── docker-compose.yml          ← 服务编排
├── src/
│   └── api/v1/endpoints/
│       └── ota.py              ← OTA API 实现
├── src/db/
│   └── models.py               ← FirmwareVersion 数据模型
├── storage/
│   └── firmware/               ← 固件存储目录（宿主机路径）
└── docs/
    └── OTA_ADMIN_GUIDE.md      ← 本文档（服务端副本）
```

### 3.3 数据库模型

```python
class FirmwareVersion(Base):
    __tablename__ = "firmware_versions"

    id           = Column(String, primary_key=True)   # UUID
    version      = Column(String(20), unique=True)    # 如 "1.3.0"
    filename     = Column(String(255))                 # 如 "1.3.0.bin"
    file_size    = Column(Integer)                     # 字节数
    md5          = Column(String(32))                  # MD5 哈希（小写十六进制）
    release_notes= Column(Text, nullable=True)        # 更新说明
    is_active    = Column(Boolean, default=True)      # false = 禁用该版本
    created_at   = Column(DateTime(timezone=True))   # 上传时间
```

### 3.4 关键配置项

编辑服务器上的 `.env` 或 `src/core/config.py`：

```python
BASE_STORAGE_PATH = "./storage"   # 固件相对路径基准
```

> `FIRMWARE_DIR` = `BASE_STORAGE_PATH/firmware`，容器内路径为 `/app/storage/firmware`，宿主机路径为 `./storage/firmware`（即 `/home/maxazure/projects/hilily-server/storage/firmware`）。

### 3.5 首次部署初始化

```bash
# 登录服务器
ssh maxazure@192.168.31.205

cd ~/projects/hilily-server

# 确保固件目录存在并可写
mkdir -p storage/firmware
chmod 755 storage/firmware

# 重启服务（如代码有更新）
docker compose restart hilily

# 验证 OTA 接口可用
curl -s "http://localhost:8000/api/v1/ota/check?device_id=test&current_version=0.0.0" | python3 -m json.tool
```

### 3.6 代理端口说明

服务使用 `socat` 做端口转发：

```
外部访问 :8801  →  socat  →  Docker 容器内 :8000
```

ESP32 设备配置的服务器地址为 `http://192.168.31.205:8801`。

---

## 4. 上传新固件

### 4.1 方式一：API 上传（推荐）

**构建固件**

```bash
# 在开发机上
cd /Users/maxazure/projects/hilily-esp32
export IDF_PATH=/Users/maxazure/.espressif/v5.5.3/esp-idf
source $IDF_PATH/export.sh
idf.py build
```

**上传到服务器**

```bash
curl -X POST http://192.168.31.205:8801/api/v1/ota/upload \
  -F "version=1.3.0" \
  -F "release_notes=修复音频播放速度问题，优化唤醒词灵敏度" \
  -F "file=@build/hilily.bin"
```

成功响应示例：

```json
{
  "version": "1.3.0",
  "filename": "1.3.0.bin",
  "file_size": 2048576,
  "md5": "a1b2c3d4e5f6...",
  "release_notes": "修复音频播放速度问题，优化唤醒词灵敏度",
  "is_active": true,
  "created_at": "2026-03-04T20:55:00+00:00"
}
```

### 4.2 方式二：SCP + API 注册

适合大文件或网络较慢的情况：

```bash
# 步骤 1：SCP 上传到服务器
scp build/hilily.bin maxazure@192.168.31.205:~/projects/hilily-server/storage/firmware/1.3.0.bin

# 步骤 2：计算 MD5 和文件大小
ssh maxazure@192.168.31.205 "md5sum ~/projects/hilily-server/storage/firmware/1.3.0.bin && ls -l ~/projects/hilily-server/storage/firmware/1.3.0.bin"

# 步骤 3：通过 API 注册（需提供 MD5 和 size）
# 直接用 POST /upload 也会读取已有文件，推荐仍用 /upload
```

### 4.3 发布流程检查清单

```
□ 在 main/ota_manager.cc 中更新 FIRMWARE_VERSION 字符串（如 "1.3.0"）
□ 构建固件并验证 0 errors / 0 warnings
□ 通过 API 上传固件
□ 验证 /check 接口返回新版本
□ 在测试设备上触发 OTA 升级，确认升级成功
□ 更新 README 中的版本号
□ 创建 Git tag（如 v1.3.0）
```

---

## 5. 版本管理

### 5.1 查看所有版本

```bash
curl -s http://192.168.31.205:8801/api/v1/ota/versions | python3 -m json.tool
```

### 5.2 禁用某个版本

直接在 PostgreSQL 中将 `is_active` 设为 `false`：

```bash
ssh maxazure@192.168.31.205

# 进入数据库
docker exec -it $(docker ps -qf name=postgres) psql -U postgres -d postgres

# 禁用版本
UPDATE firmware_versions SET is_active = false WHERE version = '1.2.0';

# 确认
SELECT version, is_active, created_at FROM firmware_versions ORDER BY created_at DESC;
\q
```

> OTA 检查接口只查询 `is_active = true` 的最新版本。禁用后设备不会被推送该版本。

### 5.3 版本回滚

如需强制设备降级（不建议，ESP32 不支持降级），可临时将旧版本设为活跃：

```sql
UPDATE firmware_versions SET is_active = false;  -- 禁用所有
UPDATE firmware_versions SET is_active = true WHERE version = '1.1.0';  -- 启用旧版
```

> ⚠️ ESP32 的语义版本比较逻辑中，**旧版本号不会触发升级**。回滚需要额外修改客户端逻辑，**通常不推荐**。

---

## 6. 客户端行为说明

### 6.1 版本检查触发条件

| 场景 | 触发 |
|------|------|
| 设备启动且 WiFi 已连接 | 自动，约启动后 2 秒 |
| 服务器通过 WebSocket 推送 `{"type":"ota","url":"...","version":"..."}` | 立即触发 |
| 每小时周期性检查 | 自动（esp_timer，`CHECK_INTERVAL_MS = 3600000`） |

### 6.2 升级流程中的屏幕显示

```
OTA: 检查更新...
OTA: 下载 v1.3.0
OTA: 进度 20%
OTA: 进度 40%
...
OTA: 进度 100%
OTA: 重启中...
```

### 6.3 客户端固件版本号位置

```cpp
// main/ota_manager.cc  第 17 行
static const char *FIRMWARE_VERSION = "1.2.0";
```

> 每次发布新版本时，**必须**先更新此字段，再构建固件。

---

## 7. 故障排查

### 7.1 OTA 检查失败（HTTP 超时）

```
E ota: OTA check failed: ESP_ERR_HTTP_CONNECT
```

**原因**：服务器不可达或端口未开放。

```bash
# 从 ESP32 所在网络测试连通性
curl -v http://192.168.31.205:8801/api/v1/ota/check?device_id=test&current_version=1.2.0

# 检查 Docker 状态
ssh maxazure@192.168.31.205 "docker ps | grep hilily"

# 检查 socat 代理
ssh maxazure@192.168.31.205 "ps aux | grep socat"
```

### 7.2 下载失败后自动回滚

```
E ota: Download attempt 3/3 failed
W ota: OTA update failed after 3 attempts
```

设备将继续运行当前固件，不会重启。下次检查（1 小时后）会重试。

若需手动重试：重启设备即可。

### 7.3 固件文件丢失（404）

```json
{"detail": "Firmware file not found on disk"}
```

检查宿主机文件是否存在：

```bash
ssh maxazure@192.168.31.205 "ls -lh ~/projects/hilily-server/storage/firmware/"
```

若文件缺失，需重新上传并确认容器可读（`chmod 644`）。

### 7.4 版本重复冲突（409）

```json
{"detail": "Version 1.3.0 already exists"}
```

删除旧记录后重传，或使用新版本号：

```sql
DELETE FROM firmware_versions WHERE version = '1.3.0';
```

### 7.5 升级后设备频繁重启（回滚触发）

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` 启用时，若新固件在启动后未调用 `MarkValid()`，引导加载程序将自动回滚到上一版本。

确认 `main/application.cc` 中 `Application::Start()` 最开始调用了：

```cpp
OtaManager::GetInstance().MarkValid();
```

---

## 8. API 参考

**Base URL**：`http://192.168.31.205:8801`

### GET /api/v1/ota/check

检查是否有可用更新。

| 参数 | 类型 | 说明 |
|------|------|------|
| `device_id` | string | 设备唯一标识（MAC 地址） |
| `current_version` | string | 设备当前固件版本（如 `1.2.0`） |

**响应**

```json
// 有更新
{
  "has_update": true,
  "version": "1.3.0",
  "url": "/api/v1/ota/firmware/1.3.0",
  "size": 2048576,
  "md5": "a1b2c3d4e5f67890abcdef1234567890"
}

// 无更新
{
  "has_update": false
}
```

---

### GET /api/v1/ota/firmware/{version}

下载指定版本的固件二进制文件。

| 路径参数 | 说明 |
|----------|------|
| `version` | 版本号，如 `1.3.0` |

**响应**：`application/octet-stream` 二进制流

---

### POST /api/v1/ota/upload

上传新固件版本（`multipart/form-data`）。

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `version` | string | ✅ | 版本号，如 `1.3.0` |
| `release_notes` | string | ❌ | 更新说明 |
| `file` | binary | ✅ | `.bin` 固件文件 |

**响应**：`201 Created` + `FirmwareInfo` JSON

---

### GET /api/v1/ota/versions

列出所有已注册的固件版本（按上传时间倒序）。

**响应**：`FirmwareInfo[]` JSON 数组

---

*最后更新：2026-03-04 · 维护人：sarahliuakl*
