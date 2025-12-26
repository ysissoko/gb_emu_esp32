#include "cpu.hpp"
#include "lcd_display.hpp"
#include "joypad.hpp"
#include "menu.hpp"
#include "esp_system.h"
#include "esp_log.h"
#include "storage.hpp"

#include <cstdio>
#include <memory>

static const char* TAG = "main";

extern "C" void app_main(void);

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Hello GB EMU ESP32!");
    ESP_LOGI(TAG, "Chip revision: %s", esp_get_idf_version());

    // Initialize LCD display
    auto lcd_display = std::make_unique<display::LCDDisplay>();
    lcd_display->initialize();

    // Initialize joypad
    auto joypad = std::make_shared<controller::Joypad>();

    // Initialize storage (SD card)
    storage::Storage storage("/sdcard");
    esp_err_t ret = storage.init("/sdcard");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize storage!");
        return;
    }

    // Initialize memory bus
    memory::MemoryBus mmu{joypad};

    // Show menu and get selected ROM
    std::string selected_rom_path;
    {
        display::menu::Menu menu(*lcd_display, joypad, storage);
        selected_rom_path = menu.loop();

        if (selected_rom_path.empty()) {
            ESP_LOGE(TAG, "No ROM selected!");
            return;
        }
    }

    // Load the selected ROM from storage
    uint8_t* rom_buffer = nullptr;
    size_t rom_size = 0;
    ret = storage::read_binary_file(selected_rom_path.c_str(), &rom_buffer, &rom_size);

    if (ret != ESP_OK || rom_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to read ROM file: %s", selected_rom_path.c_str());
        return;
    }

    ESP_LOGI(TAG, "Loaded ROM: %s (%zu bytes)", selected_rom_path.c_str(), rom_size);

    // Load ROM into memory bus
    mmu.loadROM(rom_buffer, rom_size);

    // Free the ROM buffer after loading (MMU should have copied it)
    free(rom_buffer);

    // Initialize PPU and CPU
    ppu::PPU ppu(mmu, std::move(lcd_display));
    cpu::CPU cpu(mmu, ppu);

    ESP_LOGI(TAG, "Starting emulation...");
    cpu.run();
}
