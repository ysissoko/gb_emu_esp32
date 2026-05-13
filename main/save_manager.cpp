#include "save_manager.hpp"
#include "esp_log.h"
#include "esp_attr.h"
#include <algorithm>
#include <cstring>
#include <stdio.h>

static const char* TAG = "SaveManager";

namespace save_manager
{
    bool has_battery(uint8_t cart_type)
    {
        // Check if cartridge has battery-backed SRAM
        switch (cart_type)
        {
            case 0x03: // MBC1+RAM+BATTERY
            case 0x06: // MBC2+BATTERY
            case 0x09: // ROM+RAM+BATTERY
            case 0x0D: // MMM01+RAM+BATTERY
            case 0x0F: // MBC3+TIMER+BATTERY
            case 0x10: // MBC3+TIMER+RAM+BATTERY
            case 0x13: // MBC3+RAM+BATTERY
            case 0x1B: // MBC5+RAM+BATTERY
            case 0x1E: // MBC5+RUMBLE+RAM+BATTERY
            case 0x22: // MBC7+SENSOR+RUMBLE+RAM+BATTERY
            case 0xFF: // HuC1+RAM+BATTERY
                return true;
            default:
                return false;
        }
    }

    std::string get_sav_path(const std::string& rom_path)
    {
        // Find last '.' to replace extension
        size_t dot_pos = rom_path.find_last_of('.');
        if (dot_pos == std::string::npos)
        {
            // No extension, just append .sav
            return rom_path + ".sav";
        }

        // Replace extension with .sav
        return rom_path.substr(0, dot_pos) + ".sav";
    }

    esp_err_t load_sram(const std::string& rom_path, uint8_t* sram_buffer, size_t sram_size)
    {
        std::string sav_path = get_sav_path(rom_path);
        ESP_LOGI(TAG, "Loading SRAM from: %s", sav_path.c_str());

        FILE* f = fopen(sav_path.c_str(), "rb");
        if (!f)
        {
            ESP_LOGW(TAG, "No save file found (new game)");
            return ESP_ERR_NOT_FOUND;
        }

        // Get file size
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_size != sram_size)
        {
            ESP_LOGW(TAG, "Save file size mismatch: expected %zu, got %ld", sram_size, file_size);
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }

        // Read SRAM
        size_t bytes_read = fread(sram_buffer, 1, sram_size, f);
        fclose(f);

        if (bytes_read != sram_size)
        {
            ESP_LOGE(TAG, "Failed to read SRAM: read %zu/%zu bytes", bytes_read, sram_size);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "SRAM loaded successfully: %zu bytes", bytes_read);
        return ESP_OK;
    }

    esp_err_t save_sram(const std::string& rom_path, const uint8_t* sram_buffer, size_t sram_size)
    {
        std::string sav_path = get_sav_path(rom_path);
        ESP_LOGI(TAG, "Saving SRAM to: %s", sav_path.c_str());

        FILE* f = fopen(sav_path.c_str(), "wb");
        if (!f)
        {
            ESP_LOGE(TAG, "Failed to open save file for writing");
            return ESP_FAIL;
        }

        // sram_buffer may point to PSRAM. Passing a PSRAM pointer directly to fwrite() causes
        // the SDSPI driver to attempt SPI DMA from PSRAM, which flushes/disables the PSRAM cache
        // and crashes the other core with a LoadProhibited fault. Copy to internal DRAM first.
        static constexpr size_t CHUNK = 4096;
        static DRAM_ATTR uint8_t chunk_buf[CHUNK];

        size_t written = 0;
        while (written < sram_size) {
            size_t len = std::min(CHUNK, sram_size - written);
            memcpy(chunk_buf, sram_buffer + written, len);
            size_t w = fwrite(chunk_buf, 1, len, f);
            written += w;
            if (w != len) break;
        }
        fclose(f);

        if (written != sram_size)
        {
            ESP_LOGE(TAG, "Failed to write SRAM: wrote %zu/%zu bytes", written, sram_size);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "SRAM saved successfully: %zu bytes", written);
        return ESP_OK;
    }
}
