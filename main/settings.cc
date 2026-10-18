#include "settings.h"

#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <algorithm>

static const char *TAG = "settings";

Settings& Settings::GetInstance() {
    static Settings instance;
    return instance;
}

esp_err_t Settings::Init() {
    ESP_LOGI(TAG, "Settings initialized (volume=%d, brightness=%d, tz=%d)",
             GetVolume(), GetBrightness(), GetTimezoneOffset());
    return ESP_OK;
}

int Settings::GetVolume() const {
    return GetInt("audio", "volume", kDefaultVolume);
}

void Settings::SetVolume(int volume) {
    volume = std::max(0, std::min(100, volume));
    SetInt("audio", "volume", volume);
    ESP_LOGI(TAG, "Volume set to %d", volume);
}

int Settings::GetBrightness() const {
    return GetInt("display", "brightness", kDefaultBrightness);
}

void Settings::SetBrightness(int brightness) {
    brightness = std::max(0, std::min(100, brightness));
    SetInt("display", "brightness", brightness);
    ESP_LOGI(TAG, "Brightness set to %d", brightness);
}

int Settings::GetTimezoneOffset() const {
    return GetInt("system", "tz_offset", kDefaultTimezoneOffset);
}

void Settings::SetTimezoneOffset(int offset_minutes) {
    offset_minutes = std::max(-720, std::min(840, offset_minutes));
    SetInt("system", "tz_offset", offset_minutes);
    ESP_LOGI(TAG, "Timezone offset set to %d minutes", offset_minutes);
}

int Settings::GetInt(const char* ns, const char* key, int default_val) const {
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) {
        return default_val;
    }
    int32_t value;
    if (nvs_get_i32(handle, key, &value) != ESP_OK) {
        nvs_close(handle);
        return default_val;
    }
    nvs_close(handle);
    return (int)value;
}

void Settings::SetInt(const char* ns, const char* key, int value) {
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s/%s", ns, key);
        return;
    }
    nvs_set_i32(handle, key, (int32_t)value);
    nvs_commit(handle);
    nvs_close(handle);
}
