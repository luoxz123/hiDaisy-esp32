#ifndef MSG_ROUTER_H
#define MSG_ROUTER_H

#include "hilily_mqtt_client.h"
#include "state_machine.h"
#include "udp_client.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>

struct AudioMeta {
  char format[16]; // "pcm" or "opus"
  int sample_rate;
  int channels;
  int bit_depth;
};

// Chunk descriptor passed through audio queues (pointer + length).
// Ownership of data transfers with the struct; receiver must free data.
struct AudioChunk {
  uint8_t *data;
  int len;
};

class MsgRouter {
public:
  using AudioMetaCallback = std::function<void(const AudioMeta &meta)>;
  using TtsEndCallback = std::function<void()>;
  using TtsStartCallback = std::function<void()>;
  using SttResultCallback = std::function<void(const char *text)>;
  using EndDialogueCallback = std::function<void(const char *reason)>;

  static MsgRouter &GetInstance();

  MsgRouter(const MsgRouter &) = delete;
  MsgRouter &operator=(const MsgRouter &) = delete;

  esp_err_t Init(StateMachine *state_machine, QueueHandle_t audio_queue);
  void Start();
  void FlushAudioQueue();

  void OnAudioMeta(AudioMetaCallback callback) {
    audio_meta_callback_ = std::move(callback);
  }
  void OnTtsEnd(TtsEndCallback callback) {
    tts_end_callback_ = std::move(callback);
  }
  void OnTtsStart(TtsStartCallback callback) {
    tts_start_callback_ = std::move(callback);
  }
  void OnSttResult(SttResultCallback callback) {
    stt_result_callback_ = std::move(callback);
  }
  void OnEndDialogue(EndDialogueCallback callback) {
    end_dialogue_callback_ = std::move(callback);
  }

private:
  MsgRouter() = default;
  ~MsgRouter() = default;

  static void RouterTask(void *arg);
  static void AudioWriterTask(void *arg);
  void HandleJsonMessage(const char *json_str, int len);
  void HandleBinaryMessage(const uint8_t *data, int len);

  StateMachine *state_machine_ = nullptr;
  QueueHandle_t audio_queue_ = nullptr;       // downstream queue owned by AudioService
  QueueHandle_t json_queue_ = nullptr;
  QueueHandle_t audio_write_queue_ =
      nullptr; // AudioChunk queue for backpressure

  AudioMetaCallback audio_meta_callback_;
  TtsEndCallback tts_end_callback_;
  TtsStartCallback tts_start_callback_;
  SttResultCallback stt_result_callback_;
  EndDialogueCallback end_dialogue_callback_;
};

#endif // MSG_ROUTER_H
