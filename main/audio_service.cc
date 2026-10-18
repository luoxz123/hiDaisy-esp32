#include "audio_service.h"
#include "afe_processor.h"
#include "hilily_mqtt_client.h"
#include "pin_config.h"
#include "prompt_tone.h"
#include "settings.h"
#include "udp_client.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <esp_log.h>
#include <freertos/idf_additions.h>
#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"

static const char *TAG = "audio";

extern const uint8_t daisy_intro_wav_start[]
    asm("_binary_daisy_intro_wav_start");
extern const uint8_t daisy_intro_wav_end[]
    asm("_binary_daisy_intro_wav_end");

// Convert 16-bit mono PCM to 32-bit mono for I2S output (matching xiaozhi NoAudioCodec::Write)
static void Mono16ToMono32(const int16_t *src, int32_t *dst, int samples) {
  for (int i = 0; i < samples; i++) {
    dst[i] = (int32_t)src[i] << 16;
  }
}

// INMP441 outputs 32-bit I2S data; shift right to fit 16-bit PCM
static constexpr int MIC_SAMPLE_SHIFT = 12;  // INMP441: 32-bit → 16-bit (shift 12 matches xiaozhi, with saturation clipping)

// Opus constants (aligned with xiaozhi: 60ms frames, AUDIO mode)
static constexpr int OPUS_SAMPLE_RATE = 16000;
static constexpr int OPUS_CHANNELS = 1;
static constexpr int OPUS_FRAME_MS = 60;
static constexpr int OPUS_FRAME_SAMPLES =
    OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000; // 960
static constexpr int OPUS_MAX_PACKET = 1500;
static constexpr int OPUS_DEC_FRAME_MS = 60;

AudioService &AudioService::GetInstance() {
  static AudioService instance;
  return instance;
}

esp_err_t AudioService::InitOpus() {
  // Both encoder and decoder are created lazily to avoid starving WiFi init of internal RAM.
  // Decoder: created on first StartPlayback() via InitDecoder()
  // Encoder: created on first capture via InitEncoder() in OpusEncodeTask
  decoder_frame_size_ = AUDIO_OUTPUT_SAMPLE_RATE * OPUS_DEC_FRAME_MS / 1000;
  encoder_frame_size_ = OPUS_FRAME_SAMPLES;  // default, updated when encoder is created
  ESP_LOGI(TAG, "Opus codec init deferred (dec: %dHz/%dms=%d samples, enc: %dHz/%dms=%d samples)",
           AUDIO_OUTPUT_SAMPLE_RATE, OPUS_DEC_FRAME_MS, decoder_frame_size_,
           OPUS_SAMPLE_RATE, OPUS_FRAME_MS, encoder_frame_size_);
  return ESP_OK;
}

// Create Opus encoder on demand (called from OpusEncodeTask)
bool AudioService::InitEncoder() {
  if (opus_encoder_) return true;  // already created

  esp_opus_enc_config_t enc_cfg = {};
  enc_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
  enc_cfg.channel = ESP_AUDIO_MONO;
  enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
  enc_cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
  enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
  enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
  enc_cfg.complexity = 0;
  enc_cfg.enable_fec = false;
  enc_cfg.enable_dtx = false;
  enc_cfg.enable_vbr = true;

  auto ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus_encoder_);
  if (ret != ESP_AUDIO_ERR_OK || !opus_encoder_) {
    ESP_LOGE(TAG, "Failed to create Opus encoder: %d", ret);
    return false;
  }
  int in_size = 0, out_size = 0;
  esp_opus_enc_get_frame_size(opus_encoder_, &in_size, &out_size);
  encoder_frame_size_ = in_size / sizeof(int16_t);
  ESP_LOGI(TAG, "Opus encoder ready: frame_size=%d samples (%dms), out_size=%d",
           encoder_frame_size_, OPUS_FRAME_MS, out_size);
  return true;
}

