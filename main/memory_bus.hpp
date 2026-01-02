#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>
#include <string>
#include "esp_attr.h"
#include "esp_err.h"
#include "save_manager.hpp"

namespace controller {
    class Joypad;
}

namespace timer {
    class Timer;
}

namespace serial {
    class Serial;
}

namespace apu {
    class APU;
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
        MemoryBus(const std::shared_ptr<controller::Joypad>&);
        ~MemoryBus();

        // 8-bit read/write operations
        IRAM_ATTR uint8_t read(uint16_t address) const __attribute__((hot));
        IRAM_ATTR void write(uint16_t address, uint8_t value) __attribute__((hot));

        // 16-bit read/write operations (little-endian)
        IRAM_ATTR uint16_t read16(uint16_t address) const __attribute__((hot));
        IRAM_ATTR void write16(uint16_t address, uint16_t value) __attribute__((hot));

        // Load ROM into memory
        void loadROM(const uint8_t* data, size_t size);

        // Set ROM path for save management
        void setROMPath(const std::string& path) { rom_path = path; }

        // SRAM save/load
        esp_err_t loadSRAM();
        esp_err_t saveSRAM();
        void markSRAMDirty();  // Call when SRAM is written, triggers auto-save

        // RTC update (call periodically, e.g., every frame)
        void updateRTC();

        // request an interruption
        inline void request_interrupt(IRQFlag flag) const {
            if_register |= (1 << static_cast<uint8_t>(flag));
        }

        // Step timer for given number of cycles
        void stepTimer(uint8_t cycles);

        // Get serial debug output
        std::string getSerialDebugOutput() const;
        void clearSerialDebugOutput();

        inline const uint8_t* getVRAM() const { return vram.data(); }
        inline const uint8_t* getOAM()  const { return oam.data(); }

        inline uint8_t readVRAM(uint16_t addr) const
        {
            // addr must be 0x8000–0x9FFF
            return vram[addr - VRAM_START];
        }

        inline uint8_t readOAM(uint16_t addr) const
        {
            // addr must be 0xFE00–0xFE9F
            return oam[addr - OAM_START];
        }

    private:
        // ROM - 32KB (0x0000-0x7FFF) base + extended banks in PSRAM
        std::array<uint8_t, 0x8000> rom;

        // MBC (Memory Bank Controller) registers
        uint8_t mbc_type{0};        // 0=ROM only, 1=MBC1, 2=MBC2, 3=MBC3, 5=MBC5
        uint16_t rom_bank{1};       // Current ROM bank (1-511, bank 0 always at 0x0000-0x3FFF)
        bool ram_enabled{false};    // External RAM enabled
        uint8_t ram_bank{0};        // Current RAM bank (0-15)
        uint8_t mbc_mode{0};        // MBC1 mode register
        size_t rom_size{0};         // Total ROM size in bytes

        // Extended ROM storage for MBC (allocated in PSRAM)
        uint8_t* rom_extended{nullptr};  // Up to 2MB (128 banks × 16KB)

        // Video RAM - 8KB (0x8000-0x9FFF)
        std::array<uint8_t, 0x2000> vram;

        // External RAM - 32KB (4 banks × 8KB) for MBC1/3/5
        // MBC1/3: Up to 4 banks of 8KB (0x00-0x03)
        // Note: MBC2 uses separate 512x4bit RAM (see mbc2_ram below)
        std::array<uint8_t, 0x8000> external_ram;

        // MBC2 internal RAM - 512 nibbles (256 bytes, only lower 4 bits used)
        std::array<uint8_t, 0x200> mbc2_ram;

        // RTC (Real-Time Clock) for MBC3
        uint8_t rtc_seconds{0};
        uint8_t rtc_minutes{0};
        uint8_t rtc_hours{0};
        uint8_t rtc_days_low{0};
        uint8_t rtc_days_high{0};  // Bit 0: Day high, Bit 6: Halt, Bit 7: Day carry
        uint8_t rtc_latch{0xFF};   // Latch data (0x00 -> 0x01 latches RTC)
        bool rtc_latched{false};
        int64_t rtc_base_time{0};  // Base time in seconds since boot

        // SRAM save management
        bool has_battery{false};
        std::string rom_path{};
        uint32_t sram_dirty_counter{0};  // Auto-save every N frames

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
        std::shared_ptr<controller::Joypad> joypad{nullptr};

        // Timer for game timing
        std::unique_ptr<timer::Timer> timer{nullptr};

        // Serial port for debugging
        std::unique_ptr<serial::Serial> serial{nullptr};

        // APU for audio (stub - no sound output)
        std::unique_ptr<apu::APU> apu{nullptr};
    };
}
