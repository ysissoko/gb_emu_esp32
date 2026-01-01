#include "memory_bus.hpp"
#include "joypad.hpp"
#include "timer.hpp"
#include "serial.hpp"
#include "esp_log.h"
#include <cstring>

namespace memory
{
    MemoryBus::MemoryBus(const std::shared_ptr<controller::Joypad>& joypad)
        : rom{0}, vram{0}, external_ram{0}, wram{0},
          oam{0}, io_registers{0}, hram{0}, ie_register{0}, if_register{0},
          prev_joypad_state{0xFF}, joypad{joypad}
    {
        // Initialize timer (must be after member initialization)
        timer = std::make_unique<timer::Timer>(*this);
        // Initialize serial port
        serial = std::make_unique<serial::Serial>(*this);
    }

    MemoryBus::~MemoryBus()
    {
        // Destructor implementation
    }

    uint8_t MemoryBus::read(uint16_t address) const
    {
        // Optimized: Use high nibble (top 4 bits) for fast region lookup
        // This reduces the number of comparisons significantly
        switch (address >> 12) {  // Get top 4 bits (0x0-0xF)
            case 0x0: case 0x1: case 0x2: case 0x3:
                // ROM Bank 0 (0x0000-0x3FFF)
                return rom[address];

            case 0x4: case 0x5: case 0x6: case 0x7:
                // ROM Bank N (0x4000-0x7FFF)
                return rom[address];

            case 0x8: case 0x9:
                // Video RAM (0x8000-0x9FFF)
                return vram[address - VRAM_START];

            case 0xA: case 0xB:
                // External RAM (0xA000-0xBFFF)
                return external_ram[address - EXTERNAL_RAM_START];

            case 0xC: case 0xD:
                // Work RAM (0xC000-0xDFFF)
                return wram[address - WRAM_START];

            case 0xE:
                // Echo RAM (0xE000-0xEFFF) - mirrors WRAM
                return wram[address - ECHO_RAM_START];

            case 0xF:
                // High memory region (0xF000-0xFFFF)
                // Need further subdivision
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
                    // Handle special registers with direct checks
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
        // Optimized: Use high nibble for fast region lookup
        switch (address >> 12) {
            case 0x0: case 0x1: case 0x2: case 0x3:
            case 0x4: case 0x5: case 0x6: case 0x7:
                // ROM (0x0000-0x7FFF) - Read-only, but MBC writes trigger bank switching
                // TODO: Handle MBC bank switching
                return;

            case 0x8: case 0x9:
                // Video RAM (0x8000-0x9FFF)
                vram[address - VRAM_START] = value;
                return;

            case 0xA: case 0xB:
                // External RAM (0xA000-0xBFFF)
                external_ram[address - EXTERNAL_RAM_START] = value;
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
                    else {
                        io_registers[address - IO_REGISTERS_START] = value;
                        // TODO: Handle special I/O register side effects (DMA, etc.)
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
        // Detect MBC type from ROM header
        if (size >= 0x8000) {
            uint8_t cart_type = data[0x0147]; // Cartridge type byte
            if (cart_type >= 0x01 && cart_type <= 0x03) {
                mbc_type = cart_type; // MBC1-3
                ESP_LOGI("MemoryBus", "MBC%d detected, ROM size: %zu bytes", mbc_type, size);
            } else if (cart_type >= 0x05 && cart_type <= 0x06) {
                mbc_type = 5; // MBC5
                ESP_LOGI("MemoryBus", "MBC5 detected, ROM size: %zu bytes", mbc_type, size);
            } else {
                mbc_type = 0; // Standard ROM
                ESP_LOGI("MemoryBus", "Standard ROM detected, size: %zu bytes", size);
            }
        } else {
            mbc_type = 0; // Standard ROM
            ESP_LOGI("MemoryBus", "Standard ROM detected, size: %zu bytes", size);
        }
        
        // Load ROM into base 32KB (0x0000-0x7FFF)
        size_t copy_size = (size > rom.size()) ? rom.size() : size;
        std::memcpy(rom.data(), data, copy_size);
        
        // Load additional banks into extended storage
        if (size > rom.size()) {
            size_t extra_size = size - rom.size();
            size_t banks_to_copy = (extra_size > rom_extended.size()) ? rom_extended.size() : extra_size;
            std::memcpy(rom_extended.data(), data + rom.size(), banks_to_copy);
            ESP_LOGI("MemoryBus", "Loaded %zu additional ROM banks", banks_to_copy / 0x4000);
        } else {
            mbc_type = 0; // Standard ROM
            ESP_LOGI("MemoryBus", "Standard ROM detected, size: %zu bytes", size);
        }
        
        // Load ROM into base 32KB
        copy_size = (size > rom.size()) ? rom.size() : size;
        std::memcpy(rom.data(), data, copy_size);
        
        // Load additional banks into extended storage
        if (size > rom.size()) {
            size_t extra_size = size - rom.size();
            size_t banks_to_copy = (extra_size > 0x100000) ? 0x100000 : extra_size;
            banks_to_copy = (banks_to_copy > rom_extended.size()) ? rom_extended.size() : banks_to_copy;
            std::memcpy(rom_extended.data(), data + rom.size(), banks_to_copy);
            ESP_LOGI("MemoryBus", "Loaded %zu additional ROM banks", banks_to_copy / 0x4000);
        }
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
}