// Create Opus decoder on demand (called from StartPlayback)
bool AudioService::InitDecoder() {
  if (opus_decoder_) return true;  // already created

  esp_opus_dec_cfg_t dec_cfg = {};
  dec_cfg.sample_rate = AUDIO_OUTPUT_SAMPLE_RATE;
  dec_cfg.channel = ESP_AUDIO_MONO;
  dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
  dec_cfg.self_delimited = false;

  auto ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &opus_decoder_);
  if (ret != ESP_AUDIO_ERR_OK || !opus_decoder_) {
    ESP_LOGE(TAG, "Failed to create Opus decoder: %d", ret);
    return false;
  }
  decoder_frame_size_ = AUDIO_OUTPUT_SAMPLE_RATE * OPUS_DEC_FRAME_MS / 1000;
  ESP_LOGI(TAG, "Opus decoder ready: %dHz/%dms=%d samples",
           AUDIO_OUTPUT_SAMPLE_RATE, OPUS_DEC_FRAME_MS, decoder_frame_size_);
  return true;
}

esp_err_t AudioService::Init(i2s_chan_handle_t spk_handle,
                             i2s_chan_handle_t mic_handle) {
  spk_handle_ = spk_handle;
  mic_handle_ = mic_handle;
  initialized_ = false;

  // Create downstream audio queue in PSRAM (large buffer for burst TTS delivery)
  downstream_queue_ = xQueueCreateWithCaps(DOWNSTREAM_QUEUE_SIZE, sizeof(AudioChunk), MALLOC_CAP_SPIRAM);
  if (!downstream_queue_) {
    ESP_LOGW(TAG, "PSRAM queue alloc failed, falling back to internal RAM");
    downstream_queue_ = xQueueCreate(DOWNSTREAM_QUEUE_SIZE, sizeof(AudioChunk));
  }
  if (!downstream_queue_) {
    ESP_LOGE(TAG, "Failed to create downstream audio queue");
    return ESP_FAIL;
  }

  // Create encode stream buffer: CaptureTask → OpusEncodeTask pipeline
  // 16KB holds ~5 frames of 100ms PCM (3200 bytes each)
  // 32KB stream buffer (~1s at 16kHz) — allocated in PSRAM to save internal RAM for WiFi
  encode_stream_ = xStreamBufferCreateWithCaps(32768, OPUS_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!encode_stream_) {
    ESP_LOGW(TAG, "PSRAM stream buffer failed, falling back to internal RAM");
    encode_stream_ = xStreamBufferCreate(32768, OPUS_FRAME_SAMPLES * sizeof(int16_t));
  }
  if (!encode_stream_) {
    ESP_LOGE(TAG, "Failed to create encode stream buffer");
    return ESP_FAIL;
  }

  // Initialize Opus codec
  esp_err_t ret = InitOpus();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Opus init failed, falling back to PCM-only mode");
  }

  // Create speaker reference stream buffer for AEC (PSRAM, single-producer single-consumer)
  speaker_ref_buf_ = xStreamBufferCreateWithCaps(kSpeakerRefBufBytes, sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!speaker_ref_buf_) {
    ESP_LOGW(TAG, "PSRAM speaker ref buffer failed, falling back to internal RAM");
    speaker_ref_buf_ = xStreamBufferCreate(kSpeakerRefBufBytes, sizeof(int16_t));
  }
  if (!speaker_ref_buf_) {
    ESP_LOGW(TAG, "Speaker ref buffer alloc failed — AEC will be unavailable");
  }
  fade_out_done_ = xSemaphoreCreateBinary();
  if (!fade_out_done_) {
    ESP_LOGW(TAG, "Fade-out semaphore unavailable; barge-in will hard stop");
  }

  ESP_LOGI(TAG, "Audio service initialized (queue size: %d, ref_buf: %s)",
           DOWNSTREAM_QUEUE_SIZE, speaker_ref_buf_ ? "ok" : "NONE");
  initialized_ = true;
  return ESP_OK;
}

void AudioService::Start() {
  if (tasks_started_) {
    return;
  }
  if (!initialized_ || !downstream_queue_ || !encode_stream_) {
    ESP_LOGE(TAG, "Audio task start skipped: service not initialized");
    return;
  }

  // Enable mic I2S permanently (always-on for AFE wake word detection)
  i2s_channel_enable(mic_handle_);

  // Register AFE audio output callback: denoised PCM → StreamBuffer (only when capturing)
  AfeProcessor::GetInstance().SetAudioCallback(
      [this](const int16_t* data, int samples) {
        if (capturing_ && encode_stream_) {
          xStreamBufferSend(encode_stream_, data,
                            samples * sizeof(int16_t), pdMS_TO_TICKS(20));
        }
      });

  // CaptureTask: 8KB stack in PSRAM, pinned to Core 1
  xTaskCreatePinnedToCoreWithCaps(CaptureTask, "audio_in", 8192, this, 10, NULL, 1, MALLOC_CAP_SPIRAM);
  // Opus encode task: 24KB stack in PSRAM to save internal RAM for WiFi
  xTaskCreatePinnedToCoreWithCaps(OpusEncodeTask, "opus_enc", 24576, this, 2, NULL, 0, MALLOC_CAP_SPIRAM);
  xTaskCreatePinnedToCoreWithCaps(PlaybackTask, "audio_out", 16384, this, 10, NULL, 1, MALLOC_CAP_SPIRAM);
  tasks_started_ = true;
  ESP_LOGI(TAG, "Audio tasks started (always-on mic, capture:Core1, opus_enc:Core0, playback:Core1)");
}

