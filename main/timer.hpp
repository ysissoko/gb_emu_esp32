#pragma once

#include <cstdint>
#include "esp_attr.h"

namespace memory {
    class MemoryBus;
}

namespace timer
{
    // Timer registers
    constexpr uint16_t DIV_REGISTER  = 0xFF04;  // Divider Register
    constexpr uint16_t TIMA_REGISTER = 0xFF05;  // Timer Counter
    constexpr uint16_t TMA_REGISTER  = 0xFF06;  // Timer Modulo
    constexpr uint16_t TAC_REGISTER  = 0xFF07;  // Timer Control

    // TAC bits
    constexpr uint8_t TAC_ENABLE       = 0x04; // Bit 2
    constexpr uint8_t TAC_CLOCK_SELECT = 0x03; // Bits 1-0

    class Timer
    {
    public:
        explicit Timer(memory::MemoryBus& mmu);
        ~Timer() = default;

        // Advance timer by CPU cycles (T-cycles). In your emulator, this is the same "cycles" you pass today.
        IRAM_ATTR void step(uint16_t cycles) __attribute__((hot));

        // Register reads
        IRAM_ATTR uint8_t readDIV()  const __attribute__((hot));
        IRAM_ATTR uint8_t readTIMA() const __attribute__((hot)) { return tima; }
        IRAM_ATTR uint8_t readTMA()  const __attribute__((hot)) { return tma; }
        IRAM_ATTR uint8_t readTAC()  const __attribute__((hot)) { return tac; }

        // Register writes (must implement hardware quirks)
        IRAM_ATTR void writeDIV(uint8_t value) __attribute__((hot));
        IRAM_ATTR void writeTIMA(uint8_t value) __attribute__((hot));
        IRAM_ATTR void writeTMA(uint8_t value) __attribute__((hot));
        IRAM_ATTR void writeTAC(uint8_t value) __attribute__((hot));

    private:
        memory::MemoryBus& mmu;

        // Visible registers
        uint8_t tima{0};
        uint8_t tma{0};
        uint8_t tac{0};

        // Internal system counter (DIV is upper 8 bits)
        uint16_t sys_counter{0}; // increments every CPU cycle in this model

        // Timer overflow pipeline (delay = 4 cycles)
        uint8_t  overflow_delay{0};   // counts down CPU cycles (4 -> 0)
        uint8_t  tma_latch{0};        // latch value used for reload
        bool     overflow_armed{false}; // true between overflow event and reload/IF set

        // Track TIMA writes during overflow window (behavioral quirk)
        bool     tima_written_during_overflow{false};

        // Current timer input state (for DIV/TAC glitch handling)
        bool     timer_input{false};

        IRAM_ATTR bool is_enabled() const __attribute__((hot)) { return (tac & TAC_ENABLE) != 0; }
        IRAM_ATTR uint8_t clock_select() const __attribute__((hot)) { return tac & TAC_CLOCK_SELECT; }

        // Selected bit of sys_counter used as timer source (per TAC)
        IRAM_ATTR uint8_t selected_bit() const __attribute__((hot));

        // Compute current timer input (enabled AND selected bit of sys_counter)
        IRAM_ATTR bool compute_timer_input() const __attribute__((hot));

        // Increment TIMA once (handles overflow scheduling)
        IRAM_ATTR void tima_tick() __attribute__((hot));

        // Apply the “glitch rule”: if timer_input goes 1 -> 0 due to DIV/TAC write, TIMA ticks once
        IRAM_ATTR void apply_input_transition(bool old_in, bool new_in) __attribute__((hot));

        // Advance sys_counter by delta cycles, with edge-driven ticking (optimized)
        IRAM_ATTR void advance_cycles(uint16_t delta) __attribute__((hot));

        // How many cycles until next falling edge of selected bit (if enabled)
        IRAM_ATTR uint16_t cycles_to_next_falling_edge() const __attribute__((hot));

        // Handle overflow pipeline countdown and reload/IF set at the correct time
        IRAM_ATTR void advance_overflow_pipeline(uint16_t delta) __attribute__((hot));
    };
}
