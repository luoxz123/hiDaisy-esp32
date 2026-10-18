#include "application.h"
#include "audio_service.h"
#include "audio_test.h"
#include "battery.h"
#include "board.h"
#include "button_manager.h"

#include <sdkconfig.h>
#include "display_manager.h"
#include "hilily_mqtt_client.h"
#include "msg_router.h"
#include "ota_manager.h"
#include "pin_config.h"
#include "reminder_manager.h"
#include "settings.h"
#include "afe_processor.h"
#include "micro_wake_word.h"
#include "wifi_manager.h"

#include <cJSON.h>
#include <cstdlib>
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <sys/select.h>

static const char *TAG = "app";
// AUTO mode: VAD silence triggers stop after speech detected
static constexpr int64_t kVadSilenceTimeoutMs = 1500;   // 1.5s silence after speech → auto stop
static constexpr int64_t kAutoHardTimeoutMs = 600000;    // 10 min hard limit for auto mode
static constexpr int64_t kVadMinSpeechMs = 500;           // Require 500ms real speech before arming auto-stop
// MANUAL mode: only second button press stops (safety limit)
static constexpr int64_t kManualHardTimeoutMs = 1800000; // 30 min safety limit
// Segmented upload (Issue #153)
static constexpr int64_t kSegmentTimeoutMs = 30000;      // 30s per segment
static constexpr int64_t kSegmentPauseMs = 1000;          // 1s pause to trigger segment

// Server auth/OTA endpoint (Azure VM, parsed by HililyMqttClient::Authenticate)
static constexpr const char *kServerHost = "4.197.40.220";
static constexpr int kServerHttpPort = 8000;
static const char *SERVER_URL = "http://4.197.40.220:8000";
static const char *DEVICE_SECRET = "hilily-secret-003";
static constexpr int kServerRetryDelayMs = 30000;

Application &Application::GetInstance() {
  static Application instance;
  return instance;
}

Application::Application() {
  event_group_ = xEventGroupCreate();
  assert(event_group_ != NULL);
}

Application::~Application() {
  if (server_retry_timer_) {
    esp_timer_stop(server_retry_timer_);
    esp_timer_delete(server_retry_timer_);
    server_retry_timer_ = nullptr;
  }
  if (event_group_) {
    vEventGroupDelete(event_group_);
  }
}

