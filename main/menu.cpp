#include "menu.hpp"
#include "text_renderer.hpp" // Nouveau header pour RGB565
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "menu";

namespace display::menu
{

    void Menu::init()
    {
        if (initialized)
        {
            return;
        }

        ESP_LOGI(TAG, "Initializing menu...");

        // Allocate smaller framebuffer (only 80 lines instead of 320)
        size_t fb_size = MENU_WIDTH * FB_CHUNK_HEIGHT * sizeof(uint16_t);
        ESP_LOGI(TAG, "Allocating framebuffer: %d bytes (%dx%d)", fb_size, MENU_WIDTH, FB_CHUNK_HEIGHT);
        ESP_LOGI(TAG, "Free heap: %lu bytes, Free SPIRAM: %lu bytes",
                 heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        // Try PSRAM first, fall back to DMA-capable RAM if needed
        framebuffer = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!framebuffer)
        {
            ESP_LOGW(TAG, "PSRAM allocation failed, trying DMA-capable RAM...");
            framebuffer = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        }

        if (!framebuffer)
        {
            ESP_LOGE(TAG, "Failed to allocate framebuffer! Need %d bytes", fb_size);
            ESP_LOGE(TAG, "Available: Internal=%lu, SPIRAM=%lu",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            return;
        }

        ESP_LOGI(TAG, "Framebuffer allocated successfully at %p", framebuffer);

        // Clear framebuffer explicitly to avoid "snow"
        ESP_LOGI(TAG, "Clearing framebuffer chunk...");
        clear_framebuffer_rgb565(framebuffer, MENU_WIDTH * FB_CHUNK_HEIGHT, RGB565_BLACK);

        ESP_LOGI(TAG, "About to call storage::list_roms()...");
        rom_count = storage::list_roms(roms_names_list);

        if (rom_count <= 0)
        {
            ESP_LOGE(TAG, "No ROMs found!");
            return;
        }

        ESP_LOGI(TAG, "Found %d ROMs", rom_count);
        rom_selected = false;
        selected_rom_idx = 0;
        needs_redraw = true;
        initialized = true;
    }

    void Menu::update()
    {
        if (!initialized || rom_selected)
        {
            return;
        }

        handleInputs();
    }

    std::string Menu::getSelectedRomPath() const
    {
        if (!rom_selected)
        {
            return "";
        }

        std::string selected_rom_path = std::string(storage::MOUNT_PATH) + "/" +
                                        std::string(storage::ROMS_PATH) + "/" +
                                        roms_names_list[selected_rom_idx];

        ESP_LOGI(TAG, "Selected ROM: %s", selected_rom_path.c_str());
        return selected_rom_path;
    }

    void Menu::handleInputs()
    {
        static bool prev_up = false;
        static bool prev_down = false;
        static bool prev_a = false;
        static bool prev_start = false;
        static int log_counter = 0;

        bool up_pressed = joypad->buttonUpPressed();
        bool down_pressed = joypad->buttonDownPressed();
        bool a_pressed = joypad->buttonAPressed();
        bool start_pressed = joypad->buttonStartPressed();

        // Log button states every 50 frames (~2.5 seconds at 20fps)
        if (++log_counter % 50 == 0)
        {
            ESP_LOGI(TAG, "Buttons: UP=%d DOWN=%d A=%d START=%d",
                     up_pressed, down_pressed, a_pressed, start_pressed);
        }

        // Detect rising edge (button just pressed, not held)
        if (up_pressed && !prev_up)
        {
            if (selected_rom_idx > 0)
            {
                selected_rom_idx--;
                ESP_LOGI(TAG, "Selected: %d", selected_rom_idx);
                needs_redraw = true;
            }
        }
        else if (down_pressed && !prev_down)
        {
            if (selected_rom_idx < rom_count - 1)
            {
                selected_rom_idx++;
                ESP_LOGI(TAG, "Selected: %d", selected_rom_idx);
                needs_redraw = true;
            }
        }

        if ((a_pressed && !prev_a) || (start_pressed && !prev_start))
        {
            rom_selected = true;
            ESP_LOGI(TAG, "ROM confirmed: %s", roms_names_list[selected_rom_idx].c_str());
        }

        // Update previous states
        prev_up = up_pressed;
        prev_down = down_pressed;
        prev_a = a_pressed;
        prev_start = start_pressed;
    }

    void Menu::draw()
    {
        // Don't draw if ROM is already selected
        if (rom_selected)
        {
            // Effacer le framebuffer RGB565
            clear_framebuffer_rgb565(framebuffer, MENU_WIDTH * MENU_HEIGHT, RGB565_BLACK);
            return;
        }

        // Only redraw if needed
        if (!needs_redraw)
        {
            return;
        }

        ESP_LOGI(TAG, "Drawing menu (needs_redraw=true, rom_count=%d, selected_idx=%d)...", rom_count, selected_rom_idx);

        // Effacer le framebuffer RGB565
        clear_framebuffer_rgb565(framebuffer, MENU_WIDTH * MENU_HEIGHT, RGB565_BLACK);

        // Dessiner le titre
        draw_text_rgb565(framebuffer, MENU_WIDTH, MENU_HEIGHT,
                         20, 20, "SELECT ROM", RGB565_WHITE);

        ESP_LOGI(TAG, "Title drawn at (20,20), color=0x%04X", RGB565_WHITE);

        // Calculer la zone visible (max 30 items sur un écran 240×320)
        constexpr uint8_t MAX_VISIBLE_ITEMS = 30;
        constexpr uint8_t ITEM_HEIGHT = 10;
        constexpr uint8_t START_Y = 40;

        int scroll_offset = 0;
        if (rom_count > MAX_VISIBLE_ITEMS)
        {
            if (selected_rom_idx >= (MAX_VISIBLE_ITEMS >> 1))
            {
                scroll_offset = selected_rom_idx - (MAX_VISIBLE_ITEMS >> 1);
                if (scroll_offset + MAX_VISIBLE_ITEMS > rom_count)
                {
                    scroll_offset = rom_count - MAX_VISIBLE_ITEMS;
                }
            }
        }

        // Dessiner la liste des ROMs
        int visible_count = (rom_count < MAX_VISIBLE_ITEMS) ? rom_count : MAX_VISIBLE_ITEMS;
        for (int i = 0; i < visible_count; i++)
        {
            int rom_idx = i + scroll_offset;
            int y_pos = START_Y + (i * ITEM_HEIGHT);

            bool is_selected = (rom_idx == selected_rom_idx);
            uint16_t text_color = is_selected ? RGB565_YELLOW : RGB565_WHITE;

            // Curseur pour l'item sélectionné
            if (is_selected)
            {
                draw_char_rgb565(framebuffer, MENU_WIDTH, MENU_HEIGHT,
                                 10, y_pos, '>', RGB565_YELLOW);
            }
            // Nom du ROM (tronquer si trop long - adapter pour largeur réelle)
            std::string rom_name = roms_names_list[rom_idx];
            int max_chars = (MENU_WIDTH - 20) / 8; // 8 pixels par caractère, marge de 20px
            if (max_chars < 10) max_chars = 10; // Minimum absolu
            
            if (rom_name.length() > max_chars && rom_name.length() > 8)
            {
                rom_name = rom_name.substr(0, max_chars - 3) + "...";
            }
            draw_text_rgb565(framebuffer, MENU_WIDTH, MENU_HEIGHT,
                             20, y_pos, rom_name, text_color);
        }

        // Indicateurs de scroll
        if (rom_count > MAX_VISIBLE_ITEMS)
        {
            if (scroll_offset > 0)
            {
                draw_char_rgb565(framebuffer, MENU_WIDTH, MENU_HEIGHT,
                                 MENU_WIDTH / 2 - 4, 30, '^', RGB565_LIGHT_GRAY);
            }
            if (scroll_offset + MAX_VISIBLE_ITEMS < rom_count)
            {
                draw_char_rgb565(framebuffer, MENU_WIDTH, MENU_HEIGHT,
                                 MENU_WIDTH / 2 - 4, MENU_HEIGHT - 10, 'v', RGB565_LIGHT_GRAY);
            }
        }

        // Afficher directement le framebuffer RGB565
        ESP_LOGI(TAG, "Sending %dx%d framebuffer to display...", MENU_WIDTH, MENU_HEIGHT);
        ESP_LOGI(TAG, "Framebuffer address: %p, first pixel: 0x%04X", framebuffer, framebuffer ? framebuffer[0] : 0);
        display.renderFrameRGB565(framebuffer, MENU_WIDTH, MENU_HEIGHT, 0, 0);
        ESP_LOGI(TAG, "Framebuffer sent to display");

        // Mark as drawn
        needs_redraw = false;
        prev_selected_rom_idx = selected_rom_idx;
        ESP_LOGI(TAG, "Menu drawn successfully, needs_redraw=false");
    }
} // namespace display::menu
