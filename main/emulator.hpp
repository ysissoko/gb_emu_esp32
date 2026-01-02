#pragma once

#include "spi.hpp"
#include "memory_bus.hpp"
#include "cpu.hpp"
#include "joypad.hpp"
#include "lcd_display.hpp"
#include "storage.hpp"
#include "menu.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace emulator {
    enum class AppState {
        MENU,
        RUNNING_GAME
    };
    
    class Emulator {
        public:
            Emulator() = default;
            ~Emulator() = default;

            esp_err_t init();
            void run();
            void start();
            void loadROM(const uint8_t* rom_buffer, size_t rom_size);
            
        private:
            static void emulator_task(void *arg);
            void loadROMFile(const std::string& rom_path);
            std::string showMenuAndSelectROM();

            AppState app_state{AppState::MENU};
            bool rom_loaded{false};

            std::unique_ptr<cpu::CPU> cpu{nullptr};
            std::unique_ptr<ppu::PPU> ppu{nullptr};
            std::unique_ptr<memory::MemoryBus> mmu{nullptr};
            std::shared_ptr<controller::Joypad> joypad{nullptr};
            std::shared_ptr<display::LCDDisplay> lcd_display{nullptr};

            static const char* TAG;
            static constexpr int64_t FRAME_US = 16742; // ~59.7 FPS
            static constexpr int MENU_FRAME_MS = 50; // ~20 FPS for menu

            // Performance monitoring: lag accumulator for dynamic frame skipping
            mutable int64_t lag_us{0};
    };
}
