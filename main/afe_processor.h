#ifndef AFE_PROCESSOR_H
#define AFE_PROCESSOR_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#include <functional>

// Forward declarations — full types only needed in .cc
#if __has_include("esp_afe_sr_models.h")
#include <esp_afe_sr_iface.h>
#define HAS_ESP_SR 1
#else
typedef struct esp_afe_sr_iface_t esp_afe_sr_iface_t;
typedef struct esp_afe_sr_data_t  esp_afe_sr_data_t;
#define HAS_ESP_SR 0
#endif

// Unified audio front-end processor:
//   - ESP-SR AFE (VC mode) with AEC (software reference) + NS
//   - AEC-cleaned output is queued to the standalone microWakeWord task
//   - Local VAD disabled — server-side Silero VAD handles speech detection
class AfeProcessor {
public:
    static AfeProcessor& GetInstance();

    AfeProcessor(const AfeProcessor&) = delete;
    AfeProcessor& operator=(const AfeProcessor&) = delete;

    using AudioCallback = std::function<void(const int16_t*, int)>;
    using VadCallback   = std::function<void(bool is_speech)>;
    using WakeCallback  = std::function<void()>;

    esp_err_t Init();
    void Start();
    void Stop();

    // Feed mic + speaker reference PCM for AEC processing.
    // Both buffers are 16-bit mono, `count` samples each.
    // `ref` may be NULL (treated as all-zeros / no reference).
    void Feed(const int16_t* mic, const int16_t* ref, int count);

    void SetAudioCallback(AudioCallback cb) { audio_cb_ = cb; }
    void SetVadCallback(VadCallback cb)     { vad_cb_ = cb; }
    void SetWakeCallback(WakeCallback cb)   { wake_cb_ = cb; }

    bool IsRunning() const { return running_; }

private:
    AfeProcessor() = default;
    ~AfeProcessor() = default;

    static void ProcessTask(void* arg);

    // ESP-SR AFE (VC mode: AEC + NS, no VAD)
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t*        afe_data_  = nullptr;

    // AFE feed buffer: interleaved [mic, ref, mic, ref, ...] for "MR" format
    int16_t* feed_buf_     = nullptr;
    int      feed_buf_pos_ = 0;       // position in per-channel samples
    int      feed_chunk_size_ = 0;    // samples per channel per feed
    static constexpr int kInputChannels = 2;  // 1 mic + 1 reference

    // Callbacks
    AudioCallback audio_cb_;
    VadCallback   vad_cb_;
    WakeCallback  wake_cb_;

    volatile bool running_     = false;
    bool          is_speaking_ = false;
};

#endif // AFE_PROCESSOR_H
