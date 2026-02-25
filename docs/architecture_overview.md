# Game Boy Emulator Architecture (ESP32-S3)

## Introduction

This document describes the architecture of a Game Boy (DMG) emulator implemented on the ESP32-S3 microcontroller. The emulator aims to run original Game Boy games with high accuracy, focusing on cycle-accurate emulation of the Sharp SM83 CPU, PPU (Picture Processing Unit), and supporting hardware components. The implementation is optimized for embedded constraints, using FreeRTOS for multitasking and SPI-based peripherals.

The codebase is written in C++ using ESP-IDF framework, with key components including CPU emulation, graphics rendering, memory management, and input handling.

## System Overview

The emulator runs as a FreeRTOS task on the ESP32-S3, managing two main states: MENU (ROM selection via SD card) and RUNNING_GAME (emulation loop). The system achieves ~59.7 FPS for 160x144 pixel rendering, upscaled to an ST7789V LCD.

Key features:
- **Cycle-accurate CPU**: SM83 implementation with interrupt handling.
- **PPU Rendering**: Scanline-based graphics with background, window, and sprite layers.
- **Memory Management**: MBC support for banking, boot ROM simulation.
- **I/O Devices**: Timer, joypad, serial, APU (stub), save/load system.
- **Hardware**: SPI LCD, SD card, 8-button input, battery-powered.

## Software Architecture

### Core Components

#### 1. Emulator (emulator.hpp/cpp)
- **Role**: Main application controller, manages app states, ROM loading, and task creation.
- **Key Classes**: `Emulator` class, handles initialization, menu loop, and emulation task.
- **Interactions**: Instantiates CPU, PPU, MemoryBus, Joypad, LCDDisplay.

#### 2. CPU (cpu.hpp/cpp)
- **Role**: Emulates the Sharp SM83 processor, executes instructions cycle-by-cycle.
- **Key Features**:
  - Full instruction set (base and extended opcodes).
  - Interrupt handling (VBLANK, STAT, TIMER, JOYPAD, SERIAL).
  - DMA support for OAM transfer.
  - Halt/Stop states, EI delay.
- **State**: Registers (AF, BC, DE, HL, SP, PC), IME, DMA in progress.
- **Interactions**: Reads/writes via MemoryBus, triggers PPU steps.

#### 3. PPU (ppu.hpp/cpp)
- **Role**: Handles graphics rendering, manages modes (OAM_SCAN, DRAWING, HBLANK, VBLANK).
- **Key Features**:
  - Scanline-based rendering (456 cycles per line).
  - Background/window/sprites with palettes.
  - STAT interrupts (edge-triggered).
  - Asynchronous rendering task for LCD output.
- **State**: Mode, LY/LYC, window counter, framebuffer in RGB565.
- **Interactions**: Accesses VRAM/OAM via MemoryBus, queues frames for display.

#### 4. MemoryBus (memory_bus.hpp/cpp)
- **Role**: Central memory manager, handles address mapping and I/O.
- **Key Features**:
  - Boot ROM support (DMG 256-byte ROM).
  - MBC1/3/5 for ROM/RAM banking.
  - VRAM/OAM protection during PPU Mode 3.
  - Save/load for battery-backed RAM.
- **Mappings**: ROM (0x0000-0x7FFF), VRAM (0x8000-0x9FFF), WRAM (0xC000-0xDFFF), OAM (0xFE00-0xFE9F), I/O (0xFF00-0xFFFF).
- **Interactions**: Accessed by CPU/PPU for all memory operations.

#### 5. Timer (timer.hpp/cpp)
- **Role**: Emulates Game Boy timer (DIV, TIMA, TMA, TAC).
- **Key Features**: Falling edge detection, overflow handling, interrupt generation.
- **Interactions**: Steps via CPU cycles, writes to MemoryBus.

#### 6. Joypad (joypad.hpp/cpp)
- **Role**: Handles button input (A/B/START/SELECT/UP/DOWN/LEFT/RIGHT).
- **Key Features**: Debounced input, interrupt on press.
- **Interactions**: Polled in emulation loop, writes to MemoryBus.

#### 7. APU (apu.hpp/cpp)
- **Role**: Audio processing unit (currently stub, no sound output).
- **Key Features**: Registers for channels (future implementation).
- **Interactions**: Steps via CPU, writes to MemoryBus.

