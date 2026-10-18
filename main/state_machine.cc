#include "state_machine.h"
#include <esp_log.h>
#include <cstring>

static const char *TAG = "state_machine";

StateMachine::StateMachine()
    : current_state_(kDeviceStateStarting)
    , current_emotion_(kEmotionNeutral)
    , emotion_intensity_(1.0f)
{
    mutex_ = xSemaphoreCreateMutex();
    assert(mutex_ != NULL);
}

StateMachine::~StateMachine()
{
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

bool StateMachine::IsValidTransition(DeviceState from, DeviceState to)
{
    // Error and WifiConnecting can be entered from any state
    if (to == kDeviceStateError || to == kDeviceStateWifiConnecting) {
        return true;
    }
    // Reminding can interrupt any state
    if (to == kDeviceStateReminding) {
        return true;
    }

    switch (from) {
        case kDeviceStateStarting:
            return to == kDeviceStateWifiConnecting || to == kDeviceStateIdle;
        case kDeviceStateWifiConnecting:
            return to == kDeviceStateIdle;
        case kDeviceStateIdle:
            // Server-driven system notifications (for example low-battery TTS)
            // may begin without a preceding user listening session.
            return to == kDeviceStateListening || to == kDeviceStateThinking ||
                   to == kDeviceStateSpeaking;
        case kDeviceStateListening:
            return to == kDeviceStateThinking || to == kDeviceStateSpeaking || to == kDeviceStateIdle;
        case kDeviceStateThinking:
            return to == kDeviceStateSpeaking || to == kDeviceStateIdle || to == kDeviceStateListening;
        case kDeviceStateSpeaking:
            return to == kDeviceStateIdle || to == kDeviceStateListening || to == kDeviceStateThinking;
        case kDeviceStateReminding:
            return to == kDeviceStateIdle || to == kDeviceStateSpeaking;
        case kDeviceStateError:
            return to == kDeviceStateIdle || to == kDeviceStateWifiConnecting;
        default:
            return false;
    }
}

bool StateMachine::TransitionTo(DeviceState new_state)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);

    DeviceState old_state = current_state_;
    if (old_state == new_state) {
        xSemaphoreGive(mutex_);
        return true;
    }

    if (!IsValidTransition(old_state, new_state)) {
        ESP_LOGW(TAG, "Invalid transition: %s -> %s",
                 device_state_to_string(old_state),
                 device_state_to_string(new_state));
        xSemaphoreGive(mutex_);
        return false;
    }

    current_state_ = new_state;
    ESP_LOGI(TAG, "State: %s -> %s",
             device_state_to_string(old_state),
             device_state_to_string(new_state));

    // Copy callbacks under lock, invoke outside
    auto callbacks = state_callbacks_;
    xSemaphoreGive(mutex_);

    for (auto& cb : callbacks) {
        cb(old_state, new_state);
    }

    return true;
}

DeviceState StateMachine::GetState() const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    DeviceState state = current_state_;
    xSemaphoreGive(mutex_);
    return state;
}

EmotionType StateMachine::GetEmotion() const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    EmotionType emotion = current_emotion_;
    xSemaphoreGive(mutex_);
    return emotion;
}

float StateMachine::GetEmotionIntensity() const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float intensity = emotion_intensity_;
    xSemaphoreGive(mutex_);
    return intensity;
}

void StateMachine::SetEmotion(EmotionType emotion, float intensity)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (current_emotion_ == emotion && emotion_intensity_ == intensity) {
        xSemaphoreGive(mutex_);
        return;
    }
    current_emotion_ = emotion;
    emotion_intensity_ = intensity;
    ESP_LOGI(TAG, "Emotion: %s (intensity=%.2f)", emotion_to_string(emotion), intensity);

    auto callbacks = emotion_callbacks_;
    xSemaphoreGive(mutex_);

    for (auto& cb : callbacks) {
        cb(emotion, intensity);
    }
}

void StateMachine::OnStateChange(StateChangeCallback callback)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    state_callbacks_.push_back(std::move(callback));
    xSemaphoreGive(mutex_);
}

void StateMachine::OnEmotionChange(EmotionChangeCallback callback)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    emotion_callbacks_.push_back(std::move(callback));
    xSemaphoreGive(mutex_);
}
