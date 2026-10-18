#include "msg_router.h"
#include "board.h"
#include "hilily_mqtt_client.h"
#include "ota_manager.h"
#include "reminder_manager.h"
#include "settings.h"
#include "udp_client.h"

#include <cJSON.h>
#include <cstdio>
#include <cstdlib>
#include <freertos/idf_additions.h>
#include <cstring>
#include <ctime>
#include <esp_log.h>

static const char *TAG = "msg_router";

// Apply timezone offset (minutes from UTC) as POSIX TZ variable
static void ApplyTimezone(int offset_minutes) {
  int hours = offset_minutes / 60;
  int mins = abs(offset_minutes % 60);
  // POSIX TZ: sign is inverted (UTC-8 means +8 ahead of UTC)
  char tz[32];
  if (mins == 0) {
    snprintf(tz, sizeof(tz), "UTC%+d", -hours);
  } else {
    snprintf(tz, sizeof(tz), "UTC%+d:%02d", -hours, mins);
  }
  setenv("TZ", tz, 1);
  tzset();
  ESP_LOGI(TAG, "Timezone set: %s (offset %+d min)", tz, offset_minutes);
}

MsgRouter &MsgRouter::GetInstance() {
  static MsgRouter instance;
  return instance;
}

esp_err_t MsgRouter::Init(StateMachine *state_machine,
                          QueueHandle_t audio_queue) {
  state_machine_ = state_machine;
  audio_queue_ = audio_queue;

  json_queue_ = xQueueCreate(16, sizeof(char *));
  if (!json_queue_) {
    ESP_LOGE(TAG, "Failed to create JSON queue");
    return ESP_FAIL;
  }

  // Audio write queue in PSRAM: holds chunk descriptors (ptr+len)
  // 2048 items to match downstream capacity for burst absorption
  audio_write_queue_ = xQueueCreateWithCaps(2048, sizeof(AudioChunk), MALLOC_CAP_SPIRAM);
  if (!audio_write_queue_) {
    ESP_LOGW(TAG, "PSRAM audio_write_queue failed, fallback to internal");
    audio_write_queue_ = xQueueCreate(2048, sizeof(AudioChunk));
  }
  if (!audio_write_queue_) {
    ESP_LOGE(TAG, "Failed to create audio write queue");
    return ESP_FAIL;
  }

  // 注册 MQTT JSON 消息回调 (device/{mac}/down)
  HililyMqttClient::GetInstance().OnJson([this](const char *json_str, int len) {
    char *copy = (char *)malloc(len + 1);
    if (copy) {
      memcpy(copy, json_str, len);
      copy[len] = 0;
      if (xQueueSend(json_queue_, &copy, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "JSON queue full, dropping message");
        free(copy);
      }
    }
  });

  // 注册 UDP 下行音频回调 (解密后的 Opus 帧)
  UdpClient::GetInstance().OnAudio(
      [this](const uint8_t *data, int len) { HandleBinaryMessage(data, len); });

  ESP_LOGI(TAG, "Message router initialized");
  return ESP_OK;
}

void MsgRouter::Start() {
  xTaskCreatePinnedToCore(RouterTask, "msg_router", 4096, this, 5, NULL, 0);
  xTaskCreatePinnedToCore(AudioWriterTask, "audio_wr", 4096, this, 8, NULL, 0);
}

void MsgRouter::FlushAudioQueue() {
  if (!audio_write_queue_)
    return;
  AudioChunk chunk;
  int flushed = 0;
  while (xQueueReceive(audio_write_queue_, &chunk, 0) == pdTRUE) {
    heap_caps_free(chunk.data);
    flushed++;
  }
  if (flushed > 0) {
    ESP_LOGI(TAG, "Flushed %d pending audio chunks", flushed);
  }
}

void MsgRouter::RouterTask(void *arg) {
  auto *self = static_cast<MsgRouter *>(arg);
  char *json_str;

  ESP_LOGI(TAG, "Message router task started");

  while (true) {
    if (xQueueReceive(self->json_queue_, &json_str, portMAX_DELAY) == pdTRUE) {
      self->HandleJsonMessage(json_str, strlen(json_str));
      free(json_str);
    }
  }
}

