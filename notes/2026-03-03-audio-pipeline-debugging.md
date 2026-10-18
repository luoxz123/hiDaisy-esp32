# 音频管道联调踩坑

> 日期: 2026-03-03 | 状态: 已修复

## 坑1: Docker DNAT 丢失 ESP32 TCP 响应
- **现象**: ESP32 HTTP POST 到 Docker 映射端口，connect 成功但 recv 0 字节
- **根因**: Docker iptables DNAT 在特定场景下丢弃对 ESP32 WiFi STA 的 TCP 响应包
- **解决**: 用 socat 绕过 Docker DNAT:
  ```bash
  socat TCP-LISTEN:8801,fork,reuseaddr TCP:172.31.0.3:8000
  socat TCP-LISTEN:1885,fork,reuseaddr TCP:172.31.0.2:1884
  ```
- **教训**: 调试先用宿主机直连排除 Docker 层；考虑 `network_mode: host`

## 坑2: FreeRTOS pvPortMalloc 不使用 SPIRAM
- **根因**: `pvPortMalloc()` 硬编码 `MALLOC_CAP_INTERNAL`，与 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 无关
- **解决**: 大 stack 任务必须用 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`
- **已迁移到 PSRAM**: OpusEncodeTask(24KB)、PlaybackTask(16KB)、UiTask(16KB)

## 坑3: 服务端 Opus 编码丢帧 → 播放 1.67x 加速
- **根因**: TTS chunk 3200B ÷ Opus 帧 1920B，余量 1280B 被丢弃（丢弃率 40%）
- **解决**: `_send_audio_chunk()` 维护 `_pcm_remainder`，结束时 flush（补零凑满最后一帧）
- **教训**: 音频"太快"不只看播放端，编码端数据丢失同样导致加速

## 坑4: Docker Compose restart 不应用代码/配置变更
- **根因**: `restart` 仅重启容器，**不会重新加载 `.env` 环境变量**，也不重建镜像
- **解决**: 配置变更后必须用 `docker compose up -d` 重建容器；镜像变更需先 `build`
- **验证**: `docker exec <container> python3 -c 'from src.core.config import settings; print(settings.XXX)'` 确认新值已生效

## 坑5: ESP32 DNS 解析公网域名失败
- **根因**: DHCP handler 在 DNS 设置后 ~30ms 覆盖自定义 DNS；ESP32 lwIP DNS 行为与桌面不同
- **解决**: 开发环境直接用 IP 地址，用 NVS 存储 IP

## 坑6: esp_http_client 在非标网络环境不可靠
- **解决**: 关键路径（如鉴权）改用 raw socket，HTTP/1.0 手写 POST 不超过 20 行

## 坑7: I2S Mono 模式在 ESP32-S3 上不稳定
- **解决**: 改为 `I2S_SLOT_MODE_STEREO` + 软件 MonoToStereo 转换，MAX98357A 取其中一声道

## 坑8: Opus 浮点编码器触发 FreeRTOS WDT
- **根因**: Opus SILK 浮点运算在 PSRAM stack 下 CPU 1 被 100% 占用，IDLE 任务无法运行
- **缓解**: 降低编码复杂度 `OPUS_SET_COMPLEXITY(5)` 或使用定点版本

## 坑9: 征辰板不支持自动进入下载模式
- **根因**: DTR 未连 GPIO0，`--before default_reset` 无效
- **解决**: 手动 BOOT+RST；esptool 参数 `-b 460800 --before no_reset`（不加 `--after hard_reset`）

## 坑10: 服务端返回局域网 IP，外网 ESP32 无法连接
- **根因**: `.env` 中 `UDP_PUBLIC_IP` 和 `MQTT_BROKER_URL` 硬编码了局域网 IP
- **解决**:
  1. 查外网 IP: `curl -s https://api.ipify.org`
  2. 更新 `.env`: `UDP_PUBLIC_IP=<公网IP>`、`MQTT_BROKER_URL=mqtt://<公网IP>:1885`
  3. **`docker compose up -d`** 重建容器（restart 不生效！）
  4. 在路由器配置端口转发: 1885/TCP, 8765/UDP, 8800/TCP → 192.168.31.205
- **教训**: 公网 IP 可能动态变化，长期方案应改用 DDNS 域名
- **当前配置**: 外网 IP `203.94.32.77`（2026-03-04 验证）