#### 8. Serial (serial.hpp/cpp)
- **Role**: Serial communication (link cable emulation, stub).
- **Key Features**: SB/SC registers, interrupt on transfer.
- **Interactions**: Steps via CPU, writes to MemoryBus.

#### 9. LCDDisplay (lcd_display.hpp/cpp)
- **Role**: Interfaces with ST7789V LCD via SPI.
- **Key Features**: Frame rendering, RGB565 to BGR565 conversion.
- **Interactions**: Receives frames from PPU async task.

#### 10. Menu (menu.hpp/cpp)
- **Role**: ROM selection interface.
- **Key Features**: File listing, navigation, rendering.
- **Interactions**: Uses LCDDisplay, Joypad.

#### 11. Storage/SaveManager (storage.hpp/cpp, save_manager.hpp/cpp)
- **Role**: SD card management, save/load states.
- **Key Features**: FAT32 filesystem, async saves.
- **Interactions**: Used by MemoryBus for SRAM persistence.

### Data Flow

1. **Initialization**: Emulator creates components, sets up FreeRTOS tasks.
2. **ROM Loading**: Menu selects ROM, loads into MemoryBus, enables boot ROM if needed.
3. **Emulation Loop**:
   - CPU executes instructions, steps PPU/Timer/etc.
   - PPU renders scanlines, triggers STAT interrupts.
   - MemoryBus handles reads/writes, MBC banking.
   - Frame ready → Async render to LCD.
4. **Input/Output**: Joypad polls buttons, generates interrupts; Serial/APU for future features.
5. **Save/Load**: Periodic async saves to SD card.

### Error Handling and Logging
- ESP-IDF logging for debug (e.g., CPU blocked detection).
- Exceptions avoided; error codes returned.
- Watchdog disabled for menu/emulation tasks.

## Hardware Architecture

Based on the PCB design (README_PCB_Design.md), the system uses ESP32-S3-WROOM-1 with:
- **Microcontroller**: ESP32-S3 with 16MB flash/PSRAM.
- **Display**: ST7789V SPI LCD (240x240, upscaled output).
- **Storage**: SD card via SPI (ROMs, saves).
- **Input**: 8 tactile buttons (GPIO1-7,15).
- **Power**: LiPo battery (3.7V), USB charging/recharge.
- **Indicators**: RGB LED for battery status, ON/OFF LED.
- **Extras**: ADC for battery level, UART for debug, USB for flashing.
- **GPIO Mapping**: SPI shared (CLK12, MOSI11, MISO13), LCD CS9/DC10/RST8/BL7, SD CS14, etc.

## CGB (Game Boy Color) Support

CGB support was added without breaking any DMG functionality. All CGB code paths are guarded by a `cgb_mode` flag detected from the ROM header byte `0x0143` (`0x80` = CGB-compatible, `0xC0` = CGB-only).

### Implemented
- **Mode detection**: `MemoryBus::loadROM()` reads `0x0143`, sets `cgb_mode`, propagates to PPU
- **VRAM banking** (`VBK` 0xFF4F): second 8KB VRAM bank in PSRAM, selected per tile in PPU
- **WRAM banking** (`SVBK` 0xFF70): 6 extra 4KB WRAM banks (2–7) in PSRAM for `0xD000–0xDFFF`
- **CGB palette RAM**: `BGPI/BGPD` (0xFF68/69), `OBPI/OBPD` (0xFF6A/6B) – 8 BG + 8 OBJ palettes
- **HDMA** (0xFF51–FF55): General-purpose DMA implemented; HBlank DMA simplified
- **PPU CGB rendering**: tile attributes from VRAM bank 1 (palette, H/V flip, VRAM bank, BG priority), 15-bit color conversion to BGR565
- **Double-speed mode** (`KEY1` 0xFF4D): STOP + KEY1 bit 0 toggles speed; CPU runs 2× cycles/frame; PPU/timer receive half cycles

### Remaining
- CGB boot ROM (Phase 6)
- Proper per-HBlank DMA state machine (Phase 5)
- `dmg_acid2` / `cgb_acid2` test ROM validation

## Future Plans

### Other Enhancements
- APU implementation for sound.
- Link cable (serial) for multiplayer.
- Wi-Fi ROM loading (via ESP32 capabilities).
- PCB prototyping and testing.

This architecture ensures accurate DMG emulation while preparing for CGB expansion.