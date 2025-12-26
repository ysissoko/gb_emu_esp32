#pragma once

#include <cstdint>
#include <string>

namespace memory {
    class MemoryBus;
}

namespace serial
{
    // Serial registers
    constexpr uint16_t SB_REGISTER = 0xFF01;  // Serial Transfer Data
    constexpr uint16_t SC_REGISTER = 0xFF02;  // Serial Transfer Control

    // SC bits
    constexpr uint8_t SC_TRANSFER_START = 0x80;  // Bit 7: Transfer Start Flag
    constexpr uint8_t SC_CLOCK_SPEED = 0x02;     // Bit 1: Clock Speed (CGB only)
    constexpr uint8_t SC_CLOCK_SELECT = 0x01;    // Bit 0: Shift Clock (0=external, 1=internal)

    class Serial
    {
    public:
        Serial(memory::MemoryBus& mmu);
        ~Serial();

        // Register access
        uint8_t readSB() const { return sb_register; }
        uint8_t readSC() const { return sc_register; }

        void writeSB(uint8_t value);
        void writeSC(uint8_t value);

        // Get accumulated debug output
        const std::string& getDebugOutput() const { return debug_output; }
        void clearDebugOutput() { debug_output.clear(); }

    private:
        memory::MemoryBus& mmu;

        // Serial registers
        uint8_t sb_register;  // 0xFF01 - Serial Transfer Data
        uint8_t sc_register;  // 0xFF02 - Serial Transfer Control

        // Debug output accumulator
        std::string debug_output;

        // Internal transfer state
        uint8_t transfer_cycles;

        void performTransfer();
    };
}