void MsgRouter::HandleJsonMessage(const char *json_str, int len) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGW(TAG, "JSON parse failed");
    return;
  }

  const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
  if (!type) {
    ESP_LOGW(TAG, "JSON missing 'type' field");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "Received: type=%s", type);

  // #94: Only handle "state_change" (removed legacy "status_update"/"status"
  // fallback)
  if (strcmp(type, "state_change") == 0) {
    const char *state =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
    if (state && state_machine_) {
      DeviceState new_state = device_state_from_string(state);
      if (new_state != kDeviceStateUnknown) {
        state_machine_->TransitionTo(new_state);
      } else {
        ESP_LOGW(TAG, "Unknown state: %s", state);
      }
    }
  } else if (strcmp(type, "emotion_update") == 0) {
    const char *emotion =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "emotion"));
    if (emotion && state_machine_) {
      cJSON *intensity_item = cJSON_GetObjectItem(root, "intensity");
      float intensity = 1.0f;
      if (intensity_item && cJSON_IsNumber(intensity_item)) {
        intensity = (float)intensity_item->valuedouble;
        if (intensity < 0.0f)
          intensity = 0.0f;
        if (intensity > 1.0f)
          intensity = 1.0f;
      }
      state_machine_->SetEmotion(emotion_from_string(emotion), intensity);
    }
  } else if (strcmp(type, "tts") == 0) {
    // #105: Handle tts type with state subfield (start/end)
    const char *tts_state =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
    if (tts_state && strcmp(tts_state, "end") == 0) {
      const char *reply_text =
          cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
      if (reply_text) {
        ESP_LOGI(TAG, "TTS end, reply: %.80s%s", reply_text,
                 strlen(reply_text) > 80 ? "..." : "");
      } else {
        ESP_LOGI(TAG, "TTS end received, signaling audio drain");
      }
      if (tts_end_callback_) {
        tts_end_callback_();
      }
    } else if (tts_state && strcmp(tts_state, "start") == 0) {
      ESP_LOGI(TAG, "TTS start (UDP sock=%d, aes=%d, rx_loops=%d)",
               UdpClient::GetInstance().GetSock(),
               UdpClient::GetInstance().IsAesReady(),
               UdpClient::GetInstance().GetRxLoopCount());
      // Reset tts_end flag to cancel any pending drain from previous segment
      if (tts_start_callback_) {
        tts_start_callback_();
      }
      // Fallback: ensure receive task is running
      if (UdpClient::GetInstance().IsConnected() &&
          UdpClient::GetInstance().GetRxLoopCount() == 0) {
        ESP_LOGW(TAG, "UDP rx task not running! Starting it now...");
        UdpClient::GetInstance().StartReceiveTask();
      }
    } else {
      ESP_LOGW(TAG, "tts message with unknown state: %s",
               tts_state ? tts_state : "(null)");
    }
  } else if (strcmp(type, "server_ping") == 0) {
    // #92: Respond to server heartbeat with client_pong via MQTT
    ESP_LOGD(TAG, "Server ping received, sending client_pong");
    cJSON *pong = cJSON_CreateObject();
    cJSON_AddStringToObject(pong, "type", "client_pong");
    char *pong_str = cJSON_PrintUnformatted(pong);
    HililyMqttClient::GetInstance().PublishJson(pong_str);
    cJSON_free(pong_str);
    cJSON_Delete(pong);
  } else if (strcmp(type, "hello") == 0) {
    // Server hello reply: parse UDP address + AES keys, connect UDP socket
    ESP_LOGI(TAG, "Server hello received, setting up UDP transport");
    cJSON *udp_obj = cJSON_GetObjectItem(root, "udp");
    if (udp_obj) {
      const char *server_ip =
          cJSON_GetStringValue(cJSON_GetObjectItem(udp_obj, "server"));
      cJSON *port_item = cJSON_GetObjectItem(udp_obj, "port");
      const char *key_hex =
          cJSON_GetStringValue(cJSON_GetObjectItem(udp_obj, "key"));
      const char *nonce_hex =
          cJSON_GetStringValue(cJSON_GetObjectItem(udp_obj, "nonce"));
      if (server_ip && port_item && key_hex && nonce_hex) {
        uint16_t port = (uint16_t)port_item->valueint;
        esp_err_t ret = UdpClient::GetInstance().Connect(
            std::string(server_ip), port, std::string(key_hex),
            std::string(nonce_hex));
        if (ret == ESP_OK) {
          UdpClient::GetInstance().StartReceiveTask();
          // Send keepalive to immediately register client_addr on server.
          // On first connect StartReceiveTask sends one, but on reconnect
          // the task is already running and skips the initial keepalive.
          static const uint8_t keepalive = 0;
          UdpClient::GetInstance().Send(&keepalive, 1, 0);
          ESP_LOGI(TAG, "UDP transport established: %s:%u (keepalive sent)",
                   server_ip, port);
        } else {
          ESP_LOGE(TAG, "UDP connect failed!");
        }
      } else {
        ESP_LOGW(TAG, "hello.udp missing required fields");
      }
    } else {
      ESP_LOGW(TAG, "hello reply has no 'udp' field");
    }
    // Parse timezone_offset from hello response
    cJSON *tz_offset = cJSON_GetObjectItem(root, "timezone_offset");
    if (tz_offset && cJSON_IsNumber(tz_offset)) {
      int offset = tz_offset->valueint;
      Settings::GetInstance().SetTimezoneOffset(offset);
      ApplyTimezone(offset);
    }
    // Parse audio_params.format from hello response to set downstream format
    cJSON *audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (audio_params) {
      const char *fmt =
          cJSON_GetStringValue(cJSON_GetObjectItem(audio_params, "format"));
      if (fmt) {
        AudioMeta meta = {};
        strncpy(meta.format, fmt, sizeof(meta.format) - 1);
        cJSON *sr = cJSON_GetObjectItem(audio_params, "sample_rate");
        cJSON *ch = cJSON_GetObjectItem(audio_params, "channels");
        if (sr)
          meta.sample_rate = sr->valueint;
        if (ch)
          meta.channels = ch->valueint;
        ESP_LOGI(TAG, "Hello audio params: format=%s", fmt);
        if (audio_meta_callback_) {
          audio_meta_callback_(meta);
        }
      }
    }
  } else if (strcmp(type, "error") == 0) {
    // #93: Log server error messages
    cJSON *code_item = cJSON_GetObjectItem(root, "code");
    const char *message =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
    int code = code_item ? code_item->valueint : 0;
    ESP_LOGW(TAG, "Server error: code=%d, message=%s", code,
             message ? message : "(null)");
  } else if (strcmp(type, "audio_meta") == 0) {
    AudioMeta meta = {};
    cJSON *format = cJSON_GetObjectItem(root, "format");
    cJSON *sr = cJSON_GetObjectItem(root, "sample_rate");
    cJSON *ch = cJSON_GetObjectItem(root, "channels");
    cJSON *bd = cJSON_GetObjectItem(root, "bit_depth");

    if (format)
      strncpy(meta.format, cJSON_GetStringValue(format),
              sizeof(meta.format) - 1);
    if (sr)
      meta.sample_rate = sr->valueint;
    if (ch)
      meta.channels = ch->valueint;
    if (bd)
      meta.bit_depth = bd->valueint;

    ESP_LOGI(TAG, "Audio meta: format=%s, sr=%d, ch=%d, bd=%d", meta.format,
             meta.sample_rate, meta.channels, meta.bit_depth);

    if (audio_meta_callback_) {
      audio_meta_callback_(meta);
    }
  } else if (strcmp(type, "stt") == 0) {
    // #107: Handle speech-to-text recognition results
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    ESP_LOGI(TAG, "STT result: %s", text ? text : "(null)");
    if (text && stt_result_callback_) {
      stt_result_callback_(text);
    }
  } else if (strcmp(type, "config") == 0) {
    // #113: Handle config messages (volume, brightness, timezone)
    cJSON *vol = cJSON_GetObjectItem(root, "volume");
    if (vol && cJSON_IsNumber(vol)) {
      Settings::GetInstance().SetVolume(vol->valueint);
    }
    cJSON *bright = cJSON_GetObjectItem(root, "brightness");
    if (bright && cJSON_IsNumber(bright)) {
      Settings::GetInstance().SetBrightness(bright->valueint);
      board_set_brightness(bright->valueint);
    }
    cJSON *tz = cJSON_GetObjectItem(root, "timezone_offset");
    if (tz && cJSON_IsNumber(tz)) {
      Settings::GetInstance().SetTimezoneOffset(tz->valueint);
      ApplyTimezone(tz->valueint);
    }
  } else if (strcmp(type, "ota") == 0) {
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    const char *version =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    if (url) {
      ESP_LOGI(TAG, "OTA update requested: version=%s url=%s",
               version ? version : "unknown", url);
      std::string ota_url(url);
      std::string ota_ver(version ? version : "");
      struct OtaArgs { std::string url; std::string ver; };
      auto *args = new OtaArgs{ota_url, ota_ver};
      xTaskCreate(
          [](void *arg) {
            auto *a = static_cast<OtaArgs *>(arg);
            OtaManager::GetInstance().StartUpdate(a->url, a->ver);
            delete a;
            vTaskDelete(NULL);
          },
          "ota_task", 8192, args, 5, NULL);
    }
  } else if (strcmp(type, "reminder_created") == 0) {
    const char *reminder_id =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "reminder_id"));
    cJSON *time_item = cJSON_GetObjectItem(root, "time");
    const char *content =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    if (reminder_id && time_item && cJSON_IsNumber(time_item)) {
      int64_t ts = (int64_t)time_item->valuedouble;
      esp_err_t ret = ReminderManager::GetInstance().AddReminder(
          reminder_id, ts, content ? content : "");
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Reminder created: %s at %lld content='%s'",
                 reminder_id, ts, content ? content : "");
      } else {
        ESP_LOGW(TAG, "Failed to add reminder: %s (err=%d)", reminder_id, ret);
      }
    } else {
      ESP_LOGW(TAG, "reminder_created missing required fields");
    }
  } else if (strcmp(type, "reminder_cancelled") == 0) {
    const char *reminder_id =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "reminder_id"));
    if (reminder_id) {
      esp_err_t ret = ReminderManager::GetInstance().CancelReminder(reminder_id);
      ESP_LOGI(TAG, "Reminder cancel %s: %s",
               ret == ESP_OK ? "OK" : "not found", reminder_id);
    }
  } else if (strcmp(type, "reminder_notify") == 0) {
    // Server-initiated reminder notification (backup channel)
    const char *reminder_id =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "reminder_id"));
    ESP_LOGI(TAG, "Server reminder notify: %s",
             reminder_id ? reminder_id : "(null)");
    if (state_machine_) {
      state_machine_->TransitionTo(kDeviceStateReminding);
    }
  } else if (strcmp(type, "end_dialogue") == 0) {
    const char *reason =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "reason"));
    ESP_LOGI(TAG, "End dialogue: reason=%s", reason ? reason : "(none)");
    if (end_dialogue_callback_) {
      end_dialogue_callback_(reason);
    }
  } else {
    ESP_LOGD(TAG, "Unhandled message type: %s", type);
  }

  cJSON_Delete(root);
}

