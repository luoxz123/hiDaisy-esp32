#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include <esp_err.h>
#include <esp_attr.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>

static constexpr uint32_t LONG_PRESS_MS = 3000;  // 3 seconds for long press

class ButtonManager {
public:
    using PressCallback = std::function<void()>;

    static ButtonManager& GetInstance();

    ButtonManager(const ButtonManager&) = delete;
    ButtonManager& operator=(const ButtonManager&) = delete;

    esp_err_t Init(int gpio_pin);
    void OnPress(PressCallback callback) { press_callback_ = std::move(callback); }
    void OnLongPress(PressCallback callback) { long_press_callback_ = std::move(callback); }

private:
    ButtonManager() = default;
    ~ButtonManager() = default;

    static void IRAM_ATTR IsrHandler(void* arg);
    static void ButtonTask(void* arg);

    int gpio_pin_ = -1;
    PressCallback press_callback_;
    PressCallback long_press_callback_;
    volatile uint32_t last_event_time_ = 0;
};

#endif // BUTTON_MANAGER_H
