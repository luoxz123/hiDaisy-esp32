# 唤醒词更换路径分析

## 现象

项目中同时存在 `main/wake_word.cc` 和 `main/afe_processor.cc` 两套 WakeNet 相关代码，容易误以为修改 `WAKENET_MODEL_NAME` 或协议中的唤醒词文本即可更换唤醒词。

## 排查过程

1. 全仓搜索 WakeNet、ESP-SR、唤醒事件和模型配置。
2. 确认 `Application` 初始化和注册的是 `AfeProcessor` 的唤醒回调。
3. 确认 `AfeProcessor` 从 `model` SPIFFS 分区加载模型，并选择第一个 `ESP_WN_PREFIX` 模型。
4. 检查 `sdkconfig`、`sdkconfig.defaults` 和 `build/srmodels`，当前选择和打包的模型均为 `wn9_hilili_tts`。
5. 检查烧录脚本，确认 `build/srmodels/srmodels.bin` 会写入 `0x610000` 的 `model` 分区。

## 根因

唤醒词由 ESP-SR WakeNet 模型权重决定，不由 C++ 字符串决定。`main/wake_word.cc` 是遗留实现，当前运行链路使用 `main/afe_processor.cc`。`application.cc` 中 `listen(detect)` 的 `text` 字段只是上报给服务端的显示/协议文本。

## 解决方案

- 更换为 ESP-SR 内置唤醒词：在 `menuconfig` 或 `sdkconfig.defaults` 中取消 `CONFIG_SR_WN_WN9_HILILI_TTS`，启用目标 `CONFIG_SR_WN_*` 模型，然后重新配置、构建并烧录包含 `srmodels.bin` 的完整固件。
- 更换为列表外的自定义唤醒词：先取得兼容当前 ESP-SR/WakeNet 版本的定制模型，再将模型接入 ESP-SR 的模型打包流程；单纯改模型名或协议文本无效。
- 更换后同步更新 `application.cc` 的 `listen(detect).text` 文案，但该修改只影响服务端收到的文本。

## 教训

- 判断实际唤醒实现应从初始化和回调注册链路入手，不能仅凭文件名。
- 必须同时烧录 `model` 分区；只更新 app OTA 分区不会替换唤醒模型。
- 当前实现用 `esp_srmodel_filter(..., ESP_WN_PREFIX, NULL)` 取第一个 WakeNet 模型，不应同时打包多个候选模型，除非代码改为按确切名称选择。
- 阈值 `0.5` 只调节灵敏度/误唤醒率，不能改变唤醒短语。
