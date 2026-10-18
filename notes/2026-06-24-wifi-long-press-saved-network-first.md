# 2026-06-24 Wi-Fi long press tries saved networks first

## 现象

用户按唤醒键后，设备经常又进入手动 Wi-Fi 配网热点。实际场景里设备已经保存过可用 Wi-Fi，误进入配网页会打断正常唤醒/对话体验。

## 排查过程

- 检查 `ButtonManager`，发现同一个 `BOOT` 键短按用于唤醒/对话，3 秒长按会触发 `EVT_BUTTON_LONG_PRESSED`。
- 检查 `Application::HandleButtonLongPressed()`，长按会直接调用 `WifiManager::StartConfigMode()`。
- 检查 `WifiManager::StartConfigMode()`，运行中调用时会写入 `force_ap` 标记并重启；下一次启动 `Init()` 看到 `force_ap` 后直接进入 AP 配网，不再尝试已保存热点。
- 旧的断线回退逻辑已经支持所有保存热点失败后再进入配网，但手动长按路径绕过了这层保护。

## 根因

长按按键路径把“请求恢复 Wi-Fi/检查网络”直接等同于“强制进入 AP 配网”。只要按键被判定为长按，即使保存的 Wi-Fi 热点就在范围内，也会跳过 STA 自动连接流程。

## 解决方案

- 新增 NVS 启动标记 `try_saved`，表示“下次启动先尝试保存的 Wi-Fi”。
- `StartConfigMode()` 在已有保存凭据时写入 `try_saved`，不再写 `force_ap`。
- `WifiManager::Init()` 消费 `try_saved` 后走正常 `InitSta()`，由现有多 SSID 重试逻辑连接保存热点。
- 保存热点全部连接失败时，沿用原有 `HandleDisconnect()` 回退逻辑写入 `force_ap` 并重启进入 AP 配网。
- `Application::HandleButtonLongPressed()` 在设备已经连上 Wi-Fi 时只显示当前 IP，不再打开配置热点。

## 教训

按键复用时，长按动作应尽量做可恢复的低风险操作。只有明确确认保存网络不可用后，才进入 AP 配网这类会中断正常使用的模式。
