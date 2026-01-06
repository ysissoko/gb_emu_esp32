#include "cpu.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <cstdio>

namespace cpu
{

    CPU::CPU(memory::MemoryBus &mmu, ppu::PPU &ppu) : mmu(mmu), ppu(ppu)
    {
        // Initialize registers
        a = 0x01;
        f = 0xB0;
        b = 0x00;
        c = 0x13;
        d = 0x00;
        e = 0xD8;
        h = 0x01;
        l = 0x4D;
        sp = 0xFFFE;
        pc = 0x0100; // Start execution at 0x0100

        // Initialize LCD registers with default values
        // LCDC (0xFF40) - LCD Control
        mmu.write(0xFF40, 0x91); // LCD on, BG on, BG tile map at 0x9800, tile data at 0x8000
        // STAT (0xFF41) - LCD Status
        mmu.write(0xFF41, 0x00);
        // SCY (0xFF42) - Scroll Y
        mmu.write(0xFF42, 0x00);
        // SCX (0xFF43) - Scroll X
        mmu.write(0xFF43, 0x00);
        // LY (0xFF44) - LCD Y-Coordinate (read-only)
        mmu.write(0xFF44, 0x00);
        // LYC (0xFF45) - LY Compare
        mmu.write(0xFF45, 0x00);
        // BGP (0xFF47) - Background Palette
        mmu.write(0xFF47, 0xE4); // 11 10 01 00 (black, dark gray, light gray, white)
        // OBP0 (0xFF48) - Object Palette 0
        mmu.write(0xFF48, 0xE4);
        // OBP1 (0xFF49) - Object Palette 1
        mmu.write(0xFF49, 0xE4);
        // WY (0xFF4A) - Window Y Position
        mmu.write(0xFF4A, 0x00);
        // WX (0xFF4B) - Window X Position
        mmu.write(0xFF4B, 0x00);
    }

    CPU::~CPU()
    {
        // Destructor implementation
    }

    /// @brief run starts the CPU execution loop
    void CPU::run_frame()
    {
        constexpr int CYCLES_PER_SCANLINE = 456;
        constexpr int TOTAL_SCANLINES = 154;

        for (int sl = 0; sl < TOTAL_SCANLINES; ++sl)
        {
            int cycles = 0;

            while (cycles < CYCLES_PER_SCANLINE)
            {
                uint8_t cpu_cycles = step();
                cycles += cpu_cycles;

                // Update timer immediately after each instruction
                mmu.stepTimer(cpu_cycles);

                // Update PPU
                ppu.step(cpu_cycles);

                // Check interrupts every instruction (required for accuracy)
                if (UNLIKELY(test_interrupts_flags()))
                    cycles += INTERRUPT_CYCLES;
            }
        }

        if (UNLIKELY(ppu.isFrameReady()))
            ppu.clearFrameReady();
    }

    bool CPU::test_interrupts_flags()
    {
        // interrupt enable read
        uint8_t ie = mmu.read(memory::IE_REGISTER);
        // interrupt flag read
        uint8_t if_ = mmu.read(memory::IF_REGISTER);
        // pending interrupts calculated from enabled interrupts and currently requested interrupts
        uint8_t pending = ie & if_;

        // Wake CPU from HALT if any interrupt is pending (IE & IF != 0)
        if (UNLIKELY(pending != 0))
        {
            cpu_stopped = false;
        }

        // Only service interrupts if IME is enabled (interrupts are rare)
        if (LIKELY(!ime_enabled || pending == 0))
            return false;

        for (uint8_t bit_idx = 0; bit_idx < irq_vec.size(); bit_idx++)
        {
            if (pending & (1 << bit_idx))
            {
                ime_enabled = false;
                // Set the flag to false
                mmu.write(memory::IF_REGISTER, if_ & ~(1 << bit_idx));
                // push PC on stack
                sp--;
                mmu.write(sp, (pc >> 8) & 0xFF); // MSB
                sp--;
                mmu.write(sp, pc & 0xFF); // LSB
                pc = irq_vec.at(bit_idx);
                return true;
            }
        }

        return false;
    }

