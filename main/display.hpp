#pragma once

#include <array>
#include <cstdint>

namespace display
{
    constexpr int LCD_WIDTH = 160;
    constexpr int LCD_HEIGHT = 144;


    // Pixel color (Game Boy has 4 shades of gray)
    enum class Color : uint8_t
    {
        WHITE = 0,
        LIGHT_GRAY = 1,
        DARK_GRAY = 2,
        BLACK = 3
    };

    class Display
    {
    public:
        virtual void initialize() = 0;
        virtual void renderFrame(const std::array<uint8_t, LCD_WIDTH * LCD_HEIGHT>& frameBuffer) = 0;
        virtual ~Display() {};
    };
}
