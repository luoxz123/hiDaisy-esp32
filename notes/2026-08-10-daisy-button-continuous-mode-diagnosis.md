# 按键唤醒后呼叫 Hi Daisy 无本地命中

## 现象

用户按下唤醒键后多次呼叫 `Hi Daisy`，感觉设备没有立即通过本地唤醒词响应。

## 排查过程

- 32.786 秒按键事件被终端正确接收，状态立即从 `idle` 进入 `listening`。
- 同一时刻日志明确显示 `Daisy inference paused`。
- 麦克风采集与 Opus 编码正常启动，服务端随后返回 `listening -> thinking -> speaking`、TTS 音频和状态更新。
- 整份约 529 秒日志统计：Daisy enabled 17 次、paused 18 次、本地 `Hi Daisy detected` 0 次、`Wake word detected` 0 次、服务端 TTS start 21 次。
- 没有 task WDT、microWakeWord 队列满或 AFE ringbuffer 满。

## 根因

按键本身已经完成唤醒并进入连续对话模式。为避免对话音频和扬声器 TTS 造成重复/误唤醒，microWakeWord 按设计只在真正的 `idle` 状态运行，在 `listening/thinking/speaking` 状态暂停。因此按键后说出的 `Hi Daisy` 会作为普通对话音频上传服务器，而不会产生本地唤醒事件。

连续模式在每次 TTS 播放结束后自动重新进入 listening，日志中的 idle 多为等待 TTS drain 的短暂中间状态，并非适合测试本地唤醒的静默待机状态。

## 解决方案

- 测试本地 Daisy 时不要先按唤醒键；等待日志出现 `Daisy inference enabled`，并确认设备没有处于连续对话/播放状态，再直接说 `Hi Daisy`。
- 预期日志顺序为：`Daisy inference enabled` → `Hi Daisy detected` → `Wake word detected!` → `idle -> listening` → `Daisy inference paused`。
- 若产品希望按键仅用于“启用唤醒词测试”而不是直接开始对话，需要另行修改按键语义；当前实现的按键行为是直接进入 listening。

## 教训

- 测试唤醒词必须区分“待机唤醒”和“已经进入对话后说出唤醒词”两个场景。
- 日志应结合状态门控判断；模型 paused 时没有 detection 并不代表模型或服务器异常。
- 服务端是否正常可由 thinking、TTS control frame 和 UDP 音频回包独立验证，不能用本地唤醒日志判断。
