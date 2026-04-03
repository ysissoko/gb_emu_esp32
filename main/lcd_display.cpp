#include "lcd_display.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>

#include "esp_heap_caps.h"
#include "driver/ledc.h"

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
        gpio_set_direction(gpio::LCD_CS, GPIO_MODE_OUTPUT);
        gpio_set_level(gpio::LCD_CS, 1);

        // ESP32-S3 compatible GPIO mapping
        // Configure backlight via LEDC PWM (~70% duty cycle)
        ledc_timer_config_t ledc_timer = {};
        ledc_timer.speed_mode      = LEDC_LOW_SPEED_MODE;
        ledc_timer.duty_resolution = LEDC_TIMER_8_BIT;
        ledc_timer.timer_num       = LEDC_TIMER_0;
        ledc_timer.freq_hz         = 5000;
        ledc_timer.clk_cfg         = LEDC_AUTO_CLK;
        ledc_timer_config(&ledc_timer);

        ledc_channel_config_t ledc_channel = {};
        ledc_channel.gpio_num   = gpio::LCD_BL;
        ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
        ledc_channel.channel    = LEDC_CHANNEL_0;
        ledc_channel.intr_type  = LEDC_INTR_DISABLE;
        ledc_channel.timer_sel  = LEDC_TIMER_0;
        ledc_channel.duty       = 178; // ~70% of 255
        ledc_channel.hpoint     = 0;
        ledc_channel_config(&ledc_channel);

        // Note: SPI bus is initialized in main.cpp and shared with SD card

        ESP_LOGI("LCD", "Configuring LCD panel IO...");

        // Configure LCD panel IO with DMA support
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = gpio::LCD_CS;
        io_cfg.dc_gpio_num = gpio::LCD_DC;
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
        panel_cfg.reset_gpio_num = gpio::LCD_RST; // RES
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
        // Portrait mode - no swap, no mirror
        esp_lcd_panel_swap_xy(panel, false);      // No XY swap (portrait mode)
        esp_lcd_panel_mirror(panel, false, false); // No mirroring

        esp_lcd_panel_invert_color(panel, true);

        // For ST7789V, we might need to configure RGB/BGR order
        // Some modules use BGR instead of RGB
        // This command sets the color order (command 0x36 - MADCTL)
        // Bit 3 controls RGB/BGR: 0 = RGB, 1 = BGR
        ESP_LOGI("LCD", "Configuring MADCTL for ST7789V (portrait mode without rotation)...");
        // ST7789V MADCTL configuration for portrait mode (240x240) without rotation
        // 0x00 = 00000000 binary:
        //   Bit 7 (MY=0): Normal row address order
        //   Bit 6 (MX=0): Normal column address order
        //   Bit 5 (MV=0): No Row/Column exchange (portrait mode)
        //   Bit 3 (BGR=0): RGB color order (with inversion below, becomes BGR effectively)
        // Result: Normal portrait orientation
        uint8_t madctl_value = 0x00;                                  // Portrait mode without rotation
        esp_lcd_panel_io_tx_param(io_handle, 0x36, &madctl_value, 1); // MADCTL command
        ESP_LOGI("LCD", "MADCTL configured to 0x00 (portrait mode without rotation)");

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
                                  int offset_y,
                                  int src_width,
                                  int src_height)
    {
        static const char *TAG_LCD = "LCD_render";

        if (!buffer || width <= 0 || height <= 0)
        {
            ESP_LOGE(TAG_LCD, "Invalid buffer or dimensions");
            return;
        }

        // Resolve source dimensions (0 = no scaling, source == destination)
        const int in_w = (src_width  > 0) ? src_width  : width;
        const int in_h = (src_height > 0) ? src_height : height;
        const bool scaling = (in_w != width || in_h != height);

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

        if (!scaling && total_bytes <= MAX_DMA_BYTES)
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
        int out_y = 0; // Output line counter

        for (; out_y < draw_height; out_y += CHUNK_LINES)
        {
            int lines = std::min(CHUNK_LINES, draw_height - out_y);

            // Prepare chunk — scale or copy
            if (scaling) {
                for (int line = 0; line < lines; ++line) {
                    int y_src = (out_y + line) * in_h / height;
                    const uint16_t* src_row = buffer + y_src * in_w;
                    uint16_t* dst_row = rgb565_chunk[buf] + line * draw_width;
                    for (int x = 0; x < draw_width; ++x) {
                        dst_row[x] = src_row[x * in_w / draw_width];
                    }
                }
            } else {
                const uint16_t *src = buffer + out_y * width;
                memcpy(rgb565_chunk[buf], src, draw_width * lines * sizeof(uint16_t));
            }

            // Wait for previous DMA before kicking off next transfer
            if (!first)
            {
                waitForTransfer();
            }
            first = false;

            esp_err_t ret = esp_lcd_panel_draw_bitmap(
                panel,
                offset_x,
                offset_y + out_y,
                offset_x + draw_width,
                offset_y + out_y + lines,
                rgb565_chunk[buf]);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG_LCD, "chunk draw failed at y=%d: %s",
                         out_y, esp_err_to_name(ret));
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
