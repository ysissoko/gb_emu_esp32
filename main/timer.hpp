#pragma once

#include <cstdint>

namespace memory {
    class MemoryBus;
}

namespace timer {

// Game Boy timer I/O register addresses
static constexpr uint16_t DIV_REGISTER  = 0xFF04; // Divider Register
static constexpr uint16_t TIMA_REGISTER = 0xFF05; // Timer Counter
static constexpr uint16_t TMA_REGISTER  = 0xFF06; // Timer Modulo
static constexpr uint16_t TAC_REGISTER  = 0xFF07; // Timer Control

class Timer {
public:
    explicit Timer(memory::MemoryBus& bus);

    // Appelé par la CPU avec le nombre exact de cycles exécutés
    void step(uint8_t cycles);

    // Registres
    uint8_t readDIV() const;
    uint8_t readTIMA() const;
    uint8_t readTMA() const;
    uint8_t readTAC() const;

    void writeDIV(uint8_t);
    void writeTIMA(uint8_t);
    void writeTMA(uint8_t);
    void writeTAC(uint8_t);

private:
    memory::MemoryBus& mmu;

    // Compteur interne 16-bit (le vrai timer hardware)
    uint16_t div_counter = 0;

    uint8_t tima = 0;
    uint8_t tma  = 0;
    uint8_t tac  = 0;

    // Overflow delay (hardware exact)
    bool tima_overflow = false;
    uint8_t overflow_delay = 0;

    bool timerEnabled() const;
    uint8_t timerBit() const;
};

} // namespace timer