void AudioService::StartCapture() {
  if (!tasks_started_ || !encode_stream_) {
    ESP_LOGE(TAG, "StartCapture skipped: audio pipeline unavailable");
    return;
  }
  if (capturing_)
    return;
  // Mic I2S is always enabled; just reset stream buffer and flag
  if (encode_stream_) xStreamBufferReset(encode_stream_);
  capturing_ = true;
  ESP_LOGI(TAG, "Capture started (AFE denoised audio → Opus encoder)");
}

void AudioService::StopCapture() {
  if (!capturing_)
    return;
  capturing_ = false;
  // Mic stays enabled for wake word detection
  if (encode_stream_) xStreamBufferReset(encode_stream_);
  ESP_LOGI(TAG, "Capture stopped (mic remains on for wake word)");
}

void AudioService::StartPlayback() {
  if (!tasks_started_) {
    ESP_LOGE(TAG, "StartPlayback skipped: audio pipeline unavailable");
    return;
  }
  if (playing_)
    return;
  // Reset Opus decoder state to prevent artifacts between TTS segments
  // Close + reopen (or lazy-init on first playback)
  if (opus_decoder_) {
    esp_opus_dec_close(opus_decoder_);
    opus_decoder_ = nullptr;
  }
  InitDecoder();  // lazy-init or re-create
  i2s_channel_enable(spk_handle_);
  // Flush DMA pipeline with silence to prevent residual audio artifacts
  static int32_t silence[AUDIO_OUTPUT_SAMPLE_RATE * 80 / 1000] = {}; // 80ms at output rate
  size_t written;
  i2s_channel_write(spk_handle_, silence, sizeof(silence), &written,
                    portMAX_DELAY);
  tts_end_received_ = false;
  playing_ = true;
  ESP_LOGI(TAG, "Playback started");
}

void AudioService::StopPlayback() {
  if (!playing_)
    return;
  playing_ = false;
  tts_end_received_ = false;
  i2s_channel_disable(spk_handle_);
  if (speaker_ref_buf_) xStreamBufferReset(speaker_ref_buf_);
  ESP_LOGI(TAG, "Playback stopped");
}

