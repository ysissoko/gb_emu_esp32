#include "cpu.hpp"
#include "lcd_display.hpp"
#include "joypad.hpp"
#include "menu.hpp"
#include "esp_system.h"
#include "esp_log.h"
#include "storage.hpp"
#include "driver/spi_master.h"

#include <cstdio>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include "freertos/task.h"

// SPI commun
constexpr gpio_num_t SPI_MISO = GPIO_NUM_16; // SD uniquement
constexpr gpio_num_t SPI_MOSI = GPIO_NUM_17;
constexpr gpio_num_t SPI_SCK  = GPIO_NUM_18;

static const char* TAG = "main";

extern "C" void app_main(void);

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Starting GB EMU ESP32 ===");
    ESP_LOGI(TAG, "Chip revision: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Enable debug logs for storage module
    esp_log_level_set("storage", ESP_LOG_DEBUG);

    // Wait for power to stabilize
    ESP_LOGI(TAG, "Waiting for power stabilization...");
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize shared SPI bus (used by LCD and SD card)
    ESP_LOGI(TAG, "Initializing shared SPI bus...");
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = SPI_MOSI;
    buscfg.miso_io_num = SPI_MISO;
    buscfg.sclk_io_num = SPI_SCK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 240 * 240 * 2;
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus! Error: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI bus initialized!");

    // Initialize storage (SD card) FIRST to avoid SPI conflicts
    ESP_LOGI(TAG, "Initializing storage...");
    storage::Storage storage("/sdcard");
    ret = storage.init("/sdcard");
    ESP_LOGI(TAG, "Storage init returned with code: %d", ret);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize storage!");
        return;
    }
    ESP_LOGI(TAG, "Storage initialized!");

    // Check if /sdcard/roms directory exists
    struct stat st;
    if (stat("/sdcard/roms", &st) != 0) {
        ESP_LOGW(TAG, "Directory /sdcard/roms does not exist, creating it...");
        mkdir("/sdcard/roms", 0755);
    } else {
        ESP_LOGI(TAG, "Directory /sdcard/roms exists");
    }

    // Wait before initializing LCD to ensure SD card is stable
    ESP_LOGI(TAG, "Waiting before LCD init...");
    vTaskDelay(pdMS_TO_TICKS(200));

    // Initialize LCD display AFTER SD card
    ESP_LOGI(TAG, "Creating LCD display...");
    auto lcd_display = std::make_unique<display::LCDDisplay>();
    ESP_LOGI(TAG, "Initializing LCD display...");
    lcd_display->initialize();
    ESP_LOGI(TAG, "LCD display initialized!");

    // Test LCD with multiple colors
    ESP_LOGI(TAG, "Testing LCD with WHITE (via renderFrame)...");
    std::array<uint8_t, 160 * 144> test_buffer;
    test_buffer.fill(0);  // Fill with color 0 (WHITE in our palette = 0xFFFF)
    lcd_display->renderFrame(test_buffer);
    ESP_LOGI(TAG, "Screen should now be WHITE. Waiting 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Testing LCD with BLACK...");
    test_buffer.fill(3);  // Fill with color 3 (BLACK in our palette = 0x0000)
    lcd_display->renderFrame(test_buffer);
    ESP_LOGI(TAG, "Screen should now be BLACK. Waiting 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Testing LCD with LIGHT GRAY...");
    test_buffer.fill(1);  // Fill with color 1 (LIGHT_GRAY = 0xC618)
    lcd_display->renderFrame(test_buffer);
    ESP_LOGI(TAG, "Screen should now be LIGHT GRAY. Waiting 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Testing LCD with DARK GRAY...");
    test_buffer.fill(2);  // Fill with color 2 (DARK_GRAY = 0x632C)
    lcd_display->renderFrame(test_buffer);
    ESP_LOGI(TAG, "Screen should now be DARK GRAY. Waiting 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Initialize joypad
    ESP_LOGI(TAG, "Creating joypad...");
    auto joypad = std::make_shared<controller::Joypad>();
    ESP_LOGI(TAG, "Joypad created!");

    // Initialize memory bus (allocated on heap due to large size ~56KB)
    ESP_LOGI(TAG, "Creating memory bus...");
    auto mmu = std::make_unique<memory::MemoryBus>(joypad);
    ESP_LOGI(TAG, "Memory bus created!");

    // Show menu and get selected ROM (allocated on heap due to large framebuffer ~23KB)
    ESP_LOGI(TAG, "Starting menu...");
    std::string selected_rom_path;
    {
        auto menu = std::make_unique<display::menu::Menu>(*lcd_display, joypad, storage);
        selected_rom_path = menu->loop();

        if (selected_rom_path.empty()) {
            ESP_LOGE(TAG, "No ROM selected!");
            return;
        }
    }
    ESP_LOGI(TAG, "ROM selected: %s", selected_rom_path.c_str());

    // Load the selected ROM from storage
    ESP_LOGI(TAG, "Loading ROM file...");
    uint8_t* rom_buffer = nullptr;
    size_t rom_size = 0;
    ret = storage::read_binary_file(selected_rom_path.c_str(), &rom_buffer, &rom_size);

    if (ret != ESP_OK || rom_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to read ROM file: %s", selected_rom_path.c_str());
        return;
    }

    ESP_LOGI(TAG, "Loaded ROM: %s (%zu bytes)", selected_rom_path.c_str(), rom_size);

    // Load ROM into memory bus
    ESP_LOGI(TAG, "Loading ROM into MMU...");
    mmu->loadROM(rom_buffer, rom_size);
    ESP_LOGI(TAG, "ROM loaded into MMU!");

    // Free the ROM buffer after loading (MMU should have copied it)
    free(rom_buffer);

    // Initialize PPU and CPU (allocated on heap due to large size)
    ESP_LOGI(TAG, "Creating PPU...");
    auto ppu = std::make_unique<ppu::PPU>(*mmu, std::move(lcd_display));
    ESP_LOGI(TAG, "Creating CPU...");
    auto cpu = std::make_unique<cpu::CPU>(*mmu, *ppu);

    ESP_LOGI(TAG, "Starting emulation...");
    cpu->run();
    ESP_LOGI(TAG, "Emulation ended.");
}
