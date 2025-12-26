#include "timer.hpp"
#include "memory_bus.hpp"

namespace timer
{
    Timer::Timer(memory::MemoryBus& mmu)
        : mmu(mmu), div_register(0), tima_register(0), tma_register(0),
          tac_register(0), div_counter(0), tima_counter(0)
    {
    }

    Timer::~Timer()
    {
    }

    void Timer::step(uint8_t cycles)
    {
        // Update DIV register (always runs at 16384 Hz = CPU_CLOCK / 256)
        // CPU clock is ~4.194 MHz, so DIV increments every 256 cycles
        div_counter += cycles;
        while (div_counter >= 256)
        {
            div_counter -= 256;
            div_register++;
        }

        // Update TIMA if timer is enabled
        if (isTimerEnabled())
        {
            tima_counter += cycles;
            uint16_t frequency = getTimerFrequency();

            while (tima_counter >= frequency)
            {
                tima_counter -= frequency;

                // Increment TIMA
                if (tima_register == 0xFF)
                {
                    // Overflow: reset to TMA and request interrupt
                    tima_register = tma_register;
                    mmu.request_interrupt(memory::IRQFlag::IRQ_TIMER);
                }
                else
                {
                    tima_register++;
                }
            }
        }
    }

    uint16_t Timer::getTimerFrequency() const
    {
        // Return the number of CPU cycles per TIMA increment
        // Based on TAC bits 1-0
        switch (tac_register & TAC_CLOCK_SELECT)
        {
            case 0: return 1024; // 4096 Hz   (CPU_CLOCK / 1024)
            case 1: return 16;   // 262144 Hz (CPU_CLOCK / 16)
            case 2: return 64;   // 65536 Hz  (CPU_CLOCK / 64)
            case 3: return 256;  // 16384 Hz  (CPU_CLOCK / 256)
            default: return 1024;
        }
    }

    void Timer::writeDIV(uint8_t value)
    {
        // Writing any value to DIV resets it to 0
        (void)value; // Unused parameter
        div_register = 0;
        div_counter = 0;
    }

    void Timer::writeTAC(uint8_t value)
    {
        // Only bits 0-2 are writable
        tac_register = value & 0x07;

        // Reset TIMA counter when changing timer settings
        tima_counter = 0;
    }
}
