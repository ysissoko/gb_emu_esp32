# Game Boy / Game Boy Color Emulator – Technical Checklist

## 1. CPU (LR35902 + extensions GBC)
- [x] Full instruction set (DMG)
- [x] Correct instruction timing (instr_timing passes)
- [x] Double-speed mode (KEY1 register)
- [x] STOP behavior differs in GBC mode (STOP + KEY1 triggers speed switch; otherwise halts)
- [x] Speed switch handling (STOP + KEY1)
- [ ] HALT + interrupt edge cases (IME=0)

## 2. Boot ROM
- [x] DMG boot ROM implemented
- [x] CGB boot ROM skipped — post-boot register state initialized correctly instead
- [x] Disable boot ROM via FF50
- [x] Proper initial register state after boot (A=0x11 CGB / A=0x01 DMG)

## 3. Memory Bus (MMU)
- [x] MBC1 / MBC3 / MBC5
- [x] Correct ROM bank masking (bank % rom_bank_count)
- [x] SRAM enable/disable
- [x] GBC WRAM banking (8 banks @ D000–DFFF, SVBK 0xFF70)
- [x] GBC VRAM banking (2 banks @ 8000–9FFF, VBK 0xFF4F)
- [x] HDMA registers (FF51–FF55) — General DMA functional; HBlank DMA simplified

## 4. PPU (DMG + CGB)
- [x] DMG background rendering
- [x] DMG sprites
- [x] STAT interrupt timing
- [x] GBC color palettes (BG + OBJ) — BGPI/BGPD, OBPI/OBPD with auto-increment
- [x] GBC attribute map (tile attributes — palette, VRAM bank, H/V flip, BG priority)
- [x] VRAM bank switching support (tile data from bank 0 or 1 per tile)
- [ ] Proper window timing edge cases
- [ ] LY=LYC interrupt exact timing

## 5. DMA
- [x] OAM DMA (FF46)
- [ ] GBC HDMA (HBlank DMA) — proper per-HBlank state machine not yet implemented
- [x] GBC General DMA (FF55, bit7=0 — immediate copy)
- [ ] CPU stall during General/HBlank DMA

## 6. Timer
- [x] Cycle-accurate DIV/TIMA
- [x] Correct TAC frequencies
- [x] TIMA overflow delay
- [x] Double-speed impact on timer (timer receives half cycles in double-speed mode)

## 7. Interrupts
- [x] IF/IE behavior correct
- [x] Interrupt priority order
- [ ] STAT interrupt conditions (mode-specific)
- [ ] Interrupt timing vs instruction boundary

## 8. Input (Joypad)
- [x] FF00 behavior correct
- [x] Interrupt on edge transition
- [ ] GBC compatibility verified (requires hardware testing)

## 9. APU (Audio)
- [ ] Stub reads return correct values
- [ ] Registers readable/writable without crashing
- [ ] (Optional) Audio generation later

## 10. Save System
- [x] SRAM persistence
- [x] Battery-backed saves
- [ ] RTC latch edge cases (MBC3)
- [ ] No save access during DMA

## 11. Performance (ESP32-S3)
- [x] Frame queue decoupled
- [x] PPU runs per-cycle, not per-frame
- [x] GBC double-speed implemented (2× CPU cycles/frame, half cycles to PPU/timer)
- [ ] IRAM hot paths (CPU, PPU inner loops)
- [ ] SPI DMA for LCD

## 12. Validation
- [x] cpu_instr
- [x] instr_timing
- [ ] dmg_acid2
- [ ] cgb_acid2
- [ ] Pokémon GBC boot & save
