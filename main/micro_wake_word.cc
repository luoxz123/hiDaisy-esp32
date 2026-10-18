#include "micro_wake_word.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <frontend.h>
#include <frontend_util.h>
#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

static const char* TAG = "mww";

// The model was supplied without its microWakeWord JSON manifest. These are
// the standard v2 streaming defaults and are deliberately centralized for
// tuning after FAR/RAR measurements on the real enclosure.
static constexpr int kSampleRate = 16000;
static constexpr int kFeatureSize = 40;
static constexpr int kFeatureDurationMs = 30;
static constexpr int kFeatureStepMs = 10;
static constexpr int kSlidingWindowSize = 5;
static constexpr float kProbabilityCutoff = 0.97f;
static constexpr uint8_t kQuantizedCutoff =
    static_cast<uint8_t>(kProbabilityCutoff * 255.0f);
static constexpr size_t kTensorArenaSize = 64 * 1024;
static constexpr size_t kVariableArenaSize = 1024;
// Keep a short contiguous window: this streaming model needs consecutive
// 10 ms feature slices. If the consumer falls behind, discard only the oldest
// frame and cap latency at roughly this queue depth.
static constexpr int kQueueLength = 8;
// Model input stride is 3 feature slices. About one second of warm-up/cooldown.
static constexpr int kIgnoreInferenceWindows = 34;

extern const uint8_t daisy_model_start[]
    asm("_binary_daisy_stream_state_internal_quant_tflite_start");

struct PcmQueueFrame {
    int16_t samples[160];
};

struct MicroWakeWordDetector::Impl {
    QueueHandle_t audio_queue = nullptr;
    FrontendConfig frontend_config = {};
    FrontendState frontend_state = {};

    tflite::MicroMutableOpResolver<13> resolver;
    tflite::MicroAllocator* variable_allocator = nullptr;
    tflite::MicroResourceVariables* resource_variables = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    void* interpreter_storage = nullptr;
    uint8_t* tensor_arena = nullptr;
    uint8_t* variable_arena = nullptr;

    int feature_stride_pos = 0;
    uint8_t probabilities[kSlidingWindowSize] = {};
    int probability_pos = 0;
    int ignore_windows = kIgnoreInferenceWindows;
    uint32_t dropped_frames = 0;
};

MicroWakeWordDetector& MicroWakeWordDetector::GetInstance() {
    static MicroWakeWordDetector instance;
    return instance;
}