void Application::Start() {
  ESP_LOGI(TAG, "HiLily Application starting...");

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Initialize settings (NVS-backed)
  Settings::GetInstance().Init();

  // Apply saved timezone
  {
    int tz_offset = Settings::GetInstance().GetTimezoneOffset();
    int hours = tz_offset / 60;
    int mins = abs(tz_offset % 60);
    char tz[32];
    if (mins == 0) {
        snprintf(tz, sizeof(tz), "UTC%+d", -hours);
    } else {
        snprintf(tz, sizeof(tz), "UTC%+d:%02d", -hours, mins);
    }
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone applied: %s", tz);
  }

  // Mark OTA firmware as valid (prevents rollback)
  OtaManager::GetInstance().MarkValid();

  // Initialize board hardware
  ESP_ERROR_CHECK(board_init());

  // Initialize display
  DisplayManager::GetInstance().Init();
  DisplayManager::GetInstance().Start();

  // Show boot animation while initializing subsystems
  DisplayManager::GetInstance().ShowBootAnimation();

  // Initialize battery monitoring
  Battery::GetInstance().Init(PIN_BATTERY_ADC);
  Battery::GetInstance().StartMonitoring();

  // Initialize audio service
  extern i2s_chan_handle_t board_get_spk_handle(void);
  extern i2s_chan_handle_t board_get_mic_handle(void);
  esp_err_t audio_ret = AudioService::GetInstance().Init(
      board_get_spk_handle(), board_get_mic_handle());
  if (audio_ret == ESP_OK) {
    AudioService::GetInstance().Start();
  } else {
    ESP_LOGE(TAG, "Audio init failed, tasks not started");
  }

  // Initialize button
  ButtonManager::GetInstance().Init(PIN_BUTTON_BOOT);
  ButtonManager::GetInstance().OnPress(
      [this]() { NotifyEvent(EVT_BUTTON_PRESSED); });
  ButtonManager::GetInstance().OnLongPress(
      [this]() { NotifyEvent(EVT_BUTTON_LONG_PRESSED); });

  // Initialize audio recording test (VOL_DOWN button)
  AudioTest::GetInstance().Init(board_get_mic_handle(), board_get_spk_handle(),
                                PIN_BUTTON_VOL_DOWN);
  AudioTest::GetInstance().Start();

  // Initialize reminder manager
  ReminderManager::GetInstance().Init();
  ReminderManager::GetInstance().OnTrigger(
      [this](const std::string &reminder_id) {
        ESP_LOGI(TAG, "Reminder triggered: %s", reminder_id.c_str());
        triggered_reminder_id_ = reminder_id;
        NotifyEvent(EVT_REMINDER_TRIGGERED);
      });
  ReminderManager::GetInstance().Start();

  // Initialize Wi-Fi driver BEFORE AFE — WiFi needs ~50-70KB internal RAM,
  // and AEC/NS in AFE can consume internal RAM even with PSRAM preference.
  // Must init WiFi first to prevent OOM on esp_wifi_init().
  WifiManager::GetInstance().OnProvisioningStart([this]() {
    NotifyEvent(EVT_PROVISIONING_STARTED);
  });
  WifiManager::GetInstance().Init();
  WifiManager::GetInstance().OnStatusChange([this](bool connected) {
    if (connected) {
      NotifyEvent(EVT_WIFI_CONNECTED);
    } else {
      NotifyEvent(EVT_WIFI_DISCONNECTED);
    }
    DisplayManager::GetInstance().SetWifiConnected(connected);
  });

  // Initialize AFE processor (AEC + NS) and local Daisy microWakeWord.
  if (AfeProcessor::GetInstance().Init() == ESP_OK) {
    if (MicroWakeWordDetector::GetInstance().Init() == ESP_OK) {
      MicroWakeWordDetector::GetInstance().SetWakeCallback(
          [this]() { NotifyEvent(EVT_WAKE_WORD_DETECTED); });
    } else {
      ESP_LOGW(TAG, "Daisy microWakeWord init failed, button-only wake mode");
    }
    AfeProcessor::GetInstance().SetVadCallback(
        [this](bool is_speech) {
          vad_is_speech_ = is_speech;
          if (is_speech) {
            vad_speech_detected_ = true;
            if (speech_start_ms_ == 0) {
              speech_start_ms_ = esp_timer_get_time() / 1000;
            }
            silence_start_ms_ = 0;  // Reset silence timer when speech resumes
          } else {
            speech_start_ms_ = 0;
            // Silence started — record timestamp for auto-stop counting
            if (vad_speech_detected_ && silence_start_ms_ == 0) {
              silence_start_ms_ = esp_timer_get_time() / 1000;
            }
          }
          // Log only during listening to reduce spam
          if (GetDeviceState() == kDeviceStateListening) {
            ESP_LOGI("app", "VAD: %s (mode=%s)",
                     is_speech ? "speech" : "silence",
                     listening_mode_ == kListeningModeAutoStop ? "auto" : "manual");
          }
        });
    AfeProcessor::GetInstance().Start();
  } else {
    ESP_LOGW(TAG, "AFE processor init failed, button-only mode");
  }

  // Register state change handlers
  state_machine_.OnStateChange(
      [this](DeviceState old_state, DeviceState new_state) {
        NotifyEvent(EVT_STATE_CHANGED);
        DisplayManager::GetInstance().SetDeviceState(new_state);
        // Server may announce IDLE before the final TTS frames have drained.
        // Never reopen wake detection over our own speaker audio, nor during
        // an active continuous-dialogue session.
        MicroWakeWordDetector::GetInstance().SetEnabled(
            new_state == kDeviceStateIdle && !continuous_mode_ &&
            !AudioService::GetInstance().IsPlaying());
        if (new_state == kDeviceStateListening ||
            new_state == kDeviceStateThinking ||
            new_state == kDeviceStateSpeaking) {
          DisplayManager::GetInstance().HideOverlay();
        }

        // Control audio based on state
        if (new_state == kDeviceStateListening) {
          listening_started_ms_ = esp_timer_get_time() / 1000;
          stop_sent_ms_ = 0;
          vad_speech_detected_ = false;
          vad_is_speech_ = false;
          silence_start_ms_ = 0;
          speech_start_ms_ = 0;
          needs_segment_ = false;
          segment_count_ = 0;
          segment_start_ms_ = listening_started_ms_;
          state_machine_.SetEmotion(kEmotionListening, 1.0f);
          // Play prompt tone on initial activation, skip on continuous re-entry
          if (!continuous_relisten_) {
            AudioService::GetInstance().PlayPrompt(true); // ascending start tone
          }
          continuous_relisten_ = false;
          AudioService::GetInstance().StartCapture();
          AudioService::GetInstance().StopPlayback();
          MsgRouter::GetInstance().FlushAudioQueue();
          AudioService::GetInstance().ClearBuffers();

          ESP_LOGI(TAG, "Listening started (mode: %s)",
                   listening_mode_ == kListeningModeAutoStop ? "AUTO" : "MANUAL");

          // Send listen(start, mode) unless already sent (wake word sends listen(detect, mode))
          if (!listen_sent_) {
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "listen");
            cJSON_AddStringToObject(msg, "state", "start");
            cJSON_AddStringToObject(msg, "mode",
                listening_mode_ == kListeningModeAutoStop ? "auto" : "manual");
            char *str = cJSON_PrintUnformatted(msg);
            esp_err_t send_ret =
                HililyMqttClient::GetInstance().PublishJson(str);
            cJSON_free(str);
            cJSON_Delete(msg);
            if (send_ret != ESP_OK) {
              ESP_LOGW(TAG, "listen(start) send failed (%s), leaving dialogue mode",
                       esp_err_to_name(send_ret));
              continuous_mode_ = false;
              continuous_relisten_ = false;
              listen_sent_ = false;
              SetDeviceState(HililyMqttClient::GetInstance().IsConnected()
                                 ? kDeviceStateIdle
                                 : kDeviceStateWifiConnecting);
            }
          }
          listen_sent_ = false;
        } else if (new_state == kDeviceStateSpeaking) {
          // Keep capture running during SPEAKING for AEC barge-in:
          // AEC-cleaned audio is sent to server → Silero VAD detects user speech
          // AudioService::GetInstance().StopCapture();  // Removed for AEC barge-in
          AudioService::GetInstance().StartPlayback();
        } else if (new_state == kDeviceStateThinking) {
          state_machine_.SetEmotion(kEmotionThinking, 0.5f);
        } else if (new_state == kDeviceStateIdle) {
          state_machine_.SetEmotion(kEmotionNeutral, 1.0f);
          listening_started_ms_ = 0;
          AudioService::GetInstance().StopCapture();
          // If server switches speaking -> idle immediately after last chunk,
          // stopping playback here can cut off buffered tail audio.
          if (old_state != kDeviceStateSpeaking) {
            AudioService::GetInstance().StopPlayback();
          }
          ESP_LOGI(TAG, "Idle (from=%s, continuous=%d)",
                   device_state_to_string(old_state), (int)continuous_mode_);
          // NOTE: Continuous re-listen is triggered by OnPlaybackComplete, NOT here.
          // state_change:idle arrives while TTS audio is still draining through speaker.
        } else if (new_state == kDeviceStateReminding) {
          listening_started_ms_ = 0;
          AudioService::GetInstance().StopCapture();
          MsgRouter::GetInstance().FlushAudioQueue();
          AudioService::GetInstance().ClearBuffers();
          state_machine_.SetEmotion(kEmotionReminding, 1.0f);
          AudioService::GetInstance().PlayAlarmTone();
        } else if (new_state == kDeviceStateWifiConnecting) {
          listening_started_ms_ = 0;
          stop_sent_ms_ = 0;
          listen_sent_ = false;
          continuous_relisten_ = false;
          AudioService::GetInstance().StopCapture();
          AudioService::GetInstance().StopPlayback();
          MsgRouter::GetInstance().FlushAudioQueue();
          AudioService::GetInstance().ClearBuffers();
        }
      });

  state_machine_.OnEmotionChange([](EmotionType emotion, float intensity) {
    DisplayManager::GetInstance().SetEmotion(emotion, intensity);
  });

  // Initialize message router
  MsgRouter::GetInstance().Init(
      &state_machine_, AudioService::GetInstance().GetDownstreamQueue());
  MsgRouter::GetInstance().OnAudioMeta([](const AudioMeta &meta) {
    if (strcmp(meta.format, "opus") == 0) {
      AudioService::GetInstance().SetDownstreamFormat(kAudioFormatOpus);
    } else {
      AudioService::GetInstance().SetDownstreamFormat(kAudioFormatPCM);
    }
  });
  MsgRouter::GetInstance().OnSttResult([](const char *text) {
    DisplayManager::GetInstance().SetText(text);
  });
  MsgRouter::GetInstance().OnTtsEnd([]() {
    AudioService::GetInstance().SignalTtsEnd();
  });
  MsgRouter::GetInstance().OnTtsStart([]() {
    AudioService::GetInstance().ResetTtsEnd();
  });
  MsgRouter::GetInstance().OnEndDialogue([this](const char *reason) {
    ESP_LOGI(TAG, "End dialogue received, disabling continuous mode");
    continuous_mode_ = false;
    if (GetDeviceState() == kDeviceStateIdle &&
        !AudioService::GetInstance().IsPlaying()) {
      MicroWakeWordDetector::GetInstance().SetEnabled(true);
    }
  });

  // Continuous conversation: re-enter listening only AFTER speaker finishes draining
  AudioService::GetInstance().OnPlaybackComplete([this]() {
    if (continuous_mode_ && GetDeviceState() == kDeviceStateIdle) {
      ESP_LOGI(TAG, "Playback complete: scheduling re-listen (500ms delay for speaker decay)");
      Schedule([this]() {
        // 500ms delay to let speaker resonance decay before opening mic
        vTaskDelay(pdMS_TO_TICKS(500));
        if (continuous_mode_ && GetDeviceState() == kDeviceStateIdle) {
          if (!HililyMqttClient::GetInstance().IsConnected()) {
            ESP_LOGW(TAG, "Continuous re-listen skipped: MQTT not connected");
            continuous_mode_ = false;
            continuous_relisten_ = false;
            SetDeviceState(kDeviceStateWifiConnecting);
            return;
          }
          ESP_LOGI(TAG, "Continuous mode: re-entering listening");
          continuous_relisten_ = true;
          listening_mode_ = kListeningModeAutoStop;
          SetDeviceState(kDeviceStateListening);
        }
      });
    } else if (GetDeviceState() == kDeviceStateIdle) {
      // Non-continuous dialogue is truly finished only after the speaker has
      // drained. Re-enable Daisy here rather than on an early server IDLE.
      MicroWakeWordDetector::GetInstance().SetEnabled(true);
    }
  });

  // Transition to wifi connecting (or provisioning)
  // WiFi was already initialized earlier (before AFE) to avoid internal RAM OOM.
  // Note: If provisioning started (no credentials), the EVT_PROVISIONING_STARTED
  // callback already fired and will be handled in Run(). We still show the overlay
  // here synchronously so it appears immediately without waiting for Run() to start.
  if (WifiManager::GetInstance().IsProvisioning()) {
    HandleProvisioningStarted();
  } else {
    state_machine_.TransitionTo(kDeviceStateWifiConnecting);
    WifiManager::GetInstance().Connect();
  }

  ESP_LOGI(TAG, "Application started, entering main loop");
}

