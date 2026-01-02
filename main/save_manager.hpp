#pragma once

#include <string>
#include <cstdint>
#include "esp_err.h"

namespace save_manager
{
    // Load SRAM from SD card (.sav file)
    // Returns ESP_OK if loaded, ESP_ERR_NOT_FOUND if no save exists
    esp_err_t load_sram(const std::string& rom_path, uint8_t* sram_buffer, size_t sram_size);

    // Save SRAM to SD card (.sav file)
    esp_err_t save_sram(const std::string& rom_path, const uint8_t* sram_buffer, size_t sram_size);

    // Check if ROM has battery-backed SRAM (from cartridge type)
    bool has_battery(uint8_t cart_type);

    // Convert ROM path to SAV path (e.g., /sd/roms/game.gb -> /sd/roms/game.sav)
    std::string get_sav_path(const std::string& rom_path);
}
