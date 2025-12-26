#include "storage.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

static const char* TAG = "storage";

namespace storage {

    Storage::Storage(const std::string& mount_path) : mount_path(mount_path) {
    }

    int Storage::list_roms(std::array<std::string, MAX_ROMS>& roms_names_list) {
        std::string roms_dir = mount_path + "/" + ROMS_PATH;
        DIR* dir = opendir(roms_dir.c_str());
        if (!dir) return -1;

        struct dirent* ent;
        int count = 0;
        while ((ent = readdir(dir)) != nullptr && count < MAX_ROMS) {
            if (ent->d_type == DT_REG) {
                std::string name = ent->d_name;
                if (name.size() > 3 &&
                    (name.ends_with(".gb") ||
                    name.ends_with(".gbc"))) {
                    roms_names_list[count] = name;
                    count++;
                }
            }
        }

        closedir(dir);
        return count;
    }

    // Read binary file
    esp_err_t read_binary_file(const char* filepath, uint8_t** buffer, size_t* size) {
        ESP_LOGI(TAG, "Reading binary file: %s", filepath);
        
        FILE* f = fopen(filepath, "rb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open file");
            return ESP_FAIL;
        }
        
        // Get file size
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        // Allocate buffer
        *buffer = (uint8_t*)malloc(fsize);
        if (*buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory");
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
        
        // Read binary data
        size_t bytes_read = fread(*buffer, 1, fsize, f);
        *size = bytes_read;
        
        fclose(f);
        ESP_LOGI(TAG, "Binary file read: %d bytes", bytes_read);
        
        return ESP_OK;
    }

    bool file_exists(const char* filepath) {
        struct stat st;
        return (stat(filepath, &st) == 0);
    }

    // Get file size
    long get_file_size(const char* filepath) {
        struct stat st;
        if (stat(filepath, &st) == 0) {
            return st.st_size;
        }
        return -1;
    }

    esp_err_t Storage::init(const char* path) {
        esp_err_t ret;

        // configure SPI mapping
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = GPIO_NUM_11,
            .miso_io_num = GPIO_NUM_13,
            .sclk_io_num = GPIO_NUM_12,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };

        ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);


        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Failed to initialize SPI bus");
            } else {
                ESP_LOGE(TAG, "Failed to initialize SPI bus (%s)", esp_err_to_name(ret));
            }
            return ret;
        }

        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = GPIO_NUM_10;
        slot_config.host_id = SPI2_HOST;

        // mount sdcard
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024
        };

        sdmmc_card_t* card;
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        ret = esp_vfs_fat_sdspi_mount(path, &host, &slot_config,
                                &mount_config, &card);

        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Failed to mount filesystem");
            } else {
                ESP_LOGE(TAG, "Failed to initialize SD card (%s)", esp_err_to_name(ret));
            }
            return ret;
        }
        mount_path = path;

        return ESP_OK;
    }
}
