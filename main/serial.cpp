#include "serial.hpp"
#include "memory_bus.hpp"
#include <cstdio>

namespace serial
{
    Serial::Serial(memory::MemoryBus& mmu)
        : mmu(mmu), sb_register(0x00), sc_register(0x00),
          debug_output(""), transfer_cycles(0)
    {
    }

    Serial::~Serial()
    {
    }

    void Serial::writeSB(uint8_t value)
    {
        sb_register = value;
    }

    void Serial::writeSC(uint8_t value)
    {
        sc_register = value;

        // Check if transfer is requested (bit 7 set and internal clock selected)
        if ((value & SC_TRANSFER_START) && (value & SC_CLOCK_SELECT))
        {
            performTransfer();
        }
    }

    void Serial::performTransfer()
    {
        // For emulator debugging: output the byte as a character
        char c = static_cast<char>(sb_register);

        // Accumulate for later retrieval
        debug_output += c;

        // Transfer complete: clear bit 7 and request interrupt
        sc_register &= ~SC_TRANSFER_START;
        mmu.request_interrupt(memory::IRQFlag::IRQ_SERIAL);

        // Note: In real hardware, the transfer takes 8192 cycles (512 µs at 4.19 MHz)
        // But for debugging purposes, we complete it immediately
    }
}
