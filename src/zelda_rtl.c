/*
 * zelda_rtl.c — Zelda Runtime Library (bridge between SNES emulation and C game)
 *
 * Part of the Zelda 3 C reimplementation (The Legend of Zelda: A Link to the Past).
 *
 * This file is the host-side "platform shell" that the rewritten C game logic
 * sits on top of. Its responsibilities, roughly:
 *
 *   - Per-frame render driver. ZeldaDrawPpuFrame runs the line-by-line PPU,
 *     advances two emulated HDMA channels (SimpleHdma), and handles the
 *     mid-frame IRQ used by the file-select character grid.
 *
 *   - Game loop entry. ZeldaRunFrame is what the platform layer (main.c
 *     SDL pump or the Switch / Win32 builds) calls 60 times per second.
 *     It records input, drives replay, decides whether to also run the
 *     polyhedral Triforce renderer this tick, and invokes the rewritten
 *     C game code (ZeldaRunFrameInternal) — or, when wired to a real
 *     65C816 emulator, defers to that for cross-implementation comparison.
 *
 *   - Save state. SaveSnesState / LoadSnesState marshal the full SNES
 *     hardware snapshot (APU RAM + DSP + DMA + PPU + WRAM + SRAM) to a
 *     blob; StateRecorder layers an input event log on top so play
 *     sessions can be replayed and rewound.
 *
 *   - SRAM persistence. ZeldaReadSram / ZeldaWriteSram read and write
 *     the 8KB battery-backed save file to disk.
 *
 *   - Localization. ZeldaSetLanguage selects the dialogue + font asset
 *     blocks based on a language string ("en", "de", "fr", ...).
 *
 *   - Emulator-parity hooks. EmuSyncMemoryRegion / EmuSynchronizeWholeState
 *     mirror WRAM mutations from the C side back into the reference
 *     emulator's address space so the two implementations stay byte-
 *     identical during regression runs.
 *
 *   - Debug patches. PatchCommand provides single-key cheats (refill
 *     health / magic / bombs / rupees, toggle bunny mode, clear the
 *     replay log) used during development.
 *
 * Related files:
 *   zelda_rtl.h        — Public interface for everything in this file
 *   variables.h        — WRAM-mapped globals (frame_counter, irq_flag, ...)
 *   snes/ppu.h         — Picture Processing Unit emulator
 *   snes/dma.h         — DMA / HDMA emulator
 *   spc_player.h       — Audio Processing Unit (SPC700 + DSP) emulator
 *   assets.h           — Dialogue / font / asset block descriptors
 *   audio.h            — APU lock/unlock and music-after-load helpers
 */

/* Engine includes — runtime / game variables / module routing */
#include "zelda_rtl.h"
#include "variables.h"
#include "misc.h"
#include "nmi.h"
#include "poly.h"
#include "attract.h"

/* SNES hardware emulation */
#include "snes/ppu.h"
#include "snes/snes_regs.h"
#include "snes/dma.h"
#include "spc_player.h"

/* Utility and platform glue */
#include "util.h"
#include "audio.h"
#include "assets.h"
#include "config.h"
#include "hud.h"

/*
 * g_zenv — Process-wide pointer hub for the SNES subsystems plus SRAM,
 *          VRAM, the SPC player, and the active dialogue/font asset
 *          blocks. Every consumer reaches the PPU/DMA/etc. through this
 *          struct rather than holding subsystem pointers directly.
 */
ZeldaEnv g_zenv;

/*
 * g_ram — 128KB host-side mirror of SNES Work RAM. The game's WRAM-mapped
 *         variables (defined in variables.h via byte offsets into this
 *         buffer) live here. Allocated statically so its address is fixed
 *         for the lifetime of the program; emulator parity code can
 *         memcpy this to/from the reference emu's WRAM image.
 */
uint8 g_ram[131072];

/*
 * g_wanted_zelda_features — Bitfield of enhancement-feature flags the
 *   platform layer has requested for this session (e.g. widescreen,
 *   no-flicker sprites, MSU-1 audio). Pushed into the game-visible
 *   enhanced_features0 once per frame so the change appears atomically
 *   in the same tick as input is read.
 */
uint32 g_wanted_zelda_features;

/* Forward declaration — Startup_InitializeMemory zeroes WRAM and validates SRAM
 * on first frame. Defined below. */
static void Startup_InitializeMemory();

/*
 * SimpleHdma — Minimal HDMA channel state for the per-scanline transfer engine.
 *
 * The SNES HDMA mechanism runs alongside scanline rendering: every line the
 * engine reads one or more bytes from a "table" in CPU memory and writes
 * them to a PPU register, producing per-scanline effects (mode 7 perspective
 * zoom, spotlight irises, wavy water, etc.). ZeldaDrawPpuFrame emulates this
 * by stepping two SimpleHdma instances (channels 6 and 7) once per scanline.
 *
 * Fields:
 *   table       Pointer into the table being read. NULL when this channel is
 *               inactive or has hit its terminator.
 *   indir_ptr   When mode & 0x40 is set (indirect addressing), this is the
 *               pointer that the table redirects to for actual transfer data.
 *   rep_count   Repeat counter for the current segment. Low 7 bits = lines
 *               remaining; bit 7 = "continue" flag (transfer every line vs
 *               only on the first line of a new segment).
 *   mode        Low 3 bits = transfer mode (see transferLength + bAdrOffsets
 *               below); bit 6 = indirect addressing.
 *   ppu_addr    PPU register low byte (target = 0x2100 + ppu_addr).
 *   indir_bank  CPU bank byte used for indirect-mode pointer resolution.
 */
typedef struct SimpleHdma {
  const uint8 *table;
  const uint8 *indir_ptr;
  uint8 rep_count;
  uint8 mode;
  uint8 ppu_addr;
  uint8 indir_bank;
} SimpleHdma;
static void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc);
static void SimpleHdma_DoLine(SimpleHdma *c);

/*
 * bAdrOffsets — Per-mode lane offsets for HDMA writes. The 8 modes map a
 * source byte to one of 4 PPU register lanes (target+0, target+1, etc.).
 *   mode 0: 1 byte to single register   (target+0)
 *   mode 1: 2 bytes to two registers    (target+0, target+1)
 *   mode 2: 2 bytes to same register    (target+0, target+0)
 *   mode 3: 4 bytes, two pairs          (+0, +0, +1, +1)
 *   mode 4: 4 bytes to four registers   (+0, +1, +2, +3)
 *   mode 5: 4 bytes, alternating pair   (+0, +1, +0, +1)
 *   mode 6/7: same as 2/3
 * The table mirrors the SNES DMA controller's hardware behavior.
 */
static const uint8 bAdrOffsets[8][4] = {
  {0, 0, 0, 0},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1},
  {0, 1, 2, 3},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1}
};

/*
 * transferLength — Number of bytes consumed per HDMA write for each mode.
 * Used by SimpleHdma_DoLine to drive its inner write loop the right number
 * of times. Indexed by mode & 7 to match bAdrOffsets.
 */
static const uint8 transferLength[8] = {
  1, 2, 2, 4, 4, 4, 2, 4
};

static bool Zelda_ShouldRenderWideHudOverlay() {
  if (overworld_map_state == 0 &&
      (main_module_index == 7 || main_module_index == 8 || main_module_index == 9 ||
       main_module_index == 11 || main_module_index == 15))
    return true;

  if (main_module_index != 14)
    return false;

  if (submodule_index == 1)
    return overworld_map_state == 0 || overworld_map_state == 1 ||
           overworld_map_state == 2 || overworld_map_state == 6;

  if (submodule_index == 12)
    return overworld_map_state == 0 || overworld_map_state == 1 ||
           overworld_map_state == 2 || overworld_map_state == 4 ||
           Hud_NewSettingsMenuWantsHudPreview();

  return false;
}

// Gameplay and transition modules keep the vanilla BG3 HUD visible, so BG3
// needs the same viewport anchor when ExtendedAspectRatio side-space is active.
static bool Zelda_IsGameplayModuleForBg3Anchor(uint8 module) {
  return module == 7 || module == 8 || module == 9 || module == 11 || module == 15;
}

static bool Zelda_ShouldAnchorBg3ToViewport() {
  if (!(enhanced_features0 & kFeatures0_ExtendScreen64) || g_config.extended_aspect_ratio == 0)
    return false;

  if (main_module_index == 14) {
    if (submodule_index == 7 || submodule_index == 10)
      return overworld_map_state == 0 && Zelda_IsGameplayModuleForBg3Anchor(saved_module_for_menu);
    if (submodule_index == 4 || submodule_index == 8 || submodule_index == 9)
      return Zelda_IsGameplayModuleForBg3Anchor(saved_module_for_menu);
    return submodule_index == 1 || submodule_index == 2 || submodule_index == 11 ||
           submodule_index == 12;
  }

  return overworld_map_state == 0 && Zelda_IsGameplayModuleForBg3Anchor(main_module_index);
}

/*
 * kUpperBitmasks — 16 single-bit masks ordered from MSB (0x8000) down to LSB
 * (0x0001). Indexed by a bit position to produce a one-hot uint16 without
 * needing a shift. Used by code that walks fixed-size bit collections such
 * as dungeon door states and overworld revealed-secret flags.
 */
const uint16 kUpperBitmasks[] = { 0x8000, 0x4000, 0x2000, 0x1000, 0x800, 0x400, 0x200, 0x100, 0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1 };

/*
 * kLitTorchesColorPlus — Palette intensity bumps applied per lit torch in a
 * dungeon room. The 4 entries index by lit-torch count (0..3); torch
 * lighting brightens the room palette in steps of 31 → 8 → 4 → 0 (i.e.
 * the first torch adds the most, additional torches contribute less).
 */
const uint8 kLitTorchesColorPlus[] = {31, 8, 4, 0};

/*
 * kDungeonCrystalPendantBit — Per-dungeon "you got the prize" save flag
 * bitmasks. Indexed by dungeon ID (0..12); each entry is a single bit
 * within byte 0xf36c of WRAM. Zero entries are dungeons that don't have
 * a pendant/crystal (Hyrule Castle, Sewers).
 */
const uint8 kDungeonCrystalPendantBit[13] = {0, 0, 4, 2, 0, 16, 2, 1, 64, 4, 1, 32, 8};

