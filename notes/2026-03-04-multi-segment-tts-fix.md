# 多段 TTS 播放修复

> 日期: 2026-03-04 | Issue: #143 | PR: #144

## 问题
多句 TTS 只播第一句；搜索后无回复

## 根因
服务端按句拆分 TTS：`tts_start → frames → tts_end → state_change(speaking→thinking)` 循环。
ESP32 两个 Bug：
1. **状态机缺 `speaking→thinking` 转换** → 被拒绝后卡在 speaking，后续同态转换不触发回调
2. **第一句 `tts_end` 触发 drain** → 后续音频虽入队但无人消费

## 修复
- `state_machine.cc`: 增加 `speaking→thinking` 合法转换
- `msg_router.cc`: 新增 `tts_start` → `ResetTtsEnd()`（重置 drain 计时）
- `application.cc`: 注册 `OnTtsStart` 回调

## 踩坑
1. **same-state transition 不触发回调**: `TransitionTo()` 对 old==new 直接 return，不调用回调 — 状态不变时不能依赖回调触发
2. **PSRAM 队列需 `idf_additions.h`**: `xQueueCreateWithCaps()` 在 ESP-IDF v5.5+ 的扩展头文件
3. 修复前 55 帧停止 → 修复后 778 帧完整播放（~47s）
