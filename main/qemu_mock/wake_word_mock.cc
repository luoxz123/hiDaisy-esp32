/*
 * QEMU mock wake word detector.
 * ESP-SR is not available in QEMU. Wake word is triggered via UART console.
 */
#include "wake_word.h"
#include <esp_log.h>

static const char *TAG = "wake_qemu";

WakeWordDetector& WakeWordDetector::GetInstance() {
    static WakeWordDetector instance;
    return instance;
}

esp_err_t WakeWordDetector::Init() {
    ESP_LOGI(TAG, "[QEMU] Wake word detector mock init (use UART 'wake' command)");
    return ESP_OK;
}

void WakeWordDetector::Start() {
    running_ = true;
    ESP_LOGI(TAG, "[QEMU] Wake word mock started");
}

void WakeWordDetector::Stop() {
    running_ = false;
    ESP_LOGI(TAG, "[QEMU] Wake word mock stopped");
}

void WakeWordDetector::Feed(const int16_t* samples, int count) {
    // No-op in QEMU mode
}

void WakeWordDetector::DetectTask(void* arg) {
    // No-op in QEMU mode
}