void Application::Run() {
  const uint32_t ALL_EVENTS = 0xFFFF;  // bits 0-15
  uint32_t battery_tick = 0;

  while (true) {
    EventBits_t bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE,
                                           pdFALSE, pdMS_TO_TICKS(1000));

    if (bits & EVT_SCHEDULE) {
      HandleScheduledTasks();
    }
    if (bits & EVT_WIFI_CONNECTED) {
      HandleWifiConnected();
    }
    if (bits & EVT_WIFI_DISCONNECTED) {
      HandleWifiDisconnected();
    }
    if (bits & EVT_WS_CONNECTED) {
      ESP_LOGI(TAG, "MQTT connected, device ready");
      if (server_retry_timer_) {
        esp_timer_stop(server_retry_timer_);
      }
      DisplayManager::GetInstance().HideOverlay();
      if (GetDeviceState() == kDeviceStateWifiConnecting) {
        SetDeviceState(kDeviceStateIdle);
      }
    }
    if (bits & EVT_WS_DISCONNECTED) {
      ESP_LOGW(TAG, "MQTT disconnected, leaving dialogue mode");
      continuous_mode_ = false;
      continuous_relisten_ = false;
      listen_sent_ = false;
      stop_sent_ms_ = 0;
      listening_started_ms_ = 0;
      UdpClient::GetInstance().Disconnect();
      if (GetDeviceState() != kDeviceStateWifiConnecting) {
        SetDeviceState(kDeviceStateWifiConnecting);
      }
    }
    if (bits & EVT_PROVISIONING_STARTED) {
      HandleProvisioningStarted();
    }
    if (bits & EVT_BUTTON_PRESSED) {
      HandleButtonPressed();
    }
    if (bits & EVT_BUTTON_LONG_PRESSED) {
      HandleButtonLongPressed();
    }
    if (bits & EVT_WAKE_WORD_DETECTED) {
      ESP_LOGI(TAG, "Wake word detected!");
      if (GetDeviceState() == kDeviceStateIdle) {
        if (!HililyMqttClient::GetInstance().IsConnected()) {
          ESP_LOGW(TAG, "Wake word ignored: MQTT not connected");
          continuous_mode_ = false;
          SetDeviceState(kDeviceStateWifiConnecting);
          continue;
        }
        // Wake word → AUTO mode + continuous conversation
        continuous_mode_ = true;
        listening_mode_ = kListeningModeAutoStop;
        // Confirm locally before opening the ASR window. The greeting itself
        // tells the user when to start speaking and replaces the start tone.
        MicroWakeWordDetector::GetInstance().SetEnabled(false);
        AudioService::GetInstance().PlayWakeGreeting();
        continuous_relisten_ = true;
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "listen");
        cJSON_AddStringToObject(msg, "state", "detect");
        cJSON_AddStringToObject(msg, "text", "嗨莉莉");
        cJSON_AddStringToObject(msg, "mode", "auto");
        char *str = cJSON_PrintUnformatted(msg);
        esp_err_t send_ret = HililyMqttClient::GetInstance().PublishJson(str);
        cJSON_free(str);
        cJSON_Delete(msg);
        if (send_ret != ESP_OK) {
          ESP_LOGW(TAG, "listen(detect) send failed (%s), staying idle",
                   esp_err_to_name(send_ret));
          continuous_mode_ = false;
          continuous_relisten_ = false;
          listen_sent_ = false;
          MicroWakeWordDetector::GetInstance().SetEnabled(true);
          if (!HililyMqttClient::GetInstance().IsConnected()) {
            SetDeviceState(kDeviceStateWifiConnecting);
          }
          continue;
        }
        listen_sent_ = true;
        SetDeviceState(kDeviceStateListening);
      }
    }
    if (bits & EVT_REMINDER_TRIGGERED) {
      // Enter reminding state, which will clear audio and show alarm UI
      SetDeviceState(kDeviceStateReminding);
      // Notify server so it can generate TTS response
      if (!triggered_reminder_id_.empty()) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "reminder_triggered");
        cJSON_AddStringToObject(msg, "reminder_id",
                                triggered_reminder_id_.c_str());
        char *str = cJSON_PrintUnformatted(msg);
        HililyMqttClient::GetInstance().PublishJson(str);
        ESP_LOGI(TAG, "Sent reminder_triggered: %s",
                 triggered_reminder_id_.c_str());
        cJSON_free(str);
        cJSON_Delete(msg);
        triggered_reminder_id_.clear();
      }
    }
    if (bits & EVT_LOW_BATTERY) {
      auto &audio = AudioService::GetInstance();
      if (low_battery_pending_ && pending_low_battery_percent_ <= 20 &&
          GetDeviceState() == kDeviceStateIdle &&
          !AudioService::GetInstance().IsPlaying() &&
          !audio.IsCapturing() &&
          HililyMqttClient::GetInstance().IsConnected()) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "low_battery");
        cJSON_AddNumberToObject(msg, "battery",
                                pending_low_battery_percent_);
        cJSON_AddNumberToObject(msg, "voltage_mv",
                                pending_low_battery_voltage_mv_);
        cJSON_AddNumberToObject(msg, "snapshot_sequence",
                                pending_low_battery_sequence_);
        cJSON_AddStringToObject(
            msg, "text", "Low battery, please charge as soon as possible.");
        char *str = cJSON_PrintUnformatted(msg);
        esp_err_t ret = HililyMqttClient::GetInstance().PublishJson(str);
        cJSON_free(str);
        cJSON_Delete(msg);
        if (ret == ESP_OK) {
          last_low_battery_reminder_ms_ = esp_timer_get_time() / 1000;
          low_battery_pending_ = false;
          DisplayManager::GetInstance().ShowOverlay(
              "LOW BATTERY\nPlease charge", 15000);
          ESP_LOGW(TAG, "Low-battery reminder sent: %u%%, %lumV, snapshot=%lu",
                   pending_low_battery_percent_,
                   static_cast<unsigned long>(pending_low_battery_voltage_mv_),
                   static_cast<unsigned long>(pending_low_battery_sequence_));
        } else {
          ESP_LOGW(TAG, "Low-battery reminder send failed: %s",
                   esp_err_to_name(ret));
        }
      }
    }
    ProcessListeningTimeout();

    // Periodic: update status icons every 10s
    battery_tick++;
    if (battery_tick >= 10) {
      battery_tick = 0;
      const BatterySnapshot battery = Battery::GetInstance().GetSnapshot();
      if (battery.valid) {
        DisplayManager::GetInstance().SetBatteryLevel(battery.percentage);
      }
      const int64_t now_ms = esp_timer_get_time() / 1000;
      constexpr int64_t kLowBatteryReminderIntervalMs = 5 * 60 * 1000;
      if (battery.valid && battery.low_confirmed &&
          (last_low_battery_reminder_ms_ == 0 ||
           now_ms - last_low_battery_reminder_ms_ >=
               kLowBatteryReminderIntervalMs)) {
        // The exact same validated snapshot drives icon, overlay and MQTT.
        pending_low_battery_percent_ = battery.percentage;
        pending_low_battery_voltage_mv_ = battery.voltage_mv;
        pending_low_battery_sequence_ = battery.sequence / 2;
        low_battery_pending_ = true;
        NotifyEvent(EVT_LOW_BATTERY);
      } else if (battery.valid && battery.percentage > 20) {
        // Allow an immediate warning if the battery later crosses the threshold.
        last_low_battery_reminder_ms_ = 0;
        low_battery_pending_ = false;
      }
      if (WifiManager::GetInstance().IsConnected()) {
        DisplayManager::GetInstance().SetWifiRssi(
            WifiManager::GetInstance().GetRssi());
      }
      DisplayManager::GetInstance().UpdateTimeDisplay();
    }
  }
}

