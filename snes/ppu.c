/*
 * ppu.c — SNES Picture Processing Unit rendering implementation.
 *
 * Renders the SNES video output scanline-by-scanline. Each line processes:
 *   1. Background layers: fetch tilemap entries from VRAM, decode tile pixels,
 *      apply scrolling and mosaic effects. Modes 0-6 use tiled rendering;
 *      Mode 7 uses affine matrix transformation.
 *   2. Sprites (OBJ): evaluate OAM for visible sprites on the current line,
 *      decode their tile data, respect priority and palette settings.
 *   3. Window masking: clip layers per-pixel using two configurable windows.
 *   4. Color math: composite main screen and sub screen using add/subtract
 *      with optional halving, or blend with a fixed color.
 *   5. Brightness: apply master brightness (0-15) to the final RGB output.
 *
 * Two rendering paths exist: the original per-pixel renderer (handlePixel)
 * and a newer whole-line renderer (PpuDrawWholeLine) selected by renderFlags.
 */

/*
 * ============================================================================
 * Modder primer — read this first if you're new to SNES PPU internals.
 * ============================================================================
 *
 * Per-frame pipeline (driven by the host emulator loop):
 *
 *   PpuBeginDrawing(buffer, pitch, flags)        - point output buffer
 *      \-> rebuilds brightnessMult / brightnessMultHalf LUTs if brightness
 *          changed, and pre-converts CGRAM to RGB if 4x Mode-7 will run.
 *
 *   for each visible line (1..224 or 1..240):
 *      ppu_runLine(line)                         - top-level scanline driver
 *         \-> rebuilds mosaicModulo LUT if mosaicSize changed
 *         \-> ppu_evaluateSprites(line - 1)      - decodes OAM into objBuffer
 *         \-> branch on renderFlags:
 *              new path: PpuDrawWholeLine(line)
 *                  \-> PpuDrawBackgrounds (main, then sub)
 *                       \-> PpuDrawBackground_{4bpp,2bpp}[_mosaic]
 *                       \-> PpuDrawBackground_mode7
 *                       \-> PpuDrawSprites
 *                  \-> color-math compositor walks cwin regions, writes RGB
 *              legacy path: ppu_calculateMode7Starts + for x in 0..255:
 *                  \-> ppu_handlePixel(x, line)
 *                       \-> ppu_getPixel for main and (optional) sub
 *                            \-> ppu_getPixelForBgLayer / ForMode7
 *
 * Why two paths?
 *   - The "legacy" path (ppu_handlePixel + ppu_getPixel) is closer to the
 *     hardware spec and easier to follow. It runs once per pixel and is
 *     more general — it handles every SNES BG mode. It is slow.
 *   - The "new" path (PpuDrawWholeLine + PpuDrawBackground_*) renders a
 *     whole line of one layer at a time into PpuPixelPrioBufs, then
 *     resolves the per-pixel winner with a single uint16 compare. It is
 *     much faster but is specialized to the BG modes Zelda 3 uses (mode 1
 *     and mode 7).
 *
 * Z-buffer trick:
 *   Both bgBuffers[0/1] and objBuffer hold PpuZbufType = uint16 entries.
 *   The HIGH byte is a "priority bucket" (see PpuDrawBackgrounds for the
 *   bucket-number table); the LOW byte is the CGRAM palette index. A plain
 *   numeric compare on the full uint16 therefore resolves layering: higher
 *   priority bucket wins, and within the same bucket, brighter palette
 *   indexes win (which doesn't matter in practice because only one layer
 *   writes a given bucket per pixel). All buffers are pre-cleared to
 *   0x0500 (priority 5 = backdrop, color 0).
 *
 * Window evaluation:
 *   Each layer can be clipped by up to two of the four hardware windows
 *   (window1, window2 — each defined by a left and right column). The
 *   per-layer logic op (OR/AND/XOR/XNOR) is packed into ppu->windowsel.
 *   PpuWindows_Calc() converts these into a sorted list of edges (up to 6)
 *   spanning the line, plus a "bits" mask saying which spans are masked
 *   off for that layer. The renderers iterate spans and skip the masked
 *   ones.
 *
 * Color math:
 *   Once the main screen and (optionally) sub screen are composited into
 *   bgBuffers[0] and bgBuffers[1], the per-pixel color math step (in
 *   PpuDrawWholeLine) computes:
 *       final = clip(main +/- (sub or fixed_color), [0,31])
 *   with optional halving of the result. The "clip window" gates the
 *   black-clipping of main, and the "math window" gates whether math
 *   applies at all — both encoded in cw_clip_math bits.
 *
 * Mode 7 specifics:
 *   Mode 7 replaces tiled backgrounds with a single 128×128-tile affine
 *   plane. The transformation is:
 *       [ X ]   [ a b ] [ x - hScroll ]   [ xCenter ]
 *       [ Y ] = [ c d ] [ y - vScroll ] + [ yCenter ]
 *   The matrix lives in m7matrix[0..3] (a/b/c/d), the scroll in
 *   m7matrix[6..7], and the center in m7matrix[4..5]. All values are
 *   13-bit signed and must be sign-extended (the "<<3, >>3" idiom).
 *   ppu_calculateMode7Starts() pre-computes the per-scanline start (X,Y),
 *   then ppu_getPixelForMode7() steps by (a,c) per output column.
 *
 * Zelda 3 only ever uses BG mode 1 and BG mode 7. Several ppu_write() cases
 * carry assert() statements that document this assumption (e.g. asserting
 * BGMODE writes are 7 or 9, M7SEL writes are 0x80, WBGLOG/WOBJLOG = 0).
 * Removing the asserts won't help an enhancement port directly — the
 * specialized renderers genuinely only implement what Zelda needs.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include "ppu.h"
#include "src/types.h"

/*
 * kSpriteSizes — the eight {small, large} sprite-size pairs the SNES PPU
 * supports, indexed by OBSEL ($2101) bits 5-7. The OAM entry's "large
 * sprite" bit (in the high-table) then selects which of the pair this
 * particular sprite uses.
 *
 * Zelda 3 uses index 0 ({8,16}) almost exclusively, occasionally
 * switching to {8,32} or {16,32} for big sprites. Indexes 6 and 7 are
 * documented as "{16,32}" but on real hardware behave specially
 * (rectangular sprites) — Zelda doesn't use them so the table is set
 * to the same {16,32} for both.
 */
// Sprite size table indexed by OBSEL bits 5-7.
// Each entry is {small_size, large_size} in pixels.
static const uint8 kSpriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

// Forward declarations for internal rendering functions.
static void ppu_handlePixel(Ppu* ppu, int x, int y);        // Per-pixel compositor (legacy path)
static int ppu_getPixel(Ppu* ppu, int x, int y, bool sub, int* r, int* g, int* b); // Resolve final pixel color
static int ppu_getPixelForBgLayer(Ppu *ppu, int x, int y, int layer, bool priority); // BG tile lookup
static void ppu_calculateMode7Starts(Ppu* ppu, int y);      // Compute Mode 7 affine start coords
static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority); // Mode 7 pixel fetch
static bool ppu_getWindowState(Ppu* ppu, int layer, int x); // Test if pixel is inside window region
static bool ppu_evaluateSprites(Ppu* ppu, int line);        // Build sprite buffer for scanline
static void PpuDrawWholeLine(Ppu *ppu, uint y);             // Whole-line renderer (new path)

/*
 * Layer/window quick-test macros. The SNES exposes per-layer enable bits
 * in four registers:
 *   TM   ($212C) - main-screen layer enables (sub index 0)
 *   TS   ($212D) - sub-screen layer enables  (sub index 1)
 *   TMW  ($212E) - main-screen window-clip enables
 *   TSW  ($212F) - sub-screen window-clip enables
 * Each is 5 bits wide: bit 0 = BG1, bit 1 = BG2, bit 2 = BG3, bit 3 = BG4,
 * bit 4 = OBJ (sprites). screenEnabled[sub]/screenWindowed[sub] are the
 * shadows of these registers; passing layer = 5 to GET_WINDOW_FLAGS asks
 * about the *color* window (used by the math step), not a real BG layer.
 *
 * GET_WINDOW_FLAGS extracts the 4-bit window selector for one layer out
 * of the packed `windowsel` field: bits 0..3 = BG1, 4..7 = BG2, ...,
 * 16..19 = OBJ, 20..23 = the color-math window. See the kWindow*
 * enum below for the individual bit meanings.
 */
// Layer enable/window macros — test bits in the TM/TS/TMW/TSW register mirrors.
#define IS_SCREEN_ENABLED(ppu, sub, layer) (ppu->screenEnabled[sub] & (1 << layer))
#define IS_SCREEN_WINDOWED(ppu, sub, layer) (ppu->screenWindowed[sub] & (1 << layer))
#define IS_MOSAIC_ENABLED(ppu, layer) ((ppu->mosaicEnabled & (1 << layer)))

static void PpuDrawWideHudOverlay(Ppu *ppu, uint y, uint32 *dst_org);
#define GET_WINDOW_FLAGS(ppu, layer) (ppu->windowsel >> (layer * 4))

// Window logic flags — extracted from the packed windowsel field per layer.
enum {
  kWindow1Inversed = 1,  // Invert window 1 region for this layer
  kWindow1Enabled = 2,   // Window 1 is active for this layer
  kWindow2Inversed = 4,  // Invert window 2 region for this layer
  kWindow2Enabled = 8,   // Window 2 is active for this layer
};


/*
 * ppu_init — Allocate a Ppu instance. Note that the struct is NOT zeroed
 * here; the caller is expected to follow up with ppu_reset() which clears
 * VRAM, CGRAM, OAM, and all register shadows to power-on defaults. Only
 * extraLeftRight (the widescreen pixel count) is initialised so it is
 * stable across resets.
 */
Ppu* ppu_init() {
  Ppu* ppu = (Ppu * )malloc(sizeof(Ppu));
  ppu->extraLeftRight = kPpuExtraLeftRight;
  return ppu;
}

/*
 * ppu_free — Release a Ppu allocated by ppu_init(). The struct is
 * malloc'd flat (no embedded heap pointers), so a single free() is all
 * that's needed.
 */
void ppu_free(Ppu* ppu) {
  free(ppu);
}

/*
 * ppu_reset — Initialise the PPU to a known "powered-on" state.
 *
 * Clears VRAM/CGRAM/OAM to zero, resets all register shadows to plausible
 * defaults, and sets the LUT-version sentinels (lastBrightnessMult /
 * lastMosaicModulo) to 0xff so the next PpuBeginDrawing / ppu_runLine
 * call will rebuild them on first use.
 *
 * Notable non-zero defaults:
 *   - mosaicSize        = 1   (no mosaic; size 1 means the LUT is a no-op)
 *   - objTileAdr1/2     = $4000/$5000 (matches Zelda's OBSEL layout)
 *   - m7largeField      = true (Mode 7 wraps past 1024)
 *   - forcedBlank       = true (screen black until BGMODE sets it false)
 */
void ppu_reset(Ppu* ppu) {
  memset(ppu->vram, 0, sizeof(ppu->vram));
  ppu->lastBrightnessMult = 0xff;
  ppu->lastMosaicModulo = 0xff;
  ppu->extraLeftCur = 0;
  ppu->extraRightCur = 0;
  ppu->extraBottomCur = 0;
  ppu->viewportLeftCur = 0;
  ppu->vramPointer = 0;
  ppu->vramIncrementOnHigh = false;
  ppu->vramIncrement = 1;
  memset(ppu->cgram, 0, sizeof(ppu->cgram));
  ppu->cgramPointer = 0;
  ppu->cgramSecondWrite = false;
  ppu->cgramBuffer = 0;
  memset(ppu->oam, 0, sizeof(ppu->oam));
  ppu->oamAdr = 0;
  ppu->oamSecondWrite = false;
  ppu->oamBuffer = 0;
  ppu->objTileAdr1 = 0x4000;
  ppu->objTileAdr2 = 0x5000;
  ppu->objSize = 0;
  memset(&ppu->objBuffer, 0, sizeof(ppu->objBuffer));
  for(int i = 0; i < 4; i++) {
    ppu->bgLayer[i].hScroll = 0;
    ppu->bgLayer[i].vScroll = 0;
    ppu->bgLayer[i].tilemapWider = false;
    ppu->bgLayer[i].tilemapHigher = false;
    ppu->bgLayer[i].tilemapAdr = 0;
    ppu->bgLayer[i].tileAdr = 0;
  }
  ppu->scrollPrev = 0;
  ppu->scrollPrev2 = 0;
  ppu->mosaicSize = 1;
  ppu->screenEnabled[0] = ppu->screenEnabled[1] = 0;
  ppu->screenWindowed[0] = ppu->screenWindowed[1] = 0;
  memset(ppu->m7matrix, 0, sizeof(ppu->m7matrix));
  ppu->m7prev = 0;
  ppu->m7largeField = true;
  ppu->m7charFill = false;
  ppu->m7xFlip = false;
  ppu->m7yFlip = false;
  ppu->m7extBg_always_zero = false;
  ppu->m7startX = 0;
  ppu->m7startY = 0;
  ppu->windowsel = 0;
  ppu->window1left = 0;
  ppu->window1right = 0;
  ppu->window2left = 0;
  ppu->window2right = 0;
  ppu->clipMode = 0;
  ppu->preventMathMode = 0;
  ppu->addSubscreen = false;
  ppu->subtractColor = false;
  ppu->halfColor = false;
  ppu->mathEnabled = 0;
  ppu->fixedColorR = 0;
  ppu->fixedColorG = 0;
  ppu->fixedColorB = 0;
  ppu->forcedBlank = true;
  ppu->brightness = 0;
  ppu->mode = 0;
}

/*
 * ppu_saveload — Stream PPU state through a SaveLoadFunc for snapshot
 * save/restore.
 *
 * The wire format is intentionally byte-compatible with the legacy
 * higan-format save states that this emulator was originally derived
 * from. That's why the function reads/writes large slabs of `tmp` filler
 * bytes between the real fields — those gaps correspond to register
 * banks and state fields that the higan layout reserved but that this
 * port doesn't need to persist (sprite caches, internal timers, etc.).
 *
 * Fields preserved:
 *   - vram        (64 KB - 0x8000 words)
 *   - cgram       (512 bytes = 256 palette entries)
 *   - For each of the 4 BG layers, only the 4-byte block
 *     {tilemapWider, tilemapHigher, tilemapAdr-low-byte,
 *      tilemapAdr-high-byte} — the "snapshot" range marked in the
 *     BgLayer struct definition. hScroll/vScroll and tileAdr are
 *     recomputed/restored from elsewhere (game RAM and BGxNBA writes).
 *
 * Total wire bytes: 0x8000*2 + 10 + 512 + 556 + 520 + 4*(4+4+4) + 123
 *                 = ~66 KB per snapshot.
 */
void ppu_saveload(Ppu *ppu, SaveLoadFunc *func, void *ctx) {
  uint8 tmp[556] = { 0 };

  func(ctx, &ppu->vram, 0x8000 * 2);
  func(ctx, tmp, 10);
  func(ctx, &ppu->cgram, 512);
  func(ctx, tmp, 556);
  func(ctx, tmp, 520);
  for (int i = 0; i < 4; i++) {
    func(ctx, tmp, 4);
    // Save the mid-BgLayer snapshot range: tilemapWider, tilemapHigher,
    // and tilemapAdr. Four bytes total because tilemapWider/Higher are
    // bools (1 byte each) and tilemapAdr is a uint16.
    func(ctx, &ppu->bgLayer[i].tilemapWider, 4);
    func(ctx, tmp, 4);
  }
  func(ctx, tmp, 123);
}

