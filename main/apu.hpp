#pragma once

#include <cstdint>

namespace apu
{
    // APU Registers
    constexpr uint16_t NR10_REGISTER = 0xFF10;  // Channel 1 Sweep
    constexpr uint16_t NR11_REGISTER = 0xFF11;  // Channel 1 Length/Wave
    constexpr uint16_t NR12_REGISTER = 0xFF12;  // Channel 1 Envelope
    constexpr uint16_t NR13_REGISTER = 0xFF13;  // Channel 1 Frequency Lo
    constexpr uint16_t NR14_REGISTER = 0xFF14;  // Channel 1 Frequency Hi

    constexpr uint16_t NR21_REGISTER = 0xFF16;  // Channel 2 Length/Wave
    constexpr uint16_t NR22_REGISTER = 0xFF17;  // Channel 2 Envelope
    constexpr uint16_t NR23_REGISTER = 0xFF18;  // Channel 2 Frequency Lo
    constexpr uint16_t NR24_REGISTER = 0xFF19;  // Channel 2 Frequency Hi

    constexpr uint16_t NR30_REGISTER = 0xFF1A;  // Channel 3 DAC Enable
    constexpr uint16_t NR31_REGISTER = 0xFF1B;  // Channel 3 Length
    constexpr uint16_t NR32_REGISTER = 0xFF1C;  // Channel 3 Output Level
    constexpr uint16_t NR33_REGISTER = 0xFF1D;  // Channel 3 Frequency Lo
    constexpr uint16_t NR34_REGISTER = 0xFF1E;  // Channel 3 Frequency Hi

    constexpr uint16_t NR41_REGISTER = 0xFF20;  // Channel 4 Length
    constexpr uint16_t NR42_REGISTER = 0xFF21;  // Channel 4 Envelope
    constexpr uint16_t NR43_REGISTER = 0xFF22;  // Channel 4 Frequency
    constexpr uint16_t NR44_REGISTER = 0xFF23;  // Channel 4 Control

    constexpr uint16_t NR50_REGISTER = 0xFF24;  // Master Volume
    constexpr uint16_t NR51_REGISTER = 0xFF25;  // Sound Panning
    constexpr uint16_t NR52_REGISTER = 0xFF26;  // Sound On/Off

    constexpr uint16_t WAVE_RAM_START = 0xFF30;
    constexpr uint16_t WAVE_RAM_END = 0xFF3F;

    // APU Stub - No actual audio output
    class APU
    {
    public:
        APU() = default;
        ~APU() = default;

        // Read APU register (stub - return reasonable defaults)
        uint8_t read(uint16_t address) const;

        // Write APU register (stub - just store value)
        void write(uint16_t address, uint8_t value);

        // Step APU (stub - does nothing)
        void step(uint8_t cycles) { (void)cycles; }

    private:
        // APU registers (0xFF10-0xFF3F)
        uint8_t registers[0x30]{0};
    };
}
