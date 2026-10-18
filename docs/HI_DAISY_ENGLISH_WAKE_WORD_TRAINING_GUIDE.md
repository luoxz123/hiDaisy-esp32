# Hi Daisy 英文唤醒词训练与发布方案

> 文档状态：项目训练规范  
> 适用项目：HiLily / HiDaisy ESP32-S3 终端  
> 目标短语：`Hi Daisy`  
> 目标语言：English（允许非母语英语口音）  
> 推理框架：microWakeWord v2 + TensorFlow Lite Micro  
> 最后核对日期：2026-10-06

## 1. 目标和非目标

本方案用于训练一个在 ESP32-S3 上离线运行的英文 `Hi Daisy` 唤醒词模型。模型只负责判断设备是否应被唤醒，不负责识别唤醒后的问题；命中后由终端播放本地欢迎语，再进入服务器 ASR/LLM/TTS 对话链路。

训练目标：

- 覆盖不同年龄、性别、口音、音量、距离和房间声学条件。
- 在电视、人声、音乐和设备自身 TTS 存在时保持低误唤醒率。
- 输出可在当前固件中直接运行的全整数量化 streaming TFLite 模型。
- 模型、manifest、训练配置、数据清单和评测报告必须一起归档，可重复训练、可回滚。

非目标：

- 不把 `Hi 黛丝`、`嗨黛丝` 自动视为英文模型的同一分布。若产品要支持中文发音，应单独采集并明确纳入多语言训练，或训练第二个模型。
- 不依赖 `idf.py menuconfig` 中的 WakeNet9 模型列表。
- 不用训练集准确率代替流式测试和真机测试。

## 2. 当前终端部署契约

下列参数来自当前 `main/micro_wake_word.cc`，新模型发布前必须逐项核对：

| 项目 | 当前值 |
|---|---:|
| 输入音频 | 16 kHz、signed 16-bit、mono PCM |
| 输入来源 | ESP-SR AFE 完成 AEC/NS 后的 PCM |
| PCM 投递帧 | 160 samples，即 10 ms |
| 特征窗 | 30 ms |
| 特征步长 | 10 ms |
| 特征维数 | 40 |
| filterbank | 125–7500 Hz |
| 模型输入 | `int8[1,3,40]` |
| 模型输出 | `uint8[1,1]` |
| 推理步幅 | 每次3个特征帧，约30 ms |
| 默认概率阈值 | 0.97 |
| 默认滑动窗口 | 5次推理结果取平均 |
| 启用后忽略窗口 | 34次，约1秒 warm-up/cooldown |
| Tensor arena | 64 KiB PSRAM |
| 模型资源 | `main/models/daisy_stream_state_internal_quant.tflite` |

