#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#include <string>
#include <array>
#include <cstdint>

namespace storage {
    constexpr const gpio_num_t SD_GPIO_CS = GPIO_NUM_10;
    constexpr const uint8_t MAX_CHARS_ROM_NAME = 48;
    constexpr size_t MAX_ROMS = 64;
    constexpr const std::string ROMS_PATH = "roms";
    constexpr const std::string MOUNT_PATH = "/sdcard";
        
    int list_roms(std::array<std::string, MAX_ROMS>&);
    esp_err_t init();
    // Utility functions
    esp_err_t read_binary_file(const char* filepath, uint8_t** buffer, size_t* size);
    bool file_exists(const char* filepath);
    long get_file_size(const char* filepath);
}
