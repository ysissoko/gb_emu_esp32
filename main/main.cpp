#include <cstdio>
#include "esp_system.h"
#include "cpu.hpp"

extern "C" void app_main(void);

extern "C" void app_main(void)
{
    std::printf("Hello GB EMU ESP32!\n");
    std::printf("Chip revision: %s\n", esp_get_idf_version());

    cpu::CPU cpu;
    cpu.run();
}
