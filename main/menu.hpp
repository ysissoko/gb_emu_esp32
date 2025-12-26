#pragma once

#include "lcd_display.hpp"
#include "joypad.hpp"
#include "storage.hpp"
#include "text_renderer.hpp"
#include <memory>
#include <string>

namespace display::menu {
    class Menu {
        public:
            Menu(display::LCDDisplay& display,
                 std::shared_ptr<controller::Joypad> joypad,
                 storage::Storage& storage)
                : display(display), joypad(joypad), storage(storage) {}

            ~Menu() = default;

            // Running loop that displays and refreshes the screen until ROM is selected
            // Returns the path to the selected ROM
            std::string loop();

        private:
            void handleInputs();
            void draw();

            display::LCDDisplay& display;
            std::shared_ptr<controller::Joypad> joypad;
            storage::Storage& storage;

            std::array<std::string, storage::MAX_ROMS> roms_names_list{};
            int rom_count{0};
            int selected_rom_idx{0};
            bool rom_selected{false};

            std::array<uint8_t, display::LCD_WIDTH * display::LCD_HEIGHT> framebuffer{};
    };
}
