/*
 * nmi.c -- NMI (Non-Maskable Interrupt) / VBlank Handler
 *
 * Part of the Zelda 3 C reimplementation (The Legend of Zelda: A Link to the Past).
 *
 * On the original SNES hardware, the NMI fires once per frame during the vertical blanking
 * interval -- the brief period after the PPU finishes drawing scanline 224 and before it
 * starts the next frame. This is the ONLY safe window to update VRAM, OAM, and CGRAM,
 * because the PPU is not actively reading those memories during vblank.
 *
 * This module reimplements that interrupt handler in C. Each call to Interrupt_NMI()
 * performs the same sequence the original 65C816 code did:
 *   1. Process pending audio commands (music, ambient sound, SFX) via APU ports.
 *   2. DMA sprite tile graphics for Link and other dynamic objects into VRAM.
 *   3. Upload HUD tile indices, palette (CGRAM), and OAM sprite attribute tables.
 *   4. Execute stripe-image transfers for scrolling tilemap updates.
 *   5. Dispatch one of 25 NMI subroutines (indexed by nmi_subroutine_index) to handle
 *      game-state-specific VRAM uploads such as overworld map loads, BG character
 *      updates, text overlays, and animated tile refreshes.
 *   6. Read and filter joypad input for the next game logic frame.
 *   7. Write shadow copies of all PPU registers to the emulated hardware.
 *
 * The 25 NMI subroutines each upload a particular category of graphics data. The game
 * engine sets nmi_subroutine_index before the frame ends, and the NMI handler dispatches
 * to the corresponding function via kNmiSubroutines[].
 *
 * Key conventions:
 *   - g_zenv.vram[] is the emulated 64 KB VRAM (word-addressed, 32K words).
 *   - g_ram[] is the emulated WRAM (128 KB), used as a staging buffer for tile data.
 *   - VRAM addresses in this file are word addresses (each word = 2 bytes).
 *   - "nmi_disable_core_updates" is cleared by subroutines that have finished their
 *     bulk transfer, signaling that the per-frame Link sprite DMA can resume next frame.
 *
 * Related files:
 *   - nmi.h         : Public declarations for the NMI handler functions.
 *   - zelda_rtl.h   : Runtime bridge providing zelda_ppu_write(), zelda_apu_read/write().
 *   - variables.h   : Shadow PPU register copies and game-state variables in WRAM.
 *   - snes/ppu.h    : PPU register constants (INIDISP, BGMODE, MOSAIC, etc.).
 *   - assets.h      : ROM asset pointers (kLinkGraphics, kBgTilemap_*, etc.).
 */

// Game engine and runtime bridge
#include "nmi.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "messaging.h"

// SNES hardware register definitions and PPU interface
#include "snes/snes_regs.h"
#include "snes/ppu.h"

// ROM asset data and audio subsystem
#include "assets.h"
#include "audio.h"
#include "hud.h"

/*
 * VRAM destination address table for NMI_UploadTilemap().
 *
 * Each entry is a high byte of a VRAM word address. The actual VRAM destination is
 * computed as kNmiVramAddrs[nmi_load_target_addr] << 8.  The values correspond to
 * the starting word addresses of the four BG tilemap pages (BG1/BG2 each have two
 * 32x32 nametable pages), plus character data regions:
 *   0x0000, 0x0400, 0x0800, 0x0C00 -- BG tilemap quadrants
 *   0x1000, 0x1400, 0x1800, 0x1C00 -- additional tilemap quadrants
 *   0x6000, 0x6800                  -- BG3 tilemap regions
 * The game indexes into this table based on which portion of the tilemap needs
 * updating during the current frame's NMI.
 */
static const uint8 kNmiVramAddrs[] = {
  0, 0, 4, 8, 12, 8, 12, 0, 4, 0, 8, 4, 12, 4, 12, 0,
  8, 16, 20, 24, 28, 24, 28, 16, 20, 16, 24, 20, 28, 20, 28, 16,
  24, 96, 104,
};
/*
 * NMI dispatch table.
 *
 * NMI_DoUpdates() reads nmi_subroutine_index (set by the previous frame's
 * game logic) and calls kNmiSubroutines[idx] inside the vblank window.
 * Each entry uploads one specific category of graphics to VRAM:
 *
 *   [ 0] doNothing        - placeholder, no upload this frame
 *   [ 1] UploadTilemap    - flush a 0x800 tilemap quadrant to VRAM
 *   [ 2] UploadBG3Text    - HUD/text layer characters
 *   [ 3] UpdateOWScroll   - overworld scroll-edge stripe transfers
 *   [ 4] UpdateSubscreenOverlay - first half of subscreen tile data
 *   [ 5] UpdateBG1Wall    - secret-wall tile reveal column
 *   [ 6] TileMapNothing   - placeholder, no upload this frame
 *   [ 7] UpdateLoadLightWorldMap - light-world map for the pause menu
 *   [ 8] UpdateBG2Left    - 0x1000 bytes into BG2's left half
 *   [ 9] UpdateBGChar3and4   - BG character data slots 3+4
 *   [10] UpdateBGChar5and6   - BG character data slots 5+6
 *   [11] UpdateBGCharHalf - half-block of BG character data at runtime addr
 *   [12] UploadSubscreenOverlayLatter - second half of subscreen overlay
 *   [13] UploadSubscreenOverlayFormer - first half of subscreen overlay
 *   [14-17] UpdateBGChar0..3 - per-slot BG character DMA helpers
 *   [18-20] UpdateObjChar0/2/3 - sprite (OBJ) character DMA helpers
 *   [21] UploadDarkWorldMap - dark-world overworld pause-menu map
 *   [22] UploadGameOverText - "GAME OVER" overlay tiles
 *   [23] UpdatePegTiles  - peg-block animation frame
 *   [24] UpdateStarTiles - star-tile animation frame
 *
 * The table is indexed-by-id so the game engine can request the exact
 * upload it needs without packing logic into the NMI handler itself.
 */
