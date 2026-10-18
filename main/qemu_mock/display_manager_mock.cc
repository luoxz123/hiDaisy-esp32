/*
 * QEMU mock display manager.
 * Uses LVGL with a dummy flush callback (no real display output).
 * All UI widgets, face rendering, and state updates work normally.
 */
#include "display_manager.h"
#include "board.h"
#include "pin_config.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <ctime>

static const char *TAG = "display_qemu";

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = NULL;
static lv_disp_drv_t disp_drv;

DisplayManager& DisplayManager::GetInstance()
{
    static DisplayManager instance;
    return instance;
}

// Dummy flush: just mark ready (no actual display output)
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    lv_disp_flush_ready(drv);
}

esp_err_t DisplayManager::Init()
{
    ui_mutex_ = xSemaphoreCreateMutex();
    if (!ui_mutex_) return ESP_FAIL;

    ESP_LOGI(TAG, "[QEMU] Display init (dummy driver, LVGL active)");

    // Initialize LVGL
    lv_init();

    // Allocate draw buffer (1/10 of screen)
    size_t buf_size = LCD_WIDTH * (LCD_HEIGHT / 10);
    buf1 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_DEFAULT);
    if (!buf1) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer");
        return ESP_FAIL;
    }

    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, buf_size);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    CreateUI();
    face_renderer_.Init(screen_);

    board_set_brightness(Settings::GetInstance().GetBrightness());

    ESP_LOGI(TAG, "[QEMU] Display initialized (LVGL dummy %dx%d)", LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

void DisplayManager::CreateUI()
{
    screen_ = lv_scr_act();
    lv_obj_set_style_bg_color(screen_, lv_color_black(), 0);

    status_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(status_label_, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(status_label_, "Starting...");

    text_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(text_label_, lv_color_white(), 0);
    lv_obj_set_width(text_label_, LCD_WIDTH - 20);
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(text_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(text_label_, "");

    battery_bar_ = lv_bar_create(screen_);
    lv_obj_set_size(battery_bar_, 28, 10);
    lv_obj_align(battery_bar_, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_bar_set_range(battery_bar_, 0, 100);
    lv_bar_set_value(battery_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(battery_bar_, lv_color_make(40, 40, 40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_bar_, lv_color_make(60, 80, 60), LV_PART_INDICATOR);
    lv_obj_add_flag(battery_bar_, LV_OBJ_FLAG_HIDDEN);

    wifi_icon_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(wifi_icon_, lv_color_make(180, 60, 60), 0);
    lv_obj_align(wifi_icon_, LV_ALIGN_TOP_LEFT, 5, 3);
    lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);

    time_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(time_label_, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 3);
    lv_label_set_text(time_label_, "");
}

void DisplayManager::Start()
{
    xTaskCreatePinnedToCore(UiTask, "ui", 16384, this, 5, NULL, 0);
}

void DisplayManager::UiTask(void* arg)
{
    auto* self = static_cast<DisplayManager*>(arg);
    ESP_LOGI(TAG, "[QEMU] UI task started");

    uint32_t last_ms = (uint32_t)(esp_timer_get_time() / 1000);

    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t elapsed = now_ms - last_ms;
        last_ms = now_ms;
        lv_tick_inc(elapsed);

        if (xSemaphoreTake(self->ui_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(self->ui_mutex_);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void DisplayManager::SetDeviceState(DeviceState state)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    current_state_ = state;
    const char* status_text = device_state_to_string(state);
    if (status_label_) {
        lv_label_set_text(status_label_, status_text);
    }
    ESP_LOGI(TAG, "[QEMU] State -> %s", status_text);
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::SetEmotion(EmotionType emotion, float intensity)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    current_emotion_ = emotion;
    face_renderer_.SetTargetEmotion(emotion, intensity);
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::SetBatteryLevel(uint8_t percent)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    battery_percent_ = percent;
    UpdateBatteryIcon();
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::UpdateBatteryIcon()
{
    if (!battery_bar_) return;
    if (battery_percent_ > 90) {
        lv_obj_add_flag(battery_bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(battery_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(battery_bar_, battery_percent_, LV_ANIM_ON);
        if (battery_percent_ < 20) {
            lv_obj_set_style_bg_color(battery_bar_, lv_color_make(180, 40, 40), LV_PART_INDICATOR);
        } else if (battery_percent_ < 60) {
            lv_obj_set_style_bg_color(battery_bar_, lv_color_make(140, 120, 40), LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_bg_color(battery_bar_, lv_color_make(60, 120, 60), LV_PART_INDICATOR);
        }
    }
}

void DisplayManager::SetText(const char* text)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    if (text_label_ && text) {
        lv_label_set_text(text_label_, text);
    }
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::SetWifiConnected(bool connected)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    wifi_connected_ = connected;
    if (!connected) wifi_rssi_ = -127;
    UpdateWifiIcon();
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::SetWifiRssi(int8_t rssi)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    wifi_rssi_ = rssi;
    UpdateWifiIcon();
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::UpdateWifiIcon()
{
    if (!wifi_icon_) return;
    lv_obj_clear_flag(wifi_icon_, LV_OBJ_FLAG_HIDDEN);
    if (!wifi_connected_) {
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI " " LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_make(180, 60, 60), 0);
    } else if (wifi_rssi_ > -65) {
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_make(60, 160, 60), 0);
    } else if (wifi_rssi_ > -75) {
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_make(160, 140, 40), 0);
    } else {
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_make(100, 100, 100), 0);
    }
}

void DisplayManager::UpdateTimeDisplay()
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    if (!time_label_) {
        xSemaphoreGive(ui_mutex_);
        return;
    }
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year + 1900 >= 2025) {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        lv_label_set_text(time_label_, buf);
    } else {
        lv_label_set_text(time_label_, "");
    }
    xSemaphoreGive(ui_mutex_);
}
