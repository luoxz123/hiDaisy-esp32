/*
 * QEMU mock audio service.
 * Replaces I2S hardware with virtual buffers.
 * CaptureTask reads from a test PCM buffer (sine wave).
 * PlaybackTask discards data (logs stats).
 * Opus encoder/decoder still works (pure software).
 */
#include "audio_service.h"
#include "hilily_mqtt_client.h"
#include "pin_config.h"
#include "prompt_tone.h"
#include "settings.h"
#include "udp_client.h"

#include <cmath>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <opus.h>

static const char *TAG = "audio_qemu";

static constexpr int OPUS_SAMPLE_RATE = 16000;
static constexpr int OPUS_CHANNELS = 1;
static constexpr int OPUS_FRAME_MS = 60;
static constexpr int OPUS_FRAME_SAMPLES = OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000;
static constexpr int OPUS_MAX_PACKET = 1500;

// Virtual test audio: 1-second 440Hz sine wave at 16kHz, 16-bit
static int16_t test_audio_buf[AUDIO_SAMPLE_RATE];
static int test_audio_pos = 0;
static bool test_audio_generated = false;

static void GenerateTestAudio() {
    if (test_audio_generated) return;
    for (int i = 0; i < AUDIO_SAMPLE_RATE; i++) {
        float t = (float)i / AUDIO_SAMPLE_RATE;
        test_audio_buf[i] = (int16_t)(16000.0f * sinf(2.0f * M_PI * 440.0f * t));
    }
    test_audio_generated = true;
    ESP_LOGI(TAG, "[QEMU] Generated 1s test audio (440Hz sine)");
}

AudioService& AudioService::GetInstance() {
    static AudioService instance;
    return instance;
}