static PlayerHandlerFunc *const kNmiSubroutines[25] = {
  &NMI_UploadTilemap_doNothing,
  &NMI_UploadTilemap,
  &NMI_UploadBG3Text,
  &NMI_UpdateOWScroll,
  &NMI_UpdateSubscreenOverlay,
  &NMI_UpdateBG1Wall,
  &NMI_TileMapNothing,
  &NMI_UpdateLoadLightWorldMap,
  &NMI_UpdateBG2Left,
  &NMI_UpdateBGChar3and4,
  &NMI_UpdateBGChar5and6,
  &NMI_UpdateBGCharHalf,
  &NMI_UploadSubscreenOverlayLatter,
  &NMI_UploadSubscreenOverlayFormer,
  &NMI_UpdateBGChar0,
  &NMI_UpdateBGChar1,
  &NMI_UpdateBGChar2,
  &NMI_UpdateBGChar3,
  &NMI_UpdateObjChar0,
  &NMI_UpdateObjChar2,
  &NMI_UpdateObjChar3,
  &NMI_UploadDarkWorldMap,
  &NMI_UploadGameOverText,
  &NMI_UpdatePegTiles,
  &NMI_UpdateStarTiles,
};
/*
 * NMI_UploadSubscreenOverlayFormer / Latter
 *
 * The subscreen (status/inventory) overlay tilemap is too large to upload
 * in a single vblank, so it is split into two halves and uploaded over
 * two consecutive frames. Each half copies 0x40 entries from a staging
 * region in WRAM (0x12000 / 0x13000) using NMI_HandleArbitraryTileMap,
 * which walks the destination address table at &word_7F4000.
 *
 * "Former" handles indices [0, 0x40) starting at WRAM 0x12000.
 * "Latter" handles indices [0x40, 0x80) starting at WRAM 0x13000.
 */
void NMI_UploadSubscreenOverlayFormer() {
  NMI_HandleArbitraryTileMap(&g_ram[0x12000], 0, 0x40);
}

void NMI_UploadSubscreenOverlayLatter() {
  NMI_HandleArbitraryTileMap(&g_ram[0x13000], 0x40, 0x80);
}

/*
 * CopyToVram: linear VRAM copy. Models DMA mode 0 (auto-increment by 1
 * after every byte) -- the destination word address advances one word
 * per source word. `dstv` is a VRAM word address, not a byte address.
 */
static void CopyToVram(uint32 dstv, const uint8 *src, int len) {
  memcpy(&g_zenv.vram[dstv], src, len);
}

/*
 * CopyToVramVertical: VRAM copy with vertical stride. Models DMA with
 * VMAIN's "increment by 32" mode, used to upload data column-major into
 * a 32-wide tilemap. Each source word lands one tilemap row below the
 * previous one (dst += 32 each iteration).
 *
 * len must be even because the loop walks the buffer in 16-bit words.
 */
static void CopyToVramVertical(uint32 dstv, const uint8 *src, int len) {
  assert(!(len & 1));
  uint16 *dst = &g_zenv.vram[dstv];
  for (int i = 0, i_end = len >> 1; i < i_end; i++, dst += 32, src += 2)
    *dst = WORD(*src);
}

/*
 * CopyToVramLow: write only the low byte of each VRAM word, preserving
 * the existing high byte. Used when the source data is only the tile
 * index (low byte) and the high byte (palette/priority/flip flags) was
 * already configured separately. The mask (& ~0xff) clears the low byte
 * so the OR does not double-write.
 */
static void CopyToVramLow(const uint8 *src, uint32 addr, int num) {
  uint16 *dst = &g_zenv.vram[addr];
  for (int i = 0; i < num; i++)
    dst[i] = (dst[i] & ~0xff) | src[i];
}

/*
 * WritePpuRegisters: flush every shadow PPU-register copy in WRAM out
 * to the emulated PPU at the end of NMI.
 *
 * The original game keeps an in-WRAM "shadow" copy of every register
 * the CPU is allowed to write (the PPU does not let the CPU read most
 * of its registers back). Game logic mutates those shadow copies
 * during the frame; this function commits all of them at vblank.
 *
 * Two-byte registers (BG scroll offsets, mode-7 matrix) require two
 * sequential writes: low byte then high byte, because the PPU latches
 * them through the same one-byte port.
 *
 * Mode 7 (BGMODE & 7 == 7) needs extra setup: M7B and M7C are zeroed
 * (no rotation/shear in the simple game-engine path) and M7X/M7Y get
 * their two-byte writes. The fixed values 0x22 / 7 written to BG12NBA
 * and BG34NBA select the engine's BG character base addresses.
 */