void AudioService::FadeOutPlayback(uint32_t timeout_ms) {
  if (!playing_) return;
  if (!fade_out_done_) {
    StopPlayback();
    return;
  }
  xSemaphoreTake(fade_out_done_, 0);
  fade_out_requested_ = true;
  if (xSemaphoreTake(fade_out_done_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGW(TAG, "Playback fade-out timed out after %lums",
             static_cast<unsigned long>(timeout_ms));
    fade_out_requested_ = false;
    StopPlayback();
  }
}

void AudioService::ClearBuffers() {
  if (downstream_queue_) {
    AudioChunk chunk;
    int flushed = 0;
    while (xQueueReceive(downstream_queue_, &chunk, 0) == pdTRUE) {
      heap_caps_free(chunk.data);
      flushed++;
    }
    if (flushed > 0) {
      ESP_LOGI(TAG, "Cleared %d audio chunks from queue", flushed);
    }
  }
  if (encode_stream_) xStreamBufferReset(encode_stream_);
  if (speaker_ref_buf_) xStreamBufferReset(speaker_ref_buf_);
  ESP_LOGI(TAG, "Buffers cleared");
}

int AudioService::GetRecentLevels(uint16_t* out, int count) const {
  int total = audio_level_idx_;
  int available = (total < kAudioLevelBufSize) ? total : kAudioLevelBufSize;
  int n = (count < available) ? count : available;
  int start = (total - n) % kAudioLevelBufSize;
  if (start < 0) start += kAudioLevelBufSize;
  for (int i = 0; i < n; i++) {
    out[i] = audio_levels_[(start + i) % kAudioLevelBufSize];
  }
  return n;
}

void AudioService::PlayPrompt(bool start) {
  const int16_t *data = start ? kPromptStart : kPromptStop;
  int bytes = start ? kPromptStartBytes : kPromptStopBytes;
  PlayPcmBlocking(data, bytes / sizeof(int16_t));
  ESP_LOGI(TAG, "Prompt %s played (%d PCM bytes)",
           start ? "start" : "stop", bytes);
}

void AudioService::PlayWakeGreeting() {
  const uint8_t* wav = daisy_intro_wav_start;
  size_t wav_size = daisy_intro_wav_end - daisy_intro_wav_start;
  auto read_le16 = [](const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
  };
  auto read_le32 = [](const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  };

  if (wav_size < 12 || memcmp(wav, "RIFF", 4) != 0 ||
      memcmp(wav + 8, "WAVE", 4) != 0) {
    ESP_LOGE(TAG, "Invalid Daisy greeting WAV (%u bytes)",
             static_cast<unsigned>(wav_size));
    PlayPrompt(true);
    return;
  }

  const uint8_t* pcm = nullptr;
  uint32_t pcm_bytes = 0;
  bool format_ok = false;
  for (size_t offset = 12; offset + 8 <= wav_size;) {
    const uint8_t* chunk = wav + offset;
    uint32_t chunk_size = read_le32(chunk + 4);
    size_t data_offset = offset + 8;
    size_t available = wav_size - data_offset;
    if (chunk_size > available) break;

    if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
      format_ok = read_le16(wav + data_offset) == 1 &&
                  read_le16(wav + data_offset + 2) == 1 &&
                  read_le32(wav + data_offset + 4) == AUDIO_SAMPLE_RATE &&
                  read_le16(wav + data_offset + 14) == 16;
    } else if (memcmp(chunk, "data", 4) == 0) {
      pcm = wav + data_offset;
      pcm_bytes = chunk_size;
    }

    // RIFF chunks are padded to an even byte boundary.
    offset = data_offset + chunk_size + (chunk_size & 1U);
  }

  if (!format_ok || pcm == nullptr || pcm_bytes < sizeof(int16_t)) {
    ESP_LOGE(TAG, "Unsupported Daisy greeting WAV format (%u bytes)",
             static_cast<unsigned>(wav_size));
    PlayPrompt(true);
    return;
  }

  pcm_bytes &= ~1U;
  PlayPcmBlocking(reinterpret_cast<const int16_t*>(pcm),
                  pcm_bytes / sizeof(int16_t));
  ESP_LOGI(TAG, "Daisy greeting played (%lu ms)",
           static_cast<unsigned long>((pcm_bytes / sizeof(int16_t)) * 1000 / 16000));
}

void AudioService::PlayPcmBlocking(const int16_t* data, int samples) {
  bool was_playing = playing_;
  if (!was_playing) {
    i2s_channel_enable(spk_handle_);
  }

  // Convert mono prompt to 32-bit mono in chunks
  static int32_t out_buf[1024]; // 1024 mono 32-bit samples
  for (int offset = 0; offset < samples; offset += 1024) {
    int chunk_samples = (samples - offset > 1024) ? 1024 : (samples - offset);
    // Write prompt PCM to reference buffer for AEC
    if (speaker_ref_buf_) {
      xStreamBufferSend(speaker_ref_buf_, data + offset,
                        chunk_samples * sizeof(int16_t), 0);
    }
    Mono16ToMono32(data + offset, out_buf, chunk_samples);
    size_t bytes_written;
    i2s_channel_write(spk_handle_, out_buf,
                      chunk_samples * sizeof(int32_t), &bytes_written,
                      pdMS_TO_TICKS(500));
  }
  // Write extra silence to fully flush DMA pipeline
  static int32_t silence[1280] = {}; // 80ms at 16kHz mono
  size_t sil_written;
  i2s_channel_write(spk_handle_, silence, sizeof(silence), &sil_written,
                    pdMS_TO_TICKS(300));
  vTaskDelay(pdMS_TO_TICKS(20));

  if (!was_playing) {
    i2s_channel_disable(spk_handle_);
  }
}

