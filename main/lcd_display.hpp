#pragma once

#include "display.hpp"

#include "esp_lcd_panel_vendor.h"
#include <array>

namespace display
{
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