void WritePpuRegisters() {
  zelda_ppu_write(W12SEL, W12SEL_copy);
  zelda_ppu_write(W34SEL, W34SEL_copy);
  zelda_ppu_write(WOBJSEL, WOBJSEL_copy);
  zelda_ppu_write(CGWSEL, CGWSEL_copy);
  zelda_ppu_write(CGADSUB, CGADSUB_copy);
  zelda_ppu_write(COLDATA, COLDATA_copy0);
  zelda_ppu_write(COLDATA, COLDATA_copy1);
  zelda_ppu_write(COLDATA, COLDATA_copy2);
  zelda_ppu_write(TM, TM_copy);
  zelda_ppu_write(TS, TS_copy);
  zelda_ppu_write(TMW, TMW_copy);
  zelda_ppu_write(TSW, TSW_copy);
  zelda_ppu_write(BG1HOFS, BG1HOFS_copy);
  zelda_ppu_write(BG1HOFS, BG1HOFS_copy >> 8);
  zelda_ppu_write(BG1VOFS, BG1VOFS_copy);
  zelda_ppu_write(BG1VOFS, BG1VOFS_copy >> 8);
  zelda_ppu_write(BG2HOFS, BG2HOFS_copy);
  zelda_ppu_write(BG2HOFS, BG2HOFS_copy >> 8);
  zelda_ppu_write(BG2VOFS, BG2VOFS_copy);
  zelda_ppu_write(BG2VOFS, BG2VOFS_copy >> 8);
  zelda_ppu_write(BG3HOFS, BG3HOFS_copy2);
  zelda_ppu_write(BG3HOFS, BG3HOFS_copy2 >> 8);
  zelda_ppu_write(BG3VOFS, BG3VOFS_copy2);
  zelda_ppu_write(BG3VOFS, BG3VOFS_copy2 >> 8);
  zelda_ppu_write(INIDISP, INIDISP_copy);
  zelda_ppu_write(MOSAIC, MOSAIC_copy);
  zelda_ppu_write(BGMODE, BGMODE_copy);
  /* Mode 7 specific setup. M7B/M7C are zeroed (no skew); M7X/M7Y carry
   * the screen-center offset and need two writes each (low/high byte). */
  if ((BGMODE_copy & 7) == 7) {
    zelda_ppu_write(M7B, 0);
    zelda_ppu_write(M7B, 0);
    zelda_ppu_write(M7C, 0);
    zelda_ppu_write(M7C, 0);
    zelda_ppu_write(M7X, M7X_copy);
    zelda_ppu_write(M7X, M7X_copy >> 8);
    zelda_ppu_write(M7Y, M7Y_copy);
    zelda_ppu_write(M7Y, M7Y_copy >> 8);
  }
  /* Fixed BG character-data base-address selectors used everywhere
   * in this engine. 0x22 = BG1 base 0x4000, BG2 base 0x4000;
   * 7 = BG3 base 0x7000. Hardcoded because the game never moves them. */
  zelda_ppu_write(BG12NBA, 0x22);
  zelda_ppu_write(BG34NBA, 7);
}

/*
 * Interrupt_NMI_AudioParts_Locked: hand pending audio commands off to
 * the APU through the four CPU<->APU mailbox ports.
 *
 * Mailbox roles:
 *   APUI00 - music track command (or MSU-1 track via ZeldaPlayMsuAudioTrack)
 *   APUI01 - ambient sound effect ID
 *   APUI02 - one-shot sound effect channel 1
 *   APUI03 - one-shot sound effect channel 2
 *
 * For the music and ambient slots a "no-op" (value 0) is interpreted as
 * "post the last command again only if the APU forgot it" -- an SFX
 * deduplication strategy. For the two SFX channels every frame writes
 * the current value and clears it so the same SFX is not repeated next
 * frame.
 *
 * The "_Locked" suffix is a reminder that the caller is expected to
 * hold the audio mutex (see ZeldaApuLock in main.c) so this routine
 * does not race the SDL audio thread.
 */
static void Interrupt_NMI_AudioParts_Locked() {
  if (music_control == 0) {
//    if (zelda_apu_read(APUI00) == last_music_control)
//      zelda_apu_write(APUI00, 0);
    // Zelda causes unwanted music change when going in a portal. last_music_control doesn't hold the
    // song but the last applied effect
  } else if (!ZeldaIsPlayingMusicTrackWithBug(music_control)) {
    /* New track requested and not already playing it: latch it into
     * last_music_control, kick the MSU player, and remember the track
     * id in music_unk1 (only the "real" music ids; ids >= 0xf2 are
     * sound-effect commands that should not be cached). */
    last_music_control = music_control;
    ZeldaPlayMsuAudioTrack(music_control);
    if (music_control < 0xf2)
      music_unk1 = music_control;
    music_control = 0;
  }

  if (sound_effect_ambient == 0) {
    /* If the APU still has the previous ambient command in its mailbox,
     * clear it so a stale value is not re-triggered. */
    if (zelda_apu_read(APUI01) == sound_effect_ambient_last)
      zelda_apu_write(APUI01, 0);
  } else {
    sound_effect_ambient_last = sound_effect_ambient;
    zelda_apu_write(APUI01, sound_effect_ambient);
    sound_effect_ambient = 0;
  }
  /* SFX channels: always post and always clear so the same SFX is not
   * retriggered next frame. */
  zelda_apu_write(APUI02, sound_effect_1);
  zelda_apu_write(APUI03, sound_effect_2);
  sound_effect_1 = 0;
  sound_effect_2 = 0;

}

/*
 * Interrupt_NMI: top-level vblank handler -- the C equivalent of the
 * original 65C816 NMI vector. Runs once per simulated frame.
 *
 * Sequence:
 *   1. Push pending audio commands to the APU.
 *   2. Reentrancy guard: if nmi_boolean is already set, the previous
 *      NMI is still running (a long subroutine ran past vblank), so
 *      skip the heavyweight VRAM updates and joypad read this frame
 *      to stay in sync. Otherwise set the guard, run the bulk update
 *      pass, and read the joypad for next frame's input state.
 *   3. If the polyhedral/Triforce intro thread is active, run the
 *      smaller IRQ-time VRAM update and ping-pong its scratch stack
 *      pointer between two scratch areas.
 *   4. Flush all shadow PPU registers to hardware.
 *
 * Parameters:
 *   joypad_input - raw joypad-1 bitfield captured this frame.
 */
void Interrupt_NMI(uint16 joypad_input) {  // 8080c9

  Interrupt_NMI_AudioParts_Locked();

  if (!nmi_boolean) {
    nmi_boolean = true;
    NMI_DoUpdates();
    NMI_ReadJoypads(joypad_input);
  }

  if (is_nmi_thread_active) {
    NMI_UpdateIRQGFX();
    /* Toggle between the two scratch-stack offsets so the polyhedral
     * thread alternates back-buffers each frame. */
    thread_other_stack = (thread_other_stack != 0x1f31) ? 0x1f31 : 0x1f2;
  }
  WritePpuRegisters();
}