void MsgRouter::AudioWriterTask(void *arg) {
  auto *self = static_cast<MsgRouter *>(arg);
  AudioChunk chunk;

  ESP_LOGI(TAG, "Audio writer task started");

  while (true) {
    if (xQueueReceive(self->audio_write_queue_, &chunk, portMAX_DELAY) ==
        pdTRUE) {
      // Forward chunk to AudioService downstream queue.
      // Ownership transfers to PlaybackTask which will free chunk.data.
      static int fwd_count = 0;
      fwd_count++;
      if (fwd_count <= 3 || fwd_count % 20 == 0) {
        UBaseType_t waiting = uxQueueMessagesWaiting(self->audio_queue_);
        ESP_LOGI(TAG, "Audio fwd #%d, %d bytes, downstream_q=%lu",
                 fwd_count, chunk.len, (unsigned long)waiting);
      }
      if (xQueueSend(self->audio_queue_, &chunk, pdMS_TO_TICKS(5000)) !=
          pdTRUE) {
        ESP_LOGW(TAG, "Downstream queue full 5s, dropping %d bytes", chunk.len);
        heap_caps_free(chunk.data);
      }
    }
  }
}

void MsgRouter::HandleBinaryMessage(const uint8_t *data, int len) {
  if (!audio_write_queue_ || len == 0)
    return;

  static int rx_count = 0;
  rx_count++;
  if (rx_count <= 3 || rx_count % 20 == 0) {
    ESP_LOGI(TAG, "UDP audio rx #%d, %d bytes", rx_count, len);
  }

  // Allocate chunk in PSRAM and enqueue (non-blocking to keep WS event loop
  // responsive)
  AudioChunk chunk;
  chunk.data =
      (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!chunk.data) {
    chunk.data = (uint8_t *)malloc(len);
  }
  if (!chunk.data) {
    ESP_LOGW(TAG, "Audio alloc failed, dropping %d bytes", len);
    return;
  }
  memcpy(chunk.data, data, len);
  chunk.len = len;

  if (xQueueSend(audio_write_queue_, &chunk, pdMS_TO_TICKS(2000)) != pdTRUE) {
    // 2s block provides TCP backpressure to server.
    // If still full after 2s, something is wrong (PlaybackTask stuck?)
    ESP_LOGW(TAG, "Audio queue full after 2s backpressure, dropping %d bytes",
             len);
    heap_caps_free(chunk.data);
  }
}
