#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>
#include <string>
#include "esp_attr.h"
#include "esp_err.h"
#include "save_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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

namespace ppu {
    class PPU;
}

namespace cpu {
    class CPU;
}

namespace memory
{
    // DMG Boot ROM (256 bytes)
    static constexpr uint8_t BOOT_ROM[0x100] = {
        0x31, 0xFE, 0xFF, 0xAF, 0x21, 0xFF, 0x9F, 0x32, 0xCB, 0x7C, 0x20, 0xFA, 0x21, 0x26, 0xFF, 0x0E,
        0x11, 0x3E, 0x80, 0x32, 0xE2, 0x0C, 0x3E, 0xF3, 0xE2, 0x32, 0xE2, 0x3E, 0x77, 0x77, 0x3E, 0xFC,
        0xE0, 0x47, 0x11, 0x04, 0x01, 0x21, 0x10, 0x80, 0x1A, 0xCD, 0x95, 0x00, 0xCD, 0x96, 0x00, 0x13,
        0x7B, 0xFE, 0x34, 0x20, 0xF3, 0x11, 0xD8, 0x00, 0x06, 0x08, 0x1A, 0x13, 0x22, 0x23, 0x05, 0x20,
        0xF9, 0x3E, 0x19, 0xEA, 0x10, 0x99, 0x21, 0x2F, 0x99, 0x0E, 0x0C, 0x3D, 0x28, 0x08, 0x32, 0x0D,
        0x20, 0xF9, 0x2E, 0x0F, 0x18, 0xF3, 0x67, 0x3E, 0x64, 0x57, 0xE0, 0x42, 0x3E, 0x91, 0xE0, 0x40,
        0x04, 0x1E, 0x02, 0x0E, 0x0C, 0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0x0D, 0x20, 0xF7, 0x1D, 0x20,
        0xF2, 0x0E, 0x13, 0x24, 0x7C, 0x1E, 0x83, 0xFE, 0x62, 0x28, 0x06, 0x1E, 0xC1, 0xFE, 0x64, 0x20,
        0x06, 0x7B, 0xE2, 0x0C, 0x3E, 0x87, 0xE2, 0xF0, 0x42, 0x90, 0x42, 0x15, 0x20, 0xD2, 0x05, 0x20,
        0x4F, 0x16, 0x20, 0x18, 0xCB, 0x4F, 0x06, 0x04, 0xC5, 0xCB, 0x11, 0x17, 0xC1, 0xCB, 0x11, 0x17,
        0x05, 0x20, 0xF5, 0x22, 0x23, 0x22, 0x23, 0xC9, 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
        0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
        0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
        0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E, 0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5, 0x42, 0x3C,
        0x21, 0x04, 0x01, 0x11, 0xA8, 0x00, 0x1A, 0x13, 0xBE, 0x20, 0xFE, 0x23, 0x7D, 0xFE, 0x34, 0x20,
        0xF5, 0x06, 0x19, 0x78, 0x86, 0x23, 0x05, 0x20, 0xFB, 0x86, 0x20, 0xFE, 0x3E, 0x01, 0xE0, 0x50
    };

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

        // Set CPU reference for DMA control (called after CPU construction)
        void setCPU(cpu::CPU* cpu_ptr) { cpu = cpu_ptr; }

        // Set PPU reference for mode checks (called after PPU construction)
        void setPPU(ppu::PPU* ppu_ptr) { ppu = ppu_ptr; }

        // Disable boot ROM (for test ROMs)
        void disableBootRom() { bootEnabled = false; }

        // Check if boot ROM is enabled
        bool isBootEnabled() const { return bootEnabled; }

        // Initialize I/O registers to post-bootrom DMG state (skip bootrom execution)
        void initializePostBootROMState();

        // SRAM save/load
        esp_err_t loadSRAM();
        esp_err_t saveSRAM();
        void markSRAMDirty();  // Call when SRAM is written, triggers auto-save
        bool isSRAMDirty() const { return sram_dirty_counter > 0; }  // Check if SRAM needs saving

        // Async save system (safe for SPI operations)
        void initSaveTask();         // Initialize save task on Core 0
        void requestSave();          // Request async save (thread-safe)
        void shutdownSaveTask();     // Cleanup save task

        // RTC update (call periodically, e.g., every frame)
        void updateRTC();

        // request an interruption
        inline void request_interrupt(IRQFlag flag) const {
            if_register |= (1 << static_cast<uint8_t>(flag));
        }

        // Step timer for given number of cycles
        void stepTimer(uint8_t cycles);

        // Reset DIV counter (e.g. on STOP / speed switch)
        void resetDIV();

        // Called by PPU when entering HBlank — copies next 16-byte HDMA block if active
        void hdmaHBlankStep();
        bool isHDMAActive() const { return hdma_active; }

