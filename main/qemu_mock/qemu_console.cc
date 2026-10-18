/*
 * QEMU test console - UART command handler for simulating hardware events.
 *
 * Commands:
 *   btn           - Simulate button press (EVT_BUTTON_PRESSED)
 *   wake          - Simulate wake word detection (EVT_WAKE_WORD_DETECTED)
 *   state         - Print current device state
 *   emotion <name> - Set emotion (neutral/happy/sad/comfort/surprise/thinking)
 *   vol <0-100>   - Set volume
 *   bright <0-100> - Set brightness
 *   info          - Print system info (heap, tasks, uptime)
 *   help          - Show available commands
 */
#include "qemu_console.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "state_machine.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "qemu_console";

static const char* StateToString(DeviceState state) {
    switch (state) {
        case kDeviceStateStarting:       return "STARTING";
        case kDeviceStateWifiConnecting:  return "WIFI_CONNECTING";
        case kDeviceStateIdle:            return "IDLE";
        case kDeviceStateListening:       return "LISTENING";
        case kDeviceStateThinking:        return "THINKING";
        case kDeviceStateSpeaking:        return "SPEAKING";
        case kDeviceStateReminding:       return "REMINDING";
        default:                          return "UNKNOWN";
    }
}

static const char* EmotionToString(EmotionType emotion) {
    switch (emotion) {
        case kEmotionNeutral:    return "neutral";
        case kEmotionHappy:      return "happy";
        case kEmotionSad:        return "sad";
        case kEmotionComfort:    return "comfort";
        case kEmotionSurprise:   return "surprise";
        case kEmotionThinking:   return "thinking";
        case kEmotionListening:  return "listening";
        default:                 return "unknown";
    }
}

static EmotionType ParseEmotion(const char* str) {
    if (strcmp(str, "happy") == 0)     return kEmotionHappy;
    if (strcmp(str, "sad") == 0)       return kEmotionSad;
    if (strcmp(str, "comfort") == 0)   return kEmotionComfort;
    if (strcmp(str, "surprise") == 0)  return kEmotionSurprise;
    if (strcmp(str, "thinking") == 0)  return kEmotionThinking;
    if (strcmp(str, "listening") == 0) return kEmotionListening;
    return kEmotionNeutral;
}

static void ProcessCommand(char* cmd) {
    // Strip newline/carriage return
    int len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r')) {
        cmd[--len] = '\0';
    }
    if (len == 0) return;

    auto& app = Application::GetInstance();

    if (strcmp(cmd, "btn") == 0) {
        printf("[QEMU] >> Button press simulated\n");
        app.NotifyEvent(EVT_BUTTON_PRESSED);

    } else if (strcmp(cmd, "wake") == 0) {
        printf("[QEMU] >> Wake word simulated\n");
        app.NotifyEvent(EVT_WAKE_WORD_DETECTED);

    } else if (strcmp(cmd, "state") == 0) {
        DeviceState state = app.GetDeviceState();
        EmotionType emotion = app.GetEmotion();
        printf("[QEMU] State: %s, Emotion: %s\n",
               StateToString(state), EmotionToString(emotion));

    } else if (strncmp(cmd, "emotion ", 8) == 0) {
        const char* name = cmd + 8;
        EmotionType emotion = ParseEmotion(name);
        app.GetStateMachine().SetEmotion(emotion, 1.0f);
        printf("[QEMU] >> Emotion set to: %s\n", EmotionToString(emotion));

    } else if (strncmp(cmd, "vol ", 4) == 0) {
        int vol = atoi(cmd + 4);
        Settings::GetInstance().SetVolume(vol);
        printf("[QEMU] >> Volume set to: %d\n", vol);

    } else if (strncmp(cmd, "bright ", 7) == 0) {
        int bright = atoi(cmd + 7);
        Settings::GetInstance().SetBrightness(bright);
        board_set_brightness(bright);
        printf("[QEMU] >> Brightness set to: %d\n", bright);

    } else if (strcmp(cmd, "info") == 0) {
        printf("[QEMU] System Info:\n");
        printf("  Free heap: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
        printf("  Min free heap: %lu bytes\n", (unsigned long)esp_get_minimum_free_heap_size());
        printf("  Uptime: %lld ms\n", (long long)(esp_timer_get_time() / 1000));
        printf("  Tasks: %lu\n", (unsigned long)uxTaskGetNumberOfTasks());
        printf("  Volume: %d, Brightness: %d\n",
               Settings::GetInstance().GetVolume(),
               Settings::GetInstance().GetBrightness());

    } else if (strcmp(cmd, "help") == 0) {
        printf("\n=== QEMU Test Console ===\n");
        printf("  btn            - Simulate button press\n");
        printf("  wake           - Simulate wake word\n");
        printf("  state          - Show device state\n");
        printf("  emotion <name> - Set emotion (neutral/happy/sad/comfort/surprise/thinking)\n");
        printf("  vol <0-100>    - Set volume\n");
        printf("  bright <0-100> - Set brightness\n");
        printf("  info           - System info\n");
        printf("  help           - This help\n\n");

    } else {
        printf("[QEMU] Unknown command: '%s' (type 'help')\n", cmd);
    }
}

static void ConsoleTask(void* arg) {
    char line_buf[128];
    int pos = 0;

    printf("\n=== HiLily QEMU Test Console ===\n");
    printf("Type 'help' for available commands\n\n");

    while (true) {
        int c = getchar();
        if (c == EOF || c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\n' || c == '\r') {
            line_buf[pos] = '\0';
            if (pos > 0) {
                ProcessCommand(line_buf);
            }
            pos = 0;
        } else if (pos < (int)sizeof(line_buf) - 1) {
            line_buf[pos++] = (char)c;
        }
    }
}

esp_err_t qemu_console_init(void) {
    ESP_LOGI(TAG, "Starting QEMU test console on UART");
    xTaskCreate(ConsoleTask, "qemu_console", 4096, NULL, 2, NULL);
    return ESP_OK;
}
