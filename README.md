# HiDaisy ESP32 Device Firmware

> ESP32-S3 firmware for the HiDaisy / HiLily voice companion device.

**[中文文档](README.zh.md)** · **[Terminal project report](docs/HILILY_TERMINAL_PROJECT_REPORT.md)** · **[User manual](USER_MANUAL_EN.md)**

## Overview

This repository contains the device-side firmware for HiDaisy, an AI voice companion designed for older adults. The ESP32-S3 is responsible for real-time audio capture and playback, local wake-word detection, the display and physical controls, Wi-Fi provisioning, device activation, reminders, battery monitoring, and OTA updates. Speech recognition, dialogue reasoning, content services, and speech synthesis run on the server.

The current production path uses HTTP for registration and authentication, MQTT for JSON control messages, and encrypted UDP for low-latency Opus audio. The old WebSocket client remains in the source tree for reference but is not part of the current hardware build.

```text
INMP441 microphone
  -> I2S capture -> ESP-SR AEC + noise suppression
  -> local Daisy microWakeWord / Opus encoder
  -> AES-CTR UDP audio -> HiDaisy server
  -> ASR -> dialogue / tools -> TTS
  -> UDP Opus -> jitter queue -> I2S -> MAX98357A speaker

Control plane: device <-> MQTT <-> server
Bootstrap:     device -> HTTP registration/authentication/OTA
```

## Current Capabilities

- ESP32-S3 dual-core FreeRTOS application with PSRAM-backed audio and UI tasks.
- Always-on 16 kHz microphone pipeline with ESP-SR AEC and NSNet2 noise suppression.
- Local quantized TFLite Micro wake-word model for "Hi Daisy".
- 16 kHz mono Opus uplink and downlink using 60 ms frames.
- MQTT control channel plus AES-CTR encrypted UDP audio channel.
- Server-side Silero VAD support for spoken barge-in while TTS is playing.
- Button barge-in with an audio-frame fade-out before queued playback is discarded.
- Continuous conversation mode and segmented upload for long requests.
- ST7789/LVGL interface with state, transcript, waveform, Wi-Fi, time, battery, overlays, and animated expressions.
- Captive-portal Wi-Fi setup with up to three saved networks, roaming, and saved-password reuse.
- Device registration, activation-code onboarding, persistent device token, and dynamic MQTT credentials.
- Persistent reminders, local alarm tone, low-battery reporting, remote settings, and dual-partition OTA.
- QEMU mock implementations for device-side development without physical hardware.
- Optional React DAISY book-search test frontend under `frontend/`.

## Hardware and Software

| Item | Current configuration |
|---|---|
| MCU | ESP32-S3-N16R8, dual-core 240 MHz |
| Memory | 16 MB flash, 8 MB octal PSRAM |
| Board | `zhengchen-1.54tft-wifi` |
| Display | ST7789, 1.54 inch, 240 x 240, SPI at 80 MHz |
| Microphone | INMP441, I2S, 16 kHz mono |
| Amplifier | MAX98357A, I2S |
| USB serial | CH340K |
| Framework | ESP-IDF 5.3 or newer; last verified build metadata uses 5.5.3 |
| UI | LVGL 8.4 |
| Audio codec | `espressif/esp_audio_codec ~2.4.1` |
| Audio front end | `espressif/esp-sr ~2.3.0` |
| Wake inference | TFLite Micro + ESP-NN + micro-speech-features |
| Reported OTA firmware version | `1.2.0` |

### Pin Map

| Function | GPIO |
|---|---:|
| LCD MOSI / SCLK / CS / DC / RST / backlight | 41 / 42 / 21 / 40 / 45 / 20 |
| Speaker DOUT / BCLK / LRCK | 7 / 15 / 16 |
| Microphone SD / WS / SCK | 6 / 4 / 5 |
| BOOT / wake button | 0 |
| Volume up / volume down-audio test | 10 / 39 |
| Battery ADC | 1 |

## Runtime Architecture

### Startup and Connectivity

```text
Power on
  -> NVS, settings, OTA validation
  -> board, display, battery, audio, buttons, reminders
  -> Wi-Fi initialization before AFE to preserve internal RAM
  -> AEC/NS and Daisy wake-word initialization
  -> connect to a saved Wi-Fi network or start provisioning
  -> probe the HTTP server
  -> register/activate when no device token exists
  -> authenticate and receive MQTT credentials
  -> connect and subscribe to MQTT
  -> send hello(features.aec=true)
  -> receive UDP address, AES key/nonce, timezone, and audio parameters
  -> enter idle and enable local wake detection
```

The application does not enter `idle` until MQTT is connected. This prevents capture from starting before `listen` control messages can be delivered.

### Device States