/*
 * PpuGetCurrentRenderScale — Tells the host what horizontal/vertical
 * upscale factor the next frame will render at. The host uses this to
 * size its display window.
 *
 * Returns 4 only when ALL of these are true:
 *   - BG mode 7 is active (Mode 7 affine plane)
 *   - the screen is not in forced blank (otherwise no Mode 7 to draw)
 *   - both kPpuRenderFlags_4x4Mode7 AND kPpuRenderFlags_NewRenderer are
 *     set (the 4× upsampler lives in the new-renderer path).
 * Otherwise returns 1. Mode 0-6 always render at native 256×.
 */
int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags) {
  bool hq = ppu->mode == 7 && !ppu->forcedBlank &&
    (render_flags & (kPpuRenderFlags_4x4Mode7 | kPpuRenderFlags_NewRenderer)) == (kPpuRenderFlags_4x4Mode7 | kPpuRenderFlags_NewRenderer);
  return hq ? 4 : 1;
}

/*
 * PpuBeginDrawing — Per-frame setup. Must be called before the first
 * ppu_runLine of the frame.
 *
 * 1. Stores the output target (pixels/pitch/flags) on the Ppu so each
 *    scanline call can locate its destination row.
 * 2. Rebuilds the brightness LUTs if the master brightness register
 *    ($2100 / INIDISP) has changed since last frame.
 * 3. Pre-converts the 256-entry CGRAM palette to packed 24-bit RGB if
 *    the 4× Mode-7 upsampler is going to run this frame — that renderer
 *    bypasses the per-pixel math step and reads directly from
 *    colorMapRgb for speed.
 *
 * brightnessMult / brightnessMultHalf details:
 *   The SNES stores each color channel in 5 bits (range 0..31). To map
 *   that into an 8-bit display channel with master-brightness scaling,
 *   the LUT computes: ((i<<3) | (i>>2)) * brightness / 15.
 *      (i<<3) | (i>>2)   <-- 5-bit -> 8-bit, "round-trip" replicate so
 *                            that 31 -> 255 cleanly (not 248).
 *      * brightness / 15 <-- master brightness ramp.
 *   brightnessMultHalf is the same table at half intensity, used for the
 *   half-color color-math result.
 *   The "31 extra entries" trick: an additive color-math result can spill
 *   above 31, so the index is allowed to be 0..62 without an extra
 *   clamp; entries 32..62 all just mirror entry 31.
 */
void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t render_flags) {
  ppu->renderFlags = render_flags;
  ppu->renderPitch = (uint)pitch;
  ppu->renderBuffer = pixels;

  // Cache the brightness computation
  if (ppu->brightness != ppu->lastBrightnessMult) {
    uint8_t ppu_brightness = ppu->brightness;
    ppu->lastBrightnessMult = ppu_brightness;
    // Build both the normal and half-color LUTs in one pass. The half
    // LUT is indexed by 2*i so that the math-side code can reuse the
    // same brightnessMult-relative arithmetic without an extra shift.
    for (int i = 0; i < 32; i++)
      ppu->brightnessMultHalf[i * 2] = ppu->brightnessMultHalf[i * 2 + 1] = ppu->brightnessMult[i] =
      ((i << 3) | (i >> 2)) * ppu_brightness / 15;
    // Store 31 extra entries to remove the need for clamping to 31.
    memset(&ppu->brightnessMult[32], ppu->brightnessMult[31], 31);
  }

  // 4× Mode-7 path reads colorMapRgb directly per pixel, so pre-compute
  // the 256-entry brightness-scaled RGB table once per frame instead of
  // doing the BGR-to-RGB swizzle 256*224 times.
  if (PpuGetCurrentRenderScale(ppu, ppu->renderFlags) == 4) {
    for (int i = 0; i < 256; i++) {
      uint32 color = ppu->cgram[i];
      // CGRAM word is 0bbbbbgg gggrrrrr (15-bit BGR); pack into 0x00RRGGBB
      // after brightness scaling for direct memcpy into the 32-bit dst.
      ppu->colorMapRgb[i] = ppu->brightnessMult[color & 0x1f] << 16 | ppu->brightnessMult[(color >> 5) & 0x1f] << 8 | ppu->brightnessMult[(color >> 10) & 0x1f];
    }
  }
}

/*
 * ClearBackdrop — Reset a scanline z-buffer to the "backdrop" state so
 * any non-transparent layer pixel will out-prioritise it.
 *
 * Writes 0x0500 to every PpuZbufType (uint16) slot:
 *   priority byte = 0x05 -> bucket "5: backdrop" (lowest non-zero bucket)
 *   color byte    = 0x00 -> CGRAM index 0
 *
 * The loop fills four uint16 slots per iteration via one uint64 store,
 * which compilers turn into a tight SIMD-ish memset on most targets.
 * countof(buf->data) is always a multiple of 4 because kPpuExtraLeftRight
 * is either 0 or 96 and the base width is 256.
 */
static inline void ClearBackdrop(PpuPixelPrioBufs *buf) {
  for (size_t i = 0; i != countof(buf->data); i += 4)
    *(uint64*)&buf->data[i] = 0x0500050005000500;
}


/*
 * ppu_runLine — Top-level scanline driver. Called once per visible line
 * by the host emulator loop (typically from a per-scanline IRQ handler).
 *
 * `line` is the 1-based SNES scanline number. line == 0 is the dummy
 * "vblank line" that the SNES counts before line 1; we skip it because
 * the actual visible region starts at 1 and ends at 224 (or 240 with
 * Height240). Output goes into renderBuffer at row (line - 1).
 *
 * Per-line work:
 *   1. Rebuild mosaicModulo if mosaicSize has changed since last frame.
 *      mosaicModulo[i] is "the X coordinate of the leftmost pixel in the
 *      mosaic block that contains pixel i" — used by the mosaic
 *      renderers to share one tile-decode across `mosaicSize` columns.
 *   2. Evaluate OAM into objBuffer for this line; remember if any
 *      sprites were placed.
 *   3. If we're past the visible bottom of the screen, blank the row
 *      and return (the renderer doesn't bother going further).
 *   4. Dispatch to the whole-line renderer or the legacy per-pixel
 *      compositor.
 *   5. In the legacy path only, zero out the widescreen padding columns
 *      at the left and right edges (the new path handles this
 *      internally as part of its compositing loop).
 */
// Render one complete scanline. Rebuilds mosaic and brightness LUTs if changed,
// evaluates sprites for the line, then dispatches to either the whole-line
// renderer (new path) or per-pixel compositor (legacy path) based on renderFlags.
void ppu_runLine(Ppu *ppu, int line) {
  if(line != 0) {
    // Rebuild the mosaic-column-grouping LUT if MOSAIC ($2106) was just
    // written. mosaicModulo[i] - i gives the X offset back to the
    // top-left of the mosaic block i belongs to; the renderers use this
    // to fetch one tile and stamp it across `mosaicSize` columns.
    if (ppu->mosaicSize != ppu->lastMosaicModulo) {
      int mod = ppu->mosaicSize;
      ppu->lastMosaicModulo = mod;
      for (int i = 0, j = 0; i < countof(ppu->mosaicModulo); i++) {
        ppu->mosaicModulo[i] = i - j;
        j = (j + 1 == mod ? 0 : j + 1);
      }
    }
    // evaluate sprites
    ClearBackdrop(&ppu->objBuffer);
    // Sprites are evaluated against scanline (line - 1) because OAM
    // y-coordinates are one less than the line they appear on (SNES
    // hardware uses "y of top edge"; line 1 displays the pixel at y == 0).
    ppu->lineHasSprites = !ppu->forcedBlank && ppu_evaluateSprites(ppu, line - 1);

    // outside of visible range?
    // 225 = (224 visible lines) + 1 (the 1-based offset). extraBottomCur
    // is the widescreen-mode extra rows below the standard 224.
    if (line >= 225 + ppu->extraBottomCur) {
      memset(&ppu->renderBuffer[(line - 1) * ppu->renderPitch], 0, sizeof(uint32) * (256 + ppu->extraLeftRight * 2));
      return;
    }

    if (ppu->renderFlags & kPpuRenderFlags_NewRenderer) {
      PpuDrawWholeLine(ppu, line);
    } else {
      // Mode 7 needs its per-line affine start (m7startX/Y) pre-computed
      // before any pixel queries — the legacy path queries one pixel at
      // a time but they all share that start.
      if (ppu->mode == 7)
        ppu_calculateMode7Starts(ppu, line);
      for (int x = 0; x < 256; x++)
        ppu_handlePixel(ppu, x, line);

      // Clear the widescreen padding the legacy compositor didn't write.
      uint8 *dst = ppu->renderBuffer + ((line - 1) * ppu->renderPitch);
      if (ppu->extraLeftRight != 0) {
        memset(dst, 0, sizeof(uint32) * ppu->extraLeftRight);
        memset(dst + sizeof(uint32) * (256 + ppu->extraLeftRight), 0, sizeof(uint32) * ppu->extraLeftRight);
      }
      PpuDrawWideHudOverlay(ppu, line, (uint32*)dst);
    }
  }
}

/*
 * PpuWindows — Per-layer window-evaluation result for one scanline.
 *
 * The SNES has two hardware windows (window1, window2). Each layer chooses
 * which windows apply to it, whether each is inverted, and which logic op
 * (OR/AND/XOR/XNOR) combines them. PpuWindows_Calc() resolves all of that
 * for one layer down to:
 *
 *   edges[0..nr]  - up to 6 sorted X coordinates partitioning the line
 *                   into nr regions ([edges[0], edges[1]), [edges[1],
 *                   edges[2]), ...). edges[0] is the leftmost output
 *                   column (which is negative when widescreen is active)
 *                   and edges[nr] is the rightmost (256 or 256 +
 *                   extraRightCur).
 *   nr            - number of regions (1..5).
 *   bits          - one bit per region: 1 = layer is *masked off* in
 *                   that region (don't draw), 0 = visible.
 *
 * The renderers walk regions in left-to-right order and skip the ones
 * whose bit is set.
 */
typedef struct PpuWindows {
  int16 edges[6];
  uint8 nr;
  uint8 bits;
} PpuWindows;

/*
 * PpuWindows_Clear — Trivial "no windows configured" path: report a
 * single visible region spanning the whole line. Used when the layer's
 * TMW/TSW bit is 0 (window-clip disabled for this layer).
 *
 * layer == 2 is BG3 which doesn't get the widescreen side padding (the
 * widescreen feature only extends BG1/BG2/OBJ). Other layers extend
 * leftward by extraLeftCur and rightward by extraRightCur.
 */
static void PpuWindows_Clear(PpuWindows *win, Ppu *ppu, uint layer) {
  win->edges[0] = -(layer != 2 ? ppu->extraLeftCur : 0);
  win->edges[1] = 256 + (layer != 2 ? ppu->extraRightCur : 0);
  win->nr = 1;
  win->bits = 0;
}

/*
 * PpuWindows_Calc — Full window evaluation for one layer on one line.
 *
 * Builds the sorted edge list and the per-region mask `bits` from the
 * raw window registers (window1/window2 left/right and the layer's
 * 4-bit window selector). The implementation is a minor reformulation
 * of the classic Snes9x window-merger.
 *
 * The 4-bit per-layer window selector contains:
 *   bit 0 (kWindow1Inversed) - if set with Enabled, the layer is masked
 *                              *outside* window1 instead of inside.
 *   bit 1 (kWindow1Enabled)  - window1 participates.
 *   bit 2 (kWindow2Inversed) - mirror of bit 0 for window2.
 *   bit 3 (kWindow2Enabled)  - window2 participates.
 *
 * Algorithm:
 *   1. Start with the trivial edge set [leftmost, rightmost].
 *   2. If window1 is in-bounds, splice its left and right+1 columns into
 *      the sorted edge list (no-op if either already an edge).
 *   3. Repeat for window2 with an explicit insertion-sort.
 *   4. Now edges[] partitions the line. Build w1_bits and w2_bits — bit
 *      `r` of each says "region r overlaps window1 / window2".
 *   5. Apply the inversion flags to each, then OR them. The final `bits`
 *      is the union of the two windows' coverage; the renderer treats
 *      `bits=1 regions` as "skip" (this matches the SNES convention that
 *      the WINDOW affects the *layer-clip* TMW/TSW which says "mask".)
 *
 * Maximum of 5 regions: 4 inserted edges (w1.left, w1.right+1, w2.left,
 * w2.right+1) plus the two outer endpoints = up to 6 edges = 5 spans;
 * `edges[6]` accommodates this.
 */
static void PpuWindows_Calc(PpuWindows *win, Ppu *ppu, uint layer) {
  // Evaluate which spans to render based on the window settings.
  // There are at most 5 windows.
  // Algorithm from Snes9x
  uint32 winflags = GET_WINDOW_FLAGS(ppu, layer);
  uint nr = 1;
  int window_right = 256 + (layer != 2 ? ppu->extraRightCur : 0);
  win->edges[0] = - (layer != 2 ? ppu->extraLeftCur : 0);
  win->edges[1] = window_right;
  uint i, j;
  int t;
  bool w1_ena = (winflags & kWindow1Enabled) && ppu->window1left <= ppu->window1right;
  if (w1_ena) {
    if (ppu->window1left > win->edges[0]) {
      win->edges[nr] = ppu->window1left;
      win->edges[++nr] = window_right;
    }
    if (ppu->window1right + 1 < window_right) {
      win->edges[nr] = ppu->window1right + 1;
      win->edges[++nr] = window_right;
    }
  }
  bool w2_ena = (winflags & kWindow2Enabled) && ppu->window2left <= ppu->window2right;
  if (w2_ena) {
    for (i = 0; i <= nr && (t = ppu->window2left) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
    for (; i <= nr && (t = ppu->window2right + 1) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
  }
  win->nr = nr;
  // get a bitmap of how regions map to windows
  uint8 w1_bits = 0, w2_bits = 0;
  if (w1_ena) {
    for (i = 0; win->edges[i] != ppu->window1left; i++);
    for (j = i; win->edges[j] != ppu->window1right + 1; j++);
    w1_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow1Enabled | kWindow1Inversed)) == (kWindow1Enabled | kWindow1Inversed))
    w1_bits = ~w1_bits;
  if (w2_ena) {
    for (i = 0; win->edges[i] != ppu->window2left; i++);
    for (j = i; win->edges[j] != ppu->window2right + 1; j++);
    w2_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow2Enabled | kWindow2Inversed)) == (kWindow2Enabled | kWindow2Inversed))
    w2_bits = ~w2_bits;
  win->bits = w1_bits | w2_bits;
}