void Application::Schedule(std::function<void()> &&callback) {
  std::lock_guard<std::mutex> lock(schedule_mutex_);
  scheduled_tasks_.push_back(std::move(callback));
  NotifyEvent(EVT_SCHEDULE);
}

void Application::NotifyEvent(uint32_t event_bits) {
  xEventGroupSetBits(event_group_, event_bits);
}

void Application::ScheduleServerRetry(int delay_ms) {
  if (!server_retry_timer_) {
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = [](void *arg) {
      static_cast<Application *>(arg)->NotifyEvent(EVT_WIFI_CONNECTED);
    };
    timer_args.arg = this;
    timer_args.name = "server_retry";
    esp_err_t err = esp_timer_create(&timer_args, &server_retry_timer_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Create server retry timer failed: %s", esp_err_to_name(err));
      return;
    }
  }

  esp_timer_stop(server_retry_timer_);
  esp_err_t err = esp_timer_start_once(server_retry_timer_, delay_ms * 1000);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Server retry scheduled in %d ms", delay_ms);
  } else {
    ESP_LOGE(TAG, "Start server retry timer failed: %s", esp_err_to_name(err));
  }
}

void Application::HandleScheduledTasks() {
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(schedule_mutex_);
    tasks.swap(scheduled_tasks_);
  }
  for (auto &task : tasks) {
    task();
  }
}

