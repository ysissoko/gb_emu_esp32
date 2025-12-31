#include "lcd_display.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

// Disable most logging in LCD for performance (keep errors only during init)
#ifndef LCD_INIT_PHASE
#undef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) do {} while(0)
#endif

namespace display
{
    // Helper to convert RGB565 to BGR565 for ST7789V Seengreat
    static constexpr uint16_t rgb_to_bgr565(uint16_t rgb) {
        // RGB565: RRRR RGGG GGGB BBBB
        // BGR565: BBBB BGGG GGGR RRRR
        uint16_t r = (rgb >> 11) & 0x1F;  // Extract 5 red bits
        uint16_t g = (rgb >> 5) & 0x3F;   // Extract 6 green bits
        uint16_t b = rgb & 0x1F;          // Extract 5 blue bits
        return (b << 11) | (g << 5) | r;  // Reassemble as BGR
    }

    // Optimized: Use constexpr lookup table instead of switch (converted to BGR for ST7789V)
    static constexpr uint16_t COLOR_PALETTE[4] = {
        rgb_to_bgr565(0xFFFF), // WHITE
        rgb_to_bgr565(0xC618), // LIGHT_GRAY
        rgb_to_bgr565(0x632C), // DARK_GRAY
        rgb_to_bgr565(0x0000)  // BLACK
    };

    LCDDisplay::~LCDDisplay()
    {
        if (rgb565_chunk != nullptr) {
            free(rgb565_chunk);
            rgb565_chunk = nullptr;
        }
    }

