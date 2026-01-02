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
        if (UNLIKELY((readLCDC() & LCDC_LCD_ENABLE) == 0))
            return;

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

    void PPU::setMode(Mode new_mode)
    {
        mode = new_mode;

        uint8_t stat = mmu.read(0xFF41);
        stat = (stat & 0xFC) | static_cast<uint8_t>(new_mode);
        mmu.write(0xFF41, stat);

        bool request_stat_int = false;
        switch (new_mode)
        {
        case Mode::HBLANK:
            if (stat & 0x08)
                request_stat_int = true;
            break;
        case Mode::VBLANK:
            if (stat & 0x10)
                request_stat_int = true;
            break;
        case Mode::OAM_SCAN:
            if (stat & 0x20)
                request_stat_int = true;
            break;
        case Mode::DRAWING:
            break;
        }

        if (request_stat_int)
            mmu.request_interrupt(memory::IRQFlag::IRQ_LCD_STAT);
    }

    void PPU::updateLY(uint8_t new_ly)
    {
        ly = new_ly;
        mmu.write(0xFF44, ly);

        uint8_t lyc = mmu.read(0xFF45);
        uint8_t stat = mmu.read(0xFF41);

        if (ly == lyc)
        {
            stat |= 0x04;
            if (stat & 0x40)
                mmu.request_interrupt(memory::IRQFlag::IRQ_LCD_STAT);
        }
        else
        {
            stat &= ~0x04;
        }

        mmu.write(0xFF41, stat);
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

        // Note: On DMG (original Game Boy), LCDC bit 0 does NOT disable the background!
        // The background is always drawn. This bit only affects sprite priority.
        // Only on Game Boy Color can this bit disable the background completely.
        // We emulate DMG behavior, so we always render the background.

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

        // Pre-calculate palette lookup table for this scanline
        uint16_t palette_lut[4];
        palette_lut[0] = GB_PALETTE_BGR565[(ctx.bgp >> 0) & 0x03];
        palette_lut[1] = GB_PALETTE_BGR565[(ctx.bgp >> 2) & 0x03];
        palette_lut[2] = GB_PALETTE_BGR565[(ctx.bgp >> 4) & 0x03];
        palette_lut[3] = GB_PALETTE_BGR565[(ctx.bgp >> 6) & 0x03];

        // 20 tiles = 160 pixels
        for (int tile = 0; tile < 20; ++tile)
        {
            const int x_base = tile * 8;
            const uint8_t x_pos = static_cast<uint8_t>(x_base + ctx.scx);

            const uint8_t tile_x = x_pos >> 3;
            const uint8_t px0 = x_pos & 0x7;

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

            // Decode entire tile row at once (8 pixels)
            uint16_t tile_row[8];
            for (int i = 0; i < 8; ++i)
            {
                const uint8_t bit = 7 - i;
                const uint8_t color =
                    ((b2 >> bit) & 1) << 1 |
                    ((b1 >> bit) & 1);
                tile_row[i] = palette_lut[color];
            }

            // Write pixels (handle scrolling offset)
            for (int i = 0; i < 8; ++i)
            {
                const int screen_x = x_base + i;
                if (UNLIKELY(screen_x >= display::LCD_WIDTH))
                    break;

                const int tile_pixel = (px0 + i) & 0x7;
                framebuffer[fb_row + screen_x] = tile_row[tile_pixel];
            }
        }
    }

    void PPU::renderWindow(const ScanlineContext &ctx)
    {
        if (UNLIKELY((ctx.lcdc & LCDC_WINDOW_ENABLE) == 0))
            return;

        if (UNLIKELY(ctx.wy > ly || ctx.wx >= 167))
            return;

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

        // Pre-calculate palette lookup table (same as background)
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

            // Decode entire tile row at once
            uint16_t tile_row[8];
            for (int i = 0; i < 8; ++i)
            {
                const uint8_t bit = 7 - i;
                const uint8_t color =
                    ((b2 >> bit) & 1) << 1 |
                    ((b1 >> bit) & 1);
                tile_row[i] = palette_lut[color];
            }

            // Write pixels
            for (int i = 0; i < 8; ++i)
            {
                const int sx = x + i;
                if (UNLIKELY(sx < 0 || sx >= display::LCD_WIDTH))
                    continue;
                if (UNLIKELY(sx < win_x_start))
                    continue;

                const int tile_pixel = (px0 + i) & 0x7;
                framebuffer[fb_row + sx] = tile_row[tile_pixel];
            }
        }

        window_line_counter++;
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

        // Background color 0 (for sprite priority check)
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
            const uint8_t b1 = vram[addr - memory::VRAM_START];
            const uint8_t b2 = vram[addr - memory::VRAM_START + 1];

            const uint16_t* palette = (s.attributes & OAM_PALETTE) ? palette_lut1 : palette_lut0;
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

                if (UNLIKELY(has_priority && framebuffer[fb_row + sx] != bg_color))
                    continue;

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
        int frame_count = 0;

        while (true)
        {
            if (xQueueReceive(frame_queue, &frame, portMAX_DELAY) == pdTRUE)
            {
                // La framebuffer est en RAM interne, mais le LCD DMA ne lit pas la PSRAM;
                // votre LCDDisplay fait déjà le chunk/copie DMA interne si besoin.
                ppu->display->renderFrameRGB565(frame, display::LCD_WIDTH, display::LCD_HEIGHT);
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