/*
 * PpuDrawBackground_4bpp — Render one scanline of a 4-bit-per-pixel
 * background layer into the per-pixel priority z-buffer.
 *
 * Parameters:
 *   y      - destination scanline (in screen space, before scrolling).
 *   sub    - false = render into the main-screen bgBuffers[0],
 *            true  = render into the sub-screen bgBuffers[1].
 *   layer  - 0..3 selecting BG1..BG4 (controls which tilemap/tile-data
 *            VRAM addresses to consult).
 *   zhi/zlo - priority bucket values to stamp into the z-buffer's high
 *             byte. zhi is used for tilemap entries with prio=1, zlo
 *             for prio=0. The 4 priority buckets per BG-mode are set
 *             up by the caller (PpuDrawBackgrounds) so a single uint16
 *             compare picks the right layer at composite time.
 *
 * SNES 4bpp tile layout in VRAM (16 words = 32 bytes per 8×8 tile):
 *      offset 0..7  : interleaved bitplane0/bitplane1 (low byte plane0,
 *                     high byte plane1, one word per row of the tile)
 *      offset 8..15 : bitplane2/bitplane3
 *   Each row in this implementation is loaded as a single 32-bit value
 *   via READ_BITS: the 16-bit word at (ta) and the 16-bit word at
 *   (ta + 8), concatenated low-half/high-half. The DO_PIXEL macros then
 *   extract bits at offsets {i, 7+i, 14+i, 21+i} to assemble one 4-bit
 *   pixel value — the bits at positions 0, 7, 14, 21 are the four
 *   plane bits for column 0 of the row (because plane0 starts at bit 0,
 *   plane1 at bit 8 = 7+i with i=1, plane2 at bit 16 = 14+i, plane3 at
 *   bit 24 = 21+i, and offset i shifts to column i).
 *
 * Tilemap entry word layout (one 16-bit word per 8×8 tile of the
 * tilemap, stored in VRAM at bglayer->tilemapAdr):
 *      bits 15  = vflip (1: read tile rows bottom-up)
 *      bits 14  = hflip (1: read tile columns right-to-left, use
 *                       DO_PIXEL_HFLIP variant)
 *      bit  13  = priority bit (selects zhi vs zlo)
 *      bits 12..10 = palette block (0..7); shifted by kPaletteShift=6
 *                    to land in the high nibble of the low-byte that
 *                    addresses the 16-color sub-palette in CGRAM
 *      bits  9..0  = 10-bit tile number (0..1023)
 *
 * Tilemap quadrant arithmetic:
 *   The tilemap is one of 32×32, 64×32, 32×64, or 64×64 tiles. Each
 *   32×32 quadrant is stored at +$000, +$400, +$800, +$C00 from
 *   tilemapAdr. The y/x scrolled coordinate's high bit selects which
 *   quadrant pair, and `tps[0]/tps[1]` are pre-loaded pointers to the
 *   two horizontally-adjacent quadrants so NEXT_TP can move between
 *   them without rechecking the wider/higher flags inside the inner
 *   loop.
 *
 * Per-region structure inside the loop:
 *   1. If the starting X is in mid-tile (x & 7), handle the left
 *      partial-tile span pixel-by-pixel.
 *   2. Walk whole 8-pixel tiles with an unrolled DO_PIXEL × 8.
 *   3. If the right end is mid-tile, handle the trailing partial-tile
 *      pixel-by-pixel.
 */
// Draw a whole line of a 4bpp background layer into bgBuffers
static void PpuDrawBackground_4bpp(Ppu *ppu, uint y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {
#define DO_PIXEL(i) do { \
  pixel = (bits >> i) & 1 | (bits >> (7 + i)) & 2 | (bits >> (14 + i)) & 4 | (bits >> (21 + i)) & 8; \
  if ((bits & (0x01010101 << i)) && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define DO_PIXEL_HFLIP(i) do { \
  pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2 | (bits >> (21 - i)) & 4 | (bits >> (28 - i)) & 8; \
  if ((bits & (0x80808080 >> i)) && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 16) & 0x7fff], addr[0] | addr[8] << 16)
  enum { kPaletteShift = 6 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer) : PpuWindows_Clear(&win, ppu, layer);
  BgLayer *bglayer = &ppu->bgLayer[layer];
  y += bglayer->vScroll;
  int sc_offs = bglayer->tilemapAdr + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && bglayer->tilemapHigher)
    sc_offs += bglayer->tilemapWider ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (bglayer->tilemapWider ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = ppu->bgLayer[layer].tileAdr, pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  int fixed_layer_offset = (layer == 2 && ppu->anchorWideHudBg3) ?
      ppu->viewportLeftCur + (ppu->extraLeftRight >> 1) - ppu->extraLeftRight : 0;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    uint x = win.edges[windex] + bglayer->hScroll;
    uint w = win.edges[windex + 1] - win.edges[windex];
    PpuZbufType *dstz = ppu->bgBuffers[sub].data + win.edges[windex] + kPpuExtraLeftRight + fixed_layer_offset;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31;
    const uint16 *tp_next = tps[(x >> 8 & 1) ^ 1];
#define NEXT_TP() if (tp != tp_last) tp += 1; else tp = tp_next, tp_next = tp_last - 31, tp_last = tp + 31;
    // Handle clipped pixels on left side
    if (x & 7) {
      int curw = IntMin(8 - (x & 7), w);
      w -= curw;
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          bits >>= (x & 7), x += curw;
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --curw);
        } else {
          bits <<= (x & 7), x += curw;
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --curw);
        }
      } else {
        dstz += curw;
      }
    }
    // Handle full tiles in the middle
    while (w >= 8) {
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);
          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);
        } else {
          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);
          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);
        }
      }
      dstz += 8, w -= 8;
    }
    // Handle remaining clipped part
    if (w) {
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --w);
        } else {
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --w);
        }
      }
    }
  }
#undef READ_BITS
#undef DO_PIXEL
#undef DO_PIXEL_HFLIP
}

/*
 * PpuDrawBackground_2bpp — 2bpp variant of PpuDrawBackground_4bpp.
 *
 * Identical structure to the 4bpp path, except:
 *   - Tile data is 8 words / 16 bytes per 8×8 tile (only bitplanes 0
 *     and 1 exist). READ_BITS therefore reads a single uint16, not
 *     two combined words.
 *   - DO_PIXEL pulls only 2 plane bits per pixel instead of 4, so each
 *     pixel value is in 0..3.
 *   - kPaletteShift is 8 instead of 6: a 2bpp layer uses 4-color
 *     sub-palettes, and the palette bits land 2 positions further left
 *     in the final CGRAM index than for 4bpp.
 * Used for Zelda 3's BG3 in mode 1.
 */
// Draw a whole line of a 2bpp background layer into bgBuffers
static void PpuDrawBackground_2bpp(Ppu *ppu, uint y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {
#define DO_PIXEL(i) do { \
  pixel = (bits >> i) & 1 | (bits >> (7 + i)) & 2; \
  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define DO_PIXEL_HFLIP(i) do { \
  pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2; \
  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define READ_BITS(ta, tile) (addr = &ppu->vram[(ta) + (tile) * 8 & 0x7fff], addr[0])
  enum { kPaletteShift = 8 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer) : PpuWindows_Clear(&win, ppu, layer);
  BgLayer *bglayer = &ppu->bgLayer[layer];
  y += bglayer->vScroll;
  int sc_offs = bglayer->tilemapAdr + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && bglayer->tilemapHigher)
    sc_offs += bglayer->tilemapWider ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (bglayer->tilemapWider ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = ppu->bgLayer[layer].tileAdr, pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);

  const uint16 *addr;
  int fixed_layer_offset = (layer == 2 && ppu->anchorWideHudBg3) ?
      ppu->viewportLeftCur + (ppu->extraLeftRight >> 1) - ppu->extraLeftRight : 0;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    uint x = win.edges[windex] + bglayer->hScroll;
    uint w = win.edges[windex + 1] - win.edges[windex];
    PpuZbufType *dstz = ppu->bgBuffers[sub].data + win.edges[windex] + kPpuExtraLeftRight + fixed_layer_offset;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31;
    const uint16 *tp_next = tps[(x >> 8 & 1) ^ 1];

#define NEXT_TP() if (tp != tp_last) tp += 1; else tp = tp_next, tp_next = tp_last - 31, tp_last = tp + 31;
    // Handle clipped pixels on left side
    if (x & 7) {
      int curw = IntMin(8 - (x & 7), w);
      w -= curw;
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          bits >>= (x & 7), x += curw;
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --curw);
        } else {
          bits <<= (x & 7), x += curw;
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --curw);
        }
      } else {
        dstz += curw;
      }
    }
    // Handle full tiles in the middle
    while (w >= 8) {
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);
          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);
        } else {
          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);
          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);
        }
      }
      dstz += 8, w -= 8;
    }
    // Handle remaining clipped part
    if (w) {
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --w);
        } else {
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --w);
        }
      }
    }
  }
#undef NEXT_TP
#undef READ_BITS
#undef DO_PIXEL
#undef DO_PIXEL_HFLIP
}

/*
 * PpuDrawBackground_4bpp_mosaic — 4bpp BG draw with the mosaic effect.
 *
 * SNES mosaic blockifies the rendered image: each `mosaicSize` × N square
 * is filled with the top-left pixel of that block. This implementation
 * does it the cheap way:
 *
 *   1. Replace y with `mosaicModulo[y]` so all rows in a mosaic block
 *      look at the same scanline of tile data — that's why the outer
 *      caller doesn't have to repeat the row.
 *   2. Along X, only sample ONE pixel per block (GET_PIXEL, then the
 *      inner do-while fills `w` columns of dstz with that pixel).
 *
 * The block-fill loop tracks `w` (pixels left in the current block,
 * capped to dstz_end - dstz so we never run past the region). After
 * filling a block, advance the tile pointer by however many full
 * 8-pixel groups we just crossed, then continue with another block of
 * size `mosaicSize`.
 *
 * Used only when MOSAIC ($2106) has selected this BG layer.
 */
// Draw a whole line of a 4bpp background layer into bgBuffers, with mosaic applied
static void PpuDrawBackground_4bpp_mosaic(Ppu *ppu, uint y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {
#define GET_PIXEL() pixel = (bits) & 1 | (bits >> 7) & 2 | (bits >> 14) & 4 | (bits >> 21) & 8
#define GET_PIXEL_HFLIP() pixel = (bits >> 7) & 1 | (bits >> 14) & 2 | (bits >> 21) & 4 | (bits >> 28) & 8
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 16) & 0x7fff], addr[0] | addr[8] << 16)
  enum { kPaletteShift = 6 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer) : PpuWindows_Clear(&win, ppu, layer);
  BgLayer *bglayer = &ppu->bgLayer[layer];
  y = ppu->mosaicModulo[y] + bglayer->vScroll;
  int sc_offs = bglayer->tilemapAdr + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && bglayer->tilemapHigher)
    sc_offs += bglayer->tilemapWider ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (bglayer->tilemapWider ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = ppu->bgLayer[layer].tileAdr, pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  int fixed_layer_offset = (layer == 2 && ppu->anchorWideHudBg3) ?
      ppu->viewportLeftCur + (ppu->extraLeftRight >> 1) - ppu->extraLeftRight : 0;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int sx = win.edges[windex];
    PpuZbufType *dstz = ppu->bgBuffers[sub].data + sx + kPpuExtraLeftRight + fixed_layer_offset;
    PpuZbufType *dstz_end = ppu->bgBuffers[sub].data + win.edges[windex + 1] + kPpuExtraLeftRight + fixed_layer_offset;
    uint x = sx + bglayer->hScroll;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31, *tp_next = tps[(x >> 8 & 1) ^ 1];
    x &= 7;
    int w = ppu->mosaicSize - (sx - ppu->mosaicModulo[sx]);
    do {
      w = IntMin(w, dstz_end - dstz);
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (tile & 0x4000) bits >>= x, GET_PIXEL(); else bits <<= x, GET_PIXEL_HFLIP();
      if (pixel) {
        pixel += (tile & 0x1c00) >> kPaletteShift;
        int i = 0;
        do {
          if (z > dstz[i])
            dstz[i] = pixel + z;
        } while (++i != w);
      }
      dstz += w, x += w;
      for (; x >= 8; x -= 8)
        tp = (tp != tp_last) ? tp + 1 : tp_next;
      w = ppu->mosaicSize;
    } while (dstz_end - dstz != 0);
  }
#undef READ_BITS
#undef GET_PIXEL
#undef GET_PIXEL_HFLIP
}

/*
 * PpuDrawBackground_2bpp_mosaic — 2bpp variant of the mosaic path.
 *
 * Same algorithm as PpuDrawBackground_4bpp_mosaic; differs only in the
 * tile-data layout (one bitplane pair instead of two) and the palette
 * shift (8 instead of 6).
 */
// Draw a whole line of a 2bpp background layer into bgBuffers, with mosaic applied
static void PpuDrawBackground_2bpp_mosaic(Ppu *ppu, int y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {
#define GET_PIXEL() pixel = (bits) & 1 | (bits >> 7) & 2
#define GET_PIXEL_HFLIP() pixel = (bits >> 7) & 1 | (bits >> 14) & 2
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 8) & 0x7fff], addr[0])
  enum { kPaletteShift = 8 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer) : PpuWindows_Clear(&win, ppu, layer);
  BgLayer *bglayer = &ppu->bgLayer[layer];
  y = ppu->mosaicModulo[y] + bglayer->vScroll;
  int sc_offs = bglayer->tilemapAdr + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && bglayer->tilemapHigher)
    sc_offs += bglayer->tilemapWider ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (bglayer->tilemapWider ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = ppu->bgLayer[layer].tileAdr, pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  int fixed_layer_offset = (layer == 2 && ppu->anchorWideHudBg3) ?
      ppu->viewportLeftCur + (ppu->extraLeftRight >> 1) - ppu->extraLeftRight : 0;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int sx = win.edges[windex];
    PpuZbufType *dstz = ppu->bgBuffers[sub].data + sx + kPpuExtraLeftRight + fixed_layer_offset;
    PpuZbufType *dstz_end = ppu->bgBuffers[sub].data + win.edges[windex + 1] + kPpuExtraLeftRight + fixed_layer_offset;
    uint x = sx + bglayer->hScroll;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31, *tp_next = tps[(x >> 8 & 1) ^ 1];
    x &= 7;
    int w = ppu->mosaicSize - (sx - ppu->mosaicModulo[sx]);
    do {
      w = IntMin(w, dstz_end - dstz);
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (tile & 0x4000) bits >>= x, GET_PIXEL(); else bits <<= x, GET_PIXEL_HFLIP();
      if (pixel) {
        pixel += (tile & 0x1c00) >> kPaletteShift;
        uint i = 0;
        do {
          if (z > dstz[i])
            dstz[i] = pixel + z;
        } while (++i != w);
      }
      dstz += w, x += w;
      for (; x >= 8; x -= 8)
        tp = (tp != tp_last) ? tp + 1 : tp_next;
      w = ppu->mosaicSize;
    } while (dstz_end - dstz != 0);
  }
#undef READ_BITS
#undef GET_PIXEL
#undef GET_PIXEL_HFLIP
}


/*
 * Sprite priority encoding macros.
 *
 * SNES sprites have a 2-bit priority field per OAM entry (0..3) which
 * places them in one of four interleaved layers relative to BG1/BG2/BG3
 * (see the PpuDrawBackgrounds priority-bucket commentary). Plus the
 * SNES color-math step has the special rule that sprites with CGRAM
 * palette indexes 0xC0..0xFF do NOT participate in color math — those
 * are the "level6" sprites.
 *
 * SPRITE_PRIO_TO_PRIO packs both pieces into the 8-bit priority byte
 * of a PpuZbufType:
 *   (prio * 4 + 2)            -> the 4-bucket offset (2 keeps sprites
 *                                slightly above the BG priority that
 *                                shares a bucket)
 *   * 16                       -> shift into the high nibble of the prio
 *                                byte where the bucket ID lives
 *   + 4                        -> add a fixed "this is a sprite" offset
 *                                so sprites win over a same-bucket BG
 *                                pixel on a numeric compare
 *   + (level6 ? 2 : 0)         -> tag the "no color math" subset by
 *                                inflating the priority byte slightly,
 *                                which the math step decodes by
 *                                looking at bit 1 of the low nibble
 *
 * SPRITE_PRIO_TO_PRIO_HI returns just the high-nibble bucket ID so the
 * legacy ppu_getPixel() can compare it directly with curPriority.
 */
// level6 should be set if it's from palette 0xc0 which means color math is not applied
#define SPRITE_PRIO_TO_PRIO(prio, level6) (((prio) * 4 + 2) * 16 + 4 + (level6 ? 2 : 0))
#define SPRITE_PRIO_TO_PRIO_HI(prio) ((prio) * 4 + 2)

