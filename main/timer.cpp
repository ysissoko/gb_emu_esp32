#include "timer.hpp"
#include "memory_bus.hpp"

namespace timer
{
    Timer::Timer(memory::MemoryBus& mmu)
        : mmu(mmu)
    {
        // Start with everything cleared (power-up sequence often sets registers elsewhere in your init).
        // Keep timer_input consistent.
        timer_input = compute_timer_input();
    }

    uint8_t Timer::selected_bit() const
    {
        // Edge-based model: TIMA increments on falling edge of selected sys_counter bit.
        //
        // TAC clock select mapping (Pan Docs):
        // 00: 4096 Hz  -> increment every 1024 CPU cycles -> use sys_counter bit 9 (period 1024, falling edge each 1024)
        // 01: 262144 Hz-> increment every 16 cycles       -> use bit 3
        // 10: 65536 Hz -> increment every 64 cycles       -> use bit 5
        // 11: 16384 Hz -> increment every 256 cycles      -> use bit 7
        //
        // (These correspond to the “system counter” wiring / falling edge detector.) :contentReference[oaicite:5]{index=5}
        switch (clock_select())
        {
            case 0: return 9;
            case 1: return 3;
            case 2: return 5;
            case 3: return 7;
            default: return 9;
        }
    }

    bool Timer::compute_timer_input() const
    {
        if (!is_enabled()) return false;
        const uint8_t b = selected_bit();
        return ((sys_counter >> b) & 0x1) != 0;
    }

    uint8_t Timer::readDIV() const
    {
        // DIV is the upper 8 bits of the system counter. :contentReference[oaicite:6]{index=6}
        return static_cast<uint8_t>(sys_counter >> 8);
    }

    void Timer::apply_input_transition(bool old_in, bool new_in)
    {
        // If the timer input goes 1 -> 0, hardware generates a TIMA tick (glitch).
        // This happens on DIV reset, and on certain TAC changes. :contentReference[oaicite:7]{index=7}
        if (old_in && !new_in)
        {
            tima_tick();
        }
    }

    void Timer::tima_tick()
    {
        // If an overflow is already armed, further ticks should still behave like hardware:
        // In practice, timer tick during the overflow window is tricky; for DMG accuracy,
        // it’s safest to ignore additional ticks while reload is pending (common emulator strategy).
        // This keeps timing ROMs happy in most cases.
        if (overflow_armed)
            return;

        if (tima == 0xFF)
        {
            // Overflow from increment:
            // TIMA becomes 00, then after 1 M-cycle (4 cycles CPU), TIMA reloads from TMA
            // and IF.TIMER is set. :contentReference[oaicite:8]{index=8}
            tima = 0x00;
            overflow_delay = 4;
            overflow_armed = true;
            tma_latch = tma; // latch value (helps with edge cases)
            tima_written_during_overflow = false;
        }
        else
        {
            tima++;
        }
    }

    void Timer::advance_overflow_pipeline(uint16_t delta)
    {
        if (!overflow_armed)
            return;

        if (delta >= overflow_delay)
        {
            // End of the delay window: reload + request interrupt,
            // unless a TIMA write during the “A cycle” canceled it. :contentReference[oaicite:9]{index=9}
            overflow_delay = 0;

            if (!tima_written_during_overflow)
            {
                // Reload TIMA from TMA and request IRQ_TIMER
                // (Using latched TMA is a reasonable approximation; exact “same M-cycle as TMA write”
                // nuance is documented in Pan Docs.) :contentReference[oaicite:10]{index=10}
                tima = tma_latch;
                mmu.request_interrupt(memory::IRQFlag::IRQ_TIMER);
            }

            overflow_armed = false;
        }
        else
        {
            overflow_delay -= static_cast<uint8_t>(delta);
        }
    }