/*
 * kGetBestActionToPerformOnTile_x / _y — Pixel offsets relative to Link's
 * position for the 4-way "what's that tile?" probe. Indices map to Down,
 * Up, Left, Right respectively. Y-offsets are deeper because the tile in
 * the direction Link is facing is biased toward his feet, not his head.
 */
const int8 kGetBestActionToPerformOnTile_x[4] = { 7, 7, -3, 16 };
const int8 kGetBestActionToPerformOnTile_y[4] = { 6, 24, 12, 12 };

/* AT_WORD — Splits a 16-bit value into low-byte, high-byte pair for use
 * inside the HDMA table literals below (SNES HDMA tables are little-endian
 * byte streams). */
#define AT_WORD(x) (uint8)(x), (x)>>8
/*
 * Static HDMA tables used by various sequences. Each entry is a stream of
 *   <rep_count> <data...> [<rep_count> <data...>...] <terminator = 0>
 * with bit 7 of rep_count toggling "transfer every line" vs "transfer
 * only on segment boundary." Indirect-mode tables encode 16-bit pointers
 * after the rep_count (split via AT_WORD).
 */
// direct
static const uint8 kAttractDmaTable0[13] = {0x20, AT_WORD(0x00ff), 0x50, AT_WORD(0xe018), 0x50, AT_WORD(0xe018), 1, AT_WORD(0x00ff), 0};
static const uint8 kAttractDmaTable1[10] = {0x48, AT_WORD(0x00ff), 0x30, AT_WORD(0xd830), 1, AT_WORD(0x00ff), 0};
/* HDMA color-band table used during the ending cutscene to drive the
 * vertically-banded sky and color-math gradients. */
static const uint8 kHdmaTableForEnding[19] = {
  0x52, AT_WORD(0x600), 8, AT_WORD(0xe2), 8, AT_WORD(0x602), 5, AT_WORD(0x604), 0x10, AT_WORD(0x606), 0x81, AT_WORD(0xe2), 0,
};
/* Window-mask HDMA used by the iris-spotlight transition between rooms. The
 * two 0xf8 segments each cover 120 scanlines, redirecting to the rolling
 * spotlight position table in hdma_table_dynamic. */
static const uint8 kSpotlightIndirectHdma[7] = {0xf8, AT_WORD(0x1b00), 0xf8, AT_WORD(0x1bf0), 0};
/* Mode-7 zoom HDMA pair driving the overworld map's perspective shrink. */
static const uint8 kMapModeHdma0[7] = {0xf0, AT_WORD(0xdd27), 0xf0, AT_WORD(0xde07), 0};
static const uint8 kMapModeHdma1[7] = {0xf0, AT_WORD(0xdee7), 0xf0, AT_WORD(0xdfc7), 0};
/* Mode-7 HDMA used in the attract / title screen Triforce zoom. */
static const uint8 kAttractIndirectHdmaTab[7] = {0xf0, AT_WORD(0x1b00), 0xf0, AT_WORD(0x1be0), 0};
/* HDMA driving the wavy-window effect during the prayer / desert tablet
 * scene. */
static const uint8 kHdmaTableForPrayingScene[7] = {0xf8, AT_WORD(0x1b00), 0xf8, AT_WORD(0x1bf0), 0};

/*
 * zelda_ppu_write — Thin wrapper around ppu_write that validates the address
 * is in the PPU register window ($2100..$213F). Strips the high bits so
 * the PPU emulator sees just the low register byte.
 */
void zelda_ppu_write(uint32_t adr, uint8_t val) {
  assert(adr >= INIDISP && adr <= STAT78);
  ppu_write(g_zenv.ppu, (uint8)adr, val);
}

/*
 * zelda_ppu_write_word — Convenience routine that writes a 16-bit value as
 * two consecutive PPU register writes (low byte then high byte). Used for
 * registers like BG scroll where the SNES expects two byte writes to the
 * same address (BGnHOFS / BGnVOFS) but the C code carries a uint16.
 */
void zelda_ppu_write_word(uint32_t adr, uint16_t val) {
  zelda_ppu_write(adr, val);
  zelda_ppu_write(adr + 1, val >> 8);
}

/*
 * SimpleHdma_GetPtr — Resolve a SNES bus address to a host-side pointer.
 *
 * The original SNES code references HDMA tables by 24-bit SNES bus address
 * (bank + 16-bit offset). After porting, those tables live in static C
 * data or in WRAM, so this routine maps each address that the game
 * actually configures into the matching C-side pointer.
 *
 * The first group of cases (0xCFA87..0x2c80c) are ROM-resident tables
 * relocated into kAttract* / kHdma* / kMapMode* / kSpotlight* constants
 * above. The 0x1b00 / 0x1be0 / 0x1bf0 cases point into hdma_table_dynamic
 * (the rolling staging buffer used for spotlight/iris animations). The
 * 0xadd27..0xadfc7 group points into kMapMode_Zooms1/2 (perspective LUT).
 * The 0x600..0x606 / 0xe2 group reads back from g_ram for the
 * color-band HDMA used in the ending sequence.
 *
 * assert(0) on miss because every address the game asks for must have a
 * mapping — an unknown address would silently produce wrong HDMA output.
 */
static const uint8 *SimpleHdma_GetPtr(uint32 p) {
  switch (p) {

  case 0xCFA87: return kAttractDmaTable0;
  case 0xCFA94: return kAttractDmaTable1;
  case 0xebd53: return kHdmaTableForEnding;
  case 0x0F2FB: return kSpotlightIndirectHdma;
  case 0xabdcf: return kMapModeHdma0;             // mode7
  case 0xabdd6: return kMapModeHdma1;             // mode7
  case 0xABDDD: return kAttractIndirectHdmaTab;   // mode7
  case 0x2c80c: return kHdmaTableForPrayingScene;

  case 0x1b00: return (uint8 *)hdma_table_dynamic;
  case 0x1be0: return (uint8 *)hdma_table_dynamic + 0xe0;
  case 0x1bf0: return (uint8 *)hdma_table_dynamic + 0xf0;
  case 0xadd27: return (uint8*)kMapMode_Zooms1;
  case 0xade07: return (uint8*)kMapMode_Zooms1 + 0xe0;
  case 0xadee7: return (uint8*)kMapMode_Zooms2;
  case 0xadfc7: return (uint8*)kMapMode_Zooms2 + 0xe0;
  case 0x600: return &g_ram[0x600];
  case 0x602: return &g_ram[0x602];
  case 0x604: return &g_ram[0x604];
  case 0x606: return &g_ram[0x606];
  case 0xe2: return &g_ram[0xe2];
  default:
    assert(0);
    return NULL;
  }
}

/*
 * SimpleHdma_Init — Seed a SimpleHdma instance from a DMA channel's state.
 *
 * Called once per frame at the start of rendering. If the channel isn't
 * marked HDMA-active, table is set to NULL so SimpleHdma_DoLine treats
 * the channel as silent. Otherwise the table pointer is resolved from
 * the SNES bus address (24-bit aBank:aAdr), and mode/indirect/bAdr are
 * cached locally so SimpleHdma_DoLine never has to touch the underlying
 * DmaChannel struct mid-scanline.
 */
static void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc) {
  if (!dc->hdmaActive) {
    c->table = 0;
    return;
  }
  c->table = SimpleHdma_GetPtr(dc->aAdr | dc->aBank << 16);
  c->rep_count = 0;
  c->mode = dc->mode | dc->indirect << 6;
  c->ppu_addr = dc->bAdr;
  c->indir_bank = dc->indBank;
}

/*
 * SimpleHdma_DoLine — Advance one HDMA channel by one scanline.
 *
 * Mirrors the SNES HDMA controller's per-line behavior:
 *
 *   1. Segment boundary. When the low 7 bits of rep_count are zero, fetch
 *      the next segment header byte. A header of zero terminates this
 *      channel (table → NULL). For indirect-mode channels (mode & 0x40),
 *      the next two bytes are a pointer to where the actual transfer
 *      data lives — resolved via SimpleHdma_GetPtr.
 *
 *   2. Transfer. The transfer happens either:
 *        - on the first line of every new segment (do_transfer set), or
 *        - on every line if rep_count's high bit is set ("continue" mode).
 *      The inner loop writes transferLength[mode] bytes to consecutive
 *      PPU register lanes picked by bAdrOffsets[mode][j].
 *
 *   3. Decrement rep_count so the next call moves toward the next segment.
 */
static void SimpleHdma_DoLine(SimpleHdma *c) {
  if (c->table == NULL)
    return;
  bool do_transfer = false;
  if ((c->rep_count & 0x7f) == 0) {
    c->rep_count = *c->table++;
    if (c->rep_count == 0) {
      c->table = NULL;
      return;
    }
    if(c->mode & 0x40) {
      c->indir_ptr = SimpleHdma_GetPtr(c->indir_bank << 16 | c->table[0] | c->table[1] * 256);
      c->table += 2;
    }
    do_transfer = true;
  }
  if(do_transfer || c->rep_count & 0x80) {
    for(int j = 0, j_end = transferLength[c->mode & 7]; j < j_end; j++) {
      uint8 v = c->mode & 0x40 ? *c->indir_ptr++ : *c->table++;
      zelda_ppu_write(0x2100 + c->ppu_addr + bAdrOffsets[c->mode & 7][j], v);
    }
  }
  c->rep_count--;
}

/*
 * ConfigurePpuSideSpace — Tell the PPU how much extra screen area is safe to
 * render for widescreen / extra-height modes.
 *
 * The original SNES only displays a 256x224 viewport, but this port can
 * widen the rendering window when widescreen mode is enabled. To avoid
 * showing garbage outside the room (or off the edge of an overworld
 * screen), the PPU emulator needs to know how far it can safely draw on
 * each side and below. That distance depends on the current game module:
 *
 *   - Module 9 (overworld) and module 11 (special overworld): use
 *     ow_scroll_vars0 to clamp to the current screen's bounds, EXCEPT when
 *     it's the Mode 7 world map (module 14 submodule 7 stage ≥ 4), where the
 *     fixed kPpuExtraLeftRight is safe.
 *   - Module 7 (indoors): clamp to room_bounds_x/_y for the current
 *     quadrant. The light-cone case (dark room with active lantern)
 *     skips the horizontal clamp because the cone shouldn't see beyond
 *     the lantern's circle anyway.
 *   - Transition modules that temporarily wrap indoor/overworld gameplay
 *     are treated as whichever world they are currently presenting, so
 *     house exits do not briefly collapse back to the 4:3 side padding.
 *   - Indoor transition submodules keep the old side-space only until
 *     the destination room has loaded enough geometry to report its own
 *     bounds, avoiding a visible resize when control returns.
 *   - Module 20/0/1 (title / intro / file select): allow the fixed
 *     widescreen extension since these screens are statically composed.
 *   - Anything else (cutscenes, menus): leave zeroed, no extension.
 */
