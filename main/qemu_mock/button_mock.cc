/*
 * QEMU mock button manager.
 * Buttons are triggered via the UART test console instead of GPIO.
 */
#include "button_manager.h"
#include <esp_log.h>

static const char *TAG = "button_qemu";

ButtonManager& ButtonManager::GetInstance() {
    static ButtonManager instance;
    return instance;
}

esp_err_t ButtonManager::Init(int gpio_pin) {
    gpio_pin_ = gpio_pin;
    ESP_LOGI(TAG, "[QEMU] Button mock init (use UART 'btn' command)");
    return ESP_OK;
}

// Called by QEMU test console to simulate button press
void ButtonManager::IsrHandler(void* arg) {
    // Not used in QEMU mode
}

void ButtonManager::DebounceTask(void* arg) {
    // Not used in QEMU mode
}