    bool LCDDisplay::lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *edata,
                                       void *user_ctx)
    {
        LCDDisplay* lcd = static_cast<LCDDisplay*>(user_ctx);
        lcd->transfer_done = true;
        return false; // No high priority task woken
    }

    esp_err_t LCDDisplay::initialize()
    {
        // Allocate small chunk buffer in internal SRAM (5.1 KB - fits easily!)
        constexpr size_t chunk_size = LCD_WIDTH * CHUNK_LINES * sizeof(uint16_t);

        rgb565_chunk = (uint16_t*)heap_caps_malloc(chunk_size,
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (rgb565_chunk == nullptr) {
            ESP_LOGE("LCD", "Failed to allocate RGB565 chunk buffer in internal SRAM!");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI("LCD", "LCD RGB565 chunk buffer allocated in internal SRAM (%zu bytes) - ultra fast!", chunk_size);

        gpio_set_direction(GPIO_LCD_CS, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_LCD_CS, 1);
        // ESP32-S3 compatible GPIO mapping
        // Configure backlight (BLK)
        gpio_config_t bk_gpio_config = {};
        bk_gpio_config.mode = GPIO_MODE_OUTPUT;
        bk_gpio_config.pin_bit_mask = 1ULL << 48;
        gpio_config(&bk_gpio_config);
        gpio_set_level(GPIO_LCD_BL, 1); // Turn on backlight

        // Note: SPI bus is initialized in main.cpp and shared with SD card

        ESP_LOGI("LCD", "Configuring LCD panel IO...");

        // Configure LCD panel IO with DMA support
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = GPIO_LCD_CS;
        io_cfg.dc_gpio_num = GPIO_LCD_DC;
        io_cfg.spi_mode = 0;
        io_cfg.pclk_hz = 80 * 1000 * 1000; // 80 MHz - ST7789V maximum
        io_cfg.trans_queue_depth = 2;  // Only need 2: current transfer + next frame queued
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 8;
        io_cfg.flags.dc_low_on_data = 0; // DC high for data

        // Register DMA completion callback
        io_cfg.on_color_trans_done = lcd_trans_done_cb;
        io_cfg.user_ctx = this;

        esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_cfg, &io_handle);
        if (ret != ESP_OK)
            return ret;

        // Configure LCD panel
        ESP_LOGI("LCD", "Configuring LCD panel...");
        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = GPIO_LCD_RST; // RES
        panel_cfg.bits_per_pixel = 16;

        ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel);
        if (ret != ESP_OK)
            return ret;
        ESP_LOGI("LCD", "esp_lcd_new_panel_st7789 returned: %d", ret);

        ESP_LOGI("LCD", "Resetting panel...");
        esp_lcd_panel_reset(panel);

        ESP_LOGI("LCD", "Initializing panel...");
        esp_lcd_panel_init(panel);

        ESP_LOGI("LCD", "Turning display on (before config)...");
        esp_lcd_panel_disp_on_off(panel, true);

        ESP_LOGI("LCD", "Setting gap offsets...");
        esp_lcd_panel_set_gap(panel, 0, 0); // No offset for standard 240x240

        ESP_LOGI("LCD", "Setting display orientation...");
        // Try different orientation - no swap, just mirror
        esp_lcd_panel_swap_xy(panel, false);  // No rotation
        esp_lcd_panel_mirror(panel, true, false);  // No mirror

        ESP_LOGI("LCD", "Testing color inversion - trying WITHOUT inversion first...");
        esp_lcd_panel_invert_color(panel, false); // Try without inversion first

        // For ST7789V, we might need to configure RGB/BGR order
        // Some modules use BGR instead of RGB
        // This command sets the color order (command 0x36 - MADCTL)
        // Bit 3 controls RGB/BGR: 0 = RGB, 1 = BGR
        ESP_LOGI("LCD", "Configuring color order for ST7789V...");
        // ST7789V Seengreat uses BGR order, configure MADCTL register
        // We need to set bit 3 (BGR bit) in MADCTL
        uint8_t madctl_value = 0x08;  // Bit 3 = BGR color order
        esp_lcd_panel_io_tx_param(io_handle, 0x36, &madctl_value, 1);  // MADCTL command
        ESP_LOGI("LCD", "Color order set to BGR");

        ESP_LOGI("LCD", "Clearing screen to eliminate snow...");
        // Create a black buffer and fill entire screen
        static uint16_t clear_buffer[SCREEN_WIDTH * 80]; // Buffer for 80 lines at a time
        for (int i = 0; i < SCREEN_WIDTH * 80; i++) {
            clear_buffer[i] = 0x0000; // BLACK
        }

        // Fill screen in chunks to avoid memory issues
        for (int y = 0; y < SCREEN_HEIGHT; y += 80) {
            int chunk_height = (y + 80 > SCREEN_HEIGHT) ? (SCREEN_HEIGHT - y) : 80;
            esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_WIDTH, y + chunk_height, clear_buffer);
        }
        ESP_LOGI("LCD", "Screen cleared!");

        // Delay to ensure screen is ready
        vTaskDelay(pdMS_TO_TICKS(100));

        return ESP_OK;
    }

    void LCDDisplay::renderFrame(const uint8_t* frameBuffer)
    {
        // Legacy synchronous rendering (for menu) - uses chunked transfer
        constexpr int OFFSET_X = (SCREEN_WIDTH - LCD_WIDTH) / 2;
        constexpr int OFFSET_Y = (SCREEN_HEIGHT - LCD_HEIGHT) / 2;

        // Transfer by chunks of CHUNK_LINES
        for (int y = 0; y < LCD_HEIGHT; y += CHUNK_LINES)
        {
            int lines = (y + CHUNK_LINES > LCD_HEIGHT) ? (LCD_HEIGHT - y) : CHUNK_LINES;
            const uint8_t* src = frameBuffer + (y * LCD_WIDTH);
            uint16_t* dst = rgb565_chunk;

            // Convert chunk to RGB565
            size_t chunk_pixels = LCD_WIDTH * lines;
            for (size_t i = 0; i < chunk_pixels; i++)
            {
                dst[i] = COLOR_PALETTE[src[i] & 0x3];
            }

            // Wait for previous chunk transfer
            waitForTransfer();

            // Transfer chunk
            transfer_done = false;
            esp_lcd_panel_draw_bitmap(panel, OFFSET_X, OFFSET_Y + y,
                                     OFFSET_X + LCD_WIDTH, OFFSET_Y + y + lines,
                                     rgb565_chunk);
        }

        // Wait for last chunk
        waitForTransfer();
    }

    void LCDDisplay::renderFrameAsync(const uint8_t* frameBuffer)
    {
        // Asynchronous chunked rendering - fast conversion in internal SRAM
        constexpr int OFFSET_X = (SCREEN_WIDTH - LCD_WIDTH) / 2;
        constexpr int OFFSET_Y = (SCREEN_HEIGHT - LCD_HEIGHT) / 2;

        // Transfer by chunks of CHUNK_LINES
        for (int y = 0; y < LCD_HEIGHT; y += CHUNK_LINES)
        {
            int lines = (y + CHUNK_LINES > LCD_HEIGHT) ? (LCD_HEIGHT - y) : CHUNK_LINES;
            const uint8_t* src = frameBuffer + (y * LCD_WIDTH);
            uint16_t* dst = rgb565_chunk;

            // Convert chunk to RGB565 in internal SRAM (fast!)
            size_t chunk_pixels = LCD_WIDTH * lines;
            for (size_t i = 0; i < chunk_pixels; i++)
            {
                dst[i] = COLOR_PALETTE[src[i] & 0x3];
            }

            // Wait for previous chunk transfer before starting new one
            waitForTransfer();

            // Start DMA transfer of this chunk
            transfer_done = false;
            esp_lcd_panel_draw_bitmap(panel, OFFSET_X, OFFSET_Y + y,
                                     OFFSET_X + LCD_WIDTH, OFFSET_Y + y + lines,
                                     rgb565_chunk);
        }
        // Note: Last chunk transfers asynchronously, no wait at end
    }

    // Legacy synchronous method for menu RGB565 rendering
    void LCDDisplay::renderFrameRGB565(const uint16_t *buffer, int width, int height, int offset_x, int offset_y)
    {
        static const char* TAG_LCD = "LCD_render";

        if (!buffer || width <= 0 || height <= 0)
        {
            ESP_LOGE(TAG_LCD, "Invalid buffer or dimensions!");
            return;
        }

        // Limiter aux dimensions de l'écran
        int draw_width = (offset_x + width > SCREEN_WIDTH) ? (SCREEN_WIDTH - offset_x) : width;
        int draw_height = (offset_y + height > SCREEN_HEIGHT) ? (SCREEN_HEIGHT - offset_y) : height;

        if (draw_width <= 0 || draw_height <= 0)
        {
            ESP_LOGE(TAG_LCD, "Invalid calculated dimensions!");
            return;
        }

        // Wait for any previous transfer
        waitForTransfer();

        ESP_LOGI(TAG_LCD, "Drawing %dx%d at (%d,%d), first pixel=0x%04X",
                 draw_width, draw_height, offset_x, offset_y, buffer[0]);

        transfer_done = false;
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, offset_x, offset_y,
                                  offset_x + draw_width, offset_y + draw_height,
                                  buffer);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG_LCD, "draw_bitmap failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG_LCD, "draw_bitmap succeeded");
        }
        transfer_done = true;
    }

    void LCDDisplay::waitForTransfer()
    {
        // Busy-wait for DMA transfer to complete
        // This is efficient because transfers are fast (~2-3ms with DMA)
        while (!transfer_done) {
            // Could add a small delay here if needed
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}