void Application::HandleWifiConnected() {
  if (!WifiManager::GetInstance().IsConnected()) {
    ESP_LOGW(TAG, "Server retry skipped: Wi-Fi is not connected");
    return;
  }

  ESP_LOGI(TAG, "Wi-Fi connected, authenticating...");
  DisplayManager::GetInstance().HideBootAnimation();

  // Log IP address (display only on explicit long-press)
  {
    std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (!ip.empty()) {
      ESP_LOGI(TAG, "Device IP: %s", ip.c_str());
    }
  }

  // Quick TCP connectivity test before auth
  bool server_tcp_ok = false;
  {
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kServerHttpPort);
    inet_pton(AF_INET, kServerHost, &dest.sin_addr);
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s >= 0) {
      // Socket send/receive timeouts do not bound a blocking connect().
      // A nonblocking connect + select gives the probe a real 5-second limit.
      const int flags = fcntl(s, F_GETFL, 0);
      int socket_error = 0;
      if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        socket_error = errno;
      } else {
        int result = connect(s, (struct sockaddr *)&dest, sizeof(dest));
        socket_error = result == 0 ? 0 : errno;
        if (result < 0 && socket_error == EINPROGRESS) {
          fd_set writable;
          FD_ZERO(&writable);
          FD_SET(s, &writable);
          struct timeval timeout = {5, 0};
          result = select(s + 1, nullptr, &writable, nullptr, &timeout);
          if (result > 0) {
            socklen_t size = sizeof(socket_error);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &socket_error, &size) < 0) {
              socket_error = errno;
            }
          } else {
            socket_error = result == 0 ? ETIMEDOUT : errno;
          }
        }
        server_tcp_ok = socket_error == 0;
      }
      ESP_LOGI(TAG, "TCP test to %s:%d => %s (errno=%d: %s)",
               kServerHost, kServerHttpPort, server_tcp_ok ? "OK" : "FAIL",
               socket_error, socket_error ? strerror(socket_error) : "connected");
      close(s);
    } else {
      ESP_LOGE(TAG, "TCP test socket create failed: %d", errno);
    }
  }

  if (!server_tcp_ok) {
    char overlay_msg[160];
    snprintf(overlay_msg, sizeof(overlay_msg),
             "WiFi connected\nServer connection failed\n\n"
             "%s:%d\n\n"
             "Retrying in %ds",
             kServerHost, kServerHttpPort, kServerRetryDelayMs / 1000);
    DisplayManager::GetInstance().ShowOverlay(overlay_msg, kServerRetryDelayMs);
    ScheduleServerRetry(kServerRetryDelayMs);
    return;
  }

  // Get device MAC as device_id
  char device_id[18];
#ifdef CONFIG_HILILY_QEMU_MODE
  snprintf(device_id, sizeof(device_id), "QE:MU:00:00:00:01");
