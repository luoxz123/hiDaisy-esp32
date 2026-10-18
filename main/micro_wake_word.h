#ifndef MICRO_WAKE_WORD_H
#define MICRO_WAKE_WORD_H

#include <esp_err.h>

#include <functional>

// Local microWakeWord v2 detector. Audio must be 16 kHz, signed 16-bit mono
// PCM. Feed() only enqueues audio; TFLite inference runs in its own task.
class MicroWakeWordDetector {
public:
    using WakeCallback = std::function<void()>;

    static MicroWakeWordDetector& GetInstance();

    MicroWakeWordDetector(const MicroWakeWordDetector&) = delete;
    MicroWakeWordDetector& operator=(const MicroWakeWordDetector&) = delete;

    esp_err_t Init();
    void Feed(const int16_t* samples, int count);
    // Wake inference is useful only while the application is idle. Disabling
    // it frees CPU for Opus/UI and discards any stale pre-transition audio.
    void SetEnabled(bool enabled);
    void SetWakeCallback(WakeCallback cb) { wake_cb_ = std::move(cb); }
    bool IsReady() const { return ready_; }

private:
    MicroWakeWordDetector() = default;
    ~MicroWakeWordDetector() = default;

    static void InferenceTask(void* arg);
    bool ProcessAudio(const int16_t* samples, int count);
    bool ProcessFeatures(const int8_t* features);
    void ResetDetectionWindow();

    struct Impl;
    Impl* impl_ = nullptr;
    WakeCallback wake_cb_;

    // Feed() has one producer (AFE task), so this carry buffer does not need a
    // mutex. A short queue preserves consecutive 10 ms model features while
    // remaining bounded.
    static constexpr int kPcmFrameSamples = 160;
    int16_t feed_carry_[kPcmFrameSamples] = {};
    int feed_carry_pos_ = 0;

    volatile bool ready_ = false;
    volatile bool enabled_ = false;
    volatile bool reset_requested_ = false;
};

#endif  // MICRO_WAKE_WORD_H
