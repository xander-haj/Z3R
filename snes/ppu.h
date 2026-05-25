/*
 * ppu.h — SNES Picture Processing Unit (PPU) emulator.
 *
 * The PPU renders all video output: up to 4 background layers (tiled or
 * affine-transformed in Mode 7), 128 sprites from OAM, and color math
 * compositing between main/sub screens. It outputs 256×224 (or 256×240)
 * pixels per frame at ~60 Hz (NTSC) or ~50 Hz (PAL).
 *
 * Key features emulated here:
 *   - 8 background modes (0-7) with per-layer tile/scroll/mosaic settings
 *   - Mode 7: affine matrix transformation with optional perspective correction
 *   - Sprite engine with configurable tile base addresses and sizes
 *   - Two hardware windows for per-layer clipping
 *   - Color math: add/subtract between main and sub screen with half-color
 *   - VRAM, CGRAM (palette), and OAM (sprite) access ports
 *   - Optional enhancements: 4× Mode 7 upsampling, extended screen area,
 *     no-sprite-limit mode, and an alternate "new renderer" path
 */
#ifndef ZELDA3_SNES_PPU_H_
#define ZELDA3_SNES_PPU_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "snes/saveload.h"
// Opaque forward declaration so other headers can hold a Ppu* without seeing
// the full struct body. The concrete layout is defined further down.
typedef struct Ppu Ppu;

#include "src/types.h"

/*
 * BgLayer — Per-background-layer configuration for one of the SNES's
 * four hardware background planes (BG1..BG4). The number of usable layers
 * depends on the current BG mode (see `mode` in struct Ppu below):
 *
 *   Mode 0: BG1..BG4, all 2bpp (4 colors each)
 *   Mode 1: BG1+BG2 at 4bpp, BG3 at 2bpp (Zelda 3 uses this for most rooms)
 *   Mode 3: BG1 at 8bpp, BG2 at 4bpp
 *   Mode 7: a single rotation/scale layer (BG1) with affine matrix
 *           (Zelda 3 uses this for the Triforce/intro and the world map)
 *
 * Scrolling is per-layer and wraps inside the tilemap region (which can be
 * 32×32, 64×32, 32×64, or 64×64 tiles depending on tilemapWider/Higher).
 * `tilemapAdr` and `tileAdr` are VRAM WORD addresses — i.e. indices into the
 * uint16 `vram` array, not byte offsets. Each tilemap entry is a 16-bit word
 * encoded as: yflip(15) xflip(14) prio(13) palette(12..10) tile#(9..0).
 */
// Per-background-layer configuration. The SNES supports up to 4 BG layers
// depending on the current mode. Each layer has independent scroll, tilemap
// address, and tile data address settings.
typedef struct BgLayer {
  uint16_t hScroll;       // Horizontal scroll offset (13-bit, wraps at tilemap width)
  uint16_t vScroll;       // Vertical scroll offset (13-bit, wraps at tilemap height)
  // -- snapshot starts here (saved mid-scanline for accurate rendering) --
  bool tilemapWider;      // Tilemap is 64 tiles wide (else 32); set by BGxSC register
  bool tilemapHigher;     // Tilemap is 64 tiles tall (else 32); set by BGxSC register
  uint16_t tilemapAdr;    // VRAM word address of this layer's tilemap data
  // -- snapshot ends here --
  uint16_t tileAdr;       // VRAM word address of this layer's character/tile data
} BgLayer;

// Total horizontal pixel count including optional extra side columns.
// kPpuExtraLeftRight is defined in src/types.h (96 when the widescreen
// enhancement is enabled, 0 otherwise). The scanline buffers are sized to
// kPpuXPixels so the renderer can write past 0..255 into the extended
// regions without bounds checks.
enum {
  kPpuXPixels = 256 + kPpuExtraLeftRight * 2,
};

/*
 * PpuZbufType — packed (priority << 8) | color in a uint16. The compositing
 * scanline buffers (bgBuffers, objBuffer) store one PpuZbufType per pixel.
 *
 * Layout:
 *   bits 15..12 : layer-priority "bucket" used by the compositor to pick
 *                 the winning pixel (see PpuDrawBackgrounds in ppu.c for
 *                 the full bucket numbering — BG1-prio1 = 0xc, BG2-prio0 =
 *                 0x7, sprites = computed via SPRITE_PRIO_TO_PRIO, etc.).
 *   bits 11..8  : the 4-bit palette/color extension used by sprites and
 *                 mode-7 priority sub-flag.
 *   bits 7..0   : the CGRAM palette entry index (0..255). Zero means
 *                 transparent.
 *
 * The compositor's "z-test" reduces to a plain uint16 compare: a higher
 * PpuZbufType value always wins. The backdrop is pre-cleared to 0x0500
 * (priority bucket 5 = backdrop, color 0), so any non-transparent pixel
 * with a higher priority bucket overwrites it.
 */
