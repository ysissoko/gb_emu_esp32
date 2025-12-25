#include "cpu.hpp"
#include "lcd_display.hpp"
#include "joypad.hpp"

#include "esp_system.h"

#include <cstdio>
#include <memory>

extern "C" void app_main(void);

extern "C" void app_main(void)
{
    std::printf("Hello GB EMU ESP32!\n");
    std::printf("Chip revision: %s\n", esp_get_idf_version());

    memory::MemoryBus mmu(std::make_unique<controller::Joypad>());
    ppu::PPU ppu(mmu, std::make_unique<ppu::display::LCDDisplay>());
    cpu::CPU cpu(mmu, ppu);
    cpu.run();
}
