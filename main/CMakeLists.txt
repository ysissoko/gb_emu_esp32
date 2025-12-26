idf_component_register(SRCS "main.cpp" "cpu.cpp" "memory_bus.cpp" "ppu.cpp" "lcd_display.cpp" "timer.cpp" "serial.cpp" "menu.cpp" "storage.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_timer driver fatfs sdmmc)
