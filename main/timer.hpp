#pragma once

#include <cstdint>

namespace memory {
    class MemoryBus;
}

namespace timer
{
    // Timer registers
    constexpr uint16_t DIV_REGISTER = 0xFF04;  // Divider Register
    constexpr uint16_t TIMA_REGISTER = 0xFF05; // Timer Counter
    constexpr uint16_t TMA_REGISTER = 0xFF06;  // Timer Modulo
    constexpr uint16_t TAC_REGISTER = 0xFF07;  // Timer Control

    // TAC bits
    constexpr uint8_t TAC_ENABLE = 0x04;       // Bit 2: Timer Enable
    constexpr uint8_t TAC_CLOCK_SELECT = 0x03; // Bits 1-0: Clock Select

    class Timer
    {
    public:
        Timer(memory::MemoryBus& mmu);
        ~Timer();

        // Update timer state for given number of cycles
        void step(uint8_t cycles);

        // Register access
        uint8_t readDIV() const { return div_register; }
        uint8_t readTIMA() const { return tima_register; }
        uint8_t readTMA() const { return tma_register; }
        uint8_t readTAC() const { return tac_register; }

        void writeDIV(uint8_t value);  // Writing to DIV resets it to 0
        void writeTIMA(uint8_t value) { tima_register = value; }
        void writeTMA(uint8_t value) { tma_register = value; }
        void writeTAC(uint8_t value);

    private:
        memory::MemoryBus& mmu;

        // Timer registers
        uint8_t div_register;   // 0xFF04 - Divider (increments at 16384 Hz)
        uint8_t tima_register;  // 0xFF05 - Timer Counter
        uint8_t tma_register;   // 0xFF06 - Timer Modulo
        uint8_t tac_register;   // 0xFF07 - Timer Control

        // Internal counters
        uint16_t div_counter;   // Internal counter for DIV (16-bit)
        uint16_t tima_counter;  // Internal counter for TIMA

        // Helper functions
        uint16_t getTimerFrequency() const;
        bool isTimerEnabled() const { return (tac_register & TAC_ENABLE) != 0; }
    };
}