static void ConfigurePpuSideSpace() {
  // Let PPU impl know about the maximum allowed extra space on the sides and bottom
  int extra_right = 0, extra_left = 0, extra_bottom = 0;
  static bool last_indoor_side_space_valid;
  static int last_indoor_extra_left, last_indoor_extra_right, last_indoor_extra_bottom;
//  printf("main %d, sub %d  (%d, %d, %d)\n", main_module_index, submodule_index, BG2HOFS_copy2, room_bounds_x.v[2 | (quadrant_fullsize_x >> 1)], quadrant_fullsize_x >> 1);
  int mod = main_module_index;
  if (mod == 14)
    mod = saved_module_for_menu;
  if (mod == 15 || mod == 16)
    mod = player_is_indoors ? 7 : 9;
  else if (mod == 8 || mod == 10 || mod == 11)
    mod = 9;
  if (mod == 9) {
    if (main_module_index == 14 && submodule_index == 7 && overworld_map_state >= 4) {
      // World map
      int target_extra = UintMin(g_config.extended_aspect_ratio, g_zenv.ppu->extraLeftRight);
      extra_left = target_extra, extra_right = target_extra;
      extra_bottom = 16;
    } else {
      // outdoors
      int target_extra = UintMin(g_config.extended_aspect_ratio, g_zenv.ppu->extraLeftRight);
      int avail_left = IntMax(BG2HOFS_copy2 - ow_scroll_vars0.xstart, 0);
      int avail_right = IntMax(ow_scroll_vars0.xend - BG2HOFS_copy2, 0);
      int wanted_total = target_extra * 2;
      extra_left = IntMin(avail_left, target_extra);
      extra_right = IntMin(avail_right, wanted_total - extra_left);
      extra_left = IntMin(avail_left, wanted_total - extra_right);
      extra_bottom = ow_scroll_vars0.yend - BG2VOFS_copy2;
    }
  } else if (mod == 7) {
    bool seamless_transition =
        (enhanced_features0 & (kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes)) &&
        (submodule_index == 1 || submodule_index == 2 || submodule_index == 6 ||
         submodule_index == 14 || (submodule_index >= 17 && submodule_index <= 19));
    bool hold_previous_side_space =
        seamless_transition &&
        (submodule_index == 1 ? true :
         submodule_index == 2 ? subsubmodule_index < 4 :
         submodule_index == 6 ? subsubmodule_index < 7 :
         submodule_index == 14 ? subsubmodule_index < 7 :
         submodule_index >= 17 && submodule_index <= 19 ? subsubmodule_index < 3 :
         false);
    // indoors, except when the light cone is in use
    if (!(hdr_dungeon_dark_with_lantern && TS_copy != 0)) {
      int target_extra = UintMin(g_config.extended_aspect_ratio, g_zenv.ppu->extraLeftRight);
      int qm = quadrant_fullsize_x >> 1;
      int avail_left = IntMax(BG2HOFS_copy2 - room_bounds_x.v[qm], 0);
      int avail_right = IntMax(room_bounds_x.v[qm + 2] - BG2HOFS_copy2, 0);
      int wanted_total = target_extra * 2;
      extra_left = IntMin(avail_left, target_extra);
      extra_right = IntMin(avail_right, wanted_total - extra_left);
      extra_left = IntMin(avail_left, wanted_total - extra_right);
    }

    int qy = quadrant_fullsize_y >> 1;
    extra_bottom = IntMax(room_bounds_y.v[qy + 2] - BG2VOFS_copy2, 0);

    if (hold_previous_side_space && last_indoor_side_space_valid) {
      extra_left = last_indoor_extra_left;
      extra_right = last_indoor_extra_right;
      extra_bottom = last_indoor_extra_bottom;
    } else {
      last_indoor_side_space_valid = true;
      last_indoor_extra_left = extra_left;
      last_indoor_extra_right = extra_right;
      last_indoor_extra_bottom = extra_bottom;
    }
  } else if (mod == 20 || mod == 0 || mod == 1) {
    int target_extra = UintMin(g_config.extended_aspect_ratio, g_zenv.ppu->extraLeftRight);
    extra_left = target_extra, extra_right = target_extra;
    extra_bottom = 16;
  }
  PpuSetExtraSideSpace(g_zenv.ppu, extra_left, extra_right, extra_bottom);
}

/*
 * ZeldaDrawPpuFrame — Render one complete frame into pixel_buffer.
 *
 * The frame-by-frame core of the visual pipeline. Called from the platform
 * layer (main.c's DrawPpuFrameWithPerf, the Switch / OpenGL backends, etc.)
 * once per video frame.
 *
 *   1. PpuBeginDrawing arms the PPU emulator with the output target.
 *   2. dma_startDma transfers any pending general DMA (e.g. queued VRAM
 *      uploads from NMI) into PPU memory before any pixels are drawn.
 *   3. Seed two SimpleHdma channels from DMA channels 6 and 7 — the
 *      original game uses exactly those two for its HDMA effects.
 *   4. Mode-7 perspective hint: when the PPU's 4x4 supersampled mode 7 is
 *      enabled, the renderer needs to know the first and last scanline's
 *      zoom values so it can interpolate cleanly. Match against the known
 *      HDMA table pointers (kMapModeHdma0/1, kAttractIndirectHdmaTab, or
 *      hdma_table_dynamic for the attract intro) and pass those values
 *      straight to the PPU instead of having it sniff them from the
 *      HDMA stream.
 *   5. If widescreen or 240-line mode is active, refresh the safe-render
 *      window via ConfigurePpuSideSpace.
 *   6. Per-scanline loop (224 or 240 lines):
 *      - At line 128, if irq_flag is set, fire the manual IRQ used by the
 *        file-select character grid (writes selectfile_var8 into BG3HOFS
 *        and resets BG3VOFS, optionally re-arming NMITIMEN).
 *      - ppu_runLine renders one scanline.
 *      - Step both HDMA channels.
 */
void ZeldaDrawPpuFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
  SimpleHdma hdma_chans[2];
  bool render_wide_hud =
      (enhanced_features0 & (kFeatures0_ExtendScreen64 | kFeatures0_RearrangeHud)) ==
          (kFeatures0_ExtendScreen64 | kFeatures0_RearrangeHud) &&
      g_config.extended_aspect_ratio != 0 &&
      Zelda_ShouldRenderWideHudOverlay();

  bool anchor_wide_hud_bg3 = Zelda_ShouldAnchorBg3ToViewport();
  PpuSetRenderWideHud(g_zenv.ppu, render_wide_hud, anchor_wide_hud_bg3, Hud_GetWideHudTilemap(),
                      render_wide_hud ? g_config.hud_shadow_size : 0);
  PpuBeginDrawing(g_zenv.ppu, pixel_buffer, pitch, render_flags);

  dma_startDma(g_zenv.dma, HDMAEN_copy, true);

  SimpleHdma_Init(&hdma_chans[0], &g_zenv.dma->channel[6]);
  SimpleHdma_Init(&hdma_chans[1], &g_zenv.dma->channel[7]);

  // Cheat: Let the PPU impl know about the hdma perspective correction so it can avoid guessing.
  if ((render_flags & kPpuRenderFlags_4x4Mode7) && g_zenv.ppu->mode == 7) {
    if (hdma_chans[0].table == kMapModeHdma0)
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, kMapMode_Zooms1[0], kMapMode_Zooms1[223]);
    else if (hdma_chans[0].table == kMapModeHdma1)
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, kMapMode_Zooms2[0], kMapMode_Zooms2[223]);
    else if (hdma_chans[0].table == kAttractIndirectHdmaTab)
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, hdma_table_dynamic[0], hdma_table_dynamic[223]);
    else
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, 0, 0);
  }

  if (g_zenv.ppu->extraLeftRight != 0 || render_flags & kPpuRenderFlags_Height240)
    ConfigurePpuSideSpace();

  int height = render_flags & kPpuRenderFlags_Height240 ? 240 : 224;

  for (int i = 0; i <= height; i++) {
    if (i == 128 && irq_flag) {
      zelda_ppu_write(BG3HOFS, selectfile_var8);
      zelda_ppu_write(BG3HOFS, selectfile_var8 >> 8);
      zelda_ppu_write(BG3VOFS, 0);
      zelda_ppu_write(BG3VOFS, 0);
      if (irq_flag & 0x80) {
        irq_flag = 0;
        zelda_snes_dummy_write(NMITIMEN, 0x81);
      }
    }
    ppu_runLine(g_zenv.ppu, i);
    SimpleHdma_DoLine(&hdma_chans[0]);
    SimpleHdma_DoLine(&hdma_chans[1]);
  }
}

/*
 * HdmaSetup — Configure DMA channels 6 and 7 for HDMA use.
 *
 * The game wires up HDMA tables by stuffing values into DMA channel
 * registers (DMAP, BBAD, A1Tx, DASx). This helper centralizes the bytes
 * each channel needs: transfer_unit (mode + indirect flags), BBAD = PPU
 * target register, A1T = source CPU address (24-bit), DASx = indirect
 * bank for indirect-mode tables.
 *
 * Channel 6 is optional — when addr6 is zero the caller only wants channel
 * 7 reprogrammed (single-channel HDMA effect). Channel 7 is always set.
 */