/*
 * NMI_ReadJoypads: capture this frame's joypad-1 bits and produce the
 * "filtered" (newly-pressed-only) view used by gameplay code.
 *
 * The SNES auto-joypad-read hardware delivers buttons in a specific
 * bit order; the bit-reverse loop turns the host-order word into the
 * bit layout the original ROM expected so all the rest of the engine
 * can read the same bit positions it always did.
 *
 * Filtering: filtered_joypad_{L,H} is set for buttons that are pressed
 * this frame but were NOT pressed last frame ("rising edge"). The
 * (last2 XOR current) AND current trick computes "bits that changed,
 * AND are currently set" -- i.e. fresh presses, not held buttons.
 *
 * Two halves: L = low byte (B/Y/Select/Start + d-pad), H = high byte
 * (A/X/L/R + the four extra-pad bits).
 */
void NMI_ReadJoypads(uint16 joypad_input) {  // 8083d1
  uint16 both = joypad_input;
  uint16 reversed = 0;
  /* Bit-reverse a 16-bit word: build `reversed` MSB-first by shifting
   * left and tacking on the LSB of `both` each iteration. */
  for (int i = 0; i < 16; i++, both >>= 1)
    reversed = reversed * 2 + (both & 1);
  uint8 r0 = reversed;
  uint8 r1 = reversed >> 8;

  /* Low byte: store raw, derive newly-pressed bits, advance history. */
  joypad1L_last = r0;
  filtered_joypad_L = (r0 ^ joypad1L_last2) & r0;
  joypad1L_last2 = r0;

  /* High byte: same pattern. */
  joypad1H_last = r1;
  filtered_joypad_H = (r1 ^ joypad1H_last2) & r1;
  joypad1H_last2 = r1;
}

static bool NMI_ShouldBlankLegacyHudForWideOverlay() {
  if (main_module_index == 7 || main_module_index == 8 || main_module_index == 9 ||
      main_module_index == 11 || main_module_index == 15 ||
      main_module_index == 17 || main_module_index == 18)
    return true;

  if (main_module_index != 14)
    return false;

  /* Dialogue uploads refresh BG3 glyph pixels, so the old HUD tilemap must be blanked
   * while gameplay text is active or stale top-strip entries turn into readable garbage. */
  if (submodule_index == 2)
    return saved_module_for_menu == 7 || saved_module_for_menu == 8 ||
           saved_module_for_menu == 9 || saved_module_for_menu == 11 ||
           saved_module_for_menu == 15 ||
           saved_module_for_menu == 17;

  if (submodule_index == 11)
    return saved_module_for_menu == 7 || saved_module_for_menu == 8 ||
           saved_module_for_menu == 9 || saved_module_for_menu == 11 ||
           saved_module_for_menu == 15 ||
           saved_module_for_menu == 17;

  if (submodule_index == 1)
    return overworld_map_state == 0 || overworld_map_state == 1 ||
           overworld_map_state == 2 || overworld_map_state == 6;

  if (submodule_index == 12)
    return overworld_map_state == 0 || overworld_map_state == 1 ||
           overworld_map_state == 2 || overworld_map_state == 4 ||
           Hud_NewSettingsMenuWantsHudPreview();

  return false;
}

/* Returns true when the custom widescreen HUD owns the status display and the
 * original 32-tile-wide BG3 HUD strip should be kept transparent. */
static bool NMI_ShouldClearLegacyHudTextStrip() {
  return (enhanced_features0 & (kFeatures0_ExtendScreen64 | kFeatures0_RearrangeHud)) ==
      (kFeatures0_ExtendScreen64 | kFeatures0_RearrangeHud) &&
      NMI_ShouldBlankLegacyHudForWideOverlay();
}

/* Clears only BG3 tilemap words for the legacy status strip. This deliberately
 * leaves BG3 character data untouched so custom menu glyph uploads remain intact. */
static void NMI_ClearLegacyHudTextStrip() {
  enum { kLegacyHudTextStripWords = 32 * 6 };
  uint16 *dst = &g_zenv.vram[word_7E0219];
  for (int i = 0; i < kLegacyHudTextStripWords; i++)
    dst[i] = 0x207f;
}

/*
 * NMI_DoUpdates: the heavy lifting that happens inside one vblank.
 * Runs in this order:
 *   1. (If core updates are not suppressed) DMA Link's animated tile
 *      strips from kLinkGraphics into VRAM, plus the dynamic object
 *      tile strips from WRAM staging buffers (sword swing animations,
 *      ancilla effects, dropped items, etc.). Each memcpy targets a
 *      specific 0x20-0x40 byte region inside VRAM page 0x4000.
 *   2. (If flagged) flush the HUD tile-index buffer and palette buffer
 *      to VRAM/CGRAM. The flags are one-shot and cleared after the
 *      copy so they do not re-fire next frame.
 *   3. Always copy the OAM mirror from g_ram[0x800..0x9FF] into the
 *      PPU's OAM. 0x220 = 512 bytes of OAM low + 32 bytes of OAM high
 *      (priority/X-bit data).
 *   4. (If flagged) handle a "stripe" upload from one of nine sources
 *      indexed by nmi_load_bg_from_vram (BG tilemap blocks, peg/star
 *      tile rebuilds, etc.) and reset the flag.
 *   5. (If flagged) plain memcpy of a 0x200-byte block from WRAM
 *      0x10000 into a tilemap quadrant.
 *   6. (If flagged) walk the variable-length packet list at
 *      uvram.data: each packet is (dst_word, vmain, len, payload...),
 *      terminated by 0xFFFF in the dst slot. vmain == 0x80 is a flat
 *      memcpy; vmain == 0x81 uses a 32-word vertical stride.
 *   7. Finally, dispatch the single per-frame NMI subroutine via
 *      kNmiSubroutines[idx] and reset the index to 0.
 */
