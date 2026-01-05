#pragma once

#include "lcd_display.hpp"
#include "joypad.hpp"
#include "storage.hpp"
#include <memory>
#include <string>

namespace display::menu {
    class Menu {
        public:
            Menu(display::LCDDisplay& display,
                 std::shared_ptr<controller::Joypad> joypad)
                : display(display), joypad(joypad) {}

            ~Menu() {
                if (framebuffer) {
                    free(framebuffer);
                    framebuffer = nullptr;
                }
            }

            void init();
            void update();
            void draw();

            bool isRomSelected() const { return rom_selected; }
            std::string getSelectedRomPath() const;

        private:
            void handleInputs();

            display::LCDDisplay& display;
            std::shared_ptr<controller::Joypad> joypad;

            std::array<std::string, storage::MAX_ROMS> roms_names_list{};
            int rom_count{0};
            int selected_rom_idx{0};
            int prev_selected_rom_idx{-1};
            bool rom_selected{false};
            bool initialized{false};
            bool needs_redraw{true};

            static constexpr int MENU_WIDTH = 240;
            static constexpr int MENU_HEIGHT = 320;
            static constexpr int FB_CHUNK_HEIGHT = 320;  // Framebuffer pleine hauteur
            uint16_t* framebuffer{nullptr};  // Allocated dynamically (240x320 full screen)
    };
}
