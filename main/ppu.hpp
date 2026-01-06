#pragma once

#include "lcd_display.hpp"
#include "memory_bus.hpp"
#include "esp_attr.h"

#include <cstdint>
#include <array>
#include <memory>

// FreeRTOS includes for pipeline async
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Branch prediction hints (from cpu.hpp)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

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

    // Helper to convert RGB565 to BGR565 for ST7789V
    static constexpr uint16_t rgb_to_bgr565(uint16_t rgb) {
        uint16_t r = (rgb >> 11) & 0x1F;  // Extract 5 red bits
        uint16_t g = (rgb >> 5) & 0x3F;   // Extract 6 green bits
        uint16_t b = rgb & 0x1F;          // Extract 5 blue bits
        return (b << 11) | (g << 5) | r;  // Reassemble as BGR
    }

    // Cached PPU register context for scanline rendering (avoid repeated MMU reads)
    struct ScanlineContext
    {
        uint8_t lcdc;   // LCD Control
        uint8_t scy;    // Scroll Y
        uint8_t scx;    // Scroll X
        uint8_t bgp;    // Background palette
        uint8_t wy;     // Window Y
        uint8_t wx;     // Window X
        uint8_t obp0;   // Sprite palette 0
        uint8_t obp1;   // Sprite palette 1
    };

    class PPU
    {
    public:
        PPU(memory::MemoryBus&, std::shared_ptr<display::LCDDisplay>);
        ~PPU();

        // Update PPU state for given number of cycles
        IRAM_ATTR void step(uint8_t cycles) __attribute__((hot));

        // Get framebuffer pointer for rendering (RGB565 format)
        const uint16_t* getFramebuffer() const { return framebuffer; }

        // Get current scanline
        uint8_t getCurrentLine() const { return ly; }

        // Check if frame is ready for display
        bool isFrameReady() const { return frame_ready; }
        void clearFrameReady() { frame_ready = false; }

        // Control frame rendering for frame skipping
        inline void setShouldRender(bool should_render) { should_render_frame = should_render; }
        inline bool getShouldRender() const { return should_render_frame; }

        // Pipeline asynchrone methods
        static void render_task(void* arg);
        void queue_frame_for_rendering();
        bool init_pipeline();
        inline uint8_t getLy() const { return ly; }
        inline uint8_t getModeCycles() const { return mode_cycles; }
        inline Mode getMode() const { return mode; }
        
    private:
        memory::MemoryBus& mmu;
        const uint8_t* vram{nullptr};
        const uint8_t* oam{nullptr};

        // Framebuffer: Direct RGB565 (no conversion needed!)
        // Allocated in INTERNAL RAM for fast access (DMA-compatible)
        uint16_t* framebuffer{nullptr};

        // Optimized palette lookup table (converted RGB565 to BGR565 for ST7789V)
        static constexpr uint16_t GB_PALETTE_BGR565[4] = {
            ppu::rgb_to_bgr565(0xFFFF), // WHITE
            ppu::rgb_to_bgr565(0xC618), // LIGHT_GRAY
            ppu::rgb_to_bgr565(0x632C), // DARK_GRAY
            ppu::rgb_to_bgr565(0x0000)  // BLACK
        };

        // PPU state
        Mode mode;
        uint16_t mode_cycles;
        uint8_t ly;  // Current scanline (0-153)
        bool frame_ready;
        bool should_render_frame = false;  // Control frame rendering for skipping, disabled initially
        uint8_t window_line_counter;  // Internal window line counter
        bool prev_stat_line;  // Previous STAT interrupt line state (for STAT blocking)

        // Sprite buffer for current scanline
        std::array<OAMEntry, MAX_SPRITES_PER_LINE> visible_sprites;
        uint8_t visible_sprite_count;

        // Helper functions
        void renderScanline();
        void renderBackground(const ScanlineContext& ctx);
        void renderWindow(const ScanlineContext& ctx);
        void scanOAM();
        void renderSprites(const ScanlineContext& ctx);
        void setMode(Mode new_mode);
        void updateLY(uint8_t new_ly);
        void triggerSTATIfNeeded();  // Check and trigger STAT interrupt if needed
        display::Color getPixelColor(uint8_t tile_index, uint8_t x, uint8_t y);

        std::shared_ptr<display::LCDDisplay> display{nullptr};

        // Pipeline asynchrone - queue de frames (style Espeon)
        static QueueHandle_t frame_queue;
        
        // Register read functions - IRAM for fast access (hot path)
        IRAM_ATTR inline uint8_t readLCDC() const;
        IRAM_ATTR inline uint8_t readSCY() const;
        IRAM_ATTR inline uint8_t readSCX() const;
        IRAM_ATTR inline uint8_t readBGP() const;   // Background palette
        IRAM_ATTR inline uint8_t readWY() const;    // Window Y position
        IRAM_ATTR inline uint8_t readWX() const;    // Window X position
        IRAM_ATTR inline uint8_t readOBP0() const;  // Sprite palette 0
        IRAM_ATTR inline uint8_t readOBP1() const;  // Sprite palette 1
    };

    // Inline implementations (must be in header for inline to work)
    inline uint8_t PPU::readLCDC() const { return mmu.read(0xFF40); }
    inline uint8_t PPU::readSCY() const { return mmu.read(0xFF42); }
    inline uint8_t PPU::readSCX() const { return mmu.read(0xFF43); }
    inline uint8_t PPU::readBGP() const { return mmu.read(0xFF47); }
    inline uint8_t PPU::readWY() const { return mmu.read(0xFF4A); }
    inline uint8_t PPU::readWX() const { return mmu.read(0xFF4B); }
    inline uint8_t PPU::readOBP0() const { return mmu.read(0xFF48); }
    inline uint8_t PPU::readOBP1() const { return mmu.read(0xFF49); }
}