void NMI_DoUpdates() {  // 8089e0
  if (!nmi_disable_core_updates) {
    /* Link sprite / dynamic object tile strips. The dma_source_addr_*
     * variables point at the current animation frames staged either in
     * the kLinkGraphics ROM block (subtract 0x8000 to land on the LoROM
     * offset baked into the original engine) or in WRAM. Each memcpy
     * lands in a fixed VRAM destination so the PPU finds Link in the
     * same character slot every frame regardless of pose. */
    memcpy(&g_zenv.vram[0x4100], &kLinkGraphics[dma_source_addr_0 - 0x8000], 0x40);
    memcpy(&g_zenv.vram[0x4120], &kLinkGraphics[dma_source_addr_1 - 0x8000], 0x40);
    memcpy(&g_zenv.vram[0x4140], &kLinkGraphics[dma_source_addr_2 - 0x8000], 0x20);

    memcpy(&g_zenv.vram[0x4000], &kLinkGraphics[dma_source_addr_3 - 0x8000], 0x40);
    memcpy(&g_zenv.vram[0x4020], &kLinkGraphics[dma_source_addr_4 - 0x8000], 0x40);
    memcpy(&g_zenv.vram[0x4040], &kLinkGraphics[dma_source_addr_5 - 0x8000], 0x20);

    memcpy(&g_zenv.vram[0x4050], &g_ram[dma_source_addr_6], 0x40);
    memcpy(&g_zenv.vram[0x4070], &g_ram[dma_source_addr_7], 0x40);
    memcpy(&g_zenv.vram[0x4090], &g_ram[dma_source_addr_8], 0x40);
    memcpy(&g_zenv.vram[0x40b0], &g_ram[dma_source_addr_9], 0x20);
    memcpy(&g_zenv.vram[0x40c0], &g_ram[dma_source_addr_10], 0x40);
    memcpy(&g_zenv.vram[0x4150], &g_ram[dma_source_addr_11], 0x40);
    memcpy(&g_zenv.vram[0x4170], &g_ram[dma_source_addr_12], 0x40);
    memcpy(&g_zenv.vram[0x4190], &g_ram[dma_source_addr_13], 0x40);
    memcpy(&g_zenv.vram[0x41b0], &g_ram[dma_source_addr_14], 0x20);
    memcpy(&g_zenv.vram[0x41c0], &g_ram[dma_source_addr_15], 0x40);
    memcpy(&g_zenv.vram[0x4200], &g_ram[dma_source_addr_16], 0x40);
    memcpy(&g_zenv.vram[0x4220], &g_ram[dma_source_addr_17], 0x40);
    memcpy(&g_zenv.vram[0x4240], &g_ram[0xbd40], 0x40);
    memcpy(&g_zenv.vram[0x4300], &g_ram[dma_source_addr_18], 0x40);
    memcpy(&g_zenv.vram[0x4320], &g_ram[dma_source_addr_19], 0x40);
    memcpy(&g_zenv.vram[0x4340], &g_ram[0xbd80], 0x40);

    /* The bird/duck travel cutscene needs two extra strips that are
     * only DMA'd while the cutscene is actually running. */
    if (BYTE(flag_travel_bird)) {
      memcpy(&g_zenv.vram[0x40e0], &g_ram[dma_source_addr_20], 0x40);
      memcpy(&g_zenv.vram[0x41e0], &g_ram[dma_source_addr_21], 0x40);
    }

    /* Animated environment tiles (waves, torches, fountains) at the
     * destination address chosen by the active tile theme. */
    memcpy(&g_zenv.vram[animated_tile_vram_addr], &g_ram[animated_tile_data_src], 0x400);
  }

  /* HUD tile indices (heart/magic icons, item counts) are staged
   * during gameplay and flushed only on frames where the HUD changed. */
  if (flag_update_hud_in_nmi) {
    if (NMI_ShouldClearLegacyHudTextStrip())
      NMI_ClearLegacyHudTextStrip();
    else
      memcpy(&g_zenv.vram[word_7E0219], hud_tile_indices_buffer, 165 * sizeof(uint16));
  }

  /* Full 256-color palette flush (32 colors x 8 BG palettes + sprite
   * palettes = 0x200 bytes). */
  if (flag_update_cgram_in_nmi) {
    memcpy(g_zenv.ppu->cgram, main_palette_buffer, 0x200);
  }

  /* One-shot flags: clear so the same buffer is not re-uploaded next
   * frame on top of a fresh value being staged into it. */
  flag_update_hud_in_nmi = 0;
  flag_update_cgram_in_nmi = 0;

  /* OAM mirror flush. WRAM 0x800..0x9FF holds the engine's OAM
   * shadow; 0x220 bytes covers all 128 sprite entries (low 4 bytes
   * each = 0x200) plus the 32 high bytes (X-bit + size). */
  memcpy(g_zenv.ppu->oam, &g_ram[0x800], 0x220);

  /* Stripe upload: a flag-driven dispatch that picks one of nine
   * source buffers (WRAM staging areas or compiled-in BG tilemap
   * blobs) and feeds it through HandleStripes14 (the "stripe-image"
   * decoder). Mode 1 additionally resets the running upload offset. */
  if (nmi_load_bg_from_vram) {
    const uint8 *p;
    switch (nmi_load_bg_from_vram) {
    case 1: p = g_ram + 0x1002; break;
    case 2: p = g_ram + 0x1000; break;
    case 3: p = kBgTilemap_0; break;
    case 4: p = g_ram + 0x21b; break;
    case 5: p = kBgTilemap_1; break;
    case 6: p = kBgTilemap_2; break;
    case 7: p = kBgTilemap_3; break;
    case 8: p = kBgTilemap_4; break;
    case 9: p = kBgTilemap_5; break;
    default: assert(0);
    }
    HandleStripes14(p);
    if (nmi_load_bg_from_vram == 1)
      vram_upload_offset = 0;
    nmi_load_bg_from_vram = 0;
  }

  /* Simple "load next room half" tilemap copy: 0x200 bytes from
   * WRAM 0x10000 + src offset to a dst chosen by nmi_update_tilemap_dst
   * scaled by 256 (one tilemap row). */
  if (nmi_update_tilemap_dst) {
    memcpy(&g_zenv.vram[nmi_update_tilemap_dst * 256], &g_ram[0x10000 + nmi_update_tilemap_src], 0x200);
    nmi_update_tilemap_dst = 0;
  }

  /* Variable-length packet stream copy. Each packet is 4 bytes of
   * header (dst_word, vmain, len) followed by len bytes of payload.
   * The list terminates when the header's destination word is 0xFFFF.
   * vmain mirrors the SNES VMAIN register: 0x80 = +1 word increment
   * (linear), 0x81 = +32 word increment (vertical/column writes). */
  if (nmi_copy_packets_flag) {
    uint8 *p = (uint8 *)uvram.data;
    do {
      int dst = WORD(p[0]);
      int vmain = p[2];
      int len = p[3];
      p += 4;
      if (vmain == 0x80) {
        // plain copy
        memcpy(&g_zenv.vram[dst], p, len);
      } else if (vmain == 0x81) {
        // copy with other increment
        assert((len & 1) == 0);
        uint16 *dp = &g_zenv.vram[dst];
        for (int i = 0; i < len; i += 2, dp += 32)
          *dp = WORD(p[i]);
      } else {
        /* Any other VMAIN value is a programmer error - the engine
         * never emits packets in other increment modes. */
        assert(0);
      }
      p += len;
    } while (WORD(p[0]) != 0xffff);
    nmi_copy_packets_flag = 0;
    nmi_disable_core_updates = 0;
  }

  /* Per-frame dispatch into the 25-entry subroutine table. The index
   * is reset before the call so a subroutine that wants to schedule
   * itself for next frame can simply re-set it. */
  int idx = nmi_subroutine_index;
  nmi_subroutine_index = 0;
  kNmiSubroutines[idx]();
}

