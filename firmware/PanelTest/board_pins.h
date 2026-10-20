// board_pins.h — ESP32-S3-PhotoPainter hardware map
//
// These are Waveshare's own assignments, taken from their factory test
// (04_PowerConsumptionTest/.../bsp_config.h) and the xiaozhi board config
// in waveshareteam/ESP32-S3-PhotoPainter. Don't guess at these.

#pragma once

// 7.3" Spectra-6 panel (SPI)
#define PIN_EPD_DC     8
#define PIN_EPD_CS     9
#define PIN_EPD_SCK    10
#define PIN_EPD_MOSI   11
#define PIN_EPD_RST    12
#define PIN_EPD_BUSY   13

// Buttons
#define PIN_BTN_BOOT   0    // BOOT — also our "refresh now" button
#define PIN_BTN_KEY    4
#define PIN_BTN_PWR    5

// Status LEDs
#define PIN_LED_RED    45
#define PIN_LED_GREEN  42

// I2C bus — AXP2101 PMU lives here at 0x34, along with the RTC and audio codecs
#define PIN_I2C_SDA    47
#define PIN_I2C_SCL    48

// AXP2101 interrupt line. Waveshare's firmware drives this as an OUTPUT and
// holds it HIGH during normal operation; a low pulse is the PMU wake signal.
// We must keep it high through deep sleep or the PMU can cut our rails.
#define PIN_PMU_IRQ    21

// Panel geometry
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
