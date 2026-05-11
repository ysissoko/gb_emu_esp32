# GB ESP32 Emulator

A Game Boy (DMG) and Game Boy Color (CGB) emulator running on the **ESP32-S3** microcontroller, built with ESP-IDF and FreeRTOS. The project includes a custom PCB design and achieves ~59.7 FPS with cycle-accurate emulation on embedded hardware.

---

## Features

### Emulation Core
- **Cycle-accurate SM83 CPU** — full base instruction set + CB-prefixed opcodes, correct flag behavior, HALT bug
- **Interrupt system** — VBLANK, STAT, TIMER, SERIAL, JOYPAD with proper IME/EI delay semantics
- **DMA** — OAM DMA (640 T-cycles), General DMA, and HDMA (HBlank-triggered 16-byte blocks)
- **Timer** — DIV, TIMA/TMA/TAC with falling-edge detection and 4-M-cycle overflow delay
- **Scanline PPU** — background, window, and sprite (up to 10/scanline) layers; STAT interrupts edge-triggered on LY/LYC comparison
- **Boot ROM** — 256-byte DMG boot ROM execution

### Game Boy Color (CGB)
- Mode detection via cartridge header byte `0x0143`
- VRAM banking (`VBK` 0xFF4F) — 2 × 8 KB banks in PSRAM
- WRAM banking (`SVBK` 0xFF70) — banks 2–7 in PSRAM
- CGB palette RAM — 8 BG + 8 OBJ palettes via `BGPI/BGPD`, `OBPI/OBPD`
- Tile attributes from VRAM bank 1 (palette index, H/V flip, VRAM bank select, BG-to-OBJ priority)
- 15-bit RGB color with GBC color correction (Gambatte-style formula)
- Double-speed CPU mode (`KEY1` 0xFF4D) — 2× CPU cycles, PPU/timer at 1×

### Cartridge Support (MBC)
| MBC | ROM Banks | RAM Banks | Notes |
|-----|-----------|-----------|-------|
| MBC1 | 32 | 4 | 512 KB ROM / 32 KB RAM |
| MBC2 | 16 | — | 512 × 4-bit internal RAM |
| MBC3 | 128 | 4 | + Real-Time Clock (RTC) |
| MBC5 | 512 | 16 | Most GBC games |

Extended ROM banks (>32 KB) are stored in PSRAM.

### Display
- **ST7789V** 240×240 SPI LCD at 80 MHz
- Game Boy 160×144 output upscaled to 240×240 (1.5×, centered)
- RGB565 framebuffer → BGR565 conversion for panel wiring
- Chunked DMA transfers (48-line chunks, 3 transfers per frame)
- Asynchronous rendering task — display I/O never blocks the emulation loop

### Storage & Saves
- SD card (FAT32, SPI-shared bus) for ROMs and save files
- `.sav` files for battery-backed SRAM, auto-saved every ~16 frames
- RTC persistence across sessions (MBC3)
- Async save task pinned to Core 0

### Input
- 8 tactile buttons — A, B, SELECT, START, UP, DOWN, LEFT, RIGHT
- Active-LOW GPIO with internal pull-ups and 20 ms debounce
- Joypad interrupt on key press

### Menu
- ROM browser with scrollable list (up to 64 entries)
- Text rendering on LCD, joypad navigation
- Menu framebuffer (~150 KB) freed before ROM loading

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         Emulator                                 │
│              State machine: MENU ↔ RUNNING_GAME                  │
│         Task scheduling, ROM loading, component wiring           │
└───────────────────┬──────────────────────────────────────────────┘
                    │
       ┌────────────┼────────────┐
       │            │            │
┌──────▼──────┐ ┌───▼──────┐ ┌──▼──────────┐
│    CPU      │ │   PPU    │ │  MemoryBus  │
│  (SM83)     │ │ Scanline │ │   (MMU)     │
│  Executes   │ │ renderer │ │  Banking    │
│  instrs     │ │ 456 cy/  │ │  I/O mux   │
│  ~60 FPS    │ │ scanline │ │  MBC 1/3/5 │
└──────┬──────┘ └───┬──────┘ └──┬──────────┘
       │            │            │
       └────────────┼────────────┘
                    │
     ┌──────────────┼──────────────────┐
     │              │                  │
┌────▼───┐    ┌─────▼────┐    ┌───────▼────┐
│ Timer  │    │  Joypad  │    │  APU/Ser.  │
│DIV/TIMA│    │8 buttons │    │  (stub)    │
└────────┘    └──────────┘    └────────────┘

Display pipeline:
PPU framebuffer ──► Async render task ──► ST7789V (DMA, 3 chunks)

