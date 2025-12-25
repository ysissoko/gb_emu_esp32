#include "ppu.hpp"
#include "memory_bus.hpp"
#include <cstdio>
#include <cstring>

namespace ppu
{
    // Cycles per scanline mode
    constexpr uint16_t OAM_SCAN_CYCLES = 80;
    constexpr uint16_t DRAWING_CYCLES = 172;
    constexpr uint16_t HBLANK_CYCLES = 204;
    constexpr uint16_t SCANLINE_CYCLES = OAM_SCAN_CYCLES + DRAWING_CYCLES + HBLANK_CYCLES;
    constexpr uint16_t VBLANK_SCANLINES = 10;
    constexpr uint16_t TOTAL_SCANLINES = LCD_HEIGHT + VBLANK_SCANLINES;

    PPU::PPU(memory::MemoryBus& mmu)
        : mmu(mmu), mode(Mode::OAM_SCAN), mode_cycles(0), ly(0), frame_ready(false),
          window_line_counter(0), visible_sprite_count(0)
    {
        framebuffer.fill(0);
    }

    PPU::~PPU()
    {
    }

    void PPU::step(uint8_t cycles)
    {
        // Check if LCD is enabled
        if ((readLCDC() & LCDC_LCD_ENABLE) == 0)
        {
            return;
        }

        mode_cycles += cycles;

        switch (mode)
        {
        case Mode::OAM_SCAN:
            if (mode_cycles >= OAM_SCAN_CYCLES)
            {
                mode_cycles -= OAM_SCAN_CYCLES;
                scanOAM();  // Scan sprites for current scanline
                setMode(Mode::DRAWING);
            }
            break;

        case Mode::DRAWING:
            if (mode_cycles >= DRAWING_CYCLES)
            {
                mode_cycles -= DRAWING_CYCLES;
                renderScanline();
                setMode(Mode::HBLANK);
            }
            break;

        case Mode::HBLANK:
            if (mode_cycles >= HBLANK_CYCLES)
            {
                mode_cycles -= HBLANK_CYCLES;
                ly++;

                if (ly >= LCD_HEIGHT)
                {
                    // Enter VBlank
                    setMode(Mode::VBLANK);
                    frame_ready = true;
                    window_line_counter = 0;  // Reset window counter at VBlank
                }
                else
                {
                    // Next scanline
                    setMode(Mode::OAM_SCAN);
                }
            }
            break;

        case Mode::VBLANK:
            if (mode_cycles >= SCANLINE_CYCLES)
            {
                mode_cycles -= SCANLINE_CYCLES;
                ly++;

                if (ly >= TOTAL_SCANLINES)
                {
                    // Restart from top
                    ly = 0;
                    setMode(Mode::OAM_SCAN);
                }
            }
            break;
        }
    }

    void PPU::setMode(Mode new_mode)
    {
        mode = new_mode;
    }

    void PPU::renderScanline()
    {
        // Render background for current scanline
        renderBackground();

        // Render window (if enabled and visible on this scanline)
        renderWindow();

        // Render sprites (already scanned during OAM_SCAN mode)
        renderSprites();
    }

