#include "storage.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

static const char *TAG = "storage";

namespace storage
{
    int list_roms(std::array<std::string, MAX_ROMS> &roms_names_list)
    {
        std::string roms_dir = MOUNT_PATH + "/" + ROMS_PATH;
        ESP_LOGI(TAG, "Looking for ROMs in: %s", roms_dir.c_str());
        DIR *dir = opendir(roms_dir.c_str());
        if (!dir)
        {
            ESP_LOGE(TAG, "Failed to open directory: %s", roms_dir.c_str());
            return -1;
        }

        struct dirent *ent;
        int count = 0;
        int total_entries = 0;
        const int MAX_ENTRIES = 1000; // Safety limit to prevent infinite loop

        ESP_LOGI(TAG, "Starting to read directory entries...");

        while ((ent = readdir(dir)) != nullptr && count < MAX_ROMS)
        {
            total_entries++;

            // Safety check to prevent infinite loop
            if (total_entries > MAX_ENTRIES)
            {
                ESP_LOGW(TAG, "Reached maximum entry limit (%d), stopping scan", MAX_ENTRIES);
                break;
            }

            std::string name = ent->d_name;
            ESP_LOGD(TAG, "Entry %d: %s", total_entries, name.c_str());

            // Skip "." and ".." directories
            if (name == "." || name == "..")
            {
                ESP_LOGD(TAG, "Skipping directory entry: %s", name.c_str());
                continue;
            }

            // Skip hidden files, system files, and FAT32 artifacts
            // Skip files starting with: . (hidden), _ (system), ~ (FAT32 artifact)
            if (name.empty() || name[0] == '.' || name[0] == '_' || name.find('~') != std::string::npos)
            {
                ESP_LOGD(TAG, "Skipping hidden/system/artifact file: %s", name.c_str());
                continue;
            }

            // Check for ROM file extensions (case-insensitive)
            // Note: d_type is not reliable on FAT32, so we just check extensions
            if (name.size() > 3 &&
                (name.ends_with(".gb") || name.ends_with(".GB") ||
                 name.ends_with(".gbc") || name.ends_with(".GBC")))
            {
                roms_names_list[count] = name;
                count++;
                ESP_LOGI(TAG, "Found ROM #%d: %s", count, name.c_str());
            }
            else
            {
                ESP_LOGD(TAG, "Skipping non-ROM file: %s", name.c_str());
            }
        }

        ESP_LOGI(TAG, "Finished reading directory. Total entries: %d, ROMs found: %d", total_entries, count);
        closedir(dir);
        return count;
    }

    // Read binary file
    esp_err_t read_binary_file(const char *filepath, uint8_t **buffer, size_t *size)
    {
        ESP_LOGI(TAG, "Reading binary file: %s", filepath);

        FILE *f = fopen(filepath, "rb");
        if (f == NULL)
        {
            ESP_LOGE(TAG, "Failed to open file");
            return ESP_FAIL;
        }

        // Get file size
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        // Validate file size (prevent excessive allocations)
        if (fsize <= 0 || fsize > 8 * 1024 * 1024) {  // Max 8MB ROM
            ESP_LOGE(TAG, "Invalid ROM size: %ld bytes", fsize);
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }

        // Check available heap before allocation
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "Free heap before allocation: %zu bytes, ROM size: %ld bytes", free_heap, fsize);
        
        if (fsize > free_heap) {
            ESP_LOGE(TAG, "Not enough memory: need %ld bytes, available %zu bytes", fsize, free_heap);
            fclose(f);
            return ESP_ERR_NO_MEM;
        }

        // Allocate buffer with safety margin
        *buffer = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
        if (*buffer == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate %ld bytes for ROM", fsize);
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

    bool file_exists(const char *filepath)
    {
        struct stat st;
        return (stat(filepath, &st) == 0);
    }

    // Get file size
    long get_file_size(const char *filepath)
    {
        struct stat st;
        if (stat(filepath, &st) == 0)
        {
            return st.st_size;
        }
        return -1;
    }

    esp_err_t init()
    {
        // CS SD
        gpio_set_direction(SD_GPIO_CS, GPIO_MODE_OUTPUT);
        gpio_set_level(SD_GPIO_CS, 1);

        ESP_LOGI(TAG, "SD CS GPIO = %d", SD_GPIO_CS);

        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = SD_GPIO_CS;
        slot_config.host_id = SPI2_HOST;

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
            .disk_status_check_enable = false,
            .use_one_fat = false
        };

        sdmmc_card_t *card;
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        host.max_freq_khz = 20000; // sécurité

        ESP_LOGW(TAG, "SD_GPIO_CS = %d", SD_GPIO_CS);
        esp_err_t ret = esp_vfs_fat_sdspi_mount(
            MOUNT_PATH.c_str(),
            &host,
            &slot_config,
            &mount_config,
            &card);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to init SD (%s)", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_PATH.c_str());
        return ESP_OK;
    }

}
