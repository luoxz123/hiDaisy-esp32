#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <functional>

// Use real ESP-SR type definitions when available; keep a fallback
// so this header can still compile in environments without ESP-SR.
#if __has_include("esp_afe_sr_iface.h")
#include <esp_afe_sr_iface.h>
#else
typedef struct esp_afe_sr_iface_t esp_afe_sr_iface_t;
typedef struct esp_afe_sr_data_t esp_afe_sr_data_t;
#endif

class WakeWordDetector {
public:
    static WakeWordDetector& GetInstance();

    WakeWordDetector(const WakeWordDetector&) = delete;
    WakeWordDetector& operator=(const WakeWordDetector&) = delete;

    using WakeCallback = std::function<void()>;

    esp_err_t Init();
    void Start();
    void Stop();

    void SetWakeCallback(WakeCallback cb) { wake_callback_ = cb; }

    // Feed audio samples for detection (called from capture task)
    void Feed(const int16_t* samples, int count);

    bool IsRunning() const { return running_; }

private:
    WakeWordDetector() = default;
    ~WakeWordDetector() = default;

    static void DetectTask(void* arg);

    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;

    WakeCallback wake_callback_;
    bool running_ = false;
    int feed_chunk_size_ = 0;

    // Audio feed buffer (accumulate until chunk size reached)
    int16_t* feed_buf_ = nullptr;
    int feed_buf_pos_ = 0;
};

#endif // WAKE_WORD_H
