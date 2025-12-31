#include "lcd_display.hpp"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

namespace display
{
    // Optimized: Use constexpr lookup table instead of switch
    static constexpr uint16_t COLOR_PALETTE[4] = {
        0xFFFF,  // WHITE
        0xC618,  // LIGHT_GRAY
        0x632C,  // DARK_GRAY
        0x0000   // BLACK
    };

    void LCDDisplay::initialize()
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

        // Configure LCD panel IO
        esp_lcd_panel_io_handle_t io_handle = nullptr;
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = GPIO_LCD_CS;   // CS integrated on LCD module (tied to GND)
        io_cfg.dc_gpio_num = GPIO_LCD_DC;   // DC
        io_cfg.spi_mode = 0;
        io_cfg.pclk_hz = 10 * 1000 * 1000; // 10 MHz for stability
        io_cfg.trans_queue_depth = 10;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 8;
        io_cfg.flags.dc_low_on_data = 0;  // DC high for data

        esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                 &io_cfg, &io_handle);
        ESP_LOGI("LCD", "esp_lcd_new_panel_io_spi returned: %d", ret);

        // Configure LCD panel
        ESP_LOGI("LCD", "Configuring LCD panel...");
        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = GPIO_LCD_RST;  // RES
        // panel_cfg.color_space = ESP_LCD_COLOR_SPACE_RGB;
        panel_cfg.rgb_endian  = LCD_RGB_ENDIAN_RGB;
        panel_cfg.bits_per_pixel = 16;

        ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel);
        ESP_LOGI("LCD", "esp_lcd_new_panel_st7789 returned: %d", ret);

        ESP_LOGI("LCD", "Resetting panel...");
        esp_lcd_panel_reset(panel);

        ESP_LOGI("LCD", "Initializing panel...");
        esp_lcd_panel_init(panel);

        ESP_LOGI("LCD", "Turning display on (before config)...");
        esp_lcd_panel_disp_on_off(panel, true);

        ESP_LOGI("LCD", "Setting gap offsets...");
        esp_lcd_panel_set_gap(panel, 0, 0);  // No offset for standard 240x240

        ESP_LOGI("LCD", "Setting display orientation...");
        esp_lcd_panel_mirror(panel, false, false);
        esp_lcd_panel_swap_xy(panel, false);

        ESP_LOGI("LCD", "Testing color inversion - trying WITHOUT inversion first...");
        esp_lcd_panel_invert_color(panel, false);  // Try without inversion first

        ESP_LOGI("LCD", "LCD initialization complete!");
    }

    void LCDDisplay::renderFrame(const std::array<uint8_t, LCD_WIDTH * LCD_HEIGHT>& frameBuffer)
    {
        // Optimized: Removed unnecessary buf.fill(0) - all pixels are overwritten anyway
        // Optimized: Direct palette lookup instead of function call + switch
        for (size_t i = 0; i < frameBuffer.size(); ++i) {
            buf[i] = COLOR_PALETTE[frameBuffer[i] & 0x3];
        }

        esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, buf.data());
    }
}
