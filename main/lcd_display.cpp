#include "lcd_display.hpp"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"

namespace ppu::display
{
    static uint16_t getColorFromPalette(display::Color c) {
        switch(c) {
            case display::Color::WHITE:
            return 0xFFFF;
            case display::Color::LIGHT_GRAY:
            return 0xC618;
            case display::Color::DARK_GRAY:
            return 0x632C;
            case display::Color::BLACK:
            return 0x0000;
        }
        
        return 0xFFFF;
    }

    void LCDDisplay::initialize()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = 11;
        buscfg.miso_io_num = -1;
        buscfg.sclk_io_num = 12;
        buscfg.quadwp_io_num = -1;
        buscfg.quadhd_io_num = -1;
        buscfg.max_transfer_sz = 240 * 240 * 2;
        spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

        esp_lcd_panel_io_handle_t io_handle = nullptr;
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = 9;
        io_cfg.dc_gpio_num = 10;
        io_cfg.spi_mode = 0;
        io_cfg.pclk_hz = 40 * 1000 * 1000; // 40 MHz
        io_cfg.trans_queue_depth = 10;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 8;
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                 &io_cfg, &io_handle);

        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = 8;
        panel_cfg.color_space = LCD_RGB_ENDIAN_RGB;
        panel_cfg.bits_per_pixel = 16;
        esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel);

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
    }

    void LCDDisplay::renderFrame(std::array<uint8_t, LCD_WIDTH * LCD_HEIGHT> frameBuffer)
    {
        buf.fill(0);

        // convert the frame buffer to RGB565
        for (size_t i=0; i< frameBuffer.size(); ++i) {
            buf[i] = getColorFromPalette(static_cast<ppu::display::Color>(frameBuffer[i] & 0x3));
        }

        esp_lcd_panel_draw_bitmap(panel, 0, 0, 240, 240, buf.data());
    }
}