/*
 * NMI_UploadTilemap: dispatch-table entry [1].
 * Flushes the 0x800-byte tilemap quadrant staged at WRAM 0x1000 to the
 * VRAM destination chosen by kNmiVramAddrs[nmi_load_target_addr] << 8
 * (table is byte high-word; shifting by 8 turns it into a word
 * address). Zeros the first staging word so the next frame can detect
 * "buffer drained" and re-clears the core-updates suppression flag so
 * Link's tile DMA can resume on the next frame.
 */
void NMI_UploadTilemap() {  // 808cb0
  memcpy(&g_zenv.vram[kNmiVramAddrs[BYTE(nmi_load_target_addr)] << 8], &g_ram[0x1000], 0x800);

  *(uint16 *)&g_ram[0x1000] = 0;
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UploadTilemap_doNothing: dispatch-table entry [0].
 * Default "no upload this frame" placeholder. Exists so the dispatch
 * table can use index 0 instead of a NULL check.
 */
void NMI_UploadTilemap_doNothing() {  // 808ce3
}

/*
 * NMI_UploadBG3Text: dispatch-table entry [2].
 * Copies 0x7E0 bytes from WRAM 0x10000 (the text/HUD staging buffer)
 * into VRAM at 0x7C00 -- the BG3 character/tilemap region used for
 * status text and dialog boxes.
 */
void NMI_UploadBG3Text() {  // 808ce4
  memcpy(&g_zenv.vram[0x7c00], &g_ram[0x10000], 0x7e0);
  if (NMI_ShouldClearLegacyHudTextStrip())
    NMI_ClearLegacyHudTextStrip();
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UpdateOWScroll: dispatch-table entry [3].
 * Drains a list of "edge stripe" packets generated when the overworld
 * camera scrolls into a new screen. Each packet is two bytes of
 * destination word address followed by `len` bytes of tile data; the
 * list terminates when the last byte of a header has its high bit set.
 *
 * The first 16-bit field at uvram.data is a packed "header":
 *   bit 15  - increment mode (1 = vertical/+32, 0 = horizontal/+1)
 *   bits 0-13 - byte length per packet
 */
void NMI_UpdateOWScroll() {  // 808d13
  uint8 *src = (uint8 *)uvram.data;
  int f = WORD(src[0]);
  int step = (f & 0x8000) ? 32 : 1;
  int len = f & 0x3fff;
  src += 2;
  do {
    uint16 *dst = &g_zenv.vram[WORD(src[0])];
    src += 2;
    for (int i = 0, i_end = len >> 1; i < i_end; i++, dst += step, src += 2)
      *dst = WORD(*src);
  } while (!(src[1] & 0x80));
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UpdateSubscreenOverlay: dispatch-table entry [4].
 * Uploads the entire 0x80-entry subscreen overlay tilemap from WRAM
 * 0x12000 in one frame (used during the brief moments where there is
 * enough vblank time to do it without splitting). Compare with the
 * Former/Latter pair that splits the same upload over two frames.
 */
void NMI_UpdateSubscreenOverlay() {  // 808d62
  NMI_HandleArbitraryTileMap(&g_ram[0x12000], 0, 0x80);
}

/*
 * NMI_HandleArbitraryTileMap: shared inner loop for the subscreen
 * overlay uploads. Walks indices [i, i_end) (in steps of 2 because
 * each entry is a 16-bit address), looks up the destination VRAM
 * word address in the table at &word_7F4000, and copies a 0x80-byte
 * chunk of `src` to each destination.
 *
 * Parameters:
 *   src   - source pointer in WRAM staging area.
 *   i     - first table index to upload (byte offset).
 *   i_end - one-past-last table index.
 */
void NMI_HandleArbitraryTileMap(const uint8 *src, int i, int i_end) {  // 808dae
  uint16 *r10 = &word_7F4000;
  do {
    memcpy(&g_zenv.vram[r10[i >> 1]], src, 0x80);
    src += 0x80;
  } while ((i += 2) != i_end);
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UpdateBG1Wall: dispatch-table entry [5].
 * Reveals a 16-row tall column of secret-wall tiles by writing the
 * column vertically (step = 32) at the runtime address in
 * nmi_load_target_addr and at +0x800 into the BG2 page. The two
 * 0x40-byte source halves come from a precomputed staging area.
 */
void NMI_UpdateBG1Wall() {  // 808e09
  // Secret Wall Right
  CopyToVramVertical(nmi_load_target_addr, &g_ram[0xc880], 0x40);
  CopyToVramVertical(nmi_load_target_addr + 0x800, &g_ram[0xc8c0], 0x40);
}

/*
 * NMI_TileMapNothing: dispatch-table entry [6]. Placeholder no-op
 * paired with NMI_UpdateBG1Wall so a wall reveal can be cancelled
 * by switching the index without having to clear scheduling state.
 */
void NMI_TileMapNothing() {  // 808e4b
}

/*
 * NMI_UpdateLoadLightWorldMap: dispatch-table entry [7].
 * Builds the light-world overworld pause-menu map. The destination
 * table picks four 0x20-byte horizontal stripes (top-left BG, top-
 * right BG, bottom-left BG, bottom-right BG). For each stripe, 32
 * rows of 32 tile-index bytes are written using CopyToVramLow so the
 * existing palette/priority bytes stay intact. The +0x80 step
 * advances one tilemap row per VRAM stripe write.
 */
void NMI_UpdateLoadLightWorldMap() {  // 808e54
  static const uint16 kLightWorldTileMapDsts[4] = { 0, 0x20, 0x1000, 0x1020 };
  const uint8 *src = GetLightOverworldTilemap();
  for (int j = 0; j != 4; j++) {
    int t = kLightWorldTileMapDsts[j];
    for (int i = 0x20; i; i--) {
      CopyToVramLow(src, t, 0x20);
      src += 32;
      t += 0x80;
    }
  }
}

/*
 * NMI_UpdateBG2Left: dispatch-table entry [8].
 * Refreshes the entire left half of BG2 (two 0x800-byte tilemap
 * pages) from the staging area at WRAM 0x10000.
 */
void NMI_UpdateBG2Left() {  // 808ea9
  CopyToVram(0, &g_ram[0x10000], 0x800);
  CopyToVram(0x800, &g_ram[0x10800], 0x800);
}

/*
 * NMI_UpdateBGChar3and4: dispatch-table entry [9].
 * Bulk upload (0x1000 bytes = two BG character slots) from WRAM
 * 0x10000 to VRAM 0x2C00, covering BG character data slots 3 and 4
 * during room transitions.
 */
void NMI_UpdateBGChar3and4() {  // 808ee7
  memcpy(&g_zenv.vram[0x2c00], &g_ram[0x10000], 0x1000);
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UpdateBGChar5and6: dispatch-table entry [10].
 * Same pattern as 3and4 but for slots 5 and 6: 0x1000 bytes from
 * WRAM 0x11000 to VRAM 0x3400.
 */
void NMI_UpdateBGChar5and6() {  // 808f16
  memcpy(&g_zenv.vram[0x3400], &g_ram[0x11000], 0x1000);
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UpdateBGCharHalf: dispatch-table entry [11].
 * Half-block (0x400 bytes) BG character upload to a runtime VRAM
 * destination word address (high byte from nmi_load_target_addr,
 * scaled by 256). Used for partial slot refreshes.
 */
void NMI_UpdateBGCharHalf() {  // 808f45
  memcpy(&g_zenv.vram[BYTE(nmi_load_target_addr) * 256], &g_ram[0x11000], 0x400);
}

/*
 * NMI_UpdateBGChar0..3: dispatch-table entries [14..17].
 * Full-block BG character DMAs to VRAM 0x2000, 0x2800, 0x3000, 0x3800
 * respectively. All four delegate to NMI_RunTileMapUpdateDMA so the
 * shared end-of-DMA bookkeeping (nmi_disable_core_updates clear) is
 * not duplicated.
 */
void NMI_UpdateBGChar0() {  // 808f72
  NMI_RunTileMapUpdateDMA(0x2000);
}

void NMI_UpdateBGChar1() {  // 808f79
  NMI_RunTileMapUpdateDMA(0x2800);
}

void NMI_UpdateBGChar2() {  // 808f80
  NMI_RunTileMapUpdateDMA(0x3000);
}

void NMI_UpdateBGChar3() {  // 808f87
  NMI_RunTileMapUpdateDMA(0x3800);
}

/*
 * NMI_UpdateObjChar0: dispatch-table entry [18].
 * Sprite character DMA: 0x800 bytes (one half-slot) from WRAM 0x10000
 * to VRAM 0x4400, used to load fresh sprite tile graphics into the
 * sprite character region.
 */
void NMI_UpdateObjChar0() {  // 808f8e
  CopyToVram(0x4400, &g_ram[0x10000], 0x800);
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UpdateObjChar2 / NMI_UpdateObjChar3: dispatch-table entries
 * [19] and [20]. Full-slot (0x1000 byte) sprite character DMAs to
 * VRAM 0x5000 and 0x5800.
 */
void NMI_UpdateObjChar2() {  // 808fbd
  NMI_RunTileMapUpdateDMA(0x5000);
}

void NMI_UpdateObjChar3() {  // 808fc4
  NMI_RunTileMapUpdateDMA(0x5800);
}

/*
 * NMI_RunTileMapUpdateDMA: shared body for the BG/OBJ character DMAs
 * above. Copies a fixed 0x1000-byte (one full character slot) block
 * from WRAM 0x10000 to the VRAM destination word `dst`, then clears
 * the core-updates suppression flag.
 */
void NMI_RunTileMapUpdateDMA(int dst) {  // 808fc9
  CopyToVram(dst, &g_ram[0x10000], 0x1000);
  nmi_disable_core_updates = 0;
}

/*
 * NMI_UploadDarkWorldMap: dispatch-table entry [21].
 * Builds the dark-world overworld pause-menu map. Same loop shape as
 * the light-world version but only one quadrant (32 rows of 32 tile
 * indices into the lower half of the BG tilemap, starting at VRAM
 * word 0x810). The tilemap data lives at WRAM 0x1000 because the
 * dark-world map shares a staging buffer with the dungeon map.
 */
void NMI_UploadDarkWorldMap() {  // 808ff3
  const uint8 *src = g_ram + 0x1000;
  int t = 0x810;
  for (int i = 0x20; i; i--) {
    CopyToVramLow(src, t, 0x20);
    src += 32;
    t += 0x80;
  }
}

/*
 * NMI_UploadGameOverText: dispatch-table entry [22].
 * Two-region upload for the "GAME OVER" overlay: the main glyphs
 * (0x800 bytes) at VRAM 0x7800 and the trailing decorative tiles
 * (0x600 bytes) at VRAM 0x7D00. Source buffers were prepared in
 * advance by the messaging subsystem.
 */
void NMI_UploadGameOverText() {  // 809038
  CopyToVram(0x7800, &g_ram[0x2000], 0x800);
  CopyToVram(0x7d00, &g_ram[0x3400], 0x600);
}

/*
 * NMI_UpdatePegTiles: dispatch-table entry [23].
 * Refreshes the four hammer-peg block tiles (0x100 bytes covering one
 * 8x8 tile per peg-state frame) at VRAM 0x3D00 to advance the
 * pop-up/down animation.
 */
void NMI_UpdatePegTiles() {  // 80908b
  CopyToVram(0x3d00, &g_ram[0x10000], 0x100);
}

/*
 * NMI_UpdateStarTiles: dispatch-table entry [24].
 * Tiny 0x40-byte refresh for the rotating-star floor-switch tiles
 * (one frame of animation) at VRAM 0x3ED0.
 */
void NMI_UpdateStarTiles() {  // 8090b7
  CopyToVram(0x3ed0, &g_ram[0x10000], 0x40);
}

/*
 * HandleStripes14: decode and apply a "stripe-image" packet stream.
 *
 * Stripe images are the original SNES game's compressed format for
 * room/tilemap updates. Each packet is a 4-byte header followed by
 * either inline tile data or a single repeat value:
 *
 *   header[0..1] = big-endian VRAM word destination
 *                  (swap16 because the original ROM stored them big-
 *                  endian for direct DMA register loading).
 *   header[2..3] = big-endian "control word":
 *     bit 15  - VRAM increment mode: 0 = horizontal (+1 word),
 *                                    1 = vertical   (+32 words).
 *     bit 14  - "memset" flag: 0 = inline payload, 1 = repeat the
 *                              next 16-bit word `len` times.
 *     bits 0-13 - length-1 (so a value of 0 means 1 word/byte).
 *
 * The stream terminates when the high bit of the destination's high
 * byte (p[0] & 0x80) becomes set -- this is how the original engine
 * marked the sentinel packet.
 *
 * Four cases (incr x memset):
 *   horizontal + copy   - plain memcpy of `len` bytes.
 *   horizontal + memset - fill `(len+1)/2` words with the repeat value.
 *   vertical   + copy   - copy `len/2` words with stride 32.
 *   vertical   + memset - fill `(len+1)/2` words with stride 32.
 *
 * All four cases share the same loop shell so the decoder fits in one
 * compact function.
 */
void HandleStripes14(const uint8 *p) {  // 8092a1
  while (!(p[0] & 0x80)) {
    uint16 vmem_addr = swap16(WORD(p[0]));
    uint8 vram_incr_amount = (p[2] & 0x80) >> 7;
    uint8 is_memset = p[2] & 0x40;  // Cpu BUS Address Step  (0=Increment, 2=Decrement, 1/3=Fixed) (DMA only)
    int len = (swap16(WORD(p[2])) & 0x3fff) + 1;
    p += 4;

    if (vram_incr_amount == 0) {
      uint16 *dst = &g_zenv.vram[vmem_addr];
      if (is_memset) {
        /* Horizontal fill: build the repeat word, round len up to a
         * whole number of words, write every slot. */
        uint16 v = p[0] | p[1] << 8;
        len = (len + 1) >> 1;
        for (int i = 0; i < len; i++)
          dst[i] = v;
        p += 2;
      } else {
        /* Horizontal copy: bytes can be memcpy'd as-is because the
         * destination is already at a word boundary. */
        memcpy(dst, p, len);
        p += len;
      }
    } else {
      // increment vram by 32 instead of 1
      uint16 *dst = &g_zenv.vram[vmem_addr];
      if (is_memset) {
        /* Vertical fill: same as horizontal fill but advance dst by
         * one tilemap row (32 words) per write. */
        uint16 v = p[0] | p[1] << 8;
        len = (len + 1) >> 1;
        for (int i = 0; i < len; i++, dst += 32)
          *dst = v;
        p += 2;
      } else {
        /* Vertical copy: walk the source word-by-word and stride the
         * destination by 32. len must be even because we can only
         * place whole words at row boundaries. */
        assert((len & 1) == 0);
        len >>= 1;
        for (int i = 0; i < len; i++, dst += 32, p += 2)
          WORD(*dst) = WORD(*p);
      }
    }
  }
}

/*
 * NMI_UpdateIRQGFX: small VRAM update path used by the polyhedral
 * Triforce thread (intro/credits/Ganon-emerges cutscenes). When the
 * polyhedral renderer has produced a new frame in WRAM 0xE800, this
 * function flushes the resulting 0x800 bytes of tile data into VRAM
 * at 0x5800. The flag is one-shot to avoid reuploading a stale frame.
 */
void NMI_UpdateIRQGFX() {  // 809347
  if (nmi_flag_update_polyhedral) {
    memcpy(&g_zenv.vram[0x5800], &g_ram[0xe800], 0x800);
    nmi_flag_update_polyhedral = 0;
  }
}

