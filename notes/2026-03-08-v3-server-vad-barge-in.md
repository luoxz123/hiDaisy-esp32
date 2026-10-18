# V3 服务端 Silero VAD Barge-in 实施记录

**日期**: 2026-03-08
**Issue**: hilily-server#259, hilily-esp32#176
**PR**: hilily-server#260

## 背景

V1/V2 在 ESP32 设备端做 VAD 检测 barge-in，但 AEC 残余导致 AI 自我打断（误触发）。
V3 改为混合架构：ESP32 做 AEC + 持续上传音频，服务端用 Silero VAD 做语音检测。

## 踩坑记录

### 坑 1: Silero VAD ONNX 模型版本差异

**现象**: 按照网上的示例代码用 `h`, `c` 分离的隐藏状态，结果报错 `Required inputs (['state']) are missing`

**根因**: 最新版 Silero VAD v5 的 ONNX 模型输入格式变了：
- 旧版: 分离的 `h` [2,1,64] + `c` [2,1,64]
- 新版: 合并的 `state` [2,1,128]

**排查**:
```python
session = ort.InferenceSession('silero_vad.onnx')
for i in session.get_inputs():
    print(f'{i.name}: {i.shape}')
# input: [None, None]
# state: [2, None, 128]  ← 合并了！
# sr: []
```

**解决**: 改为 `state = np.zeros((2, 1, 128))` 单 tensor

**教训**: 使用 ONNX 模型前必须先检查实际的 input/output schema，不要照搬教程代码。

### 坑 2: ONNX 输出也需要注意 shape

**现象**: `float(result[0])` 报错 `only 0-dimensional arrays can be converted to Python scalars`

**根因**: 输出 shape 是 `(1, 1)` 而非标量

**解决**: `float(out.item())` 或 `float(out[0][0])`

### 坑 3: pip 和 python3 指向不同版本

**现象**: `pip install` 成功但 `python3 -c "import onnxruntime"` 失败

**根因**: Mac 上 `pip` 指向 Python 3.12，`python3` 指向 Python 3.14

**解决**: `python3 -m pip install` 确保安装到正确的 Python 版本

## N100 压力测试结果

| 场景 | 实时率 | 帧延迟(中位) | P99 |
|------|--------|-------------|-----|
| 1 设备 | 0.006x | 163µs | 198µs |
| 4 并发 | 0.006x/设备 | 305µs | 463µs |
| 8 并发 | 0.007x/设备 | 544µs | 5.1ms |
| 持续 20 轮 | 稳定 | 182µs | 228µs |

结论：N100 完全没有压力，8 设备并发仍有 363x 实时余量。

## 关键设计决策

### 为什么用 ONNX-only 而非 PyTorch
- PyTorch: ~700MB 依赖，Docker 镜像巨大
- ONNX-only: ~30MB (onnxruntime) + ~2MB (模型) = 32MB

### 音频路由保证
`feed_audio()` 中 SPEAKING+AEC 时音频 **只进 VAD 检测器，不进 ASR 队列**：
```python
if current_state == DeviceState.SPEAKING and self._aec_enabled and self._barge_in_detector:
    await self._barge_in_detector.feed(pcm)
    return  # 不入 ASR 队列
```

### 兜底开关
ESP32 hello 不发 `features.aec: true` 即可完全禁用服务端 VAD。

## 文件变更

### 服务端 (hilily-server)
- `src/services/barge_in_detector.py` — 新建，ONNX Silero VAD 检测器
- `src/services/pipeline.py` — feed_audio 路由 + barge-in 回调
- `src/services/mqtt_gateway.py` — 解析 features.aec
- `requirements.txt` — 添加 onnxruntime
- `models/silero_vad.onnx` — 模型文件
- `tests/stress_vad.py` — 压力测试脚本
- `docs/VAD_DEPLOYMENT.md` — 部署文档

### ESP32 端 (hilily-esp32)
- `main/application.cc` — 移除 StopCapture, 移除 V2 设备端 barge-in, 添加 features.aec
- `main/application.h` — 移除 V2 常量