/*
 * PpuDrawSprites — Composite the per-line sprite buffer (objBuffer)
 * into the BG buffer (main or sub).
 *
 * objBuffer was already filled by ppu_evaluateSprites() at the top of
 * the scanline; each pixel there already carries its full priority byte
 * (computed via SPRITE_PRIO_TO_PRIO) and CGRAM index.
 *
 * Two modes via clear_backdrop:
 *   true  - In mode 1 (the normal Zelda path), the BG buffer hasn't been
 *           touched yet so a straight memcpy plants the sprite pixels
 *           with their priorities. The BG draw passes that follow will
 *           only overwrite slots where their bucket beats the sprite.
 *   false - In Mode 7, the BG layer has already been drawn (mode 7 is
 *           a single layer at z=0xc000), so sprites are merged
 *           pixel-by-pixel with a priority compare.
 *
 * The "layer = 4" choice is the OBJ pseudo-layer index used for window
 * lookups (the SNES OBJ window is logically layer 4 in our packed
 * windowsel field).
 */
static void PpuDrawSprites(Ppu *ppu, uint y, uint sub, bool clear_backdrop) {
  int layer = 4;
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer) : PpuWindows_Clear(&win, ppu, layer);
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int left = win.edges[windex];
    int width = win.edges[windex + 1] - left;
    PpuZbufType *src = ppu->objBuffer.data + left + kPpuExtraLeftRight;
    PpuZbufType *dst = ppu->bgBuffers[sub].data + left + kPpuExtraLeftRight;
    if (clear_backdrop) {
      memcpy(dst, src, width * sizeof(uint16));
    } else {
      do {
        if (src[0] > dst[0])
          dst[0] = src[0];
      } while (src++, dst++, --width);
    }
  }
}

/*
 * PpuDrawBackground_mode7 — Render one scanline of the Mode 7 affine
 * plane into the BG buffer.
 *
 * Mode 7 replaces tiled BGs with a single 128×128-tile, 256-color plane
 * stored entirely in VRAM. Each VRAM word holds: low byte = tilemap
 * entry (tile #), high byte = the pixel itself. So a tile lookup reads
 * `vram[(yy>>3)*128 + (xx>>3)] & 0xff` for the tile number, then
 * `vram[tile*64 + (yy & 7)*8 + (xx & 7)] >> 8` for the actual pixel
 * inside that 8×8 tile.
 *
 * Math:
 *   The (X, Y) destination in the tilemap is:
 *      X = m7a*(x - hScroll - xCenter) + m7b*(y - vScroll - yCenter)
 *          + xCenter
 *      Y = m7c*(x - hScroll - xCenter) + m7d*(y - vScroll - yCenter)
 *          + yCenter
 *   This function pre-computes the y-dependent part (m7startX/Y) and
 *   then walks x by dx/dy per column.
 *
 * 13-bit sign extension: hScroll/vScroll/xCenter/yCenter are stored as
 * 13-bit signed values inside 16-bit fields. The `((int16_t)(v << 3)) >> 3`
 * idiom shifts the sign bit into position 15, arithmetic-right-shifts
 * it back, and yields a properly sign-extended 16-bit int.
 *
 * largeField:
 *   If true (the default), out-of-range coordinates wrap or clip. If
 *   false, the entire field is treated as 1024×1024 without wrap.
 *   m7charFill picks between "render tile 0" and "render transparent"
 *   for out-of-range positions.
 *
 * The caller passes `z` directly (no zhi/zlo pair) because Mode 7 has
 * no per-tile priority bit.
 */
// Assumes it's drawn on an empty backdrop
static void PpuDrawBackground_mode7(Ppu *ppu, uint y, bool sub, PpuZbufType z) {
  int layer = 0;
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer) : PpuWindows_Clear(&win, ppu, layer);

  // expand 13-bit values to signed values
  int hScroll = ((int16_t)(ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t)(ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  bool mosaic_enabled = IS_MOSAIC_ENABLED(ppu, 0);
  if (mosaic_enabled)
    y = ppu->mosaicModulo[y];
  uint32 ry = ppu->m7yFlip ? 255 - y : y;
  uint32 m7startX = (ppu->m7matrix[0] * clippedH & ~63) + (ppu->m7matrix[1] * ry & ~63) +
    (ppu->m7matrix[1] * clippedV & ~63) + (xCenter << 8);
  uint32 m7startY = (ppu->m7matrix[2] * clippedH & ~63) + (ppu->m7matrix[3] * ry & ~63) +
    (ppu->m7matrix[3] * clippedV & ~63) + (yCenter << 8);
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int x = win.edges[windex], x2 = win.edges[windex + 1], tile;
    PpuZbufType *dstz = ppu->bgBuffers[sub].data + x + kPpuExtraLeftRight;
    PpuZbufType *dstz_end = ppu->bgBuffers[sub].data + x2 + kPpuExtraLeftRight;
    uint32 rx = ppu->m7xFlip ? 255 - x : x;
    uint32 xpos = m7startX + ppu->m7matrix[0] * rx;
    uint32 ypos = m7startY + ppu->m7matrix[2] * rx;
    uint32 dx = ppu->m7xFlip ? -ppu->m7matrix[0] : ppu->m7matrix[0];
    uint32 dy = ppu->m7xFlip ? -ppu->m7matrix[2] : ppu->m7matrix[2];
    uint32 outside_value = ppu->m7largeField ? 0x3ffff : 0xffffffff;
    bool char_fill = ppu->m7charFill;
    if (mosaic_enabled) {
      int w = ppu->mosaicSize - (x - ppu->mosaicModulo[x]);
      do {
        w = IntMin(w, dstz_end - dstz);
        if ((uint32)(xpos | ypos) > outside_value) {
          if (!char_fill)
            continue;
          tile = 0;
        } else {
          tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        }
        uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        if (pixel) {
          int i = 0;
          do dstz[i] = pixel + z; while (++i != w);
        }
      } while (xpos += dx * w, ypos += dy * w, dstz += w, w = ppu->mosaicSize, dstz_end - dstz != 0);
    } else {
      do {
        if ((uint32)(xpos | ypos) > outside_value) {
          if (!char_fill)
            continue;
          tile = 0;
        } else {
          tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        }
        uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        if (pixel)
          dstz[0] = pixel + z;
      } while (xpos += dx, ypos += dy, ++dstz != dstz_end);
    }
  }
}

/*
 * PpuSetMode7PerspectiveCorrection — Configure the 4×-upsampler's
 * synthetic perspective ramp.
 *
 * The upsampler can fake perspective by varying the Mode 7 a-coefficient
 * across the screen (the larger a is, the more zoomed-in). `low` is the
 * `a` divisor at the top of the screen, `high` at the bottom, with
 * linear interpolation between. Passing low == 0 disables perspective
 * (sets the ramp to "use a as-is" at every line).
 *
 * Stored as inverses so the per-scanline math is a multiplication
 * rather than a division.
 */
void PpuSetMode7PerspectiveCorrection(Ppu *ppu, int low, int high) {
  ppu->mode7PerspectiveLow = low ? 1.0f / low : 0.0f;
  ppu->mode7PerspectiveHigh = 1.0f / high;
}

/*
 * PpuSetExtraSideSpace — Set per-frame widescreen extension widths.
 *
 * `left` and `right` are clamped to extraLeftRight (the compile-time
 * maximum, 96 when widescreen is enabled). `bottom` is clamped to 16
 * (the maximum extra rows below the standard 224). The renderer
 * consults extraLeftCur/extraRightCur/extraBottomCur to expand the
 * rendered region.
 */
void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom) {
  ppu->extraLeftCur = UintMin(left, ppu->extraLeftRight);
  ppu->extraRightCur = UintMin(right, ppu->extraLeftRight);
  ppu->extraBottomCur = UintMin(bottom, 16);
  int target_extra = ppu->extraLeftRight >> 1;
  int wanted_total = target_extra * 2;
  ppu->viewportLeftCur = (ppu->extraLeftCur + ppu->extraRightCur >= wanted_total) ?
      ppu->extraLeftRight - ppu->extraLeftCur : ppu->extraLeftRight - target_extra;
}

void PpuSetRenderWideHud(Ppu *ppu, bool enabled, bool anchor_bg3, const uint16_t *tilemap, uint8_t shadow_size) {
  ppu->renderWideHud = enabled;
  ppu->anchorWideHudBg3 = anchor_bg3;
  ppu->wideHudTilemap = tilemap;
  ppu->wideHudShadowSize = shadow_size;
}

static int PpuGetWideHudPixel(Ppu *ppu, uint bg_x, uint bg_y) {
  if (bg_y >= 240 || bg_x >= 512)
    return 0;
  BgLayer *bglayer = &ppu->bgLayer[2];
  uint32 tile = ppu->wideHudTilemap[UintMin(bg_y >> 3, 29) * 64 + ((bg_x >> 3) & 63)];
  if ((tile & 0x3ff) == 0x7f)
    return 0;
  int tileadr0 = bglayer->tileAdr + (bg_y & 7);
  int tileadr1 = bglayer->tileAdr + 7 - (bg_y & 7);
  int tileadr = (tile & 0x8000) ? tileadr1 : tileadr0;
  uint32 bits = ppu->vram[(tileadr + (tile & 0x3ff) * 8) & 0x7fff];
  int bit = bg_x & 7;
  return (tile & 0x4000) ?
      ((bits >> bit) & 1) | ((bits >> (7 + bit)) & 2) :
      ((bits >> (7 - bit)) & 1) | ((bits >> (14 - bit)) & 2);
}

static bool PpuHasWideHudShadowSource(Ppu *ppu, uint bg_x, uint bg_y) {
  uint shadow_size = ppu->wideHudShadowSize;
  for (uint d = 1; d <= shadow_size; d++) {
    if (bg_x >= d && PpuGetWideHudPixel(ppu, bg_x - d, bg_y))
      return true;
    if (bg_y >= d && PpuGetWideHudPixel(ppu, bg_x, bg_y - d))
      return true;
    if (bg_x >= d && bg_y >= d && PpuGetWideHudPixel(ppu, bg_x - d, bg_y - d))
      return true;
  }
  return false;
}

static void PpuDrawWideHudOverlay(Ppu *ppu, uint y, uint32 *dst_org) {
  if (!ppu->renderWideHud || ppu->wideHudTilemap == NULL || ppu->mode != 1 || y == 0 ||
      y > 224 + ppu->extraBottomCur ||
      !IS_SCREEN_ENABLED(ppu, false, 2))
    return;
  if (IS_SCREEN_WINDOWED(ppu, false, 2))
    return;

  int target_extra = ppu->extraLeftRight >> 1;
  if (target_extra == 0)
    return;

  BgLayer *bglayer = &ppu->bgLayer[2];
  uint bg_y = y - 1;
  int tileadr0 = bglayer->tileAdr + (bg_y & 7);
  int tileadr1 = bglayer->tileAdr + 7 - (bg_y & 7);
  int width = 256 + target_extra * 2;
  int output_x = ppu->viewportLeftCur;
  uint32 *dst = dst_org + output_x;

  for (int out_x = 0; out_x < width; out_x++) {
    uint bg_x = out_x;
    uint32 tile = ppu->wideHudTilemap[UintMin(bg_y >> 3, 29) * 64 + ((bg_x >> 3) & 63)];
    if ((tile & 0x3ff) == 0x7f)
      continue;
    int tileadr = (tile & 0x8000) ? tileadr1 : tileadr0;
    uint32 bits = ppu->vram[(tileadr + (tile & 0x3ff) * 8) & 0x7fff];
    int bit = bg_x & 7;
    int pixel = (tile & 0x4000) ?
        ((bits >> bit) & 1) | ((bits >> (7 + bit)) & 2) :
        ((bits >> (7 - bit)) & 1) | ((bits >> (14 - bit)) & 2);

    if (pixel) {
      pixel += (tile & 0x1c00) >> 8;
      uint32 color = ppu->cgram[pixel];
      dst[out_x] = ppu->brightnessMult[color & 0x1f] << 16 |
                   ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |
                   ppu->brightnessMult[(color >> 10) & 0x1f];
    } else if (ppu->wideHudShadowSize && PpuHasWideHudShadowSource(ppu, bg_x, bg_y)) {
      dst[out_x] = 0;
    }
  }
}

/*
 * FloatInterpolate — Standard linear interpolation:
 *   ymin + (ymax-ymin) * (x-xmin)/(xmax-xmin)
 * Used by the upsampler to walk the perspective ramp.
 */
static FORCEINLINE float FloatInterpolate(float x, float xmin, float xmax, float ymin, float ymax) {
  return ymin + (ymax - ymin) * (x - xmin) * (1.0f / (xmax - xmin));
}

/*
 * PpuDrawMode7Upsampled — 4×4 upsampled Mode 7 renderer.
 *
 * Bypasses the BG/sub buffers and the color-math compositor entirely;
 * writes 16 output pixels (4 columns × 4 rows of 32-bit RGBA) per
 * source Mode-7 pixel directly into renderBuffer. Used only when:
 *   ppu->mode == 7
 *   AND renderFlags has kPpuRenderFlags_4x4Mode7
 *   AND renderFlags has kPpuRenderFlags_NewRenderer
 *
 * What's different from the native Mode 7 path:
 *   - Each output scanline covers 4 source rows × 4 sub-pixels each.
 *     The outer `j` loop iterates the 4 sub-rows.
 *   - The m7a coefficient is replaced by `m0v[j]` which (when
 *     perspective is enabled) varies per sub-row to fake a slope.
 *   - Sprites are *also* upsampled but as solid 4×4 blocks (no
 *     anti-aliasing) — see the post-loop block that stretches each
 *     objBuffer pixel into a 4×4 tile.
 *   - halfColor is applied by ANDing with 0xfefefe and shifting right
 *     (per-channel halve in one instruction). This is the only color
 *     math operation the upsampler supports — additive/subtractive
 *     math falls back to the native Mode 7 path.
 *
 * The kInterpolateOffsets table samples 4 sub-row positions within the
 * 1-line slot (-1 + 0/0.25/0.5/0.75) so the perspective ramp is
 * sampled at 4× the vertical resolution.
 */