        // Direct STAT register write for PPU internal use only.
        // Bypasses the CPU-facing write protection on bits 0-2 (mode + LYC=LY flag).
        inline void writePPUStat(uint8_t stat) { io_registers[0x41] = stat; }

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

        bool isCGBMode() const { return cgb_mode; }

        inline const uint8_t* getVRAMBank1() const { return vram_bank1; }
        inline const uint8_t* getBGPaletteRAM() const { return bg_palette_ram; }
        inline const uint8_t* getOBJPaletteRAM() const { return obj_palette_ram; }
        inline const uint16_t* getBGPalCache() const { return &bg_pal_cache[0][0]; }
        inline const uint16_t* getOBJPalCache() const { return &obj_pal_cache[0][0]; }
        inline uint8_t getVRAMBankIndex() const { return vram_bank; }

    private:
        // ROM - 32KB (0x0000-0x7FFF) base + extended banks in PSRAM
        std::array<uint8_t, 0x8000> rom;

        // MBC (Memory Bank Controller) registers
        uint8_t mbc_type{0};        // 0=ROM only, 1=MBC1, 2=MBC2, 3=MBC3, 5=MBC5
        uint16_t rom_bank{1};       // Current ROM bank (1-511, bank 0 always at 0x0000-0x3FFF)
        uint16_t rom_bank_mask{0x1FF}; // Mask for valid ROM banks (default 512 banks = 9 bits)
        bool ram_enabled{false};    // External RAM enabled
        uint8_t ram_bank{0};        // Current RAM bank (0-15)
        uint8_t mbc_mode{0};        // MBC1 mode register
        size_t rom_size{0};         // Total ROM size in bytes
        ppu::PPU* ppu = nullptr;    // PPU reference for mode checks
        bool bootEnabled = false;   // Boot ROM disabled by default (skip animation, faster startup)

        // Extended ROM storage for MBC (allocated in PSRAM)
        uint8_t* rom_extended{nullptr};  // Up to 2MB (128 banks × 16KB)

        // Video RAM - 8KB (0x8000-0x9FFF)
        std::array<uint8_t, 0x2000> vram;

        // External RAM - 32KB (4 banks × 8KB) for MBC1/3/5 (allocated in PSRAM)
        // MBC1/3: Up to 4 banks of 8KB (0x00-0x03)
        // Note: MBC2 uses separate 512x4bit RAM (see mbc2_ram below)
        uint8_t* external_ram{nullptr};  // Allocated in PSRAM to save SRAM
        static constexpr size_t EXTERNAL_RAM_SIZE = 0x8000;  // 32KB

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

        // Async save task (Core 0)
        static void save_task(void* arg);
        TaskHandle_t save_task_handle{nullptr};
        QueueHandle_t save_queue{nullptr};
        volatile bool save_task_running{false};

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

        // CPU reference for DMA control (set after construction)
        cpu::CPU* cpu{nullptr};

        // Timer for game timing
        std::unique_ptr<timer::Timer> timer{nullptr};

        // Serial port for debugging
        std::unique_ptr<serial::Serial> serial{nullptr};

        // APU for audio (stub - no sound output)
        std::unique_ptr<apu::APU> apu{nullptr};

        // CGB mode flag
        bool cgb_mode{false};

        // CGB VRAM bank 1 (8KB, PSRAM)
        uint8_t* vram_bank1{nullptr};
        uint8_t vram_bank{0};   // Current VRAM bank (0 or 1), VBK register

        // CGB WRAM extra banks 2-7 (6 × 4KB = 24KB, PSRAM)
        uint8_t* wram_extra{nullptr};
        uint8_t wram_bank{1};   // Active WRAM bank for 0xD000-0xDFFF (1-7)

        // CGB color palette RAM
        uint8_t bg_palette_ram[64]{};   // 8 palettes × 4 colors × 2 bytes
        uint8_t obj_palette_ram[64]{};  // 8 palettes × 4 colors × 2 bytes
        uint8_t bg_palette_index{0};    // BGPI register (bit 7 = auto-increment)
        uint8_t obj_palette_index{0};   // OBPI register (bit 7 = auto-increment)
        // Pre-converted RGB565 palette cache (updated on every BGPD/OBPD write)
        uint16_t bg_pal_cache[8][4]{};
        uint16_t obj_pal_cache[8][4]{};

        // CGB HDMA (HBlank DMA) state
        bool hdma_active{false};      // HBlank DMA in progress
        uint16_t hdma_src{0};         // Current source address
        uint16_t hdma_dst{0};         // Current destination (always 0x8000-0x9FFF)
        uint8_t hdma_remaining{0};    // Remaining 16-byte blocks minus 1 (0x7F = done)
    };
}
