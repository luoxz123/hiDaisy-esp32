#include "display_manager.h"
#include "audio_service.h"
#include "board.h"
#include "pin_config.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <cmath>
#include <ctime>

static const char *TAG = "display";

// LVGL display buffer
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;
static lv_disp_drv_t disp_drv;

static esp_lcd_panel_handle_t panel_handle = NULL;

DisplayManager& DisplayManager::GetInstance()
{
    static DisplayManager instance;
    return instance;
}

// LCD flush callback
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    int x_start = area->x1;
    int y_start = area->y1;
    int x_end = area->x2 + 1;
    int y_end = area->y2 + 1;

    // ST7789 expects big-endian RGB565, ESP32 is little-endian — swap bytes
    int num_pixels = (x_end - x_start) * (y_end - y_start);
    uint16_t* buf = (uint16_t*)color_map;
    for (int i = 0; i < num_pixels; i++) {
        buf[i] = __builtin_bswap16(buf[i]);
    }

    esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, color_map);
    lv_disp_flush_ready(drv);
}

esp_err_t DisplayManager::Init()
{
    ui_mutex_ = xSemaphoreCreateMutex();
    if (!ui_mutex_) return ESP_FAIL;

    // Initialize ST7789 panel over SPI
    esp_lcd_panel_io_handle_t io_handle = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = PIN_LCD_DC;
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.pclk_hz = LCD_SPI_FREQ_HZ;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 3;
    io_config.trans_queue_depth = 10;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Initialize LVGL
    lv_init();

    // Allocate draw buffers (1/10 of screen)
    size_t buf_size = LCD_WIDTH * (LCD_HEIGHT / 10);
    buf1 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    buf2 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffers");
        return ESP_FAIL;
    }

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_size);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    CreateUI();

    // Initialize face renderer (must be after LVGL + screen creation)
    face_renderer_.Init(screen_);

    // Apply saved brightness from NVS
    board_set_brightness(Settings::GetInstance().GetBrightness());

    ESP_LOGI(TAG, "Display initialized (ST7789 %dx%d, brightness=%d%%)",
             LCD_WIDTH, LCD_HEIGHT, Settings::GetInstance().GetBrightness());
    return ESP_OK;
}

