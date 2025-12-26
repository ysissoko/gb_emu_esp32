#pragma once

#include "joypad.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>

namespace timer {
    class Timer;
}

namespace memory
{
    // Game Boy memory map
    constexpr uint16_t ROM_BANK_0_START = 0x0000;
    constexpr uint16_t ROM_BANK_0_END = 0x3FFF;
    constexpr uint16_t ROM_BANK_N_START = 0x4000;
    constexpr uint16_t ROM_BANK_N_END = 0x7FFF;
    constexpr uint16_t VRAM_START = 0x8000;
    constexpr uint16_t VRAM_END = 0x9FFF;
    constexpr uint16_t EXTERNAL_RAM_START = 0xA000;
    constexpr uint16_t EXTERNAL_RAM_END = 0xBFFF;
    constexpr uint16_t WRAM_START = 0xC000;
    constexpr uint16_t WRAM_END = 0xDFFF;
    constexpr uint16_t ECHO_RAM_START = 0xE000;
    constexpr uint16_t ECHO_RAM_END = 0xFDFF;
    constexpr uint16_t OAM_START = 0xFE00;
    constexpr uint16_t OAM_END = 0xFE9F;
    constexpr uint16_t UNUSABLE_START = 0xFEA0;
    constexpr uint16_t UNUSABLE_END = 0xFEFF;
    constexpr uint16_t IO_REGISTERS_START = 0xFF00;
    constexpr uint16_t IF_REGISTER = 0xFF0F;
    constexpr uint16_t IO_REGISTERS_END = 0xFF7F;
    constexpr uint16_t HRAM_START = 0xFF80;
    constexpr uint16_t HRAM_END = 0xFFFE;
    constexpr uint16_t IE_REGISTER = 0xFFFF;

    // Interrupt Request Flag

    enum class IRQFlag {
        IRQ_VBLANK,
        IRQ_LCD_STAT,
        IRQ_TIMER,
        IRQ_SERIAL,
        IRQ_JOYP
    };

    class MemoryBus
    {
    public:
        MemoryBus(std::unique_ptr<controller::Joypad>);
        ~MemoryBus();

        // 8-bit read/write operations
        uint8_t read(uint16_t address) const;
        void write(uint16_t address, uint8_t value);

        // 16-bit read/write operations (little-endian)
        uint16_t read16(uint16_t address) const;
        void write16(uint16_t address, uint16_t value);

        // Load ROM into memory
        void loadROM(const uint8_t* data, size_t size);

        // request an interruption
        inline void request_interrupt(IRQFlag flag) {
            if_register |= (1 << static_cast<uint8_t>(flag));
        }

        // Step timer for given number of cycles
        void stepTimer(uint8_t cycles);

    private:
        // ROM - 32KB (0x0000-0x7FFF)
        std::array<uint8_t, 0x8000> rom;

        // Video RAM - 8KB (0x8000-0x9FFF)
        std::array<uint8_t, 0x2000> vram;

        // External RAM - 8KB (0xA000-0xBFFF)
        std::array<uint8_t, 0x2000> external_ram;

        // Work RAM - 8KB (0xC000-0xDFFF)
        std::array<uint8_t, 0x2000> wram;

        // OAM (Object Attribute Memory) - 160 bytes (0xFE00-0xFE9F)
        std::array<uint8_t, 0xA0> oam;

        // I/O Registers - 128 bytes (0xFF00-0xFF7F)
        std::array<uint8_t, 0x80> io_registers;

        // High RAM - 127 bytes (0xFF80-0xFFFE)
        std::array<uint8_t, 0x7F> hram;

        // Interrupt Enable register - 1 byte (0xFFFF)
        uint8_t ie_register;

        // Interrupt Flag Register - 1 byte (0xFF0F)
        // Mutable because reading joypad can trigger interrupt (hardware side-effect)
        mutable uint8_t if_register;

        // Previous joypad state for detecting button transitions
        mutable uint8_t prev_joypad_state;

        // Physical controller button handling
        std::unique_ptr<controller::Joypad> joypad{nullptr};

        // Timer for game timing
        std::unique_ptr<timer::Timer> timer{nullptr};
    };
}
