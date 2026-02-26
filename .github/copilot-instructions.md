# GitHub Copilot Instructions – GB/GBC ESP32 Emulator

## Project Overview

This is a **Game Boy (DMG) and Game Boy Color (CGB) emulator** running on the **ESP32-S3** microcontroller, built with **ESP-IDF** (C++17) and **FreeRTOS**. The emulator targets cycle-accurate emulation of the Sharp SM83 CPU, PPU, and supporting hardware at ~59.7 FPS on a custom PCB with an SPI ST7789V LCD and SD card ROM storage.

---

## Technology Stack

- **Language**: C++17
- **Framework**: ESP-IDF (Espressif IoT Development Framework)
- **RTOS**: FreeRTOS (multi-core, ESP32-S3 dual-core Xtensa LX7)
- **Target MCU**: ESP32-S3-WROOM-1 (240 MHz, 16 MB flash, 16 MB PSRAM octal)
- **Build system**: CMake via `idf.py`
- **Display**: ST7789V SPI LCD (240×240), Game Boy output is 160×144 upscaled
- **Storage**: SD card via SPI (FAT32, for ROMs and save files)
- **Peripherals**: 8 tactile buttons (active LOW, internal pull-ups), battery ADC, status LED

---

## Repository Structure

```
main/
  main.cpp           – Entry point: creates Emulator, calls init() then run()
  emulator.hpp/cpp   – Top-level controller: AppState FSM, ROM loading, task creation
  cpu.hpp/cpp        – Sharp SM83 CPU: full instruction set, interrupts, DMA, double-speed
  ppu.hpp/cpp        – PPU: scanline renderer, OAM scan, STAT interrupts, async LCD output
  memory_bus.hpp/cpp – MMU: address decoding, MBC1/2/3/5, VRAM/WRAM banking, I/O registers
  timer.hpp/cpp      – DIV/TIMA/TMA/TAC timer with falling-edge detection
  joypad.hpp/cpp     – Button input with debounce and interrupt-on-edge
  apu.hpp/cpp        – APU stub (registers present, no audio output yet)
  serial.hpp/cpp     – Serial/link-cable stub (SB/SC registers)
  lcd_display.hpp/cpp– ST7789V SPI driver, RGB565→BGR565 conversion, frame push
  menu.hpp/cpp       – ROM selection UI (file listing, navigation, rendering)
  storage.hpp/cpp    – SD card init, file listing
  save_manager.hpp/cpp– Async battery-backed SRAM save/load (Core 0 FreeRTOS task)
  gpio_pins.hpp      – All GPIO assignments as typed constexpr constants
  spi.hpp            – SPI bus initialization helpers
  text_renderer.hpp  – Text rendering utility for menu/UI
docs/
  architecture_overview.md – Full architecture reference (keep in sync with code)
  CGB_refactor_plan        – Phase-by-phase CGB evolution plan
  CGB_refactor_checklist.md– Feature checklist (DMG/CGB, CPU/PPU/MMU/DMA/APU/saves)
  README_PCB_Design.md     – PCB BOM, GPIO mapping, schematic notes (French)
```

---

## Architecture

### Application State Machine

`Emulator` manages two states via `emulator::AppState`:
- **`MENU`** – ROM picker runs at ~20 FPS; uses `Menu`, `LCDDisplay`, `Joypad`
- **`RUNNING_GAME`** – Emulation loop runs at ~59.7 FPS (`FRAME_US = 16742 µs`); spawns a FreeRTOS task on Core 1

### Component Ownership

```
Emulator
 ├── unique_ptr<CPU>          (holds reference to MMU and PPU)
 ├── unique_ptr<PPU>          (holds reference to MMU and shared_ptr<LCDDisplay>)
 ├── unique_ptr<MemoryBus>    (owns Timer, APU, Serial; holds raw ptr to CPU and PPU)
 ├── shared_ptr<Joypad>       (shared between MemoryBus and Emulator)
 └── shared_ptr<LCDDisplay>   (shared between PPU and Menu)
```

