#pragma once

#include "driver/gpio.h"
#include "esp_lcd_panel_vendor.h"

#include <array>

namespace display
{
    constexpr gpio_num_t GPIO_LCD_BL = GPIO_NUM_12;
    constexpr gpio_num_t GPIO_LCD_CS = GPIO_NUM_9;
    constexpr gpio_num_t GPIO_LCD_RST = GPIO_NUM_11;
    constexpr gpio_num_t GPIO_LCD_DC = GPIO_NUM_13;

    constexpr uint16_t SCREEN_WIDTH = 240;
    constexpr uint16_t SCREEN_HEIGHT = 320;

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

    class LCDDisplay {
    public:
        LCDDisplay() = default;
        virtual ~LCDDisplay();
        esp_err_t initialize();

        // Asynchronous frame rendering: converts 8-bit to RGB565 + DMA transfer (non-blocking)
        void renderFrameAsync(const uint8_t* frameBuffer);

        // Wait for DMA transfer to complete
        void waitForTransfer();

        // Legacy synchronous methods (for menu)
        void renderFrame(const uint8_t* frameBuffer);
        void renderFrameRGB565(const uint16_t* buffer, int width, int height, int offset_x = 0, int offset_y = 0);

    private:
        esp_lcd_panel_handle_t panel{nullptr};
        esp_lcd_panel_io_handle_t io_handle{nullptr};

        // DMA transfer tracking
        volatile bool transfer_done{true};

        // Chunked transfer: small RGB565 buffer in internal SRAM (fast)
        static constexpr int CHUNK_LINES = 16;  // Transfer 16 lines at a time
        uint16_t* rgb565_chunk{nullptr};  // 160×16×2 = 5.1 KB only!

        // Callback for DMA completion
        static bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx);
    };
}
