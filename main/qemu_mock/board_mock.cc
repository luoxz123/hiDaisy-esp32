/*
 * QEMU mock board implementation.
 * Stubs all hardware peripherals that QEMU doesn't emulate:
 * I2S, SPI LCD, LEDC PWM, GPIO buttons.
 */
#include "board.h"
#include "pin_config.h"
#include <driver/i2s_std.h>
#include <esp_log.h>

static const char *TAG = "board_qemu";

// Dummy I2S handles (non-NULL so null checks pass, but never used for real I/O)
static int qemu_spk_dummy = 1;
static int qemu_mic_dummy = 2;
static i2s_chan_handle_t i2s_spk_handle = (i2s_chan_handle_t)&qemu_spk_dummy;
static i2s_chan_handle_t i2s_mic_handle = (i2s_chan_handle_t)&qemu_mic_dummy;

i2s_chan_handle_t board_get_spk_handle(void) { return i2s_spk_handle; }
i2s_chan_handle_t board_get_mic_handle(void) { return i2s_mic_handle; }

esp_err_t board_init_spi_lcd(void)
{
    ESP_LOGI(TAG, "[QEMU] SPI LCD init skipped (no SPI emulation)");
    return ESP_OK;
}

esp_err_t board_init_i2s_speaker(void)
{
    ESP_LOGI(TAG, "[QEMU] I2S speaker init skipped (mock handle)");
    return ESP_OK;
}

esp_err_t board_init_i2s_microphone(void)
{
    ESP_LOGI(TAG, "[QEMU] I2S microphone init skipped (mock handle)");
    return ESP_OK;
}

esp_err_t board_init_button(void)
{
    ESP_LOGI(TAG, "[QEMU] GPIO button init skipped (use UART console)");
    return ESP_OK;
}

void board_set_brightness(int percent)
{
    ESP_LOGI(TAG, "[QEMU] Brightness set to %d%% (no LEDC)", percent);
}

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "[QEMU] Board init - all peripherals mocked");

    board_init_spi_lcd();
    board_init_i2s_speaker();
    board_init_i2s_microphone();
    board_init_button();

    ESP_LOGI(TAG, "[QEMU] Board init complete (mock mode)");
    return ESP_OK;
}
