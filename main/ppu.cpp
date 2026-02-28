#include "ppu.hpp"
#include "memory_bus.hpp"
#include "text_renderer.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

namespace ppu
{
    // Cycles per scanline mode
    constexpr uint16_t OAM_SCAN_CYCLES = 80;
    constexpr uint16_t DRAWING_CYCLES = 172;
    constexpr uint16_t HBLANK_CYCLES = 204;
    constexpr uint16_t SCANLINE_CYCLES = OAM_SCAN_CYCLES + DRAWING_CYCLES + HBLANK_CYCLES;
    constexpr uint16_t VBLANK_SCANLINES = 10;
    constexpr uint16_t TOTAL_SCANLINES = display::LCD_HEIGHT + VBLANK_SCANLINES;

    QueueHandle_t PPU::frame_queue = nullptr;

    PPU::PPU(memory::MemoryBus &mmu, std::shared_ptr<display::LCDDisplay> display)
        : mmu(mmu),
          mode(Mode::OAM_SCAN),
          mode_cycles(0),
          ly(0),
          frame_ready(false),
          should_render_frame(true),
          window_line_counter(0),
          prev_stat_line(false),
          visible_sprite_count(0),
          display(std::move(display))
    {
        // Framebuffer en RAM interne (beaucoup plus rapide que PSRAM)
        framebuffer = static_cast<uint16_t *>(
            heap_caps_calloc(display::LCD_WIDTH * display::LCD_HEIGHT,
                             sizeof(uint16_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        if (!framebuffer)
        {
            ESP_LOGE("PPU", "Failed to allocate framebuffer in INTERNAL RAM!");
        }
        else
        {
            ESP_LOGI("PPU", "Framebuffer allocated in INTERNAL RAM (%d bytes)",
                     display::LCD_WIDTH * display::LCD_HEIGHT * (int)sizeof(uint16_t));
        }

        vram = mmu.getVRAM();
        oam = mmu.getOAM();

        if (!init_pipeline())
        {
            ESP_LOGE("PPU", "Failed to initialize render pipeline!");
        }

        // NOTE: LCD registers are NOT initialized here!
        // They will be initialized by either:
        //   1. The boot ROM (if enabled) during execution
        //   2. initializePostBootROMState() in MemoryBus (if boot ROM is disabled)
        // This ensures correct boot ROM behavior when enabled.
    }

    PPU::~PPU()
    {
        if (framebuffer)
        {
            heap_caps_free(framebuffer);
            framebuffer = nullptr;
        }
    }

    // --- Core step/state machine (inchangé) ---
    void PPU::step(uint8_t cycles)
    {
        // When LCD is disabled (LCDC bit 7 = 0), reset PPU state
        // This must happen even during boot ROM execution
        if (UNLIKELY((readLCDC() & LCDC_LCD_ENABLE) == 0)) {
            // Force PPU to HBLANK mode when LCD is off
            mode = Mode::HBLANK;
            mode_cycles = 0;

            // Update LY to 0 (LCD off state)
            updateLY(0);

            // Force STAT to show mode 0 (HBLANK) when LCD is disabled
            // This is required for games that poll STAT even when LCD is off
            uint8_t stat = mmu.read(0xFF41);
            stat = (stat & 0xFC);  // Clear mode bits (0-1), keep other bits
            mmu.write(0xFF41, stat);

            // Check STAT interrupt after state changes
            triggerSTATIfNeeded();
            return;
        }

        mode_cycles += cycles;

        switch (mode)
        {
        case Mode::OAM_SCAN:
            if (mode_cycles >= OAM_SCAN_CYCLES)
            {
                mode_cycles -= OAM_SCAN_CYCLES;
                scanOAM();
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
                updateLY(ly + 1);

                if (ly >= display::LCD_HEIGHT)
                {
                    setMode(Mode::VBLANK);
                    mmu.request_interrupt(memory::IRQFlag::IRQ_VBLANK);
                    frame_ready = true;

                    queue_frame_for_rendering();
                    window_line_counter = 0;
                }
                else
                {
                    setMode(Mode::OAM_SCAN);
                }
            }
            break;

        case Mode::VBLANK:
            if (mode_cycles >= SCANLINE_CYCLES)
            {
                mode_cycles -= SCANLINE_CYCLES;
                updateLY(ly + 1);

                if (ly >= TOTAL_SCANLINES)
                {
                    updateLY(0);
                    setMode(Mode::OAM_SCAN);
                }
            }
            break;
        }


    }

    void PPU::triggerSTATIfNeeded()
    {
        // Read STAT register
        uint8_t stat = mmu.read(0xFF41);
        uint8_t lyc = mmu.read(0xFF45);

        // Calculate STAT interrupt line state
        // The line is HIGH if any of these conditions are met:
        bool stat_line = false;

        // Mode 0 (HBLANK) interrupt enabled (bit 3) AND in HBLANK
        if ((stat & 0x08) && mode == Mode::HBLANK)
            stat_line = true;

        // Mode 1 (VBLANK) interrupt enabled (bit 4) AND in VBLANK
        if ((stat & 0x10) && mode == Mode::VBLANK)
            stat_line = true;

        // Mode 2 (OAM) interrupt enabled (bit 5) AND in OAM_SCAN
        if ((stat & 0x20) && mode == Mode::OAM_SCAN)
            stat_line = true;

        // LYC=LY interrupt enabled (bit 6) AND LY matches LYC
        if ((stat & 0x40) && (ly == lyc))
            stat_line = true;

        // Trigger interrupt only on LOW->HIGH transition (edge trigger)
        if (stat_line && !prev_stat_line)
        {
            mmu.request_interrupt(memory::IRQFlag::IRQ_LCD_STAT);
        }

        prev_stat_line = stat_line;
    }

    void PPU::setMode(Mode new_mode)
    {
        if (mode == new_mode)
            return;

        mode = new_mode;

        // Update STAT register mode bits (0-1)
        uint8_t stat = mmu.read(0xFF41);
        stat = (stat & 0xFC) | static_cast<uint8_t>(new_mode);
        mmu.write(0xFF41, stat);

        // Trigger STAT interrupt immediately if needed
        triggerSTATIfNeeded();

        // CGB: step HBlank DMA when entering HBlank
        if (new_mode == Mode::HBLANK) {
            mmu.hdmaHBlankStep();
        }
    }

    void PPU::updateLY(uint8_t new_ly)
    {
        ly = new_ly;
        mmu.write(0xFF44, ly);

        // Update LYC=LY coincidence flag (bit 2)
        uint8_t lyc = mmu.read(0xFF45);
        uint8_t stat = mmu.read(0xFF41);

        if (ly == lyc) {
            stat |= 0x04;  // Set coincidence flag
        } else {
            stat &= ~0x04;  // Clear coincidence flag
        }

        mmu.write(0xFF41, stat);

        // Trigger STAT interrupt immediately if needed
        triggerSTATIfNeeded();
    }

    void PPU::renderScanline()
    {
        // Skip rendering if frame skipping is active (optimization)
        if (UNLIKELY(!should_render_frame))
            return;

        ScanlineContext ctx = {
            .lcdc = readLCDC(),
            .scy = readSCY(),
            .scx = readSCX(),
            .bgp = readBGP(),
            .wy = readWY(),
            .wx = readWX(),
            .obp0 = readOBP0(),
            .obp1 = readOBP1()};

        renderBackground(ctx);
        renderWindow(ctx);
        renderSprites(ctx);
    }

    void PPU::renderBackground(const ScanlineContext &ctx)
    {
        const size_t fb_row = static_cast<size_t>(ly) * display::LCD_WIDTH;

        const uint16_t tile_map_base =
            (ctx.lcdc & LCDC_BG_TILE_MAP) ? 0x9C00 : 0x9800;

        const bool signed_tiles =
            (ctx.lcdc & LCDC_BG_WINDOW_TILES) == 0;

        const uint16_t tile_data_base =
            signed_tiles ? 0x9000 : 0x8000;

        const uint8_t y_pos = static_cast<uint8_t>(ly + ctx.scy);
        const uint8_t tile_y = y_pos >> 3;
        const uint8_t pixel_y = y_pos & 0x7;

        const uint16_t tile_row_base =
            tile_map_base + (tile_y * TILES_PER_ROW);

        const uint8_t fine_x   = ctx.scx & 0x07;
        const uint8_t coarse_x = ctx.scx >> 3;
        const int num_tiles = 20 + (fine_x ? 1 : 0);

        if (LIKELY(!cgb_mode)) {
            // DMG: LCDC.0=0 disables BG and Window → fill line with BGP color 0 (white)
            if (UNLIKELY(!(ctx.lcdc & LCDC_BG_WINDOW_ENABLE))) {
                const uint16_t white = GB_PALETTE_BGR565[(ctx.bgp >> 0) & 0x03];
                uint16_t* row = framebuffer + fb_row;
                for (int x = 0; x < display::LCD_WIDTH; ++x) row[x] = white;
                return;
            }

            // --- DMG fast path ---
            uint16_t palette_lut[4];
            palette_lut[0] = GB_PALETTE_BGR565[(ctx.bgp >> 0) & 0x03];
            palette_lut[1] = GB_PALETTE_BGR565[(ctx.bgp >> 2) & 0x03];
            palette_lut[2] = GB_PALETTE_BGR565[(ctx.bgp >> 4) & 0x03];
            palette_lut[3] = GB_PALETTE_BGR565[(ctx.bgp >> 6) & 0x03];

            int screen_x = 0;
            for (int tile = 0; tile < num_tiles && screen_x < display::LCD_WIDTH; ++tile)
            {
                const uint8_t tile_x = (coarse_x + tile) & 0x1F;
                const uint8_t tile_index =
                    vram[tile_row_base - memory::VRAM_START + tile_x];

                uint16_t tile_addr;
                if (UNLIKELY(signed_tiles))
                    tile_addr = tile_data_base + (static_cast<int8_t>(tile_index) * 16);
                else
                    tile_addr = tile_data_base + (tile_index * 16);

                const uint16_t row_addr = tile_addr + (pixel_y * 2);
                const uint8_t b1 = vram[row_addr - memory::VRAM_START];
                const uint8_t b2 = vram[row_addr - memory::VRAM_START + 1];

                uint16_t tile_row[8];
                for (int i = 0; i < 8; ++i)
                {
                    const uint8_t bit = 7 - i;
                    const uint8_t color =
                        ((b2 >> bit) & 1) << 1 |
                        ((b1 >> bit) & 1);
                    tile_row[i] = palette_lut[color];
                }

                const int start_px = (tile == 0) ? fine_x : 0;
                for (int px = start_px; px < 8 && screen_x < display::LCD_WIDTH; ++px)
                {
                    framebuffer[fb_row + screen_x++] = tile_row[px];
                }
            }
        } else {
            // --- CGB path ---
            int screen_x = 0;
            for (int tile = 0; tile < num_tiles && screen_x < display::LCD_WIDTH; ++tile)
            {
                const uint8_t tile_x = (coarse_x + tile) & 0x1F;
                const uint16_t map_offset = tile_row_base - memory::VRAM_START + tile_x;

                const uint8_t tile_index = vram[map_offset];
                const uint8_t attrs = (vram_bank1) ? vram_bank1[map_offset] : 0;

                const uint8_t palette_num = attrs & 0x07;
                const bool use_bank1     = (attrs & 0x08) != 0;
                const bool x_flip        = (attrs & 0x20) != 0;
                const bool y_flip        = (attrs & 0x40) != 0;

                const uint8_t effective_py = y_flip ? (7 - pixel_y) : pixel_y;

                uint16_t tile_addr;
                if (UNLIKELY(signed_tiles))
                    tile_addr = tile_data_base + (static_cast<int8_t>(tile_index) * 16);
                else
                    tile_addr = tile_data_base + (tile_index * 16);

                const uint16_t row_addr = tile_addr + (effective_py * 2);
                const uint16_t vram_offset = row_addr - memory::VRAM_START;

                const uint8_t* tile_vram = (use_bank1 && vram_bank1) ? vram_bank1 : vram;
                const uint8_t b1 = tile_vram[vram_offset];
                const uint8_t b2 = tile_vram[vram_offset + 1];

                // Use pre-converted palette cache (avoids per-tile cgb_color_to_rgb565 calls)
                const uint16_t* palette_lut = bg_pal_cache
                    ? (bg_pal_cache + palette_num * 4)
                    : nullptr;
                uint16_t palette_lut_local[4];
                if (UNLIKELY(!palette_lut)) {
                    const uint8_t* pal = bg_palette_ram + palette_num * 8;
                    for (int c = 0; c < 4; ++c)
                        palette_lut_local[c] = cgb_color_to_rgb565(pal[c * 2], pal[c * 2 + 1]);
                    palette_lut = palette_lut_local;
                }

                uint16_t tile_row[8];
                uint8_t  tile_colors[8];
                const bool bg_prio = (attrs & 0x80) != 0;
                for (int i = 0; i < 8; ++i)
                {
                    const uint8_t bit = x_flip ? i : (7 - i);
                    const uint8_t color =
                        ((b2 >> bit) & 1) << 1 |
                        ((b1 >> bit) & 1);
                    tile_row[i] = palette_lut[color];
                    tile_colors[i] = color;
                }

                const int start_px = (tile == 0) ? fine_x : 0;
                for (int px = start_px; px < 8 && screen_x < display::LCD_WIDTH; ++px)
                {
                    framebuffer[fb_row + screen_x] = tile_row[px];
                    bg_color_index[screen_x]   = tile_colors[px];
                    bg_tile_priority[screen_x] = bg_prio;
                    ++screen_x;
                }
            }
        }
    }

    void PPU::renderWindow(const ScanlineContext &ctx)
    {
        if (UNLIKELY((ctx.lcdc & LCDC_WINDOW_ENABLE) == 0))
            return;

        if (UNLIKELY(ctx.wy > ly || ctx.wx >= 167))
            return;

        bool pixels_rendered = false;
        const int win_x_start = ctx.wx - 7;
        const size_t fb_row = static_cast<size_t>(ly) * display::LCD_WIDTH;

        const uint16_t tile_map_base =
            (ctx.lcdc & LCDC_WINDOW_TILE_MAP) ? 0x9C00 : 0x9800;

        const bool signed_tiles =
            (ctx.lcdc & LCDC_BG_WINDOW_TILES) == 0;

        const uint16_t tile_data_base =
            signed_tiles ? 0x9000 : 0x8000;

        const uint8_t tile_y = window_line_counter >> 3;
        const uint8_t pixel_y = window_line_counter & 0x7;

        if (LIKELY(!cgb_mode)) {
            // DMG: LCDC.0=0 also disables Window (renderBackground already filled the line)
            if (UNLIKELY(!(ctx.lcdc & LCDC_BG_WINDOW_ENABLE))) return;

            // --- DMG fast path ---
            uint16_t palette_lut[4];
            palette_lut[0] = GB_PALETTE_BGR565[(ctx.bgp >> 0) & 0x03];
            palette_lut[1] = GB_PALETTE_BGR565[(ctx.bgp >> 2) & 0x03];
            palette_lut[2] = GB_PALETTE_BGR565[(ctx.bgp >> 4) & 0x03];
            palette_lut[3] = GB_PALETTE_BGR565[(ctx.bgp >> 6) & 0x03];

            for (int x = std::max(0, win_x_start); x < display::LCD_WIDTH; x += 8)
            {
                const int win_x = x - win_x_start;
                const uint8_t tile_x = win_x >> 3;
                const uint8_t px0 = win_x & 0x7;

                const uint16_t map_addr =
                    tile_map_base + tile_y * TILES_PER_ROW + tile_x;

                const uint8_t tile_index =
                    vram[map_addr - memory::VRAM_START];

                uint16_t tile_addr;
                if (UNLIKELY(signed_tiles))
                    tile_addr = tile_data_base + (static_cast<int8_t>(tile_index) * 16);
                else
                    tile_addr = tile_data_base + (tile_index * 16);

                const uint16_t row_addr = tile_addr + pixel_y * 2;
                const uint8_t b1 = vram[row_addr - memory::VRAM_START];
                const uint8_t b2 = vram[row_addr - memory::VRAM_START + 1];

                uint16_t tile_row[8];
                for (int i = 0; i < 8; ++i)
                {
                    const uint8_t bit = 7 - i;
                    const uint8_t color =
                        ((b2 >> bit) & 1) << 1 |
                        ((b1 >> bit) & 1);
                    tile_row[i] = palette_lut[color];
                }

                for (int i = 0; i < 8; ++i)
                {
                    const int sx = x + i;
                    if (UNLIKELY(sx < 0 || sx >= display::LCD_WIDTH))
                        continue;
                    if (UNLIKELY(sx < win_x_start))
                        continue;

                    const int tile_pixel = (px0 + i) & 0x7;
                    pixels_rendered = true;
                    framebuffer[fb_row + sx] = tile_row[tile_pixel];
                }
            }
        } else {
            // --- CGB path ---
            for (int x = std::max(0, win_x_start); x < display::LCD_WIDTH; x += 8)
            {
                const int win_x = x - win_x_start;
                const uint8_t tile_x = win_x >> 3;
                const uint8_t px0 = win_x & 0x7;

                const uint16_t map_addr =
                    tile_map_base + tile_y * TILES_PER_ROW + tile_x;
                const uint16_t map_offset = map_addr - memory::VRAM_START;

                const uint8_t tile_index = vram[map_offset];
                const uint8_t attrs = (vram_bank1) ? vram_bank1[map_offset] : 0;

                const uint8_t palette_num = attrs & 0x07;
                const bool use_bank1     = (attrs & 0x08) != 0;
                const bool x_flip        = (attrs & 0x20) != 0;
                const bool y_flip        = (attrs & 0x40) != 0;

                const uint8_t effective_py = y_flip ? (7 - pixel_y) : pixel_y;

                uint16_t tile_addr;
                if (UNLIKELY(signed_tiles))
                    tile_addr = tile_data_base + (static_cast<int8_t>(tile_index) * 16);
                else
                    tile_addr = tile_data_base + (tile_index * 16);

                const uint16_t row_addr = tile_addr + effective_py * 2;
                const uint16_t vram_offset = row_addr - memory::VRAM_START;
                const uint8_t* tile_vram = (use_bank1 && vram_bank1) ? vram_bank1 : vram;
                const uint8_t b1 = tile_vram[vram_offset];
                const uint8_t b2 = tile_vram[vram_offset + 1];

                const uint8_t* pal = bg_palette_ram + palette_num * 8;
                // Use pre-converted palette cache when available
                const uint16_t* palette_lut = bg_pal_cache
                    ? (bg_pal_cache + palette_num * 4)
                    : nullptr;
                uint16_t palette_lut_local[4];
                if (UNLIKELY(!palette_lut)) {
                    for (int c = 0; c < 4; ++c)
                        palette_lut_local[c] = cgb_color_to_rgb565(pal[c * 2], pal[c * 2 + 1]);
                    palette_lut = palette_lut_local;
                }

                uint16_t tile_row[8];
                uint8_t  tile_colors[8];
                const bool bg_prio = (attrs & 0x80) != 0;
                for (int i = 0; i < 8; ++i)
                {
                    const uint8_t bit = x_flip ? i : (7 - i);
                    const uint8_t color =
                        ((b2 >> bit) & 1) << 1 |
                        ((b1 >> bit) & 1);
                    tile_row[i] = palette_lut[color];
                    tile_colors[i] = color;
                }

                for (int i = 0; i < 8; ++i)
                {
                    const int sx = x + i;
                    if (UNLIKELY(sx < 0 || sx >= display::LCD_WIDTH))
                        continue;
                    if (UNLIKELY(sx < win_x_start))
                        continue;

                    const int tile_pixel = (px0 + i) & 0x7;
                    pixels_rendered = true;
                    framebuffer[fb_row + sx]   = tile_row[tile_pixel];
                    bg_color_index[sx]         = tile_colors[tile_pixel];
                    bg_tile_priority[sx]       = bg_prio;
                }
            }
        }

        if (pixels_rendered) window_line_counter++;
    }

    void PPU::scanOAM()
    {
        visible_sprite_count = 0;

        if ((readLCDC() & LCDC_OBJ_ENABLE) == 0)
            return;

        const uint8_t sprite_height =
            (readLCDC() & LCDC_OBJ_SIZE) ? 16 : 8;

        for (int i = 0; i < MAX_SPRITES &&
                        visible_sprite_count < MAX_SPRITES_PER_LINE;
             ++i)
        {
            const int base = i * 4;

            OAMEntry sprite;
            sprite.y = oam[base + 0];
            sprite.x = oam[base + 1];
            sprite.tile_index = oam[base + 2];
            sprite.attributes = oam[base + 3];

            const int sprite_y = sprite.y - 16;

            if (ly >= sprite_y && ly < sprite_y + sprite_height)
            {
                visible_sprites[visible_sprite_count++] = sprite;
            }
        }
    }

    void PPU::renderSprites(const ScanlineContext &ctx)
    {
        if (UNLIKELY((ctx.lcdc & LCDC_OBJ_ENABLE) == 0))
            return;

        if (UNLIKELY(visible_sprite_count == 0))
            return;

        const uint8_t sprite_height =
            (ctx.lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

        const size_t fb_row = static_cast<size_t>(ly) * display::LCD_WIDTH;

        // Pre-calculate sprite palettes
        uint16_t palette_lut0[4], palette_lut1[4];
        palette_lut0[0] = palette_lut1[0] = 0;  // Color 0 is always transparent
        palette_lut0[1] = GB_PALETTE_BGR565[(ctx.obp0 >> 2) & 0x03];
        palette_lut0[2] = GB_PALETTE_BGR565[(ctx.obp0 >> 4) & 0x03];
        palette_lut0[3] = GB_PALETTE_BGR565[(ctx.obp0 >> 6) & 0x03];
        palette_lut1[1] = GB_PALETTE_BGR565[(ctx.obp1 >> 2) & 0x03];
        palette_lut1[2] = GB_PALETTE_BGR565[(ctx.obp1 >> 4) & 0x03];
        palette_lut1[3] = GB_PALETTE_BGR565[(ctx.obp1 >> 6) & 0x03];

        // Background color 0 for DMG sprite priority check (CGB uses bg_color_index[])
        const uint16_t bg_color = GB_PALETTE_BGR565[(ctx.bgp >> 0) & 0x03];

        for (int i = visible_sprite_count - 1; i >= 0; --i)
        {
            const OAMEntry &s = visible_sprites[i];

            int y_in = ly - (s.y - 16);
            if (UNLIKELY(s.attributes & OAM_Y_FLIP))
                y_in = sprite_height - 1 - y_in;

            uint8_t tile = s.tile_index;
            if (UNLIKELY(sprite_height == 16))
            {
                tile &= 0xFE;
                if (y_in >= 8)
                {
                    tile |= 1;
                    y_in -= 8;
                }
            }

            const uint16_t addr = 0x8000 + tile * 16 + y_in * 2;
            uint8_t b1 = vram[addr - memory::VRAM_START];
            uint8_t b2 = vram[addr - memory::VRAM_START + 1];

            // CGB: bit 3 of attributes = VRAM bank
            if (cgb_mode && (s.attributes & 0x08) && vram_bank1) {
                const uint16_t addr_offset = addr - memory::VRAM_START;
                b1 = vram_bank1[addr_offset];
                b2 = vram_bank1[addr_offset + 1];
            }

            const uint16_t* palette = (s.attributes & OAM_PALETTE) ? palette_lut1 : palette_lut0;

            // In CGB mode, override palette with OBJ palette cache (or raw RAM fallback)
            uint16_t cgb_palette_lut[4];
            if (cgb_mode) {
                const uint8_t cgb_pal_num = s.attributes & 0x07;
                if (obj_pal_cache) {
                    // Use pre-converted cache; color 0 is transparent
                    cgb_palette_lut[0] = 0;
                    const uint16_t* pal_entry = obj_pal_cache + cgb_pal_num * 4;
                    cgb_palette_lut[1] = pal_entry[1];
                    cgb_palette_lut[2] = pal_entry[2];
                    cgb_palette_lut[3] = pal_entry[3];
                } else if (obj_palette_ram) {
                    const uint8_t* pal = obj_palette_ram + cgb_pal_num * 8;
                    cgb_palette_lut[0] = 0;
                    for (int c = 1; c < 4; ++c)
                        cgb_palette_lut[c] = cgb_color_to_rgb565(pal[c * 2], pal[c * 2 + 1]);
                }
                palette = cgb_palette_lut;
            }
            const bool has_priority = s.attributes & OAM_PRIORITY;
            const bool x_flip = s.attributes & OAM_X_FLIP;

            for (int x = 0; x < 8; ++x)
            {
                const int sx = (s.x - 8) + x;
                if (UNLIKELY(sx < 0 || sx >= display::LCD_WIDTH))
                    continue;

                const uint8_t bit = x_flip ? x : (7 - x);
                const uint8_t color =
                    ((b2 >> bit) & 1) << 1 |
                    ((b1 >> bit) & 1);

                if (UNLIKELY(color == 0))
                    continue;

                if (cgb_mode) {
                    // CGB: BG wins if LCDC.0=1 AND (OAM attr.7=1 OR BG tile attr.7=1)
                    //      AND the BG pixel is not color 0 (transparent)
                    if ((ctx.lcdc & LCDC_BG_WINDOW_ENABLE) &&
                        (has_priority || bg_tile_priority[sx]) &&
                        bg_color_index[sx] != 0)
                        continue;
                } else {
                    // DMG: OAM attr.7=1 → sprite behind BG (only draws over BG color 0)
                    if (UNLIKELY(has_priority && framebuffer[fb_row + sx] != bg_color))
                        continue;
                }

                framebuffer[fb_row + sx] = palette[color];
            }
        }
    }

    // -------------------------
    // Pipeline / Render task
    // -------------------------
    bool PPU::init_pipeline()
    {
        // Pour xQueueOverwrite, la queue doit être de longueur 1
        frame_queue = xQueueCreate(1, sizeof(uint16_t *));
        if (!frame_queue)
        {
            ESP_LOGE("PPU", "Failed to create frame queue!");
            return false;
        }

        ESP_LOGI("PPU", "Render pipeline initialized (queue length = 1)");
        return true;
    }

    void PPU::render_task(void *arg)
    {
        auto *ppu = static_cast<PPU *>(arg);
        ESP_LOGI("PPU", "Render task started on core %d", xPortGetCoreID());

        uint16_t *frame = nullptr;
        while (true)
        {
            if (xQueueReceive(frame_queue, &frame, portMAX_DELAY) == pdTRUE)
            {
                // La framebuffer est en RAM interne, mais le LCD DMA ne lit pas la PSRAM;
                // votre LCDDisplay fait déjà le chunk/copie DMA interne si besoin.

                // Center Game Boy display (160x144) on LCD screen (240x320)
                constexpr int offset_x = (display::SCREEN_WIDTH - display::LCD_WIDTH) >> 1;   // (240-160)/2 = 40
                constexpr int offset_y = (display::SCREEN_HEIGHT - display::LCD_HEIGHT) >> 1; // (320-144)/2 = 88
                ppu->display->renderFrameRGB565(frame, display::LCD_WIDTH, display::LCD_HEIGHT, offset_x, offset_y);
            }
        }
    }

    void PPU::queue_frame_for_rendering()
    {
        if (!frame_queue)
            return;

        // Always queue the framebuffer for display
        // Even if rendering was skipped, we want to show the last frame
        // The should_render_frame flag only affects whether we UPDATE the framebuffer
        uint16_t *fb_ptr = framebuffer;
        xQueueOverwrite(frame_queue, &fb_ptr);
    }

} // namespace ppu
