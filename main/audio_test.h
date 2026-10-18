#ifndef AUDIO_TEST_H
#define AUDIO_TEST_H

#include <driver/i2s_std.h>
#include <driver/gpio.h>

// Simple record/playback test: hold VOL_DOWN to record, release to play back
class AudioTest {
public:
    static AudioTest& GetInstance();
    void Init(i2s_chan_handle_t mic, i2s_chan_handle_t spk, gpio_num_t button_gpio);
    void Start();

private:
    AudioTest() = default;
    static void TaskFunc(void* arg);

    i2s_chan_handle_t mic_handle_ = nullptr;
    i2s_chan_handle_t spk_handle_ = nullptr;
    gpio_num_t button_gpio_ = GPIO_NUM_NC;

    // Recording buffer in PSRAM (max 10s at 16kHz mono 16-bit = 320KB)
    static constexpr int MAX_RECORD_SAMPLES = 16000 * 10;
    int16_t* record_buf_ = nullptr;
    int record_len_ = 0;  // samples recorded
};

#endif // AUDIO_TEST_H
