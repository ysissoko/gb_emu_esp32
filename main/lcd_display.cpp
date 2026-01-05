#include "lcd_display.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_heap_caps.h"

namespace display
{
    DMA_ATTR uint16_t LCDDisplay::rgb565_chunk[2][CHUNK_PIXELS];
    
    // Helper to convert RGB565 to BGR565 for ST7789V Seengreat
    static constexpr uint16_t rgb_to_bgr565(uint16_t rgb)
    {
        // RGB565: RRRR RGGG GGGB BBBB
        // BGR565: BBBB BGGG GGGR RRRR
        uint16_t r = (rgb >> 11) & 0x1F; // Extract 5 red bits
        uint16_t g = (rgb >> 5) & 0x3F;  // Extract 6 green bits
        uint16_t b = rgb & 0x1F;         // Extract 5 blue bits
        return (b << 11) | (g << 5) | r; // Reassemble as BGR
    }

    // Optimized: Use constexpr lookup table instead of switch (converted to BGR for ST7789V)
    static constexpr uint16_t COLOR_PALETTE[4] = {
        rgb_to_bgr565(0xFFFF), // WHITE
        rgb_to_bgr565(0xC618), // LIGHT_GRAY
        rgb_to_bgr565(0x632C), // DARK_GRAY
        rgb_to_bgr565(0x0000)  // BLACK
    };

    bool LCDDisplay::lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *edata,
                                       void *user_ctx)
    {
        auto lcd = static_cast<LCDDisplay *>(user_ctx);

        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(lcd->dma_done_sem, &hpw);

        return hpw == pdTRUE;
    }

    esp_err_t LCDDisplay::initialize()
    {
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
        io_cfg.pclk_hz = SPI_CLK_FREQ_MHZ * 1000 * 1000;
        io_cfg.trans_queue_depth = 4;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 8;
        io_cfg.flags.dc_low_on_data = 0;

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

        // Create DMA semaphore for buffer transfer sync
        dma_done_sem = xSemaphoreCreateBinary();
        if (!dma_done_sem)
        {
            ESP_LOGE("LCD", "Failed to create DMA semaphore");
            return ESP_ERR_NO_MEM;
        }

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
        esp_lcd_panel_swap_xy(panel, false);      // No rotation
        esp_lcd_panel_mirror(panel, true, false); // No mirror

        ESP_LOGI("LCD", "Activating color inversion (matching example code)...");
        // Example code calls 0x21 (INVON) twice (lines 87 and 164)
        // This inverts display colors - RGB becomes BGR effectively
        esp_lcd_panel_invert_color(panel, true); // Enable inversion like example

        // For ST7789V, we might need to configure RGB/BGR order
        // Some modules use BGR instead of RGB
        // This command sets the color order (command 0x36 - MADCTL)
        // Bit 3 controls RGB/BGR: 0 = RGB, 1 = BGR
        ESP_LOGI("LCD", "Configuring MADCTL for ST7789V (matching example code)...");
        // ST7789V Seengreat configuration from example code (lcd_2inch.cpp line 82)
        // 0xA0 = 10100000 binary:
        //   Bit 7 (MY=1): Row address order
        //   Bit 5 (MV=1): Row/Column exchange
        //   Bit 3 (BGR=0): RGB color order (with inversion below, becomes BGR effectively)
        uint8_t madctl_value = 0xA0;                                  // Match example exactly
        esp_lcd_panel_io_tx_param(io_handle, 0x36, &madctl_value, 1); // MADCTL command
        ESP_LOGI("LCD", "MADCTL configured to 0xA0 (RGB + rotation)");

        ESP_LOGI("LCD", "Clearing screen to eliminate snow...");
        // Create a black buffer and fill entire screen
        static uint16_t clear_buffer[SCREEN_WIDTH * 80]; // Buffer for 80 lines at a time
        for (int i = 0; i < SCREEN_WIDTH * 80; i++)
        {
            clear_buffer[i] = 0x0000; // BLACK
        }

        // Fill screen in chunks to avoid memory issues
        for (int y = 0; y < SCREEN_HEIGHT; y += 80)
        {
            int chunk_height = (y + 80 > SCREEN_HEIGHT) ? (SCREEN_HEIGHT - y) : 80;
            esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_WIDTH, y + chunk_height, clear_buffer);
        }
        ESP_LOGI("LCD", "Screen cleared!");

        // Delay to ensure screen is ready
        vTaskDelay(pdMS_TO_TICKS(100));

        return ESP_OK;
    }

    void LCDDisplay::renderFrameRGB565(const uint16_t *buffer,
                                  int width,
                                  int height,
                                  int offset_x,
                                  int offset_y)
    {
        static const char *TAG_LCD = "LCD_render";

        if (!buffer || width <= 0 || height <= 0)
        {
            ESP_LOGE(TAG_LCD, "Invalid buffer or dimensions");
            return;
        }

        int draw_width = (offset_x + width > SCREEN_WIDTH)
                             ? (SCREEN_WIDTH - offset_x)
                             : width;
        int draw_height = (offset_y + height > SCREEN_HEIGHT)
                              ? (SCREEN_HEIGHT - offset_y)
                              : height;

        if (draw_width <= 0 || draw_height <= 0)
        {
            ESP_LOGE(TAG_LCD, "Invalid clipped dimensions");
            return;
        }

        constexpr size_t MAX_DMA_BYTES = 4092;
        size_t total_bytes = draw_width * draw_height * sizeof(uint16_t);

        if (total_bytes <= MAX_DMA_BYTES)
        {
            waitForTransfer(); // s'assurer qu'aucun DMA précédent n'est actif

            esp_err_t ret = esp_lcd_panel_draw_bitmap(
                panel,
                offset_x,
                offset_y,
                offset_x + draw_width,
                offset_y + draw_height,
                buffer);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG_LCD, "draw_bitmap failed: %s", esp_err_to_name(ret));
            }

            return;
        }

        int buf = 0;
        bool first = true;
        int source_y = 0; // Position réelle dans le buffer source
        
        for (int y = 0; y < draw_height; y += CHUNK_LINES)
        {

            int lines = std::min(CHUNK_LINES, draw_height - y);
            
            // CORRECTION CRITIQUE: Utiliser source_y pour le calcul du buffer
            const uint16_t *src = buffer + source_y * width;
            size_t chunk_bytes = draw_width * lines * sizeof(uint16_t);
            
            // Préparer le buffer courant PENDANT que le DMA précédent tourne
            memcpy(rgb565_chunk[buf], src, chunk_bytes);
            
            // Si ce N'EST PAS le premier chunk, attendre que l'autre buffer soit libéré
            if (!first)
            {
                waitForTransfer();
            }
            first = false;
            
            // AVANCER source_y pour le prochain chunk
            source_y += lines;

            esp_err_t ret = esp_lcd_panel_draw_bitmap(
                panel,
                offset_x,
                offset_y + y,
                offset_x + draw_width,
                offset_y + y + lines,
                rgb565_chunk[buf]);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG_LCD, "chunk draw failed at y=%d: %s",
                         y, esp_err_to_name(ret));
                return;
            }

            buf ^= 1;
        }

        // Wait for latest transfer ends
        waitForTransfer();
    }

    void LCDDisplay::waitForTransfer()
    {
        xSemaphoreTake(dma_done_sem, portMAX_DELAY);
    }
}
