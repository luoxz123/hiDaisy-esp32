#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include <driver/gpio.h>
#include <driver/spi_master.h>

// LCD (ST7789 via SPI3_HOST)
#define PIN_LCD_MOSI    GPIO_NUM_41
#define PIN_LCD_SCLK    GPIO_NUM_42
#define PIN_LCD_CS      GPIO_NUM_21
#define PIN_LCD_DC      GPIO_NUM_40
#define PIN_LCD_RST     GPIO_NUM_45
#define PIN_LCD_BLK     GPIO_NUM_20

// I2S Speaker (MAX98357A)
#define PIN_SPK_DOUT    GPIO_NUM_7
#define PIN_SPK_BCLK    GPIO_NUM_15
#define PIN_SPK_LRCK    GPIO_NUM_16

// I2S Microphone (INMP441)
#define PIN_MIC_SD      GPIO_NUM_6
#define PIN_MIC_WS      GPIO_NUM_4
#define PIN_MIC_SCK     GPIO_NUM_5

// Buttons
#define PIN_BUTTON_BOOT GPIO_NUM_0
#define PIN_BUTTON_VOL_UP   GPIO_NUM_10
#define PIN_BUTTON_VOL_DOWN GPIO_NUM_39

// Battery ADC
#define PIN_BATTERY_ADC GPIO_NUM_1

// Audio parameters (matching xiaozhi zhengchen-1.54tft-wifi config)
#define AUDIO_INPUT_SAMPLE_RATE   16000
#define AUDIO_OUTPUT_SAMPLE_RATE  16000  // Must match server Opus encode rate (16kHz)
#define AUDIO_SAMPLE_RATE         AUDIO_INPUT_SAMPLE_RATE  // backward compat
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      1

// LCD parameters
#define LCD_WIDTH           240
#define LCD_HEIGHT          240
#define LCD_SPI_HOST        SPI3_HOST
#define LCD_SPI_FREQ_HZ     (80 * 1000 * 1000)

#endif // PIN_CONFIG_H