    /// @brief step executes a single CPU instruction
    /// @return the number of cycles the instruction took
    uint8_t CPU::step()
    {
        // DMA: CPU is blocked during DMA transfer (critical for timing accuracy!)
        // DMA has highest priority - checked before HALT
        if (UNLIKELY(dma_in_progress))
        {
            if (dma_cycles_remaining > 0)
            {
                uint8_t cycles_to_consume = (dma_cycles_remaining >= 4) ? 4 : dma_cycles_remaining;
                dma_cycles_remaining -= cycles_to_consume;

                if (dma_cycles_remaining == 0)
                {
                    dma_in_progress = false;
                }

                return cycles_to_consume;
            }
            dma_in_progress = false;  // Safety fallback
        }

        // HALT: CPU is stopped, wait for interrupt
        // Return 4 cycles for cycle-accurate emulation (required for test ROMs)
        // Note: HALT optimization was removed to fix instr_timing test failures
        if (UNLIKELY(cpu_stopped))
        {
            return 4;
        }

        // Handle EI 1-instruction delay: activate IME before next instruction
        if (UNLIKELY(ei_pending))
        {
            ei_pending = false;
            ime_enabled = true;
        }

        uint8_t opcode;

        if (halt_bug)
        {
            opcode = mmu.read(pc); // PC volontairement NON incrémenté
            halt_bug = false;      // one-shot
        }
        else
        {
            opcode = mmu.read(pc++);
        }

        return execute(opcode);
    }