void HdmaSetup(uint32 addr6, uint32 addr7, uint8 transfer_unit, uint8 reg6, uint8 reg7, uint8 indirect_bank) {
  Dma *dma = g_zenv.dma;
  if (addr6) {
    dma_write(dma, DMAP6, transfer_unit);
    dma_write(dma, BBAD6, reg6);
    dma_write(dma, A1T6L, addr6);
    dma_write(dma, A1T6H, addr6 >> 8);
    dma_write(dma, A1B6, addr6 >> 16);
    dma_write(dma, DAS60, indirect_bank);
  }
  dma_write(dma, DMAP7, transfer_unit);
  dma_write(dma, BBAD7, reg7);
  dma_write(dma, A1T7L, addr7);
  dma_write(dma, A1T7H, addr7 >> 8);
  dma_write(dma, A1B7, addr7 >> 16);
  dma_write(dma, DAS70, indirect_bank);
}

/*
 * ZeldaInitializationCode — One-shot game boot routine.
 *
 * Models the SNES reset vector + intro setup that the original ROM runs
 * once on power-up. Disables NMI/HDMA/general DMA while the boot is in
 * progress, loads the intro song bank, zeroes WRAM via
 * Startup_InitializeMemory, then primes the animated tile and per-frame
 * DMA source addresses so the NMI handler has somewhere valid to read
 * from. Finally NMITIMEN gets the 0x81 value (NMI enable + auto-joypad).
 *
 * animated_tile_data_src acts as a "first-frame" sentinel checked in
 * ZeldaRunFrameInternal — when it's still zero, the engine knows this
 * is the very first tick and needs to call this initialization routine
 * first.
 */
static void ZeldaInitializationCode() {
  zelda_snes_dummy_write(NMITIMEN, 0);
  zelda_snes_dummy_write(HDMAEN, 0);
  zelda_snes_dummy_write(MDMAEN, 0);

  Sound_LoadIntroSongBank();

  Startup_InitializeMemory();

  animated_tile_data_src = 0xa680;
  dma_source_addr_9 = 0xb280;
  dma_source_addr_14 = 0xb280 + 0x60;
  zelda_snes_dummy_write(NMITIMEN, 0x81);
}

/*
 * ClearOamBuffer — Push every OAM entry off-screen so they don't render.
 *
 * Run every frame before sprites are rebuilt. Setting Y = 0xf0 puts each
 * sprite on scanline 240 — outside the 224-line visible area — so any OAM
 * slot the game code doesn't actively populate this frame contributes
 * nothing to the output. Equivalent to the SNES routine at 80841e.
 */
static void ClearOamBuffer() {  // 80841e
  for (int i = 0; i < 128; i++)
    oam_buf[i].y = 0xf0;
}

/*
 * ZeldaRunGameLoop — One game-logic frame.
 *
 * frame_counter is bumped first (used throughout the game for animation
 * timing, blinking sprites, etc.), then OAM is cleared, the module
 * dispatcher Module_MainRouting runs (which fans out to the active
 * game-state handler — title screen, overworld, dungeon, menu, etc.),
 * and NMI_PrepareSprites builds the OAM bytes for transfer at NMI.
 * nmi_boolean = 0 closes out any "NMI fired during this frame" flag so
 * the next vblank starts clean.
 */
static void ZeldaRunGameLoop() {
  frame_counter++;
  ClearOamBuffer();
  Module_MainRouting();
  NMI_PrepareSprites();
  nmi_boolean = 0;
}

/*
 * ZeldaInitialize — One-time global construction.
 *
 * Called once at program startup (before any frame). Allocates the SNES
 * subsystems (DMA, PPU, SPC player), zeroes a fresh 8KB SRAM, wires up
 * the g_zenv hub, resets DMA/PPU to power-on state, and primes the SPC
 * player so audio is ready to render silence until a song is loaded.
 * The 0x2000 byte SRAM allocation matches the SNES cart's S-RAM size.
 */
void ZeldaInitialize() {
  g_zenv.dma = dma_init(NULL);
  g_zenv.ppu = ppu_init(NULL);
  g_zenv.ram = g_ram;
  g_zenv.sram = (uint8*)calloc(8192, 1);
  g_zenv.vram = g_zenv.ppu->vram;
  g_zenv.player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_zenv.player);
  dma_reset(g_zenv.dma);
  ppu_reset(g_zenv.ppu);
}

/*
 * ZeldaRunPolyLoop — Tick the polyhedral Triforce renderer if it has work.
 *
 * The intro / credits Triforce uses a separate software 3D pipeline
 * (poly.c). Originally this ran on a parallel SPC700 "thread" between
 * NMIs; here we synthesize that by gating on two flags:
 *   - intro_did_run_step  : "the game loop asked for a new poly frame"
 *   - nmi_flag_update_polyhedral : "we already have a poly frame queued
 *     for NMI upload, don't overwrite it"
 * When both conditions allow, render one frame and flip the flags so the
 * NMI handler picks it up and the game loop knows it's been served.
 */
static void ZeldaRunPolyLoop() {
  if (intro_did_run_step && !nmi_flag_update_polyhedral) {
    Poly_RunFrame();
    intro_did_run_step = 0;
    nmi_flag_update_polyhedral = 0xff;
  }
}

/*
 * ZeldaRunFrameInternal — Drive one frame of the C game implementation.
 *
 * This is the "no real CPU emulator" path — the C code runs the game
 * directly. The boot-init check (animated_tile_data_src == 0) means the
 * very first call also fires ZeldaInitializationCode before the game
 * loop runs.
 *
 * run_what is a bitmask from ZeldaRunFrame:
 *   bit 0 (1) — run the game loop this frame
 *   bit 1 (2) — also tick the polyhedral renderer this frame
 * Combined 3 means both. The poly renderer runs FIRST so that any state
 * it produces (e.g. nmi_flag_update_polyhedral) is visible to the game
 * loop and NMI handler that follow.
 *
 * Interrupt_NMI is always called to consume input and drive VRAM/CGRAM
 * upload at the end of the simulated frame.
 */
void ZeldaRunFrameInternal(uint16 input, int run_what) {
  if (animated_tile_data_src == 0)
    ZeldaInitializationCode();

  if (run_what & 2)
    ZeldaRunPolyLoop();
  if (run_what & 1)
    ZeldaRunGameLoop();
  Interrupt_NMI(input);
}

static bool g_dungeon_transition_fast_forward_active;

static bool Zelda_ShouldFastForwardDungeonTransition() {
  if (!(enhanced_features0 & (kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes)) ||
      main_module_index != 7) {
    g_dungeon_transition_fast_forward_active = false;
    return false;
  }

  if (submodule_index == 2) {
    g_dungeon_transition_fast_forward_active = true;
    return true;
  }

  if (submodule_index == 5 && g_dungeon_transition_fast_forward_active)
    return true;

  g_dungeon_transition_fast_forward_active = false;
  return false;
}


/*
 * IncrementCrystalCountdown — Add v to *a and return the byte carry (0/1).
 *
 * Used as the "should we tick the poly renderer this frame?" check —
 * accumulates virq_trigger ticks into a single byte; whenever that byte
 * wraps past 256, return 1 (tick now) and resume accumulating from the
 * remainder. This produces a smooth fractional rate without floating
 * point — matches the rhythm at which the SNES SPC thread would normally
 * complete a poly frame.
 */
static int IncrementCrystalCountdown(uint8 *a, int v) {
  int t = *a + v;
  *a = t;
  return t >> 8;
}

/*
 * frame_ctr_dbg — Free-running frame counter used by debug printf calls.
 *               Distinct from frame_counter (game-visible) so debug
 *               output keeps counting even when the game pauses.
 */
int frame_ctr_dbg;

/* Emulator-parity callback slots. When wired (via ZeldaSetupEmuCallbacks),
 * a reference 65C816 emulator runs alongside this C implementation and
 * the two are diffed each frame. Production builds leave these NULL. */
static uint8 *g_emu_memory_ptr;          // Pointer to the reference emu's WRAM image
static ZeldaRunFrameFunc *g_emu_runframe; // Reference emu's per-frame entry
static ZeldaSyncAllFunc *g_emu_syncall;   // Forces a full state copy back from C to emu

/*
 * ZeldaSetupEmuCallbacks — Install pointers for the parity test harness.
 *
 * Called once by the test harness (zelda_cpu_infra.c) before running.
 * Production builds pass NULL for all three and operate as if the
 * reference emu doesn't exist.
 */
void ZeldaSetupEmuCallbacks(uint8 *emu_ram, ZeldaRunFrameFunc *func, ZeldaSyncAllFunc *sync_all) {
  g_emu_memory_ptr = emu_ram;
  g_emu_runframe = func;
  g_emu_syncall = sync_all;
}

/*
 * EmuSynchronizeWholeState — Force the reference emulator's state to match
 * ours after a load / reset. No-op when the parity harness isn't wired.
 */
static void EmuSynchronizeWholeState() {
  if (g_emu_syncall)
    g_emu_syncall();
}

/*
 * EmuSyncMemoryRegion — Mirror a byte range from our WRAM to the reference
 * emulator's WRAM. No-op without a wired parity harness.
 *
 * Called from any code that mutates WRAM outside the natural game flow
 * (state recorder patches, MSU-1 hook updates, enhanced-features writes)
 * so the two implementations stay in lockstep.
 */
// |ptr| must be a pointer into g_ram, will synchronize the RAM memory with the
// emulator.
static void EmuSyncMemoryRegion(void *ptr, size_t n) {
  uint8 *data = (uint8 *)ptr;
  assert(data >= g_ram && data < g_ram + 0x20000);
  if (g_emu_memory_ptr)
    memcpy(g_emu_memory_ptr + (data - g_ram), data, n);
}

/*
 * Startup_InitializeMemory — Boot-time WRAM/SRAM sanity pass (SNES $80:87c0).
 *
 * Zeros the first 0x2000 bytes of WRAM (the game's primary working area),
 * resets the first palette entry, and clears srm_var1. Then validates the
 * three save-slot magic words at fixed SRAM offsets 0x3e5 / 0x8e5 / 0xde5:
 * if a slot's magic isn't 0x55aa, the slot is treated as empty and its
 * magic word is zeroed so the file-select screen shows it as unused.
 *
 * Finally, INIDISP_copy = 0x80 keeps the screen blanked (force-blank bit)
 * until the first real render is ready, and the cgram-update flag is
 * incremented so NMI will push the freshly-cleared palette to the PPU.
 */
