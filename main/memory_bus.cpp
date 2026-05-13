#include "memory_bus.hpp"
#include "cpu.hpp"  // For DMA control
#include "joypad.hpp"
#include "timer.hpp"
#include "serial.hpp"
#include "apu.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstring>

// Branch prediction hints
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace memory
{
    // Convert CGB 15-bit color to RGB565 with GBC color correction (same as PPU).
    // GBC format: bits [4:0]=R, [9:5]=G, [14:10]=B (little-endian). MADCTL=0x00 RGB order.
    static inline uint16_t cgb_pal_to_rgb565(uint8_t lo, uint8_t hi)
    {
        uint16_t color = (static_cast<uint16_t>(hi) << 8) | lo;
        uint8_t r = (color >> 0) & 0x1F;
        uint8_t g = (color >> 5) & 0x1F;
        uint8_t b = (color >> 10) & 0x1F;
        // BGR565: B in bits 15:11, G in bits 10:6, R in bits 4:0 — panel is physically BGR-wired
        return static_cast<uint16_t>((b << 11) | (g << 6) | r);
    }
    MemoryBus::MemoryBus(const std::shared_ptr<controller::Joypad>& joypad)
        : rom{0}, vram{0}, wram{0},
          oam{0}, io_registers{0}, hram{0}, ie_register{0}, if_register{0},
          prev_joypad_state{0xFF}, joypad{joypad}
    {
        // rom_extended is allocated in loadROM() based on actual ROM size

        // Allocate external RAM in PSRAM (32KB for MBC1/3/5)
        external_ram = static_cast<uint8_t*>(
            heap_caps_malloc(EXTERNAL_RAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

        if (!external_ram)
        {
            ESP_LOGE("MemoryBus", "Failed to allocate external RAM in PSRAM!");
        }
        else
        {
            ESP_LOGI("MemoryBus", "Allocated 32KB external RAM in PSRAM");
            memset(external_ram, 0x00, EXTERNAL_RAM_SIZE);
        }

        // CGB VRAM bank 1 (8KB)
        vram_bank1 = static_cast<uint8_t*>(
            heap_caps_malloc(0x2000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (vram_bank1) {
            memset(vram_bank1, 0x00, 0x2000);
            ESP_LOGI("MemoryBus", "Allocated CGB VRAM bank 1 (8KB) in PSRAM");
        } else {
            ESP_LOGE("MemoryBus", "Failed to allocate CGB VRAM bank 1!");
        }

        // CGB WRAM extra banks 2-7 (6 × 4KB = 24KB)
        wram_extra = static_cast<uint8_t*>(
            heap_caps_malloc(6 * 0x1000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (wram_extra) {
            memset(wram_extra, 0x00, 6 * 0x1000);
            ESP_LOGI("MemoryBus", "Allocated CGB WRAM extra banks 2-7 (24KB) in PSRAM");
        } else {
            ESP_LOGE("MemoryBus", "Failed to allocate CGB WRAM extra banks!");
        }

        // Initialize I/O registers to DMG post-boot ROM state (boot ROM disabled)
        initializePostBootROMState();

        // Initialize timer (must be after member initialization)
        timer = std::make_unique<timer::Timer>(*this);
        // Initialize serial port
        serial = std::make_unique<serial::Serial>(*this);
        // Initialize APU (stub - no audio output)
        apu = std::make_unique<apu::APU>();
    }

    MemoryBus::~MemoryBus()
    {
        // Free PSRAM allocations
        if (rom_extended)
        {
            heap_caps_free(rom_extended);
            rom_extended = nullptr;
        }
        if (external_ram)
        {
            heap_caps_free(external_ram);
            external_ram = nullptr;
        }
        if (vram_bank1) { heap_caps_free(vram_bank1); vram_bank1 = nullptr; }
        if (wram_extra) { heap_caps_free(wram_extra); wram_extra = nullptr; }
    }

    void MemoryBus::initializePostBootROMState()
    {
        // Initialize I/O registers to their state after the DMG boot ROM has executed
        // This allows games to run correctly without executing the boot ROM
        // Values taken from Pan Docs (DMG post-boot state)

        ESP_LOGI("MemoryBus", "Initializing I/O registers to post-bootrom DMG state");

        // Serial transfer
        io_registers[0x01] = 0x00;  // SB - Serial transfer data
        io_registers[0x02] = 0x7E;  // SC - Serial transfer control

        // Timer
        io_registers[0x04] = 0xAB;  // DIV - Divider register (arbitrary start value)
        io_registers[0x05] = 0x00;  // TIMA - Timer counter
        io_registers[0x06] = 0x00;  // TMA - Timer modulo
        io_registers[0x07] = 0xF8;  // TAC - Timer control

        // Interrupt Flag - Clear all interrupt requests at startup
        // Upper 3 bits are unused and read as 1, but interrupt bits (0-4) must be 0
        if_register = 0xE0;  // 0b11100000 - no interrupts pending

        // Sound registers
        io_registers[0x10] = 0x80;  // NR10 - Channel 1 sweep
        io_registers[0x11] = 0xBF;  // NR11 - Channel 1 length/wave pattern
        io_registers[0x12] = 0xF3;  // NR12 - Channel 1 volume envelope
        io_registers[0x14] = 0xBF;  // NR14 - Channel 1 frequency hi
        io_registers[0x16] = 0x3F;  // NR21 - Channel 2 length/wave pattern
        io_registers[0x17] = 0x00;  // NR22 - Channel 2 volume envelope
        io_registers[0x19] = 0xBF;  // NR24 - Channel 2 frequency hi
        io_registers[0x1A] = 0x7F;  // NR30 - Channel 3 sound on/off
        io_registers[0x1B] = 0xFF;  // NR31 - Channel 3 sound length
        io_registers[0x1C] = 0x9F;  // NR32 - Channel 3 select output level
        io_registers[0x1E] = 0xBF;  // NR34 - Channel 3 frequency hi
        io_registers[0x20] = 0xFF;  // NR41 - Channel 4 sound length
        io_registers[0x21] = 0x00;  // NR42 - Channel 4 volume envelope
        io_registers[0x22] = 0x00;  // NR43 - Channel 4 polynomial counter
        io_registers[0x23] = 0xBF;  // NR44 - Channel 4 counter/consecutive
        io_registers[0x24] = 0x77;  // NR50 - Channel control / ON-OFF / Volume
        io_registers[0x25] = 0xF3;  // NR51 - Sound output terminal selection
        io_registers[0x26] = 0xF1;  // NR52 - Sound on/off

        // LCD Control registers
        io_registers[0x40] = 0x91;  // LCDC - LCD control
        io_registers[0x41] = 0x85;  // STAT - LCD status (mode will be updated by PPU)
        io_registers[0x42] = 0x00;  // SCY - Scroll Y
        io_registers[0x43] = 0x00;  // SCX - Scroll X
        io_registers[0x44] = 0x00;  // LY - LCD Y coordinate (updated by PPU)
        io_registers[0x45] = 0x00;  // LYC - LY compare
        io_registers[0x46] = 0xFF;  // DMA - DMA transfer
        io_registers[0x47] = 0xFC;  // BGP - BG palette data
        io_registers[0x48] = 0xFF;  // OBP0 - Object palette 0 data
        io_registers[0x49] = 0xFF;  // OBP1 - Object palette 1 data
        io_registers[0x4A] = 0x00;  // WY - Window Y position
        io_registers[0x4B] = 0x00;  // WX - Window X position

        // Interrupt Enable
        ie_register = 0x00;

        ESP_LOGI("MemoryBus", "I/O registers initialized to post-bootrom state");
    }

    void MemoryBus::updateROMBankCache() {
        uint16_t bank = rom_bank & rom_bank_mask;
        if (bank == 0 && mbc_type != 5) bank = 1;

        if (bank == 0) {
            memcpy(rom_bank_cache, rom.data(), 0x4000);
        } else if (bank == 1) {
            memcpy(rom_bank_cache, rom.data() + 0x4000, 0x4000);
        } else if (rom_extended) {
            const size_t offset = static_cast<size_t>(bank - 2) * 0x4000;
            if (offset < rom_extended_size)
                memcpy(rom_bank_cache, rom_extended + offset, 0x4000);
        }
    }

    void MemoryBus::updateWRAMBankCache() {
        if (!wram_extra || wram_bank < 2) return;
        const size_t offset = static_cast<size_t>(wram_bank - 2) * 0x1000;
        memcpy(wram_bank_cache, wram_extra + offset, 0x1000);
    }

    // readSlow: handles all addresses not covered by the inline fast paths in the header.
    uint8_t MemoryBus::readSlow(uint16_t address) const
    {
        // Restrict VRAM/OAM access during PPU Mode 3 (drawing)
        if (UNLIKELY((address >= 0x8000 && address < 0xA000) || (address >= 0xFE00 && address < 0xFEA0))) {
            if (ppu && ppu->getMode() == ::ppu::Mode::DRAWING)
                return 0xFF;
        }

        switch (address >> 12) {  // Get top 4 bits (0x0-0xF)
            case 0x0: case 0x1: case 0x2: case 0x3:
                // ROM Bank 0 (0x0000-0x3FFF)
                // NOTE: (optionnel) MBC1 mode 1 peut bank-switcher ici, mais non requis
                // pour corriger Wario Land; on garde ton comportement actuel.
                return rom[address];

            case 0x4: case 0x5: case 0x6: case 0x7:
                // ROM Bank N (0x4000-0x7FFF) — always served from DRAM cache
                return rom_bank_cache[address - 0x4000];

            case 0x8: case 0x9:
                // Video RAM (0x8000-0x9FFF)
                if (cgb_mode && vram_bank == 1 && vram_bank1) {
                    return vram_bank1[address - VRAM_START];
                }
                return vram[address - VRAM_START];

            case 0xA: case 0xB:
                // External RAM (0xA000-0xBFFF)
                if (!ram_enabled)
                    return 0xFF;

                if (mbc_type == 2) {
                    // MBC2: 512 nibbles (only lower 4 bits, upper 4 are 1s)
                    uint16_t offset = address & 0x01FF;  // Only 9 bits address
                    return mbc2_ram[offset] | 0xF0;  // Upper nibble always 1s
                }
                else if (mbc_type == 3 && ram_bank >= 0x08 && ram_bank <= 0x0C) {
                    // MBC3 RTC registers (0x08-0x0C)
                    // Some games (like Pokémon) read RTC without latching first
                    // Always update to current time before reading if not latched
                    if (!rtc_latched) {
                        const_cast<MemoryBus*>(this)->updateRTC();
                    }

                    // Return current RTC values (latched or live)
                    switch (ram_bank) {
                        case 0x08: return rtc_seconds;
                        case 0x09: return rtc_minutes;
                        case 0x0A: return rtc_hours;
                        case 0x0B: return rtc_days_low;
                        case 0x0C: return rtc_days_high;
                        default: return 0xFF;
                    }
                }
                else {
                    // Normal RAM (MBC1/3/5) with banking
                    uint8_t effective_ram_bank = ram_bank;

                    // MBC1 mode 0: RAM bank always 0
                    if (mbc_type == 1 && mbc_mode == 0) {
                        effective_ram_bank = 0;
                    }

                    size_t offset = (effective_ram_bank * 0x2000) + (address - EXTERNAL_RAM_START);

                    // Bounds check
                    if (LIKELY(external_ram && offset < EXTERNAL_RAM_SIZE)) {
                        return external_ram[offset];
                    }
                    return 0xFF;
                }

            case 0xC: case 0xD:
                // Work RAM (0xC000-0xDFFF)
                if (cgb_mode && address >= 0xD000) {
                    // CGB: 0xD000-0xDFFF = switchable bank (wram_bank 1-7)
                    if (wram_bank == 1) {
                        return wram[address - WRAM_START];  // bank 1 = wram[0x1000-0x1FFF]
                    } else {
                        return wram_bank_cache[address - 0xD000];  // banks 2-7: DRAM cache
                    }
                }
                return wram[address - WRAM_START];

            case 0xE:
                // Echo RAM (0xE000-0xEFFF) - mirrors WRAM
                return wram[address - ECHO_RAM_START];

            case 0xF:
                // High memory region (0xF000-0xFFFF)
                if (address < 0xFE00) {
                    // Echo RAM (0xF000-0xFDFF)
                    return wram[address - ECHO_RAM_START];
                }
                else if (address < 0xFEA0) {
                    // OAM (0xFE00-0xFE9F)
                    return oam[address - OAM_START];
                }
                else if (address < 0xFF00) {
                    // Unusable memory (0xFEA0-0xFEFF)
                    return 0xFF;
                }
                else if (address < 0xFF80) {
                    // I/O Registers (0xFF00-0xFF7F)
                    if (address == IO_REGISTERS_START) {
                        // Joypad register (0xFF00)
                        uint8_t current_state = joypad->read(io_registers[0]);
                        uint8_t current_pressed = (~current_state) & 0x0F;
                        uint8_t prev_pressed = (~prev_joypad_state) & 0x0F;
                        uint8_t new_presses = current_pressed & ~prev_pressed;
                        if (new_presses != 0) {
                            request_interrupt(IRQFlag::IRQ_JOYP);
                        }
                        prev_joypad_state = current_state;
                        return current_state;
                    }
                    else if (address == serial::SB_REGISTER) return serial->readSB();
                    else if (address == serial::SC_REGISTER) return serial->readSC();
                    else if (address == timer::DIV_REGISTER) return timer->readDIV();
                    else if (address == timer::TIMA_REGISTER) return timer->readTIMA();
                    else if (address == timer::TMA_REGISTER) return timer->readTMA();
                    else if (address == timer::TAC_REGISTER) return timer->readTAC();
                    else if (address == IF_REGISTER) return if_register | 0xE0;  // Bits 7-5 always 1
                    else if (address == 0xFF41) {
                        // STAT register: bit 7 is always 1 (unused)
                        return io_registers[0x41] | 0x80;
                    }
                    else if (address >= 0xFF10 && address <= 0xFF3F) {
                        // APU registers (0xFF10-0xFF3F)
                        return apu->read(address);
                    }
                    else if (address == 0xFF4F) {
                        // VBK: VRAM bank (CGB only), upper bits always 1
                        return cgb_mode ? (vram_bank | 0xFE) : 0xFF;
                    }
                    else if (address == 0xFF4D) {
                        // KEY1: speed switch register (CGB only)
                        // Bit 7 = current speed (0=normal, 1=double), bit 0 = switch armed
                        return cgb_mode ? io_registers[0x4D] : 0xFF;
                    }
                    else if (address == 0xFF68) {
                        // BGPI: BG palette index (CGB only)
                        return cgb_mode ? bg_palette_index : 0xFF;
                    }
                    else if (address == 0xFF69) {
                        // BGPD: BG palette data (CGB only)
                        if (!cgb_mode) return 0xFF;
                        uint8_t idx = bg_palette_index & 0x3F;
                        return bg_palette_ram[idx];
                    }
                    else if (address == 0xFF6A) {
                        // OBPI: OBJ palette index (CGB only)
                        return cgb_mode ? obj_palette_index : 0xFF;
                    }
                    else if (address == 0xFF6B) {
                        // OBPD: OBJ palette data (CGB only)
                        if (!cgb_mode) return 0xFF;
                        uint8_t idx = obj_palette_index & 0x3F;
                        return obj_palette_ram[idx];
                    }
                    else if (address == 0xFF70) {
                        // SVBK: WRAM bank (CGB only)
                        return cgb_mode ? (wram_bank | 0xF8) : 0xFF;
                    }
                    else if (address == 0xFF55) {
                        return cgb_mode ? io_registers[0x55] : 0xFF;
                    }
                    else return io_registers[address - IO_REGISTERS_START];
                }
                else if (address < 0xFFFF) {
                    // High RAM (0xFF80-0xFFFE)
                    return hram[address - HRAM_START];
                }
                else {
                    // Interrupt Enable Register (0xFFFF)
                    return ie_register;
                }

            default:
                return 0xFF;
        }
    }

    void MemoryBus::write(uint16_t address, uint8_t value)
    {
        // Disable boot ROM when writing to 0xFF50 (ignored since boot ROM is disabled)
        if (address == 0xFF50) {
            bootEnabled = false;
            // When boot ROM finishes, inform PPU of CGB mode
            if (ppu) ppu->setCGBMode(cgb_mode);
            return;
        }

        // Restrict VRAM/OAM access during PPU Mode 3 (drawing)
        if (ppu && ppu->getMode() == ::ppu::Mode::DRAWING &&
            ((address >= 0x8000 && address < 0xA000) || (address >= 0xFE00 && address < 0xFEA0))) {
            return;
        }

        // Optimized: Use high nibble for fast region lookup
        switch (address >> 12) {
            case 0x0: case 0x1:
                // 0x0000-0x1FFF: RAM Enable (MBC1/2/3/5)
                if (LIKELY(mbc_type != 0)) {
                    // MBC2: RAM enable only if bit 8 of address is 0
                    if (mbc_type == 2) {
                        if ((address & 0x0100) == 0) {
                            ram_enabled = ((value & 0x0F) == 0x0A);
                        }
                    }
                    else {
                        ram_enabled = ((value & 0x0F) == 0x0A);
                    }
                }
                return;

            case 0x2: case 0x3:
                // 0x2000-0x3FFF: ROM Bank Number (lower bits)
                if (mbc_type == 2) {
                    // MBC2: ROM bank only if bit 8 of address is 1
                    if (address & 0x0100) {
                        uint8_t bank = value & 0x0F;  // 4-bit bank number
                        if (bank == 0) bank = 1;
                        rom_bank = bank;
                        updateROMBankCache();
                    }
                }
                else if (mbc_type == 1) {
                    // MBC1: 5-bit bank number (0x01-0x1F), upper bits preserved
                    uint8_t bank = value & 0x1F;
                    if (UNLIKELY(bank == 0)) bank = 1;  // Bank 0 forbidden
                    rom_bank = (rom_bank & 0xE0) | bank;
                    rom_bank &= rom_bank_mask;  // Mask to valid banks
                    updateROMBankCache();
                } else if (mbc_type == 3) {
                    // MBC3: 7-bit bank number (0x01-0x7F), set directly
                    uint8_t bank = value & 0x7F;
                    if (UNLIKELY(bank == 0)) bank = 1;  // Bank 0 forbidden
                    rom_bank = bank;
                    rom_bank &= rom_bank_mask;  // Mask to valid banks
                    updateROMBankCache();
                } else if (mbc_type == 5) {
                    // MBC5: 0x2000-0x2FFF = lower 8 bits; 0x3000-0x3FFF = bit 8
                    if ((address >> 12) == 0x2) {
                        rom_bank = (rom_bank & 0x100) | value;
                    } else {
                        rom_bank = (rom_bank & 0xFF) | ((value & 0x01) << 8);
                    }
                    rom_bank &= rom_bank_mask;
                    updateROMBankCache();
                }
                return;

            case 0x4: case 0x5:
                // 0x4000-0x5FFF: RAM Bank Number or ROM Bank Number (upper bits)
                if (LIKELY(mbc_type == 1)) {
                    // MBC1: RAM bank or upper ROM bank bits (mode-dependent)
                    if (LIKELY(mbc_mode == 0)) {
                        // ROM banking mode: upper 2 bits of ROM bank
                        rom_bank = (rom_bank & 0x1F) | ((value & 0x03) << 5);
                        rom_bank &= rom_bank_mask;  // Mask to valid banks
                        updateROMBankCache();
                    } else {
                        // RAM banking mode
                        ram_bank = value & 0x03;
                    }
                } else if (mbc_type == 3) {
                    // MBC3: RAM bank (0x00-0x03) or RTC register (0x08-0x0C)
                    ram_bank = value & 0x0F;
                } else if (mbc_type == 5) {
                    // MBC5: RAM bank select (4 bits, 0x00-0x0F)
                    ram_bank = value & 0x0F;
                }
                return;

            case 0x6: case 0x7:
                // 0x6000-0x7FFF: Banking Mode Select (MBC1) or RTC Latch (MBC3)
                if (LIKELY(mbc_type == 1)) {
                    mbc_mode = value & 0x01;
                }
                else if (mbc_type == 3) {
                    // MBC3: RTC latch (write 0x00 then 0x01 to latch)
                    if (rtc_latch == 0x00 && value == 0x01) {
                        rtc_latched = true;
                        updateRTC();  // Update and latch current RTC values
                    }
                    else if (value == 0x00) {
                        rtc_latched = false;
                    }
                    rtc_latch = value;
                }
                return;

            case 0x8: case 0x9:
                // Video RAM (0x8000-0x9FFF)
                if (cgb_mode && vram_bank == 1 && vram_bank1) {
                    vram_bank1[address - VRAM_START] = value;
                    vram_bank1_cache[address - VRAM_START] = value;  // write-through to DRAM cache
                } else {
                    vram[address - VRAM_START] = value;
                }
                return;

            case 0xA: case 0xB:
                // External RAM (0xA000-0xBFFF)
                if (!ram_enabled)
                    return;

                if (mbc_type == 2) {
                    // MBC2: 512 nibbles (only lower 4 bits)
                    uint16_t offset = address & 0x01FF;
                    mbc2_ram[offset] = value & 0x0F;  // Only lower nibble
                    markSRAMDirty();
                }
                else if (mbc_type == 3 && ram_bank >= 0x08 && ram_bank <= 0x0C) {
                    // MBC3: Write to RTC registers
                    switch (ram_bank) {
                        case 0x08: rtc_seconds = value; break;
                        case 0x09: rtc_minutes = value; break;
                        case 0x0A: rtc_hours = value; break;
                        case 0x0B: rtc_days_low = value; break;
                        case 0x0C: rtc_days_high = value; break;
                    }
                }
                else {
                    // Normal RAM write (MBC1/3/5) with banking
                    uint8_t effective_ram_bank = ram_bank;

                    // MBC1 mode 0: RAM bank always 0
                    if (mbc_type == 1 && mbc_mode == 0) {
                        effective_ram_bank = 0;
                    }

                    size_t offset = (effective_ram_bank * 0x2000) + (address - EXTERNAL_RAM_START);

                    // Bounds check
                    if (LIKELY(external_ram && offset < EXTERNAL_RAM_SIZE)) {
                        external_ram[offset] = value;
                        markSRAMDirty();
                    }
                }
                return;

            case 0xC: case 0xD:
                // Work RAM (0xC000-0xDFFF)
                if (cgb_mode && address >= 0xD000) {
                    if (wram_bank == 1) {
                        wram[address - WRAM_START] = value;
                    } else if (wram_extra) {
                        size_t offset = (wram_bank - 2) * 0x1000 + (address - 0xD000);
                        wram_extra[offset] = value;
                        wram_bank_cache[address - 0xD000] = value;  // write-through to DRAM cache
                    }
                } else {
                    wram[address - WRAM_START] = value;
                }
                return;

            case 0xE:
                // Echo RAM (0xE000-0xEFFF) - mirrors WRAM
                wram[address - ECHO_RAM_START] = value;
                return;

            case 0xF:
                // High memory region (0xF000-0xFFFF)
                if (address < 0xFE00) {
                    // Echo RAM (0xF000-0xFDFF)
                    wram[address - ECHO_RAM_START] = value;
                }
                else if (address < 0xFEA0) {
                    // OAM (0xFE00-0xFE9F)
                    oam[address - OAM_START] = value;
                }
                else if (address < 0xFF00) {
                    // Unusable memory (0xFEA0-0xFEFF) - ignore writes
                    return;
                }
                else if (address < 0xFF80) {
                    // I/O Registers (0xFF00-0xFF7F)
                    if (address == IO_REGISTERS_START) {
                        // Joypad register (0xFF00)
                        io_registers[0] = (value & 0x30) | 0xC0;
                    }
                    else if (address == serial::SB_REGISTER) serial->writeSB(value);
                    else if (address == serial::SC_REGISTER) serial->writeSC(value);
                    else if (address == timer::DIV_REGISTER) timer->writeDIV(value);
                    else if (address == timer::TIMA_REGISTER) timer->writeTIMA(value);
                    else if (address == timer::TMA_REGISTER) timer->writeTMA(value);
                    else if (address == timer::TAC_REGISTER) timer->writeTAC(value);
                    else if (address == IF_REGISTER) if_register = value;
                    else if (address == 0xFF41) {
                        // STAT (0xFF41): bits 0-2 are read-only (PPU-controlled mode + LYC=LY flag)
                        // Only bits 3-6 (interrupt enable flags) are writable by the CPU
                        io_registers[0x41] = (io_registers[0x41] & 0x07) | (value & 0x78);
                    }
                    else if (address == 0xFF46) {
                        // DMA Transfer (0xFF46): Copy 160 bytes from XX00-XX9F to OAM (0xFE00-0xFE9F)
                        // DMA blocks CPU for 160 M-cycles
                        uint16_t source = value << 8;  // XX00
                        for (uint16_t i = 0; i < 0xA0; i++) {
                            oam[i] = read(source + i);
                        }
                        io_registers[0x46] = value;

                        // Signal CPU to block for DMA duration
                        if (cpu) {
                            cpu->startDMA();
                        }
                    }
                    else if (address >= 0xFF10 && address <= 0xFF3F) {
                        // APU registers (0xFF10-0xFF3F)
                        apu->write(address, value);
                    }
                    else if (address == 0xFF4D && cgb_mode) {
                        // KEY1: speed switch request (CGB only) - arm speed switch (bit 0)
                        io_registers[0x4D] = (io_registers[0x4D] & 0x80) | (value & 0x01);
                    }
                    else if (address == 0xFF4F && cgb_mode) {
                        // VBK: VRAM bank select (CGB only)
                        vram_bank = value & 0x01;
                        io_registers[0x4F] = vram_bank | 0xFE;
                    }
                    else if (address == 0xFF51 || address == 0xFF52 || address == 0xFF53 ||
                             address == 0xFF54) {
                        // HDMA source/dest (CGB only) - store for future HDMA implementation
                        io_registers[address - IO_REGISTERS_START] = value;
                    }
                    else if (address == 0xFF55 && cgb_mode) {
                        uint16_t src = ((io_registers[0x51] << 8) | (io_registers[0x52] & 0xF0));
                        uint16_t dst = (((io_registers[0x53] & 0x1F) << 8) | (io_registers[0x54] & 0xF0)) | 0x8000;
                        uint8_t blocks = value & 0x7F;  // Number of 16-byte blocks minus 1

                        if ((value & 0x80) == 0) {
                            // General Purpose DMA: terminate any active HBlank DMA, then copy all at once
                            if (hdma_active) {
                                hdma_active = false;
                                io_registers[0x55] = 0x80 | hdma_remaining;
                                return;
                            }
                            // Copy (blocks+1)*16 bytes immediately
                            uint16_t len = (static_cast<uint16_t>(blocks) + 1) * 16;
                            for (uint16_t i = 0; i < len; i++) {
                                write(dst + i, read(src + i));
                            }
                            io_registers[0x55] = 0xFF;  // DMA complete

                            // Stall CPU for General DMA duration: 8 M-cycles per 16-byte block = 32 T-cycles/block
                            if (cpu) {
                                uint16_t stall_cycles = (static_cast<uint16_t>(blocks) + 1) * 32;
                                cpu->startGeneralDMAStall(stall_cycles);
                            }
                        } else {
                            // HBlank DMA: arm state machine
                            hdma_active = true;
                            hdma_src = src;
                            hdma_dst = dst;
                            hdma_remaining = blocks;
                            io_registers[0x55] = blocks;  // Remaining blocks (bit 7 = 0 = active)
                        }
                    }
                    else if (address == 0xFF68 && cgb_mode) {
                        // BGPI: BG palette index
                        bg_palette_index = value;
                    }
                    else if (address == 0xFF69 && cgb_mode) {
                        // BGPD: BG palette data
                        uint8_t idx = bg_palette_index & 0x3F;
                        bg_palette_ram[idx] = value;
                        // Update RGB565 cache: each entry is 2 bytes (lo=even, hi=odd)
                        if ((idx & 1) == 1) {  // hi byte just written → entry is complete
                            uint8_t pal = (idx >> 1) >> 2;   // palette 0-7
                            uint8_t col = (idx >> 1) & 0x03; // color 0-3
                            bg_pal_cache[pal][col] = cgb_pal_to_rgb565(bg_palette_ram[idx - 1], value);
                        }
                        if (bg_palette_index & 0x80) {
                            bg_palette_index = (bg_palette_index & 0x80) | ((idx + 1) & 0x3F);
                        }
                    }
                    else if (address == 0xFF6A && cgb_mode) {
                        // OBPI: OBJ palette index
                        obj_palette_index = value;
                    }
                    else if (address == 0xFF6B && cgb_mode) {
                        // OBPD: OBJ palette data
                        uint8_t idx = obj_palette_index & 0x3F;
                        obj_palette_ram[idx] = value;
                        // Update RGB565 cache
                        if ((idx & 1) == 1) {
                            uint8_t pal = (idx >> 1) >> 2;
                            uint8_t col = (idx >> 1) & 0x03;
                            obj_pal_cache[pal][col] = cgb_pal_to_rgb565(obj_palette_ram[idx - 1], value);
                        }
                        if (obj_palette_index & 0x80) {
                            obj_palette_index = (obj_palette_index & 0x80) | ((idx + 1) & 0x3F);
                        }
                    }
                    else if (address == 0xFF70 && cgb_mode) {
                        // SVBK: WRAM bank select (CGB only), bits 2-0, value 0 treated as bank 1
                        io_registers[0x70] = value & 0x07;  // store raw written value
                        wram_bank = io_registers[0x70];
                        if (wram_bank == 0) wram_bank = 1;
                        updateWRAMBankCache();
                    }
                    else {
                        io_registers[address - IO_REGISTERS_START] = value;
                    }
                }
                else if (address < 0xFFFF) {
                    // High RAM (0xFF80-0xFFFE)
                    hram[address - HRAM_START] = value;
                }
                else {
                    // Interrupt Enable Register (0xFFFF)
                    ie_register = value;
                }
                return;

            default:
                return;
        }
    }

    uint16_t MemoryBus::read16(uint16_t address) const
    {
        // Little-endian: LSB first, then MSB
        uint8_t lsb = read(address);
        uint8_t msb = read(address + 1);
        return (static_cast<uint16_t>(msb) << 8) | lsb;
    }

    void MemoryBus::write16(uint16_t address, uint16_t value)
    {
        // Little-endian: write LSB first, then MSB
        uint8_t lsb = value & 0xFF;
        uint8_t msb = (value >> 8) & 0xFF;
        write(address, lsb);
        write(address + 1, msb);
    }

    void memory::MemoryBus::loadROM(const uint8_t* data, size_t size, bool skip_extended_copy)
    {
        rom_size = size;
        rom_extended_size = (size > 0x8000) ? (size - 0x8000) : 0;

        // Detect MBC type from ROM header (0x0147)
        if (size >= 0x150) {
            uint8_t cart_type = data[0x0147];
            uint8_t rom_size_code = data[0x0148];

            // Decode MBC type
            if (cart_type == 0x00) {
                mbc_type = 0;  // ROM ONLY
            } else if (cart_type >= 0x01 && cart_type <= 0x03) {
                mbc_type = 1;  // MBC1
            } else if (cart_type >= 0x05 && cart_type <= 0x06) {
                mbc_type = 2;  // MBC2
            } else if (cart_type >= 0x0F && cart_type <= 0x13) {
                mbc_type = 3;  // MBC3
            } else if (cart_type >= 0x19 && cart_type <= 0x1E) {
                mbc_type = 5;  // MBC5
             } else {
                 mbc_type = 0;  // Unknown, treat as ROM ONLY
                 ESP_LOGW("MemoryBus", "Unknown cart type 0x%02X, treating as ROM ONLY", cart_type);
             }

             // Check if cartridge has battery-backed SRAM
             has_battery = save_manager::has_battery(cart_type);

             // Detect CGB mode from header byte 0x0143
             // 0x80 = CGB-compatible, 0xC0 = CGB-only
             uint8_t cgb_flag = data[0x0143];
             cgb_mode = (cgb_flag == 0x80 || cgb_flag == 0xC0);
             ESP_LOGI("MemoryBus", "CGB mode: %s (flag=0x%02X)", cgb_mode ? "YES" : "NO", cgb_flag);
             if (ppu) ppu->setCGBMode(cgb_mode);

             // Initialize CGB-specific I/O registers to their post-boot state
             if (cgb_mode) {
                 io_registers[0x4D] = 0x7E;  // KEY1: normal speed, bits 6-1 unused = 1
                 io_registers[0x4F] = 0xFE;  // VBK: bank 0 selected
                 io_registers[0x70] = 0x01;  // SVBK: WRAM bank 1
                 // HDMA registers: inactive
                 io_registers[0x51] = 0xFF;
                 io_registers[0x52] = 0xFF;
                 io_registers[0x53] = 0xFF;
                 io_registers[0x54] = 0xFF;
                 io_registers[0x55] = 0xFF;
                 // Initialize BG palette to white (CGB post-boot has white palette 0)
                 memset(bg_palette_ram, 0xFF, sizeof(bg_palette_ram));
                 memset(obj_palette_ram, 0xFF, sizeof(obj_palette_ram));
                 // Pre-warm palette RGB565 cache from initial palette RAM (all 0xFF = white)
                 for (int p = 0; p < 8; ++p)
                     for (int c = 0; c < 4; ++c) {
                         uint8_t lo = bg_palette_ram[p * 8 + c * 2];
                         uint8_t hi = bg_palette_ram[p * 8 + c * 2 + 1];
                         bg_pal_cache[p][c] = cgb_pal_to_rgb565(lo, hi);
                         obj_pal_cache[p][c] = cgb_pal_to_rgb565(lo, hi);
                     }
                 ESP_LOGI("MemoryBus", "CGB I/O registers initialized to post-boot state");
             }

             // Disable boot ROM for test ROMs (small size or specific titles)
             if (size < 0x8000) {
                 bootEnabled = false;
                 ESP_LOGI("MemoryBus", "Boot ROM disabled for small ROM (test mode)");
             }

            // Info (approx) for logs
            size_t expected_size = 0x8000u << rom_size_code;

            ESP_LOGI("MemoryBus", "ROM Header: type=0x%02X (MBC%d), size_code=0x%02X (%zu KB expected), actual=%zu KB, battery=%d",
                     cart_type, mbc_type, rom_size_code, expected_size / 1024, size / 1024, has_battery);
        } else {
            mbc_type = 0;
            ESP_LOGW("MemoryBus", "ROM too small for header, treating as ROM ONLY");
        }

        // Load first 32KB into base ROM (banks 0 and 1)
        size_t base_size = (size > rom.size()) ? rom.size() : size;
        std::memcpy(rom.data(), data, base_size);

        // Load remaining banks into extended ROM (PSRAM), allocated to exact ROM size
        if (size > rom.size()) {
            size_t extra_size = size - rom.size();

            // Free previous allocation if any (e.g. loading a second game)
            if (rom_extended) {
                heap_caps_free(rom_extended);
                rom_extended = nullptr;
            }

            rom_extended = static_cast<uint8_t*>(
                heap_caps_malloc(extra_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

            if (!rom_extended) {
                ESP_LOGE("MemoryBus", "Failed to allocate %zu KB for extended ROM in PSRAM!", extra_size / 1024);
            } else {
                if (!skip_extended_copy) {
                    std::memcpy(rom_extended, data + rom.size(), extra_size);
                }
                size_t num_banks = extra_size / 0x4000;
                ESP_LOGI("MemoryBus", "%s %zu additional ROM banks (%zu KB) into PSRAM",
                         skip_extended_copy ? "Allocated" : "Loaded",
                         num_banks, extra_size / 1024);
            }
        } else {
            ESP_LOGI("MemoryBus", "ROM fits in base 32KB, no extended banks needed");
        }

        // Calculate ROM bank mask based on actual ROM size
        // This prevents accessing non-existent banks (e.g., bank 257 in 64-bank ROM)
        size_t total_banks = (rom_size + 0x3FFF) / 0x4000;  // Round up
        if (total_banks <= 2) {
            rom_bank_mask = 0x01;  // 2 banks (32KB ROM)
        } else if (total_banks <= 4) {
            rom_bank_mask = 0x03;  // 4 banks (64KB)
        } else if (total_banks <= 8) {
            rom_bank_mask = 0x07;  // 8 banks (128KB)
        } else if (total_banks <= 16) {
            rom_bank_mask = 0x0F;  // 16 banks (256KB)
        } else if (total_banks <= 32) {
            rom_bank_mask = 0x1F;  // 32 banks (512KB)
        } else if (total_banks <= 64) {
            rom_bank_mask = 0x3F;  // 64 banks (1MB) ← Pokemon Blue
        } else if (total_banks <= 128) {
            rom_bank_mask = 0x7F;  // 128 banks (2MB)
        } else if (total_banks <= 256) {
            rom_bank_mask = 0xFF;  // 256 banks (4MB)
        } else {
            rom_bank_mask = 0x1FF;  // 512 banks (8MB, MBC5 maximum)
        }

        ESP_LOGI("MemoryBus", "ROM has %zu banks, using mask 0x%03X", total_banks, rom_bank_mask);

        // Initialize bank registers
        rom_bank = 1;  // Start with bank 1 (bank 0 is fixed at 0x0000-0x3FFF)
        ram_enabled = false;
        ram_bank = 0;
        mbc_mode = 0;
        updateROMBankCache();  // Populate DRAM cache for initial bank 1

        // Initialize RTC (MBC3)
        if (mbc_type == 3) {
            rtc_base_time = esp_timer_get_time() / 1000000;  // Convert µs to seconds
            rtc_seconds = 0;
            rtc_minutes = 0;
            rtc_hours = 0;
            rtc_days_low = 0;
            rtc_days_high = 0;
            ESP_LOGI("MemoryBus", "RTC initialized (MBC3)");
        }

        // Boot ROM is disabled, I/O registers already initialized in constructor
        ESP_LOGI("MemoryBus", "ROM loaded successfully: %zu KB total, MBC%d, battery=%d",
                 rom_size / 1024, mbc_type, has_battery);
    }

    // loadROMFromFile: reads ROM directly into rom[] and rom_extended (PSRAM),
    // avoiding a 2× peak PSRAM allocation that would occur with a full intermediate buffer.
    void MemoryBus::loadROMFromFile(const char* path, size_t file_size)
    {
        FILE* f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE("MemoryBus", "loadROMFromFile: cannot open %s", path);
            return;
        }

        // Static buffer for the first 32KB — avoids stack overflow (task stack is only 8KB)
        static uint8_t header_buf[0x8000];
        size_t header_bytes = std::min(file_size, (size_t)0x8000);
        memset(header_buf, 0, sizeof(header_buf));
        fread(header_buf, 1, header_bytes, f);

        // loadROM with skip_extended_copy=true: sets up MBC/CGB metadata and allocates
        // rom_extended in PSRAM, but skips the memcpy (data only has 32KB).
        loadROM(header_buf, file_size, /*skip_extended_copy=*/true);

        // Fill rom_extended directly from the file — no intermediate buffer needed
        if (file_size > 0x8000 && rom_extended && rom_extended_size > 0) {
            // File cursor is already past the first 32KB (fread advanced it)
            size_t bytes_read = fread(rom_extended, 1, rom_extended_size, f);
            if (bytes_read != rom_extended_size) {
                ESP_LOGW("MemoryBus", "loadROMFromFile: read %zu/%zu extended bytes",
                         bytes_read, rom_extended_size);
            }
            // Refresh ROM bank cache now that extended ROM data is correct
            updateROMBankCache();
            ESP_LOGI("MemoryBus", "ROM extended loaded directly from file (%zu KB)", bytes_read / 1024);
        }

        fclose(f);
    }

    void MemoryBus::stepTimer(uint8_t cycles)
    {
        if (timer)
        {
            timer->step(cycles);
        }
    }

    void MemoryBus::resetDIV()
    {
        if (timer) timer->writeDIV(0);
    }

    void MemoryBus::applySpeedSwitch(bool ds)
    {
        if (!cgb_mode) return;
        io_registers[0x4D] = ds ? 0xFE : 0x7E;
    }
    std::string MemoryBus::getSerialDebugOutput() const
    {
        if (serial)
        {
            return serial->getDebugOutput();
        }
        return "";
    }

    void MemoryBus::clearSerialDebugOutput()
    {
        if (serial)
        {
            serial->clearDebugOutput();
        }
    }

    esp_err_t MemoryBus::loadSRAM()
    {
        if (!has_battery || rom_path.empty())
        {
            return ESP_OK;  // No battery or ROM path not set
        }

        size_t sram_size = (mbc_type == 2) ? mbc2_ram.size() : EXTERNAL_RAM_SIZE;
        uint8_t* sram_ptr = (mbc_type == 2) ? mbc2_ram.data() : external_ram;

        esp_err_t ret = save_manager::load_sram(rom_path, sram_ptr, sram_size);
        if (ret == ESP_OK)
        {
            ESP_LOGI("MemoryBus", "SRAM loaded from save file");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGI("MemoryBus", "No save file found, starting fresh");
        }
        return ret;
    }

    esp_err_t MemoryBus::saveSRAM()
    {
        if (!has_battery || rom_path.empty())
        {
            return ESP_OK;  // No battery or ROM path not set
        }

        size_t sram_size = (mbc_type == 2) ? mbc2_ram.size() : EXTERNAL_RAM_SIZE;
        const uint8_t* sram_ptr = (mbc_type == 2) ? mbc2_ram.data() : external_ram;

        return save_manager::save_sram(rom_path, sram_ptr, sram_size);
    }

    void MemoryBus::markSRAMDirty()
    {
        if (!has_battery)
            return;

        sram_dirty_counter++;
    }

    void MemoryBus::hdmaHBlankStep()
    {
        if (!hdma_active || !cgb_mode)
            return;

        // Copy 16 bytes from hdma_src to hdma_dst
        for (uint8_t i = 0; i < 16; i++) {
            write(hdma_dst + i, read(hdma_src + i));
        }
        hdma_src += 16;
        hdma_dst += 16;

        if (hdma_remaining == 0) {
            // All blocks transferred
            hdma_active = false;
            io_registers[0x55] = 0xFF;  // Bit 7 = 1 means inactive/done
        } else {
            hdma_remaining--;
            io_registers[0x55] = hdma_remaining;  // Remaining blocks, bit 7 = 0 (active)
        }

        // Stall CPU for HDMA block transfer: 8 M-cycles = 32 T-cycles (normal speed),
        // 16 M-cycles = 64 T-cycles (double speed, KEY1 bit 7 = 1)
        if (cpu) {
            const bool double_speed = (io_registers[0x4D] & 0x80) != 0;
            cpu->startGeneralDMAStall(double_speed ? 64 : 32);
        }
    }

    void MemoryBus::updateRTC()
    {
        if (mbc_type != 3)
            return;

        // Check if RTC is halted (bit 6 of rtc_days_high)
        if (rtc_days_high & 0x40)
            return;

        // Get elapsed time since base time
        int64_t current_time = esp_timer_get_time() / 1000000;  // Convert µs to seconds
        int64_t elapsed = current_time - rtc_base_time;

        // Calculate RTC values from elapsed time
        rtc_seconds = elapsed % 60;
        rtc_minutes = (elapsed / 60) % 60;
        rtc_hours = (elapsed / 3600) % 24;
        uint16_t days = elapsed / 86400;

        rtc_days_low = days & 0xFF;

        // Handle day overflow (>511 days)
        if (days > 511)
        {
            rtc_days_high |= 0x80;  // Set carry bit
            rtc_days_high = (rtc_days_high & 0xFE) | ((days >> 8) & 0x01);
        }
        else
        {
            rtc_days_high = (rtc_days_high & 0xFE) | ((days >> 8) & 0x01);
        }
    }

    // Async save task - runs on Core 0 to avoid SPI conflicts
    void MemoryBus::save_task(void* arg)
    {
        MemoryBus* mmu = static_cast<MemoryBus*>(arg);
        ESP_LOGI("SaveTask", "Save task started on core %d", xPortGetCoreID());

        uint8_t dummy;
        while (mmu->save_task_running)
        {
            // Wait for save request (blocking with timeout)
            if (xQueueReceive(mmu->save_queue, &dummy, pdMS_TO_TICKS(1000)) == pdTRUE)
            {
                // Save requested
                ESP_LOGI("SaveTask", "Save requested, saving SRAM...");
                esp_err_t ret = mmu->saveSRAM();
                if (ret == ESP_OK)
                {
                    ESP_LOGI("SaveTask", "SRAM saved successfully");
                    mmu->sram_dirty_counter = 0;  // Reset dirty counter
                }
                else
                {
                    ESP_LOGE("SaveTask", "Failed to save SRAM: %s", esp_err_to_name(ret));
                }
            }
        }

        ESP_LOGI("SaveTask", "Save task shutting down");
        vTaskDelete(nullptr);
    }

    void MemoryBus::initSaveTask()
    {
        if (!has_battery || rom_path.empty())
        {
            ESP_LOGI("MemoryBus", "No battery or ROM path, skipping save task");
            return;
        }

        // Create queue for save requests
        save_queue = xQueueCreate(2, sizeof(uint8_t));
        if (!save_queue)
        {
            ESP_LOGE("MemoryBus", "Failed to create save queue");
            return;
        }

        // Start save task on Core 0 (to avoid conflicts with emulation on Core 1)
        save_task_running = true;
        BaseType_t ret = xTaskCreatePinnedToCore(
            save_task,
            "save_task",
            4096,           // Stack size
            this,           // Parameter (this pointer)
            2,              // Priority (lower than emulation)
            &save_task_handle,
            0               // Core 0
        );

        if (ret != pdPASS)
        {
            ESP_LOGE("MemoryBus", "Failed to create save task");
            vQueueDelete(save_queue);
            save_queue = nullptr;
            save_task_running = false;
        }
        else
        {
            ESP_LOGI("MemoryBus", "Save task initialized on Core 0");
        }
    }

    void MemoryBus::requestSave()
    {
        if (!save_queue || !has_battery)
            return;

        uint8_t dummy = 1;
        // Non-blocking send (if queue full, skip this request)
        xQueueSend(save_queue, &dummy, 0);
    }

    void MemoryBus::shutdownSaveTask()
    {
        if (!save_task_handle)
            return;

        ESP_LOGI("MemoryBus", "Shutting down save task...");

        // Request final save if dirty
        if (sram_dirty_counter > 0)
        {
            requestSave();
            vTaskDelay(pdMS_TO_TICKS(100));  // Wait for save to complete
        }

        // Stop task
        save_task_running = false;

        // Wait for task to finish
        if (save_task_handle)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            save_task_handle = nullptr;
        }

        // Clean up queue
        if (save_queue)
        {
            vQueueDelete(save_queue);
            save_queue = nullptr;
        }

        ESP_LOGI("MemoryBus", "Save task shut down");
    }
}
