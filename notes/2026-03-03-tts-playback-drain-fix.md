# TTS 播放中断修复

> 日期: 2026-03-03 | Issue: #141 | PR: #142

## 问题
- 长 TTS 回复说到一半就停（多句只播第一句）
- 搜索工具调用需 5-10s，drain 超时导致提前停止

## 根因
1. drain 设为"连续 1s 无数据即停止"，但句间间隙通常 1-3s
2. `tts_end_callback_` 已声明但**从未注册**，服务端 `tts_end` 信号被静默忽略

## 修复
drain 逻辑改为：
```
if (tts_end_received && empty_count >= 10)   → 正常停止 (1s drain)
if (!tts_end_received && empty_count >= 300)  → 兜底停止 (30s)
```
关键：在 `application.cc` 注册 `OnTtsEnd()` → `AudioService::SignalTtsEnd()`

## 踩坑
1. **callback 声明但未注册** — 必须在 `application.cc` 中显式注册，缺少时静默失效
2. **StopPlayback 需重置 `tts_end_received_`** — 否则 barge-in 后残留 true，下次误触 drain
3. 队列太小（64 frames）→ 改为 256，后续扩至 4096（PSRAM）
