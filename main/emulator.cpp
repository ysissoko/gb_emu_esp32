#include "emulator.hpp"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_log.h"

namespace emulator
{
    const char *Emulator::TAG = "emulator";

    std::string Emulator::showMenuAndSelectROM()
    {
        ESP_LOGI(TAG, "creating menu...");

        // Allocate menu on stack (temporary allocation)
        // The menu framebuffer (~150KB) will be freed after ROM selection
        display::menu::Menu menu(*lcd_display, joypad);

        ESP_LOGI(TAG, "Menu object created successfully");
        ESP_LOGI(TAG, "Free heap after menu: %lu bytes", esp_get_free_heap_size());

        menu.init();

        ESP_LOGI(TAG, "Starting menu loop...");
        int loop_count = 0;

        // Run menu loop until ROM selected
        while (!menu.isRomSelected())
        {
            menu.update();
            menu.draw();

            loop_count++;
            if (loop_count % 20 == 0)
            { // Log every second (20 * 50ms = 1s)
                ESP_LOGI(TAG, "Menu loop iteration %d, rom_selected=%d", loop_count, menu.isRomSelected());
            }

            vTaskDelay(pdMS_TO_TICKS(MENU_FRAME_MS));
        }

        ESP_LOGI(TAG, "ROM selected! Exited menu loop after %d iterations", loop_count);

        // Clear the menu display before loading ROM
        ESP_LOGI(TAG, "Clearing menu display...");

        std::string selected_rom = menu.getSelectedRomPath();
        ESP_LOGI(TAG, "ROM selected: %s", selected_rom.c_str());
        ESP_LOGI(TAG, "Menu will be destroyed, freeing ~150KB");

        return selected_rom;
    }

    void Emulator::emulator_task(void *arg)
    {
        Emulator *emulator = static_cast<Emulator *>(arg);

        ESP_LOGI(TAG, "Emulator task started!");

        // Unsubscribe from watchdog for this task (emulation can take longer than 5s)
        esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
        ESP_LOGI(TAG, "Task watchdog disabled for emulator task");

        // Show menu and load ROM (only once at startup)
        if (!emulator->rom_loaded)
        {
            std::string rom_path = emulator->showMenuAndSelectROM();

            if (!rom_path.empty())
            {
                ESP_LOGI(TAG, "Loading ROM: %s", rom_path.c_str());
                emulator->loadROMFile(rom_path);
                emulator->rom_loaded = true;
                emulator->app_state = AppState::RUNNING_GAME;
                ESP_LOGI(TAG, "ROM loaded, switching to RUNNING_GAME state");
                ESP_LOGI(TAG, "Free heap after menu destroyed: %lu bytes", esp_get_free_heap_size());
            }
            else
            {
                ESP_LOGE(TAG, "No ROM selected, halting");
                vTaskDelete(nullptr);
                return;
            }
        }

        // Main emulation loop
        ESP_LOGI(TAG, "Starting Game Boy emulation...");

// Log optimization settings
#ifdef NDEBUG
        ESP_LOGI(TAG, "Build: RELEASE mode (optimizations enabled)");
#else
        ESP_LOGW(TAG, "Build: DEBUG mode (optimizations disabled!)");
#endif

        ESP_LOGI(TAG, "Running on CPU core: %d", xPortGetCoreID());

        int frame_count = 0;
        TickType_t last_wake_time = xTaskGetTickCount();

        while (true)
        {
            int64_t start = esp_timer_get_time();

            // Run one frame of the Game Boy
            int64_t emu_start = esp_timer_get_time();
            emulator->cpu->run_frame();
            int64_t emu_end = esp_timer_get_time();
            int64_t emu_time = emu_end - emu_start;

            frame_count++;

            // Log FPS every 60 frames (~1 second) avec profiling détaillé
            if (frame_count % 60 == 0)
            {
                int64_t frame_time = esp_timer_get_time() - start;
                float fps = 1000000.0f / frame_time;
                float target_fps = 60.0f;
                int64_t target_frame_time = 16743; // 60 FPS target (1000000 / 60)

                ESP_LOGI(TAG, "Frame %d | Time: %lld us (%.1f FPS) | Target: %lld us (%.0f FPS) | Emulation: %lld us | Overhead: %lld us | Heap: %lu",
                         frame_count, frame_time, fps, target_frame_time, target_fps,
                         emu_time, frame_time - emu_time, esp_get_free_heap_size());
            }

            // Maintain timing - always yield to prevent watchdog
            int64_t elapsed = esp_timer_get_time() - start;
            int64_t delay_us = FRAME_US - elapsed;

            if (delay_us > 1000)
            {
                // Délai normal - libérer le CPU
                vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
            }
            else
            {
                // Toujours yield, même si on est en retard
                taskYIELD();
            }
        }
    }

