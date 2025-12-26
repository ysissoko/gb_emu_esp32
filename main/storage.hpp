#pragma once

#include <string>
#include <array>
#include <cstdint>
#include "esp_err.h"

namespace storage {
    constexpr const uint8_t MAX_CHARS_ROM_NAME = 48;
    constexpr size_t MAX_ROMS = 64;
    constexpr const char* ROMS_PATH = "roms";

    class Storage {
        public:
            Storage(const std::string& mount_path);
            ~Storage() = default;
            int list_roms(std::array<std::string, MAX_ROMS>&);
            esp_err_t init(const char* path);

            const std::string& get_mount_path() const { return mount_path; }

        private:
            std::string mount_path{""};
    };

    // Utility functions
    esp_err_t read_binary_file(const char* filepath, uint8_t** buffer, size_t* size);
    bool file_exists(const char* filepath);
    long get_file_size(const char* filepath);
}