void AudioService::PlayAlarmTone() {
  bool was_playing = playing_;
  if (!was_playing) {
    i2s_channel_enable(spk_handle_);
  }

  // Generate 3 beeps: 200ms tone + 150ms silence each, ~1050ms total
  // 1kHz sine wave at 16kHz sample rate
  static constexpr int kBeepSamples = AUDIO_SAMPLE_RATE * 200 / 1000;   // 3200
  static constexpr int kSilenceSamples = AUDIO_SAMPLE_RATE * 150 / 1000; // 2400
  static constexpr int kBeepCount = 3;
  static constexpr float kFreqHz = 1000.0f;
  static constexpr float kAmplitude = 16000.0f;

  int16_t pcm_buf[512];
  int32_t out_buf[512];

  for (int beep = 0; beep < kBeepCount; beep++) {
    // Tone
    for (int offset = 0; offset < kBeepSamples; offset += 512) {
      int chunk = (kBeepSamples - offset > 512) ? 512 : (kBeepSamples - offset);
      for (int i = 0; i < chunk; i++) {
        float t = (float)(offset + i) / (float)AUDIO_SAMPLE_RATE;
        pcm_buf[i] = (int16_t)(sinf(2.0f * M_PI * kFreqHz * t) * kAmplitude);
      }
      // Write alarm PCM to reference buffer for AEC
      if (speaker_ref_buf_) {
        xStreamBufferSend(speaker_ref_buf_, pcm_buf,
                          chunk * sizeof(int16_t), 0);
      }
      Mono16ToMono32(pcm_buf, out_buf, chunk);
      size_t written;
      i2s_channel_write(spk_handle_, out_buf, chunk * sizeof(int32_t),
                        &written, pdMS_TO_TICKS(500));
    }
    // Silence between beeps (skip after last)
    if (beep < kBeepCount - 1) {
      memset(out_buf, 0, sizeof(out_buf));
      for (int offset = 0; offset < kSilenceSamples; offset += 512) {
        int chunk = (kSilenceSamples - offset > 512) ? 512 : (kSilenceSamples - offset);
        size_t written;
        i2s_channel_write(spk_handle_, out_buf, chunk * sizeof(int32_t),
                          &written, pdMS_TO_TICKS(500));
      }
    }
  }

  // Flush DMA
  static int32_t silence[1280] = {};
  size_t sil_written;
  i2s_channel_write(spk_handle_, silence, sizeof(silence), &sil_written,
                    pdMS_TO_TICKS(300));
  vTaskDelay(pdMS_TO_TICKS(20));

  if (!was_playing) {
    i2s_channel_disable(spk_handle_);
  }
  ESP_LOGI(TAG, "Alarm tone played (%d beeps)", kBeepCount);
}

int AudioService::ReadReferenceData(int16_t* buf, size_t samples) {
  if (!speaker_ref_buf_ || !buf || samples == 0) {
    if (buf) memset(buf, 0, samples * sizeof(int16_t));
    return 0;
  }
  size_t needed = samples * sizeof(int16_t);
  size_t got = xStreamBufferReceive(speaker_ref_buf_, buf, needed, 0);
  int real_samples = got / sizeof(int16_t);
  // Zero-fill remainder if not enough reference data
  if (real_samples < (int)samples) {
    memset(buf + real_samples, 0, (samples - real_samples) * sizeof(int16_t));
  }
  return real_samples;
}

void AudioService::SetDownstreamFormat(AudioFormat format) {
  downstream_format_ = format;
  ESP_LOGI(TAG, "Downstream format: %s",
           format == kAudioFormatOpus ? "opus" : "pcm");
}

// SendAudioUpstream removed — encoding is done in OpusEncodeTask

int AudioService::DecodeOpus(const uint8_t *data, int len, int16_t *pcm_out,
                             int max_samples) {
  if (!opus_decoder_)
    return -1;

  esp_audio_dec_in_raw_t raw = {};
  raw.buffer = (uint8_t *)data;
  raw.len = (uint32_t)len;
  raw.consumed = 0;
  raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

  esp_audio_dec_out_frame_t out_frame = {};
  out_frame.buffer = (uint8_t *)pcm_out;
  out_frame.len = (uint32_t)(max_samples * sizeof(int16_t));
  out_frame.decoded_size = 0;

  esp_audio_dec_info_t dec_info = {};
  auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
  if (ret != ESP_AUDIO_ERR_OK) {
    ESP_LOGW(TAG, "Opus decode error: %d (input %d bytes)", ret, len);
    return -1;
  }
  return out_frame.decoded_size / sizeof(int16_t);
}