    esp_err_t Emulator::init()
    {
        ESP_LOGI(TAG, "Waiting for power stabilization...");
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_err_t ret = spi::init();
        ESP_LOGI(TAG, "Initializing shared SPI bus...");

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize SPI bus! Error: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "SPI bus initialized!");

        ESP_LOGI(TAG, "Initializing storage...");
        ret = storage::init();

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize storage! %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Storage initialized!");

        ESP_LOGI(TAG, "Waiting before LCD init...");
        vTaskDelay(pdMS_TO_TICKS(200));

        ESP_LOGI(TAG, "Creating LCD display...");
        lcd_display = std::make_shared<display::LCDDisplay>();
        ESP_LOGI(TAG, "Initializing LCD display...");

        ret = lcd_display->initialize();

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize lcd display! %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "LCD display initialized!");

        ESP_LOGI(TAG, "Creating joypad...");
        joypad = std::make_shared<controller::Joypad>();
        ESP_LOGI(TAG, "Joypad created!");

        ESP_LOGI(TAG, "Creating memory bus...");
        mmu = std::make_unique<memory::MemoryBus>(joypad);
        ESP_LOGI(TAG, "Memory bus created!");

        ESP_LOGI(TAG, "Creating PPU...");
        ppu = std::make_unique<ppu::PPU>(*mmu, lcd_display);

        ESP_LOGI(TAG, "Creating CPU...");
        cpu = std::make_unique<cpu::CPU>(*mmu, *ppu);

        ESP_LOGI(TAG, "Starting async render task...");
        BaseType_t result = xTaskCreatePinnedToCore(
            ppu::PPU::render_task,
            "render_task",
            8192, // Stack pour conversion + DMA
            ppu.get(),
            6, // Priorité haute > émulation
            nullptr,
            0 // Core 0 : rendu dédié
        );

        if (result == pdPASS)
        {
            ESP_LOGI(TAG, "Render task started successfully on core 0");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create render task!");
            return ESP_ERR_NO_MEM;
        }

        return ESP_OK;
    }

    void Emulator::start()
    {
        ESP_LOGI(TAG, "Starting emulation...");
        ESP_LOGI(TAG, "Free heap before task creation: %lu bytes", esp_get_free_heap_size());

        BaseType_t result = xTaskCreatePinnedToCore(
            emulator_task,
            "emulator",
            8192, // 8KB stack
            this,
            5,
            nullptr,
            1
        );

        if (result == pdPASS)
        {
            ESP_LOGI(TAG, "Emulator task created successfully");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create emulator task! Result: %d", result);
            ESP_LOGE(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
        }
    }

    void Emulator::loadROM(const uint8_t *rom_buffer, size_t rom_size)
    {
        ESP_LOGI(TAG, "Loading ROM into MMU...");
        mmu->loadROM(rom_buffer, rom_size);
        ESP_LOGI(TAG, "ROM loaded into MMU!");
    }

    void Emulator::loadROMFile(const std::string &rom_path)
    {
        ESP_LOGI(TAG, "Loading ROM file: %s", rom_path.c_str());
        uint8_t *rom_buffer = nullptr;
        size_t rom_size = 0;
        esp_err_t ret = storage::read_binary_file(rom_path.c_str(), &rom_buffer, &rom_size);

        if (ret != ESP_OK || rom_buffer == nullptr)
        {
            ESP_LOGE(TAG, "Failed to read ROM file: %s", rom_path.c_str());
            return;
        }

        ESP_LOGI(TAG, "Loaded ROM: %s (%zu bytes)", rom_path.c_str(), rom_size);

        // Load ROM into memory bus
        loadROM(rom_buffer, rom_size);

        // Free the ROM buffer after loading (MMU should have copied it)
        free(rom_buffer);

        ESP_LOGI(TAG, "ROM loaded successfully!");
    }

    void Emulator::run()
    {
        ESP_LOGI(TAG, "Starting emulator main loop...");

        // Start the emulation task (will handle menu and game states)
        start();

        ESP_LOGI(TAG, "Emulation task created!");
    }
}
