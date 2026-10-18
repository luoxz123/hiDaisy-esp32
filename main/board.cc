#include "board.h"
#include "pin_config.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <driver/i2s_std.h>

static const char *TAG = "board";

static i2s_chan_handle_t i2s_spk_handle = NULL;
static i2s_chan_handle_t i2s_mic_handle = NULL;

i2s_chan_handle_t board_get_spk_handle(void) { return i2s_spk_handle; }
i2s_chan_handle_t board_get_mic_handle(void) { return i2s_mic_handle; }

esp_err_t board_init_spi_lcd(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure backlight as PWM (LEDC, 10-bit @ 25kHz)
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_ch = {
        .gpio_num = PIN_LCD_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1023, // Full brightness initially
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_ch);
    ledc_fade_func_install(0);

    ESP_LOGI(TAG, "SPI LCD bus initialized (ST7789 240x240)");
    return ESP_OK;
}

esp_err_t board_init_i2s_speaker(void)
{
    // Match xiaozhi zhengchen-1.54tft-wifi: simplex TX on I2S_NUM_0
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t ret = i2s_new_channel(&chan_cfg, &i2s_spk_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S speaker channel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 32-bit MONO at 24kHz — matching xiaozhi's proven config for MAX98357A
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_OUTPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_SPK_BCLK,
            .ws = PIN_SPK_LRCK,
            .dout = PIN_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(i2s_spk_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S speaker init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S speaker initialized (MAX98357A, %dHz, 32-bit mono)", AUDIO_OUTPUT_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t board_init_i2s_microphone(void)
{
    // Match xiaozhi: simplex RX on I2S_NUM_1
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &i2s_mic_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S mic channel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // INMP441: 32-bit mono at 16kHz, matching xiaozhi's slot config
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_INPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(i2s_mic_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S mic init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S microphone initialized (INMP441, %dHz, 32-bit mono)", AUDIO_INPUT_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t board_init_button(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&btn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Button initialized (GPIO%d, pull-up, falling edge)", PIN_BUTTON_BOOT);
    return ESP_OK;
}

void board_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)(percent * 1023 / 100);
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty, 200);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_NO_WAIT);
}

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "Initializing board peripherals...");

    esp_err_t ret;

    ret = board_init_spi_lcd();
    if (ret != ESP_OK) return ret;

    ret = board_init_i2s_speaker();
    if (ret != ESP_OK) return ret;

    ret = board_init_i2s_microphone();
    if (ret != ESP_OK) return ret;

    ret = board_init_button();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "All board peripherals initialized successfully");
    return ESP_OK;
}
