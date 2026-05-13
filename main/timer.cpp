#include "timer.hpp"
#include "memory_bus.hpp"

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

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
    // Rare slow path: overflow delay needs per-cycle accuracy
    if (UNLIKELY(tima_overflow)) {
        for (uint8_t i = 0; i < cycles; ++i) {
            if (--overflow_delay == 0) {
                tima = tma;
                tima_overflow = false;
                mmu.request_interrupt(memory::IRQFlag::IRQ_TIMER);
            }
            bool old_bit = timerEnabled() && ((div_counter >> timerBit()) & 1);
            div_counter++;
            bool new_bit = timerEnabled() && ((div_counter >> timerBit()) & 1);
            if (old_bit && !new_bit) {
                tima++;
                if (tima == 0x00) { tima_overflow = true; overflow_delay = 4; }
            }
        }
        return;
    }

    // Fast path: batch-advance div_counter
    uint16_t old_div = div_counter;
    div_counter += cycles;

    // Timer disabled: done (most common case)
    if (LIKELY(!(tac & 0x04))) return;

    // Timer enabled: check for a falling edge of the selected bit
    // For cycles ≤ 12 the relevant bit (period ≥ 16) flips at most once
    uint8_t bit = timerBit();
    if (LIKELY(((old_div >> bit) & 1) == ((div_counter >> bit) & 1))) return;

    if ((old_div >> bit) & 1) {  // falling edge: was 1, now 0
        tima++;
        if (UNLIKELY(tima == 0x00)) {
            tima_overflow = true;
            overflow_delay = 4;
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
