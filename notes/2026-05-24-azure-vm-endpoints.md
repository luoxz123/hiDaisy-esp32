# 2026-05-24 Azure VM endpoints migration

## 现象

服务端迁移到 Azure VM 后，ESP32 固件仍指向旧域名 `https://api.hilily.sarah.xin`，启动时 TCP 探测也仍连旧 HTTPS 地址。

## 排查过程

- 搜索固件和文档中的旧服务端地址、端口和 WebSocket 示例。
- 确认当前主链路是 MQTT + UDP：HTTP `/api/v1/device/auth` 返回 `mqtt_broker`，hello 消息返回 UDP server/port/key/nonce。
- 验证新健康检查地址 `http://4.197.40.220:8000/health` 返回 `{"status":"ok","version":"1.0.0"}`。

## 根因

服务器公网入口从旧域名/旧局域网转发迁移为 Azure VM 公网 IP `4.197.40.220`，固件中的 HTTP base URL 和启动连通性探测没有同步更新。

## 解决方案

- 将固件 `SERVER_URL` 更新为 `http://4.197.40.220:8000`，供注册、鉴权和 OTA 使用。
- 将启动 TCP 探测目标改为 `4.197.40.220:8000`。
- 将本地 API 文档中的 WebSocket 示例更新为 `ws://4.197.40.220:8000/ws/v1/device/...`。
- MQTT broker 和 UDP 地址继续使用服务端返回值，不在固件中硬编码；当前预期分别为 `mqtt://4.197.40.220:1884` 和 `4.197.40.220:8765`。

## 教训

客户端只应硬编码最小入口地址。MQTT、UDP、AI service、STT/TTS/翻译等服务端内部 endpoint 应由服务端配置和鉴权/hello 响应下发，避免每次服务端迁移都需要改固件。
