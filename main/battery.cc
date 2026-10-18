#include "battery.h"
#include "pin_config.h"

#include <esp_log.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <algorithm>

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static adc_channel_t battery_channel = ADC_CHANNEL_0;

Battery& Battery::GetInstance()
{
    static Battery instance;
    return instance;
}

esp_err_t Battery::Init(int adc_pin)
{
    adc_pin_ = adc_pin;

    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(adc_pin_, &unit, &battery_channel));

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = unit;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, battery_channel, &chan_cfg));

    // Try to create calibration handle
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = battery_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration not available, using raw values");
        adc_cali_handle = NULL;
    }

    ESP_LOGI(TAG, "Battery ADC initialized (GPIO%d, ADC%d channel %d)",
             adc_pin_, static_cast<int>(unit) + 1, static_cast<int>(battery_channel));
    return ESP_OK;
}

uint32_t Battery::ReadRaw()
{
    int raw = 0;
    if (adc_oneshot_read(adc_handle, battery_channel, &raw) != ESP_OK) return 0;

    if (adc_cali_handle) {
        int voltage = 0;
        if (adc_cali_raw_to_voltage(adc_cali_handle, raw, &voltage) != ESP_OK) return 0;
        return voltage * 2; // Assuming 1:1 voltage divider
    }

    // Rough conversion without calibration
    return (raw * 3300 * 2) / 4095;
}

uint8_t Battery::GetPercentage() const
{
    return percentage_;
}

BatterySnapshot Battery::GetSnapshot() const
{
    // Read sequence before/after the fields so the application never combines
    // values from two ADC updates.
    BatterySnapshot snapshot;
    uint32_t before;
    do {
        before = snapshot_sequence_;
        if (before & 1U) continue;
        snapshot.voltage_mv = voltage_mv_;
        snapshot.percentage = percentage_;
        snapshot.valid = snapshot_valid_;
        snapshot.low_confirmed = low_confirmed_;
        snapshot.sequence = before;
    } while (before != snapshot_sequence_ || (snapshot_sequence_ & 1U));
    return snapshot;
}

void Battery::StartMonitoring()
{
    // ADC calibration and median filtering exceed the old 2KB stack. Keep this
    // non-DMA task in PSRAM so Wi-Fi retains scarce internal RAM.
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        MonitorTask, "battery", 6144, this, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create battery monitor task");
    }
}

void Battery::MonitorTask(void* arg)
{
    auto* self = static_cast<Battery*>(arg);
    ESP_LOGI(TAG, "Battery monitor started (stack free: %u bytes)",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    uint32_t invalid_samples = 0;
    while (true) {
        uint32_t reading = self->ReadRaw();

        if (reading < 2500 || reading > 5000) {
            ++invalid_samples;
            if (invalid_samples == 1 || invalid_samples % 30 == 0) {
                ESP_LOGW(TAG, "Ignoring invalid battery ADC reading: %lumV (%lu consecutive); check GPIO%d and voltage divider",
                         static_cast<unsigned long>(reading),
                         static_cast<unsigned long>(invalid_samples), self->adc_pin_);
            }
            self->low_sample_count_ = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        invalid_samples = 0;

        if (self->warmup_remaining_ > 0) {
            self->warmup_remaining_--;
            ESP_LOGI(TAG, "Battery ADC warm-up: %d sample(s) remaining",
                     self->warmup_remaining_);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Moving average filter
        self->filter_buf_[self->filter_idx_] = reading;
        self->filter_idx_ = (self->filter_idx_ + 1) % FILTER_SIZE;
        if (self->filter_idx_ == 0) self->filter_filled_ = true;

        if (!self->filter_filled_) {
            ESP_LOGI(TAG, "Battery median window: %d/%d", self->filter_idx_,
                     FILTER_SIZE);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t sorted[FILTER_SIZE];
        std::copy_n(self->filter_buf_, FILTER_SIZE, sorted);
        std::sort(sorted, sorted + FILTER_SIZE);
        uint32_t filtered_mv = sorted[FILTER_SIZE / 2];
        uint8_t percent = filtered_mv >= 4200
                              ? 100
                              : filtered_mv <= 3000
                                    ? 0
                                    : static_cast<uint8_t>(
                                          (filtered_mv - 3000) * 100 / 1200);

        if (percent <= 20) {
            if (self->low_sample_count_ < 3) self->low_sample_count_++;
        } else {
            self->low_sample_count_ = 0;
        }

        // Odd sequence means a write is in progress; even means stable.
        self->snapshot_sequence_ = self->snapshot_sequence_ + 1;
        self->voltage_mv_ = filtered_mv;
        self->percentage_ = percent;
        self->low_confirmed_ = self->low_sample_count_ >= 3;
        self->snapshot_valid_ = true;
        self->snapshot_sequence_ = self->snapshot_sequence_ + 1;

        ESP_LOGI(TAG, "Battery snapshot #%lu: %lumV, %u%%, low=%d (%u/3)",
                 static_cast<unsigned long>(self->snapshot_sequence_ / 2),
                 static_cast<unsigned long>(filtered_mv), percent,
                 self->low_confirmed_, self->low_sample_count_);
        ESP_LOGD(TAG, "Battery task stack free: %u bytes",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