当前模型没有随附原始 manifest，因此固件使用了集中定义的推断参数。**下一版禁止只交付 `.tflite`；必须同时交付 JSON manifest。** 官方/参考模型的 manifest 会记录阈值、滑窗、特征步长和 tensor arena 等部署参数；阈值升高通常减少误触发但增加漏唤醒，窗口缩小通常降低延迟但可能增加误触发。[ESPHome microWakeWord model manifest说明](https://github.com/esphome/micro-wake-word-models)

## 3. 发音定义

### 3.1 主发音

- Display text：`Hi Daisy`
- 标准参考 IPA：`/haɪ ˈdeɪzi/`
- 语义重音：主要落在 `Dai-`
- 合理自然变体：连读、轻微停顿、较弱的 `Hi`、较快或较慢语速。

### 3.2 正样本边界

正样本必须完整包含两个词。以下可以作为正样本：

- `Hi Daisy`
- 自然连读的 `HiDaisy`
- 带自然句首或句尾静音的 `... Hi Daisy ...`

以下默认不作为正样本：

- 只有 `Daisy`
- `Hey Daisy`
- `Hi, Daisy, what time is it?` 的整句长音频直接作为单一训练标签
- 中文 `嗨黛丝`

若希望一句话说完 `Hi Daisy, what time is it?`，产品还需要唤醒前后音频缓存；这不是仅靠模型训练可以解决的问题。

## 4. 数据集设计

### 4.1 推荐规模

| 数据类型 | 最低可用 | 推荐生产规模 |
|---|---:|---:|
| 真实正样本 | 500 | 2,000–5,000 |
| 真实说话人 | 20 | 50–100 |
| 合成正样本 | 5,000 | 20,000–50,000 |
| 困难负样本短句 | 2,000 | 10,000+ |
| 连续负样本音频 | 20小时 | 100–500小时 |
| 真机目标环境录音 | 2小时 | 10–30小时 |

合成语音扩大覆盖面，真实录音负责修正 TTS 与真实人声之间的 domain gap。官方 microWakeWord 数据流程也同时使用合成正样本、对抗短语、背景声音、音乐、环境噪声和房间脉冲响应。[官方数据源说明](https://github.com/kahrendt/microWakeWord/blob/main/documentation/data_sources.md)

### 4.2 真实正样本采集矩阵

优先覆盖产品用户，而不是只采集研发人员：

- 年龄：18–39、40–64、65+；老年用户至少占真实正样本30%。
- 性别与音高：尽可能均衡。
- 口音：新西兰、澳大利亚、英式、美式，以及普通话母语者的英语口音。
- 距离：0.3 m、1 m、2 m、3 m。
- 方向：正面、侧面、背对设备。
- 音量：轻声、正常、大声；避免要求受试者刻意表演不自然发音。
- 房间：安静卧室、客厅、厨房、走廊、混响房间。
- 设备状态：静音、风扇声、电视、音乐、设备扬声器刚结束播放。

每条原始录音建议保留2–4秒，含自然前后静音；不要预先做自动增益、强降噪或动态压缩。保留原始高质量母版，再派生16 kHz mono训练文件。

### 4.3 困难负样本（hard negatives）

困难负样本决定实际误唤醒表现，至少包括：

- 近音短语：`Hi lazy`、`Hi Stacy`、`Hi Maisie`、`Hi Tracy`、`Hi Casey`、`Hi baby`。
- 名字变体：`Daisy`、`Hey Daisy`、`Okay Daisy`、`Bye Daisy`。
- 常见助手词：`Hey Siri`、`Alexa`、`Okay Google`。
- 包含部分音素的自然句子，如 `a lazy day`、`high daily rate`。
- 电视对白、播客、电话免提、多人交谈、笑声、咳嗽、餐具声、音乐歌词。
- 当前 Daisy TTS 音色说出的 `Daisy`、`Hi Daisy` 和欢迎语，用于抑制自唤醒。
- 真实部署产生的每一条误触发录音。

困难负样本不能包含真实完整唤醒词，否则会形成互相矛盾的标签。误触发音频应保存到独立的 `hard_negative_mining/`，记录固件版本、阈值和现场条件。

### 4.4 数据授权与隐私

- 每个数据源保留许可证、下载地址、版本和校验和。
- 录制真人声音必须获得训练和内部评测授权。
- 原始录音不得提交到固件仓库；只提交配置、清单、统计和经批准的小型回归集。
- 训练、验证、测试的背景数据源也必须隔离，不能把同一长音频的切片放进不同集合。

## 5. 数据目录与命名

建议训练仓库结构：

```text
hi_daisy_training/
├── configs/
│   ├── wake_phrases.yaml
│   ├── hard_negatives.yaml
│   └── training_parameters.yaml
├── data/
│   ├── raw_positive_real/
│   ├── raw_positive_synthetic/
│   ├── hard_negatives/
│   ├── continuous_negatives/
│   ├── noise/
│   └── rir/
├── splits/
│   ├── train.csv
│   ├── validation.csv
│   └── test.csv
├── experiments/
│   └── hi_daisy_vNN/
├── export/
│   ├── hi_daisy_vNN.tflite
│   ├── hi_daisy_vNN.json
│   ├── metrics.json
│   └── SHA256SUMS
└── README.md
```

文件名至少编码匿名 speaker ID、session、distance、room 和 take，例如：

```text
spk023_session02_room_living_1m_take04.wav
```

## 6. 音频预处理与增强

### 6.1 标准化

- 解码后统一为16 kHz、mono、signed 16-bit PCM。
- 禁止峰值归一化到接近0 dBFS；应保留真实音量差异。
- 检测削波、全零、过短、错误采样率和重复文件。
- 保存转换前后的 SHA-256、处理脚本版本和处理日志。

### 6.2 增强建议

对每条正样本随机组合，而不是永远套用全部增强：

- 增益：约 -12～+6 dB。
- 速度：0.90～1.10；不要改变到不自然发音。
- 时间偏移：随机前后静音。
- 背景噪声 SNR：建议覆盖 -5～25 dB，验证集重点覆盖0、5、10、20 dB。
- 房间脉冲响应：小房间、客厅、走廊及远场。
- 频响变化：模拟麦克风、外壳和说话方向。
- 轻量带限或编解码失真：仅用于鲁棒性，比例不宜过高。

训练增强必须贴近终端实际链路。当前模型输入是 **AFE AEC/NS后的音频**，因此最终真机验证一定要经 INMP441、外壳和实际 AFE；不能只在桌面原始 WAV 上下结论。

## 7. 数据划分与防泄漏

推荐 `80% train / 10% validation / 10% test`，但必须满足：

- 按说话人分组：同一说话人不能跨集合。
- 按原始音频分组：同一录音的增强版本只能属于一个集合。
- 按 TTS voice 分组：至少保留若干完全未参与训练的 voice 做测试。
- 按噪声和 RIR 源分组：测试集包含训练中未见来源。
- 最终 test 集训练期间只运行有限次数，阈值调优使用 validation 集。

另建 `field_regression` 集，专门保存量产设备收集到的误触发和漏触发案例；它不参与训练结果的主测试评分。

## 8. 训练环境与可复现性

推荐以官方 `kahrendt/microWakeWord` 为基线，并固定 commit SHA；官方模型集合也将其作为自定义训练入口。[microWakeWord](https://github.com/kahrendt/microWakeWord) / [官方模型集合](https://github.com/esphome/micro-wake-word-models)

每次实验必须记录：

- Git commit、Python版本、TensorFlow版本、CUDA/cuDNN版本。
- GPU型号、随机种子、训练时长。
- 数据清单哈希和各集合数量。
- 模型结构、stride、batch size、学习率计划、epoch和早停规则。
- 正负采样比例、所有增强概率。
- 导出和量化使用的代表性数据集。

优先使用容器或固定的 Colab notebook 快照，不要依赖“当前最新版”环境。社区训练器可用于快速实验，但生产模型仍需保存其底层 microWakeWord commit、补丁和完整配置，不能只保存 notebook 输出。

## 9. 建议训练流程

### 阶段 A：基线

1. 校验所有 WAV 和 metadata。
2. 生成 speaker/source-disjoint splits。
3. 用英文正样本、普通负样本和初始困难负样本训练 v1。
4. 监控 positive recall、validation loss 和估算 false accepts/hour。
5. 选择最佳 checkpoint，而不是自动使用最后一个 epoch。

### 阶段 B：困难负样本迭代

1. 用 v1 扫描长时间电视、播客、音乐和房间录音。
2. 保存高概率但错误的窗口。
3. 人工复核，排除真正包含 `Hi Daisy` 的片段。
4. 加入 hard negatives 重训 v2。
5. 使用完全相同的测试集 A/B 对比 v1、v2。

### 阶段 C：目标硬件适配

1. 在 HiLily 真机上录制 AFE 输出或等价诊断数据。
2. 覆盖真实外壳、INMP441、扬声器回声和不同距离。
3. 针对漏唤醒补充真实正样本；针对误唤醒补充困难负样本。
4. 不允许把最终测试案例直接混入训练集后仍宣称同一测试结果有效。

## 10. 量化与导出

最终输出应为 streaming state-internal、全整数量化 TFLite：

```text
input:  int8  [1, 3, 40]
output: uint8 [1, 1]
```

发布前必须用脚本检查：

- TFLite schema可由当前 `esp-tflite-micro 1.3.3~1` 加载。
- 输入/输出 dtype、shape与固件一致。
- 所有算子都在当前13个 resolver算子集合内；若新增算子，先评估 flash/RAM，再修改固件。
- `AllocateTensors()` 在设定 arena 内成功，并记录实际最小 arena。
- 使用代表性正负数据做 INT8 校准，不能只用正样本。
- 导出模型的 SHA-256 与 manifest、评测报告一致。

## 11. Manifest 模板

每个模型必须随附类似文件；数值以实际评测结果为准：

```json
{
  "type": "micro",
  "wake_word": "Hi Daisy",
  "author": "HiLily Team",
  "website": "PROJECT_REPOSITORY_OR_MODEL_CARD_URL",
  "model": "hi_daisy_v3.tflite",
  "trained_languages": ["en"],
  "version": 2,
  "micro": {
    "probability_cutoff": 0.97,
    "sliding_window_size": 5,
    "feature_step_size": 10,
    "tensor_arena_size": 65536,
    "minimum_esphome_version": "2024.7.0"
  }
}
```

即使项目不直接使用 ESPHome，该 manifest 仍是模型与推理器之间的版本化契约。参考 v2 模型通常使用上述字段结构。[v2 manifest实例](https://github.com/esphome/micro-wake-word-models/pull/20/files)

## 12. 离线评测

### 12.1 必须模拟真实流式判定

不能只看单个分类窗口。评测器必须完全复现固件：

1. 每10 ms生成40维特征。
2. 每3帧调用一次 streaming模型。
3. 保存模型内部状态。
4. 对最近5个输出取平均。
5. 平均值达到阈值才触发。
6. 触发后应用与固件相同的 cooldown。

### 12.2 核心指标

- **Recall / True Accept Rate**：正样本成功唤醒比例。
- **False Reject Rate (FRR)**：正样本漏唤醒比例。
- **False Accepts per Hour (FAH)**：连续负样本每小时误触发次数。
- **Detection latency**：从唤醒词结束到触发的延迟。
- **CPU realtime factor**：推理速度必须持续快于实时音频。

### 12.3 建议发布门槛

| 场景 | 建议门槛 |
|---|---:|
| 安静、0.3–1 m | Recall ≥ 95% |
| 正常家庭噪声、1–2 m | Recall ≥ 90% |
| 较强噪声或3 m | Recall ≥ 80% |
| 普通连续语音/环境音 | FAH ≤ 0.5 |
| 电视、播客、音乐压力集 | FAH ≤ 1.0 |
| 词尾至触发 P95 延迟 | ≤ 700 ms |
| 24小时稳定性 | 无WDT、无内存持续下降、无推理停止 |

这些是本项目建议验收线，不是 microWakeWord 官方保证值。若业务更重视“不误醒”，优先降低 FAH；若面向行动不便的老年用户，需同时关注较轻声和较远距离 recall。

## 13. 阈值与滑窗调优

固定模型权重后，在 validation 流式集上网格搜索，例如：

```text
probability_cutoff: 0.85, 0.90, 0.93, 0.95, 0.97, 0.98, 0.99
sliding_window_size: 3, 4, 5, 6, 8
```

原则：

- 阈值升高：通常减少 FAH，但提高 FRR。
- 窗口增大：通常减少瞬时误触发，但增加延迟，可能漏掉较短发音。
- 选择满足 FAH 门槛下 recall 最高的组合，不按单次主观测试决定。
- 最终参数写入 manifest，并同步固件常量；禁止只改一侧。

当前0.97/5只是既有模型的部署基线，不是所有新模型的固定答案。

## 14. 真机测试矩阵

每个候选模型至少在2台设备上测试：

| 维度 | 测试条件 |
|---|---|
| 距离 | 0.3、1、2、3 m |
| 方向 | 正面、侧面、背面 |
| 音量 | 轻声、正常、大声 |
| 噪声 | 安静、电视、音乐、厨房、风扇 |
| 用户 | 至少10人，含老年人和非母语英语口音 |
| 状态 | 冷启动、长时间Idle、网络断开、TTS结束后 |
| 压力 | 8小时电视/播客；24小时稳定性 |

预期正常日志：

```text
mww: Daisy inference enabled
mww: Hi Daisy detected (avg=..., max=...)
app: Wake word detected!
mww: Daisy inference paused
audio: Daisy greeting played (... ms)
state_machine: State: idle -> listening
```

同时监控：

- `Inference lag, evicted ...`：频繁出现说明CPU预算或队列连续性有问题。
- Task WDT、AFE ringbuffer overflow、内存下降。
- TTS播放期间是否自唤醒；当前固件应在播放和连续对话期间暂停MWW。
- 网络故障不能被误判成模型失败：本地命中日志在服务器连接之前即可确认。

## 15. 固件集成步骤

1. 将候选模型和 manifest 放入版本化目录，例如：

   ```text
   main/models/hi_daisy/v3/hi_daisy_v3.tflite
   main/models/hi_daisy/v3/hi_daisy_v3.json
   ```

2. 更新 `main/CMakeLists.txt` 的 embedded binary路径。
3. 更新 `main/micro_wake_word.cc` 的链接器资源符号。
4. 从 manifest同步阈值、滑窗、特征步长和 arena；检查 tensor shape和算子。
5. 执行完整 `idf.py build`，确认分区余量。
6. 烧录后先看模型初始化，再做静音、正样本、困难负样本和长时间压力测试。
7. 不覆盖上一量产模型；保留可回滚版本。

建议后续把 manifest解析或生成固件常量纳入构建流程，减少手工同步错误。

## 16. 版本发布包

每个候选版本至少包含：

```text
hi_daisy_vNN.tflite
hi_daisy_vNN.json
model_card.md
training_parameters.yaml
dataset_manifest.csv
split_manifest.csv
metrics_offline.json
metrics_device.csv
false_accept_examples.csv
SHA256SUMS
```

`model_card.md` 应记录：目标用户、训练语言/口音、已知弱点、数据许可证、伦理/隐私说明、推荐阈值、测试硬件、固件 commit和回滚版本。

## 17. 现场闭环

上线后采用固定迭代流程：

1. 用户报告误唤醒或漏唤醒。
2. 在得到授权的前提下保存短音频和现场 metadata。
3. 先用当前模型离线重放，确认可复现。
4. 误唤醒加入 hard-negative候选；漏唤醒加入 positive候选。
5. 数据审核、去重并重新划分，避免污染最终测试集。
6. 重训候选模型，与当前量产模型做同集 A/B。
7. 只有通过全部门槛才替换量产模型。

阈值微调可以快速缓解问题，但不能替代数据闭环。连续误触发应优先补充真实困难负样本；特定人群漏唤醒应优先补充该口音、年龄和距离的真实正样本。

## 18. 执行清单

### 训练前

- [ ] 明确只支持英文 `Hi Daisy`，或批准多语言范围。
- [ ] 完成录音授权和数据许可证清单。
- [ ] 固定训练代码 commit、环境和随机种子。
- [ ] 完成 speaker/source-disjoint 数据划分。
- [ ] 建立困难负样本和现场回归集。

### 导出前

- [ ] 检查 `int8[1,3,40] -> uint8[1,1]`。
- [ ] 检查算子集合和 tensor arena。
- [ ] 输出 JSON manifest和 SHA-256。
- [ ] 使用真实流式逻辑评测阈值/滑窗。
- [ ] 生成模型卡与数据统计。

### 发布前

- [ ] 安静/噪声/距离/口音 recall达标。
- [ ] 普通负样本和电视压力集 FAH达标。
- [ ] 真机延迟、CPU、内存和24小时稳定性达标。
- [ ] 验证设备自身 TTS不触发。
- [ ] 验证断网时本地检测仍正常，但不会错误进入在线会话。
- [ ] 保留上一模型和一键回滚方法。

## 19. 本项目相关文件

- `main/micro_wake_word.cc`：当前特征前端、TFLite推理和判定逻辑。
- `main/micro_wake_word.h`：16 kHz PCM输入契约。
- `main/afe_processor.cc`：AEC/NS输出到MWW的实际音频路径。
- `main/CMakeLists.txt`：模型嵌入固件配置。
- `main/models/daisy_stream_state_internal_quant.tflite`：当前模型。
- `notes/2026-08-08-microwakeword-daisy-integration.md`：首次集成记录。
- `notes/2026-08-09-microwakeword-cpu-backlog-watchdog.md`：实时性与WDT经验。
- `notes/2026-08-10-microwakeword-contiguous-frame-recall.md`：连续特征帧经验。
- `notes/2026-08-11-daisy-repeat-during-tts-drain.md`：扬声器回声误唤醒经验。

---

本方案的核心原则是：**模型不是一个孤立的 `.tflite` 文件，而是数据、特征前端、流式状态、阈值、滑窗、硬件音频链路和评测证据共同组成的版本化产品。**
