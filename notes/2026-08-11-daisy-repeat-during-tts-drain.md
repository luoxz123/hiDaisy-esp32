# Daisy 在 TTS 播放期间重复回应

## 现象

说出 `Hi Daisy` 后，本地欢迎语 `I am Daisy. How can I help you?` 正常播放一次；随后服务器 TTS 尚未实际播放完毕时，设备再次检测到 `Hi Daisy`，导致欢迎语重复播放，并伴随 `i2s_channel_write: channel is not enabled`。

## 排查过程

- 日志中第一次 `Hi Daisy detected` 后，本地欢迎语正常结束并进入对话。
- 服务器较早发送 `speaking -> idle`，但 PlaybackTask 仍在排空下行音频。
- 状态回调在进入 `idle` 时立即重新启用 microWakeWord。
- 扬声器播放内容被麦克风回采，触发第二次唤醒；第二次本地直播放音又与 PlaybackTask 争用输出 I2S。

## 根因

microWakeWord 的启用条件只检查设备逻辑状态是否为 `idle`，没有检查：

1. 连续对话是否仍在进行；
2. 扬声器音频是否已真正播放完毕。

服务器状态与设备物理播放完成时间并不完全同步，因此逻辑 `idle` 不能单独作为重新启用唤醒词的依据。

## 解决方案

- 状态切换到 `idle` 时，仅在非连续对话且 `AudioService::IsPlaying()` 为 false 时启用 microWakeWord。
- 连续对话结束后，同样等待设备处于 `idle` 且播放已停止再启用。
- 在 `OnPlaybackComplete()` 中处理最终启用：连续对话按原逻辑重新监听；非连续模式且处于 `idle` 时恢复 microWakeWord。

## 验证

- ESP-IDF 增量构建成功。
- 固件 `build/hilily-esp32.bin` 大小 `0x230730`，应用分区剩余 27%。

## 教训

唤醒检测的恢复必须以“物理播放完成”为边界，不能只依赖服务器状态。播放期间开启唤醒不仅会造成声学回声误触发，还可能让本地提示音与网络 TTS 同时操作输出 I2S。
