# microWakeWord Daisy 本地模型集成

## 现象

ESP-SR WakeNet9 的预置模型列表没有 `Hi, Daisy / Hi, 黛丝`，但已有训练完成的
`stream_state_internal_quant.tflite`，需要在不增加第二个 I2S 读取者的前提下接入现有唤醒事件。

## 排查过程

- 检查现有音频链路，确认 `AfeProcessor` 已输出 16 kHz、16-bit、单声道的 AEC/NS 清理后 PCM。
- 解析 TFLite 模型，确认输入为 `int8[1,3,40]`、输出为 `uint8[1,1]`，并列出模型所需的 13 个算子。
- 模型没有随附 microWakeWord JSON manifest，因此采用 v2 常用的 30 ms 窗、10 ms 步长、40 通道特征参数，以及 0.97 阈值和 5 帧滑动窗口作为初始配置。
- 首次 Windows 全量构建受到共享 ccache 临时文件冲突影响，关闭 ccache 后完成构建。
- 链接阶段发现 `target_add_binary_data` 生成的符号不包含 `models_` 路径前缀；按目标文件中的实际符号名修正。

## 根因

WakeNet 只能加载 Espressif 提供/生成的 WakeNet 模型，通用 microWakeWord TFLite 不能通过
menuconfig 增加到 WakeNet9 列表。新模型必须使用 TFLite Micro 推理，并在其训练所需的音频特征前端后运行。

## 解决方案

- 将 Daisy 模型作为固件二进制资源嵌入，不依赖 SPIFFS 或云端。
- 引入 `esp-tflite-micro`、`esp-nn` 和 `esp-micro-speech-features`。
- 禁用 WakeNet 模型，保留 ESP-SR AFE 的 AEC/NS；直接把 AFE 清理后的 PCM 投递给 microWakeWord。
- AFE 任务只向 10 ms PSRAM 队列写入音频；独立 Core 0、PSRAM 栈任务执行特征计算和推理，避免阻塞音频链路。
- 推理成功后复用 `EVT_WAKE_WORD_DETECTED`，因此状态机和后续 `start_dialogue` 流程无需分叉。
- TFLite tensor arena、resource-variable arena、解释器和队列均放入 PSRAM，降低内部 RAM 压力。

## 验证

- ESP-IDF 5.5.3 全量构建通过。
- 生成固件 `build/hilily-esp32.bin`，大小 `0x214ba0`；最小 app 分区 `0x300000`，剩余 31%。
- 输入、输出张量类型和维度在启动时校验；不匹配会退化为按键唤醒而不是启动错误模型。

## 教训

- 自训练 microWakeWord 模型应与 `.tflite` 一起保存 manifest；阈值、滑窗、特征参数和训练版本都是部署契约。
- 音频识别引擎不能各自读取 I2S；应共享 AFE 的单一 PCM 出口。
- ESP32-S3 内部 RAM 紧张，模型 arena、任务栈和队列应优先使用 PSRAM，并通过有界队列隔离实时音频任务。
- 链接嵌入资源时，应从生成的目标文件确认实际 `_binary_*` 符号，不能仅根据源相对路径推测。