The state machine contains `starting`, `wifi_connecting`, `idle`, `listening`, `thinking`, `speaking`, `reminding`, and `error`. The server drives dialogue state with `state_change`; the device applies audio and UI behavior for each transition.

Supported expressions are `neutral`, `happy`, `sad`, `comfort`, `surprise`, `thinking`, `listening`, and `reminding`. The server may provide an `intensity` from 0.0 to 1.0.

### Important Tasks

| Task | Core | Priority | Role |
|---|---:|---:|---|
| `audio_in` | 1 | 10 | Always-on microphone capture |
| `afe_proc` | 1 | 8 | AEC and noise suppression |
| `audio_out` | 1 | 10 | Audio decode and speaker output |
| `opus_enc` | 0 | 2 | 60 ms Opus encoding |
| `mww_daisy` | 0 | 2 | Local wake-word inference |
| `udp_rx` | 0 | 11 | UDP receive and decryption |
| `audio_wr` | 0 | 8 | Downlink queue forwarding |
| `msg_router` | 0 | 5 | MQTT JSON routing |
| `ui` | 0 | 5 | LVGL rendering and animation |

Large task stacks and audio queues are allocated from PSRAM to retain internal SRAM for Wi-Fi and real-time drivers.

## Audio Pipeline

### Uplink

```text
INMP441 32-bit I2S
  -> shift right by 12 and saturate to signed 16-bit PCM
  -> ESP-SR AFE in MR mode: microphone + software speaker reference
  -> AEC_MODE_VOIP_LOW_COST
  -> NSNet2, with WebRTC NS fallback
  -> 16 kHz mono Opus, 960 samples / 60 ms, VBR, DTX disabled
  -> UDP packet header -> AES-CTR payload encryption -> server
```

The microphone I2S channel remains enabled in idle so the wake-word path continues to receive audio. ESP-SR VAD and WakeNet are disabled in the current implementation. A bounded RMS detector provides local speech/silence timing for auto-stop, while the server uses Silero VAD for spoken barge-in.

### Wake Word

The embedded model is `main/models/daisy_stream_state_internal_quant.tflite`. It consumes AEC/NS-cleaned 16 kHz PCM through a short contiguous queue. Current deployment parameters are:

- Tensor input/output: `int8[1,3,40]` to `uint8[1,1]`.
- 30 ms feature window with a 10 ms step.
- Five-result sliding window.
- Probability cutoff: 0.97.
- Inference enabled only when the application is fully idle and the speaker has drained.

The supplied model has no authoritative microWakeWord manifest, so field FAR/FRR testing is still required before production tuning is considered complete.

### Downlink

```text
UDP packet
  -> sequence check -> AES-CTR decryption
  -> PSRAM-backed router and downstream queues
  -> five-frame startup watermark
  -> Opus decode or PCM path
  -> quadratic software volume
  -> 16-bit mono to 32-bit I2S
  -> MAX98357A speaker
```

`tts.end` starts a drain operation instead of immediately stopping playback. Each new `tts.start` resets the end flag so multi-segment responses are not cut off. During playback, volume-adjusted PCM is also written to the software AEC reference channel.

### Conversation and Barge-In

- A Daisy wake plays a local spoken acknowledgement, sends `listen.detect`, and enters automatic continuous mode.
- A short BOOT press in idle starts automatic continuous mode.
- After valid speech, 1.5 seconds of local RMS silence sends `listen.stop`.
- After TTS fully drains, the device waits 500 ms for speaker decay before listening again.
- Recordings longer than 30 seconds can emit `listen.segment` at a natural pause.
- Pressing BOOT during playback sends `abort`, fades the active frame, clears queued audio, and enters listening.
- While speaking, AEC-cleaned microphone audio continues upstream so the server can detect spoken barge-in.

## Network Protocol

### Current Endpoints

| Channel | Value |
|---|---|
| HTTP bootstrap/OTA | `http://4.197.40.220:8000` |
| MQTT | Returned by `/api/v1/device/auth`; current deployment expects port `1884` |
| UDP audio | Returned by MQTT `hello`; current deployment expects port `8765` |
| Local provisioning | `http://192.168.4.1` |

The ESP32 does not call Azure OpenAI, Azure Speech, or Translator directly. Those providers and credentials belong to the server deployment.

### HTTP Bootstrap

1. `POST /api/v1/device/register` with a lowercase `mac_address`.
2. `GET /api/v1/device/register/status?mac=...` until activation returns a `device_token`.
3. `POST /api/v1/device/auth` with `mac_address` and `device_token`.
4. `GET /api/v1/ota/check?device_id=...&current_version=1.2.0` after authentication and every hour.

Authentication returns the broker URI, client ID, username/password, publish and subscribe topics, and expiry metadata. MQTT topics are normally `device/{mac}/up` and `device/{mac}/down`.

