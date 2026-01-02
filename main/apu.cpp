#include "apu.hpp"

namespace apu
{
    uint8_t APU::read(uint16_t address) const
    {
        if (address >= 0xFF10 && address <= 0xFF3F)
        {
            uint8_t offset = address - 0xFF10;

            // NR52 (0xFF26) - Always return 0xF0 (power on, all channels off)
            if (address == NR52_REGISTER)
            {
                return 0xF0;  // Bit 7: power on, bits 0-3: channel status
            }

            return registers[offset];
        }

        return 0xFF;
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
