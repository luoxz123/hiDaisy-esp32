# 2026-05-26 Server entrypoint reset retry

## 现象

设备 Wi-Fi 已连接并拿到 IP，但访问 `4.197.40.220:8000` 时 TCP test 失败，`esp_http_client` 报 `ESP_ERR_HTTP_CONNECT`，auth 和 OTA 都无法连接。

关键日志：

- `TCP test to 4.197.40.220:8000 => FAIL (errno=104)`
- `delayed connect error: Connection reset by peer`
- `HTTP request failed: ESP_ERR_HTTP_CONNECT`
- OTA check 也同样连接失败

## 排查过程

- ESP32 NTP 同步成功，说明 Wi-Fi 和外网基础链路可用。
- auth 与 OTA 都失败在连接建立阶段，尚未进入 HTTP 状态码或 JSON 解析。
- 从开发机访问 `http://4.197.40.220:8000/health` 也失败，说明问题不只在设备侧。
- 固件此前在 auth 失败后仍继续 OTA check，并且没有主动重试 auth，只停留在 `wifi_connecting`。

## 根因

Azure VM 的 HTTP 入口 `4.197.40.220:8000` 当前不可达或被主动 reset。设备端把 auth 失败当作一次性失败处理，缺少服务器入口不可用时的等待/重试状态。

## 解决方案

- auth 前的 TCP 预检查失败时，不再继续 auth/OTA，显示 `Server unavailable` overlay，并 30 秒后自动重试。
- auth 失败时显示 `Server auth failed` overlay，并 30 秒后自动重试。
- auth 成功前不启动 OTA check，避免同一个不可达入口产生重复错误日志。
- MQTT connected 后隐藏 overlay 并进入 `idle`。
- 在 `wifi_connecting` 状态按按钮可立即触发一次服务器重试。

## 教训

Wi-Fi 可用不代表服务端入口可用。设备状态机需要区分网络层、服务端 HTTP auth 层和 MQTT 控制通道层，并为服务端不可达提供持续重试和可见状态。
