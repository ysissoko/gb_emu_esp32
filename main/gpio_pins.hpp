#pragma once

#include "driver/gpio.h"

namespace gpio {

/* =========================
 * 🎮 JOYPAD BUTTONS
 * =========================
 * Active LOW (internal pull-ups enabled)
 */
constexpr gpio_num_t BTN_RIGHT  = GPIO_NUM_17;
constexpr gpio_num_t BTN_LEFT   = GPIO_NUM_18;
constexpr gpio_num_t BTN_UP     = GPIO_NUM_15;
constexpr gpio_num_t BTN_DOWN   = GPIO_NUM_16;

constexpr gpio_num_t BTN_A      = GPIO_NUM_5;
constexpr gpio_num_t BTN_B      = GPIO_NUM_6;
constexpr gpio_num_t BTN_SELECT = GPIO_NUM_47;
constexpr gpio_num_t BTN_START  = GPIO_NUM_21;


/* =========================
 * 🖥 LCD – ST7789 (SPI)
 * =========================
 */
constexpr gpio_num_t LCD_BL  = GPIO_NUM_7;   // Backlight (PWM capable)
constexpr gpio_num_t LCD_CS  = GPIO_NUM_10;
constexpr gpio_num_t LCD_RST = GPIO_NUM_8;
constexpr gpio_num_t LCD_DC  = GPIO_NUM_9;


/* =========================
 * 🧠 SHARED SPI BUS
 * =========================
 * SPI2_HOST
 */
constexpr gpio_num_t SPI_MOSI = GPIO_NUM_11;
constexpr gpio_num_t SPI_MISO = GPIO_NUM_13;
constexpr gpio_num_t SPI_SCK  = GPIO_NUM_12;


/* =========================
 * 💾 SD CARD (SPI)
 * =========================
 */
constexpr gpio_num_t SD_CS = GPIO_NUM_4;


/* =========================
 * 🔋 BATTERY / POWER
 * =========================
 */
constexpr gpio_num_t ADC_BATTERY = GPIO_NUM_38; // ADC1 channel (safe)


/* =========================
 * 💡 STATUS LED (optional)
 * =========================
 */
constexpr gpio_num_t LED_STATUS_1 = GPIO_NUM_41;
constexpr gpio_num_t LED_STATUS_2 = GPIO_NUM_42;


/* =========================
 * 🔌 USB (DO NOT TOUCH)
 * =========================
 */
constexpr gpio_num_t USB_DM = GPIO_NUM_19; // Reserved
constexpr gpio_num_t USB_DP = GPIO_NUM_20; // Reserved


/* =========================
 * 🧰 SYSTEM
 * =========================
 */
constexpr gpio_num_t GPIO_BOOT = GPIO_NUM_0; // Boot button only

} // namespace gpio