esp_err_t MicroWakeWordDetector::Init() {
    if (ready_) return ESP_OK;

    impl_ = new (std::nothrow) Impl();
    if (!impl_) return ESP_ERR_NO_MEM;

    impl_->frontend_config.window.size_ms = kFeatureDurationMs;
    impl_->frontend_config.window.step_size_ms = kFeatureStepMs;
    impl_->frontend_config.filterbank.num_channels = kFeatureSize;
    impl_->frontend_config.filterbank.lower_band_limit = 125.0f;
    impl_->frontend_config.filterbank.upper_band_limit = 7500.0f;
    impl_->frontend_config.noise_reduction.smoothing_bits = 10;
    impl_->frontend_config.noise_reduction.even_smoothing = 0.025f;
    impl_->frontend_config.noise_reduction.odd_smoothing = 0.06f;
    impl_->frontend_config.noise_reduction.min_signal_remaining = 0.05f;
    impl_->frontend_config.pcan_gain_control.enable_pcan = true;
    impl_->frontend_config.pcan_gain_control.strength = 0.95f;
    impl_->frontend_config.pcan_gain_control.offset = 80.0f;
    impl_->frontend_config.pcan_gain_control.gain_bits = 21;
    impl_->frontend_config.log_scale.enable_log = true;
    impl_->frontend_config.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&impl_->frontend_config, &impl_->frontend_state,
                               kSampleRate)) {
        ESP_LOGE(TAG, "Failed to initialize audio feature frontend");
        return ESP_FAIL;
    }

    bool ops_ok = true;
    ops_ok &= impl_->resolver.AddCallOnce() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddVarHandle() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddReshape() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddReadVariable() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddConcatenation() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddStridedSlice() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddAssignVariable() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddConv2D() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddDepthwiseConv2D() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddSplitV() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddFullyConnected() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddLogistic() == kTfLiteOk;
    ops_ok &= impl_->resolver.AddQuantize() == kTfLiteOk;
    if (!ops_ok) {
        ESP_LOGE(TAG, "Failed to register TFLite operators");
        return ESP_FAIL;
    }

    impl_->variable_arena = static_cast<uint8_t*>(heap_caps_malloc(
        kVariableArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    impl_->tensor_arena = static_cast<uint8_t*>(heap_caps_malloc(
        kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    impl_->interpreter_storage = heap_caps_malloc(
        sizeof(tflite::MicroInterpreter), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!impl_->variable_arena || !impl_->tensor_arena ||
        !impl_->interpreter_storage) {
        ESP_LOGE(TAG, "Failed to allocate TFLite arenas in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    impl_->variable_allocator = tflite::MicroAllocator::Create(
        impl_->variable_arena, kVariableArenaSize);
    impl_->resource_variables = tflite::MicroResourceVariables::Create(
        impl_->variable_allocator, 20);
    if (!impl_->variable_allocator || !impl_->resource_variables) {
        ESP_LOGE(TAG, "Failed to create TFLite resource variables");
        return ESP_ERR_NO_MEM;
    }

    const tflite::Model* model = tflite::GetModel(daisy_model_start);
    if (!model || model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Unsupported Daisy model schema");
        return ESP_ERR_NOT_SUPPORTED;
    }
    impl_->interpreter = new (impl_->interpreter_storage)
        tflite::MicroInterpreter(model, impl_->resolver, impl_->tensor_arena,
                                 kTensorArenaSize, impl_->resource_variables);
    if (impl_->interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena=%u)",
                 static_cast<unsigned>(kTensorArenaSize));
        return ESP_ERR_NO_MEM;
    }

    TfLiteTensor* input = impl_->interpreter->input(0);
    TfLiteTensor* output = impl_->interpreter->output(0);
    if (!input || input->type != kTfLiteInt8 || input->dims->size != 3 ||
        input->dims->data[0] != 1 || input->dims->data[1] != 3 ||
        input->dims->data[2] != kFeatureSize || !output ||
        output->type != kTfLiteUInt8 || output->dims->size != 2 ||
        output->dims->data[0] != 1 || output->dims->data[1] != 1) {
        ESP_LOGE(TAG, "Unexpected model tensors; expected int8[1,3,40] -> uint8[1,1]");
        return ESP_ERR_INVALID_SIZE;
    }

    impl_->audio_queue = xQueueCreateWithCaps(
        kQueueLength, sizeof(PcmQueueFrame), MALLOC_CAP_SPIRAM);
    if (!impl_->audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    ready_ = true;
    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(
        InferenceTask, "mww_daisy", 4096, this, 2, nullptr, 0,
        MALLOC_CAP_SPIRAM);
    if (task_ok != pdPASS) {
        ready_ = false;
        ESP_LOGE(TAG, "Failed to create inference task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Daisy ready: int8[1,3,40], step=%dms, cutoff=%.2f, window=%d, arena=%u PSRAM",
             kFeatureStepMs, kProbabilityCutoff, kSlidingWindowSize,
             static_cast<unsigned>(kTensorArenaSize));
    return ESP_OK;
}

void MicroWakeWordDetector::Feed(const int16_t* samples, int count) {
    if (!ready_ || !enabled_ || !impl_ || !impl_->audio_queue || !samples || count <= 0)
        return;

    int pos = 0;
    while (pos < count) {
        int copy_count = std::min(count - pos,
                                  kPcmFrameSamples - feed_carry_pos_);
        memcpy(feed_carry_ + feed_carry_pos_, samples + pos,
               copy_count * sizeof(int16_t));
        feed_carry_pos_ += copy_count;
        pos += copy_count;
        if (feed_carry_pos_ == kPcmFrameSamples) {
            PcmQueueFrame frame;
            memcpy(frame.samples, feed_carry_, sizeof(frame.samples));
            if (xQueueSend(impl_->audio_queue, &frame, 0) != pdTRUE) {
                // Preserve real-time behavior without destroying continuity
                // on every inference: evict one oldest frame only when full.
                PcmQueueFrame oldest;
                xQueueReceive(impl_->audio_queue, &oldest, 0);
                xQueueSend(impl_->audio_queue, &frame, 0);
                impl_->dropped_frames++;
                if ((impl_->dropped_frames % 50) == 1) {
                    ESP_LOGW(TAG, "Inference lag, evicted %lu old frame(s)",
                             static_cast<unsigned long>(impl_->dropped_frames));
                }
            }
            feed_carry_pos_ = 0;
        }
    }
}

void MicroWakeWordDetector::SetEnabled(bool enabled) {
    if (!ready_ || enabled_ == enabled) return;
    enabled_ = enabled;
    feed_carry_pos_ = 0;
    reset_requested_ = true;
    if (!enabled && impl_ && impl_->audio_queue) {
        xQueueReset(impl_->audio_queue);
    }
    ESP_LOGI(TAG, "Daisy inference %s", enabled ? "enabled" : "paused");
}

void MicroWakeWordDetector::InferenceTask(void* arg) {
    auto* self = static_cast<MicroWakeWordDetector*>(arg);
    PcmQueueFrame frame;
    ESP_LOGI(TAG, "Inference task started on core %d", xPortGetCoreID());
    while (self->ready_) {
        if (xQueueReceive(self->impl_->audio_queue, &frame,
                          pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!self->enabled_) continue;
            if (self->reset_requested_) {
                FrontendReset(&self->impl_->frontend_state);
                self->impl_->feature_stride_pos = 0;
                self->ResetDetectionWindow();
                self->reset_requested_ = false;
            }
            if (!self->ProcessAudio(frame.samples, kPcmFrameSamples)) {
                ESP_LOGE(TAG, "Daisy inference stopped after processing error");
                self->ready_ = false;
            }
            // Guarantee that IDLE0 gets scheduled even when inference is
            // continuously slower than real time.
            vTaskDelay(1);
        }
    }
    vTaskDelete(nullptr);
}

bool MicroWakeWordDetector::ProcessAudio(const int16_t* samples, int count) {
    int pos = 0;
    while (pos < count) {
        size_t processed = 0;
        FrontendOutput output = FrontendProcessSamples(
            &impl_->frontend_state, samples + pos, count - pos, &processed);
        if (processed == 0 && output.size == 0) {
            ESP_LOGE(TAG, "Audio frontend made no progress");
            return false;
        }
        pos += static_cast<int>(processed);
        if (output.size == 0) continue;
        if (output.size != kFeatureSize) {
            ESP_LOGE(TAG, "Unexpected feature size: %u",
                     static_cast<unsigned>(output.size));
            return false;
        }

        int8_t features[kFeatureSize];
        for (int i = 0; i < kFeatureSize; ++i) {
            // Matches the quantization used by microWakeWord v2 training.
            int32_t value = ((output.values[i] * 256) + 333) / 666 - 128;
            features[i] = static_cast<int8_t>(
                std::max<int32_t>(INT8_MIN, std::min<int32_t>(INT8_MAX, value)));
        }
        if (!ProcessFeatures(features)) return false;
    }
    return true;
}

bool MicroWakeWordDetector::ProcessFeatures(const int8_t* features) {
    TfLiteTensor* input = impl_->interpreter->input(0);
    memcpy(input->data.int8 + impl_->feature_stride_pos * kFeatureSize,
           features, kFeatureSize);
    impl_->feature_stride_pos++;
    if (impl_->feature_stride_pos < input->dims->data[1]) return true;
    impl_->feature_stride_pos = 0;

    if (impl_->interpreter->Invoke() != kTfLiteOk) return false;
    uint8_t probability = impl_->interpreter->output(0)->data.uint8[0];
    impl_->probabilities[impl_->probability_pos] = probability;
    impl_->probability_pos =
        (impl_->probability_pos + 1) % kSlidingWindowSize;

    if (impl_->ignore_windows > 0) {
        impl_->ignore_windows--;
        return true;
    }

    uint32_t sum = 0;
    uint8_t max_probability = 0;
    for (uint8_t value : impl_->probabilities) {
        sum += value;
        max_probability = std::max(max_probability, value);
    }
    uint8_t average = static_cast<uint8_t>(sum / kSlidingWindowSize);
    if (average >= kQuantizedCutoff) {
        ESP_LOGI(TAG, "Hi Daisy detected (avg=%.3f max=%.3f)",
                 average / 255.0f, max_probability / 255.0f);
        ResetDetectionWindow();
        if (wake_cb_) wake_cb_();
    }
    return true;
}

void MicroWakeWordDetector::ResetDetectionWindow() {
    memset(impl_->probabilities, 0, sizeof(impl_->probabilities));
    impl_->probability_pos = 0;
    impl_->ignore_windows = kIgnoreInferenceWindows;
}