// Combined priority+color type for the per-pixel composition buffers.
typedef uint16_t PpuZbufType;

// Scanline buffer holding one layer's rendered pixels with priority data.
// Used during compositing to determine which layer's pixel wins at each position.
typedef struct PpuPixelPrioBufs {
  // Upper 8 bits = priority, lower 8 bits = palette color index.
  PpuZbufType data[kPpuXPixels];
} PpuPixelPrioBufs;

/*
 * Render feature flags — bitfield passed to PpuBeginDrawing() and stored
 * in ppu->renderFlags. These select between rendering paths and toggle
 * accuracy-versus-quality enhancements that are not part of the original
 * SNES hardware.
 *
 * NewRenderer:      Selects PpuDrawWholeLine() (whole-scanline path) over
 *                   the legacy per-pixel ppu_handlePixel() compositor.
 *                   The whole-line path is faster and is what Zelda 3
 *                   uses in normal play.
 * 4x4Mode7:         When Mode 7 is active AND NewRenderer is also set,
 *                   render Mode 7 at 4× horizontal and 4× vertical
 *                   resolution by sub-sampling the affine map four times
 *                   per output row (see PpuDrawMode7Upsampled in ppu.c).
 *                   Used for the smooth-zoomed world map and Triforce.
 * Height240:        Render 240 lines instead of the NTSC-standard 224.
 *                   The original game uses 224; this flag exposes the
 *                   8-line top/bottom overscan area to the renderer.
 * NoSpriteLimits:   Disable the hardware's 32-sprites-per-line and
 *                   34-tile-slivers-per-line caps. Lets enhancement
 *                   patches render more sprites without flicker.
 */
// Render feature flags — bitfield passed to PpuBeginDrawing and rendering functions.
enum {
  kPpuRenderFlags_NewRenderer = 1,      // Use the alternate (enhanced) rendering path
  kPpuRenderFlags_4x4Mode7 = 2,        // Render Mode 7 upsampled 4× (1024×1024 output)
  kPpuRenderFlags_Height240 = 4,        // Use 240 visible lines instead of default 224
  kPpuRenderFlags_NoSpriteLimits = 8,   // Disable 32-sprite / 34-tile per-line limits
};

/*
 * struct Ppu — Complete PPU instance state.
 *
 * Holds every hardware register's shadow value, the three private memory
 * arrays (VRAM/CGRAM/OAM), and the scanline render buffers used by the
 * compositor. One Ppu per emulator instance.
 *
 * Hardware reference (for modders new to the SNES):
 *   - VRAM (Video RAM):      64 KB, accessed as 32K 16-bit words. Holds
 *                            both the tilemaps (which tile to draw where)
 *                            and the tile bitplane data (the pixels of
 *                            each tile). Layout is software-defined; see
 *                            BgLayer.tilemapAdr / tileAdr.
 *   - CGRAM (Color Generator RAM): 512 bytes = 256 × 15-bit BGR entries.
 *                            The PPU's palette. Bit 15 is unused; bits
 *                            14..10 = blue, 9..5 = green, 4..0 = red.
 *   - OAM (Object Attribute Memory): 544 bytes for 128 sprite descriptors
 *                            plus a 32-byte "high table" that holds the
 *                            high-X-bit and large/small flag for each
 *                            sprite (4 sprites packed per high-table byte).
 *
 * Register interface: ppu_write() handles writes to $2100..$2133 and
 * ppu_read() handles reads from $2134..$213F. The case labels in ppu.c
 * are register-name annotated (INIDISP, BGMODE, MOSAIC, etc.).
 *
 * Each scanline is rendered by ppu_runLine(), which either dispatches to
 * the whole-line path (PpuDrawWholeLine) or the per-pixel path
 * (ppu_handlePixel), depending on renderFlags. Output lands in the
 * caller-provided renderBuffer at the renderPitch byte stride.
 */
