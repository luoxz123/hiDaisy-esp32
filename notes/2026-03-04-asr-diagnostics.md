# ASR 诊断与优化

> 日期: 2026-03-04 | Issue: #145/#147 | PR: #146

## 问题
Whisper 中文同音字识别错误（"金价"→"晶片环境大哥"）

## 诊断
- 服务端保存 WAV (`/app/audio_logs/`)，分析 RMS 波形
- 结论：麦克风正常（语音 RMS 峰值 30446），问题是**前导静音过长**（2.5s 静音 + 1.1s 语音）

## 修复
1. 服务端裁剪 RMS < 150 的前导静音（保留 200ms 上下文）
2. Whisper prompt 添加常见词汇提示（金价、天气、健康）

## 踩坑
1. **Docker volume mount 需 recreate**: `restart` 不应用新 volume，需 `down && up -d`
2. **容器文件权限**: appuser(1001) 无法写 root 目录，需 `chmod 777`
3. **LVGL z-ordering**: 后创建对象在最前，需 `lv_obj_move_foreground()` 调整
4. **Face canvas 必须显式隐藏**: `SetVisible(false)` + `move_foreground`，光 z-order 不够
5. **volatile++ C++20 deprecated**: 用 `x = x + 1` 替代 `x++`
