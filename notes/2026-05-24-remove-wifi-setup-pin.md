# 2026-05-24 Remove Wi-Fi setup PIN

## 现象

进入 Wi-Fi 配网页时，设备会要求输入屏幕上的 6 位 PIN。实际使用中每次设置 Wi-Fi 都多了一步，影响现场配置效率。

## 排查过程

- 检查 `WifiManager::HttpGetHandler()`，发现有 cookie/query PIN 校验，不通过时返回 `PIN_HTML` 登录页。
- 检查 HTTP server 注册，`StartProvisioning()` 和 `StartConfigMode()` 都注册了 `/auth` POST 路由。
- 检查应用层 overlay，配网提示和长按 Wi-Fi 设置提示都会显示 PIN。

## 根因

旧实现为了保护已有网络配置，在存在已保存网络时强制走 PIN 登录页；新需求要求配网页直接进入设置界面。

## 解决方案

- 删除 captive portal 的 PIN 登录 HTML、`/auth` 路由和 POST PIN 校验。
- `GET /` 直接渲染 Wi-Fi 设置页。
- 配网和 Wi-Fi 设置 overlay 不再显示 PIN。
- 日志不再打印 config PIN。

## 教训

配网页用于本地现场维护时，入口认证会明显增加操作成本。若后续需要保护删除已保存网络等危险操作，应优先只保护具体危险动作，而不是阻塞整个设置入口。