static void Startup_InitializeMemory() {  // 8087c0
  memset(g_ram + 0x0, 0, 0x2000);
  main_palette_buffer[0] = 0;
  srm_var1 = 0;
  uint8 *sram = g_zenv.sram;
  if (WORD(sram[0x3e5]) != 0x55aa)
    WORD(sram[0x3e5]) = 0;
  if (WORD(sram[0x8e5]) != 0x55aa)
    WORD(sram[0x8e5]) = 0;
  if (WORD(sram[0xde5]) != 0x55aa)
    WORD(sram[0xde5]) = 0;
  INIDISP_copy = 0x80;
  flag_update_cgram_in_nmi++;
}

/*
 * ByteArray_AppendVl — Append a variable-length integer to a ByteArray.
 *
 * Format: any number of 0xff bytes followed by the remainder. Equivalent
 * to "write 255 if the value is at least 255, subtract 255, repeat,
 * finally write the leftover (< 255)." This lets the state-recorder log
 * encode small numbers in one byte and arbitrarily large numbers in many
 * bytes without needing a fixed-width field.
 */
void ByteArray_AppendVl(ByteArray *arr, uint32 v) {
  for (; v >= 255; v -= 255)
    ByteArray_AppendByte(arr, 255);
  ByteArray_AppendByte(arr, v);
}

/*
 * saveFunc — SaveLoadFunc that appends bytes to a ByteArray.
 *
 * Plugged into InternalSaveLoad to write the SNES state into a growable
 * buffer (in-memory snapshot for the state recorder). The (void *) ctx
 * indirection makes the save/load core implementation-agnostic — the
 * same InternalSaveLoad can write to a ByteArray, a FILE, or a network
 * sink just by swapping out the function.
 */
void saveFunc(void *ctx_in, void *data, size_t data_size) {
  ByteArray_AppendData((ByteArray *)ctx_in, data, data_size);
}

/*
 * LoadFuncState — Cursor + end pointer for in-memory loading. Used as the
 * ctx for loadFunc when restoring state from a ByteArray.
 */
typedef struct LoadFuncState {
  uint8 *p, *pend;
} LoadFuncState;

/*
 * loadFunc — SaveLoadFunc that reads bytes from a LoadFuncState cursor.
 *
 * Counterpart to saveFunc. assert checks that the cursor hasn't run off
 * the end of the buffer (malformed snapshot would otherwise corrupt
 * memory silently).
 */
void loadFunc(void *ctx, void *data, size_t data_size) {
  LoadFuncState *st = (LoadFuncState *)ctx;
  assert(st->pend - st->p >= data_size);
  memcpy(data, st->p, data_size);
  st->p += data_size;
}

/*
 * InternalSaveLoad — Walk the full SNES hardware state through a transfer
 *                    function (either save or load direction).
 *
 * The same routine handles both saving and loading: `func` is either
 * saveFunc (write to ByteArray) or loadFunc (read from buffer). The
 * order and sizes here exactly match the SNES emulator's snapshot
 * format so existing save files remain compatible. The 27 / 40 / 15 /
 * 58 / 4-byte "junk" regions are placeholders for fields that the
 * reference emulator carries (CPU register dump, internal scratch)
 * but this port doesn't need — they're zeroed on save and ignored on
 * load.
 *
 * Section order: 27B header → 64KB APU RAM → DSP state → SPC junk →
 * 192B DMA → PPU state → 8KB SRAM → 64KB(+) WRAM → 4B trailer junk.
 */
static void InternalSaveLoad(SaveLoadFunc *func, void *ctx) {
  uint8 junk[58] = { 0 };
  func(ctx, junk, 27);
  func(ctx, g_zenv.player->ram, 0x10000);  // apu ram
  func(ctx, junk, 40); // junk
  dsp_saveload(g_zenv.player->dsp, func, ctx); // 3024 bytes of dsp
  func(ctx, junk, 15); // spc junk
  dma_saveload(g_zenv.dma, func, ctx); // 192 bytes of dma state
  ppu_saveload(g_zenv.ppu, func, ctx); // 66619 + 512 + 174
  func(ctx, g_zenv.sram, 0x2000);  // 8192 bytes of sram
  func(ctx, junk, 58); // snes junk
  func(ctx, g_zenv.ram, 0x20000);  // 0x20000 bytes of ram
  func(ctx, junk, 4); // snes junk
}

/*
 * ZeldaReset — Wipe state back to "fresh boot" (optionally keeping SRAM).
 *
 * Used by the file-erase flow and by StateRecorder_Load when no base
 * snapshot is present. Resets DMA + PPU, zeroes all of WRAM, and (unless
 * preserve_sram) zeroes the 8KB SRAM as well. The APU lock pair brackets
 * a music-restore call so audio doesn't glitch from being half-reset
 * while another thread is mixing samples.
 */
void ZeldaReset(bool preserve_sram) {
  frame_ctr_dbg = 0;
  dma_reset(g_zenv.dma);
  ppu_reset(g_zenv.ppu);
  memset(g_zenv.ram, 0, 0x20000);
  if (!preserve_sram)
    memset(g_zenv.sram, 0, 0x2000);
  ZeldaApuLock();
  ZeldaRestoreMusicAfterLoad_Locked(true);
  ZeldaApuUnlock();
  EmuSynchronizeWholeState();

}

/*
 * LoadSnesState — Restore a full SNES snapshot via the given transfer func.
 *
 * The HDMA-table copy at +0x1DBA0 is a port-specific quirk: the dynamic
 * HDMA staging buffer was originally at WRAM offset 0x1b00 but was
 * relocated for performance; save files still use the 0x1b00 layout
 * for compatibility, so on load we copy the loaded bytes into the new
 * spot. EmuSynchronizeWholeState mirrors the loaded state back to the
 * reference emulator if the parity harness is wired.
 */
static void LoadSnesState(SaveLoadFunc *func, void *ctx) {
  // Do the actual loading
  ZeldaApuLock();
  InternalSaveLoad(func, ctx);
  memcpy(g_zenv.ram + 0x1DBA0, g_zenv.ram + 0x1b00, 224 * 2); // hdma table was moved

  ZeldaRestoreMusicAfterLoad_Locked(false);
  ZeldaApuUnlock();
  EmuSynchronizeWholeState();
}

/*
 * SaveSnesState — Reverse of LoadSnesState; emit the SNES snapshot.
 *
 * Pre-pass copies the relocated HDMA staging buffer back to the
 * compatibility offset 0x1b00 so the snapshot matches the original
 * layout. ZeldaSaveMusicStateToRam_Locked dumps the current SPC player
 * state into WRAM so it's captured by the InternalSaveLoad pass.
 */
static void SaveSnesState(SaveLoadFunc *func, void *ctx) {
  memcpy(g_zenv.ram + 0x1b00, g_zenv.ram + 0x1DBA0, 224 * 2); // hdma table was moved
  ZeldaApuLock();
  ZeldaSaveMusicStateToRam_Locked();
  InternalSaveLoad(func, ctx);
  ZeldaApuUnlock();
}

/*
 * StateRecorder — Input event log + base snapshot for replay / rewind.
 *
 * The recorder takes a "base snapshot" of the entire SNES state at some
 * reference point (typically the load slot) and then logs every input
 * change after it. Combined, the two are enough to replay the session
 * frame-perfect, or to wind back to any earlier moment.
 *
 * Live recording state:
 *   last_inputs         Previous frame's input bitmask; used to compute
 *                       which bits changed this frame.
 *   frames_since_last   How many frames since the most recent recorded
 *                       event (used to encode gap durations efficiently).
 *   total_frames        Monotonic frame counter since the base snapshot.
 *
 * Replay state (only meaningful when replay_mode is true):
 *   replay_pos          Cursor into log.data, next command to peek at.
 *   replay_pos_last_complete  Cursor into log.data of the most-recently
 *                       executed command — used by ClearKeyLog and
 *                       StopReplay to safely truncate.
 *   replay_frame_counter      How many frames have been replayed so far.
 *   replay_next_cmd_at  Number of frames between now and the next event.
 *   replay_cmd          The current command byte being decoded.
 *   replay_mode         true if we're in replay; false if recording live.
 *
 * Storage:
 *   log                 Variable-length command stream. Each command is
 *                       1 + N bytes:
 *                         cmd < 0xc0: input bit toggle. High nibble = bit
 *                           index 0..11 in the input word; low nibble =
 *                           frames-since-last (0..14), with 15 meaning
 *                           "extended via AppendVl".
 *                         cmd >= 0xc0: memory patch (0xc0..0xcf).
 *                           Format: 0xc0 | (high_addr_bit << 1) | (numlen << 2),
 *                           then optional Vl extension if numlen == 3,
 *                           then 2 address bytes (high, low), then the
 *                           value bytes.
 *   base_snapshot       Full SNES state at recording start. Empty for
 *                       "fresh boot" replays.
 */
typedef struct StateRecorder {
  uint16 last_inputs;
  uint32 frames_since_last;
  uint32 total_frames;

  // For replay
  uint32 replay_pos, replay_pos_last_complete;
  uint32 replay_frame_counter;
  uint32 replay_next_cmd_at;
  uint8 replay_cmd;
  bool replay_mode;

  ByteArray log;
  ByteArray base_snapshot;
} StateRecorder;

/* Singleton instance — every ZeldaRunFrame call funnels through this
 * recorder, and SaveLoadSlot / PatchCommand mutate it directly. */
static StateRecorder state_recorder;

/*
 * StateRecorder_Init — Wipe a recorder to all-zero state.
 *
 * Trivial zero-out. The ByteArray fields are zero-initialized which
 * leaves them as empty buffers; first AppendByte allocates storage.
 */
void StateRecorder_Init(StateRecorder *sr) {
  memset(sr, 0, sizeof(*sr));
}

/*
 * StateRecorder_RecordCmd — Emit one command byte plus its frame-delta.
 *
 * The frame-delta is packed into the command byte's low bits: 4 bits for
 * input toggles (cmd < 0xc0) so deltas 0..14 fit inline, 1 bit for memory
 * patches (cmd >= 0xc0) so only delta 0 fits inline. Anything bigger spills
 * out as a variable-length tail via ByteArray_AppendVl.
 *
 * frames_since_last is reset to 0 because the delta has been "consumed" —
 * the next event will be measured from this point.
 */