### MQTT Messages

Device to server:

| Type | Purpose |
|---|---|
| `hello` | Negotiate capabilities and UDP transport |
| `listen` | `start`, `detect`, `segment`, and `stop` lifecycle |
| `abort` | Button barge-in |
| `goodbye` | End a dialogue |
| `client_pong` | Server heartbeat response |
| `reminder_triggered` | Local reminder fired |
| `low_battery` | Validated low-battery snapshot |

Server to device:

| Type | Purpose |
|---|---|
| `hello` | UDP session, audio parameters, and timezone |
| `state_change` | Dialogue state; this is the only current state-message name |
| `emotion_update` | Expression and intensity |
| `tts` | TTS `start` and `end` lifecycle |
| `audio_meta` | PCM/Opus format metadata |
| `stt` | Transcript text for the display |
| `server_ping` | Heartbeat request |
| `config` | Volume, brightness, and timezone |
| `ota` | Remote firmware update request |
| `reminder_created`, `reminder_cancelled`, `reminder_notify` | Reminder synchronization |
| `end_dialogue` | Disable continuous conversation |
| `error` | Server-side error information |

### UDP Packet

```text
| type 1 B | flags 1 B | payload_len 2 B | ssrc 4 B |
| timestamp 4 B | sequence 4 B | encrypted Opus payload |
```

The 16-byte network-order header also forms the per-packet AES-CTR counter input. Sequence numbers provide a basic old-packet check. AES-CTR does not authenticate the payload; see Known Limitations.

## Wi-Fi Provisioning

The device stores up to three SSID/password pairs in NVS.

- Saved networks are always attempted before manual setup.
- Initial connection uses two fast retries per network; all saved networks must fail before provisioning starts.
- Runtime reconnection uses longer exponential backoff.
- RSSI is checked every 10 seconds. Three readings below -75 dBm trigger a scan; roaming requires an alternative at least 10 dB stronger.
- Provisioning creates the open AP `HiLily-XXXX` and a captive portal at `192.168.4.1`.
- The portal does not require a PIN.
- Selecting an existing SSID with an empty password reuses the stored password.
- A new password is needed only when the saved password is invalid or the user explicitly changes it.
- Long-pressing BOOT while already connected shows the device IP and does not reopen provisioning.
- Long-pressing while disconnected first retries saved networks and opens setup only if they still fail.

First-time setup:

1. Connect a phone to `HiLily-XXXX`.
2. Use the captive-portal popup or open `http://192.168.4.1`.
3. Scan for a network or select a saved entry.
4. Save the credentials and restart when prompted.

## Display, Battery, Reminders, and OTA

- LVGL renders the animated face, listening waveform, transcript, clock, Wi-Fi status, five-cell battery indicator, and full-screen status overlays.
- Battery readings outside 2500-5000 mV are rejected. The monitor discards five warm-up samples, takes a five-sample median, and requires three low readings before reporting a low battery.
- Battery percentage currently maps 3000 mV to 0% and 4200 mV to 100%; the final divider and battery curve still require board-level calibration.
- Up to ten reminders are persisted in NVS and scanned once per minute. Due reminders play a local alarm and notify the server.
- OTA checks run after authentication and every hour. Images are written to the inactive 3 MB application partition with rollback support.

## Repository Layout

```text
hiDaisy-esp32/
├── main/
│   ├── application.*              application lifecycle and event loop
│   ├── audio_service.*            I2S, Opus, buffering, playback, AEC reference
│   ├── afe_processor.*            ESP-SR AEC and noise suppression
│   ├── micro_wake_word.*          Daisy TFLite Micro inference
│   ├── hilily_mqtt_client.*       HTTP onboarding/auth and MQTT client
│   ├── udp_client.*               encrypted UDP audio transport
│   ├── msg_router.*               MQTT JSON and UDP audio routing
│   ├── wifi_manager.*             saved networks, roaming, captive portal, NTP
│   ├── display_manager.*          LVGL display and overlays
│   ├── face_renderer.*            parametric animated expressions
│   ├── battery.*                  ADC filtering and low-battery snapshots
│   ├── reminder_manager.*         persistent reminder scheduling
│   ├── ota_manager.*              dual-partition OTA
│   ├── qemu_mock/                 hardware/network mock implementations
│   ├── models/                    embedded Daisy wake model
│   └── assets/                    embedded local audio assets
├── docs/                           protocol, architecture, OTA, and project reports
├── notes/                          engineering investigations and resolved issues
├── tests/                          source-level firmware checks
├── scripts/qemu_run.sh             isolated QEMU build and runner
├── frontend/                       DAISY book-search test interface
├── partitions.csv                  16 MB flash layout
├── sdkconfig.defaults              hardware defaults
├── sdkconfig.defaults.qemu         QEMU defaults
├── flash.sh                        manual-reset-safe flashing helper
└── monitor.sh                      reconnecting serial monitor and logger
```

