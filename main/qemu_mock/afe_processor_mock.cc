/*
 * QEMU mock AFE processor.
 * ESP-SR is not available in QEMU. Wake word is triggered via UART console.
 */
#include "afe_processor.h"
#include <esp_log.h>

static const char *TAG = "afe_qemu";

AfeProcessor& AfeProcessor::GetInstance() {
    static AfeProcessor instance;
    return instance;
}

esp_err_t AfeProcessor::Init() {
    ESP_LOGI(TAG, "[QEMU] AFE processor mock init (NS/VAD/WakeNet disabled)");
    return ESP_OK;
}

void AfeProcessor::Start() {
    running_ = true;
    ESP_LOGI(TAG, "[QEMU] AFE processor mock started");
}

void AfeProcessor::Stop() {
    running_ = false;
    ESP_LOGI(TAG, "[QEMU] AFE processor mock stopped");
}

void AfeProcessor::Feed(const int16_t* mic, const int16_t* ref, int count) {
    // In QEMU mode, pass mic audio through directly (no AEC/NS)
    if (audio_cb_ && count > 0) {
        audio_cb_(mic, count);
    }
}

void AfeProcessor::ProcessTask(void* arg) {
    // No-op in QEMU mode
}
