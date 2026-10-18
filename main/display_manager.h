#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "device_state.h"
#include "face_renderer.h"

#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

// Number of bars in waveform display
#define WAVEFORM_BAR_COUNT 40

class DisplayManager {
public:
    static DisplayManager& GetInstance();

    DisplayManager(const DisplayManager&) = delete;
    DisplayManager& operator=(const DisplayManager&) = delete;

    esp_err_t Init();
    void Start();

    // Thread-safe UI updates (schedules onto UI task)
    void SetDeviceState(DeviceState state);
    void SetEmotion(EmotionType emotion, float intensity = 1.0f);
    void SetBatteryLevel(uint8_t percent);
    void SetText(const char* text);
    void SetWifiConnected(bool connected);
    void SetWifiRssi(int8_t rssi);
    void UpdateTimeDisplay();

    // Show a full-screen overlay message (hides face, auto-dismisses after timeout_ms)
    void ShowOverlay(const char* text, uint32_t timeout_ms = 15000);
    void HideOverlay();

    // Boot animation (water-ripple effect with "initializing" text)
    void ShowBootAnimation();
    void HideBootAnimation();

    SemaphoreHandle_t GetMutex() { return ui_mutex_; }

private:
    DisplayManager() = default;
    ~DisplayManager() = default;

    static void UiTask(void* arg);
    void InitLvglDisplay();
    void CreateUI();
    void UpdateWifiIcon();
    void UpdateBatteryIcon();
    void UpdateWaveform();

    SemaphoreHandle_t ui_mutex_ = NULL;

    // LVGL objects
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* text_label_ = nullptr;
    lv_obj_t* battery_bar_ = nullptr;
    static constexpr int kBatteryCellCount = 5;
    lv_obj_t* battery_cells_[kBatteryCellCount] = {};
    lv_obj_t* wifi_icon_ = nullptr;
    lv_obj_t* time_label_ = nullptr;

    // Waveform display (shown during LISTENING)
    lv_obj_t* waveform_container_ = nullptr;
    lv_obj_t* waveform_bars_[WAVEFORM_BAR_COUNT] = {};
    bool waveform_visible_ = false;

    FaceRenderer face_renderer_;

    // Overlay
    lv_obj_t* overlay_bg_ = nullptr;
    lv_obj_t* overlay_label_ = nullptr;
    esp_timer_handle_t overlay_timer_ = nullptr;
    static void OverlayTimerCallback(void* arg);

    // Boot animation
    static constexpr int kBootRippleCount = 3;
    lv_obj_t* boot_anim_container_ = nullptr;
    lv_obj_t* boot_center_dot_ = nullptr;
    lv_obj_t* boot_text_label_ = nullptr;
    lv_obj_t* boot_ripples_[kBootRippleCount] = {};
    esp_timer_handle_t boot_anim_timer_ = nullptr;
    uint32_t boot_anim_frame_ = 0;
    bool boot_anim_active_ = false;
    static void BootAnimTimerCallback(void* arg);
    void UpdateBootAnimation();

    DeviceState current_state_ = kDeviceStateStarting;
    EmotionType current_emotion_ = kEmotionNeutral;
    uint8_t battery_percent_ = 0;
    bool wifi_connected_ = false;
    int8_t wifi_rssi_ = -127;
};

#endif // DISPLAY_MANAGER_H
