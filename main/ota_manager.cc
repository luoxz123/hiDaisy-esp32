#include "ota_manager.h"
#include "display_manager.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_app_format.h>
#include <cJSON.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

static const char *TAG = "ota";
static const int OTA_BUF_SIZE = 4096;

// Firmware version — update with each release
static const char *FIRMWARE_VERSION = "1.2.0";

OtaManager& OtaManager::GetInstance() {
    static OtaManager instance;
    return instance;
}

void OtaManager::MarkValid() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Firmware marked valid, rollback cancelled");
        }
    }
    ESP_LOGI(TAG, "Running from partition '%s' (v%s)", running->label, FIRMWARE_VERSION);
}

const char* OtaManager::GetVersion() const {
    return FIRMWARE_VERSION;
}

void OtaManager::SetServerUrl(const std::string& url) {
    server_url_ = url;
}

void OtaManager::SetDeviceId(const std::string& id) {
    device_id_ = id;
}

// Semantic version comparison: "1.2.0" vs "1.1.0" → returns >0
int OtaManager::CompareVersions(const char* a, const char* b) {
    int a_major = 0, a_minor = 0, a_patch = 0;
    int b_major = 0, b_minor = 0, b_patch = 0;
    sscanf(a, "%d.%d.%d", &a_major, &a_minor, &a_patch);
    sscanf(b, "%d.%d.%d", &b_major, &b_minor, &b_patch);
    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

esp_err_t OtaManager::CheckForUpdate() {
    if (updating_) {
        ESP_LOGW(TAG, "OTA already in progress, skip check");
        return ESP_ERR_INVALID_STATE;
    }
    if (server_url_.empty() || device_id_.empty()) {
        ESP_LOGW(TAG, "Server URL or device ID not set, skip OTA check");
        return ESP_ERR_INVALID_ARG;
    }

    // Build check URL: GET /api/v1/ota/check?device_id=XX&current_version=YY
    char check_url[256];
    snprintf(check_url, sizeof(check_url),
             "%s/api/v1/ota/check?device_id=%s&current_version=%s",
             server_url_.c_str(), device_id_.c_str(), FIRMWARE_VERSION);

    ESP_LOGI(TAG, "OTA check: %s", check_url);

    esp_http_client_config_t config = {};
    config.url = check_url;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200 || content_length <= 0 || content_length > 2048) {
        ESP_LOGI(TAG, "OTA check: no update (status=%d, len=%d)", status_code, content_length);
        esp_http_client_cleanup(client);
        return ESP_OK; // no update is not an error
    }

    char *buf = (char *)malloc(content_length + 1);
    if (!buf) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read(client, buf, content_length);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[read_len] = '\0';

    // Parse JSON: {has_update, version, url, size, md5}
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "OTA check: JSON parse failed");
        return ESP_FAIL;
    }

    cJSON *has_update = cJSON_GetObjectItem(root, "has_update");
    if (!cJSON_IsTrue(has_update)) {
        ESP_LOGI(TAG, "OTA check: current=%s, no update available", FIRMWARE_VERSION);
        cJSON_Delete(root);
        return ESP_OK;
    }

    const char *new_version = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    const char *url_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));

    if (!new_version || !url_str) {
        ESP_LOGE(TAG, "OTA check: missing version or url in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA check: current=%s, latest=%s", FIRMWARE_VERSION, new_version);

    if (CompareVersions(new_version, FIRMWARE_VERSION) <= 0) {
        ESP_LOGI(TAG, "OTA check: already up to date");
        cJSON_Delete(root);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA update available: %s → %s", FIRMWARE_VERSION, new_version);

    // Build full URL if server returned a relative path
    std::string ota_url(url_str);
    if (ota_url[0] == '/') {
        ota_url = server_url_ + ota_url;
    }
    std::string ota_version(new_version);
    cJSON_Delete(root);

    // Run update in a separate task
    struct OtaParams {
        std::string url;
        std::string version;
    };
    auto *params = new OtaParams{ota_url, ota_version};

    xTaskCreate([](void *arg) {
        auto *p = static_cast<OtaParams *>(arg);
        OtaManager::GetInstance().StartUpdate(p->url, p->version);
        delete p;
        vTaskDelete(NULL);
    }, "ota_task", 8192, params, 5, NULL);

    return ESP_OK;
}

esp_err_t OtaManager::StartUpdate(const std::string& url, const std::string& version) {
    if (updating_) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    updating_ = true;

    const char *ver = version.empty() ? "unknown" : version.c_str();
    ESP_LOGI(TAG, "Starting OTA: v%s → v%s from: %s", FIRMWARE_VERSION, ver, url.c_str());

    // Show initial progress on screen
    char overlay_msg[128];
    snprintf(overlay_msg, sizeof(overlay_msg), "Updating to v%s\n\nDownloading... 0%%", ver);
    DisplayManager::GetInstance().ShowOverlay(overlay_msg, 300000); // 5 min timeout

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= MAX_RETRY; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "OTA retry %d/%d", attempt, MAX_RETRY);
            snprintf(overlay_msg, sizeof(overlay_msg),
                     "Update retry %d/%d\n\nDownloading... 0%%", attempt, MAX_RETRY);
            DisplayManager::GetInstance().ShowOverlay(overlay_msg, 300000);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        ret = DoDownload(url, ver);
        if (ret == ESP_OK) break;

        ESP_LOGE(TAG, "OTA attempt %d failed: %s", attempt, esp_err_to_name(ret));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed after %d attempts", MAX_RETRY);
        snprintf(overlay_msg, sizeof(overlay_msg), "Update failed!\n\nResuming...");
        DisplayManager::GetInstance().ShowOverlay(overlay_msg, 5000);
        updating_ = false;
        return ret;
    }

    snprintf(overlay_msg, sizeof(overlay_msg), "Update complete!\n\nRestarting...");
    DisplayManager::GetInstance().ShowOverlay(overlay_msg, 10000);
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "OTA complete, restarting...");
    esp_restart();
    return ESP_OK; // unreachable
}