    uint16_t Timer::cycles_to_next_falling_edge() const
    {
        if (!is_enabled())
            return 0xFFFF;

        const uint8_t b = selected_bit();
        const uint16_t period = static_cast<uint16_t>(1u << (b + 1)); // full period of that bit
        const uint16_t mask   = period - 1;
        const uint16_t mod    = sys_counter & mask;

        // Falling edge happens when sys_counter mod period == period-1,
        // i.e., at the increment that wraps that lower (b+1)-bit range.
        // Cycles until that increment:
        const uint16_t to_edge = static_cast<uint16_t>(period - mod);
        return (to_edge == 0) ? period : to_edge;
    }

    void Timer::advance_cycles(uint16_t delta)
    {
        // We must advance:
        // 1) overflow pipeline countdown
        // 2) sys_counter
        // 3) detect timer input falling edge (normal ticking is driven by sys_counter progression)
        //
        // We do it in an edge-driven way: jump to next falling edge rather than per-cycle loop.

        while (delta > 0)
        {
            // If timer disabled, no need to search for edges—just advance counter and pipeline.
            if (!is_enabled())
            {
                advance_overflow_pipeline(delta);
                sys_counter = static_cast<uint16_t>(sys_counter + delta);
                timer_input = compute_timer_input(); // will be false anyway
                return;
            }

            const uint16_t to_edge = cycles_to_next_falling_edge();
            const uint16_t step    = (to_edge < delta) ? to_edge : delta;

            // Advance overflow pipeline first for this chunk
            advance_overflow_pipeline(step);

            // Advance sys counter
            // We also need to detect if timer input changed in a way that creates a tick.
            // For “normal ticking”, the tick occurs exactly on the falling edge increments;
            // since we jump to the next falling edge, we can emit one tick when step == to_edge.
            sys_counter = static_cast<uint16_t>(sys_counter + step);

            // Update and handle edge if we landed exactly on an edge boundary.
            // At the point right after sys_counter advanced by "to_edge", the selected bit fell.
            if (step == to_edge)
            {
                // In the real circuit, the falling edge is on the timer input (enabled AND selected bit).
                // Since enabled is true in this block, it’s effectively the bit’s falling edge. :contentReference[oaicite:11]{index=11}
                tima_tick();
            }

            // Refresh timer_input for possible DIV/TAC glitch consistency (not strictly needed here)
            timer_input = compute_timer_input();

            delta = static_cast<uint16_t>(delta - step);
        }
    }

    void Timer::step(uint16_t cycles)
    {
        // In your emulator you pass CPU cycles per instruction. This function must be fast.
        advance_cycles(cycles);
    }

    void Timer::writeDIV(uint8_t value)
    {
        (void)value;

        // Writing any value resets DIV (system counter). :contentReference[oaicite:12]{index=12}
        const bool old_in = timer_input;

        sys_counter = 0;

        const bool new_in = compute_timer_input();
        apply_input_transition(old_in, new_in);

        timer_input = new_in;
    }

    void Timer::writeTIMA(uint8_t value)
    {
        // Writing to TIMA during the overflow window can cancel the reload/IF set
        // (Cycle A behavior). :contentReference[oaicite:13]{index=13}
        tima = value;

        if (overflow_armed && overflow_delay > 0)
        {
            // Cancel the pending reload/IRQ
            tima_written_during_overflow = true;
        }
    }

    void Timer::writeTMA(uint8_t value)
    {
        // Normal write
        tma = value;

        // For accuracy: if overflow is armed, allow reload to use the latest TMA value
        // (the “same M-cycle as transfer uses old value” nuance exists; this latch approach is usually good enough). :contentReference[oaicite:14]{index=14}
        if (overflow_armed)
        {
            tma_latch = tma;
        }
    }

    void Timer::writeTAC(uint8_t value)
    {
        // Only bits 0..2 are writable :contentReference[oaicite:15]{index=15}
        const bool old_in = timer_input;

        tac = (value & 0x07);

        const bool new_in = compute_timer_input();
        apply_input_transition(old_in, new_in);

        timer_input = new_in;
    }
}
