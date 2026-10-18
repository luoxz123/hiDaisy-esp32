#include "reminder_manager.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <algorithm>

static const char *TAG = "reminder";
static const char *NVS_NAMESPACE = "reminders";
static const char *NVS_KEY = "entries";

ReminderManager& ReminderManager::GetInstance()
{
    static ReminderManager instance;
    return instance;
}

esp_err_t ReminderManager::Init()
{
    esp_err_t ret = LoadFromNvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved reminders found");
    } else {
        ESP_LOGI(TAG, "Loaded %d reminders from NVS", (int)reminders_.size());
    }
    return ESP_OK;
}

void ReminderManager::Start()
{
    xTaskCreatePinnedToCore(ScanTask, "reminder", 4096, this, 1, NULL, 0);
    ESP_LOGI(TAG, "Reminder scan task started");
}

esp_err_t ReminderManager::AddReminder(const char* reminder_id, int64_t timestamp, const char* content)
{
    if ((int)reminders_.size() >= MAX_REMINDERS) {
        ESP_LOGW(TAG, "Max reminders reached (%d)", MAX_REMINDERS);
        return ESP_ERR_NO_MEM;
    }

    // Check duplicate
    for (auto& r : reminders_) {
        if (strcmp(r.reminder_id, reminder_id) == 0) {
            r.timestamp = timestamp;
            r.triggered = false;
            strncpy(r.content, content ? content : "", sizeof(r.content) - 1);
            r.content[sizeof(r.content) - 1] = '\0';
            ESP_LOGI(TAG, "Updated reminder: %s", reminder_id);
            return SaveToNvs();
        }
    }

    ReminderEntry entry = {};
    strncpy(entry.reminder_id, reminder_id, sizeof(entry.reminder_id) - 1);
    strncpy(entry.content, content ? content : "", sizeof(entry.content) - 1);
    entry.timestamp = timestamp;
    entry.triggered = false;

    reminders_.push_back(entry);
    ESP_LOGI(TAG, "Added reminder: %s at %lld", reminder_id, timestamp);

    return SaveToNvs();
}

esp_err_t ReminderManager::CancelReminder(const char* reminder_id)
{
    auto it = std::remove_if(reminders_.begin(), reminders_.end(),
        [reminder_id](const ReminderEntry& r) {
            return strcmp(r.reminder_id, reminder_id) == 0;
        });

    if (it != reminders_.end()) {
        reminders_.erase(it, reminders_.end());
        ESP_LOGI(TAG, "Cancelled reminder: %s", reminder_id);
        return SaveToNvs();
    }

    ESP_LOGW(TAG, "Reminder not found: %s", reminder_id);
    return ESP_ERR_NOT_FOUND;
}

int64_t ReminderManager::GetNextReminderTimestamp() const
{
    int64_t next = INT64_MAX;
    for (auto& r : reminders_) {
        if (!r.triggered && r.timestamp < next) {
            next = r.timestamp;
        }
    }
    return next;
}

void ReminderManager::ScanTask(void* arg)
{
    auto* self = static_cast<ReminderManager*>(arg);

    while (true) {
        time_t now = time(NULL);
        if (now < 1000000000) {
            // Time not synced yet
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        for (auto& r : self->reminders_) {
            if (!r.triggered && now >= r.timestamp) {
                r.triggered = true;
                ESP_LOGI(TAG, "REMINDER TRIGGERED: %s", r.reminder_id);

                if (self->trigger_callback_) {
                    self->trigger_callback_(std::string(r.reminder_id));
                }

                self->SaveToNvs();
            }
        }

        // Clean up old triggered reminders
        auto it = std::remove_if(self->reminders_.begin(), self->reminders_.end(),
            [now](const ReminderEntry& r) {
                return r.triggered && (now - r.timestamp > 3600); // Remove after 1 hour
            });
        if (it != self->reminders_.end()) {
            self->reminders_.erase(it, self->reminders_.end());
            self->SaveToNvs();
        }

        vTaskDelay(pdMS_TO_TICKS(60000)); // Scan every minute
    }
}

esp_err_t ReminderManager::LoadFromNvs()
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;

    size_t size = 0;
    ret = nvs_get_blob(handle, NVS_KEY, NULL, &size);
    if (ret != ESP_OK || size == 0) {
        nvs_close(handle);
        return ret;
    }

    int count = size / sizeof(ReminderEntry);
    reminders_.resize(count);
    ret = nvs_get_blob(handle, NVS_KEY, reminders_.data(), &size);
    nvs_close(handle);

    return ret;
}

esp_err_t ReminderManager::SaveToNvs()
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    if (reminders_.empty()) {
        nvs_erase_key(handle, NVS_KEY);
    } else {
        ret = nvs_set_blob(handle, NVS_KEY, reminders_.data(),
                           reminders_.size() * sizeof(ReminderEntry));
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}
