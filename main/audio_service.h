#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include "device_state.h"
#include "msg_router.h"

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <freertos/ringbuf.h>
#include <driver/i2s_std.h>

// Audio frame sizes
#define AUDIO_FRAME_MS          100
#define AUDIO_FRAME_SAMPLES     (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)  // 1600
#define AUDIO_FRAME_BYTES       (AUDIO_FRAME_SAMPLES * sizeof(int16_t))       // 3200

// Max items in downstream audio queue (Opus packets, PSRAM-backed)
// Server sends TTS in burst (faster than real-time), need large buffer.
// 4096 items ≈ 245s at 60ms per frame; queue metadata ~48KB in PSRAM.
#define DOWNSTREAM_QUEUE_SIZE   4096

// Start playback after this many queued items (~300ms at 60ms Opus frames)
// Higher watermark reduces initial distortion from underrun
#define JITTER_ITEMS_WATERMARK  5

enum AudioFormat {
    kAudioFormatPCM,
    kAudioFormatOpus
};

class AudioService {
public:
    static AudioService& GetInstance();

    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;

    esp_err_t Init(i2s_chan_handle_t spk_handle, i2s_chan_handle_t mic_handle);
    void Start();

    // Get the downstream audio queue (for MsgRouter to push AudioChunks into)
    QueueHandle_t GetDownstreamQueue() const { return downstream_queue_; }

    // Control
    void StartCapture();
    void StopCapture();
    void StartPlayback();
    void StopPlayback();
    // Ask PlaybackTask to ramp the current frame to silence before stopping.
    void FadeOutPlayback(uint32_t timeout_ms = 150);
    void ClearBuffers();

    // Signal that server has finished sending TTS audio.
    // PlaybackTask will drain remaining buffer then auto-stop.
    void SignalTtsEnd() { tts_end_received_ = true; }

    // Reset TTS end flag (called on new tts_start to cancel premature drain
    // when server sends multi-segment TTS responses)
    void ResetTtsEnd() { tts_end_received_ = false; }

    // Register callback fired when playback drain completes (TTS fully played)
    using PlaybackCompleteCallback = std::function<void()>;
    void OnPlaybackComplete(PlaybackCompleteCallback cb) { playback_complete_cb_ = cb; }

    // Set downstream audio format based on audio_meta
    void SetDownstreamFormat(AudioFormat format);

    // Send PCM data upstream (currently unused — encoding done in OpusEncodeTask)
    // void SendAudioUpstream(const int16_t* samples, int count);

    // Decode Opus packet to PCM
    int DecodeOpus(const uint8_t* data, int len, int16_t* pcm_out, int max_samples);

    bool IsCapturing() const { return capturing_; }
    bool IsPlaying() const { return playing_; }

    // Play embedded prompt tone (blocking, ~180ms)
    void PlayPrompt(bool start = true);

    // Play local spoken acknowledgement after a Daisy wake (blocking).
    void PlayWakeGreeting();

    // Play alarm tone for reminders (blocking, ~1.5s: 3 beeps)
    void PlayAlarmTone();

    // Audio level ring buffer for waveform display (written by CaptureTask)
    static constexpr int kAudioLevelBufSize = 60;  // ~6s at 100ms per frame
    volatile uint16_t audio_levels_[kAudioLevelBufSize] = {};
    volatile int audio_level_idx_ = 0;

    // Get recent audio levels for waveform display
    // Copies up to `count` most recent levels into `out`, returns actual count
    int GetRecentLevels(uint16_t* out, int count) const;

    // Exclusive mic access for AudioTest (pauses CaptureTask I2S reads)
    void SetMicExclusive(bool exclusive) { mic_exclusive_ = exclusive; }

    // Speaker reference data for AEC: reads `samples` of 16-bit PCM from the
    // reference ring buffer.  If fewer samples are available, the remainder is
    // zero-filled.  Returns the number of real (non-zero) samples copied.
    int ReadReferenceData(int16_t* buf, size_t samples);

private:
    AudioService() = default;
    ~AudioService() = default;

    static void CaptureTask(void* arg);
    static void OpusEncodeTask(void* arg);
    static void PlaybackTask(void* arg);

    i2s_chan_handle_t spk_handle_ = NULL;
    i2s_chan_handle_t mic_handle_ = NULL;

    QueueHandle_t downstream_queue_ = NULL;      // Server -> Speaker (AudioChunk items)
    StreamBufferHandle_t encode_stream_ = NULL;   // CaptureTask -> OpusEncodeTask
    bool initialized_ = false;
    bool tasks_started_ = false;
    volatile bool capturing_ = false;
    volatile bool playing_ = false;
    volatile bool tts_end_received_ = false;  // Server signals TTS stream complete
    volatile bool fade_out_requested_ = false;
    SemaphoreHandle_t fade_out_done_ = nullptr;
    volatile bool mic_exclusive_ = false;     // AudioTest takes exclusive mic access

    AudioFormat downstream_format_ = kAudioFormatPCM;
    PlaybackCompleteCallback playback_complete_cb_;

    // Speaker reference ring buffer for AEC software reference channel.
    // PlaybackTask writes volume-adjusted 16-bit PCM here; CaptureTask reads
    // via ReadReferenceData() to build interleaved [mic, ref] for AFE.
    // 400ms @ 16kHz mono 16-bit = 6400 samples = 12800 bytes
    static constexpr size_t kSpeakerRefBufBytes = 12800;
    StreamBufferHandle_t speaker_ref_buf_ = nullptr;

    // Opus encoder/decoder state (esp_audio_codec handles)
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    int encoder_frame_size_ = 0;   // samples per encoder frame
    int decoder_frame_size_ = 0;   // samples per decoder frame
    esp_err_t InitOpus();
    bool InitEncoder();  // Lazy encoder init (called from OpusEncodeTask)
    bool InitDecoder();  // Lazy decoder init (called from StartPlayback)
    void PlayPcmBlocking(const int16_t* data, int samples);


};

#endif // AUDIO_SERVICE_H
