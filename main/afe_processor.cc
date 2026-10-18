#include "afe_processor.h"
#include "micro_wake_word.h"

#include <cstring>
#include <cmath>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#if HAS_ESP_SR
#include <esp_afe_sr_models.h>
#endif

static const char* TAG = "afe";

AfeProcessor& AfeProcessor::GetInstance() {
    static AfeProcessor instance;
    return instance;
}

esp_err_t AfeProcessor::Init() {
#if !HAS_ESP_SR
    ESP_LOGW(TAG, "ESP-SR not available, AFE disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    // Load SR models from SPIFFS partition
    srmodel_list_t* models = esp_srmodel_init("model");
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "Failed to load SR models from 'model' partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Loaded %d SR model(s)", models->num);

    // --- AFE Configuration (VC mode: AEC + NS, no VAD) ---
    // "MR" = 1 Mic + 1 Reference (software reference from speaker output)
    afe_config_t* cfg = afe_config_init("MR", models, AFE_TYPE_VC, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    // AEC enabled — software reference channel from speaker playback
    // Use LOW_COST mode to reduce internal RAM usage (HIGH_PERF causes WiFi OOM)
    cfg->aec_init = true;
    cfg->aec_mode = AEC_MODE_VOIP_LOW_COST;
    cfg->se_init  = false;

    // Noise Suppression: prefer NSNet2, fall back to WebRTC
    char* ns_model = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    if (ns_model) {
        cfg->ns_init       = true;
        cfg->ns_model_name = ns_model;
        cfg->afe_ns_mode   = AFE_NS_MODE_NET;
        ESP_LOGI(TAG, "NS model: %s (NSNet)", ns_model);
    } else {
        cfg->ns_init     = true;
        cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
        ESP_LOGW(TAG, "NSNet model not found, using WebRTC NS");
    }

    // VAD disabled — server-side Silero VAD handles speech detection
    cfg->vad_init = false;

    // WakeNet stays disabled; microWakeWord consumes the AEC/NS-cleaned output.
    cfg->wakenet_init = false;
    cfg->agc_init     = false;

    // Use PSRAM to avoid starving WiFi of internal RAM
    cfg->memory_alloc_mode    = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core   = 1;
    cfg->afe_perferred_priority = 5;
    cfg->pcm_config.total_ch_num = 2;   // 1 mic + 1 reference
    cfg->pcm_config.mic_num      = 1;
    cfg->pcm_config.ref_num      = 1;
    cfg->pcm_config.sample_rate  = 16000;

    // Create AFE handle + instance
    afe_iface_ = esp_afe_handle_from_config(cfg);
    if (!afe_iface_) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        return ESP_FAIL;
    }

    afe_data_ = afe_iface_->create_from_config(cfg);
    if (!afe_data_) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    // Allocate interleaved feed buffer: [mic0, ref0, mic1, ref1, ...]
    feed_chunk_size_ = afe_iface_->get_feed_chunksize(afe_data_);
    int interleaved_size = feed_chunk_size_ * kInputChannels;
    feed_buf_ = (int16_t*)heap_caps_calloc(interleaved_size, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!feed_buf_) {
        feed_buf_ = (int16_t*)calloc(interleaved_size, sizeof(int16_t));
    }
    if (!feed_buf_) {
        ESP_LOGE(TAG, "Failed to allocate feed buffer (%d interleaved samples)", interleaved_size);
        return ESP_FAIL;
    }
    feed_buf_pos_ = 0;
    ESP_LOGI(TAG, "AFE feed chunk: %d samples/ch × %d ch = %d total",
             feed_chunk_size_, kInputChannels, interleaved_size);

    ESP_LOGI(TAG, "AFE processor initialized (AEC=VOIP_LOW_COST, NS=%s, VAD=off/server, WakeNet=off, microWakeWord=external)",
             ns_model ? "NSNet" : "WebRTC");
    return ESP_OK;
#endif // HAS_ESP_SR
}

