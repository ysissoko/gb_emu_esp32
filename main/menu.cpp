#include "menu.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "menu";

namespace display::menu {

std::string Menu::loop() {
    // Load ROM list from storage
    rom_count = storage.list_roms(roms_names_list);

    if (rom_count <= 0) {
        ESP_LOGE(TAG, "No ROMs found!");
        return "";
    }

    ESP_LOGI(TAG, "Found %d ROMs", rom_count);

    // Main menu loop
    rom_selected = false;
    while (!rom_selected) {
        handleInputs();
        draw();

        // Small delay to avoid busy-waiting
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Return the full path to the selected ROM
    std::string selected_rom_path = storage.get_mount_path() + "/" +
                                    std::string(storage::ROMS_PATH) + "/" +
                                    roms_names_list[selected_rom_idx];

    ESP_LOGI(TAG, "Selected ROM: %s", selected_rom_path.c_str());
    return selected_rom_path;
}

void Menu::handleInputs() {
    // Handle up button - move cursor up
    if (joypad->buttonUpPressed()) {
        if (selected_rom_idx > 0) {
            selected_rom_idx--;
            ESP_LOGI(TAG, "Selected: %d", selected_rom_idx);
        }
        vTaskDelay(pdMS_TO_TICKS(150)); // Debounce delay
    }

    // Handle down button - move cursor down
    if (joypad->buttonDownPressed()) {
        if (selected_rom_idx < rom_count - 1) {
            selected_rom_idx++;
            ESP_LOGI(TAG, "Selected: %d", selected_rom_idx);
        }
        vTaskDelay(pdMS_TO_TICKS(150)); // Debounce delay
    }

    // Handle A button - select ROM
    if (joypad->buttonAPressed() || joypad->buttonStartPressed()) {
        rom_selected = true;
        ESP_LOGI(TAG, "ROM confirmed: %s", roms_names_list[selected_rom_idx].c_str());
        vTaskDelay(pdMS_TO_TICKS(200)); // Debounce delay
    }
}

void Menu::draw() {
    // Clear framebuffer
    clear_framebuffer(framebuffer, COLOR_BLACK);

    // Draw title
    draw_text(framebuffer, 8, 8, "SELECT ROM", COLOR_WHITE);

    // Calculate visible ROM range (show max 12 items on screen)
    constexpr int MAX_VISIBLE_ITEMS = 12;
    constexpr int ITEM_HEIGHT = 10;
    constexpr int START_Y = 24;

    int scroll_offset = 0;
    if (rom_count > MAX_VISIBLE_ITEMS) {
        // Scroll to keep selected item visible
        if (selected_rom_idx >= MAX_VISIBLE_ITEMS / 2) {
            scroll_offset = selected_rom_idx - MAX_VISIBLE_ITEMS / 2;
            if (scroll_offset + MAX_VISIBLE_ITEMS > rom_count) {
                scroll_offset = rom_count - MAX_VISIBLE_ITEMS;
            }
        }
    }

    // Draw ROM list
    int visible_count = (rom_count < MAX_VISIBLE_ITEMS) ? rom_count : MAX_VISIBLE_ITEMS;
    for (int i = 0; i < visible_count; i++) {
        int rom_idx = i + scroll_offset;
        int y_pos = START_Y + (i * ITEM_HEIGHT);

        // Determine if this is the selected item
        bool is_selected = (rom_idx == selected_rom_idx);
        uint8_t text_color = is_selected ? COLOR_YELLOW : COLOR_WHITE;

        // Draw cursor for selected item
        if (is_selected) {
            draw_char(framebuffer, 0, y_pos, '>', COLOR_YELLOW);
        }

        // Draw ROM name (truncate if too long)
        std::string rom_name = roms_names_list[rom_idx];
        if (rom_name.length() > 18) {
            rom_name = rom_name.substr(0, 15) + "...";
        }
        draw_text(framebuffer, 8, y_pos, rom_name, text_color);
    }

    // Draw scroll indicator if needed
    if (rom_count > MAX_VISIBLE_ITEMS) {
        int scroll_bar_height = (MAX_VISIBLE_ITEMS * 100) / rom_count;
        int scroll_bar_pos = (scroll_offset * 100) / (rom_count - MAX_VISIBLE_ITEMS);

        // Draw simple scroll indicator at the bottom
        if (scroll_offset > 0) {
            draw_char(framebuffer, LCD_WIDTH / 2 - 4, LCD_HEIGHT - 10, '^', COLOR_LIGHT_GRAY);
        }
        if (scroll_offset + MAX_VISIBLE_ITEMS < rom_count) {
            draw_char(framebuffer, LCD_WIDTH / 2 - 4, LCD_HEIGHT - 2, 'v', COLOR_LIGHT_GRAY);
        }
    }

    // Render the framebuffer to the LCD display
    display.renderFrame(framebuffer);
}

} // namespace display::menu