#else
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif

  // 从 NVS 读取已保存的 device token (首次激活后持久化)
  std::string device_token;
  {
    nvs_handle_t nvs;
    if (nvs_open("device", NVS_READONLY, &nvs) == ESP_OK) {
      size_t len = 0;
      if (nvs_get_str(nvs, "device_token", nullptr, &len) == ESP_OK && len > 1) {
        char buf[256];
        if (len <= sizeof(buf)) {
          nvs_get_str(nvs, "device_token", buf, &len);
          device_token = buf;
          ESP_LOGI(TAG, "Loaded device_token from NVS (%zu chars)", device_token.size());
        }
      }
      nvs_close(nvs);
    }
  }

  // 如果没有 device_token，走首次注册→激活码→轮询流程
  if (device_token.empty()) {
    ESP_LOGI(TAG, "No device_token in NVS, starting onboarding flow...");
    HandleDeviceOnboarding(SERVER_URL, device_id);
    // Reload token after onboarding completes
    {
      nvs_handle_t nvs;
      if (nvs_open("device", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_str(nvs, "device_token", nullptr, &len) == ESP_OK && len > 1) {
          char buf[256];
          if (len <= sizeof(buf)) {
            nvs_get_str(nvs, "device_token", buf, &len);
            device_token = buf;
          }
        }
        nvs_close(nvs);
      }
    }
    if (device_token.empty()) {
      ESP_LOGE(TAG, "Onboarding failed, no device_token available");
      return;
    }
  }
  const std::string secret = device_token;

  // Authenticate with retry
  esp_err_t ret = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; attempt++) {
    ret = HililyMqttClient::GetInstance().Authenticate(
        SERVER_URL, device_id, secret, OtaManager::GetInstance().GetVersion(),
        Battery::GetInstance().GetPercentage());
    if (ret == ESP_OK) break;
    if (ret == ESP_ERR_NOT_FINISHED) break;  // pending activation, 不再重试
    ESP_LOGW(TAG, "Auth attempt %d/3 failed, retrying in %ds...", attempt, attempt * 2);
    vTaskDelay(pdMS_TO_TICKS(attempt * 2000));
  }

  // 设备待激活 → 显示提示，每 30s 重试直到 admin 激活
  if (ret == ESP_ERR_NOT_FINISHED) {
    char overlay_msg[256];
    snprintf(overlay_msg, sizeof(overlay_msg),
        "Pending Activation\n\n"
        "Please activate this device at\n"
        "admin.hilily.sarah.xin\n\n"
        "MAC: %s", device_id);
    DisplayManager::GetInstance().ShowOverlay(overlay_msg, 0);
    ESP_LOGI(TAG, "Device pending activation, polling every 30s...");

    while (ret == ESP_ERR_NOT_FINISHED) {
      vTaskDelay(pdMS_TO_TICKS(30000));  // 30 秒轮询
      ret = HililyMqttClient::GetInstance().Authenticate(
          SERVER_URL, device_id, secret, OtaManager::GetInstance().GetVersion(),
          Battery::GetInstance().GetPercentage());
    }
    DisplayManager::GetInstance().HideOverlay();
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "Device activated! Proceeding with connection.");
    }
  }

  // Auth 成功后：检查是否有新下发的 token 需要持久化到 NVS
  if (ret == ESP_OK) {
    const auto &auth_result = HililyMqttClient::GetInstance().GetAuth();
    if (!auth_result.device_token.empty()) {
      nvs_handle_t nvs;
      if (nvs_open("device", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "device_token", auth_result.device_token.c_str());
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "New device token saved to NVS (%zu chars)",
                 auth_result.device_token.size());
      }
    }
  }

  if (ret == ESP_OK) {
    if (server_retry_timer_) {
      esp_timer_stop(server_retry_timer_);
    }
    DisplayManager::GetInstance().HideOverlay();
    HililyMqttClient::GetInstance().OnStatus([this](bool connected) {
      if (connected) {
        NotifyEvent(EVT_WS_CONNECTED); // reuse event name for compat
        // Send hello to the server so it triggers UDP session negotiation
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "type", "hello");
        // Signal AEC capability so server enables Silero VAD barge-in detection
        cJSON *features = cJSON_AddObjectToObject(hello, "features");
        cJSON_AddBoolToObject(features, "aec", true);
        char *hello_str = cJSON_PrintUnformatted(hello);
        HililyMqttClient::GetInstance().PublishJson(hello_str);
        cJSON_free(hello_str);
        cJSON_Delete(hello);
        MsgRouter::GetInstance().Start();
      } else {
        NotifyEvent(EVT_WS_DISCONNECTED);
      }
    });
    HililyMqttClient::GetInstance().Connect();
  } else {
    ESP_LOGE(TAG, "Authentication failed, retrying in %ds",
             kServerRetryDelayMs / 1000);
    char overlay_msg[160];
    snprintf(overlay_msg, sizeof(overlay_msg),
             "Server auth failed\n\n"
             "%s\n\n"
             "Retrying in %ds",
             kServerHost, kServerRetryDelayMs / 1000);
    DisplayManager::GetInstance().ShowOverlay(overlay_msg, kServerRetryDelayMs);
    ScheduleServerRetry(kServerRetryDelayMs);
    return;
  }

  // Configure OTA manager with server URL and device ID, then check for updates
  OtaManager::GetInstance().SetServerUrl(SERVER_URL);
  OtaManager::GetInstance().SetDeviceId(device_id);
  OtaManager::GetInstance().CheckForUpdate();
  OtaManager::GetInstance().StartPeriodicCheck();

  // Stay in wifi_connecting until MQTT reports connected. Entering idle earlier
  // lets button/wake-word start listening before PublishJson() can succeed.
}

void Application::HandleDeviceOnboarding(const std::string &server_url,
                                          const std::string &mac_address) {
  ESP_LOGI(TAG, "Starting device onboarding for MAC: %s", mac_address.c_str());

  DeviceRegistration reg_result;
  bool need_register = true;

  while (true) {
    // Step 1: Register device (get activation code)
    if (need_register) {
      esp_err_t reg_ret = HililyMqttClient::GetInstance().RegisterDevice(
          server_url, mac_address, reg_result);

      if (reg_ret == ESP_ERR_INVALID_STATE) {
        // Device already registered but token wasn't saved — skip to polling
        ESP_LOGW(TAG, "Device already registered, skipping to activation polling");
        need_register = false;
      } else if (reg_ret != ESP_OK) {
        ESP_LOGE(TAG, "RegisterDevice failed: %s, retrying in 10s...",
                 esp_err_to_name(reg_ret));
        vTaskDelay(pdMS_TO_TICKS(10000));
        continue;
      } else {
        ESP_LOGI(TAG, "Device registered, activation_code: %s, expires_in: %d",
                 reg_result.activation_code.c_str(), reg_result.expires_in);
        need_register = false;
      }
    }

    // Step 2: Display activation code on screen
    if (!reg_result.activation_code.empty()) {
      char overlay_msg[256];
      snprintf(overlay_msg, sizeof(overlay_msg),
               "激活码: %s\n请告诉家人此激活码",
               reg_result.activation_code.c_str());
      DisplayManager::GetInstance().ShowOverlay(overlay_msg, 0);
    } else {
      // Already registered case — show waiting message
      DisplayManager::GetInstance().ShowOverlay("等待激活中...\n请联系家人激活设备", 0);
    }

    // Step 3: Poll for activation status
    int64_t poll_start_ms = esp_timer_get_time() / 1000;
    int64_t expires_in_ms = reg_result.expires_in > 0
                               ? (int64_t)reg_result.expires_in * 1000
                               : (int64_t)86400 * 1000; // default 24h

    while (true) {
      vTaskDelay(pdMS_TO_TICKS(10000)); // Poll every 10 seconds

      ActivationStatus status;
      esp_err_t poll_ret = HililyMqttClient::GetInstance().CheckActivationStatus(
          server_url, mac_address, status);

      if (poll_ret != ESP_OK) {
        ESP_LOGW(TAG, "CheckActivationStatus failed: %s, will retry...",
                 esp_err_to_name(poll_ret));
        continue;
      }

      ESP_LOGI(TAG, "Activation status: %s", status.status.c_str());

      if (status.status == "active" && !status.device_token.empty()) {
        // Activation successful — save token to NVS
        nvs_handle_t nvs;
        if (nvs_open("device", NVS_READWRITE, &nvs) == ESP_OK) {
          nvs_set_str(nvs, "device_token", status.device_token.c_str());
          nvs_commit(nvs);
          nvs_close(nvs);
          ESP_LOGI(TAG, "device_token saved to NVS (%zu chars)",
                   status.device_token.size());
        } else {
          ESP_LOGE(TAG, "Failed to open NVS for writing device_token");
        }

        // Show success message
        DisplayManager::GetInstance().ShowOverlay("设备激活成功!", 3000);
        vTaskDelay(pdMS_TO_TICKS(3000));
        DisplayManager::GetInstance().HideOverlay();

        ESP_LOGI(TAG, "Device onboarding complete");
        return;
      }

      // Check expiry — if exceeded, re-register to get a new activation code
      int64_t elapsed_ms = (esp_timer_get_time() / 1000) - poll_start_ms;
      if (elapsed_ms >= expires_in_ms) {
        ESP_LOGW(TAG, "Activation code expired after %llds, re-registering...",
                 (long long)(elapsed_ms / 1000));
        need_register = true;
        break; // Break inner loop, outer loop will re-register
      }
    }
  }
}