Cross-references (`setCPU()`, `setPPU()`) are set after construction to resolve circular dependencies.

### Emulation Loop (Core 1)

```
CPU::run_frame()  (70224 cycles/frame, 2× in double-speed)
  └── CPU::step()
        ├── fetch opcode from MMU
        ├── CPU::execute() or execute_extended()
        ├── handle DMA stalls
        ├── PPU::step(cycles)        ← stepped inline per M-cycle
        ├── MemoryBus::stepTimer()
        └── test_interrupts_flags()
```

Frame timing is enforced by the `Emulator` using `esp_timer_get_time()` with a lag accumulator for dynamic frame skipping.

### PPU Pipeline (async rendering)

The PPU runs **synchronously** per CPU cycle for timing accuracy. When a full frame is complete (at VBlank), it queues the RGB565 framebuffer to a **FreeRTOS queue** (`frame_queue`). A separate `render_task` (Core 0) dequeues and pushes the frame to the LCD via SPI, decoupling render latency from emulation timing.

### Memory Map

| Range | Description |
|-------|-------------|
| `0x0000–0x00FF` | Boot ROM (DMG, 256 bytes, disabled after `0xFF50` write) |
| `0x0000–0x7FFF` | ROM (Bank 0 + switchable bank via MBC) |
| `0x8000–0x9FFF` | VRAM (8KB; CGB: bank 0 or 1 via `VBK 0xFF4F`) |
| `0xA000–0xBFFF` | External RAM / SRAM (MBC-controlled, battery-backed) |
| `0xC000–0xDFFF` | WRAM (8KB; CGB: bank 1–7 at `0xD000` via `SVBK 0xFF70`) |
| `0xE000–0xFDFF` | Echo RAM (mirrors WRAM) |
| `0xFE00–0xFE9F` | OAM (40 sprites × 4 bytes) |
| `0xFF00–0xFF7F` | I/O registers |
| `0xFF80–0xFFFE` | HRAM |
| `0xFFFF` | Interrupt Enable (IE) |

---

## CGB (Game Boy Color) Support

All CGB code paths are guarded by `cgb_mode` (set from ROM header byte `0x0143`):
- `0x80` = CGB-compatible (also runs on DMG)
- `0xC0` = CGB-only

### Implemented CGB Features
- **VRAM banking** (`VBK` 0xFF4F): second 8KB bank in PSRAM
- **WRAM banking** (`SVBK` 0xFF70): 6 extra 4KB banks (2–7) in PSRAM
- **CGB palette RAM**: `BGPI/BGPD` (0xFF68/69) and `OBPI/OBPD` (0xFF6A/6B); 8 BG + 8 OBJ palettes, auto-increment supported
- **HDMA** (0xFF51–0xFF55): General DMA (immediate + CPU stall) and per-HBlank 16-byte state machine
- **PPU CGB rendering**: tile attributes from VRAM bank 1 (palette, H/V flip, VRAM bank, BG priority), 15-bit → BGR565 conversion
- **Double-speed mode** (`KEY1` 0xFF4D): `STOP + KEY1` toggles 2× CPU speed; PPU/timer receive half cycles

### Not Yet Implemented
- CGB boot ROM (post-boot register state is manually initialized instead)
- `dmg_acid2` / `cgb_acid2` test ROM validation
- Proper STAT interrupt timing edge cases

---

## GPIO Pin Assignments

Defined in `gpio_pins.hpp` as `constexpr gpio_num_t` values (never use raw integers):

| Signal | GPIO |
|--------|------|
| SPI MOSI | 17 |
| SPI MISO | 16 |
| SPI SCK | 18 |
| LCD CS | 9 |
| LCD RST | 11 |
| LCD DC | 13 |
| LCD BL | 12 |
| SD CS | 10 |
| BTN RIGHT/LEFT/UP/DOWN | 1/2/3/4 |
| BTN A/B/SELECT/START | 5/6/7/15 |
| ADC Battery | 38 |
| Status LED | 21 |
| USB D-/D+ | 19/20 (reserved) |
| BOOT button | 0 (reserved) |