    void PPU::renderBackground()
    {
        uint8_t lcdc = readLCDC();

        // Check if background is enabled
        if ((lcdc & LCDC_BG_WINDOW_ENABLE) == 0)
        {
            // Background disabled, fill with white
            for (int x = 0; x < LCD_WIDTH; x++)
            {
                framebuffer[ly * LCD_WIDTH + x] = static_cast<uint8_t>(Color::WHITE);
            }
            return;
        }

        uint8_t scy = readSCY();
        uint8_t scx = readSCX();
        uint8_t bgp = readBGP();

        // Determine which tile map to use (0x9800 or 0x9C00)
        uint16_t tile_map_base = (lcdc & LCDC_BG_TILE_MAP) ? 0x9C00 : 0x9800;

        // Determine which tile data to use
        bool use_signed_tiles = (lcdc & LCDC_BG_WINDOW_TILES) == 0;
        uint16_t tile_data_base = use_signed_tiles ? 0x9000 : 0x8000;

        // Calculate Y position in the 256x256 background map
        uint8_t y_pos = (ly + scy) & 0xFF;
        uint8_t tile_y = y_pos / TILE_SIZE;
        uint8_t pixel_y = y_pos % TILE_SIZE;

        // Render each pixel in the scanline
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            // Calculate X position in the 256x256 background map
            uint8_t x_pos = (x + scx) & 0xFF;
            uint8_t tile_x = x_pos / TILE_SIZE;
            uint8_t pixel_x = x_pos % TILE_SIZE;

            // Get tile index from tile map
            uint16_t tile_map_addr = tile_map_base + (tile_y * TILES_PER_ROW) + tile_x;
            uint8_t tile_index = mmu.read(tile_map_addr);

            // Calculate tile data address
            uint16_t tile_addr;
            if (use_signed_tiles)
            {
                // Treat tile_index as signed
                int8_t signed_index = static_cast<int8_t>(tile_index);
                tile_addr = tile_data_base + (signed_index * 16);
            }
            else
            {
                tile_addr = tile_data_base + (tile_index * 16);
            }

            // Each tile is 16 bytes (8x8 pixels, 2 bits per pixel)
            // Each row of pixels takes 2 bytes
            uint16_t tile_row_addr = tile_addr + (pixel_y * 2);
            uint8_t byte1 = mmu.read(tile_row_addr);
            uint8_t byte2 = mmu.read(tile_row_addr + 1);

            // Get color bits for this pixel (bits are stored MSB first)
            uint8_t bit_pos = 7 - pixel_x;
            uint8_t color_bit_low = (byte1 >> bit_pos) & 1;
            uint8_t color_bit_high = (byte2 >> bit_pos) & 1;
            uint8_t color_id = (color_bit_high << 1) | color_bit_low;

            // Apply palette
            uint8_t palette_color = (bgp >> (color_id * 2)) & 0x03;

            // Write to framebuffer
            framebuffer[ly * LCD_WIDTH + x] = palette_color;
        }
    }

    uint8_t PPU::readLCDC() const
    {
        return mmu.read(0xFF40);
    }

    uint8_t PPU::readSCY() const
    {
        return mmu.read(0xFF42);
    }

    uint8_t PPU::readSCX() const
    {
        return mmu.read(0xFF43);
    }

    uint8_t PPU::readBGP() const
    {
        return mmu.read(0xFF47);
    }

    uint8_t PPU::readWY() const
    {
        return mmu.read(0xFF4A);
    }

    uint8_t PPU::readWX() const
    {
        return mmu.read(0xFF4B);
    }

    uint8_t PPU::readOBP0() const
    {
        return mmu.read(0xFF48);
    }

    uint8_t PPU::readOBP1() const
    {
        return mmu.read(0xFF49);
    }

    void PPU::renderWindow()
    {
        uint8_t lcdc = readLCDC();

        // Check if window is enabled
        if ((lcdc & LCDC_WINDOW_ENABLE) == 0)
        {
            return;
        }

        uint8_t wy = readWY();
        uint8_t wx = readWX();

        // Window is only displayed if WY <= LY and WX < 167
        if (wy > ly || wx >= 167)
        {
            return;
        }

        uint8_t bgp = readBGP();

        // Determine which tile map to use for window
        uint16_t tile_map_base = (lcdc & LCDC_WINDOW_TILE_MAP) ? 0x9C00 : 0x9800;

        // Determine which tile data to use
        bool use_signed_tiles = (lcdc & LCDC_BG_WINDOW_TILES) == 0;
        uint16_t tile_data_base = use_signed_tiles ? 0x9000 : 0x8000;

        // Calculate tile Y based on window internal line counter
        uint8_t tile_y = window_line_counter / TILE_SIZE;
        uint8_t pixel_y = window_line_counter % TILE_SIZE;

        // WX is offset by 7, so actual X start is WX - 7
        int window_x_start = wx - 7;

        // Render window pixels
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            // Check if this pixel is within the window area
            if (x < window_x_start)
            {
                continue;
            }

            // Calculate position in window
            int window_x = x - window_x_start;
            uint8_t tile_x = window_x / TILE_SIZE;
            uint8_t pixel_x = window_x % TILE_SIZE;

            // Get tile index from tile map
            uint16_t tile_map_addr = tile_map_base + (tile_y * TILES_PER_ROW) + tile_x;
            uint8_t tile_index = mmu.read(tile_map_addr);

            // Calculate tile data address
            uint16_t tile_addr;
            if (use_signed_tiles)
            {
                int8_t signed_index = static_cast<int8_t>(tile_index);
                tile_addr = tile_data_base + (signed_index * 16);
            }
            else
            {
                tile_addr = tile_data_base + (tile_index * 16);
            }

            // Get pixel data
            uint16_t tile_row_addr = tile_addr + (pixel_y * 2);
            uint8_t byte1 = mmu.read(tile_row_addr);
            uint8_t byte2 = mmu.read(tile_row_addr + 1);

            // Get color bits
            uint8_t bit_pos = 7 - pixel_x;
            uint8_t color_bit_low = (byte1 >> bit_pos) & 1;
            uint8_t color_bit_high = (byte2 >> bit_pos) & 1;
            uint8_t color_id = (color_bit_high << 1) | color_bit_low;

            // Apply palette
            uint8_t palette_color = (bgp >> (color_id * 2)) & 0x03;

            // Write to framebuffer
            framebuffer[ly * LCD_WIDTH + x] = palette_color;
        }

        // Increment window line counter (window is being rendered on this scanline)
        window_line_counter++;
    }

    void PPU::scanOAM()
    {
        uint8_t lcdc = readLCDC();

        // Reset sprite count
        visible_sprite_count = 0;

        // Check if sprites are enabled
        if ((lcdc & LCDC_OBJ_ENABLE) == 0)
        {
            return;
        }

        // Determine sprite height (8x8 or 8x16)
        uint8_t sprite_height = (lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

        // Scan OAM for sprites on current scanline
        // OAM is at 0xFE00-0xFE9F (40 sprites, 4 bytes each)
        for (int i = 0; i < MAX_SPRITES && visible_sprite_count < MAX_SPRITES_PER_LINE; i++)
        {
            uint16_t oam_addr = 0xFE00 + (i * 4);

            OAMEntry sprite;
            sprite.y = mmu.read(oam_addr);
            sprite.x = mmu.read(oam_addr + 1);
            sprite.tile_index = mmu.read(oam_addr + 2);
            sprite.attributes = mmu.read(oam_addr + 3);

            // Sprite coordinates are offset: Y-16, X-8
            int sprite_y = sprite.y - 16;

            // Check if sprite is on current scanline
            if (ly >= sprite_y && ly < sprite_y + sprite_height)
            {
                visible_sprites[visible_sprite_count] = sprite;
                visible_sprite_count++;
            }
        }
    }

    void PPU::renderSprites()
    {
        uint8_t lcdc = readLCDC();

        // Check if sprites are enabled
        if ((lcdc & LCDC_OBJ_ENABLE) == 0)
        {
            return;
        }

        // Determine sprite height
        uint8_t sprite_height = (lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

        // Render sprites in reverse order (lower index = higher priority)
        for (int i = visible_sprite_count - 1; i >= 0; i--)
        {
            OAMEntry& sprite = visible_sprites[i];

            // Sprite coordinates are offset
            int sprite_y = sprite.y - 16;
            int sprite_x = sprite.x - 8;

            // Get palette
            uint8_t palette = (sprite.attributes & OAM_PALETTE) ? readOBP1() : readOBP0();

            // Calculate Y position within sprite
            int y_in_sprite = ly - sprite_y;

            // Apply Y flip
            if (sprite.attributes & OAM_Y_FLIP)
            {
                y_in_sprite = sprite_height - 1 - y_in_sprite;
            }

            // Get tile index (for 8x16 mode, bit 0 is ignored)
            uint8_t tile_idx = sprite.tile_index;
            if (sprite_height == 16)
            {
                tile_idx &= 0xFE;  // Clear bit 0
                // If we're in bottom half, use next tile
                if (y_in_sprite >= 8)
                {
                    tile_idx |= 0x01;
                    y_in_sprite -= 8;
                }
            }

            // Tile data is always at 0x8000 for sprites
            uint16_t tile_addr = 0x8000 + (tile_idx * 16);
            uint16_t tile_row_addr = tile_addr + (y_in_sprite * 2);
            uint8_t byte1 = mmu.read(tile_row_addr);
            uint8_t byte2 = mmu.read(tile_row_addr + 1);

            // Render each pixel of the sprite
            for (int x = 0; x < 8; x++)
            {
                int screen_x = sprite_x + x;

                // Skip if off screen
                if (screen_x < 0 || screen_x >= LCD_WIDTH)
                {
                    continue;
                }

                // Calculate bit position (apply X flip)
                uint8_t bit_pos;
                if (sprite.attributes & OAM_X_FLIP)
                {
                    bit_pos = x;
                }
                else
                {
                    bit_pos = 7 - x;
                }

                // Get color bits
                uint8_t color_bit_low = (byte1 >> bit_pos) & 1;
                uint8_t color_bit_high = (byte2 >> bit_pos) & 1;
                uint8_t color_id = (color_bit_high << 1) | color_bit_low;

                // Color 0 is transparent for sprites
                if (color_id == 0)
                {
                    continue;
                }

                // Check priority
                uint8_t bg_color = framebuffer[ly * LCD_WIDTH + screen_x];

                // If priority flag is set and BG color is not 0, BG has priority
                if ((sprite.attributes & OAM_PRIORITY) && bg_color != 0)
                {
                    continue;
                }

                // Apply palette
                uint8_t palette_color = (palette >> (color_id * 2)) & 0x03;

                // Write to framebuffer
                framebuffer[ly * LCD_WIDTH + screen_x] = palette_color;
            }
        }
    }
}
