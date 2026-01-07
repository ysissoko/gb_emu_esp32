#include "memory_bus.hpp"
#include "cpu.hpp"  // For DMA control
#include "joypad.hpp"
#include "timer.hpp"
#include "serial.hpp"
#include "apu.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstring>

// Branch prediction hints
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace memory
{
    MemoryBus::MemoryBus(const std::shared_ptr<controller::Joypad>& joypad)
        : rom{0}, vram{0}, wram{0},
          oam{0}, io_registers{0}, hram{0}, ie_register{0}, if_register{0},
          prev_joypad_state{0xFF}, joypad{joypad}
    {
        // Allocate extended ROM storage in PSRAM (2MB max)
        rom_extended = static_cast<uint8_t*>(
            heap_caps_malloc(0x200000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

        if (!rom_extended)
        {
            ESP_LOGE("MemoryBus", "Failed to allocate ROM extended storage in PSRAM!");
        }
        else
        {
            ESP_LOGI("MemoryBus", "Allocated 2MB ROM extended storage in PSRAM");
            memset(rom_extended, 0xFF, 0x200000);
        }

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

    uint8_t MemoryBus::read(uint16_t address) const
    {
        // Boot ROM (0x0000-0x00FF) if enabled
        if (bootEnabled && address < 0x0100) {
            return memory::BOOT_ROM[address];
        }

        // Restrict VRAM/OAM access during PPU Mode 3 (drawing)
        if (ppu && ppu->getMode() == ::ppu::Mode::DRAWING &&
            ((address >= 0x8000 && address < 0xA000) || (address >= 0xFE00 && address < 0xFEA0))) {
            return 0xFF;
        }

        // Taille réelle du segment "extended" (banks >= 2).
        // Robustesse: éviter tout underflow si rom_size < 0x8000.
        const size_t extended_size = (rom_size > 0x8000) ? (rom_size - 0x8000) : 0;

        // Optimized: Use high nibble (top 4 bits) for fast region lookup
        switch (address >> 12) {  // Get top 4 bits (0x0-0xF)
            case 0x0: case 0x1: case 0x2: case 0x3:
                // ROM Bank 0 (0x0000-0x3FFF)
                // NOTE: (optionnel) MBC1 mode 1 peut bank-switcher ici, mais non requis
                // pour corriger Wario Land; on garde ton comportement actuel.
                return rom[address];

            case 0x4: case 0x5: case 0x6: case 0x7:
            {
                // ROM Bank N (0x4000-0x7FFF) - switchable bank

                // ROM ONLY / pas de ROM extended / ROM <= 32KB
                if (UNLIKELY(mbc_type == 0 || !rom_extended || extended_size == 0)) {
                    // Dans ce cas, tout est dans rom[]
                    return rom[address];
                }

                // IMPORTANT: Ne PAS masquer rom_bank en MBC1 mode 1 ici.
                // La zone 0x4000-0x7FFF utilise la banque ROM "complète".
                uint16_t bank = rom_bank;

                // Bank 0 est interdit dans la zone switchable
                if (bank == 0) bank = 1;

                // Bank 1 est déjà dans rom[] (0x4000-0x7FFF)
                if (bank == 1) {
                    return rom[address];
                }

                // Banks >= 2 sont dans rom_extended, avec TON layout:
                // rom_extended[0] => bank 2
                // rom_extended[0x4000] => bank 3
                const size_t bank_index = static_cast<size_t>(bank - 2);
                const size_t offset = bank_index * 0x4000 + (address - 0x4000);

                if (LIKELY(offset < extended_size)) {
                    return rom_extended[offset];
                }
                return 0xFF;
            }

            case 0x8: case 0x9:
                // Video RAM (0x8000-0x9FFF)
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
                    else if (address == IF_REGISTER) return if_register;
                    else if (address >= 0xFF10 && address <= 0xFF3F) {
                        // APU registers (0xFF10-0xFF3F)
                        return apu->read(address);
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
                    }
                }
                else if (LIKELY(mbc_type == 1 || mbc_type == 3)) {
                    // MBC1/MBC3: 5-bit bank number (0x01-0x1F)
                    uint8_t bank = value & 0x1F;
                    if (UNLIKELY(bank == 0)) bank = 1;  // Bank 0 forbidden
                    rom_bank = (rom_bank & 0xE0) | bank;
                    rom_bank &= rom_bank_mask;  // Mask to valid banks
                } else if (mbc_type == 5) {
                    // MBC5: 9-bit bank number (lower 8 bits)
                    rom_bank = (rom_bank & 0x100) | value;
                    rom_bank &= rom_bank_mask;  // Mask to valid banks
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
                    } else {
                        // RAM banking mode
                        ram_bank = value & 0x03;
                    }
                } else if (mbc_type == 3) {
                    // MBC3: RAM bank (0x00-0x03) or RTC register (0x08-0x0C)
                    ram_bank = value & 0x0F;
                } else if (mbc_type == 5) {
                    // MBC5: 9th bit of ROM bank
                    rom_bank = (rom_bank & 0xFF) | ((value & 0x01) << 8);
                    rom_bank &= rom_bank_mask;  // Mask to valid banks
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
                vram[address - VRAM_START] = value;
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
                wram[address - WRAM_START] = value;
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

    void memory::MemoryBus::loadROM(const uint8_t* data, size_t size)
    {
        rom_size = size;

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

        // Load remaining banks into extended ROM (PSRAM)
        if (size > rom.size() && rom_extended) {
            size_t extra_size = size - rom.size();
            size_t max_extended = 0x200000;  // 2MB PSRAM limit
            size_t copy_size = (extra_size > max_extended) ? max_extended : extra_size;

            std::memcpy(rom_extended, data + rom.size(), copy_size);

            size_t num_banks = copy_size / 0x4000;
            ESP_LOGI("MemoryBus", "Loaded %zu additional ROM banks (%zu KB) into PSRAM",
                     num_banks, copy_size / 1024);
        } else if (size <= rom.size()) {
            ESP_LOGI("MemoryBus", "ROM fits in base 32KB, no extended banks needed");
        }

        // Calculate ROM bank mask based on actual ROM size
        // This prevents accessing non-existent banks (e.g., bank 257 in 64-bank ROM)
        size_t total_banks = (rom_size + 0x3FFF) / 0x4000;  // Roundal_banks <= 2) {
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

    void MemoryBus::stepTimer(uint8_t cycles)
    {
        if (timer)
        {
            timer->step(cycles);
        }
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

        // DISABLED: Auto-save causes Cache error / MMU fault during SPI operations
        // Save will be done manually or on game exit
        // TODO: Implement async save in dedicated task on Core 0
        #if 0
        if (sram_dirty_counter >= 300)
        {
            sram_dirty_counter = 0;
            esp_err_t ret = saveSRAM();
            if (ret == ESP_OK)
            {
                ESP_LOGD("MemoryBus", "Auto-saved SRAM");
            }
        }
        #endif
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