---

## Performance & Embedded Constraints

- **Hot paths** (`CPU::step`, `CPU::execute`, `PPU::step`, `renderBackground`, `renderWindow`, `renderSprites`) are annotated `IRAM_ATTR __attribute__((hot))` to place them in fast internal RAM.
- **Branch prediction**: `LIKELY(x)` / `UNLIKELY(x)` macros (`__builtin_expect`) used throughout CPU and PPU hot loops.
- **PSRAM** (16 MB) holds: extended ROM banks, external SRAM, CGB VRAM bank 1, CGB WRAM extra banks.
- **Internal RAM** holds: VRAM bank 0, WRAM bank 0, OAM, HRAM, I/O registers, framebuffer.
- **Framebuffer**: RGB565, allocated in internal RAM for DMA compatibility.
- **Watchdog**: disabled for emulation and menu tasks.
- **Avoid** `new`/`delete` in the hot path; prefer `std::array` over `std::vector` for fixed-size buffers.

---

## Coding Conventions

- Namespaces: `emulator`, `cpu`, `ppu`, `memory`, `display`, `controller`, `timer`, `serial`, `apu`, `gpio`
- All components live in `main/`; no subdirectory split yet (planned in `CGB_refactor_plan`)
- Prefer `std::unique_ptr` for owned components, `std::shared_ptr` for shared peripherals (LCD, Joypad)
- Logging: use `ESP_LOGI/LOGE/LOGD(TAG, ...)` with a per-file `static const char* TAG`
- Error handling: return `esp_err_t`, check with `ESP_ERROR_CHECK` or explicit `if (res != ESP_OK)`
- No exceptions; no dynamic allocation in hot loops
- All GPIO constants must come from `gpio_pins.hpp`

---

## Key Registers Quick Reference

| Register | Address | Description |
|----------|---------|-------------|
| LCDC | 0xFF40 | LCD Control |
| STAT | 0xFF41 | LCD Status / STAT interrupt |
| SCY/SCX | 0xFF42/43 | BG scroll Y/X |
| LY | 0xFF44 | Current scanline (read-only) |
| LYC | 0xFF45 | LY compare |
| DMA | 0xFF46 | OAM DMA trigger |
| BGP | 0xFF47 | DMG BG palette |
| OBP0/1 | 0xFF48/49 | DMG sprite palettes |
| WY/WX | 0xFF4A/4B | Window position |
| KEY1 | 0xFF4D | CGB speed switch |
| VBK | 0xFF4F | CGB VRAM bank select |
| HDMA1–5 | 0xFF51–55 | CGB DMA source/dest/control |
| BGPI/BGPD | 0xFF68/69 | CGB BG palette index/data |
| OBPI/OBPD | 0xFF6A/6B | CGB OBJ palette index/data |
| SVBK | 0xFF70 | CGB WRAM bank select |
| IF | 0xFF0F | Interrupt Flag |
| IE | 0xFFFF | Interrupt Enable |

---

## Build & Flash

```bash
idf.py build                  # Build
idf.py -p /dev/ttyUSB0 flash  # Flash to ESP32-S3
idf.py -p /dev/ttyUSB0 monitor# Serial monitor
```

CPU frequency and PSRAM mode are configured in `sdkconfig`. Target is `esp32s3`.

---

## Pending / Future Work

See `docs/CGB_refactor_checklist.md` for the canonical status of all features.

- [ ] APU audio generation (channels 1–4)
- [ ] Link cable / serial multiplayer
- [ ] SPI DMA for LCD transfers (Phase 7)
- [ ] `dmg_acid2` / `cgb_acid2` test ROM validation
- [ ] CGB boot ROM
- [ ] Wi-Fi ROM loading