## Build

### Prerequisites

- ESP-IDF 5.3 or newer; 5.5.3 is the currently recorded build environment.
- An ESP-IDF Python environment with managed-component downloads enabled.
- Git and CMake/Ninja as installed by ESP-IDF.

### Hardware Build

```bash
cd hiDaisy-esp32
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
```

The generated application binary is `build/hilily-esp32.bin`. The firmware embeds the Daisy TFLite model and the local wake acknowledgement audio through `main/CMakeLists.txt`.

### QEMU Build

```bash
./scripts/qemu_run.sh build
./scripts/qemu_run.sh run
```

Use `graphics` instead of `run` for the virtual framebuffer. QEMU uses mock board, Wi-Fi, battery, audio, display, MQTT, and UDP implementations; it does not reproduce physical AEC, RF, ADC, or acoustic behavior.

## Flash and Monitor

This board cannot reliably enter or leave download mode through DTR/RTS. Enter download mode manually:

1. Hold BOOT.
2. Press and release RST.
3. Release BOOT.
4. Start flashing with `--before no_reset --after no_reset`.
5. Press RST manually after flashing.

macOS helper:

```bash
./flash.sh /dev/cu.wchusbserial1130
./monitor.sh /dev/tty.wchusbserial1130 0
```

`flash.sh` currently contains a developer-specific ESP-IDF path; update its `IDF_PATH` before using it on another machine.

Windows ESP-IDF terminal example:

```powershell
Set-Location build
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before no_reset --after no_reset write_flash '@flash_args'
Set-Location ..
```

Close other serial monitors before opening COM3. If the ROM prints `DOWNLOAD(USB/UART0) waiting for download`, release BOOT and press RST after a successful flash.

## DAISY Book Search Test Frontend

The optional React/Vite interface calls `GET /api/books/search`, displays DAISY book cards, and tests browser voice commands and audio responses.

```bash
cd frontend
npm install
npm run dev
```

See [frontend/README.md](frontend/README.md) for the proxy and response schema.

## Documentation

- [Terminal project report](docs/HILILY_TERMINAL_PROJECT_REPORT.md): current architecture, integration contract, risk register, and acceptance checklist.
- [Device/server API specification](docs/API_SPECIFICATION.md): historical shared API document; parts still describe the legacy WebSocket path.
- [ESP32 architecture PRD](docs/PRD_ESP32_Architecture.md): product and target architecture.
- [OTA administrator guide](docs/OTA_ADMIN_GUIDE.md): server-side firmware release workflow.
- [English user manual](USER_MANUAL_EN.md): end-user operation and safety guidance.
- [Engineering notes](notes/): issue investigations and implementation decisions.

## Known Limitations

- The configured public HTTP endpoint and the expected MQTT deployment currently use plaintext `http://` and `mqtt://`. Production deployment should use a stable domain, HTTPS, MQTTS, and certificate validation.
- UDP audio uses AES-CTR without an authentication tag. Production hardening should use an AEAD construction or add a MAC with replay-safe associated data.
- OTA currently does not verify the response MD5 or a signed firmware manifest. TLS, SHA-256/signature verification, and Secure Boot should be evaluated before production release.
- `CMakeLists.txt` reports project version `1.0.0`, while `OtaManager` reports firmware `1.2.0`; these should be generated from one version source.
- MQTT credential `expires_in` is parsed but not proactively refreshed.
- The wake-word model was supplied without its authoritative feature manifest and still needs enclosure-specific FAR/FRR validation.
- Battery ADC wiring, divider ratio, and percentage curve require final hardware calibration.
- `tests/test_firmware.py` still contains assertions for parts of the legacy WebSocket implementation and is not a complete validation signal for the current MQTT/UDP firmware.

## Server Integration Checklist

For end-to-end testing, validate these layers in order:

1. Wi-Fi, DHCP, NTP, and HTTP reachability.
2. Registration, activation, token persistence, and authentication.
3. MQTT connection, topics, `hello`, heartbeat, and state messages.
4. Publicly reachable UDP address, keepalive, AES parameters, and Opus decode.
5. Multi-segment TTS playback and `tts.start`/`tts.end` ordering.
6. Button and spoken barge-in.
7. Long-recording segment assembly.
8. Reminder synchronization and local trigger.
9. OTA success, interrupted download, invalid image, and rollback.
10. Wi-Fi, MQTT, and UDP disconnect/recovery without duplicate tasks or resource leaks.

The server and firmware must agree on the exact JSON schemas, 16-byte UDP header, network byte order, 16 kHz mono audio format, and 60 ms Opus framing.
