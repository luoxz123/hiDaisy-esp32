# 终端项目报告与接口契约审计

## 现象

需要生成一份能够与服务端项目报告合并的终端报告。现有资料分散在 README、API 文档、源代码和多轮调试笔记中，而且部分早期文档仍描述 WebSocket、WakeNet9 和本地 VAD，与当前 MQTT + UDP、microWakeWord、AEC/NS 和服务端 VAD 实现不一致。

## 排查过程

- 检查 `AGENTS.md` 和 `notes/` 中网络、音频、Wi-Fi、唤醒、电池、OTA 和烧录记录。
- 读取当前参与构建的 `main/CMakeLists.txt`，确认 `ws_client.cc` 没有进入现行硬件构建。
- 对照 `application.cc`、`hilily_mqtt_client.cc`、`udp_client.cc` 和 `msg_router.cc` 整理真实 HTTP、MQTT 和 UDP 契约。
- 核对 AFE、Opus、microWakeWord、FreeRTOS 任务、分区和 NVS 数据。
- 检查最近构建元数据和二进制大小。
- 检查 `tests/test_firmware.py`，发现其中仍断言旧 WebSocket 和旧状态消息；当前环境也没有可直接调用的 `python`/`py`。

## 根因

项目在多次架构演进中从 WebSocket 转向 MQTT + UDP，从 WakeNet9/本地 VAD 转向 microWakeWord + AEC/NS + 服务端 VAD，但协议文档、README 和静态测试没有同步成为单一事实来源。终端和服务端报告若直接拼接，容易对 endpoint、状态消息、音频格式和安全边界产生错误理解。

## 解决方案

- 新增 `docs/HILILY_TERMINAL_PROJECT_REPORT.md`。
- 报告区分终端职责、服务端职责和服务端内部 Azure endpoint。
- 按当前代码记录 HTTP、MQTT、UDP、音频、状态机、Wi-Fi、提醒、OTA 和持久化契约。
- 增加完成度、P0/P1/P2 风险、合并步骤、端到端联调顺序和可测量验收标准。
- 明确标记 API 文档、README、测试、版本来源和传输安全等技术债。

## 教训

- 架构迁移后必须同时更新代码、协议文档、测试和部署说明，不能只更新 README 示例。
- 完整项目报告应将“当前代码事实”“目标架构”和“历史兼容代码”分开。
- 终端与服务端合并前，应先冻结机器可验证的 schema 和音频 fixture，再进行业务层联调。
- 构建产物存在不等于当前提交已验证；报告必须明确构建时间、验证边界和未执行项。