Storage:
SD card (FAT32) ──► ROM loading / .sav files ──► async save task
```

### Component Breakdown

| Component | File | Responsibility |
|-----------|------|----------------|
| Emulator | `emulator.cpp` | App states, task creation, ROM lifecycle |
| CPU | `cpu.cpp` (1 654 lines) | SM83 instruction execution, interrupts, DMA |
| PPU | `ppu.cpp` (754 lines) | Scanline rendering, STAT, VBLANK, CGB palettes |
| MemoryBus | `memory_bus.cpp` (1 143 lines) | Address decoding, MBC, PSRAM banking, I/O registers |
| Timer | `timer.cpp` | DIV/TIMA/TMA/TAC, overflow delay |
| Joypad | `joypad.hpp` | GPIO debounce, P14/P15 multiplexed reads |
| LCD Display | `lcd_display.cpp` | ST7789V SPI + DMA, RGB565 output |
| Storage | `storage.cpp` | SD card, FAT32, ROM enumeration |
| Save Manager | `save_manager.cpp` | SRAM persistence, async write to `.sav` |
| Menu | `menu.cpp` | ROM browser UI, framebuffer rendering |
| APU | `apu.cpp` | Stub — register map only |
| Serial | `serial.cpp` | Stub — SB/SC, debug accumulation |

---

## Memory Map

| Address | Region |
|---------|--------|
| `0x0000–0x00FF` | Boot ROM (DMG, 256 B; disabled after boot) |
| `0x0000–0x3FFF` | ROM Bank 0 (fixed) |
| `0x4000–0x7FFF` | ROM Bank N (MBC-switched, in PSRAM) |
| `0x8000–0x9FFF` | VRAM (8 KB; bank 0/1 in CGB via PSRAM) |
| `0xA000–0xBFFF` | External RAM (MBC; up to 32 KB in PSRAM) |
| `0xC000–0xCFFF` | WRAM Bank 0 |
| `0xD000–0xDFFF` | WRAM Bank 1 (banks 2–7 in CGB via PSRAM) |
| `0xE000–0xFDFF` | Echo RAM (mirrors 0xC000–0xDDFF) |
| `0xFE00–0xFE9F` | OAM (160 B, sprite attributes) |
| `0xFF00–0xFF7F` | I/O Registers |
| `0xFF80–0xFFFE` | HRAM (127 B) |
| `0xFFFF` | IE register |

---

## Hardware

### Microcontroller — ESP32-S3-WROOM-1
- Dual-core Xtensa LX7 at 240 MHz
- 512 KB internal SRAM + 8 MB PSRAM (extended ROM/RAM banks, CGB extras)
- 16 MB Flash

### Pin Mapping

| Function | GPIO |
|----------|------|
| **Joypad** | |
| A | 5 |
| B | 6 |
| UP | 15 |
| DOWN | 16 |
| RIGHT | 17 |
| LEFT | 18 |
| START | 21 |
| SELECT | 47 |
| **SPI Bus (shared)** | |
| MOSI | 11 |
| MISO | 13 |
| SCK | 12 |
| **ST7789V LCD** | |
| CS | 10 |
| RST | 8 |
| DC | 9 |
| Backlight | 7 |
| **SD Card** | |
| CS | 4 |
| **Battery ADC** | 38 |
| **Status LEDs** | 41, 42 |
| **USB** | 19 (D−), 20 (D+) |

### PCB
A custom KiCad board design is included in `main_board/` with JLCPCB production files. The board integrates the ESP32-S3, ST7789V LCD connector, SD slot, LiPo charging circuit, 8 tactile buttons, RGB LED, and USB-C port.

---

## Performance Optimizations

- **IRAM placement** (`IRAM_ATTR`) on all hot-path functions (CPU execute, PPU step, memory read/write) — avoids flash cache misses on the Xtensa core
- **Branch prediction hints** (`LIKELY` / `UNLIKELY` macros) throughout instruction dispatch
- **Async PPU rendering** — PPU queues completed framebuffers; a dedicated FreeRTOS task drives the LCD, so display I/O never stalls the emulation loop
- **Chunked DMA** — 48-line (23 KB) buffers sent in 3 transfers; DMA completion semaphore prevents tearing
- **Frame skipping** — dynamically skips PPU rendering when the CPU falls behind
- **Compiler flags** — `-O3 -ffast-math -funroll-loops` (see `CMakeLists.txt` / `optimize.sh`)
- **ScanlineContext** struct — caches frequently-read PPU registers for each scanline to avoid repeated MMU calls

---

## Build

### Prerequisites
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/) v5.x
- ESP32-S3 target

### Steps

```bash
# Configure target
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

### SD Card Setup
Format the SD card as FAT32 and place ROM files (`.gb` / `.gbc`) in a `roms/` folder at the root.

---

## Project Structure

```
gb_esp32_emulator/
├── main/                  # Emulator source (~6 800 lines of C++)
│   ├── emulator.cpp/hpp   # Main controller
│   ├── cpu.cpp/hpp        # SM83 CPU
│   ├── ppu.cpp/hpp        # PPU / renderer
│   ├── memory_bus.cpp/hpp # MMU / MBC
│   ├── timer.cpp/hpp      # Timer
│   ├── joypad.hpp         # Input
│   ├── lcd_display.cpp/hpp# ST7789V driver
│   ├── storage.cpp/hpp    # SD card
│   ├── save_manager.cpp/hpp
│   ├── menu.cpp/hpp       # ROM browser UI
│   ├── apu.cpp/hpp        # Audio (stub)
│   ├── serial.cpp/hpp     # Serial (stub)
│   ├── gpio_pins.hpp      # Pin definitions
│   └── spi.hpp            # Shared SPI init
├── main_board/            # KiCad PCB design + JLCPCB files
├── docs/                  # Architecture notes, CGB refactor plan
├── pandocs_sources/       # Game Boy hardware reference
├── CMakeLists.txt
└── sdkconfig              # ESP-IDF build configuration
```

---

## Roadmap

- [ ] APU — 4-channel audio output
- [ ] Link cable / serial multiplayer
- [ ] CGB boot ROM
- [ ] Per-HBlank HDMA state machine
- [ ] Wi-Fi ROM loading via ESP32 network stack
- [ ] `dmg_acid2` / `cgb_acid2` test ROM validation

---

## References

- [Pan Docs](https://gbdev.io/pandocs/) — Game Boy hardware documentation
- [Game Boy CPU Manual](http://marc.rawer.de/Gameboy/Docs/GBCPUman.pdf)
- [Gambatte](https://github.com/sinamas/gambatte) — color correction formula
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/)