void StateRecorder_RecordCmd(StateRecorder *sr, uint8 cmd) {
  int frames = sr->frames_since_last;
  sr->frames_since_last = 0;
  int x = (cmd < 0xc0) ? 0xf : 0x1;
  ByteArray_AppendByte(&sr->log, cmd | (frames < x ? frames : x));
  if (frames >= x)
    ByteArray_AppendVl(&sr->log, frames - x);
}

/*
 * StateRecorder_Record — Per-frame input recorder.
 *
 * Computes the XOR diff against last_inputs to find which buttons changed.
 * For each set bit (0..11), emits a "toggle bit i" command with the
 * current frame delta. Multiple bits changing on the same frame each get
 * their own entry; the second and beyond have delta 0 (this frame). Then
 * frames_since_last is incremented for the next call.
 *
 * The commented-out printf scaffolding was used during development to
 * debug log-encoding bugs; preserved verbatim.
 */
void StateRecorder_Record(StateRecorder *sr, uint16 inputs) {
  uint16 diff = inputs ^ sr->last_inputs;
  if (diff != 0) {
    sr->last_inputs = inputs;
    //    printf("0x%.4x %d: ", diff, sr->frames_since_last);
    //    size_t lb = sr->log.size;
    for (int i = 0; i < 12; i++) {
      if ((diff >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
    //    while (lb < sr->log.size)
    //      printf("%.2x ", sr->log.data[lb++]);
    //    printf("\n");
  }
  sr->frames_since_last++;
  sr->total_frames++;
}

/*
 * StateRecorder_RecordPatchByte — Log an arbitrary WRAM patch event.
 *
 * Used when game state needs to change between frames in a way that
 * doesn't come from input — typically MSU-1 hook updates, enhanced-features
 * flag changes, or debug cheats via PatchCommand. The patch is encoded as
 * a 0xc0+ command:
 *   bit 1 of cmd : carry of bit 16 of address (lets the encoding cover
 *                  the full 128KB WRAM with only 16-bit address bytes)
 *   bits 2-3     : numlen-1 saturated to 3 (so 1..3 fit inline, 4+ spills
 *                  via AppendVl)
 * After the cmd byte, optional Vl length tail, then 2 address bytes
 * (high, low), then `num` value bytes.
 *
 * The commented-out printf scaffolding was used during development to
 * inspect the encoded bytes; preserved verbatim.
 */
void StateRecorder_RecordPatchByte(StateRecorder *sr, uint32 addr, const uint8 *value, int num) {
  assert(addr < 0x20000);

  //  printf("%d: PatchByte(0x%x, 0x%x. %d): ", sr->frames_since_last, addr, *value, num);
  //  size_t lb = sr->log.size;
  int lq = (num - 1) <= 3 ? (num - 1) : 3;
  StateRecorder_RecordCmd(sr, 0xc0 | (addr & 0x10000 ? 2 : 0) | lq << 2);
  if (lq == 3)
    ByteArray_AppendVl(&sr->log, num - 1 - 3);
  ByteArray_AppendByte(&sr->log, addr >> 8);
  ByteArray_AppendByte(&sr->log, addr);
  for (int i = 0; i < num; i++)
    ByteArray_AppendByte(&sr->log, value[i]);
  //  while (lb < sr->log.size)
  //    printf("%.2x ", sr->log.data[lb++]);
  //  printf("\n");
}

/*
 * ReadFromFile — fread with a Die() on short read so the caller doesn't
 * need to check return values everywhere. Used by StateRecorder_Load.
 */
void ReadFromFile(FILE *f, void *data, size_t n) {
  if (fread(data, 1, n, f) != n)
    Die("fread failed\n");
}

/*
 * StateRecorder_Load — Restore a recorder from a .sav file.
 *
 * Save-file layout (matching StateRecorder_Save):
 *   hdr[0]  Format version (must be 1).
 *   hdr[1]  total_frames at save time.
 *   hdr[2]  log byte length.
 *   hdr[3]  last_inputs.
 *   hdr[4]  frames_since_last.
 *   hdr[5]  bit 0  = base_snapshot present, bits 1+ = replay_pos cursor.
 *   hdr[6]  base_snapshot byte length.
 *   hdr[7]  replay_frame_counter (used if resuming a replay).
 *   Then:   log bytes, base_snapshot bytes, current snapshot bytes.
 *
 * Two paths:
 *   replay_mode = true  : seek to the start of the log, load the base
 *                          snapshot (or reset if absent), and prepare to
 *                          replay from frame 0.
 *   replay_mode = false : restore the recorder's live position from
 *                          hdr[5]>>1 / hdr[7], and load the *current*
 *                          (post-replay) state snapshot rather than the
 *                          base — so saving and re-loading mid-session
 *                          puts us exactly where we left off.
 */
void StateRecorder_Load(StateRecorder *sr, FILE *f, bool replay_mode) {
  // todo: fix robustness on invalid data.
  uint32 hdr[8] = { 0 };
  ReadFromFile(f, hdr, sizeof(hdr));

  assert(hdr[0] == 1);

  sr->total_frames = hdr[1];
  ByteArray_Resize(&sr->log, hdr[2]);
  ReadFromFile(f, sr->log.data, sr->log.size);
  sr->last_inputs = hdr[3];
  sr->frames_since_last = hdr[4];

  ByteArray_Resize(&sr->base_snapshot, (hdr[5] & 1) ? hdr[6] : 0);
  ReadFromFile(f, sr->base_snapshot.data, sr->base_snapshot.size);

  sr->replay_next_cmd_at = 0;

  sr->replay_mode = replay_mode;
  if (replay_mode) {
    sr->frames_since_last = 0;
    sr->last_inputs = 0;
    sr->replay_pos = sr->replay_pos_last_complete = 0;
    sr->replay_frame_counter = 0;
    // Load snapshot from |base_snapshot_|, or reset if empty.

    if (sr->base_snapshot.size) {
      LoadFuncState state = { sr->base_snapshot.data, sr->base_snapshot.data + sr->base_snapshot.size };
      LoadSnesState(&loadFunc, &state);
      assert(state.p == state.pend);
    } else {
      ZeldaReset(false);
    }
  } else {
    // Resume replay from the saved position?
    sr->replay_pos = sr->replay_pos_last_complete = hdr[5] >> 1;
    sr->replay_frame_counter = hdr[7];
    sr->replay_mode = (sr->replay_frame_counter != 0);

    ByteArray arr = { 0 };
    ByteArray_Resize(&arr, hdr[6]);
    ReadFromFile(f, arr.data, arr.size);
    LoadFuncState state = { arr.data, arr.data + arr.size };
    LoadSnesState(&loadFunc, &state);
    ByteArray_Destroy(&arr);
    assert(state.p == state.pend);
  }
}

/*
 * StateRecorder_Save — Write the recorder + current SNES state to a file.
 *
 * Captures the live state into a temporary ByteArray via SaveSnesState,
 * then writes:
 *   - The 8-uint32 header (see StateRecorder_Load for layout)
 *   - The input event log
 *   - The base snapshot (if present)
 *   - The current snapshot (so reload-after-replay-pause works)
 *
 * The assertion that base_snapshot.size matches arr.size (when both
 * exist) is a sanity check — both should be full SNES snapshots and
 * therefore the same length. Mismatches would indicate a format-version
 * drift.
 */
void StateRecorder_Save(StateRecorder *sr, FILE *f) {
  uint32 hdr[8] = { 0 };
  ByteArray arr = { 0 };
  SaveSnesState(&saveFunc, &arr);
  assert(sr->base_snapshot.size == 0 || sr->base_snapshot.size == arr.size);

  hdr[0] = 1;
  hdr[1] = sr->total_frames;
  hdr[2] = (uint32)sr->log.size;
  hdr[3] = sr->last_inputs;
  hdr[4] = sr->frames_since_last;
  hdr[5] = sr->base_snapshot.size ? 1 : 0;
  hdr[6] = (uint32)arr.size;
  // If saving while in replay mode, also need to persist
  // sr->replay_pos_last_complete and sr->replay_frame_counter
  // so the replaying can be resumed.
  if (sr->replay_mode) {
    hdr[5] |= sr->replay_pos_last_complete << 1;
    hdr[7] = sr->replay_frame_counter;
  }
  fwrite(hdr, 1, sizeof(hdr), f);
  fwrite(sr->log.data, 1, hdr[2], f);
  fwrite(sr->base_snapshot.data, 1, sr->base_snapshot.size, f);
  fwrite(arr.data, 1, arr.size, f);

  ByteArray_Destroy(&arr);
}

/*
 * StateRecorder_ClearKeyLog — Take a new base snapshot at the current frame.
 *
 * Discards the recorded log up to "now" by capturing fresh state as the
 * new base snapshot and starting a new log. If any input is currently
 * held, it's re-emitted at timestamp 0 in the new log so the input state
 * matches before/after the clear.
 *
 * Replay-mode special case: the existing tail of the log (commands after
 * the current replay cursor) is preserved by splicing it into the new
 * log, with the timestamp of the next command adjusted to account for
 * the frames already elapsed since the previous one. This lets you
 * keep replaying forward after creating a new base snapshot.
 */
void StateRecorder_ClearKeyLog(StateRecorder *sr) {
  printf("Clearing key log!\n");
  sr->base_snapshot.size = 0;
  SaveSnesState(&saveFunc, &sr->base_snapshot);
  ByteArray old_log = sr->log;
  int old_frames_since_last = sr->frames_since_last;
  memset(&sr->log, 0, sizeof(sr->log));
  // If there are currently any active inputs, record them initially at timestamp 0.
  sr->frames_since_last = 0;
  if (sr->last_inputs) {
    for (int i = 0; i < 12; i++) {
      if ((sr->last_inputs >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
  }
  if (sr->replay_mode) {
    // When clearing the key log while in replay mode, we want to keep
    // replaying but discarding all key history up until this point.
    if (sr->replay_next_cmd_at != 0xffffffff) {
      sr->replay_next_cmd_at -= old_frames_since_last;
      sr->frames_since_last = sr->replay_next_cmd_at;
      sr->replay_pos_last_complete = (uint32)sr->log.size;
      StateRecorder_RecordCmd(sr, sr->replay_cmd);
      int old_replay_pos = sr->replay_pos;
      sr->replay_pos = (uint32)sr->log.size;
      ByteArray_AppendData(&sr->log, old_log.data + old_replay_pos, old_log.size - old_replay_pos);
    }
    sr->total_frames -= sr->replay_frame_counter;
    sr->replay_frame_counter = 0;
  } else {
    sr->total_frames = 0;
  }
  ByteArray_Destroy(&old_log);
  sr->frames_since_last = 0;
}

/*
 * StateRecorder_ReadNextReplayState — Pop pending events and return the
 *                                     current input bitmask for this frame.
 *
 * Called once per frame while replay_mode is true. Drives a small state
 * machine that decodes log entries as time advances:
 *
 *   1. While frames_since_last has reached replay_next_cmd_at, apply the
 *      pending replay_cmd:
 *        - cmd < 0xc0     : toggle bit (cmd >> 4) in last_inputs.
 *        - cmd 0xc0..0xcf : memory patch — decode address, length (with
 *                           AppendVl extension if numlen == 3 / nb == 4),
 *                           and value bytes; write each into g_ram and
 *                           sync to the reference emulator.
 *        - otherwise      : assert(0) (corrupted log).
 *      Then read the next log entry: decode its embedded frame-delta
 *      (4 bits for input toggles, 1 bit for patches), with AppendVl
 *      extension for big gaps.
 *
 *   2. End-of-log handling: replay_next_cmd_at = 0xffffffff so we don't
 *      apply commands after the log ends — the held last_inputs simply
 *      replays forever (silent), and replay mode exits when
 *      replay_frame_counter reaches total_frames.
 *
 *   3. Increment frames_since_last and replay_frame_counter, return the
 *      current input mask so the game loop can consume it as if it were
 *      live input.
 */
uint16 StateRecorder_ReadNextReplayState(StateRecorder *sr) {
  assert(sr->replay_mode);
  while (sr->frames_since_last >= sr->replay_next_cmd_at) {
    int replay_pos = sr->replay_pos;
    if (replay_pos != sr->replay_pos_last_complete) {
      // Apply next command
      sr->frames_since_last = 0;
      if (sr->replay_cmd < 0xc0) {
        sr->last_inputs ^= 1 << (sr->replay_cmd >> 4);
      } else if (sr->replay_cmd < 0xd0) {
        int nb = 1 + ((sr->replay_cmd >> 2) & 3);
        uint8 t;
        if (nb == 4) do {
          nb += t = sr->log.data[replay_pos++];
        } while (t == 255);
        uint32 addr = ((sr->replay_cmd >> 1) & 1) << 16;
        addr |= sr->log.data[replay_pos++] << 8;
        addr |= sr->log.data[replay_pos++];
        do {
          g_ram[addr & 0x1ffff] = sr->log.data[replay_pos++];
          EmuSyncMemoryRegion(&g_ram[addr & 0x1ffff], 1);
        } while (addr++, --nb);
      } else {
        assert(0);
      }
    }
    sr->replay_pos_last_complete = replay_pos;
    if (replay_pos >= sr->log.size) {
      sr->replay_pos = replay_pos;
      sr->replay_next_cmd_at = 0xffffffff;
      break;
    }
    // Read the next one
    uint8 cmd = sr->log.data[replay_pos++], t;
    int mask = (cmd < 0xc0) ? 0xf : 0x1;
    int frames = cmd & mask;
    if (frames == mask) do {
      frames += t = sr->log.data[replay_pos++];
    } while (t == 255);
    sr->replay_next_cmd_at = frames;
    sr->replay_cmd = cmd;
    sr->replay_pos = replay_pos;
  }
  sr->frames_since_last++;
  // Turn off replay mode after we reached the final frame position
  if (++sr->replay_frame_counter >= sr->total_frames) {
    sr->replay_mode = false;
  }
  return sr->last_inputs;
}

/*
 * StateRecorder_StopReplay — Truncate replay at the current point and
 *                            switch back to live recording.
 *
 * Sets total_frames to where we stopped (anything past this is forgotten)
 * and truncates the log at replay_pos_last_complete so the post-replay
 * commands are discarded. The next ZeldaRunFrame call will start
 * recording the player's live inputs from this frame onward.
 */
void StateRecorder_StopReplay(StateRecorder *sr) {
  if (!sr->replay_mode)
    return;
  sr->replay_mode = false;
  sr->total_frames = sr->replay_frame_counter;
  sr->log.size = sr->replay_pos_last_complete;
}

/*
 * InputStateReadFromFile — Debug-only input source from a text scenario file.
 *
 * When _DEBUG is set, the platform layer can substitute this for live
 * input so a repeatable bug-repro scenario can be re-run. Reads lines of
 * the form "TIMESTAMP: KEYS" (e.g. "300: UR" meaning "press Up+Right at
 * frame 300") from boss_bug.txt; the kKeys mapping converts each
 * character into the bit index used by the game's input word.
 *
 * Active until the file's last line, then next_ts becomes 0xffffffff and
 * the held cur_keys is returned forever (effectively "no more inputs").
 */
#ifdef _DEBUG
// This can be used to read inputs from a text file for easier debugging
int InputStateReadFromFile() {
  static FILE *f;
  static uint32 next_ts, next_keys, cur_keys;
  char buf[64];
  char keys[64];

  while (state_recorder.total_frames == next_ts) {
    cur_keys = next_keys;
    if (!f)
      f = fopen("boss_bug.txt", "r");
    if (fgets(buf, sizeof(buf), f)) {
      if (sscanf(buf, "%d: %s", &next_ts, keys) == 1) keys[0] = 0;
      int i = 0;
      for (const char *s = keys; *s; s++) {
        static const char kKeys[] = "AXsSUDLRBY";
        const char *t = strchr(kKeys, *s);
        assert(t);
        i |= 1 << (t - kKeys);
      }
      next_keys = i;
    } else {
      next_ts = 0xffffffff;
    }
  }

  return cur_keys;
}
#endif

/*
 * ZeldaRunFrame — Top-level per-frame entry point.
 *
 * Called by the platform layer once per video frame. Returns true if
 * we're currently replaying a recorded session (so the platform layer
 * can show a "replaying" badge).
 *
 * Steps:
 *   1. Input sanitization. The original game doesn't expect simultaneous
 *      U+D or L+R (e.g. some D-pad hardware can produce both); strip the
 *      conflicting pair via XOR so neither is registered.
 *   2. Replay or record. In replay mode, fetch the next replayed input
 *      from the recorder. In record mode, log the live input, then sync
 *      the music-playing flag (APUI00) and bug-fix / features bits to
 *      WRAM so they ride along in the recorded session.
 *   3. Decide which subsystems run this frame. Older save files used a
 *      strict "alternate game and poly each frame" pattern; newer ones
 *      use the IncrementCrystalCountdown approach which runs the poly
 *      renderer on a configurable fraction of frames (controlled by
 *      virq_trigger) to simulate the SNES SPC700 thread completing
 *      every Nth frame. The bug-fix gate kRam_BugsFixed < kBugFix_PolyRenderer
 *      picks between the two.
 *   4. Run the frame. ZeldaRunFrameInternal does the work, unless a
 *      reference emulator callback is wired AND no enhancement features
 *      are active AND no dialogue translation is enabled — in that case
 *      the emulator runs and the C code observes. Enhancements break
 *      cross-impl byte-parity so they always force the C path.
 *   5. ZeldaPushApuState publishes the latest SPC state to the audio
 *      mixer so audio.c can render samples for this frame.
 */
bool ZeldaRunFrame(int inputs) {

  // Avoid up/down and left/right from being pressed at the same time
  if ((inputs & 0x30) == 0x30) inputs ^= 0x30;
  if ((inputs & 0xc0) == 0xc0) inputs ^= 0xc0;

  frame_ctr_dbg++;

  bool is_replay = state_recorder.replay_mode;

  // Either copy state or apply state
  if (is_replay) {
    inputs = StateRecorder_ReadNextReplayState(&state_recorder);
  } else {
    //    input_state = InputStateReadFromFile();
    StateRecorder_Record(&state_recorder, inputs);

    // This is whether APUI00 is true or false, this is used by the ancilla code.
    uint8 apui00 = ZeldaIsMusicPlaying();
    if (apui00 != g_ram[kRam_APUI00]) {
      g_ram[kRam_APUI00] = apui00;
      EmuSyncMemoryRegion(&g_ram[kRam_APUI00], 1);
      StateRecorder_RecordPatchByte(&state_recorder, 0x648, &apui00, 1);
    }

    if (animated_tile_data_src != 0) {
      // Whenever we're no longer replaying, we'll remember what bugs were fixed,
      // but only if game is initialized.
      if (g_ram[kRam_BugsFixed] < kBugFix_Latest) {
        g_ram[kRam_BugsFixed] = kBugFix_Latest;
        EmuSyncMemoryRegion(&g_ram[kRam_BugsFixed], 1);
        StateRecorder_RecordPatchByte(&state_recorder, kRam_BugsFixed, &g_ram[kRam_BugsFixed], 1);
      }

      if (enhanced_features0 != g_wanted_zelda_features) {
        enhanced_features0 = g_wanted_zelda_features;
        EmuSyncMemoryRegion(&enhanced_features0, sizeof(enhanced_features0));
        StateRecorder_RecordPatchByte(&state_recorder, kRam_Features0, (uint8 *)&enhanced_features0, 4);
      }
    }
  }

  int run_what;
  if (g_ram[kRam_BugsFixed] < kBugFix_PolyRenderer) {
    // A previous version of this code alternated the game loop with
    // the poly renderer.
    run_what = (is_nmi_thread_active && thread_other_stack != 0x1f31) ? 2 : 1;
  } else {
    // The snes seems to let poly rendering run for a little
    // while each fram until it eventually completes a frame.
    // Simulate this by rendering the poly every n:th frame.
    run_what = (is_nmi_thread_active && IncrementCrystalCountdown(&g_ram[kRam_CrystalRotateCounter], virq_trigger)) ? 3 : 1;
    EmuSyncMemoryRegion(&g_ram[kRam_CrystalRotateCounter], 1);
  }

  if (g_emu_runframe == NULL || enhanced_features0 != 0 || g_zenv.dialogue_flags) {
    // can't compare against real impl when running with extra features.
    ZeldaRunFrameInternal(inputs, run_what);
  } else {
    g_emu_runframe(inputs, run_what);
  }

  for (int guard = 0; guard < 128 && Zelda_ShouldFastForwardDungeonTransition(); guard++)
    ZeldaRunFrameInternal(0, 1);

  ZeldaPushApuState();

  return is_replay;
}

/*
 * ZeldaSetLanguage — Select the dialogue + font asset bundle by language ID.
 *
 * The asset pack ships with a kDialogueMap indexed by language code
 * ("en", "de", "fr", "es", ...). Each entry contains three sub-blocks:
 *   [0] : language name string (matched against `language`)
 *   [1] : dialogue text block index (used to look up kDialogue)
 *   [2] : font block index (used to look up kDialogueFont)
 *   [3] : feature flags (stored in g_zenv.dialogue_flags)
 *
 * If `language` is NULL or doesn't match any entry, the kDefaultConf
 * {0,0,0} triple is used (default English dialogue + font, no special
 * flags). A mismatch prints an error to stderr but doesn't abort —
 * the game falls back to defaults so a typo'd locale doesn't lock
 * the user out.
 */
void ZeldaSetLanguage(const char *language) {
  static const uint8 kDefaultConf[3] = { 0, 0, 0 };
  MemBlk found = { kDefaultConf, 3 };
  if (language) {
    size_t n = strlen(language);
    for (int i = 0; ; i++) {
      MemBlk mb = kDialogueMap(i);
      if (mb.ptr == 0) {
        fprintf(stderr, "Unable to find language '%s'\n", language);
        break;
      }
      MemBlk name = FindIndexInMemblk(mb, 0);
      if (name.size == n && !memcmp(name.ptr, language, n)) {
        found = FindIndexInMemblk(mb, 1);
        break;
      }
    }
  }
  g_zenv.dialogue_blk = kDialogue(found.ptr[0]);
  g_zenv.dialogue_font_blk = kDialogueFont(found.ptr[1]);
  g_zenv.dialogue_flags = found.ptr[2];
}


/*
 * kReferenceSaves — Filenames of the bundled "chapter waypoint" save files.
 *
 * Distributed under saves/ref/ so testers can jump directly to any point
 * in the story without having to play through. Indexed via `which - 256`
 * by SaveLoadSlot (the +256 bit signals "reference save" vs "user slot").
 */
static const char *const kReferenceSaves[] = {
  "Chapter 1 - Zelda's Rescue.sav",
  "Chapter 2 - After Eastern Palace.sav",
  "Chapter 3 - After Desert Palace.sav",
  "Chapter 4 - After Tower of Hera.sav",
  "Chapter 5 - After Hyrule Castle Tower.sav",
  "Chapter 6 - After Dark Palace.sav",
  "Chapter 7 - After Swamp Palace.sav",
  "Chapter 8 - After Skull Woods.sav",
  "Chapter 9 - After Gargoyle's Domain.sav",
  "Chapter 10 - After Ice Palace.sav",
  "Chapter 11 - After Misery Mire.sav",
  "Chapter 12 - After Turtle Rock.sav",
  "Chapter 13 - After Ganon's Tower.sav",
};

/*
 * SaveLoadSlot — Save, load, or start replaying a slot file.
 *
 * cmd is one of kSaveLoad_Save / kSaveLoad_Load / kSaveLoad_Replay. The
 * `which` parameter picks the slot: 0..N for user slots ("saves/saveN.sav"),
 * or (256 + chapter_index) for the bundled reference saves in saves/ref/.
 *
 * Save-to-reference is rejected (reference saves are read-only). For
 * non-save commands, the file is opened "rb" and either loaded directly
 * or loaded with replay_mode=true depending on cmd. A missing file is
 * silently ignored (no error path because the user might be probing
 * empty slots from the UI).
 */
void SaveLoadSlot(int cmd, int which) {
  char name[128];
  if (which & 256) {
    if (cmd == kSaveLoad_Save)
      return;
    sprintf(name, "saves/ref/%s", kReferenceSaves[which - 256]);
  } else {
    sprintf(name, "saves/save%d.sav", which);
  }
  FILE *f = fopen(name, cmd != kSaveLoad_Save ? "rb" : "wb");
  if (f) {
    printf("*** %s slot %d\n",
      cmd == kSaveLoad_Save ? "Saving" : cmd == kSaveLoad_Load ? "Loading" : "Replaying", which);

    if (cmd != kSaveLoad_Save)
      StateRecorder_Load(&state_recorder, f, cmd == kSaveLoad_Replay);
    else
      StateRecorder_Save(&state_recorder, f);

    fclose(f);
  }
}

/*
 * StateRecoderMultiPatch — Batched WRAM patch builder.
 *
 * Collects a run of byte writes to consecutive addresses so they record
 * as one combined patch event in the log (much cheaper than N single-byte
 * patches). Holds up to 256 bytes; addr is the run's start address and
 * count is its length so far.
 */
typedef struct StateRecoderMultiPatch {
  uint32 count;
  uint32 addr;
  uint8 vals[256];
} StateRecoderMultiPatch;


/*
 * StateRecoderMultiPatch_Init — Reset the builder to empty state.
 */
void StateRecoderMultiPatch_Init(StateRecoderMultiPatch *mp) {
  mp->count = mp->addr = 0;
}

/*
 * StateRecoderMultiPatch_Commit — Emit the accumulated patch (if any).
 *
 * No-op if count is zero. The actual log write goes through
 * StateRecorder_RecordPatchByte which handles the variable-length encoding.
 */
void StateRecoderMultiPatch_Commit(StateRecoderMultiPatch *mp) {
  if (mp->count)
    StateRecorder_RecordPatchByte(&state_recorder, mp->addr, mp->vals, mp->count);
}

/*
 * StateRecoderMultiPatch_Patch — Append one byte to the patch builder.
 *
 * If the buffer is full (count >= 256) or the new address isn't the next
 * byte after the current run, flush the existing run first and start a
 * new one anchored at this address. Then apply the patch to g_ram
 * immediately AND mirror to the reference emu so the game sees the change
 * this frame.
 */
void StateRecoderMultiPatch_Patch(StateRecoderMultiPatch *mp, uint32 addr, uint8 value) {
  if (mp->count >= 256 || addr != mp->addr + mp->count) {
    StateRecoderMultiPatch_Commit(mp);
    mp->addr = addr;
    mp->count = 0;
  }
  mp->vals[mp->count++] = value;
  g_ram[addr] = value;
  EmuSyncMemoryRegion(&g_ram[addr], 1);
}

/*
 * PatchCommand — Apply a debug "cheat" by single character.
 *
 * Wired to keyboard shortcuts in main.c HandleInput. All patches go
 * through the state recorder so they're captured in the log and replayed
 * correctly if the session is saved.
 *
 *   'w'  : Refill health (0xf372 = 80) and magic (0xf373 = 80).
 *   'W'  : Refill bombs (0xf375 = 10), arrows (0xf376 = 10), and add
 *          100 rupees to the goal counter (which the rupee animation
 *          will roll toward over the next few seconds).
 *   'k'  : Clear the key log (take a new base snapshot here).
 *   'o'  : Set 0xf36f = 1 (something the test team uses; map flag).
 *   'l'  : Stop replay mode and resume live recording.
 *   'E'  : Toggle 0x37f bit 0 (enables/disables an internal flag).
 *
 * The commented-out "b.Patch(0x1FE01, 25);" line is a leftover from an
 * earlier debug patch; preserved verbatim.
 */
void PatchCommand(char c) {
  StateRecoderMultiPatch mp;

  StateRecoderMultiPatch_Init(&mp);
  if (c == 'w') {
    StateRecoderMultiPatch_Patch(&mp, 0xf372, 80);  // health filler
    StateRecoderMultiPatch_Patch(&mp, 0xf373, 80);  // magic filler
    //    b.Patch(0x1FE01, 25);
  } else if (c == 'W') {
    StateRecoderMultiPatch_Patch(&mp, 0xf375, 10);  // link_bomb_filler
    StateRecoderMultiPatch_Patch(&mp, 0xf376, 10);  // link_arrow_filler
    uint16 rupees = link_rupees_goal + 100;
    StateRecoderMultiPatch_Patch(&mp, 0xf360, rupees);  // link_rupees_goal
    StateRecoderMultiPatch_Patch(&mp, 0xf361, rupees >> 8);  // link_rupees_goal
  } else if (c == 'k') {
    StateRecorder_ClearKeyLog(&state_recorder);
  } else if (c == 'o') {
    StateRecoderMultiPatch_Patch(&mp, 0xf36f, 1);
  } else if (c == 'l') {
    StateRecorder_StopReplay(&state_recorder);
  } else if (c == 'E') {
    StateRecoderMultiPatch_Patch(&mp, 0x37f, g_ram[0x37f] ^ 1);
  }
  StateRecoderMultiPatch_Commit(&mp);
}


/*
 * ZeldaReadSram — Load the 8KB battery-backed save file from disk.
 *
 * Called on program startup. Missing file is silently ignored (fresh
 * install / first run). A short read is logged but doesn't abort —
 * the file-select screen will treat partially-loaded slots as empty.
 * After loading, sync to the reference emulator so both have the same
 * initial SRAM.
 */
void ZeldaReadSram() {
  FILE *f = fopen("saves/sram.dat", "rb");
  if (f) {
    if (fread(g_zenv.sram, 1, 8192, f) != 8192)
      fprintf(stderr, "Error reading saves/sram.dat\n");
    fclose(f);
    EmuSynchronizeWholeState();
  }
}

/*
 * ZeldaWriteSram — Persist the 8KB battery-backed save file to disk.
 *
 * Atomic-ish: the existing sram.dat is renamed to sram.bak first so a
 * crash mid-write leaves the previous good save intact as a fallback.
 * On open failure (read-only directory, full disk, etc.), an error is
 * logged but the game continues — the in-memory SRAM survives even if
 * we can't persist it.
 */
void ZeldaWriteSram() {
  rename("saves/sram.dat", "saves/sram.bak");
  FILE *f = fopen("saves/sram.dat", "wb");
  if (f) {
    fwrite(g_zenv.sram, 1, 8192, f);
    fclose(f);
  } else {
    fprintf(stderr, "Unable to write saves/sram.dat\n");
  }
}