// Main PPU state — holds all registers, VRAM, CGRAM, OAM, and rendering buffers.
struct Ppu {
  // -- Rendering state --
  bool lineHasSprites;     // True if current scanline has any visible sprites
  uint8_t lastBrightnessMult; // Cached brightness LUT version (rebuild on brightness change)
  uint8_t lastMosaicModulo;   // Cached mosaic LUT version (rebuild on mosaic size change)
  uint8_t renderFlags;     // Active render feature flags (see kPpuRenderFlags_*)
  bool renderWideHud;      // Draw BG3 HUD over the full widescreen viewport
  bool anchorWideHudBg3;   // Apply BG3 HUD anchoring only while the legacy gameplay strip is active
  const uint16_t *wideHudTilemap; // 64-column widescreen HUD tilemap supplied by the game layer
  uint8_t wideHudShadowSize; // Pixel width of the bottom/right HUD shadow
  uint32_t renderPitch;    // Byte stride between rows in the output renderBuffer
  uint8_t *renderBuffer;   // Pointer to the caller's output pixel buffer
  uint8_t extraLeftCur, extraRightCur, extraLeftRight, extraBottomCur; // Extended screen area (pixels)
  int16_t viewportLeftCur; // Source-column crop used when presenting the configured viewport
  float mode7PerspectiveLow, mode7PerspectiveHigh; // Perspective correction range for Mode 7

  // -- Screen enable / window masks (TMW $212E, TSW $212F, TM $212C, TS $212D) --
  uint8 screenEnabled[2];  // [0]=main screen, [1]=sub screen; bit per layer (BG1-4 + OBJ)
  uint8 screenWindowed[2]; // [0]=main window mask, [1]=sub window mask; bit per layer
  uint8 mosaicEnabled;     // Bitmask: which BG layers have mosaic effect active
  uint8 mosaicSize;        // Mosaic block size (1-16 pixels)
  // -- Object / sprite settings --
  uint16_t objTileAdr1;   // VRAM base address for sprite tiles (name base, OBSEL bits 0-2)
  uint16_t objTileAdr2;   // VRAM base address for second sprite name table (name select)
  uint8_t objSize;         // Sprite size selector (OBSEL bits 5-7): indexes size table
  // -- Hardware windows --
  uint8_t window1left;     // Window 1 left edge (register $2126)
  uint8_t window1right;    // Window 1 right edge (register $2127)
  uint8_t window2left;     // Window 2 left edge (register $2128)
  uint8_t window2right;    // Window 2 right edge (register $2129)
  uint32_t windowsel;      // Packed window logic per layer (W12SEL, W34SEL, WOBJSEL, WBGLOG, WOBJLOG)

  // -- Color math (registers $2130-$2132) --
  uint8_t clipMode;        // Color clip mode: 0=never, 1=outside window, 2=inside, 3=always
  uint8_t preventMathMode; // Prevent math: same encoding as clipMode
  bool addSubscreen;       // True = add sub screen; false = add fixed color
  bool subtractColor;      // True = subtract instead of add
  bool halfColor;          // True = halve the result of color math
  uint8 mathEnabled;       // Bitmask: which layers participate in color math
  uint8_t fixedColorR, fixedColorG, fixedColorB; // Fixed color for color math (register $2132)
  // -- Display settings --
  bool forcedBlank;        // When true, screen is black (INIDISP bit 7)
  uint8_t brightness;      // Master brightness 0-15 (INIDISP bits 0-3)
  uint8_t mode;            // BG mode 0-7 (BGMODE register bits 0-2)

  // -- VRAM access ports (registers $2115-$2119, $2139-$213A) --
  uint16_t vramPointer;    // Current VRAM word address for read/write
  uint16_t vramIncrement;  // VRAM address increment step (1, 32, or 128 words)
  bool vramIncrementOnHigh; // If true, increment after high-byte access; else after low-byte
  // -- CGRAM access ports (registers $2121-$2122, $213B) --
  uint8_t cgramPointer;   // Current palette entry index (0-255)
  bool cgramSecondWrite;   // Toggle: CGRAM writes alternate between low and high bytes
  uint8_t cgramBuffer;    // Latch for the first byte of a CGRAM word write
  // -- OAM access ports (registers $2101-$2104, $2138) --
  uint16_t oamAdr;         // Current OAM byte address for read/write
  bool oamSecondWrite;     // Toggle: OAM writes alternate between low and high bytes
  uint8_t oamBuffer;      // Latch for the first byte of an OAM word write

