#ifndef APPLICATION_H
#define APPLICATION_H

#include "device_state.h"
#include "state_machine.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <cstdint>

// Main event bits
#define EVT_SCHEDULE              (1 << 0)
#define EVT_WIFI_CONNECTED        (1 << 1)
#define EVT_WIFI_DISCONNECTED     (1 << 2)
#define EVT_WS_CONNECTED          (1 << 3)
#define EVT_WS_DISCONNECTED       (1 << 4)
#define EVT_STATE_CHANGED         (1 << 5)
#define EVT_BUTTON_PRESSED        (1 << 6)
#define EVT_WAKE_WORD_DETECTED    (1 << 7)
#define EVT_SEND_AUDIO            (1 << 8)
#define EVT_REMINDER_TRIGGERED    (1 << 9)
#define EVT_TIME_SYNCED           (1 << 10)
#define EVT_START_LISTENING       (1 << 11)
#define EVT_STOP_LISTENING        (1 << 12)
#define EVT_BUTTON_LONG_PRESSED   (1 << 13)
#define EVT_PROVISIONING_STARTED  (1 << 14)
#define EVT_LOW_BATTERY            (1 << 15)

// Listening modes (determines how recording ends)
enum ListeningMode {
    kListeningModeAutoStop,    // Wake word: VAD silence auto-stop
    kListeningModeManualStop,  // Button: second press to stop
};

class Application {
public:
    static Application& GetInstance();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void Run();

    StateMachine& GetStateMachine() { return state_machine_; }
    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }
    EmotionType GetEmotion() const { return state_machine_.GetEmotion(); }

    void Schedule(std::function<void()>&& callback);
    void NotifyEvent(uint32_t event_bits);
    EventGroupHandle_t GetEventGroup() { return event_group_; }

private:
    Application();
    ~Application();

    StateMachine state_machine_;
    EventGroupHandle_t event_group_;
    std::mutex schedule_mutex_;
    std::deque<std::function<void()>> scheduled_tasks_;
    int64_t listening_started_ms_ = 0;
    int64_t stop_sent_ms_ = 0;  // When listen(stop) was sent, for fallback timeout
    bool listen_sent_ = false;  // True if listen msg already sent before state transition

    // Continuous conversation mode: auto re-enter listening after server TTS ends
    volatile bool continuous_mode_ = false;
    bool continuous_relisten_ = false;  // Suppress prompt tone on re-entry

    // Listening mode state
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    volatile bool vad_speech_detected_ = false;  // Speech seen in current session
    volatile bool vad_is_speech_ = false;         // Current VAD state (speech/silence)
    volatile int64_t silence_start_ms_ = 0;       // When silence started (for auto-stop)
    volatile int64_t speech_start_ms_ = 0;         // When current speech started (for min duration)

    // Segmented upload state (Issue #153)
    bool needs_segment_ = false;       // True when segment timer expired, awaiting pause
    int segment_count_ = 0;            // Segments sent in current session
    int64_t segment_start_ms_ = 0;     // When current segment started

    // Reminder state: stores the reminder_id that triggered most recently
    std::string triggered_reminder_id_;
    int64_t last_low_battery_reminder_ms_ = 0;
    uint8_t pending_low_battery_percent_ = 0;
    uint32_t pending_low_battery_voltage_mv_ = 0;
    uint32_t pending_low_battery_sequence_ = 0;
    bool low_battery_pending_ = false;

    // Event handlers
    void HandleWifiConnected();
    void HandleWifiDisconnected();
    void HandleButtonPressed();
    void HandleButtonLongPressed();
    void HandleScheduledTasks();
    void ProcessListeningTimeout();
    void SendListenStop();
    void SendSegmentBoundary();
    void SendGoodbye();
    void HandleProvisioningStarted();
    void HandleDeviceOnboarding(const std::string &server_url, const std::string &mac_address);
    void ScheduleServerRetry(int delay_ms);

    esp_timer_handle_t server_retry_timer_ = nullptr;
};

#endif // APPLICATION_H
