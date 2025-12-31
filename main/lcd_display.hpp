#pragma once

#include "display.hpp"
#include "driver/gpio.h"

#include "esp_lcd_panel_vendor.h"
#include <array>

namespace display
{
    constexpr gpio_num_t GPIO_LCD_BL = GPIO_NUM_12;
    constexpr gpio_num_t GPIO_LCD_CS = GPIO_NUM_9;
    constexpr gpio_num_t GPIO_LCD_RST = GPIO_NUM_11;
    constexpr gpio_num_t GPIO_LCD_DC = GPIO_NUM_13;

    constexpr uint8_t SCREEN_WIDTH = 240;
    constexpr uint8_t SCREEN_HEIGHT = 320;

    class LCDDisplay : public Display {
    public:
        LCDDisplay() = default;
        virtual ~LCDDisplay() = default;
        void initialize() final;
        void renderFrame(const std::array<uint8_t, LCD_WIDTH * LCD_HEIGHT>& frameBuffer) final;
    private:
        esp_lcd_panel_handle_t panel{nullptr};
        std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> buf;
    };
}