  // -- Background layers --
  BgLayer bgLayer[4];      // Configuration for BG1-BG4
  uint8_t scrollPrev;      // Previous value written to a scroll register (write-twice latch)
  uint8_t scrollPrev2;     // Second previous scroll value (for Mode 7 13-bit sign extension)

  // -- Mode 7 affine transformation (registers $211B-$2120) --
  int16_t m7matrix[8];    // [a, b, c, d, x, y, h, v]: rotation/scale matrix + center/scroll
  uint8_t m7prev;          // Previous byte written to a Mode 7 register (write-twice latch)
  bool m7largeField;       // When true, Mode 7 playfield extends beyond 1024×1024
  bool m7charFill;         // Fill mode for out-of-range Mode 7 pixels (tile 0 vs transparent)
  bool m7xFlip;            // Mirror Mode 7 horizontally
  bool m7yFlip;            // Mirror Mode 7 vertically
  bool m7extBg_always_zero; // Mode 7 EXTBG flag (always zero in this emulator)
  // -- Mode 7 per-scanline computed values --
  int32_t m7startX;        // Affine-transformed X start position for current scanline
  int32_t m7startY;        // Affine-transformed Y start position for current scanline

  // -- Memory arrays --
  uint16_t oam[0x110];    // Object Attribute Memory: 128 sprites × 4 bytes + 32 bytes high table
  uint8_t brightnessMult[32 + 31]; // Brightness LUT (31 extra entries avoid clamping)
  uint8_t brightnessMultHalf[32 * 2]; // Half-brightness LUT for half-color math
  uint16_t cgram[0x100];  // Color Generator RAM: 256 palette entries (15-bit BGR)
  uint8_t mosaicModulo[kPpuXPixels]; // Precomputed mosaic column grouping per pixel
  uint32_t colorMapRgb[256]; // Palette mapped to output RGB format (after brightness)
  PpuPixelPrioBufs bgBuffers[2]; // Scanline render buffers for background layers
  PpuPixelPrioBufs objBuffer;    // Scanline render buffer for sprites
  uint16_t vram[0x8000];  // Video RAM: 64KB (32K words) for tilemaps and tile graphics
};

/*
 * Public API.
 *
 * Typical lifecycle:
 *   ppu = ppu_init();
 *   ppu_reset(ppu);
 *   loop per frame:
 *     PpuBeginDrawing(ppu, dst_buffer, pitch, flags);
 *     ppu_runLine(ppu, line) for each visible line;
 *     ppu_handleVblank(ppu);
 *   ppu_free(ppu);
 *
 * ppu_read/ppu_write are called by the CPU emulator whenever the game
 * touches one of the $21xx PPU registers.
 */
// Allocate and return a new PPU with zeroed state.
Ppu* ppu_init();
// Free the PPU and its allocated memory.
void ppu_free(Ppu* ppu);
// Reset all PPU registers and state to power-on defaults.
void ppu_reset(Ppu* ppu);
// Called at the start of vertical blank: resets OAM address, prepares for next frame.
void ppu_handleVblank(Ppu* ppu);
// Render one scanline (0-223/239) into the output buffer using current PPU state.
void ppu_runLine(Ppu* ppu, int line);
// Read from a PPU register ($2134-$213F): multiplication result, latch data, etc.
uint8_t ppu_read(Ppu* ppu, uint8_t adr);
// Write to a PPU register ($2100-$2133): display, scroll, VRAM/CGRAM/OAM ports, etc.
void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val);
// Serialize or deserialize all PPU state for save/load snapshots.
void ppu_saveload(Ppu *ppu, SaveLoadFunc *func, void *ctx);
// Set up the output buffer and render flags for the upcoming frame.
void PpuBeginDrawing(Ppu *ppu, uint8_t *buffer, size_t pitch, uint32_t render_flags);
// Returns the current render scale: 1 (256px), 2 (512px), or 4 (1024px for 4×Mode7).
int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags);
// Configure perspective correction strength for Mode 7 rendering.
// `low` and `high` define the scanline range over which correction ramps.
void PpuSetMode7PerspectiveCorrection(Ppu *ppu, int low, int high);
// Configure extra pixels rendered beyond the standard 256×224 area.
void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom);
void PpuSetRenderWideHud(Ppu *ppu, bool enabled, bool anchor_bg3, const uint16_t *tilemap, uint8_t shadow_size);

#endif  // ZELDA3_SNES_PPU_H_