void AfeProcessor::Start() {
    if (running_) return;
#if HAS_ESP_SR
    if (!afe_iface_ || !afe_data_) {
        ESP_LOGE(TAG, "Cannot start: AFE not initialized");
        return;
    }
    running_ = true;
    // Process task: fetch denoised audio and forward it to consumers.
    xTaskCreatePinnedToCoreWithCaps(
        ProcessTask, "afe_proc", 8192, this, 8, NULL, 1, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "AFE processor started (Core 1, 8KB stack)");
#else
    ESP_LOGW(TAG, "ESP-SR not available");
#endif
}

void AfeProcessor::Stop() {
    if (!running_) return;
    running_ = false;
#if HAS_ESP_SR
    if (afe_iface_ && afe_data_) {
        afe_iface_->reset_buffer(afe_data_);
    }
#endif
    feed_buf_pos_ = 0;
    is_speaking_ = false;
    ESP_LOGI(TAG, "AFE processor stopped");
}

void AfeProcessor::Feed(const int16_t* mic, const int16_t* ref, int count) {
#if HAS_ESP_SR
    if (!running_ || !feed_buf_ || !afe_iface_ || !afe_data_) return;

    // Interleave mic + ref into feed buffer: [mic0, ref0, mic1, ref1, ...]
    // feed_buf_pos_ tracks per-channel position (not interleaved position)
    int pos = 0;
    while (pos < count) {
        int remaining = feed_chunk_size_ - feed_buf_pos_;
        int to_copy = count - pos;
        if (to_copy > remaining) to_copy = remaining;

        int base = feed_buf_pos_ * kInputChannels;
        for (int i = 0; i < to_copy; i++) {
            feed_buf_[base + i * kInputChannels + 0] = mic[pos + i];
            feed_buf_[base + i * kInputChannels + 1] = ref ? ref[pos + i] : 0;
        }
        feed_buf_pos_ += to_copy;
        pos += to_copy;

        if (feed_buf_pos_ >= feed_chunk_size_) {
            afe_iface_->feed(afe_data_, feed_buf_);
            feed_buf_pos_ = 0;
        }
    }
#endif
}

void AfeProcessor::ProcessTask(void* arg) {
#if HAS_ESP_SR
    auto* self = static_cast<AfeProcessor*>(arg);
    ESP_LOGI(TAG, "Process task started (Core %d)", xPortGetCoreID());

    while (self->running_) {
        if (!self->afe_iface_ || !self->afe_data_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Fetch denoised audio from AFE (blocks until data available)
        afe_fetch_result_t* res = self->afe_iface_->fetch(self->afe_data_);
        if (!res || res->data_size <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int samples = res->data_size / sizeof(int16_t);

        // --- RMS-based speech detection (local fallback) ---
        // AFE VAD is disabled (server-side Silero VAD is authoritative).
        // We provide a simple RMS energy check as a local auto-stop fallback.
        // AEC-cleaned audio: speaker echo removed → RMS mainly reflects user speech.
        {
            int64_t sum_sq = 0;
            for (int i = 0; i < samples; i++) {
                int32_t s = res->data[i];
                sum_sq += s * s;
            }
            uint32_t rms = (samples > 0) ? (uint32_t)sqrt((double)sum_sq / samples) : 0;
            // After AEC + NS, speech typically > 300 RMS, noise < 150
            static constexpr uint32_t kRmsSpeechThreshold = 200;
            bool is_speech = (rms >= kRmsSpeechThreshold);

            if (is_speech && !self->is_speaking_) {
                self->is_speaking_ = true;
                if (self->vad_cb_) self->vad_cb_(true);
            } else if (!is_speech && self->is_speaking_) {
                self->is_speaking_ = false;
                if (self->vad_cb_) self->vad_cb_(false);
            }
        }

        // --- Output AEC-cleaned + denoised audio ---
        if (self->audio_cb_) {
            self->audio_cb_(res->data, samples);
        }

        // Queue AEC-cleaned PCM for local Daisy inference. Feed() never runs
        // TFLite on this latency-sensitive AFE task.
        MicroWakeWordDetector::GetInstance().Feed(res->data, samples);
    }
#endif
    ESP_LOGI(TAG, "Process task exiting");
    vTaskDelete(NULL);
}
