#include "wake_word.h"

#include <esp_log.h>
#include <cstring>

// ESP-SR is an optional dependency. Guard all AFE usage.
#if __has_include("esp_afe_sr_models.h")
#include <esp_afe_sr_models.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#define HAS_ESP_SR 1
#else
#define HAS_ESP_SR 0
#endif

static const char *TAG = "wake";

// WakeNet model: built-in "Hi, Lily" wake word
static const char *WAKENET_MODEL_NAME = "wn9_hilili_tts";
// Detection threshold: lower = more tolerant (range: 0.4 ~ 0.9999)
static const float WAKE_DET_THRESHOLD = 0.5f;

WakeWordDetector& WakeWordDetector::GetInstance()
{
    static WakeWordDetector instance;
    return instance;
}

esp_err_t WakeWordDetector::Init()
{
#if HAS_ESP_SR
    // Load models from partition
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num == -1) {
        ESP_LOGE(TAG, "Failed to load SR models from partition");
        return ESP_FAIL;
    }

    // Find wake word model
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (!wn_name) {
        ESP_LOGW(TAG, "No WakeNet model found, using default: %s", WAKENET_MODEL_NAME);
        wn_name = (char *)WAKENET_MODEL_NAME;
    }
    ESP_LOGI(TAG, "Using WakeNet model: %s", wn_name);

    // Initialize AFE with model list
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->vad_init = true;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 5;
    afe_config->pcm_config.total_ch_num = 1;
    afe_config->pcm_config.mic_num = 1;
    afe_config->pcm_config.ref_num = 0;
    afe_config->pcm_config.sample_rate = 16000;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (!afe_iface_) {
        ESP_LOGE(TAG, "Failed to get AFE interface");
        return ESP_FAIL;
    }

    afe_data_ = afe_iface_->create_from_config(afe_config);
    if (!afe_data_) {
        ESP_LOGE(TAG, "Failed to create AFE instance");
        return ESP_FAIL;
    }

    // Lower the detection threshold for higher tolerance
    // DET_MODE is set via afe_config (default), but we further relax via set_det_threshold
    esp_afe_sr_iface_t *sr_iface = (esp_afe_sr_iface_t *)afe_iface_;
    if (sr_iface->set_wakenet_threshold) {
        sr_iface->set_wakenet_threshold(afe_data_, WAKE_DET_THRESHOLD, 1);
        ESP_LOGI(TAG, "Wake word threshold set to %.2f (more tolerant)", WAKE_DET_THRESHOLD);
    }

    feed_chunk_size_ = afe_iface_->get_feed_chunksize(afe_data_);
    feed_buf_ = (int16_t*)heap_caps_calloc(feed_chunk_size_, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!feed_buf_) {
        feed_buf_ = (int16_t*)calloc(feed_chunk_size_, sizeof(int16_t));
    }
    if (!feed_buf_) {
        ESP_LOGE(TAG, "Failed to allocate feed buffer");
        return ESP_FAIL;
    }
    feed_buf_pos_ = 0;

    ESP_LOGI(TAG, "Wake word detector initialized (model: %s, threshold: %.2f, chunk: %d)",
             wn_name, WAKE_DET_THRESHOLD, feed_chunk_size_);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "ESP-SR not available, wake word detection disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void WakeWordDetector::Start()
{
    if (running_) return;
#if HAS_ESP_SR
    running_ = true;
    xTaskCreatePinnedToCore(DetectTask, "wake", 4096, this, 8, NULL, 1);
    ESP_LOGI(TAG, "Wake word detection started");
#else
    ESP_LOGW(TAG, "ESP-SR not available, cannot start wake word detection");
#endif
}

void WakeWordDetector::Stop()
{
    running_ = false;
#if HAS_ESP_SR
    if (afe_iface_ && afe_data_) {
        afe_iface_->reset_buffer(afe_data_);
    }
#endif
    feed_buf_pos_ = 0;
    ESP_LOGI(TAG, "Wake word detection stopped");
}

void WakeWordDetector::Feed(const int16_t* samples, int count)
{
#if HAS_ESP_SR
    if (!running_ || !feed_buf_) return;

    int pos = 0;
    while (pos < count) {
        int to_copy = std::min(count - pos, feed_chunk_size_ - feed_buf_pos_);
        memcpy(feed_buf_ + feed_buf_pos_, samples + pos, to_copy * sizeof(int16_t));
        feed_buf_pos_ += to_copy;
        pos += to_copy;

        if (feed_buf_pos_ >= feed_chunk_size_) {
            if (afe_iface_ && afe_data_) {
                afe_iface_->feed(afe_data_, feed_buf_);
            }
            feed_buf_pos_ = 0;
        }
    }
#endif
}

void WakeWordDetector::DetectTask(void* arg)
{
#if HAS_ESP_SR
    auto* self = static_cast<WakeWordDetector*>(arg);
    ESP_LOGI(TAG, "Detect task started (Core %d)", xPortGetCoreID());

    while (self->running_) {
        if (!self->afe_iface_ || !self->afe_data_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        afe_fetch_result_t* result = self->afe_iface_->fetch(self->afe_data_);
        if (!result) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected!");
            if (self->wake_callback_) {
                self->wake_callback_();
            }
        }
    }
#endif
    vTaskDelete(NULL);
}
