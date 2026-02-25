#include "apu.hpp"

namespace apu
{
    uint8_t APU::read(uint16_t address) const
    {
        // Open-bus masks per Pan Docs — unused bits read as 1
        switch (address) {
            case 0xFF10: return registers[0x00] | 0x80;  // NR10: bit 7 unused
            case 0xFF11: return registers[0x01] | 0x3F;  // NR11: lower 6 bits write-only
            case 0xFF12: return registers[0x02];          // NR12: fully readable
            case 0xFF13: return 0xFF;                     // NR13: write-only
            case 0xFF14: return registers[0x04] | 0xBF;  // NR14: only bit 6 readable
            case 0xFF15: return 0xFF;                     // unused
            case 0xFF16: return registers[0x06] | 0x3F;  // NR21: lower 6 bits write-only
            case 0xFF17: return registers[0x07];          // NR22
            case 0xFF18: return 0xFF;                     // NR23: write-only
            case 0xFF19: return registers[0x09] | 0xBF;  // NR24: only bit 6 readable
            case 0xFF1A: return registers[0x0A] | 0x7F;  // NR30: only bit 7 readable
            case 0xFF1B: return 0xFF;                     // NR31: write-only
            case 0xFF1C: return registers[0x0C] | 0x9F;  // NR32: only bits 6-5 readable
            case 0xFF1D: return 0xFF;                     // NR33: write-only
            case 0xFF1E: return registers[0x0E] | 0xBF;  // NR34: only bit 6 readable
            case 0xFF1F: return 0xFF;                     // unused
            case 0xFF20: return 0xFF;                     // NR41: write-only
            case 0xFF21: return registers[0x11];          // NR42
            case 0xFF22: return registers[0x12];          // NR43
            case 0xFF23: return registers[0x13] | 0xBF;  // NR44: only bit 6 readable
            case 0xFF24: return registers[0x14];          // NR50
            case 0xFF25: return registers[0x15];          // NR51
            case 0xFF26: return registers[0x16] | 0x70;  // NR52: bits 6-4 unused (always 1)
            default:
                if (address >= 0xFF30 && address <= 0xFF3F) {
                    // Wave RAM - always readable
                    return registers[address - 0xFF10];
                }
                return 0xFF;
        }
    }

    void APU::write(uint16_t address, uint8_t value)
    {
        if (address >= 0xFF10 && address <= 0xFF3F)
        {
            uint8_t offset = address - 0xFF10;
            registers[offset] = value;

            // NR52 (0xFF26) - Power control
            if (address == NR52_REGISTER)
            {
                // Bit 7: APU power (stub - always on)
                // If turning off, clear all registers (except NR52)
                if ((value & 0x80) == 0)
                {
                    for (int i = 0; i < 0x30; i++)
                    {
                        if (i != (NR52_REGISTER - 0xFF10))
                            registers[i] = 0;
                    }
                }
            }
        }
    }
}
