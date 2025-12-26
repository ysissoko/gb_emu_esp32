#pragma once

#include "display.hpp"

#include <cstdint>
#include <array>
#include <memory>

namespace memory {
    class MemoryBus;
}

namespace ppu
{
    // Game Boy LCD dimensions
    constexpr int TILE_SIZE = 8;
    constexpr int TILES_PER_ROW = 32;
    constexpr int TILES_PER_COL = 32;

    // LCD Control Register bits (LCDC - 0xFF40)
    constexpr uint8_t LCDC_BG_WINDOW_ENABLE = 0x01;
    constexpr uint8_t LCDC_OBJ_ENABLE = 0x02;
    constexpr uint8_t LCDC_OBJ_SIZE = 0x04;
    constexpr uint8_t LCDC_BG_TILE_MAP = 0x08;
    constexpr uint8_t LCDC_BG_WINDOW_TILES = 0x10;
    constexpr uint8_t LCDC_WINDOW_ENABLE = 0x20;
    constexpr uint8_t LCDC_WINDOW_TILE_MAP = 0x40;
    constexpr uint8_t LCDC_LCD_ENABLE = 0x80;

    // PPU modes
    enum class Mode : uint8_t
    {
        HBLANK = 0,
        VBLANK = 1,
        OAM_SCAN = 2,
        DRAWING = 3
    };

    // Sprite OAM entry (4 bytes per sprite)
    struct OAMEntry
    {
        uint8_t y;           // Y position - 16
        uint8_t x;           // X position - 8
        uint8_t tile_index;  // Tile number
        uint8_t attributes;  // Attributes/flags
    };

    // OAM attribute flags
    constexpr uint8_t OAM_PRIORITY = 0x80;    // 0: Sprite above BG, 1: BG colors 1-3 over sprite
    constexpr uint8_t OAM_Y_FLIP = 0x40;      // Vertical flip
    constexpr uint8_t OAM_X_FLIP = 0x20;      // Horizontal flip
    constexpr uint8_t OAM_PALETTE = 0x10;     // Palette number (0: OBP0, 1: OBP1)

    constexpr int MAX_SPRITES = 40;
    constexpr int MAX_SPRITES_PER_LINE = 10;

    class PPU
    {
    public:
        PPU(memory::MemoryBus&, std::unique_ptr<display::Display>);
        ~PPU();

        // Update PPU state for given number of cycles
        void step(uint8_t cycles);

        // Get framebuffer pointer for rendering
        const std::array<uint8_t, display::LCD_WIDTH * display::LCD_HEIGHT>& getFramebuffer() const { return framebuffer; }

        // Get current scanline
        uint8_t getCurrentLine() const { return ly; }

        // Check if frame is ready for display
        bool isFrameReady() const { return frame_ready; }
        void clearFrameReady() { frame_ready = false; }

    private:
        memory::MemoryBus& mmu;

        // Framebuffer: each byte represents a pixel (0-3 for the 4 gray shades)
        std::array<uint8_t, display::LCD_WIDTH  * display::LCD_HEIGHT> framebuffer;

        // PPU state
        Mode mode;
        uint16_t mode_cycles;
        uint8_t ly;  // Current scanline (0-153)
        bool frame_ready;
        uint8_t window_line_counter;  // Internal window line counter

        // Sprite buffer for current scanline
        std::array<OAMEntry, MAX_SPRITES_PER_LINE> visible_sprites;
        uint8_t visible_sprite_count;

        // Helper functions
        void renderScanline();
        void renderBackground();
        void renderWindow();
        void scanOAM();
        void renderSprites();
        void setMode(Mode new_mode);
        void updateLY(uint8_t new_ly);
        display::Color getPixelColor(uint8_t tile_index, uint8_t x, uint8_t y);

        std::unique_ptr<display::Display> display{nullptr};

        // Register read functions
        uint8_t readLCDC() const;
        uint8_t readSCY() const;
        uint8_t readSCX() const;
        uint8_t readBGP() const;   // Background palette
        uint8_t readWY() const;    // Window Y position
        uint8_t readWX() const;    // Window X position
        uint8_t readOBP0() const;  // Sprite palette 0
        uint8_t readOBP1() const;  // Sprite palette 1
    };
}
