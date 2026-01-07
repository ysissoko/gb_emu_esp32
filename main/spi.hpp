#pragma once

#include "driver/spi_master.h"
#include "gpio_pins.hpp"

namespace spi {

    inline esp_err_t init() {
        // Initialize shared SPI bus (used by LCD and SD card)
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = gpio::SPI_MOSI;
        buscfg.miso_io_num = gpio::SPI_MISO;
        buscfg.sclk_io_num = gpio::SPI_SCK;
        buscfg.quadwp_io_num = -1;
        buscfg.quadhd_io_num = -1;
        buscfg.max_transfer_sz = 240 * 240 * 2;
        return spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    }
}