void DisplayManager::CreateUI()
{
    screen_ = lv_scr_act();
    lv_obj_set_style_bg_color(screen_, lv_color_black(), 0);

    // Face renderer canvas is created later in Init() via face_renderer_.Init()

    // Status label (below emotion)
    status_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(status_label_, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(status_label_, "Starting...");

    // Text display (bottom)
    text_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(text_label_, lv_color_white(), 0);
    lv_obj_set_width(text_label_, LCD_WIDTH - 20);
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(text_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(text_label_, "");

    // Five-cell battery indicator. It remains hidden only until the first ADC
    // reading arrives, then stays visible even when the battery is full.
    battery_bar_ = lv_obj_create(screen_);
    lv_obj_set_size(battery_bar_, 42, 12);
    lv_obj_align(battery_bar_, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_bg_opa(battery_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(battery_bar_, 1, 0);
    lv_obj_set_style_border_color(battery_bar_, lv_color_make(150, 150, 150), 0);
    lv_obj_set_style_radius(battery_bar_, 2, 0);
    lv_obj_set_style_pad_all(battery_bar_, 1, 0);
    lv_obj_clear_flag(battery_bar_, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < kBatteryCellCount; ++i) {
        battery_cells_[i] = lv_obj_create(battery_bar_);
        lv_obj_set_size(battery_cells_[i], 6, 8);
        lv_obj_set_pos(battery_cells_[i], 1 + i * 7, 0);
        lv_obj_set_style_border_width(battery_cells_[i], 0, 0);
        lv_obj_set_style_radius(battery_cells_[i], 1, 0);
        lv_obj_set_style_pad_all(battery_cells_[i], 0, 0);
        lv_obj_clear_flag(battery_cells_[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_flag(battery_bar_, LV_OBJ_FLAG_HIDDEN); // Hidden until battery data available

    // WiFi icon (top left) - shows signal strength or disconnected
    wifi_icon_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(wifi_icon_, lv_color_make(180, 60, 60), 0);
    lv_obj_align(wifi_icon_, LV_ALIGN_TOP_LEFT, 5, 3);
    lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);

    // Time label (top center)
    time_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_color(time_label_, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 3);
    lv_label_set_text(time_label_, "");

    // Waveform container (hidden by default, shown during LISTENING)
    waveform_container_ = lv_obj_create(screen_);
    lv_obj_set_size(waveform_container_, 220, 120);
    lv_obj_align(waveform_container_, LV_ALIGN_CENTER, 0, -15);
    lv_obj_set_style_bg_opa(waveform_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(waveform_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(waveform_container_, 0, 0);
    lv_obj_clear_flag(waveform_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(waveform_container_, LV_OBJ_FLAG_HIDDEN);

    // Create waveform bars
    int bar_w = 4;
    int gap = 1;
    int total_w = WAVEFORM_BAR_COUNT * (bar_w + gap) - gap;
    int start_x = (220 - total_w) / 2;
    for (int i = 0; i < WAVEFORM_BAR_COUNT; i++) {
        waveform_bars_[i] = lv_obj_create(waveform_container_);
        lv_obj_set_size(waveform_bars_[i], bar_w, 4);
        lv_obj_set_pos(waveform_bars_[i], start_x + i * (bar_w + gap), 116);
        lv_obj_set_style_bg_color(waveform_bars_[i], lv_color_make(60, 180, 120), 0);
        lv_obj_set_style_bg_opa(waveform_bars_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(waveform_bars_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(waveform_bars_[i], 2, 0);
        lv_obj_clear_flag(waveform_bars_[i], LV_OBJ_FLAG_SCROLLABLE);
    }
}

void DisplayManager::Start()
{
    xTaskCreatePinnedToCoreWithCaps(UiTask, "ui", 16384, this, 5, NULL, 0, MALLOC_CAP_SPIRAM);
}

void DisplayManager::UiTask(void* arg)
{
    auto* self = static_cast<DisplayManager*>(arg);
    ESP_LOGI(TAG, "UI task started (Core %d)", xPortGetCoreID());

    uint32_t last_ms = (uint32_t)(esp_timer_get_time() / 1000);

    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t elapsed = now_ms - last_ms;
        last_ms = now_ms;
        lv_tick_inc(elapsed);

        if (xSemaphoreTake(self->ui_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (self->waveform_visible_) {
                self->UpdateWaveform();
            }
            lv_timer_handler();
            xSemaphoreGive(self->ui_mutex_);
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // ~50 FPS max
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

    // Show waveform during listening, hide otherwise
    bool show_waveform = (state == kDeviceStateListening);
    if (show_waveform != waveform_visible_) {
        waveform_visible_ = show_waveform;
        if (waveform_container_) {
            if (show_waveform) {
                face_renderer_.SetVisible(false);
                lv_obj_clear_flag(waveform_container_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(waveform_container_);
            } else {
                lv_obj_add_flag(waveform_container_, LV_OBJ_FLAG_HIDDEN);
                face_renderer_.SetVisible(true);
            }
        }
    }
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

    lv_obj_clear_flag(battery_bar_, LV_OBJ_FLAG_HIDDEN);
    const int filled = battery_percent_ == 0
                           ? 0
                           : (battery_percent_ + 19) / 20;
    const bool low = battery_percent_ <= 20;
    const lv_color_t active = low ? lv_color_make(210, 35, 35)
                                  : lv_color_make(55, 180, 85);
    const lv_color_t empty = lv_color_make(45, 45, 45);
    for (int i = 0; i < kBatteryCellCount; ++i) {
        lv_obj_set_style_bg_color(battery_cells_[i],
                                  i < filled ? active : empty, 0);
        lv_obj_set_style_bg_opa(battery_cells_[i], LV_OPA_COVER, 0);
    }
    lv_obj_move_foreground(battery_bar_);
}

void DisplayManager::SetText(const char* text)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    if (text_label_ && text) {
        lv_label_set_text(text_label_, text);
    }
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::ShowOverlay(const char* text, uint32_t timeout_ms)
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);

    // Create overlay background (full screen, dark semi-transparent)
    if (!overlay_bg_) {
        overlay_bg_ = lv_obj_create(screen_);
        lv_obj_set_size(overlay_bg_, LCD_WIDTH, LCD_HEIGHT);
        lv_obj_set_pos(overlay_bg_, 0, 0);
        lv_obj_set_style_bg_color(overlay_bg_, lv_color_make(0, 0, 0), 0);
        lv_obj_set_style_bg_opa(overlay_bg_, LV_OPA_90, 0);
        lv_obj_set_style_border_width(overlay_bg_, 0, 0);
        lv_obj_set_style_pad_all(overlay_bg_, 15, 0);
        lv_obj_clear_flag(overlay_bg_, LV_OBJ_FLAG_SCROLLABLE);

        overlay_label_ = lv_label_create(overlay_bg_);
        lv_obj_set_style_text_color(overlay_label_, lv_color_make(0, 220, 100), 0);
        lv_obj_set_width(overlay_label_, LCD_WIDTH - 40);
        lv_label_set_long_mode(overlay_label_, LV_LABEL_LONG_WRAP);
        lv_obj_align(overlay_label_, LV_ALIGN_CENTER, 0, 0);
    }

    lv_label_set_text(overlay_label_, text);
    lv_obj_clear_flag(overlay_bg_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay_bg_);

    // Hide face behind overlay
    face_renderer_.SetVisible(false);

    xSemaphoreGive(ui_mutex_);

    // Auto-dismiss timer (0 = persistent, no auto-dismiss)
    if (timeout_ms > 0) {
        if (overlay_timer_) {
            esp_timer_stop(overlay_timer_);
        } else {
            esp_timer_create_args_t args = {};
            args.callback = OverlayTimerCallback;
            args.arg = this;
            args.name = "overlay";
            esp_timer_create(&args, &overlay_timer_);
        }
        esp_timer_start_once(overlay_timer_, (uint64_t)timeout_ms * 1000);
        ESP_LOGI(TAG, "Overlay shown (auto-dismiss in %lums)", (unsigned long)timeout_ms);
    } else {
        if (overlay_timer_) {
            esp_timer_stop(overlay_timer_);
        }
        ESP_LOGI(TAG, "Overlay shown (persistent, no auto-dismiss)");
    }
}

void DisplayManager::HideOverlay()
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);
    if (overlay_bg_) {
        lv_obj_add_flag(overlay_bg_, LV_OBJ_FLAG_HIDDEN);
    }
    // Restore face if not in listening mode (waveform visible)
    if (!waveform_visible_) {
        face_renderer_.SetVisible(true);
    }
    xSemaphoreGive(ui_mutex_);
    ESP_LOGI(TAG, "Overlay hidden");
}

void DisplayManager::OverlayTimerCallback(void* arg)
{
    auto* self = static_cast<DisplayManager*>(arg);
    self->HideOverlay();
}

// ── Boot Animation (water-ripple effect) ────────────────────────────────────

void DisplayManager::ShowBootAnimation()
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);

    if (boot_anim_active_) {
        xSemaphoreGive(ui_mutex_);
        return;
    }
    boot_anim_active_ = true;
    boot_anim_frame_ = 0;

    // Hide face and normal UI during boot animation
    face_renderer_.SetVisible(false);
    if (status_label_) lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (wifi_icon_) lv_obj_add_flag(wifi_icon_, LV_OBJ_FLAG_HIDDEN);
    if (time_label_) lv_obj_add_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
    if (battery_bar_) lv_obj_add_flag(battery_bar_, LV_OBJ_FLAG_HIDDEN);

    // Full-screen container
    boot_anim_container_ = lv_obj_create(screen_);
    lv_obj_set_size(boot_anim_container_, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(boot_anim_container_, 0, 0);
    lv_obj_set_style_bg_color(boot_anim_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_anim_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_anim_container_, 0, 0);
    lv_obj_clear_flag(boot_anim_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(boot_anim_container_);

    // Center pulsing dot
    boot_center_dot_ = lv_obj_create(boot_anim_container_);
    lv_obj_set_size(boot_center_dot_, 16, 16);
    lv_obj_align(boot_center_dot_, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(boot_center_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(boot_center_dot_, lv_color_make(100, 200, 255), 0);
    lv_obj_set_style_bg_opa(boot_center_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_center_dot_, 0, 0);
    lv_obj_clear_flag(boot_center_dot_, LV_OBJ_FLAG_SCROLLABLE);

    // 3 ripple rings
    for (int i = 0; i < kBootRippleCount; i++) {
        boot_ripples_[i] = lv_obj_create(boot_anim_container_);
        lv_obj_set_size(boot_ripples_[i], 20, 20);
        lv_obj_align(boot_ripples_[i], LV_ALIGN_CENTER, 0, -20);
        lv_obj_set_style_radius(boot_ripples_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(boot_ripples_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(boot_ripples_[i], lv_color_make(100, 200, 255), 0);
        lv_obj_set_style_border_width(boot_ripples_[i], 2, 0);
        lv_obj_set_style_border_opa(boot_ripples_[i], LV_OPA_70, 0);
        lv_obj_clear_flag(boot_ripples_[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    // "Initializing..." text
    boot_text_label_ = lv_label_create(boot_anim_container_);
    lv_obj_set_style_text_color(boot_text_label_, lv_color_make(150, 150, 150), 0);
    lv_obj_align(boot_text_label_, LV_ALIGN_CENTER, 0, 50);
    lv_label_set_text(boot_text_label_, "Initializing...");

    xSemaphoreGive(ui_mutex_);

    // Start animation timer (~33fps)
    if (!boot_anim_timer_) {
        esp_timer_create_args_t args = {};
        args.callback = BootAnimTimerCallback;
        args.arg = this;
        args.name = "boot_anim";
        esp_timer_create(&args, &boot_anim_timer_);
    }
    esp_timer_start_periodic(boot_anim_timer_, 30000);  // 30ms

    ESP_LOGI(TAG, "Boot animation started");
}

void DisplayManager::BootAnimTimerCallback(void* arg)
{
    auto* self = static_cast<DisplayManager*>(arg);
    self->UpdateBootAnimation();
}

void DisplayManager::UpdateBootAnimation()
{
    if (!boot_anim_active_) return;
    if (xSemaphoreTake(ui_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return;

    boot_anim_frame_++;

    // Center dot pulse: gentle size oscillation
    if (boot_center_dot_) {
        float pulse = 1.0f + 0.3f * sinf((float)boot_anim_frame_ * 0.1f);
        int dot_size = (int)(16.0f * pulse);
        lv_obj_set_size(boot_center_dot_, dot_size, dot_size);
        lv_obj_align(boot_center_dot_, LV_ALIGN_CENTER, 0, -20);
    }

    // Ripple animation: each ring expands outward, fading, staggered
    int cycle_frames = 60;  // ~1.8s per full cycle
    for (int i = 0; i < kBootRippleCount; i++) {
        if (!boot_ripples_[i]) continue;

        int offset = i * (cycle_frames / kBootRippleCount);
        int phase = ((int)boot_anim_frame_ + offset) % cycle_frames;
        float t = (float)phase / (float)cycle_frames;  // 0.0 → 1.0

        // Size: 20px → 180px
        int size = 20 + (int)(t * 160.0f);
        lv_obj_set_size(boot_ripples_[i], size, size);
        lv_obj_align(boot_ripples_[i], LV_ALIGN_CENTER, 0, -20);

        // Opacity: 70% → 0% as it expands
        uint8_t opa = (uint8_t)((1.0f - t) * 178);
        lv_obj_set_style_border_opa(boot_ripples_[i], opa, 0);

        // Border thins as it expands
        int bw = 2 - (int)(t * 1.5f);
        if (bw < 1) bw = 1;
        lv_obj_set_style_border_width(boot_ripples_[i], bw, 0);
    }

    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::HideBootAnimation()
{
    if (!boot_anim_active_) return;

    // Stop and delete timer. If callback is currently executing, it holds
    // ui_mutex_, so our xSemaphoreTake below will block until it finishes.
    if (boot_anim_timer_) {
        esp_timer_stop(boot_anim_timer_);
        esp_timer_delete(boot_anim_timer_);
        boot_anim_timer_ = nullptr;
    }

    xSemaphoreTake(ui_mutex_, portMAX_DELAY);

    boot_anim_active_ = false;

    // Clean up LVGL objects (children deleted with container)
    if (boot_anim_container_) {
        lv_obj_del(boot_anim_container_);
        boot_anim_container_ = nullptr;
        boot_center_dot_ = nullptr;
        boot_text_label_ = nullptr;
        for (int i = 0; i < kBootRippleCount; i++) boot_ripples_[i] = nullptr;
    }

    // Restore normal UI elements
    if (status_label_) lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (wifi_icon_) lv_obj_clear_flag(wifi_icon_, LV_OBJ_FLAG_HIDDEN);
    if (time_label_) lv_obj_clear_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
    UpdateBatteryIcon();  // restore battery_bar_ based on current percent
    face_renderer_.SetVisible(true);

    xSemaphoreGive(ui_mutex_);

    ESP_LOGI(TAG, "Boot animation hidden");
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
        // Disconnected: red WiFi with slash
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI " " LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_make(180, 60, 60), 0);
    } else if (wifi_rssi_ > -65) {
        // Strong signal: green
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_make(60, 160, 60), 0);
    } else if (wifi_rssi_ > -75) {
        // Medium signal: yellow
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_make(160, 140, 40), 0);
    } else {
        // Weak signal: dim grey
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

    // Only show time if NTP has synced (year >= 2025)
    if (timeinfo.tm_year + 1900 >= 2025) {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        lv_label_set_text(time_label_, buf);
    } else {
        lv_label_set_text(time_label_, "");
    }
    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::UpdateWaveform()
{
    // Read audio levels from AudioService (lock-free ring buffer)
    uint16_t levels[WAVEFORM_BAR_COUNT];
    int count = AudioService::GetInstance().GetRecentLevels(levels, WAVEFORM_BAR_COUNT);

    // Find max for normalization
    uint16_t max_level = 1;
    for (int i = 0; i < count; i++) {
        if (levels[i] > max_level) max_level = levels[i];
    }
    // Use a minimum floor to avoid noise-only flat bars
    if (max_level < 200) max_level = 200;

    int container_h = 120;
    int min_h = 4;
    int max_h = container_h - 4;

    for (int i = 0; i < WAVEFORM_BAR_COUNT; i++) {
        if (!waveform_bars_[i]) continue;
        int h = min_h;
        if (i < count) {
            h = min_h + (int)((float)levels[i] / max_level * (max_h - min_h));
            if (h > max_h) h = max_h;
        }
        int y = container_h - h;
        lv_obj_set_height(waveform_bars_[i], h);
        lv_obj_set_y(waveform_bars_[i], y);

        // Color: green for low, yellow for medium, red for high
        if (levels[i] > max_level * 3 / 4) {
            lv_obj_set_style_bg_color(waveform_bars_[i], lv_color_make(220, 80, 60), 0);
        } else if (levels[i] > max_level / 3) {
            lv_obj_set_style_bg_color(waveform_bars_[i], lv_color_make(60, 180, 120), 0);
        } else {
            lv_obj_set_style_bg_color(waveform_bars_[i], lv_color_make(60, 120, 100), 0);
        }
    }
}