esp_err_t AudioService::InitOpus() {
    int err;
    opus_encoder_ = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS,
                                        OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !opus_encoder_) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %d", err);
        return ESP_FAIL;
    }
    opus_encoder_ctl((OpusEncoder*)opus_encoder_, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl((OpusEncoder*)opus_encoder_, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl((OpusEncoder*)opus_encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    opus_decoder_ = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, &err);
    if (err != OPUS_OK || !opus_decoder_) {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[QEMU] Opus codec initialized");
    return ESP_OK;
}

esp_err_t AudioService::Init(i2s_chan_handle_t spk_handle, i2s_chan_handle_t mic_handle) {
    spk_handle_ = spk_handle;
    mic_handle_ = mic_handle;

    // Use smaller buffer in QEMU (no PSRAM available)
    static constexpr size_t QEMU_JITTER_SIZE = 16 * 1024;
    downstream_ringbuf_ = xRingbufferCreate(QEMU_JITTER_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (!downstream_ringbuf_) {
        ESP_LOGE(TAG, "Failed to create downstream ringbuf");
        return ESP_FAIL;
    }

    encode_stream_ = xStreamBufferCreate(16384, OPUS_FRAME_SAMPLES * sizeof(int16_t));
    if (!encode_stream_) {
        ESP_LOGE(TAG, "Failed to create encode stream buffer");
        return ESP_FAIL;
    }

    esp_err_t ret = InitOpus();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Opus init failed, falling back to PCM-only mode");
    }

    GenerateTestAudio();

    ESP_LOGI(TAG, "[QEMU] Audio service initialized (mock I/O, real Opus)");
    return ESP_OK;
}

void AudioService::Start() {
    xTaskCreatePinnedToCore(CaptureTask, "audio_in", 8192, this, 10, NULL, 0);
    xTaskCreatePinnedToCore(OpusEncodeTask, "opus_enc", 24576, this, 10, NULL, 0);
    xTaskCreatePinnedToCore(PlaybackTask, "audio_out", 16384, this, 10, NULL, 0);
    ESP_LOGI(TAG, "[QEMU] Audio tasks started (virtual I/O)");
}

void AudioService::StartCapture() {
    if (capturing_) return;
    test_audio_pos = 0;
    if (encode_stream_) xStreamBufferReset(encode_stream_);
    capturing_ = true;
    ESP_LOGI(TAG, "[QEMU] Capture started (reading test audio)");
}

void AudioService::StopCapture() {
    if (!capturing_) return;
    capturing_ = false;
    if (encode_stream_) xStreamBufferReset(encode_stream_);
    ESP_LOGI(TAG, "[QEMU] Capture stopped");
}

void AudioService::StartPlayback() {
    if (playing_) return;
    playing_ = true;
    ESP_LOGI(TAG, "[QEMU] Playback started (sink mode)");
}

void AudioService::StopPlayback() {
    if (!playing_) return;
    playing_ = false;
    ESP_LOGI(TAG, "[QEMU] Playback stopped");
}

void AudioService::ClearBuffers() {
    if (downstream_ringbuf_) {
        size_t item_size;
        void *item;
        while ((item = xRingbufferReceive(downstream_ringbuf_, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(downstream_ringbuf_, item);
        }
    }
    if (encode_stream_) xStreamBufferReset(encode_stream_);
    ESP_LOGI(TAG, "[QEMU] Buffers cleared");
}

void AudioService::PlayPrompt(bool start) {
    ESP_LOGI(TAG, "[QEMU] Prompt %s (skipped)", start ? "start" : "stop");
}

void AudioService::FadeOutPlayback(uint32_t timeout_ms) {
    (void)timeout_ms;
    StopPlayback();
}

void AudioService::PlayAlarmTone() {
    ESP_LOGI(TAG, "[QEMU] Alarm tone (skipped)");
}

void AudioService::SetDownstreamFormat(AudioFormat format) {
    downstream_format_ = format;
    ESP_LOGI(TAG, "[QEMU] Downstream format: %s",
             format == kAudioFormatOpus ? "opus" : "pcm");
}

void AudioService::SendAudioUpstream(const int16_t* samples, int count) {
    if (!UdpClient::GetInstance().IsConnected()) return;
    if (!opus_encoder_) return;

    static int16_t opus_frame_buf[OPUS_FRAME_SAMPLES];
    static int opus_frame_pos = 0;
    static uint8_t opus_pkt_buf[OPUS_MAX_PACKET];

    int remaining = count;
    const int16_t* src = samples;
    while (remaining > 0) {
        int to_copy = std::min(remaining, OPUS_FRAME_SAMPLES - opus_frame_pos);
        memcpy(opus_frame_buf + opus_frame_pos, src, to_copy * sizeof(int16_t));
        opus_frame_pos += to_copy;
        src += to_copy;
        remaining -= to_copy;

        if (opus_frame_pos >= OPUS_FRAME_SAMPLES) {
            opus_frame_pos = 0;
            int encoded = opus_encode((OpusEncoder*)opus_encoder_, opus_frame_buf,
                                      OPUS_FRAME_SAMPLES, opus_pkt_buf, OPUS_MAX_PACKET);
            if (encoded > 0) {
                uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
                UdpClient::GetInstance().Send(opus_pkt_buf, encoded, ts);
            }
        }
    }
}

int AudioService::DecodeOpus(const uint8_t* data, int len, int16_t* pcm_out, int max_samples) {
    if (!opus_decoder_) return -1;
    int decoded_samples = opus_decode((OpusDecoder*)opus_decoder_, data, len, pcm_out, max_samples, 0);
    if (decoded_samples < 0) {
        ESP_LOGW(TAG, "Opus decode error: %d", decoded_samples);
        return -1;
    }
    return decoded_samples;
}

static void ApplyVolume(int16_t* samples, int count) {
    int vol = Settings::GetInstance().GetVolume();
    if (vol >= 100) return;
    if (vol <= 0) { memset(samples, 0, count * sizeof(int16_t)); return; }
    int32_t factor = (int32_t)((vol / 100.0f) * (vol / 100.0f) * 65536.0f);
    for (int i = 0; i < count; i++) {
        samples[i] = (int16_t)((samples[i] * factor) >> 16);
    }
}

// CaptureTask: reads from virtual test audio buffer instead of I2S
void AudioService::CaptureTask(void* arg) {
    auto *self = static_cast<AudioService*>(arg);
    int16_t pcm_buffer[AUDIO_FRAME_SAMPLES];

    ESP_LOGI(TAG, "[QEMU] Capture task started (virtual mic)");

    while (true) {
        if (!self->capturing_) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Read from circular test audio buffer (simulates real-time)
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            pcm_buffer[i] = test_audio_buf[test_audio_pos];
            test_audio_pos = (test_audio_pos + 1) % AUDIO_SAMPLE_RATE;
        }

        xStreamBufferSend(self->encode_stream_, pcm_buffer,
                         AUDIO_FRAME_SAMPLES * sizeof(int16_t), pdMS_TO_TICKS(50));

        // Simulate real-time: wait 100ms per frame
        vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
    }
}

// OpusEncodeTask: same as real, encodes PCM to Opus
void AudioService::OpusEncodeTask(void* arg) {
    auto *self = static_cast<AudioService*>(arg);
    static int16_t frame_buf[OPUS_FRAME_SAMPLES];
    static uint8_t opus_pkt[OPUS_MAX_PACKET];
    int frame_pos = 0;

    ESP_LOGI(TAG, "[QEMU] Opus encode task started");

    while (true) {
        if (!self->capturing_) {
            frame_pos = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t needed = (OPUS_FRAME_SAMPLES - frame_pos) * sizeof(int16_t);
        size_t received = xStreamBufferReceive(
            self->encode_stream_, (uint8_t*)(frame_buf + frame_pos), needed,
            pdMS_TO_TICKS(200));

        if (received > 0) {
            frame_pos += received / sizeof(int16_t);
        }

        if (frame_pos >= OPUS_FRAME_SAMPLES) {
            frame_pos = 0;
            if (!UdpClient::GetInstance().IsConnected() || !self->opus_encoder_) {
                continue;
            }
            int encoded = opus_encode((OpusEncoder*)self->opus_encoder_, frame_buf,
                                      OPUS_FRAME_SAMPLES, opus_pkt, OPUS_MAX_PACKET);
            if (encoded > 0) {
                uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
                UdpClient::GetInstance().Send(opus_pkt, encoded, ts);
            }
        }
    }
}

// PlaybackTask: consumes ring buffer data (discards - no real speaker)
void AudioService::PlaybackTask(void* arg) {
    auto *self = static_cast<AudioService*>(arg);
    bool buffering = true;
    bool has_played = false;
    int empty_count = 0;
    int items_played = 0;
    int16_t decode_buf[OPUS_FRAME_SAMPLES * 2];

    ESP_LOGI(TAG, "[QEMU] Playback task started (sink mode)");

    while (true) {
        if (!self->playing_) {
            buffering = true;
            has_played = false;
            empty_count = 0;
            items_played = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (buffering) {
            UBaseType_t free_size = xRingbufferGetCurFreeSize(self->downstream_ringbuf_);
            UBaseType_t used_size = JITTER_BUFFER_SIZE - free_size;
            if (used_size >= JITTER_HIGH_WATERMARK) {
                buffering = false;
                ESP_LOGI(TAG, "[QEMU] Jitter buffer ready (%lu bytes)", (unsigned long)used_size);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        size_t item_size;
        void *item = xRingbufferReceive(self->downstream_ringbuf_, &item_size, pdMS_TO_TICKS(100));

        if (item) {
            empty_count = 0;
            if (self->downstream_format_ == kAudioFormatOpus && self->opus_decoder_) {
                int decoded = self->DecodeOpus((const uint8_t*)item, item_size,
                                               decode_buf, OPUS_FRAME_SAMPLES * 2);
                if (decoded > 0) {
                    ApplyVolume(decode_buf, decoded);
                    has_played = true;
                    items_played++;
                }
            } else {
                has_played = true;
                items_played++;
            }
            vRingbufferReturnItem(self->downstream_ringbuf_, item);

            // Simulate real-time playback pacing
            vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS / 2));
        } else {
            if (has_played) {
                empty_count++;
                if (empty_count >= 10) {
                    ESP_LOGI(TAG, "[QEMU] Playback drain complete (%d items)", items_played);
                    self->playing_ = false;
                    has_played = false;
                    empty_count = 0;
                    items_played = 0;
                    buffering = true;
                }
            }
        }
    }
}