// Upsampled version of mode7 rendering. Draws everything in 4x the normal resolution.
// Draws directly to the pixel buffer and bypasses any math, and supports only
// a subset of the normal features (all that zelda needs)
static void PpuDrawMode7Upsampled(Ppu *ppu, uint y) {
  // expand 13-bit values to signed values
  uint32 xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3, yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
  uint32 clippedH = (((int16_t)(ppu->m7matrix[6] << 3)) >> 3) - xCenter;
  uint32 clippedV = (((int16_t)(ppu->m7matrix[7] << 3)) >> 3) - yCenter;
  int32 m0v[4];
  if (*(uint32*)&ppu->mode7PerspectiveLow == 0) {
    m0v[0] = m0v[1] = m0v[2] = m0v[3] = ppu->m7matrix[0] << 12;
  } else {
    static const float kInterpolateOffsets[4] = { -1, -1 + 0.25f, -1 + 0.5f, -1 + 0.75f };
    for (int i = 0; i < 4; i++)
      m0v[i] = 4096.0f / FloatInterpolate((int)y + kInterpolateOffsets[i], 0, 223, ppu->mode7PerspectiveLow, ppu->mode7PerspectiveHigh);
  }
  size_t pitch = ppu->renderPitch;
  uint8 *render_buffer_ptr = &ppu->renderBuffer[(y - 1) * 4 * pitch];
  uint8 *dst_start = render_buffer_ptr + (ppu->extraLeftRight - ppu->extraLeftCur) * 16;
  size_t draw_width = 256 + ppu->extraLeftCur + ppu->extraRightCur;
  uint8 *dst_curline = dst_start;
  uint32 m1 = ppu->m7matrix[1] << 12;  // xpos increment per vert movement
  uint32 m2 = ppu->m7matrix[2] << 12;  // ypos increment per horiz movement
  for (int j = 0; j < 4; j++) {
    uint32 m0 = m0v[j], m3 = m0;
    uint32 xpos = m0 * clippedH + m1 * (clippedV + y) + (xCenter << 20), xcur;
    uint32 ypos = m2 * clippedH + m3 * (clippedV + y) + (yCenter << 20), ycur;

    uint32 tile, pixel;
    xpos -= (m0 + m1) >> 1;
    ypos -= (m2 + m3) >> 1;
    xcur = (xpos << 2) + j * m1;
    ycur = (ypos << 2) + j * m3;

    xcur -= ppu->extraLeftCur * 4 * m0;
    ycur -= ppu->extraLeftCur * 4 * m2;

    uint8 *dst = dst_curline;
    uint8 *dst_end = dst_curline + draw_width * 16;

#define DRAW_PIXEL(mode) \
    tile = ppu->vram[(ycur >> 25 & 0x7f) * 128 + (xcur >> 25 & 0x7f)] & 0xff;  \
    pixel = ppu->vram[tile * 64 + (ycur >> 22 & 7) * 8 + (xcur >> 22 & 7)] >> 8; \
    pixel = (xcur & 0x80000000) ? 0 : pixel; \
    *(uint32*)dst = (mode ? (ppu->colorMapRgb[pixel] & 0xfefefe) >> 1 : ppu->colorMapRgb[pixel]); \
    xcur += m0, ycur += m2, dst += 4;

    if (!ppu->halfColor) {
      do {
        DRAW_PIXEL(0);
        DRAW_PIXEL(0);
        DRAW_PIXEL(0);
        DRAW_PIXEL(0);
      } while (dst != dst_end);
    } else {
      do {
        DRAW_PIXEL(1);
        DRAW_PIXEL(1);
        DRAW_PIXEL(1);
        DRAW_PIXEL(1);
      } while (dst != dst_end);
    }
#undef DRAW_PIXEL

    dst_curline += pitch;
  }

  if (ppu->lineHasSprites) {
    uint8 *dst = dst_start;
    PpuZbufType *pixels = ppu->objBuffer.data + (kPpuExtraLeftRight - ppu->extraLeftCur);
    for (size_t i = 0; i < draw_width; i++, dst += 16) {
      uint32 pixel = pixels[i] & 0xff;
      if (pixel) {
        uint32 color = ppu->colorMapRgb[pixel];
        ((uint32 *)dst)[3] = ((uint32 *)dst)[2] = ((uint32 *)dst)[1] = ((uint32 *)dst)[0] = color;
        ((uint32 *)(dst + pitch * 1))[3] = ((uint32 *)(dst + pitch * 1))[2] = ((uint32 *)(dst + pitch * 1))[1] = ((uint32 *)(dst + pitch * 1))[0] = color;
        ((uint32 *)(dst + pitch * 2))[3] = ((uint32 *)(dst + pitch * 2))[2] = ((uint32 *)(dst + pitch * 2))[1] = ((uint32 *)(dst + pitch * 2))[0] = color;
        ((uint32 *)(dst + pitch * 3))[3] = ((uint32 *)(dst + pitch * 3))[2] = ((uint32 *)(dst + pitch * 3))[1] = ((uint32 *)(dst + pitch * 3))[0] = color;
      }
    }
  }

  if (ppu->extraLeftRight - ppu->extraLeftCur != 0) {
    size_t n = 4 * sizeof(uint32) * (ppu->extraLeftRight - ppu->extraLeftCur);
    for(int i = 0; i < 4; i++)
      memset(render_buffer_ptr + pitch * i, 0, n);
  }
  if (ppu->extraLeftRight - ppu->extraRightCur != 0) {
    size_t n = 4 * sizeof(uint32) * (ppu->extraLeftRight - ppu->extraRightCur);
    for (int i = 0; i < 4; i++)
      memset(render_buffer_ptr + pitch * i + (256 + ppu->extraLeftRight * 2 - (ppu->extraLeftRight - ppu->extraRightCur)) * 4 * sizeof(uint32), 0, n);
  }
#undef DRAW_PIXEL
}

/*
 * PpuDrawBackgrounds — Render every active layer for one main- or
 * sub-screen pass into bgBuffers[sub], in priority order.
 *
 * Only mode 1 and mode 7 are implemented (Zelda 3's only two BG modes).
 * The priority bucket constants (0xc000, 0xb100, 0xf200, etc.) encode
 * "high nibble of high byte = bucket, low byte = palette base or 0"
 * so that the per-pixel zhi/zlo compare against the running z-buffer
 * value naturally resolves the SNES priority order in a single uint16
 * compare. The author-provided priority table immediately below this
 * comment is the canonical map from bucket value to layer; do not edit
 * those without also updating the BG draw calls.
 */
static void PpuDrawBackgrounds(Ppu *ppu, int y, bool sub) {
// Top 4 bits contain the prio level, and bottom 4 bits the layer type.
// SPRITE_PRIO_TO_PRIO can be used to convert from obj prio to this prio.
//  15: BG3 tiles with priority 1 if bit 3 of $2105 is set
//  14: Sprites with priority 3 (4 * sprite_prio + 2)
//  12: BG1 tiles with priority 1
//  11: BG2 tiles with priority 1
//  10: Sprites with priority 2 (4 * sprite_prio + 2)
//  8: BG1 tiles with priority 0
//  7: BG2 tiles with priority 0
//  6: Sprites with priority 1 (4 * sprite_prio + 2)
//  3: BG3 tiles with priority 1 if bit 3 of $2105 is clear
//  2: Sprites with priority 0 (4 * sprite_prio + 2)
//  1: BG3 tiles with priority 0
//  0: backdrop

  if (ppu->mode == 1) {
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, true);

    if (IS_MOSAIC_ENABLED(ppu, 0))
      PpuDrawBackground_4bpp_mosaic(ppu, y, sub, 0, 0xc000, 0x8000);
    else
      PpuDrawBackground_4bpp(ppu, y, sub, 0, 0xc000, 0x8000);

    if (IS_MOSAIC_ENABLED(ppu, 1))
      PpuDrawBackground_4bpp_mosaic(ppu, y, sub, 1, 0xb100, 0x7100);
    else
      PpuDrawBackground_4bpp(ppu, y, sub, 1, 0xb100, 0x7100);

    if (IS_MOSAIC_ENABLED(ppu, 2))
      PpuDrawBackground_2bpp_mosaic(ppu, y, sub, 2, 0xf200, 0x1200);
    else
      PpuDrawBackground_2bpp(ppu, y, sub, 2, 0xf200, 0x1200);
  } else {
    // mode 7
    PpuDrawBackground_mode7(ppu, y, sub, 0xc000);
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, false);
  }
}

/*
 * PpuDrawWholeLine — Whole-scanline path for the modern renderer.
 *
 * Pipeline for one visible scanline:
 *   1. If forced-blank ($2100 bit 7), zero the output row and return.
 *   2. If Mode 7 + 4× upsampling is selected, hand off to the
 *      PpuDrawMode7Upsampled fast path (it writes RGBA directly).
 *   3. Clear bgBuffers[0] (main-screen z-buffer) to the backdrop value.
 *   4. PpuDrawBackgrounds(main): fill bgBuffers[0] with BG1/BG2/BG3 +
 *      sprites in their priority buckets.
 *   5. If color math is "add subscreen" and any layer participates,
 *      clear bgBuffers[1] and draw the sub-screen the same way.
 *   6. Build the color-window decisions for this line into a per-
 *      region bitmask. There are up to 5 regions; for each, two bits
 *      say "clip main RGB to black?" and "perform math?".
 *   7. Walk the regions left-to-right, doing one of two inner loops:
 *      - Fast path: no math active in this region, so the output is
 *        just brightness-mapped CGRAM lookup of the main bucket.
 *      - Slow path: per-pixel math. Check the main pixel's layer
 *        against mathEnabled; if math is on, fetch the sub-pixel (or
 *        fixed color), add or subtract, optionally halve, clip and
 *        emit the RGB.
 *   8. Zero out any widescreen padding the main draw didn't cover.
 *
 * cw_clip_math encoding:
 *   bit 0..(nr-1): "clip RGB to zero in this region" (color clip)
 *   bit 8..(8+nr-1): "color math is active in this region" (math clip)
 *   The do/while at the end of the loop does `cw_clip_math >>= 1` so
 *   bit 0 always represents the current region's clip flag, and bit 8
 *   always represents the current region's math flag.
 *
 * cgram word format: 0bbbbbgg gggrrrrr (15-bit BGR). The fast and slow
 * paths both treat r/g/b independently and feed each through
 * brightnessMult/brightnessMultHalf to land in 0..255.
 */
static NOINLINE void PpuDrawWholeLine(Ppu *ppu, uint y) {
  if (ppu->forcedBlank) {
    uint8 *dst = &ppu->renderBuffer[(y - 1) * ppu->renderPitch];
    size_t n = sizeof(uint32) * (256 + ppu->extraLeftRight * 2);
    memset(dst, 0, n);
    return;
  }

  if (ppu->mode == 7 && (ppu->renderFlags & kPpuRenderFlags_4x4Mode7)) {
    PpuDrawMode7Upsampled(ppu, y);
    return;
  }

  // Default background is backdrop
  ClearBackdrop(&ppu->bgBuffers[0]);

  // Render main screen
  PpuDrawBackgrounds(ppu, y, false);

  // The 6:th bit is automatically zero, math is never applied to the first half of the sprites.
  uint32 math_enabled = ppu->mathEnabled;

  // Render also the subscreen?
  bool rendered_subscreen = false;
  if (ppu->preventMathMode != 3 && ppu->addSubscreen && math_enabled) {
    ClearBackdrop(&ppu->bgBuffers[1]);
    if (ppu->screenEnabled[1] != 0) {
      PpuDrawBackgrounds(ppu, y, true);
      rendered_subscreen = true;
    }
  }

  // Color window affects the drawing mode in each region
  PpuWindows cwin;
  PpuWindows_Calc(&cwin, ppu, 5);
  static const uint8 kCwBitsMod[8] = {
    0x00, 0xff, 0xff, 0x00,
    0xff, 0x00, 0xff, 0x00,
  };
  uint32 cw_clip_math = ((cwin.bits & kCwBitsMod[ppu->clipMode]) ^ kCwBitsMod[ppu->clipMode + 4]) |
                        ((cwin.bits & kCwBitsMod[ppu->preventMathMode]) ^ kCwBitsMod[ppu->preventMathMode + 4]) << 8;

  uint32 *dst = (uint32*)&ppu->renderBuffer[(y - 1) * ppu->renderPitch], *dst_org = dst;
  
  dst += (ppu->extraLeftRight - ppu->extraLeftCur);

  uint32 windex = 0;
  do {
    uint32 left = cwin.edges[windex] + kPpuExtraLeftRight, right = cwin.edges[windex + 1] + kPpuExtraLeftRight;
    // If clip is set, then zero out the rgb values from the main screen.
    uint32 clip_color_mask = (cw_clip_math & 1) ? 0x1f : 0;
    uint32 math_enabled_cur = (cw_clip_math & 0x100) ? math_enabled : 0;
    uint32 fixed_color = ppu->fixedColorR | ppu->fixedColorG << 5 | ppu->fixedColorB << 10;
    if (math_enabled_cur == 0 || fixed_color == 0 && !ppu->halfColor && !rendered_subscreen) {
      // Math is disabled (or has no effect), so can avoid the per-pixel maths check
      uint32 i = left;
      do {
        uint32 color = ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff];
        dst[0] = ppu->brightnessMult[color & clip_color_mask] << 16 |
                 ppu->brightnessMult[(color >> 5) & clip_color_mask] << 8 |
                 ppu->brightnessMult[(color >> 10) & clip_color_mask];
      } while (dst++, ++i < right);
    } else {
      uint8 *half_color_map = ppu->halfColor ? ppu->brightnessMultHalf : ppu->brightnessMult;
      // Store this in locals
      math_enabled_cur |= ppu->addSubscreen << 8 | ppu->subtractColor << 9;
      // Need to check for each pixel whether to use math or not based on the main screen layer.
      uint32 i = left;
      do {
        uint32 color = ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff], color2;
        uint8 main_layer = (ppu->bgBuffers[0].data[i] >> 8) & 0xf;
        uint32 r = color & clip_color_mask;
        uint32 g = (color >> 5) & clip_color_mask;
        uint32 b = (color >> 10) & clip_color_mask;
        uint8 *color_map = ppu->brightnessMult;
        if (math_enabled_cur & (1 << main_layer)) {
          if (math_enabled_cur & 0x100) {  // addSubscreen ?
            if ((ppu->bgBuffers[1].data[i] & 0xff) != 0)
              color2 = ppu->cgram[ppu->bgBuffers[1].data[i] & 0xff], color_map = half_color_map;
            else  // Don't halve if ppu->addSubscreen && backdrop
              color2 = fixed_color;
          } else {
            color2 = fixed_color, color_map = half_color_map;
          }
          uint32 r2 = (color2 & 0x1f), g2 = ((color2 >> 5) & 0x1f), b2 = ((color2 >> 10) & 0x1f);
          if (math_enabled_cur & 0x200) {  // subtractColor?
            r = (r >= r2) ? r - r2 : 0;
            g = (g >= g2) ? g - g2 : 0;
            b = (b >= b2) ? b - b2 : 0;
          } else {
            r += r2;
            g += g2;
            b += b2;
          }
        }
        dst[0] = color_map[b] | color_map[g] << 8 | color_map[r] << 16;
      } while (dst++, ++i < right);
    }
  } while (cw_clip_math >>= 1, ++windex < cwin.nr);

  // Clear out stuff on the sides.
  if (ppu->extraLeftRight - ppu->extraLeftCur != 0)
    memset(dst_org, 0, sizeof(uint32) * (ppu->extraLeftRight - ppu->extraLeftCur));
  if (ppu->extraLeftRight - ppu->extraRightCur != 0)
    memset(dst_org + (256 + ppu->extraLeftRight * 2 - (ppu->extraLeftRight - ppu->extraRightCur)), 0,
        sizeof(uint32) * (ppu->extraLeftRight - ppu->extraRightCur));
  PpuDrawWideHudOverlay(ppu, y, dst_org);
}

/*
 * ppu_handlePixel — Legacy single-pixel compositor.
 *
 * Called for every (x,y) when kPpuRenderFlags_NewRenderer is *not* set.
 * Conceptually simpler than the whole-line path because each pixel is
 * handled independently:
 *
 *   1. Look up the main-screen pixel via ppu_getPixel. That walks the
 *      layer priority table for the current BG mode and returns the
 *      winning layer (0..6) plus its r/g/b in 5-bit channels.
 *   2. Apply the clip-window (clipMode + colorWindowState) — if active
 *      and the pixel is on the masked side, zero the rgb (clip to
 *      black before math).
 *   3. Decide if color math is active for this pixel (mathEnabled
 *      gates per main-screen-layer, preventMathMode gates per window
 *      region).
 *   4. If math is on AND addSubscreen is on, fetch the sub-screen
 *      pixel. Otherwise math uses fixedColorR/G/B.
 *   5. Apply add/subtract/half and clamp.
 *   6. Scale by brightness and write the final RGB into renderBuffer.
 *      Note: this path uses the unscaled SNES 5-bit channels through
 *      step 5, then expands to 8-bit and multiplies in step 6 — that's
 *      different from the whole-line path which uses brightnessMult
 *      LUTs and is therefore much faster.
 *
 * Modes 5 and 6 are "hi-res" modes (512px output); when active, the
 * sub-screen pixel is always fetched and written as r2/g2/b2 (the
 * higher-resolution output is interleaved in hardware). Not used by
 * Zelda 3 but supported here for completeness.
 *
 * Author TODOs preserved verbatim below — they note two pieces of
 * hardware accuracy that aren't yet implemented for the sub screen.
 */
