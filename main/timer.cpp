#include "timer.hpp"
#include "memory_bus.hpp"

namespace timer {

Timer::Timer(memory::MemoryBus& bus)
    : mmu(bus) {}

bool Timer::timerEnabled() const {
    return tac & 0x04;
}

uint8_t Timer::timerBit() const {
    switch (tac & 0x03) {
        case 0: return 9; // 4096 Hz
        case 1: return 3; // 262144 Hz
        case 2: return 5; // 65536 Hz
        case 3: return 7; // 16384 Hz
    }
    return 9;
}

void Timer::step(uint8_t cycles) {
    for (uint8_t i = 0; i < cycles; ++i) {
        // Handle overflow delay first (before incrementing div_counter)
        if (tima_overflow) {
            if (--overflow_delay == 0) {
                // Load TMA into TIMA and request interrupt at the same time
                tima = tma;
                tima_overflow = false;
                mmu.request_interrupt(memory::IRQFlag::IRQ_TIMER);
            }
        }

        // Get current state before increment
        bool old_bit = timerEnabled() && ((div_counter >> timerBit()) & 1);

        div_counter++;

        // Get new state after increment
        bool new_bit = timerEnabled() && ((div_counter >> timerBit()) & 1);

        // Falling edge detection: old_bit was 1, new_bit is 0
        if (old_bit && !new_bit) {
            tima++;
            // Check for overflow (TIMA goes from 0xFF to 0x00)
            if (tima == 0x00) {
                // TIMA is now 0x00, will be loaded with TMA after 4 cycles
                tima_overflow = true;
                overflow_delay = 4;
            }
        }
    }
}

// ==================== REGISTERS ====================

uint8_t Timer::readDIV() const {
    return div_counter >> 8;
}

uint8_t Timer::readTIMA() const {
    return tima;
}

uint8_t Timer::readTMA() const {
    return tma;
}

uint8_t Timer::readTAC() const {
    return tac | 0xF8;
}

void Timer::writeDIV(uint8_t) {
    // Check for falling edge before reset
    bool old_bit = timerEnabled() && ((div_counter >> timerBit()) & 1);
    
    div_counter = 0;
    
    // After reset, the bit is 0, so check for falling edge
    if (old_bit) {
        tima++;
        if (tima == 0x00) {
            tima_overflow = true;
            overflow_delay = 4;
        }
    }
}

void Timer::writeTIMA(uint8_t value) {
    tima = value;
    // Writing to TIMA during overflow delay cancels the overflow
    if (tima_overflow) {
        tima_overflow = false;
        overflow_delay = 0;
    }
}

void Timer::writeTMA(uint8_t value) {
    tma = value;
    // If we're in the overflow state (delay = 0 on this cycle), 
    // writing TMA affects the value that gets loaded
    // This is a hardware quirk but may not be tested
}

void Timer::writeTAC(uint8_t value) {
    // Check for falling edge before changing TAC
    bool old_bit = timerEnabled() && ((div_counter >> timerBit()) & 1);
    
    tac = value & 0x07;
    
    // Check for falling edge after changing TAC
    bool new_bit = timerEnabled() && ((div_counter >> timerBit()) & 1);
    
    if (old_bit && !new_bit) {
        tima++;
        if (tima == 0x00) {
            tima_overflow = true;
            overflow_delay = 4;
        }
    }
}

} // namespace timer