void OtaManager::StartPeriodicCheck() {
    if (check_timer_) return; // already started

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = PeriodicCheckCallback;
    timer_args.arg = this;
    timer_args.name = "ota_check";

    esp_err_t ret = esp_timer_create(&timer_args, &check_timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create OTA check timer: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_timer_start_periodic(check_timer_, (uint64_t)CHECK_INTERVAL_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA check timer: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Periodic OTA check started (every %d min)", CHECK_INTERVAL_MS / 60000);
}

void OtaManager::PeriodicCheckCallback(void* arg) {
    // Schedule check in a task (don't block timer task)
    xTaskCreate([](void *) {
        OtaManager::GetInstance().CheckForUpdate();
        vTaskDelete(NULL);
    }, "ota_periodic", 4096, NULL, 3, NULL);
}

// Internal: download firmware and write to OTA partition
esp_err_t OtaManager::DoDownload(const std::string& url, const char* ver) {
    esp_http_client_config_t http_config = {};
    http_config.url = url.c_str();
    http_config.timeout_ms = 30000;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200 || content_length <= 0) {
        ESP_LOGE(TAG, "Bad response: status=%d, len=%d", status_code, content_length);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition available");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition '%s' (%d bytes)", update_partition->label, content_length);

    esp_ota_handle_t ota_handle;
    ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    char *buf = (char *)malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int last_progress = -1;
    while (true) {
        int read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) break;

        ret = esp_ota_write(ota_handle, buf, read_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(ret));
            break;
        }

        total_read += read_len;
        int progress = (total_read * 100) / content_length;
        if (progress / 5 != last_progress / 5) { // update every 5%
            ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d)", progress, total_read, content_length);
            char overlay_msg[128];
            snprintf(overlay_msg, sizeof(overlay_msg),
                     "Updating to v%s\n\nDownloading... %d%%", ver, progress);
            DisplayManager::GetInstance().ShowOverlay(overlay_msg, 300000);
            last_progress = progress;
        }
    }

    free(buf);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        esp_ota_abort(ota_handle);
        return ret;
    }

    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
