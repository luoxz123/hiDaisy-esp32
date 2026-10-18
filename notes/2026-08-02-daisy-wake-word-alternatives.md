# Hi Daisy / Hi 黛丝唤醒方案评估

## 现象

ESP-SR 2.3.1 内置 WakeNet9 模型列表没有 `Hi Daisy / Hi 黛丝`，且用户希望避免修改 ESP-SR 的 menuconfig 模型菜单。

## 排查过程

1. 检查当前音频链路：麦克风常开并持续进入 AFE，但只有 `capturing_ == true` 时，AFE 输出才写入 Opus 编码缓冲并通过 UDP 上行。
2. 检查本地依赖：项目未集成 TensorFlow Lite Micro、microWakeWord、Porcupine、openWakeWord、MultiNet 或 LilyWarp 实现。
3. 核对 ESP-SR 官方资料：WakeNet 自定义短语仍需相应模型权重；MultiNet 支持运行时定制命令，但定位为唤醒后的命令词识别，且中英混合命令不受支持。
4. 调研替代方案：microWakeWord 可生成面向 ESP32-S3 的 INT8 TFLite 自定义模型；Porcupine 支持自定义关键词但依赖 AccessKey/授权与目标平台 SDK；openWakeWord 更适合服务器而非直接运行在 ESP32-S3。

## 根因

唤醒检测是声学模型推理，不是字符串匹配。没有 Daisy 的模型权重时，既不能通过修改 WakeNet 配置实现，也不能通过服务端文本字段实现。

## 解决方案

推荐顺序：

1. **本地 microWakeWord**：训练 `Hi Daisy` 模型，设备端对 AFE 的 16kHz 单声道输出做推理，命中后复用现有 `EVT_WAKE_WORD_DETECTED`。不依赖 ESP-SR WakeNet 菜单，也不持续上传音频。
2. **正式 WakeNet 定制模型**：获得乐鑫生成的 Daisy 权重后直接固定打包和加载，无需暴露 menuconfig 选择项。
3. **云端 KWS**：空闲状态持续上传低码率音频，服务端用 openWakeWord/其他 KWS 检测后下发唤醒事件；只适合可接受持续联网、隐私和服务器成本的部署。
4. **LilyWarp/DTW**：适合少量目标用户录制模板的原型或个性化设备，但跨用户、口音、距离和噪声泛化通常弱于神经网络 KWS。

## 教训

- 云端唤醒必须单独设计空闲态音频上行和鉴权/断线策略，不能复用当前“唤醒后才开始编码”的链路而不做修改。
- 项目内部 RAM 曾因 AFE/AEC/Wi-Fi 出现 OOM；集成任何新本地推理引擎时，模型张量和任务栈应优先放 PSRAM，并实测内部 RAM、水位、CPU 实时率。
- `Hi Daisy` 与 `Hi 黛丝` 是两种语言/发音数据分布。若期望两种说法都能触发，训练集必须明确同时包含两者，而不是只修改显示名称。
- 替代引擎应直接消费现有 AFE 清理后的 16kHz/16-bit/mono PCM，并通过回调或队列产生现有唤醒事件，避免第二个任务读取 I2S。
