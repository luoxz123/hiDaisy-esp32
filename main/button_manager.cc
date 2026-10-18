#include "button_manager.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_timer.h>

static const char *TAG = "button";
static const uint32_t DEBOUNCE_MS = 50;

// Queue carries GPIO level: 0 = pressed (active low), 1 = released
static QueueHandle_t button_queue = NULL;

ButtonManager& ButtonManager::GetInstance()
{
    static ButtonManager instance;
    return instance;
}

esp_err_t ButtonManager::Init(int gpio_pin)
{
    gpio_pin_ = gpio_pin;

    button_queue = xQueueCreate(8, sizeof(uint8_t));
    if (!button_queue) return ESP_FAIL;

    // Configure GPIO with both edges to detect press and release
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << gpio_pin);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)gpio_pin_, IsrHandler, this);

    xTaskCreatePinnedToCore(ButtonTask, "button", 2048, this, 5, NULL, 0);

    ESP_LOGI(TAG, "Button manager initialized (GPIO%d, long_press=%lums)",
             gpio_pin_, (unsigned long)LONG_PRESS_MS);
    return ESP_OK;
}

void IRAM_ATTR ButtonManager::IsrHandler(void* arg)
{
    auto* self = static_cast<ButtonManager*>(arg);
    uint8_t level = gpio_get_level((gpio_num_t)self->gpio_pin_);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(button_queue, &level, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

void ButtonManager::ButtonTask(void* arg)
{
    auto* self = static_cast<ButtonManager*>(arg);
    uint8_t level;
    uint32_t press_start_ms = 0;
    bool pressed = false;
    bool long_fired = false;

    while (true) {
        // Poll with timeout to detect long-press while held
        BaseType_t got = xQueueReceive(button_queue, &level, pdMS_TO_TICKS(100));

        if (got == pdTRUE) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

            // Debounce
            if (now - self->last_event_time_ < DEBOUNCE_MS) continue;
            self->last_event_time_ = now;

            if (level == 0 && !pressed) {
                // Button pressed (active low)
                pressed = true;
                long_fired = false;
                press_start_ms = now;
            } else if (level == 1 && pressed) {
                // Button released
                pressed = false;
                uint32_t duration = now - press_start_ms;

                if (!long_fired && duration < LONG_PRESS_MS) {
                    ESP_LOGI(TAG, "Button pressed (%lums)", (unsigned long)duration);
                    if (self->press_callback_) {
                        self->press_callback_();
                    }
                }
            }
        }

        // Check for long-press while still held
        if (pressed && !long_fired) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - press_start_ms >= LONG_PRESS_MS) {
                long_fired = true;
                ESP_LOGI(TAG, "Button long-pressed (>%lums)", (unsigned long)LONG_PRESS_MS);
                if (self->long_press_callback_) {
                    self->long_press_callback_();
                }
            }
        }
    }
}
