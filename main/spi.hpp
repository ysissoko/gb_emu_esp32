#pragma once

#include "driver/spi_master.h"
#include "driver/gpio.h"

namespace spi {
    // SPI commun
    constexpr gpio_num_t SPI_MISO = GPIO_NUM_16; // SD uniquement
    constexpr gpio_num_t SPI_MOSI = GPIO_NUM_17;
    constexpr gpio_num_t SPI_SCK  = GPIO_NUM_18;

    inline esp_err_t init() {
        // Initialize shared SPI bus (used by LCD and SD card)
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = SPI_MOSI;
        buscfg.miso_io_num = SPI_MISO;
        buscfg.sclk_io_num = SPI_SCK;
        buscfg.quadwp_io_num = -1;
        buscfg.quadhd_io_num = -1;
        buscfg.max_transfer_sz = 240 * 240 * 2;
        return spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    }
}
