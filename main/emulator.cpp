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

        // Run menu loop until ROM selected
        while (!menu.isRomSelected())
        {
            menu.update();
            menu.draw();

            vTaskDelay(pdMS_TO_TICKS(MENU_FRAME_MS));
        }

        // Clear the menu display before loading ROM
        ESP_LOGI(TAG, "Clearing menu display...");

        // Clear entire screen to black (240x320) for game display
        static uint16_t black_buffer[display::SCREEN_WIDTH * 40]; // 40 lines at a time
        for (int i = 0; i < display::SCREEN_WIDTH * 40; i++)
        {
            black_buffer[i] = 0x0000; // Black
        }

        // Fill screen in chunks
        for (int y = 0; y < display::SCREEN_HEIGHT; y += 40)
        {
            int chunk_height = (y + 40 > display::SCREEN_HEIGHT) ? (display::SCREEN_HEIGHT - y) : 40;
            lcd_display->renderFrameRGB565(black_buffer, display::SCREEN_WIDTH, chunk_height, 0, y);
        }

        ESP_LOGI(TAG, "Screen cleared to black");

        std::string selected_rom = menu.getSelectedRomPath();
        ESP_LOGI(TAG, "ROM selected: %s", selected_rom.c_str());
        ESP_LOGI(TAG, "Menu will be destroyed, freeing ~150KB");

        return selected_rom;
    }

    void Emulator::emulator_task(void *arg)
    {
        Emulator *emulator = static_cast<Emulator *>(arg);

        ESP_LOGI(TAG, "Emulator task started!");

        // Unsubscribe from watchdog for this task (MUST be done BEFORE menu)
        // Menu can take several seconds while waiting for user input
        esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
        ESP_LOGI(TAG, "Task watchdog disabled for emulator task");

        // Show menu and load ROM (only once at startup)
        if (!emulator->rom_loaded)
        {
            // Show menu and let user select ROM
            std::string rom_path = emulator->showMenuAndSelectROM();

            if (!rom_path.empty())
            {
                ESP_LOGI(TAG, "Loading ROM: %s", rom_path.c_str());
                emulator->loadROMFile(rom_path);
                emulator->rom_loaded = true;
                emulator->ppu->setShouldRender(true); // Enable rendering now that game is loaded
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
        int rendered_frames = 0;
        int skipped_frames = 0;

        // Emulation FPS counter (Core 1 side — independent of LCD render)
        int64_t emu_fps_last_time = esp_timer_get_time();
        int emu_fps_count = 0;
        int64_t emu_frame_time_sum = 0;  // total emulation time (µs) over last second

        // Manual save: SELECT+START held for 2 seconds (no autosave — avoids SPI contention)
        int select_start_hold_frames = 0;
        constexpr int SAVE_HOLD_FRAMES = 120; // ~2 seconds at 60 FPS
        bool save_triggered = false;

        while (true)
        {
            int64_t frame_start = esp_timer_get_time();

            // Frame skipping disabled: if emulator runs slow, lag accumulates past FRAME_US/2
            // and all frames get skipped permanently. Re-enable only when emulator is fast enough.
            bool should_skip = false;

            // Set PPU rendering flag based on skip decision (affects render only!)
            emulator->ppu->setShouldRender(!should_skip);

            // Always run Game Boy frame (CPU + PPU + APU)
            emulator->cpu->run_frame();

            // Update RTC (Real-Time Clock for MBC3)
            emulator->mmu->updateRTC();

            // Manual save: SELECT+START held for ~2 seconds
            bool select_pressed = emulator->joypad->buttonSelectPressed();
            bool start_pressed  = emulator->joypad->buttonStartPressed();

            if (select_pressed && start_pressed) {
                select_start_hold_frames++;
                if (select_start_hold_frames == SAVE_HOLD_FRAMES && !save_triggered) {
                    ESP_LOGI(TAG, "Saving SRAM...");
                    emulator->mmu->saveSRAM();
                    save_triggered = true;
                }
            } else {
                select_start_hold_frames = 0;
                save_triggered = false;
            }

            int64_t frame_end = esp_timer_get_time();

            int64_t emu_time = frame_end - frame_start;

            // Update lag accumulator
            emulator->lag_us += emu_time - FRAME_US;

            // Clamp lag to prevent excessive accumulation
            emulator->lag_us = std::clamp(emulator->lag_us, -FRAME_US, 2 * FRAME_US);

            // Stats
            frame_count++;
            emu_fps_count++;
            emu_frame_time_sum += emu_time;
            if (!should_skip)
                rendered_frames++;
            else
                skipped_frames++;

            // Emulation FPS log every second (Core 1 — true emulation speed)
            int64_t now = esp_timer_get_time();
            if (now - emu_fps_last_time >= 1000000LL) {
                int64_t avg_frame_us = emu_fps_count ? (emu_frame_time_sum / emu_fps_count) : 0;
                ESP_LOGI(TAG, "Emu FPS: %d  avg frame: %lld us  (target: 16742 us)",
                         emu_fps_count, avg_frame_us);
                emu_fps_count = 0;
                emu_frame_time_sum = 0;
                emu_fps_last_time = now;
            }

            // Synchronization: sleep for remaining frame time
            int64_t elapsed = esp_timer_get_time() - frame_start;
            int64_t sleep_us = FRAME_US - elapsed;

            if (sleep_us > 1000)
                vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
            // No forced yield when lagging: watchdog is disabled for this task (esp_task_wdt_delete)
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

        // Connect PPU to MemoryBus for mode checks
        mmu->setPPU(ppu.get());
        ESP_LOGI(TAG, "PPU linked to MemoryBus for mode checks");

        // Link CGB resources to PPU (safe even if not CGB - pointers will be null-checked)
        ppu->setVRAMBank1(mmu->getVRAMBank1());  // TEST: revert to PSRAM to check regression
        ppu->setBGPaletteRAM(mmu->getBGPaletteRAM());
        ppu->setOBJPaletteRAM(mmu->getOBJPaletteRAM());
        ppu->setBGPalCache(mmu->getBGPalCache());
        ppu->setOBJPalCache(mmu->getOBJPalCache());

        ESP_LOGI(TAG, "Creating CPU...");
        cpu = std::make_unique<cpu::CPU>(*mmu, *ppu);

        // Connect CPU to MemoryBus for DMA control
        mmu->setCPU(cpu.get());
        ESP_LOGI(TAG, "CPU linked to MemoryBus for DMA control");

        ESP_LOGI(TAG, "Starting async render task...");
        BaseType_t result = xTaskCreatePinnedToCore(
            ppu::PPU::render_task,
            "render_task",
            8192, // Stack for DMA transfers
            ppu.get(),
            4, // Priority: lower than emulation (5) to prevent blocking
            nullptr,
            0 // Core 0: dedicated rendering
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
            1);

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

        // Get file size first to decide loading strategy
        long file_size_l = storage::get_file_size(rom_path.c_str());
        if (file_size_l <= 0) {
            ESP_LOGE(TAG, "Cannot get file size for: %s", rom_path.c_str());
            return;
        }
        size_t file_size = static_cast<size_t>(file_size_l);

        // Cap to 4MB (PSRAM limit for simultaneous ROM banks)
        if (file_size > 0x400000) {
            ESP_LOGW(TAG, "ROM too large: %zu bytes, truncating to 4MB", file_size);
            file_size = 0x400000;
        }

        // For large ROMs (> 512KB): stream directly into final destination to avoid
        // double PSRAM allocation (buffer + rom_extended would exceed available PSRAM).
        if (file_size > 0x80000) {
            ESP_LOGI(TAG, "Large ROM (%zu KB): using direct streaming load", file_size / 1024);
            mmu->loadROMFromFile(rom_path.c_str(), file_size);
            // Skip the buffer path — rom is already loaded
            goto rom_loaded;
        }

        {
        // Small ROM: use existing buffer path
        uint8_t *rom_buffer = nullptr;
        size_t rom_size = 0;
        esp_err_t ret = storage::read_binary_file(rom_path.c_str(), &rom_buffer, &rom_size);
        if (ret != ESP_OK || rom_buffer == nullptr) {
            ESP_LOGE(TAG, "Failed to read ROM file: %s", rom_path.c_str());
            return;
        }
        loadROM(rom_buffer, rom_size);
        free(rom_buffer);
        }

        rom_loaded:

        // Re-initialize CPU registers now that cgb_mode is known from ROM header
        cpu->initializePostBootROMState();
        ESP_LOGI(TAG, "CPU re-initialized for %s mode", mmu->isCGBMode() ? "CGB" : "DMG");

        // Set ROM path for save management
        mmu->setROMPath(rom_path);

        // Load SRAM from save file (if cartridge has battery)
        esp_err_t sram_ret = mmu->loadSRAM();
        if (sram_ret == ESP_OK)
            ESP_LOGI(TAG, "SRAM loaded from save file");

        // Save task disabled (SPI contention with LCD)
        // mmu->initSaveTask();

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
