#pragma once

#include <cstdint>
#include <array>
#include "esp_attr.h"
#include "memory_bus.hpp"
#include "ppu.hpp"

// Branch prediction hints for compiler optimization (from Walnut-GB)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace cpu
{
    constexpr uint16_t Z_FLAG_MASK = 0x80;
    constexpr uint16_t N_FLAG_MASK = 0x40;
    constexpr uint16_t H_FLAG_MASK = 0x20;
    constexpr uint16_t C_FLAG_MASK = 0x10;
    constexpr int GB_CYCLES_PER_FRAME = 70224;
    constexpr int64_t FRAME_US = 16743; // ~16.743 ms
    constexpr uint8_t INTERRUPT_CYCLES = 20;
    // interrupt request vector
    constexpr const std::array<uint16_t, 5> irq_vec = {0x40, 0x48, 0x50, 0x58, 0x60};

    class CPU
    {
    public:
        CPU(memory::MemoryBus&, ppu::PPU&);
        ~CPU();
        IRAM_ATTR uint8_t execute(uint8_t) __attribute__((hot));
        IRAM_ATTR uint8_t execute_extended(uint8_t) __attribute__((hot));
        IRAM_ATTR uint8_t step() __attribute__((hot));
        void run_frame();

        // DMA control - called by MemoryBus when DMA is triggered
        IRAM_ATTR inline void startDMA() {
            dma_in_progress = true;
            dma_cycles_remaining = 160;  // DMA takes 160 M-cycles (640 clock cycles)
        }
    private:
        uint8_t a{0};
        uint8_t b{0};
        uint8_t c{0};
        uint8_t d{0};
        uint8_t e{0};
        uint8_t f{0};
        uint8_t h{0};
        uint8_t l{0};
        uint16_t sp{0};
        uint16_t pc{0};

        // Memory bus for accessing ROM, RAM, and I/O
        memory::MemoryBus& mmu;

        // Picture Processing Unit for rendering
        ppu::PPU& ppu;

        bool cpu_stopped{false};
        bool ime_enabled{false};
        bool ei_pending{false};  // EI has 1-instruction delay
        bool halt_bug{false};

        // DMA state tracking
        bool dma_in_progress{false};
        uint16_t dma_cycles_remaining{0};

        IRAM_ATTR inline bool readZFlag() const { return (f & Z_FLAG_MASK) != 0; };
        IRAM_ATTR inline bool readNFlag() const { return (f & N_FLAG_MASK) != 0; };
        IRAM_ATTR inline bool readHFlag() const { return (f & H_FLAG_MASK) != 0; };
        IRAM_ATTR inline bool readCFlag() const { return (f & C_FLAG_MASK) != 0; };

        bool test_interrupts_flags();

        IRAM_ATTR inline void setZFlag(bool value) {
            if (value) {
                f |= Z_FLAG_MASK;
            } else {
                f &= ~Z_FLAG_MASK;
            }
        }

        IRAM_ATTR inline void setNFlag(bool value) {
            if (value) {
                f |= N_FLAG_MASK;
            } else {
                f &= ~N_FLAG_MASK;
            }
        }

        IRAM_ATTR inline void setHFlag(bool value) {
            if (value) {
                f |= H_FLAG_MASK;
            } else {
                f &= ~H_FLAG_MASK;
            }
        }

        IRAM_ATTR inline void setCFlag(bool value) {
            if (value) {
                f |= C_FLAG_MASK;
            } else {
                f &= ~C_FLAG_MASK;
            }
        }

        // Helper functions for register access and common operations
        IRAM_ATTR inline uint8_t* getReg(uint8_t idx) {
            switch (idx) {
                case 0: return &b;
                case 1: return &c;
                case 2: return &d;
                case 3: return &e;
                case 4: return &h;
                case 5: return &l;
                case 7: return &a;
                default: return &b;
            }
        }

        IRAM_ATTR inline void doInc(uint8_t& reg) {
            setHFlag((reg & 0x0F) == 0x0F);
            reg += 1;
            setZFlag(reg == 0);
            setNFlag(false);
        }

        IRAM_ATTR inline void doDec(uint8_t& reg) {
            setHFlag((reg & 0x0F) == 0x00);
            reg -= 1;
            setZFlag(reg == 0);
            setNFlag(true);
        }

        IRAM_ATTR inline void doAdd(uint8_t value, bool use_carry = false) {
            uint8_t carry = (use_carry && readCFlag()) ? 1 : 0;
            setHFlag(((a & 0x0F) + (value & 0x0F) + carry) > 0x0F);
            setCFlag(((uint16_t)a + (uint16_t)value + carry) > 0xFF);
            a += value + carry;
            setZFlag(a == 0);
            setNFlag(false);
        }

        IRAM_ATTR inline void doSub(uint8_t value, bool use_carry = false) {
            uint8_t carry = (use_carry && readCFlag()) ? 1 : 0;
            setHFlag((a & 0x0F) < ((value & 0x0F) + carry));
            setCFlag(a < (value + carry));
            a -= value + carry;
            setZFlag(a == 0);
            setNFlag(true);
        }

        IRAM_ATTR inline void doAnd(uint8_t value) {
            a &= value;
            setZFlag(a == 0);
            setNFlag(false);
            setHFlag(true);
            setCFlag(false);
        }

        IRAM_ATTR inline void doXor(uint8_t value) {
            a ^= value;
            setZFlag(a == 0);
            setNFlag(false);
            setHFlag(false);
            setCFlag(false);
        }

        IRAM_ATTR inline void doOr(uint8_t value) {
            a |= value;
            setZFlag(a == 0);
            setNFlag(false);
            setHFlag(false);
            setCFlag(false);
        }

        IRAM_ATTR inline void doCp(uint8_t value) {
            setZFlag(a == value);
            setNFlag(true);
            setHFlag((a & 0x0F) < (value & 0x0F));
            setCFlag(a < value);
        }

        IRAM_ATTR inline uint16_t getAF() const {
            return a << 8 | f;
        }

        IRAM_ATTR inline uint16_t getBC() const {
            return b << 8 | c;
        }

        IRAM_ATTR inline uint16_t getDE() const {
            return d << 8 | e;
        }

        IRAM_ATTR inline uint16_t getHL() const {
            return h << 8 | l;
        }

        IRAM_ATTR inline void setAF(uint16_t value) {
            a = (value >> 8) & 0xFF;
            f = value & 0xF0; // Lower nibble of F is always 0
        }

        IRAM_ATTR inline void setBC(uint16_t value) {
            b = (value >> 8) & 0xFF;
            c = value & 0xFF;
        }

        IRAM_ATTR inline void setDE(uint16_t value) {
            d = (value >> 8) & 0xFF;
            e = value & 0xFF;
        }

        IRAM_ATTR inline void setHL(uint16_t value) {
            h = (value >> 8) & 0xFF;
            l = value & 0xFF;
        }
    };
}
