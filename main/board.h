#ifndef BOARD_H
#define BOARD_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize all board peripherals (SPI, I2S, ADC, GPIO)
esp_err_t board_init(void);

// Individual peripheral init (called by board_init)
esp_err_t board_init_spi_lcd(void);
esp_err_t board_init_i2s_speaker(void);
esp_err_t board_init_i2s_microphone(void);
esp_err_t board_init_button(void);

// Set backlight brightness (0-100), with smooth fade
void board_set_brightness(int percent);

#ifdef __cplusplus
}
#endif

#endif // BOARD_H
