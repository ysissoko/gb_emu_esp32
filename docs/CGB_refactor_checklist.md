# Game Boy / Game Boy Color Emulator – Technical Checklist

## 1. CPU (LR35902 + extensions GBC)
- [x] Full instruction set (DMG)
- [x] Correct instruction timing (instr_timing passes)
- [ ] Double-speed mode (KEY1 register)
- [ ] STOP behavior differs in GBC mode
- [ ] Speed switch handling (STOP + KEY1)
- [ ] HALT + interrupt edge cases (IME=0)

## 2. Boot ROM
- [x] DMG boot ROM implemented
- [ ] CGB boot ROM support (0x200 bytes)
- [ ] Disable boot ROM via FF50
- [ ] Proper initial register state after boot

## 3. Memory Bus (MMU)
- [x] MBC1 / MBC3 / MBC5
- [x] Correct ROM bank masking (bank % rom_bank_count)
- [x] SRAM enable/disable
- [ ] GBC WRAM banking (8 banks @ D000–DFFF)
- [ ] GBC VRAM banking (2 banks @ 8000–9FFF)
- [ ] HDMA registers (FF51–FF55)

## 4. PPU (DMG + CGB)
- [x] DMG background rendering
- [x] DMG sprites
- [x] STAT interrupt timing
- [ ] GBC color palettes (BG + OBJ)
- [ ] GBC attribute map (tile attributes)
- [ ] VRAM bank switching support
- [ ] Proper window timing edge cases
- [ ] LY=LYC interrupt exact timing

## 5. DMA
- [x] OAM DMA (FF46)
- [ ] GBC HDMA (HBlank DMA)
- [ ] GBC General DMA
- [ ] CPU stall during DMA

## 6. Timer
- [x] Cycle-accurate DIV/TIMA
- [x] Correct TAC frequencies
- [x] TIMA overflow delay
- [ ] Double-speed impact on timer

## 7. Interrupts
- [x] IF/IE behavior correct
- [x] Interrupt priority order
- [ ] STAT interrupt conditions (mode-specific)
- [ ] Interrupt timing vs instruction boundary

## 8. Input (Joypad)
- [x] FF00 behavior correct
- [x] Interrupt on edge transition
- [ ] GBC compatibility verified

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
- [x] Frame queue decoupledss
- [x] PPU runs per-cycle, not per-frame
- [ ] GBC double-speed optimized
- [ ] IRAM hot paths (CPU, PPU inner loops)
- [ ] SPI DMA for LCD

## 12. Validation
- [x] cpu_instr
- [x] instr_timing
- [ ] dmg_acid2
- [ ] cgb_acid2
- [ ] Pokémon GBC boot & save
