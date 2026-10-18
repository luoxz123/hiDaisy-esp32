#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "device_state.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>
#include <vector>

class StateMachine {
public:
    using StateChangeCallback = std::function<void(DeviceState old_state, DeviceState new_state)>;
    using EmotionChangeCallback = std::function<void(EmotionType emotion, float intensity)>;

    StateMachine();
    ~StateMachine();

    bool TransitionTo(DeviceState new_state);
    DeviceState GetState() const;
    EmotionType GetEmotion() const;
    float GetEmotionIntensity() const;
    void SetEmotion(EmotionType emotion, float intensity = 1.0f);

    void OnStateChange(StateChangeCallback callback);
    void OnEmotionChange(EmotionChangeCallback callback);

    // Check if transition is valid
    static bool IsValidTransition(DeviceState from, DeviceState to);

private:
    DeviceState current_state_;
    EmotionType current_emotion_;
    float emotion_intensity_;
    mutable SemaphoreHandle_t mutex_;
    std::vector<StateChangeCallback> state_callbacks_;
    std::vector<EmotionChangeCallback> emotion_callbacks_;
};

#endif // STATE_MACHINE_H
