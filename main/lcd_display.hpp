#pragma once

#include "gpio_pins.hpp"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include <array>

namespace display
{

    constexpr uint16_t SCREEN_WIDTH = 240;
    constexpr uint16_t SCREEN_HEIGHT = 320;

    constexpr int LCD_WIDTH = 160;
    constexpr int LCD_HEIGHT = 144;
    constexpr int SPI_CLK_FREQ_MHZ = 80; // clock in mhz

    static constexpr int CHUNK_LINES = 8;
    static constexpr int CHUNK_PIXELS = SCREEN_WIDTH * CHUNK_LINES;  // Use SCREEN_WIDTH (240) instead of LCD_WIDTH (160)

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
        ~LCDDisplay() = default;
        esp_err_t initialize();

        // Wait for DMA transfer to complete
        void waitForTransfer();
        void renderFrameRGB565(const uint16_t* buffer, int width, int height, int offset_x = 0, int offset_y = 0);
                
    private:
        uint32_t frame_times[10]{0};  // Historique pour adaptation
        int frame_time_index{0};

    private:
        esp_lcd_panel_handle_t panel{nullptr};
        esp_lcd_panel_io_handle_t io_handle{nullptr};

        // semaphore for DMA transfer tracking
        SemaphoreHandle_t dma_done_sem;

        // Chunked transfer: small RGB565 buffer in internal SRAM (fast)
        static constexpr int CHUNK_LINES = 8;  // Transfer 8 lines at a time (optimized for pipeline)
        static uint16_t rgb565_chunk[2][CHUNK_PIXELS];  // 160×8×2 = 2.56 KB ultra-fast!

        // Callback for DMA completion
        static bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx);
    };
}