// Per-pixel compositor (legacy rendering path). Resolves the final RGB color
// for pixel (x,y) by getting the main screen and sub screen pixels, then
// applying color math (add/subtract/half) and brightness scaling.
static void ppu_handlePixel(Ppu* ppu, int x, int y) {
  int r = 0, r2 = 0;
  int g = 0, g2 = 0;
  int b = 0, b2 = 0;
  if (!ppu->forcedBlank) {
    int mainLayer = ppu_getPixel(ppu, x, y, false, &r, &g, &b);

    bool colorWindowState = ppu_getWindowState(ppu, 5, x);
    if (
      ppu->clipMode == 3 ||
      (ppu->clipMode == 2 && colorWindowState) ||
      (ppu->clipMode == 1 && !colorWindowState)
      ) {
      r = g = b = 0;
    }
    int secondLayer = 5; // backdrop
    bool mathEnabled = mainLayer < 6 && (ppu->mathEnabled & (1 << mainLayer)) && !(
      ppu->preventMathMode == 3 ||
      (ppu->preventMathMode == 2 && colorWindowState) ||
      (ppu->preventMathMode == 1 && !colorWindowState)
      );
    if ((mathEnabled && ppu->addSubscreen) || ppu->mode == 5 || ppu->mode == 6) {
      secondLayer = ppu_getPixel(ppu, x, y, true, &r2, &g2, &b2);
    }
    // TODO: subscreen pixels can be clipped to black as well
    // TODO: math for subscreen pixels (add/sub sub to main)
    if (mathEnabled) {
      if (ppu->subtractColor) {
        r -= (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g -= (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b -= (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      } else {
        r += (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g += (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b += (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      }
      if (ppu->halfColor && (secondLayer != 5 || !ppu->addSubscreen)) {
        r >>= 1;
        g >>= 1;
        b >>= 1;
      }
      if (r > 31) r = 31;
      if (g > 31) g = 31;
      if (b > 31) b = 31;
      if (r < 0) r = 0;
      if (g < 0) g = 0;
      if (b < 0) b = 0;
    }
    if (!(ppu->mode == 5 || ppu->mode == 6)) {
      r2 = r; g2 = g; b2 = b;
    }
  }
  int row = y - 1;
  uint8 *pixelBuffer = (uint8*) &ppu->renderBuffer[row * ppu->renderPitch + (x + ppu->extraLeftRight) * 4];
  pixelBuffer[0] = ((b << 3) | (b >> 2)) * ppu->brightness / 15;
  pixelBuffer[1] = ((g << 3) | (g >> 2)) * ppu->brightness / 15;
  pixelBuffer[2] = ((r << 3) | (r >> 2)) * ppu->brightness / 15;
  pixelBuffer[3] = 0;
}

/*
 * bitDepthsPerMode — BG layer bit depths indexed by [actMode][bgLayer].
 *
 * Row index meaning (matches the layersPerMode/prioritysPerMode tables
 * in ppu_getPixel):
 *   0..7 : raw SNES BG mode 0..7
 *   8    : mode 1 with BG3 priority bit set (mode "1+L3prio")
 *   9    : mode 7 with EXTBG set (mode "7+EXTBG")
 *
 * Column index = BG layer 0..3 (BG1..BG4).
 *
 * Cell values:
 *   2, 4, 8 -> tile bit depth in bits-per-pixel (4 colors, 16 colors,
 *              256 colors)
 *   5       -> layer is "OBJ" or unused at this slot, signalling the
 *              caller to treat this as a sprite lookup, not a BG fetch.
 *   7       -> Mode 7 special: 8bpp tile data with the high bit acting
 *              as an extra priority flag (EXTBG behaviour).
 */
static const int bitDepthsPerMode[10][4] = {
  {2, 2, 2, 2},
  {4, 4, 2, 5},
  {4, 4, 5, 5},
  {8, 4, 5, 5},
  {8, 2, 5, 5},
  {4, 2, 5, 5},
  {4, 5, 5, 5},
  {8, 5, 5, 5},
  {4, 4, 2, 5},
  {8, 7, 5, 5}
};

/*
 * ppu_getPixel — Legacy-path single-pixel lookup.
 *
 * Returns the winning layer number for (x, y) and fills r/g/b (each in
 * 0..31) with that layer's pixel color from CGRAM. The "winning layer"
 * follows the SNES priority order encoded in layersPerMode +
 * prioritysPerMode for the current BG mode:
 *
 *   layersPerMode[actMode][i]  : layer ID (0..3 for BG, 4 for sprites,
 *                                5 = sentinel/no-layer)
 *   prioritysPerMode[actMode][i]: priority bit to match for that slot.
 *                                For BG layers, this is the tilemap
 *                                entry's prio bit; for sprites, the
 *                                2-bit sprite priority.
 *   layerCountPerMode[actMode] : how many slots to walk.
 *
 * Layer enumeration stops at the first opaque (non-zero) pixel — that
 * one wins. If nothing matches, layer stays 5 (backdrop) and the
 * CGRAM[0] color is returned.
 *
 * Return value adjustment: a sprite with palette index < 0xc0 is
 * remapped from layer 4 to layer 6, which the caller uses to decide
 * whether color math applies (only level-6 sprites use math, matching
 * hardware).
 *
 * `actMode` is the row index into the priority tables. mode 1 with
 * BG3 priority enabled becomes row 8 (the "L3prio" alternate ordering);
 * mode 7 with EXTBG becomes row 9.
 */
// Determine the winning pixel at (x,y) for main or sub screen.
// Iterates through layers in priority order (defined by layersPerMode table)
// for the current BG mode. Returns the layer number that won and fills r/g/b.
static int ppu_getPixel(Ppu *ppu, int x, int y, bool sub, int *r, int *g, int *b) {
  // array for layer definitions per mode:
//   0-7: mode 0-7; 8: mode 1 + l3prio; 9: mode 7 + extbg

//   0-3; layers 1-4; 4: sprites; 5: nonexistent
  static const int layersPerMode[10][12] = {
    {4, 0, 1, 4, 0, 1, 4, 2, 3, 4, 2, 3},
    {4, 0, 1, 4, 0, 1, 4, 2, 4, 2, 5, 5},
    {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
    {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
    {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
    {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
    {4, 0, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5},
    {4, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5, 5},
    {2, 4, 0, 1, 4, 0, 1, 4, 4, 2, 5, 5},
    {4, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5, 5}
  };

  static const int prioritysPerMode[10][12] = {
    {3, 1, 1, 2, 0, 0, 1, 1, 1, 0, 0, 0},
    {3, 1, 1, 2, 0, 0, 1, 1, 0, 0, 5, 5},
    {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
    {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
    {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
    {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
    {3, 1, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5},
    {3, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5, 5},
    {1, 3, 1, 1, 2, 0, 0, 1, 0, 0, 5, 5},
    {3, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5, 5}
  };

  static const int layerCountPerMode[10] = {
    12, 10, 8, 8, 8, 8, 6, 5, 10, 7
  };

  
  // figure out which color is on this location on main- or subscreen, sets it in r, g, b
  // returns which layer it is: 0-3 for bg layer, 4 or 6 for sprites (depending on palette), 5 for backdrop
  int actMode = ppu->mode == 1 ? 8 : ppu->mode;
  actMode = ppu->mode == 7 && ppu->m7extBg_always_zero ? 9 : actMode;
  int layer = 5;
  int pixel = 0;
  for (int i = 0; i < layerCountPerMode[actMode]; i++) {
    int curLayer = layersPerMode[actMode][i];
    int curPriority = prioritysPerMode[actMode][i];
    bool layerActive = false;
    if (!sub) {
      layerActive = IS_SCREEN_ENABLED(ppu, 0, curLayer) && (
        !IS_SCREEN_WINDOWED(ppu, 0, curLayer) || !ppu_getWindowState(ppu, curLayer, x)
        );
    } else {
      layerActive = IS_SCREEN_ENABLED(ppu, 1, curLayer) && (
        !IS_SCREEN_WINDOWED(ppu, 1, curLayer) || !ppu_getWindowState(ppu, curLayer, x)
        );
    }
    if (layerActive) {
      if (curLayer < 4) {
        // bg layer
        int lx = x;
        int ly = y;
        if (IS_MOSAIC_ENABLED(ppu, curLayer)) {
          lx -= lx % ppu->mosaicSize;
          ly -= (ly - 1) % ppu->mosaicSize;
        }
        if (ppu->mode == 7) {
          pixel = ppu_getPixelForMode7(ppu, lx, curLayer, curPriority);
        } else {
          lx += ppu->bgLayer[curLayer].hScroll;
          ly += ppu->bgLayer[curLayer].vScroll;
          pixel = ppu_getPixelForBgLayer(
            ppu, lx & 0x3ff, ly & 0x3ff,
            curLayer, curPriority
          );
        }
      } else {
        // get a pixel from the sprite buffer
        pixel = 0;
        if ((ppu->objBuffer.data[x + kPpuExtraLeftRight] >> 12) == SPRITE_PRIO_TO_PRIO_HI(curPriority))
          pixel = ppu->objBuffer.data[x + kPpuExtraLeftRight] & 0xff;
      }
    }
    if (pixel > 0) {
      layer = curLayer;
      break;
    }
  }
  uint16_t color = ppu->cgram[pixel & 0xff];
  *r = color & 0x1f;
  *g = (color >> 5) & 0x1f;
  *b = (color >> 10) & 0x1f;
  if (layer == 4 && pixel < 0xc0) layer = 6; // sprites with palette color < 0xc0
  return layer;

}


/*
 * ppu_getPixelForBgLayer — Legacy-path single-BG-pixel lookup.
 *
 * Walks the same data structures as PpuDrawBackground_*bpp* but for one
 * pixel only. Steps:
 *   1. Compute the tilemap-entry VRAM address from (x, y) and the
 *      layer's tilemapAdr/Wider/Higher settings. Wide tiles (modes 5
 *      and 6) use a different bit-shift count so each pair of adjacent
 *      tilemap entries represents a 16×8 tile.
 *   2. Read the tilemap entry. Bail with 0 if its priority bit doesn't
 *      match the requested priority — that lets the caller make
 *      separate priority-0 and priority-1 passes.
 *   3. Decompose the tilemap entry into palette#, flip flags, and tile#.
 *      Figure out (row, col) within the 8×8 tile, accounting for h/v
 *      flip.
 *   4. Read up to 4 bitplane words from tile data, extracting bit
 *      `col` from each. Bits assembled little-endian give the 2/4/8-bit
 *      pixel value.
 *   5. Return paletteSize*paletteNum + pixel, with the special case
 *      that pixel == 0 always returns 0 (transparent).
 *
 * In mode 0 each BG layer uses its own 8 sub-palettes (BG1 -> 0..7,
 * BG2 -> 8..15, BG3 -> 16..23, BG4 -> 24..31), so paletteNum gets
 * shifted by 8*layer.
 */
// Fetch a background layer pixel at screen position (x,y). Applies scrolling,
// looks up the tilemap entry in VRAM, decodes the tile bitplanes, and returns
// the palette color index (0 = transparent). Respects priority bit filtering.
static int ppu_getPixelForBgLayer(Ppu *ppu, int x, int y, int layer, bool priority) {
  BgLayer *layerp = &ppu->bgLayer[layer];
  // figure out address of tilemap word and read it
  bool wideTiles = ppu->mode == 5 || ppu->mode == 6;
  int tileBitsX = wideTiles ? 4 : 3;
  int tileHighBitX = wideTiles ? 0x200 : 0x100;
  int tileBitsY = 3;
  int tileHighBitY = 0x100;
  uint16_t tilemapAdr = layerp->tilemapAdr + (((y >> tileBitsY) & 0x1f) << 5 | ((x >> tileBitsX) & 0x1f));
  if ((x & tileHighBitX) && layerp->tilemapWider) tilemapAdr += 0x400;
  if ((y & tileHighBitY) && layerp->tilemapHigher) tilemapAdr += layerp->tilemapWider ? 0x800 : 0x400;
  uint16_t tile = ppu->vram[tilemapAdr & 0x7fff];
  // check priority, get palette
  if (((bool)(tile & 0x2000)) != priority) return 0; // wrong priority
  int paletteNum = (tile & 0x1c00) >> 10;
  // figure out position within tile
  int row = (tile & 0x8000) ? 7 - (y & 0x7) : (y & 0x7);
  int col = (tile & 0x4000) ? (x & 0x7) : 7 - (x & 0x7);
  int tileNum = tile & 0x3ff;
  if (wideTiles) {
    // if unflipped right half of tile, or flipped left half of tile
    if (((bool)(x & 8)) ^ ((bool)(tile & 0x4000))) tileNum += 1;
  }
  // read tiledata, ajust palette for mode 0
  int bitDepth = bitDepthsPerMode[ppu->mode][layer];
  if (ppu->mode == 0) paletteNum += 8 * layer;
  // plane 1 (always)
  int paletteSize = 4;
  uint16_t plane1 = ppu->vram[(layerp->tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + row) & 0x7fff];
  int pixel = (plane1 >> col) & 1;
  pixel |= ((plane1 >> (8 + col)) & 1) << 1;
  // plane 2 (for 4bpp, 8bpp)
  if (bitDepth > 2) {
    paletteSize = 16;
    uint16_t plane2 = ppu->vram[(layerp->tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 8 + row) & 0x7fff];
    pixel |= ((plane2 >> col) & 1) << 2;
    pixel |= ((plane2 >> (8 + col)) & 1) << 3;
  }
  // plane 3 & 4 (for 8bpp)
  if (bitDepth > 4) {
    paletteSize = 256;
    uint16_t plane3 = ppu->vram[(layerp->tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 16 + row) & 0x7fff];
    pixel |= ((plane3 >> col) & 1) << 4;
    pixel |= ((plane3 >> (8 + col)) & 1) << 5;
    uint16_t plane4 = ppu->vram[(layerp->tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 24 + row) & 0x7fff];
    pixel |= ((plane4 >> col) & 1) << 6;
    pixel |= ((plane4 >> (8 + col)) & 1) << 7;
  }
  // return cgram index, or 0 if transparent, palette number in bits 10-8 for 8-color layers
  return pixel == 0 ? 0 : paletteSize * paletteNum + pixel;
}

/*
 * ppu_calculateMode7Starts — Pre-compute the affine (X, Y) for column 0
 * of scanline `y`. Called once per scanline by the legacy renderer
 * (the whole-line Mode 7 path inlines the equivalent math).
 *
 * The full Mode 7 affine equation is:
 *      X = m7a*(x - hScroll - xCenter) + m7b*(y - vScroll - yCenter)
 *          + xCenter
 *      Y = m7c*(x - hScroll - xCenter) + m7d*(y - vScroll - yCenter)
 *          + yCenter
 *
 * Splitting out the y-dependent constants lets the per-column inner
 * loop just do `m7startX + m7a * x` and `m7startY + m7c * x`. The
 * `& ~63` masks below truncate to the 6 low fractional bits of the
 * fixed-point multiply, mimicking real hardware's 16.16-with-low-6-
 * dropped behaviour.
 *
 * clippedH/clippedV implement the 10-bit signed wrap of (hScroll -
 * xCenter): the sign bit (bit 13) is checked and the value is then
 * either sign-extended into a negative int or masked to 10 bits.
 *
 * If mosaic is active for BG1, snap y down to the top of its mosaic
 * block so all rows in the block sample the same source row.
 */
// Precompute the affine transformation start coordinates for Mode 7 rendering.
// Uses the 4-element rotation/scale matrix (a,b,c,d) and center point (x,y)
// to calculate where the first pixel of scanline `y` maps in the tilemap.
static void ppu_calculateMode7Starts(Ppu* ppu, int y) {
  // expand 13-bit values to signed values
  int hScroll = ((int16_t) (ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t) (ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t) (ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t) (ppu->m7matrix[5] << 3)) >> 3;
  // do calculation
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  if(IS_MOSAIC_ENABLED(ppu, 0)) {
    y -= (y - 1) % ppu->mosaicSize;
  }
  uint8_t ry = ppu->m7yFlip ? 255 - y : y;
  ppu->m7startX = (
    ((ppu->m7matrix[0] * clippedH) & ~63) +
    ((ppu->m7matrix[1] * ry) & ~63) +
    ((ppu->m7matrix[1] * clippedV) & ~63) +
    (xCenter << 8)
  );
  ppu->m7startY = (
    ((ppu->m7matrix[2] * clippedH) & ~63) +
    ((ppu->m7matrix[3] * ry) & ~63) +
    ((ppu->m7matrix[3] * clippedV) & ~63) +
    (yCenter << 8)
  );
}

/*
 * ppu_getPixelForMode7 — Legacy-path single-pixel Mode 7 lookup.
 *
 * Walks one output column at position `x` along the pre-computed
 * scanline start (m7startX/Y + m7a/c * x), then:
 *   - Snaps to the start of the mosaic block if mosaic is on.
 *   - Mirrors x if m7xFlip is set.
 *   - Tests for out-of-range and selects the tile or returns 0 based
 *     on m7largeField/m7charFill (see PpuDrawBackground_mode7 doc for
 *     the full behaviour matrix).
 *
 * EXTBG (layer == 1) case: the high bit of each Mode 7 pixel is reused
 * as an extra priority flag, splitting the 256-color Mode 7 plane into
 * two priority halves. If the bit doesn't match the requested priority,
 * return 0 (transparent at this priority).
 */
// Fetch a Mode 7 pixel. The affine-transformed (x,y) maps into a 128×128 tile
// grid. Each tile is 8×8 pixels. Out-of-range coordinates are handled by
// m7largeField (wrap/clip) and m7charFill (use tile 0 or transparent).
static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority) {
  if (IS_MOSAIC_ENABLED(ppu, layer))
    x -= x % ppu->mosaicSize;
  uint8_t rx = ppu->m7xFlip ? 255 - x : x;
  int xPos = (ppu->m7startX + ppu->m7matrix[0] * rx) >> 8;
  int yPos = (ppu->m7startY + ppu->m7matrix[2] * rx) >> 8;
  bool outsideMap = xPos < 0 || xPos >= 1024 || yPos < 0 || yPos >= 1024;
  xPos &= 0x3ff;
  yPos &= 0x3ff;
  if(!ppu->m7largeField) outsideMap = false;
  uint8_t tile = outsideMap ? 0 : ppu->vram[(yPos >> 3) * 128 + (xPos >> 3)] & 0xff;
  uint8_t pixel = outsideMap && !ppu->m7charFill ? 0 : ppu->vram[tile * 64 + (yPos & 7) * 8 + (xPos & 7)] >> 8;
  if(layer == 1) {
    if(((bool) (pixel & 0x80)) != priority) return 0;
    return pixel & 0x7f;
  }
  return pixel;
}

/*
 * ppu_getWindowState — Legacy-path window inclusion test.
 *
 * Returns true if pixel `x` is inside the effective window region for
 * `layer` (one of 0..4 for BG1..BG4/OBJ, or 5 for the color-math
 * window). The whole-line path uses PpuWindows_Calc instead, but the
 * legacy path checks per pixel because it doesn't pre-compute regions.
 *
 * Four cases:
 *   neither window enabled  -> false (no clipping)
 *   only window 1 enabled   -> "x in [w1.left, w1.right]", optionally
 *                              inverted
 *   only window 2 enabled   -> same, with window 2
 *   both enabled            -> OR (the SNES also supports AND/XOR/XNOR
 *                              via WBGLOG/WOBJLOG, but Zelda's writes to
 *                              those are asserted to be 0, so OR is the
 *                              only path that runs in practice)
 */
// Test whether pixel position `x` is inside the effective window region for
// the given layer. Combines window 1 and window 2 results using the per-layer
// logic operation (OR, AND, XOR, XNOR) configured in WBGLOG/WOBJLOG.
static bool ppu_getWindowState(Ppu* ppu, int layer, int x) {
  uint32 winflags = GET_WINDOW_FLAGS(ppu, layer);
  if (!(winflags & kWindow1Enabled) && !(winflags & kWindow2Enabled)) {
    return false;
  }
  if ((winflags & kWindow1Enabled) && !(winflags & kWindow2Enabled)) {
    bool test = x >= ppu->window1left && x <= ppu->window1right;
    return (winflags & kWindow1Inversed) ? !test : test;
  }
  if (!(winflags & kWindow1Enabled) && (winflags & kWindow2Enabled)) {
    bool test = x >= ppu->window2left && x <= ppu->window2right;
    return (winflags & kWindow2Inversed) ? !test : test;
  }
  bool test1 = x >= ppu->window1left && x <= ppu->window1right;
  bool test2 = x >= ppu->window2left && x <= ppu->window2right;
  if (winflags & kWindow1Inversed) test1 = !test1;
  if (winflags & kWindow2Inversed) test2 = !test2;
  return test1 || test2;
}

/*
 * ppu_evaluateSprites — Per-scanline sprite (OBJ) rasteriser.
 *
 * Walks all 128 OAM entries, decides which are on this scanline,
 * decodes their tile data, and writes the resulting pixels into
 * objBuffer with each pixel's full priority byte already baked in
 * (so the BG compositor can merge with a plain numeric compare).
 *
 * OAM layout (in `ppu->oam`):
 *   ppu->oam[2*i + 0] -> low 16 bits of sprite i: y (hi byte), x-low (lo)
 *   ppu->oam[2*i + 1] -> high 16 bits of sprite i: vflip(15), hflip(14),
 *                        prio(13..12), palette(11..9), tile.bank(8),
 *                        tile.num(7..0)
 *   ppu->oam[0x100 + (i >> 4)] -> packed high table; each byte stores
 *                        4 sprites' (x-high, size) bits.
 *
 * Hardware limits:
 *   - 32 sprites visible per line. If the 33rd is hit, evaluation stops.
 *   - 34 8×1 tile slivers fetched per line. If the 35th is hit,
 *     subsequent sprite columns drop. kPpuRenderFlags_NoSpriteLimits
 *     raises both caps to 1024.
 *
 * Scanning starts at index 0 and wraps; the loop terminates when index
 * == index_end (both start at 0, so the loop walks all 128 entries
 * unless an early break fires).
 *
 * Pixel-locking rule: `(dst[0] & 0xff) == 0` means "don't overwrite an
 * already-drawn sprite pixel". This is the classic "first sprite wins"
 * SNES sprite priority — the earlier-in-OAM sprite stays on top of a
 * same-priority later sprite at the same pixel.
 *
 * Author TODOs preserved verbatim.
 */
// Evaluate all 128 OAM sprites for the given scanline. Builds the sprite pixel
// buffer (objBuffer) with decoded tile data. Respects the 32-sprite-per-line
// and 34-tile-per-line hardware limits (unless NoSpriteLimits flag is set).
// Returns true if any sprites were found on this line.
static bool ppu_evaluateSprites(Ppu* ppu, int line) {
  // TODO: iterate over oam normally to determine in-range sprites,
  //   then iterate those in-range sprites in reverse for tile-fetching
  // TODO: rectangular sprites, wierdness with sprites at -256
  int index = 0, index_end = index;
  int spritesLeft = 32 + 1, tilesLeft = 34 + 1;
  uint8 spriteSizes[2] = { kSpriteSizes[ppu->objSize][0], kSpriteSizes[ppu->objSize][1] };
  int extra_left = ppu->extraLeftCur;
  int extra_right = ppu->extraRightCur;
  if (ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)
    spritesLeft = tilesLeft = 1024;
  int tilesLeftOrg = tilesLeft;

  do {
    int yy = ppu->oam[index] >> 8;
    if (yy == 0xf0)
      continue;  // this works for zelda because sprites are always 8 or 16.
    // check if the sprite is on this line and get the sprite size
    int row = (line - yy) & 0xff;
    int highOam = ppu->oam[0x100 + (index >> 4)] >> (index & 15);
    int spriteSize = spriteSizes[(highOam >> 1) & 1];
    if (row >= spriteSize)
      continue;
    // in y-range, get the x location, using the high bit as well
    int x = (ppu->oam[index] & 0xff) + (highOam & 1) * 256;
    x -= (x >= 256 + extra_right) * 512;
    // if in x-range
    if (x <= -(spriteSize + extra_left))
      continue;
    // break if we found 32 sprites already
    if (--spritesLeft == 0) {
      break;
    }
    // get some data for the sprite and y-flip row if needed
    int oam1 = ppu->oam[index + 1];
    int objAdr = (oam1 & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
    if (oam1 & 0x8000)
      row = spriteSize - 1 - row;
    // fetch all tiles in x-range
    int paletteBase = 0x80 + 16 * ((oam1 & 0xe00) >> 9);
    int prio = SPRITE_PRIO_TO_PRIO((oam1 & 0x3000) >> 12, (oam1 & 0x800) == 0);
    PpuZbufType z = paletteBase + (prio << 8);
    
    for (int col = 0; col < spriteSize; col += 8) {
      if (col + x > -8 - extra_left && col + x < 256 + extra_right) {
        // break if we found 34 8*1 slivers already
        if (--tilesLeft == 0) {
          return true;
        }
        // figure out which tile this uses, looping within 16x16 pages, and get it's data
        int usedCol = oam1 & 0x4000 ? spriteSize - 1 - col : col;
        int usedTile = ((((oam1 & 0xff) >> 4) + (row >> 3)) << 4) | (((oam1 & 0xf) + (usedCol >> 3)) & 0xf);
        uint16 *addr = &ppu->vram[(objAdr + usedTile * 16 + (row & 0x7)) & 0x7fff];
        uint32 plane = addr[0] | addr[8] << 16;
        // go over each pixel
        int px_left = IntMax(-(col + x + kPpuExtraLeftRight), 0);
        int px_right = IntMin(256 + kPpuExtraLeftRight - (col + x), 8);
        PpuZbufType *dst = ppu->objBuffer.data + col + x + px_left + kPpuExtraLeftRight;
        
        for (int px = px_left; px < px_right; px++, dst++) {
          int shift = oam1 & 0x4000 ? px : 7 - px;
          uint32 bits = plane >> shift;
          int pixel = (bits >> 0) & 1 | (bits >> 7) & 2 | (bits >> 14) & 4 | (bits >> 21) & 8;
          // draw it in the buffer if there is a pixel here, and the buffer there is still empty
          if (pixel != 0 && (dst[0] & 0xff) == 0)
            dst[0] = z + pixel;
        }
      }
    }
  } while ((index = (index + 2) & 0xff) != index_end);
  return (tilesLeft != tilesLeftOrg);
}

/*
 * ppu_read — Read one byte from a PPU register (CPU $21xx range, where
 * `adr` is the low byte 0x34..0x3F).
 *
 * The SNES exposes a handful of read-only PPU registers; this emulator
 * only implements the Mode 7 multiplication result registers (MPYL/M/H,
 * $2134-$2136) because that's all Zelda 3 ever reads. The hardware
 * computes a 24-bit signed product of M7A (signed 16-bit) and M7B's
 * high byte (signed 8-bit) and exposes it across three byte registers
 * for cheap fixed-point math without the CPU's own multiplier.
 *
 * Everything else returns 0xFF, which is what the SNES bus delivers
 * for unmapped reads (open-bus behaviour on real hardware varies but
 * a constant 0xFF is what Zelda's code paths actually expect).
 */
// Read a PPU register ($2134-$213F). Only the Mode 7 multiplication result
// registers ($2134-$2136 = MPYL/MPYM/MPYH) are implemented here; others
// return 0xFF (open bus). The multiply result is M7A × (M7B >> 8).
uint8_t ppu_read(Ppu* ppu, uint8_t adr) {
  switch (adr) {
  case 0x34:
  case 0x35:
  case 0x36: {
    int result = ppu->m7matrix[0] * (ppu->m7matrix[1] >> 8);
    return (result >> (8 * (adr - 0x34))) & 0xff;
  }
  }
  return 0xff;
}

/*
 * ppu_write — Write one byte to a PPU register (CPU $21xx range,
 * where `adr` is the low byte 0x00..0x33).
 *
 * Quick register map (only the cases Zelda 3 actually exercises are
 * implemented; many fall through to the default no-op or to an
 * assert() documenting the assumed value):
 *
 *   $2100 INIDISP    brightness (low nibble) + forced blank (bit 7)
 *   $2101 OBSEL      object name base / select / size
 *   $2102/$2103 OAMADDL/H   OAM byte address + priority-rotate bit
 *   $2104 OAMDATA    write-twice into OAM
 *   $2105 BGMODE     BG mode + per-layer tile size (asserted 1 or 7)
 *   $2106 MOSAIC     mosaic size + per-layer enable
 *   $2107..$210A BG1SC..BG4SC  per-layer tilemap address + wider/higher
 *   $210B BG12NBA    tile data address for BG1 + BG2
 *   $210C BG34NBA    tile data address for BG3 + BG4
 *   $210D..$2114 BG1HOFS..BG4VOFS  scroll registers (each write-twice)
 *                                  $210D / $210E also store the Mode 7
 *                                  hScroll/vScroll into m7matrix[6/7]
 *   $2115 VMAIN      VRAM increment mode
 *   $2116/$2117 VMADDL/H   VRAM word address
 *   $2118/$2119 VMDATAL/H  VRAM data port (write low/high byte; either
 *                          may trigger the increment, controlled by
 *                          VMAIN bit 7)
 *   $211A M7SEL      Mode 7 flip/largeField/charFill flags
 *   $211B..$2120 M7A..M7Y matrix + center registers (write-twice)
 *   $2121 CGADD      CGRAM index
 *   $2122 CGDATA     write-twice into CGRAM
 *   $2123 W12SEL     window 1/2 selector for BG1 + BG2
 *   $2124 W34SEL     window 1/2 selector for BG3 + BG4
 *   $2125 WOBJSEL    window 1/2 selector for OBJ + color window
 *   $2126/$2127 WH0/WH1   window 1 left/right
 *   $2128/$2129 WH2/WH3   window 2 left/right
 *   $212A WBGLOG     BG window logic op (asserted 0 = OR)
 *   $212B WOBJLOG    OBJ/Math window logic op (asserted 0 = OR)
 *   $212C TM         main-screen layer enables
 *   $212D TS         sub-screen layer enables
 *   $212E TMW        main-screen window-clip enables
 *   $212F TSW        sub-screen window-clip enables
 *   $2130 CGWSEL     color clip / math-prevent / addsubscreen / directColor
 *   $2131 CGADSUB    subtract / halfColor / per-layer math enable
 *   $2132 COLDATA    fixed-color R/G/B channel writes (bits select which)
 *   $2133 SETINI     interlace/EXTBG (asserted 0)
 *
 * The "write-twice" registers (scroll, M7A..M7Y, CGDATA, OAMDATA)
 * latch the first byte into a small buffer and commit on the second
 * byte. The latch state lives in ppu->oamSecondWrite / cgramSecondWrite
 * for OAM/CGRAM, and in ppu->scrollPrev/m7prev for the scroll/M7
 * groups. Reading/writing the address registers resets the per-port
 * latch.
 */
// Write to a PPU register ($2100-$2133). Handles all display configuration:
// INIDISP, BGMODE, tilemap/tile addresses, scroll, Mode 7 matrix, window
// settings, color math, VRAM/CGRAM/OAM access ports, etc.
void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val) {
  switch(adr) {
    case 0x00: {  // INIDISP
      // Bits 0-3: master brightness 0..15 (0 = black, 15 = full)
      // Bit 7   : forced blank — outputs solid black regardless of
      //           any layer content. Also disables sprite eval.
      ppu->brightness = val & 0xf;
      ppu->forcedBlank = val & 0x80;
      break;
    }
    case 0x01: {
      // $2101 OBSEL — object name base / select / size. Zelda only
      // writes 0x02 (small sprite size = 0, two name tables back-to-
      // back at $0000 and $1000 / $2000 in the SNES's view, mapped
      // here to the fixed objTileAdr1=$4000, objTileAdr2=$5000 set
      // by ppu_reset). The assert pins that assumption.
      assert(val == 2);
      break;
    }
    case 0x02: {
      // $2102 OAMADDL — low byte of OAM word address (in bytes, then
      // halved). Resets the second-write latch so the next $2104 is
      // a fresh first-byte.
      ppu->oamAdr = (ppu->oamAdr & ~0xff) | val;
      ppu->oamSecondWrite = false;
      break;
    }
    case 0x03: {
      // $2103 OAMADDH — high bit of OAM word address (bit 0 here, =
      // OAM byte address bit 9). Bit 7 is the OAM priority-rotate
      // flag, which Zelda never uses (asserted off).
      assert((val & 0x80) == 0);
      ppu->oamAdr = (ppu->oamAdr & ~0xff00) | ((val & 1) << 8);
      ppu->oamSecondWrite = false;
      break;
    }
    case 0x04: {
      // $2104 OAMDATA — write-twice into OAM. First byte buffered,
      // second byte combined with the buffered low byte and stored
      // at the current OAM word address; address advances by 1 word
      // after a complete pair. The < 0x110 bound is because OAM is
      // 0x110 words long (128 sprites × 2 words = 0x100, plus the
      // 0x10-word "high table" for x-msb and size bits).
      if (!ppu->oamSecondWrite) {
        ppu->oamBuffer = val;
      } else {
        if (ppu->oamAdr < 0x110)
          ppu->oam[ppu->oamAdr++] = (val << 8) | ppu->oamBuffer;
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      break;
    }
    case 0x05: {  // BGMODE
      // $2105 BGMODE
      //   bits 0..2 : BG mode (only 1 or 7 are used by Zelda)
      //   bit  3    : BG3 priority bit (mode 1 only — flips BG3 to a
      //               high-priority bucket; the asserts below confirm
      //               Zelda writes either 0x07 (mode 7) or 0x09 (mode
      //               1 with BG3prio), never anything else)
      //   bits 4..7 : per-BG tile size (8×8 vs 16×16) — asserted 0
      //               because Zelda always uses 8×8
      ppu->mode = val & 0x7;
      assert(val == 7 || val == 9);
      assert(ppu->mode == 1 || ppu->mode == 7);
      assert((val & 0xf0) == 0);
      break;
    }
    case 0x06: {  // MOSAIC
      // $2106 MOSAIC
      //   bits 0..3 : per-layer mosaic enable (BG1..BG4)
      //   bits 4..7 : mosaic block size minus 1 (so 0 -> 1px, 15 -> 16px)
      // We disable mosaic entirely when size==1 because a 1×1 mosaic
      // is a no-op; this lets the renderers skip the slower mosaic
      // path whenever the game writes the default value.
      ppu->mosaicSize = (val >> 4) + 1;
      ppu->mosaicEnabled = (ppu->mosaicSize > 1) ? val : 0;
      break;
    }
    case 0x07:  // BG1SC
    case 0x08:
    case 0x09:
    case 0x0a: {
      // $2107..$210A BGxSC — per-layer tilemap base + dimensions
      //   bit 0 : tilemapWider  (1 -> 64 tiles wide vs 32)
      //   bit 1 : tilemapHigher (1 -> 64 tiles tall vs 32)
      //   bits 2..7 : tilemap base address (VRAM word << 8)
      // small tilemaps are used in attract intro
      ppu->bgLayer[adr - 7].tilemapWider = val & 0x1;
      ppu->bgLayer[adr - 7].tilemapHigher = val & 0x2;
      ppu->bgLayer[adr - 7].tilemapAdr = (val & 0xfc) << 8;
      break;
    }
    case 0x0b: {  // BG12NBA
      // $210B BG12NBA — tile-data base for BG1 (low nibble) and BG2
      // (high nibble). Each nibble is shifted left by 12/8 to give a
      // VRAM word address.
      ppu->bgLayer[0].tileAdr = (val & 0xf) << 12;
      ppu->bgLayer[1].tileAdr = (val & 0xf0) << 8;
      break;
    }
    case 0x0c: { // BG34NBA
      // $210C BG34NBA — same encoding as BG12NBA but for BG3/BG4.
      ppu->bgLayer[2].tileAdr = (val & 0xf) << 12;
      ppu->bgLayer[3].tileAdr = (val & 0xf0) << 8;
      break;
    }
    case 0x0d: { // BG1HOFS
      // $210D BG1HOFS — pulls double duty as Mode 7 hScroll (m7matrix[6])
      // AND BG1 horizontal scroll. The mode-7 store happens first
      // (write-twice via m7prev), then fallthrough falls into the
      // normal BG-HOFS handler which uses scrollPrev/scrollPrev2.
      // The 10-bit mask keeps scroll in 0..1023; mode 7 keeps 13 bits.
      ppu->m7matrix[6] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-HOFS
    }
    case 0x0f:
    case 0x11:
    case 0x13: {
      // $210F, $2111, $2113 — BG2/BG3/BG4 hScroll (write-twice). The
      // weird `(scrollPrev & 0xf8) | (scrollPrev2 & 0x7)` formula
      // matches the real SNES write-twice latch behaviour: low 3 bits
      // come from the *previous* register's previous write, top 5
      // bits from the prior write to THIS register.
      ppu->bgLayer[(adr - 0xd) / 2].hScroll = ((val << 8) | (ppu->scrollPrev & 0xf8) | (ppu->scrollPrev2 & 0x7)) & 0x3ff;
      ppu->scrollPrev = val;
      ppu->scrollPrev2 = val;
      break;
    }
    case 0x0e: { // BG1VOFS
      // $210E BG1VOFS — like $210D but for vertical: also stores into
      // Mode 7 vScroll (m7matrix[7]) before falling through.
      ppu->m7matrix[7] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-VOFS
    }
    case 0x10:
    case 0x12:
    case 0x14: {
      // $2110, $2112, $2114 — BG2/BG3/BG4 vScroll. Simpler latch than
      // hScroll: just (val << 8) | scrollPrev, no scrollPrev2.
      ppu->bgLayer[(adr - 0xe) / 2].vScroll = ((val << 8) | ppu->scrollPrev) & 0x3ff;
      ppu->scrollPrev = val;
      break;
    }
    case 0x15: {  // VMAIN
      // $2115 VMAIN — VRAM access mode
      //   bits 0..1 : increment step
      //               00 -> 1 word, 01 -> 32 words, 10/11 -> 128 words
      //   bits 2..3 : address translation mode (asserted 0 — Zelda
      //               always uses straight increment, never the
      //               4bpp/2bpp/8bpp bitplane interleave modes)
      //   bit  7    : increment-on-which-half (0 = after low byte
      //               write, 1 = after high byte write)
      if((val & 3) == 0) {
        ppu->vramIncrement = 1;
      } else if((val & 3) == 1) {
        ppu->vramIncrement = 32;
      } else {
        ppu->vramIncrement = 128;
      }
      assert(((val & 0xc) >> 2) == 0);
      ppu->vramIncrementOnHigh = val & 0x80;
      break;
    }
    case 0x16: {  // VMADDL
      // $2116 VMADDL — low byte of VRAM word address.
      ppu->vramPointer = (ppu->vramPointer & 0xff00) | val;
      break;
    }
    case 0x17: {  // VMADDH
      // $2117 VMADDH — high byte of VRAM word address. VRAM is 64 KB
      // and is indexed by 15-bit word address (bits 0..14); bit 15
      // is masked off on every read/write below.
      ppu->vramPointer = (ppu->vramPointer & 0x00ff) | (val << 8);
      break;
    }
    case 0x18: {  // VMDATAL
      // $2118 VMDATAL — write low byte of the VRAM word; if VMAIN
      // bit 7 is 0, advance the pointer after THIS write.
      uint16_t vramAdr = ppu->vramPointer;
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0xff00) | val;
      if(!ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x19: {  // VMDATAH
      // $2119 VMDATAH — write high byte of the VRAM word; if VMAIN
      // bit 7 is 1, advance the pointer after THIS write instead.
      uint16_t vramAdr = ppu->vramPointer;
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0x00ff) | (val << 8);
      if(ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x1a: {  // M7SEL
      // $211A M7SEL — Mode 7 control bits
      //   bit 0 : m7xFlip       (horizontal mirror)
      //   bit 1 : m7yFlip       (vertical mirror)
      //   bit 6 : m7charFill    (out-of-range -> tile 0 vs transparent)
      //   bit 7 : m7largeField  (clip vs wrap past 1024)
      // Zelda always writes 0x80 (only largeField on), which the
      // assert documents.
      assert(val == 0x80);
      ppu->m7largeField = val & 0x80;
      ppu->m7charFill = val & 0x40;
      ppu->m7yFlip = val & 0x2;
      ppu->m7xFlip = val & 0x1;
      break;
    }
    case 0x1b:  // M7A etc
    case 0x1c:
    case 0x1d:
    case 0x1e: {
      // $211B..$211E M7A/M7B/M7C/M7D — 16-bit signed matrix elements
      // (a, b, c, d). Write-twice via m7prev.
      ppu->m7matrix[adr - 0x1b] = (val << 8) | ppu->m7prev;
      ppu->m7prev = val;
      break;
    }
    case 0x1f:
    case 0x20: {
      // $211F M7X, $2120 M7Y — 13-bit signed center coordinates.
      // Masked to 13 bits here; the 13-bit-to-int sign extension is
      // done at the read site (PpuDrawBackground_mode7 and
      // ppu_calculateMode7Starts) so we don't lose precision storing
      // them.
      ppu->m7matrix[adr - 0x1b] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      break;
    }
    case 0x21: {
      // $2121 CGADD — CGRAM word index (0..255). Resets the
      // write-twice latch.
      ppu->cgramPointer = val;
      ppu->cgramSecondWrite = false;
      break;
    }
    case 0x22: {
      // $2122 CGDATA — write-twice 15-bit BGR palette word. Address
      // post-increments after the high byte commits the word.
      if(!ppu->cgramSecondWrite) {
        ppu->cgramBuffer = val;
      } else {
        ppu->cgram[ppu->cgramPointer++] = (val << 8) | ppu->cgramBuffer;
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      break;
    }
    case 0x23:  // W12SEL
      // $2123 W12SEL — packed 4-bit selectors for BG1 (low nibble) and
      // BG2 (high nibble). Each nibble matches the kWindow* enum:
      // bit 0 = w1 inverse, bit 1 = w1 enable, bit 2 = w2 inverse,
      // bit 3 = w2 enable. Stored in the low 8 bits of windowsel.
      ppu->windowsel = (ppu->windowsel & ~0xff) | val;
      break;
    case 0x24:  // W34SEL
      // $2124 W34SEL — same encoding as W12SEL but for BG3 (low nibble)
      // and BG4 (high nibble). Stored in bits 8..15 of windowsel.
      ppu->windowsel = (ppu->windowsel & ~0xff00) | (val << 8);
      break;
    case 0x25:  // WOBJSEL
      // $2125 WOBJSEL — OBJ (low nibble) and color-math window (high
      // nibble). Stored in bits 16..23 of windowsel.
      ppu->windowsel = (ppu->windowsel & ~0xff0000) | (val << 16);
      break;
    case 0x26:
      // $2126 WH0 — window 1 left edge (inclusive)
      ppu->window1left = val;
      break;
    case 0x27:
      // $2127 WH1 — window 1 right edge (inclusive)
      ppu->window1right = val;
      break;
    case 0x28:
      // $2128 WH2 — window 2 left edge (inclusive)
      ppu->window2left = val;
      break;
    case 0x29:
      // $2129 WH3 — window 2 right edge (inclusive)
      ppu->window2right = val;
      break;
    case 0x2a:  // WBGLOG
      // $212A WBGLOG — BG window logic op. The SNES supports OR, AND,
      // XOR, XNOR; Zelda always writes 0 (OR), which PpuWindows_Calc
      // hard-codes.
      assert(val == 0);
      break;
    case 0x2b:  // WOBJLOG
      // $212B WOBJLOG — same as WBGLOG but for OBJ and color-math
      // windows. Asserted 0 (OR).
      assert(val == 0);
      break;
    case 0x2c:  // TM
      // $212C TM — main-screen layer enables (bit 0=BG1, ..., bit 4=OBJ)
      ppu->screenEnabled[0] = val;
      break;
    case 0x2d:  // TS
      // $212D TS — sub-screen layer enables (same bit layout)
      ppu->screenEnabled[1] = val;
      break;
    case 0x2e: // TMW
      // $212E TMW — per-layer "apply window clip on main screen" mask
      ppu->screenWindowed[0] = val;
      break;
    case 0x2f:  // TSW
      // $212F TSW — per-layer "apply window clip on sub screen" mask
      ppu->screenWindowed[1] = val;
      break;
    case 0x30: {  // CGWSEL
      // $2130 CGWSEL — color clip & math control
      //   bit 0    : directColor (asserted 0 — Zelda always uses CGRAM)
      //   bit 1    : addSubscreen (true -> math adds sub screen pixel,
      //              false -> math uses fixedColor)
      //   bits 4..5: preventMathMode (when math is "off in a region"):
      //              0 = never prevent
      //              1 = prevent outside color window
      //              2 = prevent inside color window
      //              3 = always prevent
      //   bits 6..7: clipMode (color clip — same encoding as
      //              preventMathMode but for blacking-out main RGB)
      assert((val & 1) == 0);  // directColor always zero
      ppu->addSubscreen = val & 0x2;
      ppu->preventMathMode = (val & 0x30) >> 4;
      ppu->clipMode = (val & 0xc0) >> 6;
      break;
    }
    case 0x31: {  // CGADSUB
      // $2131 CGADSUB — color math operation
      //   bit 7 : subtract instead of add
      //   bit 6 : halve the result
      //   bits 0..5 : per-layer math enable (BG1..BG4, OBJ, backdrop)
      ppu->subtractColor = val & 0x80;
      ppu->halfColor = val & 0x40;
      ppu->mathEnabled = val & 0x3f;
      break;
    }
    case 0x32: {  // COLDATA
      // $2132 COLDATA — fixed-color channel writes
      //   bit 7 : if set, low 5 bits go into the Blue channel
      //   bit 6 : Green channel
      //   bit 5 : Red channel
      // All three may be set in the same write; the same low-5-bit
      // value is then written to multiple channels.
      if(val & 0x80) ppu->fixedColorB = val & 0x1f;
      if(val & 0x40) ppu->fixedColorG = val & 0x1f;
      if(val & 0x20) ppu->fixedColorR = val & 0x1f;
      break;
    }
    case 0x33: {
      // $2133 SETINI — interlace + EXTBG + overscan + external sync.
      // Asserted 0 because Zelda never enables any of those. The
      // m7extBg_always_zero shadow exists for completeness but is, as
      // the field name says, always zero.
      assert(val == 0);
      ppu->m7extBg_always_zero = val & 0x40;
      break;
    }
    default: {
      // Any other $21xx write is ignored. The host CPU emulator may
      // still call us with unmapped addresses; silently dropping them
      // matches open-bus behaviour for the unused PPU registers.
      break;
    }
  }
}