    /// @brief execute instruction based on opcode
    /// @param opcode the opcode to execute
    /// @return the number of cycles the instruction took
    /// @note see full list at https://gbdev.io/gb-opcodes/optables/
    uint8_t CPU::execute(uint8_t opcode)
    {
        switch (opcode)
        {
        case 0x00: // NOP
            // No operation
            return 4;
        case 0x01: // LD BC, d16
            setBC(mmu.read16(pc));
            pc += 2;
            return 12;
        case 0x02: // LD (BC), A
            mmu.write(getBC(), a);
            return 8;
        case 0x03: // INC BC
            setBC(getBC() + 1);
            return 8;
        case 0x04: // INC B
            doInc(b);
            return 4;
        case 0x05: // DEC B
            doDec(b);
            return 4;
        case 0x06: // LD B, d8
            b = mmu.read(pc++);
            return 8;
        case 0x07: // RLCA
        {
            uint8_t carry = (a >> 7) & 1;
            a = (a << 1) | carry;
            setZFlag(false);
            setNFlag(false);
            setHFlag(false);
            setCFlag(carry);
            return 4;
        }
        case 0x08: // LD (a16), SP
            mmu.write16(mmu.read16(pc), sp);
            pc += 2;
            return 20;
        case 0x09: // ADD HL, BC
        {
            uint32_t result = (uint32_t)getHL() + (uint32_t)getBC();
            setNFlag(false);
            setHFlag(((getHL() & 0xFFF) + (getBC() & 0xFFF)) > 0xFFF);
            setHL(result & 0xFFFF);
            setCFlag(result > 0xFFFF);
        }
            return 8;
        case 0x0A: // LD A, (BC)
            a = mmu.read(getBC());
            return 8;
        case 0x0B: // DEC BC
            setBC(getBC() - 1);
            return 8;
        case 0x0C: // INC C
            doInc(c);
            return 4;
        case 0x0D: // DEC C
            doDec(c);
            return 4;
        case 0x0E: // LD C, d8
            c = mmu.read(pc++);
            return 8;
        case 0x0F: // RRCA
        {
            uint8_t carry = (a & 1);
            a = (a >> 1) | (carry << 7);
            setZFlag(false);
            setNFlag(false);
            setHFlag(false);
            setCFlag(carry);
            return 4;
        }
        case 0x10: // STOP n8
        {
            (void)mmu.read(pc++); // Read and discard operand
            cpu_stopped = true;
            return 8;
        }
        case 0x11: // LD DE, n16
            setDE(mmu.read16(pc));
            pc += 2;
            return 12;
        case 0x12: // LD (DE), A
            mmu.write(getDE(), a);
            return 8;
        case 0x13: // INC DE
            setDE(getDE() + 1);
            return 8;
        case 0x14: // INC D
            doInc(d);
            return 4;
        case 0x15: // DEC D
            doDec(d);
            return 4;
        case 0x16: // LD D, d8
            d = mmu.read(pc++);
            return 8;
        case 0x17: // RLA
        {
            uint8_t old_carry = readCFlag() ? 1 : 0;
            uint8_t new_carry = (a >> 7) & 1;
            a = (a << 1) | old_carry;
            setZFlag(false);
            setNFlag(false);
            setHFlag(false);
            setCFlag(new_carry);
            return 4;
        }
        case 0x18: // JR r8
        {
            int8_t n8 = mmu.read(pc++);
            pc += n8;
            return 12;
        }
        case 0x19: // ADD HL, DE
            setNFlag(false);
            setHFlag(((getHL() & 0xFFF) + (getDE() & 0xFFF)) > 0xFFF);
            setCFlag(((uint32_t)getHL() + (uint32_t)getDE()) > 0xFFFF);
            setHL(getHL() + getDE());
            return 8;
        case 0x1A: // LD A, (DE)
            a = mmu.read(getDE());
            return 8;
        case 0x1B: // DEC DE
            setDE(getDE() - 1);
            return 8;
        case 0x1C: // INC E
            doInc(e);
            return 4;
        case 0x1D: // DEC E
            doDec(e);
            return 4;
        case 0x1E: // LD E, d8
            e = mmu.read(pc++);
            return 8;
        case 0x1F: // RRA
        {
            uint8_t old_carry = readCFlag() ? 1 : 0;
            uint8_t new_carry = a & 1;
            a = (a >> 1) | (old_carry << 7);
            setZFlag(false);
            setNFlag(false);
            setHFlag(false);
            setCFlag(new_carry);
            return 4;
        }
        case 0x20: // JR NZ, r8
        {
            int8_t n8 = mmu.read(pc++);
            if (!readZFlag())
            {
                pc += n8;
                return 12;
            }
            return 8;
        }
        case 0x21: // LD HL, d16
            setHL(mmu.read16(pc));
            pc += 2;
            return 12;
        case 0x22: // LD (HL+), A
            mmu.write(getHL(), a);
            setHL(getHL() + 1);
            return 8;
        case 0x23: // INC HL
            setHL(getHL() + 1);
            return 8;
        case 0x24: // INC H
            doInc(h);
            return 4;
        case 0x25: // DEC H
            doDec(h);
            return 4;
        case 0x26: // LD H, d8
            h = mmu.read(pc++);
            return 8;
        case 0x27: // DAA
        {
            uint8_t correction = 0;
            bool set_carry = false;

            if (!readNFlag())
            { // après addition
                if (readHFlag() || (a & 0x0F) > 9)
                    correction |= 0x06;
                if (readCFlag() || a > 0x99)
                {
                    correction |= 0x60;
                    set_carry = true;
                }
                a += correction;
            }
            else
            { // après soustraction
                if (readHFlag())
                    correction |= 0x06;
                if (readCFlag())
                    correction |= 0x60;
                a -= correction;
            }

            setZFlag(a == 0);
            setHFlag(false);
            setCFlag(set_carry);  // TOUJOURS set/clear le carry flag selon set_carry
        }
            return 4;

        case 0x28: // JR Z, r8
        {
            int8_t n8 = mmu.read(pc++);
            if (readZFlag())
            {
                pc += n8;
                return 12;
            }
            return 8;
        }
        case 0x29: // ADD HL, HL
            setNFlag(false);
            setHFlag(((getHL() & 0xFFF) + (getHL() & 0xFFF)) > 0xFFF);
            setCFlag(((uint32_t)getHL() + (uint32_t)getHL()) > 0xFFFF);
            setHL(getHL() * 2);
            return 8;
        case 0x2A: // LD A, (HL+)
            a = mmu.read(getHL());
            setHL(getHL() + 1);
            return 8;
        case 0x2B: // DEC HL
            setHL(getHL() - 1);
            return 8;
        case 0x2C: // INC L
            doInc(l);
            return 4;
        case 0x2D: // DEC L
            doDec(l);
            return 4;
        case 0x2E: // LD L, d8
            l = mmu.read(pc++);
            return 8;
        case 0x2F: // CPL
            setNFlag(true);
            setHFlag(true);
            a = ~a;
            return 4;
        case 0x30: // JR NC, r8
        {
            int8_t n8 = mmu.read(pc++);
            if (!readCFlag())
            {
                pc += n8;
                return 12;
            }
            return 8;
        }
        case 0x31: // LD SP, d16
            sp = mmu.read16(pc);
            pc += 2;
            return 12;
        case 0x32: // LD (HL-), A
            mmu.write(getHL(), a);
            setHL(getHL() - 1);
            return 8;
        case 0x33: // INC SP
            sp += 1;
            return 8;
        case 0x34: // INC (HL)
        {
            uint8_t value = mmu.read(getHL());
            doInc(value);
            mmu.write(getHL(), value);
            return 12;
        }
        case 0x35: // DEC (HL)
        {
            uint8_t value = mmu.read(getHL());
            doDec(value);
            mmu.write(getHL(), value);
            return 12;
        }
        case 0x36: // LD (HL), d8
        {
            uint8_t value = mmu.read(pc++);
            mmu.write(getHL(), value);
        }
            return 12;
        case 0x37: // SCF
            // Set Carry Flag
            setNFlag(false);
            setHFlag(false);
            setCFlag(true);
            return 4;
        case 0x38: // JR C, r8
        {
            int8_t n8 = mmu.read(pc++);
            if (readCFlag())
            {
                pc += n8;
                return 12;
            }
            return 8;
        }
        case 0x39: // ADD HL, SP
            setNFlag(false);
            setHFlag(((getHL() & 0xFFF) + (sp & 0xFFF)) > 0xFFF);
            setCFlag(((uint32_t)getHL() + (uint32_t)sp) > 0xFFFF);
            setHL(getHL() + sp);
            return 8;
        case 0x3A: // LD A, (HL-)
            a = mmu.read(getHL());
            setHL(getHL() - 1);
            return 8;
        case 0x3B: // DEC SP
            sp -= 1;
            return 8;
        case 0x3C: // INC A
            doInc(a);
            return 4;
        case 0x3D: // DEC A
            doDec(a);
            return 4;
        case 0x3E: // LD A, d8
            a = mmu.read(pc++);
            return 8;
        case 0x3F: // CCF
            // Complement Carry Flag
            setCFlag(!readCFlag());
            setNFlag(false);
            setHFlag(false);
            return 4;
        // LD r, r' instructions (0x40-0x7F except 0x76, 0x77) - Factorized
        default:
            if ((opcode >= 0x40 && opcode <= 0x75) || (opcode >= 0x78 && opcode <= 0x7F))
            {
                uint8_t dst = (opcode >> 3) & 0x07;
                uint8_t src = opcode & 0x07;

                if (dst == 6)
                { // LD (HL), r
                    if (src == 6)
                        break; // Invalid: LD (HL), (HL)
                    mmu.write(getHL(), *getReg(src));
                    return 8;
                }
                else if (src == 6)
                { // LD r, (HL)
                    *getReg(dst) = mmu.read(getHL());
                    return 8;
                }
                else
                { // LD r, r'
                    *getReg(dst) = *getReg(src);
                    return 4;
                }
            }
            break;
        case 0x76: // HALT
        {
            uint8_t ie = mmu.read(memory::IE_REGISTER);
            uint8_t if_ = mmu.read(memory::IF_REGISTER);

            if (!ime_enabled && (ie & if_))
            {
                // HALT bug
                halt_bug = true;
                cpu_stopped = false;
            }
            else
            {
                cpu_stopped = true;
            }
        }
            return 4;

        case 0x77: // LD (HL), A
            mmu.write(getHL(), a);
            return 8;
        // ADD A, r / ADC A, r (0x80-0x8F) - Factorized
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8F:
        {
            uint8_t reg_idx = opcode & 0x07;
            bool use_carry = (opcode & 0x08) != 0;
            doAdd(*getReg(reg_idx), use_carry);
            return 4;
        }
        case 0x86: // ADD A, (HL)
            doAdd(mmu.read(getHL()), false);
            return 8;
        case 0x8E: // ADC A, (HL)
            doAdd(mmu.read(getHL()), true);
            return 8;
        // SUB r / SBC A, r (0x90-0x9F) - Factorized
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x97:
        case 0x98:
        case 0x99:
        case 0x9A:
        case 0x9B:
        case 0x9C:
        case 0x9D:
        case 0x9F:
        {
            uint8_t reg_idx = opcode & 0x07;
            bool use_carry = (opcode & 0x08) != 0;
            doSub(*getReg(reg_idx), use_carry);
            return 4;
        }
        case 0x96: // SUB (HL)
            doSub(mmu.read(getHL()), false);
            return 8;
        case 0x9E: // SBC A, (HL)
            doSub(mmu.read(getHL()), true);
            return 8;
        // AND r (0xA0-0xA7) - Factorized
        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA3:
        case 0xA4:
        case 0xA5:
        case 0xA7:
        {
            uint8_t reg_idx = opcode & 0x07;
            doAnd(*getReg(reg_idx));
            return 4;
        }
        case 0xA6: // AND (HL)
            doAnd(mmu.read(getHL()));
            return 8;
        // XOR r (0xA8-0xAF) - Factorized
        case 0xA8:
        case 0xA9:
        case 0xAA:
        case 0xAB:
        case 0xAC:
        case 0xAD:
        case 0xAF:
        {
            uint8_t reg_idx = opcode & 0x07;
            doXor(*getReg(reg_idx));
            return 4;
        }
        case 0xAE: // XOR (HL)
            doXor(mmu.read(getHL()));
            return 8;
        // OR r (0xB0-0xB7) - Factorized
        case 0xB0:
        case 0xB1:
        case 0xB2:
        case 0xB3:
        case 0xB4:
        case 0xB5:
        case 0xB7:
        {
            uint8_t reg_idx = opcode & 0x07;
            doOr(*getReg(reg_idx));
            return 4;
        }
        case 0xB6: // OR (HL)
            doOr(mmu.read(getHL()));
            return 8;
        // CP r (0xB8-0xBF) - Factorized
        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBF:
        {
            uint8_t reg_idx = opcode & 0x07;
            doCp(*getReg(reg_idx));
            return 4;
        }
        case 0xBE: // CP (HL)
            doCp(mmu.read(getHL()));
            return 8;
        case 0xC0: // RET NZ
            if (!readZFlag())
            {
                pc = mmu.read16(sp);
                sp += 2;
                return 20;
            }
            return 8;
        case 0xC1: // POP BC
            setBC(mmu.read16(sp));
            sp += 2;
            return 12;
        case 0xC2: // JP NZ, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (!readZFlag())
            {
                pc = addr;
                return 16;
            }
            return 12;
        }
        case 0xC3: // JP a16
        {
            uint16_t addr = mmu.read16(pc);
            pc = addr;
        }
            return 16;
        case 0xC4: // CALL NZ, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (!readZFlag())
            {
                sp -= 2;
                mmu.write16(sp, pc);
                pc = addr;
                return 24;
            }
            return 12;
        }
        case 0xC5: // PUSH BC
            sp -= 2;
            mmu.write16(sp, getBC());
            return 16;
        case 0xC6: // ADD A, d8
            setHFlag(((a & 0x0F) + (mmu.read(pc) & 0x0F)) > 0x0F);
            setCFlag(((uint16_t)a + (uint16_t)mmu.read(pc)) > 0xFF);
            {
                uint8_t value = mmu.read(pc++);
                a += value;
            }
            setZFlag(a == 0);
            setNFlag(false);
            return 8;
        case 0xC7: // RST 00H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x00;
            return 16;
        case 0xC8: // RET Z
            if (readZFlag())
            {
                pc = mmu.read16(sp);
                sp += 2;
                return 20;
            }
            return 8;
        case 0xC9: // RET
            pc = mmu.read16(sp);
            sp += 2;
            return 16;
        case 0xCA: // JP Z, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (readZFlag())
            {
                pc = addr;
                return 16;
            }
            return 12;
        }
        case 0xCB: // Prefix for extended instructions
        {
            uint8_t ext_opcode = mmu.read(pc++);
            // Handle extended opcode
            return execute_extended(ext_opcode);
        }
        break;
        case 0xCC: // CALL Z, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (readZFlag())
            {
                sp -= 2;
                mmu.write16(sp, pc);
                pc = addr;
                return 24;
            }
            return 12;
        }
        case 0xCD: // CALL a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            sp -= 2;
            mmu.write16(sp, pc);
            pc = addr;
        }
            return 24;
        case 0xCE: // ADC A, d8
        {
            uint8_t carry_in = readCFlag() ? 1 : 0;
            setHFlag((a & 0x0F) + (mmu.read(pc) & 0x0F) + carry_in > 0x0F);
            setCFlag(((uint16_t)a + (uint16_t)mmu.read(pc) + carry_in) > 0xFF);
            {
                uint8_t value = mmu.read(pc++);
                a += value + carry_in;
            }
            setZFlag(a == 0);
            setNFlag(false);
        }
            return 8;
        case 0xCF: // RST 08H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x08;
            return 16;
        case 0xD0: // RET NC
            if (!readCFlag())
            {
                pc = mmu.read16(sp);
                sp += 2;
                return 20;
            }
            return 8;
        case 0xD1: // POP DE
            setDE(mmu.read16(sp));
            sp += 2;
            return 12;
        case 0xD2: // JP NC, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (!readCFlag())
            {
                pc = addr;
                return 16;
            }
            return 12;
        }
        case 0xD4: // CALL NC, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (!readCFlag())
            {
                sp -= 2;
                mmu.write16(sp, pc);
                pc = addr;
                return 24;
            }
            return 12;
        }
        case 0xD5: // PUSH DE
            sp -= 2;
            mmu.write16(sp, getDE());
            return 16;
        case 0xD6: // SUB d8
        {
            uint8_t value = mmu.read(pc++);
            setHFlag((a & 0x0F) < (value & 0x0F));
            setCFlag(a < value);
            a -= value;
        }
            setZFlag(a == 0);
            setNFlag(true);
            return 8;
        case 0xD7: // RST 10H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x10;
            return 16;
        case 0xD8: // RET C
            if (readCFlag())
            {
                pc = mmu.read16(sp);
                sp += 2;
                return 20;
            }
            return 8;
        case 0xD9: // RETI
            pc = mmu.read16(sp);
            sp += 2;
            ime_enabled = true;
            return 16;
        case 0xDA: // JP C, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (readCFlag())
            {
                pc = addr;
                return 16;
            }
            return 12;
        }
        case 0xDC: // CALL C, a16
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            if (readCFlag())
            {
                sp -= 2;
                mmu.write16(sp, pc);
                pc = addr;
                return 24;
            }
            return 12;
        }
        case 0xDE: // SBC A, d8
        {
            auto carry_in = readCFlag() ? 1 : 0;
            setHFlag((a & 0x0F) < (mmu.read(pc) & 0x0F) + carry_in);
            setCFlag(a < mmu.read(pc) + carry_in);
            {
                uint8_t value = mmu.read(pc++);
                a -= value + carry_in;
            }
            setZFlag(a == 0);
            setNFlag(true);
        }
            return 8;

        case 0xDF: // RST 18H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x18;
            return 16;
        case 0xE0: // LDH (0xFF00 + n), A
        {
            uint8_t offset = mmu.read(pc++);
            mmu.write(0xFF00 + offset, a);
        }
            return 12;
        case 0xE1: // POP HL
            setHL(mmu.read16(sp));
            sp += 2;
            return 12;
        case 0xE2: // LD (0xFF00 + C), A
            mmu.write(0xFF00 + c, a);
            return 8;
        case 0xE5: // PUSH HL
            sp -= 2;
            mmu.write16(sp, getHL());
            return 16;
        case 0xE6: // AND d8
        {
            uint8_t value = mmu.read(pc++);
            a &= value;
            setZFlag(a == 0);
            setNFlag(false);
            setHFlag(true);
            setCFlag(false);
        }
            return 8;
        case 0xE7: // RST 20H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x20;
            return 16;
        case 0xE8: // ADD SP, r8
        {
            int8_t n8 = static_cast<int8_t>(mmu.read(pc++));
            setHFlag(((sp & 0x0F) + (n8 & 0x0F)) > 0x0F);
            setCFlag(((sp & 0xFF) + (n8 & 0xFF)) > 0xFF);
            sp += n8;
            setNFlag(false);
            setZFlag(false);
        }
            return 16;
        case 0xE9: // JP (HL)
            pc = getHL();
            return 4;
        case 0xEA: // LD (a16), A
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            mmu.write(addr, a);
        }
            return 16;
        case 0xEE: // XOR d8
        {
            uint8_t value = mmu.read(pc++);
            a ^= value;
        }
            setZFlag(a == 0);
            setNFlag(false);
            setHFlag(false);
            setCFlag(false);
            return 8;
        case 0xEF: // RST 28H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x28;
            return 16;
        case 0xF0: // LDH A, (0xFF00 + n)
        {
            uint8_t offset = mmu.read(pc++);
            a = mmu.read(0xFF00 + offset);
        }
            return 12;
        case 0xF1: // POP AF
        {
            uint16_t value = mmu.read16(sp);
            a = (value >> 8) & 0xFF;
            f = value & 0xF0; // Lower nibble of F is always 0
            sp += 2;
        }
            return 12;
        case 0xF2: // LD A, (0xFF00 + C)
            a = mmu.read(0xFF00 + c);
            return 8;
        case 0xF3: // DI (Disable Interrupts immediately, no delay)
            ime_enabled = false;
            ei_pending = false; // Cancel any pending EI
            return 4;
        case 0xF5: // PUSH AF
        {
            uint16_t value = (a << 8) | (f & 0xF0); // Lower nibble of F is always 0
            sp -= 2;
            mmu.write16(sp, value);
        }
            return 16;
        case 0xF6: // OR d8
        {
            uint8_t value = mmu.read(pc++);
            a |= value;
        }
            setZFlag(a == 0);
            setNFlag(false);
            setHFlag(false);
            setCFlag(false);
            return 8;
        case 0xF7: // RST 30H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x30;
            return 16;
        case 0xF8: // LD HL, SP+r8
        {
            int8_t n8 = static_cast<int8_t>(mmu.read(pc++));
            setHL(sp + n8);
            setHFlag(((sp & 0x0F) + (n8 & 0x0F)) > 0x0F);
            setCFlag(((sp & 0xFF) + (n8 & 0xFF)) > 0xFF);
        }
            setZFlag(false);
            setNFlag(false);
            return 12;
        case 0xF9: // LD SP, HL
            sp = getHL();
            return 8;
        case 0xFA: // LD A, (a16)
        {
            uint16_t addr = mmu.read16(pc);
            pc += 2;
            a = mmu.read(addr);
        }
            return 16;
        case 0xFB: // EI (Enable Interrupts with 1-instruction delay)
            ei_pending = true;
            return 4;
        case 0xFE: // CP d8
        {
            uint8_t value = mmu.read(pc++);
            uint8_t result = a - value;
            setZFlag((result == 0));
            setNFlag(true);
            setHFlag((a & 0x0F) < (value & 0x0F));
            setCFlag(a < value);
        }
            return 8;
        case 0xFF: // RST 38H
            sp -= 2;
            mmu.write16(sp, pc);
            pc = 0x38;
            return 16;
        }

        return 4; // Default cycle count
    }

    /// @brief execute extended instruction based on extended opcode
    /// @param ext_opcode  the extended opcode to execute
    /// @return the number of cycles the instruction took
    uint8_t CPU::execute_extended(uint8_t ext_opcode)
    {
        // Helper lambda for getting register reference
        auto getReg = [&](uint8_t idx) -> uint8_t &
        {
            switch (idx)
            {
            case 0:
                return b;
            case 1:
                return c;
            case 2:
                return d;
            case 3:
                return e;
            case 4:
                return h;
            case 5:
                return l;
            case 7:
                return a;
            default:
                return b; // Should not happen
            }
        };

        uint8_t reg_idx = ext_opcode & 0x07;
        uint8_t bit_idx = (ext_opcode >> 3) & 0x07;

        // RLC r (0x00-0x07)
        if (ext_opcode <= 0x07)
        {
            if (reg_idx == 6)
            { // RLC (HL)
                uint8_t value = mmu.read(getHL());
                uint8_t carry = (value >> 7) & 1;
                value = (value << 1) | carry;
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 16;
            }
            else
            { // RLC r
                uint8_t &reg = getReg(reg_idx);
                uint8_t carry = (reg >> 7) & 1;
                reg = (reg << 1) | carry;
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 8;
            }
        }

        // RRC r (0x08-0x0F)
        if (ext_opcode >= 0x08 && ext_opcode <= 0x0F)
        {
            if (reg_idx == 6)
            { // RRC (HL)
                uint8_t value = mmu.read(getHL());
                uint8_t carry = value & 1;
                value = (value >> 1) | (carry << 7);
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 16;
            }
            else
            { // RRC r
                uint8_t &reg = getReg(reg_idx);
                uint8_t carry = reg & 1;
                reg = (reg >> 1) | (carry << 7);
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 8;
            }
        }

        // RL r (0x10-0x17)
        if (ext_opcode >= 0x10 && ext_opcode <= 0x17)
        {
            uint8_t old_carry = readCFlag() ? 1 : 0;
            if (reg_idx == 6)
            { // RL (HL)
                uint8_t value = mmu.read(getHL());
                uint8_t new_carry = (value >> 7) & 1;
                value = (value << 1) | old_carry;
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(new_carry);
                return 16;
            }
            else
            { // RL r
                uint8_t &reg = getReg(reg_idx);
                uint8_t new_carry = (reg >> 7) & 1;
                reg = (reg << 1) | old_carry;
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(new_carry);
                return 8;
            }
        }

        // RR r (0x18-0x1F)
        if (ext_opcode >= 0x18 && ext_opcode <= 0x1F)
        {
            uint8_t old_carry = readCFlag() ? 1 : 0;
            if (reg_idx == 6)
            { // RR (HL)
                uint8_t value = mmu.read(getHL());
                uint8_t new_carry = value & 1;
                value = (value >> 1) | (old_carry << 7);
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(new_carry);
                return 16;
            }
            else
            { // RR r
                uint8_t &reg = getReg(reg_idx);
                uint8_t new_carry = reg & 1;
                reg = (reg >> 1) | (old_carry << 7);
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(new_carry);
                return 8;
            }
        }

        // SLA r (0x20-0x27)
        if (ext_opcode >= 0x20 && ext_opcode <= 0x27)
        {
            if (reg_idx == 6)
            { // SLA (HL)
                uint8_t value = mmu.read(getHL());
                uint8_t carry = (value >> 7) & 1;
                value = value << 1;
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 16;
            }
            else
            { // SLA r
                uint8_t &reg = getReg(reg_idx);
                uint8_t carry = (reg >> 7) & 1;
                reg = reg << 1;
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 8;
            }
        }

        // SRA r (0x28-0x2F)
        if (ext_opcode >= 0x28 && ext_opcode <= 0x2F)
        {
            if (reg_idx == 6)
            { // SRA (HL)
                uint8_t value = mmu.read(getHL());
                uint8_t carry = value & 1;
                value = (value >> 1) | (value & 0x80); // Keep MSB
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 16;
            }
            else
            { // SRA r
                uint8_t &reg = getReg(reg_idx);
                uint8_t carry = reg & 1;
                reg = (reg >> 1) | (reg & 0x80); // Keep MSB
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 8;
            }
        }

        // SWAP r (0x30-0x37)
        if (ext_opcode >= 0x30 && ext_opcode <= 0x37)
        {
            if (reg_idx == 6)
            { // SWAP (HL)
                uint8_t value = mmu.read(getHL());
                value = ((value & 0x0F) << 4) | ((value & 0xF0) >> 4);
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(false);
                return 16;
            }
            else
            { // SWAP r
                uint8_t &reg = getReg(reg_idx);
                reg = ((reg & 0x0F) << 4) | ((reg & 0xF0) >> 4);
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(false);
                return 8;
            }
        }

        // SRL r (0x38-0x3F)
        if (ext_opcode >= 0x38 && ext_opcode <= 0x3F)
        {
            if (reg_idx == 6)
            { // SRL (HL)
                uint8_t value = mmu.read(getHL());
                uint8_t carry = value & 1;
                value = value >> 1;
                mmu.write(getHL(), value);
                setZFlag(value == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 16;
            }
            else
            { // SRL r
                uint8_t &reg = getReg(reg_idx);
                uint8_t carry = reg & 1;
                reg = reg >> 1;
                setZFlag(reg == 0);
                setNFlag(false);
                setHFlag(false);
                setCFlag(carry);
                return 8;
            }
        }

        // BIT b, r (0x40-0x7F)
        if (ext_opcode >= 0x40 && ext_opcode <= 0x7F)
        {
            uint8_t value;
            if (reg_idx == 6)
            { // BIT b, (HL)
                value = mmu.read(getHL());
                setZFlag((value & (1 << bit_idx)) == 0);
                setNFlag(false);
                setHFlag(true);
                return 12;
            }
            else
            { // BIT b, r
                value = getReg(reg_idx);
                setZFlag((value & (1 << bit_idx)) == 0);
                setNFlag(false);
                setHFlag(true);
                return 8;
            }
        }

        // RES b, r (0x80-0xBF)
        if (ext_opcode >= 0x80 && ext_opcode <= 0xBF)
        {
            if (reg_idx == 6)
            { // RES b, (HL)
                uint8_t value = mmu.read(getHL());
                value &= ~(1 << bit_idx);
                mmu.write(getHL(), value);
                return 16;
            }
            else
            { // RES b, r
                uint8_t &reg = getReg(reg_idx);
                reg &= ~(1 << bit_idx);
                return 8;
            }
        }

        // SET b, r (0xC0-0xFF)
        if (ext_opcode >= 0xC0)
        {
            if (reg_idx == 6)
            { // SET b, (HL)
                uint8_t value = mmu.read(getHL());
                value |= (1 << bit_idx);
                mmu.write(getHL(), value);
                return 16;
            }
            else
            { // SET b, r
                uint8_t &reg = getReg(reg_idx);
                reg |= (1 << bit_idx);
                return 8;
            }
        }

        // Unknown opcode
        return 8;
    }
}
