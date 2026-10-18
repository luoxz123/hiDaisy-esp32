#include "audio_test.h"
#include "audio_service.h"
#include "pin_config.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <math.h>

static const char *TAG = "audio_test";
static constexpr int MIC_SHIFT = 12;  // Same as main audio: INMP441 32-bit → 16-bit

AudioTest& AudioTest::GetInstance() {
    static AudioTest instance;
    return instance;
}

void AudioTest::Init(i2s_chan_handle_t mic, i2s_chan_handle_t spk, gpio_num_t button_gpio) {
    mic_handle_ = mic;
    spk_handle_ = spk;
    button_gpio_ = button_gpio;

    // Configure button GPIO as input with pull-up
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << button_gpio_,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    // Allocate recording buffer in PSRAM
    record_buf_ = (int16_t*)heap_caps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!record_buf_) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer in PSRAM");
        return;
    }

    ESP_LOGI(TAG, "Audio test initialized (button=GPIO%d, buffer=%dKB in PSRAM)",
             button_gpio_, MAX_RECORD_SAMPLES * 2 / 1024);
}

void AudioTest::Start() {
    if (!record_buf_) return;
    xTaskCreatePinnedToCore(TaskFunc, "audio_test", 4096, this, 5, NULL, 0);
}

// Convert 16-bit mono to 32-bit mono for I2S speaker output
static void Mono16ToMono32(const int16_t *src, int32_t *dst, int samples) {
    for (int i = 0; i < samples; i++) {
        dst[i] = (int32_t)src[i] << 16;
    }
}

void AudioTest::TaskFunc(void* arg) {
    auto* self = static_cast<AudioTest*>(arg);
    bool was_pressed = false;
    int32_t raw_buf[160];   // 10ms at 16kHz
    int32_t out_buf[160];   // 32-bit mono output

    ESP_LOGI(TAG, "Audio test task started — hold VOL_DOWN to record, release to play");

    while (true) {
        bool pressed = (gpio_get_level(self->button_gpio_) == 0);

        if (pressed && !was_pressed) {
            // Button just pressed: start recording
            self->record_len_ = 0;
            // Take exclusive mic access (pauses CaptureTask)
            AudioService::GetInstance().SetMicExclusive(true);
            ESP_LOGI(TAG, "Recording started (mic exclusive mode)...");
        }

        if (pressed) {
            // Recording: read from mic
            if (self->record_len_ < self->MAX_RECORD_SAMPLES) {
                size_t bytes_read;
                esp_err_t ret = i2s_channel_read(self->mic_handle_, raw_buf,
                    sizeof(raw_buf), &bytes_read, pdMS_TO_TICKS(100));
                if (ret == ESP_OK && bytes_read > 0) {
                    int samples = bytes_read / sizeof(int32_t);
                    int space = self->MAX_RECORD_SAMPLES - self->record_len_;
                    if (samples > space) samples = space;

                    for (int i = 0; i < samples; i++) {
                        int32_t value = raw_buf[i] >> MIC_SHIFT;
                        if (value > INT16_MAX) value = INT16_MAX;
                        else if (value < INT16_MIN) value = INT16_MIN;
                        self->record_buf_[self->record_len_ + i] = (int16_t)value;
                    }
                    self->record_len_ += samples;
                }
            }
        }

        if (!pressed && was_pressed) {
            // Button just released: stop recording, start playback
            AudioService::GetInstance().SetMicExclusive(false);
            float duration = self->record_len_ / 16000.0f;
            ESP_LOGI(TAG, "Recording stopped: %d samples (%.2fs)", self->record_len_, duration);

            if (self->record_len_ > 0) {
                // Compute RMS and peak for diagnostics
                int64_t sum_sq = 0;
                int16_t peak = 0;
                for (int i = 0; i < self->record_len_; i++) {
                    int16_t s = self->record_buf_[i];
                    sum_sq += (int64_t)s * s;
                    if (abs(s) > peak) peak = abs(s);
                }
                float rms = sqrtf((float)sum_sq / self->record_len_);
                ESP_LOGI(TAG, "Audio stats: RMS=%.0f, Peak=%d, Clipped=%s",
                         rms, peak, peak >= 32767 ? "YES" : "no");

                // Play back through speaker (16kHz mono, raw speed test)
                ESP_LOGI(TAG, "Playing back (%dHz I2S, %d samples = %.2fs)...",
                         AUDIO_OUTPUT_SAMPLE_RATE, self->record_len_, duration);
                i2s_channel_enable(self->spk_handle_);

                // Write silence to flush DMA (20ms at output rate)
                constexpr int SILENCE_SAMPLES = AUDIO_OUTPUT_SAMPLE_RATE / 50; // 20ms
                static int32_t silence[SILENCE_SAMPLES] = {};
                size_t written;
                i2s_channel_write(self->spk_handle_, silence, sizeof(silence),
                                  &written, portMAX_DELAY);

                int offset = 0;
                while (offset < self->record_len_) {
                    int chunk = self->record_len_ - offset;
                    if (chunk > 160) chunk = 160;

                    Mono16ToMono32(self->record_buf_ + offset, out_buf, chunk);
                    i2s_channel_write(self->spk_handle_, out_buf,
                                      chunk * sizeof(int32_t), &written,
                                      portMAX_DELAY);
                    offset += chunk;
                }

                // Wait for DMA to drain
                vTaskDelay(pdMS_TO_TICKS(200));
                i2s_channel_disable(self->spk_handle_);
                ESP_LOGI(TAG, "Playback complete");
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