void Application::HandleWifiDisconnected() {
  ESP_LOGW(TAG, "Wi-Fi disconnected");
  continuous_mode_ = false;
  continuous_relisten_ = false;
  listen_sent_ = false;
  stop_sent_ms_ = 0;
  listening_started_ms_ = 0;
  if (server_retry_timer_) {
    esp_timer_stop(server_retry_timer_);
  }
  SendGoodbye();
  HililyMqttClient::GetInstance().Disconnect();
  UdpClient::GetInstance().Disconnect();
  SetDeviceState(kDeviceStateWifiConnecting);
}

void Application::SendGoodbye() {
  if (!HililyMqttClient::GetInstance().IsConnected())
    return;
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "goodbye");
  char *str = cJSON_PrintUnformatted(msg);
  HililyMqttClient::GetInstance().PublishJson(str);
  ESP_LOGI(TAG, "Sent goodbye to server");
  cJSON_free(str);
  cJSON_Delete(msg);
}

void Application::HandleProvisioningStarted() {
  ESP_LOGI(TAG, "Provisioning mode activated, showing setup UI");
  DisplayManager::GetInstance().HideBootAnimation();

  auto &wm = WifiManager::GetInstance();
  char msg[256];
  snprintf(msg, sizeof(msg),
           "WiFi Setup\n\n"
           "1. Connect WiFi:\n"
           "   %s\n\n"
           "2. Follow the popup\n"
           "   or open browser:\n"
           "   http://192.168.4.1",
           wm.GetApSsid().c_str());
  // timeout=0: persistent overlay until WiFi configured successfully
  DisplayManager::GetInstance().ShowOverlay(msg, 0);
}

void Application::HandleButtonPressed() {
  ESP_LOGI(TAG, "Button pressed");
  // A pending low-battery overlay must never cover wake/listening UI.
  DisplayManager::GetInstance().HideOverlay();
  DeviceState state = GetDeviceState();

  if (state == kDeviceStateSpeaking ||
      AudioService::GetInstance().IsPlaying()) {
    // Barge-in: send abort to stop server TTS+LLM pipeline (#90, #104)
    cJSON *abort_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(abort_msg, "type", "abort");
    cJSON_AddStringToObject(abort_msg, "reason", "barge-in");
    char *abort_str = cJSON_PrintUnformatted(abort_msg);
    HililyMqttClient::GetInstance().PublishJson(abort_str);
    ESP_LOGI(TAG, "Barge-in: sent abort");
    cJSON_free(abort_str);
    cJSON_Delete(abort_msg);

    // Fade the currently audible frame before clearing queued audio, avoiding
    // an abrupt mid-syllable I2S cutoff.
    listening_mode_ = kListeningModeManualStop;
    AudioService::GetInstance().FadeOutPlayback(180);
    MsgRouter::GetInstance().FlushAudioQueue();
    AudioService::GetInstance().ClearBuffers();
    SetDeviceState(kDeviceStateListening);
  } else if (state == kDeviceStateIdle) {
    if (!HililyMqttClient::GetInstance().IsConnected()) {
      ESP_LOGW(TAG, "Button ignored: MQTT not connected");
      continuous_mode_ = false;
      SetDeviceState(kDeviceStateWifiConnecting);
      return;
    }
    // Button in idle → start continuous conversation (AUTO mode, VAD auto-stop + re-listen)
    continuous_mode_ = true;
    listening_mode_ = kListeningModeAutoStop;
    SetDeviceState(kDeviceStateListening);
  } else if (state == kDeviceStateListening) {
    // Button while listening → exit continuous conversation
    continuous_mode_ = false;
    SendListenStop();
    AudioService::GetInstance().PlayPrompt(false); // descending stop tone
  } else if (state == kDeviceStateReminding) {
    // Dismiss reminder
    SetDeviceState(kDeviceStateIdle);
  } else if (state == kDeviceStateWifiConnecting) {
    if (WifiManager::GetInstance().IsConnected() &&
        !HililyMqttClient::GetInstance().IsConnected()) {
      ESP_LOGI(TAG, "Button pressed while waiting for server, retrying now");
      DisplayManager::GetInstance().ShowOverlay("Retrying server connection...", 5000);
      NotifyEvent(EVT_WIFI_CONNECTED);
    }
  }
}