// Apply software volume scaling with quadratic curve (following xiaozhi pattern)
static void ApplyVolume(int16_t *samples, int count) {
  int vol = Settings::GetInstance().GetVolume();
  if (vol >= 100) return;
  if (vol <= 0) {
    memset(samples, 0, count * sizeof(int16_t));
    return;
  }
  // Quadratic curve: factor = (vol/100)^2, scaled to fixed-point 16.16
  int32_t factor = (int32_t)((vol / 100.0f) * (vol / 100.0f) * 65536.0f);
  for (int i = 0; i < count; i++) {
    samples[i] = (int16_t)((samples[i] * factor) >> 16);
  }
}

static void ApplyFadeOut(int16_t *samples, int count) {
  if (count <= 1) return;
  for (int i = 0; i < count; ++i) {
    samples[i] = static_cast<int16_t>(
        (static_cast<int32_t>(samples[i]) * (count - 1 - i)) /
        (count - 1));
  }
}

void AudioService::CaptureTask(void *arg) {
  auto *self = static_cast<AudioService *>(arg);
  static int32_t raw_buffer[AUDIO_FRAME_SAMPLES];
  static int16_t pcm_buffer[AUDIO_FRAME_SAMPLES];
  static int16_t ref_buffer[AUDIO_FRAME_SAMPLES];
  size_t bytes_read;

  ESP_LOGI(TAG, "Capture task started (Core %d, always-on → AFE+AEC)", xPortGetCoreID());

  while (true) {
    // Yield mic I2S to AudioTest when exclusive mode is active
    if (self->mic_exclusive_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Always read I2S — mic is always on for AFE/wake word
    esp_err_t ret = i2s_channel_read(self->mic_handle_, raw_buffer,
                                     AUDIO_FRAME_SAMPLES * sizeof(int32_t),
                                     &bytes_read, pdMS_TO_TICKS(200));
    if (ret != ESP_OK || bytes_read == 0) {
      continue;
    }

    int samples_read = bytes_read / sizeof(int32_t);

    // 32-bit → 16-bit conversion with saturation
    int64_t sum_sq = 0;
    for (int i = 0; i < samples_read; i++) {
      int32_t value = raw_buffer[i] >> MIC_SAMPLE_SHIFT;
      if (value > INT16_MAX)
        value = INT16_MAX;
      else if (value < INT16_MIN)
        value = INT16_MIN;
      pcm_buffer[i] = (int16_t)value;
      sum_sq += (int64_t)value * value;
    }

    // RMS for waveform display (only update when capturing to avoid UI noise)
    if (self->capturing_) {
      uint16_t rms = (uint16_t)sqrtf((float)sum_sq / samples_read);
      int idx = self->audio_level_idx_ % kAudioLevelBufSize;
      self->audio_levels_[idx] = rms;
      self->audio_level_idx_ = self->audio_level_idx_ + 1;
    }

    // Read speaker reference data for AEC (zeros when not playing)
    self->ReadReferenceData(ref_buffer, samples_read);

    // Feed mic + reference to AFE processor (AEC + NS + WakeNet)
    AfeProcessor::GetInstance().Feed(pcm_buffer, ref_buffer, samples_read);
  }
}

void AudioService::OpusEncodeTask(void *arg) {
  auto *self = static_cast<AudioService *>(arg);
  // Opus encode: 960 samples (60ms) per frame, matching xiaozhi
  static int16_t frame_buf[OPUS_FRAME_SAMPLES];
  static uint8_t opus_pkt[OPUS_MAX_PACKET];
  int frame_pos = 0;
  int64_t enc_start_us = 0;
  int enc_frame_count = 0;

  ESP_LOGI(TAG, "Opus encode task started (Core %d, %dms frames, esp_audio_codec)",
           xPortGetCoreID(), OPUS_FRAME_MS);

  while (true) {
    if (!self->capturing_) {
      if (enc_frame_count > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - enc_start_us) / 1000;
        ESP_LOGW(TAG, "Encode session ended: %d frames in %lld ms (expected ~%.0f)",
                 enc_frame_count, elapsed_ms,
                 (float)elapsed_ms / (float)OPUS_FRAME_MS);
        enc_frame_count = 0;
      }
      frame_pos = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (!self->encode_stream_) {
      frame_pos = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Lazy-init encoder on first capture (deferred from boot to avoid WiFi OOM)
    if (!self->opus_encoder_) {
      if (!self->InitEncoder()) {
        ESP_LOGE(TAG, "Opus encoder init failed, retrying in 1s");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }

    // Read PCM samples to fill one Opus frame (960 samples = 60ms)
    size_t needed = (OPUS_FRAME_SAMPLES - frame_pos) * sizeof(int16_t);
    size_t received = xStreamBufferReceive(
        self->encode_stream_, (uint8_t *)(frame_buf + frame_pos), needed,
        pdMS_TO_TICKS(200));

    if (received > 0) {
      frame_pos += received / sizeof(int16_t);
    }

    if (frame_pos >= OPUS_FRAME_SAMPLES) {
      frame_pos = 0;
      if (!UdpClient::GetInstance().IsConnected()) {
        continue;
      }
      if (enc_frame_count == 0) {
        enc_start_us = esp_timer_get_time();
      }

      // Opus encode via esp_audio_codec (Espressif optimized)
      esp_audio_enc_in_frame_t in = {};
      in.buffer = (uint8_t *)frame_buf;
      in.len = (uint32_t)(OPUS_FRAME_SAMPLES * sizeof(int16_t));

      esp_audio_enc_out_frame_t out = {};
      out.buffer = opus_pkt;
      out.len = (uint32_t)OPUS_MAX_PACKET;
      out.encoded_bytes = 0;

      auto ret = esp_opus_enc_process(self->opus_encoder_, &in, &out);
      if (ret == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
        UdpClient::GetInstance().Send(opus_pkt, out.encoded_bytes, ts);
        enc_frame_count++;
        if (enc_frame_count <= 3) {
          ESP_LOGI(TAG, "Opus encoded: %d bytes from %d samples",
                   (int)out.encoded_bytes, OPUS_FRAME_SAMPLES);
        }
      } else {
        ESP_LOGW(TAG, "Opus encode error: %d", ret);
      }
    }
  }
}

void AudioService::PlaybackTask(void *arg) {
  auto *self = static_cast<AudioService *>(arg);
  bool buffering = true;
  bool has_played = false;
  int empty_count = 0;
  int items_played = 0;
  int64_t last_stats_ms = 0;
  // Opus decoder at 24kHz: server may send 60ms frames, so buffer for max expected frame
  static constexpr int MAX_DECODE_SAMPLES = (AUDIO_OUTPUT_SAMPLE_RATE * 60 / 1000) * 2;  // 60ms max
  static int16_t decode_buf[MAX_DECODE_SAMPLES];
  static int32_t output_buf[MAX_DECODE_SAMPLES]; // 32-bit mono output for I2S

  ESP_LOGI(TAG, "Playback task started (Core %d)", xPortGetCoreID());

  while (true) {
    if (!self->playing_) {
      buffering = true;
      has_played = false;
      empty_count = 0;
      items_played = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Initial buffering: wait for enough queued items before first playback
    if (buffering) {
      UBaseType_t waiting = uxQueueMessagesWaiting(self->downstream_queue_);
      if (waiting >= JITTER_ITEMS_WATERMARK) {
        buffering = false;
        last_stats_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "Jitter buffer ready, starting playback (%lu items queued)",
                 (unsigned long)waiting);
      } else {
        // Periodic diagnostic: log queue depth every ~500ms while waiting
        static int buf_wait_count = 0;
        if (++buf_wait_count % 50 == 0) {
          ESP_LOGW(TAG, "Buffering: %lu items (need %d) | UDP sock=%d aes=%d rx_loops=%d",
                   (unsigned long)waiting, JITTER_ITEMS_WATERMARK,
                   UdpClient::GetInstance().GetSock(),
                   UdpClient::GetInstance().IsAesReady(),
                   UdpClient::GetInstance().GetRxLoopCount());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
    }

    // Periodic stats every 5 seconds
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_stats_ms >= 5000) {
      UBaseType_t waiting = uxQueueMessagesWaiting(self->downstream_queue_);
      ESP_LOGI(TAG, "Playback stats: played=%d items, queued=%lu, format=%s",
               items_played, (unsigned long)waiting,
               self->downstream_format_ == kAudioFormatOpus ? "opus" : "pcm");
      last_stats_ms = now_ms;
    }

    AudioChunk chunk;
    if (xQueueReceive(self->downstream_queue_, &chunk, pdMS_TO_TICKS(100)) ==
        pdTRUE) {
      size_t bytes_written;
      empty_count = 0;
      const bool fade_this_frame = self->fade_out_requested_;

      if (self->downstream_format_ == kAudioFormatOpus && self->opus_decoder_) {
        int decoded = self->DecodeOpus((const uint8_t *)chunk.data, chunk.len,
                                       decode_buf, MAX_DECODE_SAMPLES);
        if (decoded > 0) {
          if (items_played < 5) {
            ESP_LOGI(TAG, "Opus decoded: %d samples from %d bytes",
                     decoded, chunk.len);
          }
          ApplyVolume(decode_buf, decoded);
          if (fade_this_frame) ApplyFadeOut(decode_buf, decoded);
          // Write volume-adjusted PCM to reference buffer for AEC
          if (self->speaker_ref_buf_) {
            xStreamBufferSend(self->speaker_ref_buf_, decode_buf,
                              decoded * sizeof(int16_t), 0);
          }
          Mono16ToMono32(decode_buf, output_buf, decoded);
          i2s_channel_write(self->spk_handle_, output_buf,
                            decoded * sizeof(int32_t), &bytes_written,
                            portMAX_DELAY);
          has_played = true;
          items_played++;
        }
      } else {
        // PCM path — convert to 32-bit mono for I2S
        static int16_t pcm_vol_buf[AUDIO_FRAME_SAMPLES];
        static int32_t pcm_out_buf[AUDIO_FRAME_SAMPLES];
        int sample_count = chunk.len / sizeof(int16_t);
        if (sample_count > AUDIO_FRAME_SAMPLES) sample_count = AUDIO_FRAME_SAMPLES;
        memcpy(pcm_vol_buf, chunk.data, sample_count * sizeof(int16_t));
        ApplyVolume(pcm_vol_buf, sample_count);
        if (fade_this_frame) ApplyFadeOut(pcm_vol_buf, sample_count);
        // Write volume-adjusted PCM to reference buffer for AEC
        if (self->speaker_ref_buf_) {
          xStreamBufferSend(self->speaker_ref_buf_, pcm_vol_buf,
                            sample_count * sizeof(int16_t), 0);
        }
        Mono16ToMono32(pcm_vol_buf, pcm_out_buf, sample_count);
        i2s_channel_write(self->spk_handle_, pcm_out_buf,
                          sample_count * sizeof(int32_t), &bytes_written,
                          portMAX_DELAY);
        has_played = true;
        items_played++;
      }

      heap_caps_free(chunk.data);
      if (fade_this_frame) {
        self->fade_out_requested_ = false;
        self->playing_ = false;
        self->tts_end_received_ = false;
        i2s_channel_disable(self->spk_handle_);
        if (self->speaker_ref_buf_) xStreamBufferReset(self->speaker_ref_buf_);
        xSemaphoreGive(self->fade_out_done_);
        ESP_LOGI(TAG, "Playback faded out over one audio frame");
      }
    } else {
      // No data received within 100ms
      if (has_played) {
        empty_count++;
        // After tts_end: drain remaining buffer then stop (1s timeout)
        if (self->tts_end_received_ && empty_count >= 10) {
          ESP_LOGI(TAG,
                   "TTS end + drain complete, auto-stopping (played %d items)",
                   items_played);
          self->playing_ = false;
          self->tts_end_received_ = false;
          i2s_channel_disable(self->spk_handle_);
          has_played = false;
          empty_count = 0;
          items_played = 0;
          buffering = true;
          if (self->playback_complete_cb_) {
            self->playback_complete_cb_();
          }
        }
        // Fallback: no tts_end after 30s silence → server likely crashed
        else if (!self->tts_end_received_ && empty_count >= 300) {
          ESP_LOGW(TAG,
                   "No tts_end after 30s, force stopping (played %d items)",
                   items_played);
          self->playing_ = false;
          self->tts_end_received_ = false;
          i2s_channel_disable(self->spk_handle_);
          has_played = false;
          empty_count = 0;
          items_played = 0;
          buffering = true;
        }
      }
    }
  }
}
