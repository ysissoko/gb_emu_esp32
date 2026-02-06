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

    constexpr uint16_t SCREEN_WIDTH = 240; // LCD screen width in pixels
    constexpr uint16_t SCREEN_HEIGHT = 240; // LCD screen height in pixels

    constexpr int LCD_WIDTH = 160; // The original gameboy screen width
    constexpr int LCD_HEIGHT = 144; // The original gameboy screen height
    constexpr int SPI_CLK_FREQ_MHZ = 80; // SPI clock set in mhz

    static constexpr int CHUNK_LINES = 8;
    static constexpr int CHUNK_PIXELS = SCREEN_WIDTH * CHUNK_LINES;

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

        // Wait for DMA transfer to complete. Will block if a transfer is ongoing using a semaphore.
        void waitForTransfer();
        /**
         * @brief Render a frame to the LCD in RGB565 format.
         * 
         * @param buffer the buffer containing the RGB565 pixel data
         * @param width the width of the frame
         * @param height the height of the frame
         * @param offset_x the x offset on the LCD to start rendering
         * @param offset_y the y offset on the LCD to start rendering
         */
        void renderFrameRGB565(const uint16_t* buffer, int width, int height, int offset_x = 0, int offset_y = 0);
                
    private:
        uint32_t frame_times[10]{0};  // Historique pour adaptation
        int frame_time_index{0};

    private:
        // LCD panel and IO handles
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
