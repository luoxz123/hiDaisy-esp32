/*
 * QEMU mock battery - returns fixed percentage.
 */
#include "battery.h"
#include <esp_log.h>
#include <sdkconfig.h>

static const char *TAG = "battery_qemu";

Battery& Battery::GetInstance() {
    static Battery instance;
    return instance;
}

esp_err_t Battery::Init(int adc_pin) {
    adc_pin_ = adc_pin;
    voltage_mv_ = 3800; // Mock ~75%
    ESP_LOGI(TAG, "[QEMU] Battery mock init (fixed %d%%)",
             CONFIG_HILILY_QEMU_MOCK_BATTERY_PERCENT);
    return ESP_OK;
}

void Battery::StartMonitoring() {
    ESP_LOGI(TAG, "[QEMU] Battery monitoring skipped (fixed value)");
}

uint8_t Battery::GetPercentage() const {
    return CONFIG_HILILY_QEMU_MOCK_BATTERY_PERCENT;
}

BatterySnapshot Battery::GetSnapshot() const {
    BatterySnapshot snapshot;
    snapshot.voltage_mv = voltage_mv_;
    snapshot.percentage = CONFIG_HILILY_QEMU_MOCK_BATTERY_PERCENT;
    snapshot.valid = true;
    snapshot.low_confirmed = snapshot.percentage <= 20;
    snapshot.sequence = 1;
    return snapshot;
}

uint32_t Battery::ReadRaw() {
    return 2048; // Mock ADC value
}

void Battery::MonitorTask(void* arg) {
    // No-op in QEMU mode
}