void Application::HandleButtonLongPressed() {
  ESP_LOGI(TAG, "Long press received");

  auto& wm = WifiManager::GetInstance();

  if (wm.IsConnected()) {
    std::string ip = wm.GetIpAddress();
    char connected_msg[160];
    snprintf(connected_msg, sizeof(connected_msg),
             "WiFi connected\n\n"
             "IP: %s\n\n"
             "Saved WiFi will be reused automatically.",
             ip.empty() ? "unknown" : ip.c_str());
    DisplayManager::GetInstance().ShowOverlay(connected_msg, 5000);
    ESP_LOGI(TAG, "Long press ignored: WiFi already connected (IP=%s)",
             ip.empty() ? "unknown" : ip.c_str());
    return;
  }

  if (wm.HasStoredCredentials()) {
    DisplayManager::GetInstance().ShowOverlay(
        "Checking saved WiFi...\n\n"
        "Setup mode will open only if connection fails.",
        8000);
    ESP_LOGI(TAG, "Long press: trying saved WiFi before config mode");
    wm.StartConfigMode();
    return;
  }

  wm.StartConfigMode();

  // Build info message for overlay display
  std::string ip = wm.GetIpAddress();

  char msg[200];
  if (wm.IsConnected() && !ip.empty()) {
    snprintf(msg, sizeof(msg),
             "WiFi Settings\n\n"
             "Open browser:\n"
             "http://%s\n\n"
             "Or connect WiFi:\n"
             "  AP: %s\n"
             "  http://192.168.4.1",
             ip.c_str(), wm.GetApSsid().c_str());
  } else {
    snprintf(msg, sizeof(msg),
             "WiFi Settings\n\n"
             "1. Connect WiFi:\n"
             "   %s\n"
             "2. Open browser:\n"
             "   http://192.168.4.1",
             wm.GetApSsid().c_str());
  }

  // Show for 30 seconds, auto-dismiss
  DisplayManager::GetInstance().ShowOverlay(msg, 30000);
  ESP_LOGI(TAG, "Config mode: AP=%s, IP=%s", wm.GetApSsid().c_str(), ip.c_str());
}

void Application::SendListenStop() {
  AudioService::GetInstance().StopCapture();
  listening_started_ms_ = 0;

  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "listen");
  cJSON_AddStringToObject(msg, "state", "stop");
  if (segment_count_ > 0) {
    cJSON_AddNumberToObject(msg, "segments", segment_count_);
  }
  char *str = cJSON_PrintUnformatted(msg);
  esp_err_t send_ret = HililyMqttClient::GetInstance().PublishJson(str);
  cJSON_free(str);
  cJSON_Delete(msg);

  if (send_ret != ESP_OK) {
    ESP_LOGW(TAG, "listen(stop) send failed (%s), leaving dialogue mode",
             esp_err_to_name(send_ret));
    stop_sent_ms_ = 0;
    continuous_mode_ = false;
    continuous_relisten_ = false;
    listen_sent_ = false;
    SetDeviceState(HililyMqttClient::GetInstance().IsConnected()
                       ? kDeviceStateIdle
                       : kDeviceStateWifiConnecting);
  } else {
    stop_sent_ms_ = esp_timer_get_time() / 1000;
  }
}

void Application::SendSegmentBoundary() {
  segment_count_++;
  segment_start_ms_ = esp_timer_get_time() / 1000;
  needs_segment_ = false;
  // Don't reset silence_start_ms_ — silence continues for auto-stop counting

  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "listen");
  cJSON_AddStringToObject(msg, "state", "segment");
  cJSON_AddNumberToObject(msg, "segment", segment_count_);
  char *str = cJSON_PrintUnformatted(msg);
  HililyMqttClient::GetInstance().PublishJson(str);
  cJSON_free(str);
  cJSON_Delete(msg);

  ESP_LOGI(TAG, "Segment boundary #%d sent (mode=%s)",
           segment_count_,
           listening_mode_ == kListeningModeAutoStop ? "auto" : "manual");
}

void Application::ProcessListeningTimeout() {
  if (GetDeviceState() != kDeviceStateListening) {
    stop_sent_ms_ = 0;
    return;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;

  // Fallback: if listen(stop) was sent but server didn't respond within 3s
  if (stop_sent_ms_ > 0 && (now_ms - stop_sent_ms_) > 3000) {
    ESP_LOGW(TAG, "Server unresponsive after listen(stop), forcing idle");
    stop_sent_ms_ = 0;
    listening_started_ms_ = 0;
    SetDeviceState(kDeviceStateIdle);
    return;
  }

  if (listening_started_ms_ <= 0 || stop_sent_ms_ > 0) {
    return;  // Not started or already stopping
  }

  int64_t elapsed_ms = now_ms - listening_started_ms_;
  int64_t segment_elapsed = segment_start_ms_ > 0 ? (now_ms - segment_start_ms_) : elapsed_ms;
  int64_t silence_ms = (!vad_is_speech_ && silence_start_ms_ > 0) ?
                        (now_ms - silence_start_ms_) : 0;

  // Mark segment needed when current segment exceeds threshold
  if (segment_elapsed >= kSegmentTimeoutMs) {
    needs_segment_ = true;
  }

  if (listening_mode_ == kListeningModeAutoStop) {
    // AUTO: 1.5s silence → final stop (only after minimum speech duration)
    if (vad_speech_detected_ && silence_ms >= kVadSilenceTimeoutMs) {
      ESP_LOGI(TAG, "AUTO: VAD silence %lldms (segments=%d), listen(stop)",
               (long long)silence_ms, segment_count_);
      SendListenStop();
      return;
    }
    // AUTO: segment at 1s natural pause after 30s
    if (needs_segment_ && vad_speech_detected_ && silence_ms >= kSegmentPauseMs) {
      SendSegmentBoundary();
      return;
    }
    // Hard timeout (10 min safety)
    if (elapsed_ms >= kAutoHardTimeoutMs) {
      ESP_LOGW(TAG, "AUTO: hard timeout %lldms (segments=%d), listen(stop)",
               (long long)elapsed_ms, segment_count_);
      SendListenStop();
      return;
    }
  } else {
    // MANUAL: segment at 1s natural pause after 30s
    if (needs_segment_ && vad_speech_detected_ && silence_ms >= kSegmentPauseMs) {
      SendSegmentBoundary();
      return;
    }
    // Safety timeout (30 min)
    if (elapsed_ms >= kManualHardTimeoutMs) {
      ESP_LOGW(TAG, "MANUAL: safety timeout %lldms (segments=%d), listen(stop)",
               (long long)elapsed_ms, segment_count_);
      SendListenStop();
      return;
    }
  }
}
