#include "memory_bus.hpp"
#include "timer.hpp"
#include <cstring>

namespace memory
{
    MemoryBus::MemoryBus(std::unique_ptr<controller::Joypad> joypad)
        : rom{0}, vram{0}, external_ram{0}, wram{0},
          oam{0}, io_registers{0}, hram{0}, ie_register{0}, if_register{0},
          prev_joypad_state{0xFF}, joypad{std::move(joypad)}
    {
        // Initialize timer (must be after member initialization)
        timer = std::make_unique<timer::Timer>(*this);
    }

    MemoryBus::~MemoryBus()
    {
        // Destructor implementation
    }

    uint8_t MemoryBus::read(uint16_t address) const
    {
        // ROM Bank 0 (0x0000-0x3FFF)
        if (address <= ROM_BANK_0_END)
        {
            return rom[address];
        }
        // ROM Bank N (0x4000-0x7FFF)
        else if (address >= ROM_BANK_N_START && address <= ROM_BANK_N_END)
        {
            return rom[address];
        }
        // Video RAM (0x8000-0x9FFF)
        else if (address >= VRAM_START && address <= VRAM_END)
        {
            return vram[address - VRAM_START];
        }
        // External RAM (0xA000-0xBFFF)
        else if (address >= EXTERNAL_RAM_START && address <= EXTERNAL_RAM_END)
        {
            return external_ram[address - EXTERNAL_RAM_START];
        }
        // Work RAM (0xC000-0xDFFF)
        else if (address >= WRAM_START && address <= WRAM_END)
        {
            return wram[address - WRAM_START];
        }
        // Echo RAM (0xE000-0xFDFF) - mirrors WRAM
        else if (address >= ECHO_RAM_START && address <= ECHO_RAM_END)
        {
            return wram[address - ECHO_RAM_START];
        }
        // OAM (0xFE00-0xFE9F)
        else if (address >= OAM_START && address <= OAM_END)
        {
            return oam[address - OAM_START];
        }
        // Unusable memory (0xFEA0-0xFEFF)
        else if (address >= UNUSABLE_START && address <= UNUSABLE_END)
        {
            return 0xFF; // Returns 0xFF for unusable memory
        } 
        else if (address == IO_REGISTERS_START)
        {
            uint8_t current_state = joypad->read(io_registers[0]);

            // Detect button press transitions for joypad interrupt
            // Buttons are active low, so we invert to get pressed state
            uint8_t current_pressed = (~current_state) & 0x0F;
            uint8_t prev_pressed = (~prev_joypad_state) & 0x0F;

            // Detect new button presses (transition from released to pressed)
            uint8_t new_presses = current_pressed & ~prev_pressed;

            // Request interrupt if any button was newly pressed
            if (new_presses != 0) {
                request_interrupt(IRQFlag::IRQ_JOYP);
            }

            prev_joypad_state = current_state;

            return current_state;
        }
        // Interrupt Flag Register (0xFF0F)
        else if (address == IF_REGISTER)
        {
            return if_register;
        }
        // Timer registers (0xFF04-0xFF07)
        else if (address == timer::DIV_REGISTER)
        {
            return timer->readDIV();
        }
        else if (address == timer::TIMA_REGISTER)
        {
            return timer->readTIMA();
        }
        else if (address == timer::TMA_REGISTER)
        {
            return timer->readTMA();
        }
        else if (address == timer::TAC_REGISTER)
        {
            return timer->readTAC();
        }
        // I/O Registers (0xFF01-0xFF03, 0xFF08-0xFF0E, 0xFF10-0xFF7F)
        else if (address > IO_REGISTERS_START && address <= IO_REGISTERS_END)
        {
            return io_registers[address - IO_REGISTERS_START];
        }
        // High RAM (0xFF80-0xFFFE)
        else if (address >= HRAM_START && address <= HRAM_END)
        {
            return hram[address - HRAM_START];
        }
        // Interrupt Enable Register (0xFFFF)
        else if (address == IE_REGISTER)
        {
            return ie_register;
        }

        return 0xFF; // Default return for unmapped memory
    }

    void MemoryBus::write(uint16_t address, uint8_t value)
    {
        // ROM (0x0000-0x7FFF) - Read-only, but MBC writes trigger bank switching
        if (address <= ROM_BANK_N_END)
        {
            // TODO: Handle MBC bank switching
            // For now, ignore writes to ROM
            return;
        }
        // Video RAM (0x8000-0x9FFF)
        else if (address >= VRAM_START && address <= VRAM_END)
        {
            vram[address - VRAM_START] = value;
        }
        // External RAM (0xA000-0xBFFF)
        else if (address >= EXTERNAL_RAM_START && address <= EXTERNAL_RAM_END)
        {
            external_ram[address - EXTERNAL_RAM_START] = value;
        }
        // Work RAM (0xC000-0xDFFF)
        else if (address >= WRAM_START && address <= WRAM_END)
        {
            wram[address - WRAM_START] = value;
        }
        // Echo RAM (0xE000-0xFDFF) - mirrors WRAM
        else if (address >= ECHO_RAM_START && address <= ECHO_RAM_END)
        {
            wram[address - ECHO_RAM_START] = value;
        }
        // OAM (0xFE00-0xFE9F)
        else if (address >= OAM_START && address <= OAM_END)
        {
            oam[address - OAM_START] = value;
        }
        // Unusable memory (0xFEA0-0xFEFF) - ignore writes
        else if (address >= UNUSABLE_START && address <= UNUSABLE_END)
        {
            return;
        }
        // Joypad register (0xFF00) - Seuls les bits 4-5 sont modifiables
        else if (address == IO_REGISTERS_START)
        {
            // Masquer pour garder uniquement P14 et P15 (bits 4-5)
            // Forcer les bits 7-6 à 1 (requis par le spec Game Boy)
            io_registers[0] = (value & 0x30) | 0xC0;
        }
        // Interrupt Flag Register (0xFF0F)
        else if (address == IF_REGISTER)
        {
            if_register = value;
        }
        // Timer registers (0xFF04-0xFF07)
        else if (address == timer::DIV_REGISTER)
        {
            timer->writeDIV(value);
        }
        else if (address == timer::TIMA_REGISTER)
        {
            timer->writeTIMA(value);
        }
        else if (address == timer::TMA_REGISTER)
        {
            timer->writeTMA(value);
        }
        else if (address == timer::TAC_REGISTER)
        {
            timer->writeTAC(value);
        }
        // I/O Registers (0xFF01-0xFF03, 0xFF08-0xFF0E, 0xFF10-0xFF7F)
        else if (address > IO_REGISTERS_START && address <= IO_REGISTERS_END)
        {
            io_registers[address - IO_REGISTERS_START] = value;
            // TODO: Handle special I/O register side effects (DMA, etc.)
        }
        // High RAM (0xFF80-0xFFFE)
        else if (address >= HRAM_START && address <= HRAM_END)
        {
            hram[address - HRAM_START] = value;
        }
        // Interrupt Enable Register (0xFFFF)
        else if (address == IE_REGISTER)
        {
            ie_register = value;
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

    void MemoryBus::loadROM(const uint8_t* data, size_t size)
    {
        size_t copy_size = (size > rom.size()) ? rom.size() : size;
        std::memcpy(rom.data(), data, copy_size);
    }

    void MemoryBus::stepTimer(uint8_t cycles)
    {
        if (timer)
        {
            timer->step(cycles);
        }
    }
}
