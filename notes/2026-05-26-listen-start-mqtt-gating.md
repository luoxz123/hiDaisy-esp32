# 2026-05-26 Gate listen start on MQTT readiness

## 现象

设备进入 `listening` 后立即出现 `listen(start) send failed (ESP_FAIL)`，随后回滚到 `idle`。日志中 `Idle (from=listening, continuous=1)` 表明连续对话状态没有被清理。

## 排查过程

- `listen(start)` 通过 `HililyMqttClient::PublishJson()` 发送。
- `PublishJson()` 在 MQTT 未连接或 client 为空时直接返回 `ESP_FAIL`。
- `HandleWifiConnected()` 在认证后调用 `HililyMqttClient::Connect()`，但没有等待 MQTT connected 回调，就把设备从 `wifi_connecting` 切到 `idle`。
- 设备处于 `idle` 后，按钮或唤醒词可以进入 `listening`，此时 MQTT 可能还没连接完成。

## 根因

应用层把“Wi-Fi 已连接”误当成“对话传输已就绪”。MQTT 连接完成前就允许进入 `idle/listening`，导致 `listen(start)` 发送失败。

## 解决方案

- `HandleWifiConnected()` 不再立即切 `idle`，保持 `wifi_connecting`。
- 新增 `EVT_WS_CONNECTED` 处理：MQTT connected 后才切到 `idle`。
- `EVT_WS_DISCONNECTED` 会清理连续对话、listen flags 和 UDP，并回到 `wifi_connecting`。
- 按钮、唤醒词和连续重听在 MQTT 未连接时不再进入 `listening`。
- `listen(start)` / `listen(stop)` 发送失败时清理 `continuous_mode_`，并根据 MQTT 状态回滚到 `idle` 或 `wifi_connecting`。

## 教训

设备状态机应区分 Wi-Fi 层连接与对话传输层可用。只有 MQTT 控制通道 ready 后，才应允许录音/对话状态启动，否则会出现音频采集已启动但服务端没有收到 listen 控制消息的半启动状态。
