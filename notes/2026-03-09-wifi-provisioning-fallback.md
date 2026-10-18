# WiFi 配网回退 — 有凭证但连不上时卡在水波纹动画

**日期**: 2026-03-09

## 现象

设备 NVS 中有保存的 WiFi 凭证，但该网络不可达时，设备卡在开机水波纹动画界面（"Initializing..."），永远不会进入配网模式。

## 排查过程

1. 追踪启动流程：`Application::Start()` → `WifiManager::Init()` → `LoadAllNetworks()` 返回 true → `InitSta()` → STA 模式
2. WiFi 连接失败后 `HandleDisconnect()` 会重试（每个网络 5 次），所有网络都失败后调用 `StartProvisioning()`
3. **但应用层完全不知道 WifiManager 进入了配网模式**，因为没有回调通知
4. `HideBootAnimation()` 只在两个地方被调用：
   - `Start()` 中的内联检查：仅在初始化时 `IsProvisioning()` 为 true 时触发
   - `HandleWifiConnected()`：仅在 WiFi 连接成功时触发
5. 结果：水波纹动画永远显示，用户无法知道设备已进入配网模式

## 根因

`WifiManager::HandleDisconnect()` → `StartProvisioning()` 这条异步路径缺少向应用层的通知机制。

## 解决方案

1. **WifiManager 添加配网回调** (`OnProvisioningStart`)
   - `StartProvisioning()` 结束时触发回调
   - 覆盖两种场景：首次启动（无凭证）和连接失败后的回退

2. **Application 添加事件处理**
   - 新增 `EVT_PROVISIONING_STARTED` (bit 14)
   - 回调中 `NotifyEvent()` → `Run()` 事件循环中 `HandleProvisioningStarted()`
   - `HandleProvisioningStarted()`: 隐藏水波纹动画 + 显示配网 overlay

3. **双路径保障**
   - 同步路径：`Start()` 中仍然检查 `IsProvisioning()` 直接调用 `HandleProvisioningStarted()`
   - 异步路径：运行中连接失败回退到配网时，通过事件触发

## 修改文件

- `main/wifi_manager.h`: 添加 `ProvisioningCallback` 类型和 `OnProvisioningStart()` 方法
- `main/wifi_manager.cc`: 实现回调注册，`StartProvisioning()` 中触发回调
- `main/application.h`: 添加 `EVT_PROVISIONING_STARTED` 事件和 `HandleProvisioningStarted()` 方法
- `main/application.cc`: 注册回调、事件处理、提取公共方法

## 教训

- 异步状态变更（如 WiFi 从 STA 模式回退到 AP 配网模式）必须有回调通知机制
- 不能只在初始化时检查状态，运行中的状态变更也需要处理
- 遵循已有模式（`OnStatusChange` 回调 → 事件位 → Run() 处理）保持一致性
