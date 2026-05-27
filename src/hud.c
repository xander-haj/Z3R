/*
 * hud.c - Heads-Up Display rendering, inventory menu, and item management
 *
 * Implements the on-screen HUD (hearts, magic meter, rupee/bomb/arrow counts,
 * equipped item icon) and the full-screen inventory menu opened with Start.
 * All HUD elements are rendered to BG3, the highest-priority SNES background
 * layer, ensuring they always draw above gameplay.
 *
 * The inventory menu is a multi-state machine that handles:
 *   - The main Y-button item grid (up to 24 items in the new layout)
 *   - The bottle sub-menu (4 slots for potions/fairies/bees)
 *   - The equipment display (sword, shield, armor, dungeon items)
 *   - Progress tracking (pendant and crystal collection status)
 *   - The ability box (gloves, boots, flippers, Moon Pearl)
 *
 * Each item in the inventory grid is represented as a 2x2 tile block in
 * VRAM, using the ItemBoxGfx structure to store the four tile indices.
 * Tile indices encode both the character number and palette via SNES
 * bitfield format: pppccctt tttttttt (priority, palette, flip, char).
 *
 * This file also handles the real-time refill animations for hearts, magic,
 * rupees, bombs, and arrows -- incrementing/decrementing each frame with
 * appropriate sound effects until the displayed value matches the target.
 *
 * Related files:
 *   hud.h       - Public API and HUD item enum definitions
 *   messaging.c - In-game text display (shares BG3 tilemap space)
 *   variables.h - WRAM-mapped game state variables (health, items, etc.)
 *   zelda_rtl.h - Runtime library bridging SNES hardware emulation
 */

/* Core includes: runtime bridge, game state variables, HUD API, text system */
#include "hud.h"
#include "zelda_rtl.h"

#include "variables.h"
#include "messaging.h"
#include "config.h"
#include "features.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strncasecmp _strnicmp
#endif

extern void ZeldaApplyRuntimeVideoConfig();

/*
 * Inventory layout mode selection.
 * kNewStyleInventory = 0 selects the original 5-column x 4-row layout (20 slots).
 * Setting to 1 enables a 6-column x 4-row layout (24 slots) that adds separate
 * bottle slots visible on the main grid, eliminating the bottle sub-menu.
 */
enum {
  kNewStyleInventory = 0,
  kHudItemCount = kNewStyleInventory ? 24 : 20,
};

/*
 * ItemBoxGfx - 2x2 tile block for a single inventory item icon.
 * v[0]=top-left, v[1]=top-right, v[2]=bottom-left, v[3]=bottom-right.
 * Each uint16 is an SNES VRAM word: palette | flip | char index.
 */
typedef struct ItemBoxGfx {
  uint16 v[4];
} ItemBoxGfx;

/* Forward declarations for internal functions */
static void Hud_DrawItem(uint16 *dst, const ItemBoxGfx *src);
static bool Hud_DoWeHaveThisItem(uint8 item);
static void Hud_EquipPrevItem(uint8 *item);
static void Hud_EquipNextItem(uint8 *item);
static int Hud_GetItemPosition(int item);
static void Hud_ReorderItem(int direction);
static void Hud_Update_Magic();
static void Hud_Update_Inventory();
static void Hud_Update_Hearts();

/*
 * Capacity tables indexed by upgrade level.
 * Bomb/arrow capacities increase as the player finds bag upgrades from
 * the pond fairy. Health capacity determines the low-health beep threshold.
 */
const uint8 kMaxBombsForLevel[] = { 10, 15, 20, 25, 30, 35, 40, 50 };
const uint8 kMaxArrowsForLevel[] = { 30, 35, 40, 45, 50, 55, 60, 70 };
/* Indexed by (link_health_capacity >> 3); value is the threshold below which
 * the low-health warning beep plays. Units are in quarter-hearts. */
static const uint8 kMaxHealthForLevel[] = { 9, 9, 9, 9, 9, 9, 9, 9, 17, 17, 17, 17, 17, 17, 17, 25, 25, 25, 25, 25, 25 };

/* Convert (x, y) tile coordinates to a linear offset in the 32-wide BG3
 * tilemap. The SNES BG3 tilemap is 32 tiles wide, so each row is 32 words. */
#define HUDXY(x, y) ((x) + (y) * 32)
#define HUDWIDEXY(x, y) ((x) + (y) * 64)

static uint16 g_wide_hud_tilemap[64 * 30];

const uint16 *Hud_GetWideHudTilemap() {
  return g_wide_hud_tilemap;
}

static bool Hud_RearrangeEnabled() {
  return (enhanced_features0 & (kFeatures0_ExtendScreen64 | kFeatures0_RearrangeHud)) ==
      (kFeatures0_ExtendScreen64 | kFeatures0_RearrangeHud) &&
      g_config.extended_aspect_ratio != 0;
}

static int Hud_WideVisibleXToBg3Column(int x) {
  return x < 0 ? 0 : x > 63 ? 63 : x;
}

static int Hud_ClampHudY(int y) {
  return y < 0 ? 0 : y > 29 ? 29 : y;
}

static int Hud_ClampHudBlockY(int y, int h) {
  int max_y = IntMax(0, 30 - h);
  return y < 0 ? 0 : y > max_y ? max_y : y;
}

static int Hud_GroupX(int default_x, int configured_x) {
  return Hud_RearrangeEnabled() ? configured_x : default_x;
}

static int Hud_GroupY(int default_y, int configured_y) {
  return Hud_RearrangeEnabled() ? Hud_ClampHudY(configured_y) : default_y;
}

static bool Hud_ItemSwitchHudEnabled() {
  return (enhanced_features0 & (kFeatures0_SwitchLR | kFeatures0_SwitchLRLimit)) != 0;
}

static bool Hud_ShouldShowDungeonKeys() {
  return link_num_keys != 0xff && (main_module_index == 7 || (main_module_index == 14 && saved_module_for_menu == 7));
}

static bool Hud_HasAssignedSwitchItem() {
  return Hud_ItemSwitchHudEnabled() && (hud_cur_item_x || hud_cur_item_l || hud_cur_item_r);
}

static void Hud_UploadItemBoxLabelTiles() {
#define PV(a0,a1,a2,a3,a4,a5,a6,a7)  ((a0 & 1) << 7 | (a0 >> 1 & 1) << 15 | (a1 & 1) << 6 | (a1 >> 1 & 1) << 14 | (a2 & 1) <<5 | (a2 >> 1&1) <<13 | (a3 & 1) << 4 | (a3>> 1 & 1) << 12 | (a4 & 1) << 3 | (a4 >> 1 & 1) << 11 | (a5 & 1) << 2 | (a5 >> 1 & 1) << 10 | (a6 & 1) << 1 | (a6 >> 1 & 1) << 9 | (a7 & 1) << 0 | (a7 >> 1 & 1) << 8)
  enum {
    kHudItemLabelTileY = 0x10,
    kHudItemLabelTileX = 0x11,
    kHudItemLabelTileL = 0x12,
    kHudItemLabelTileR = 0x13,
  };
#define O 3
#define F 2
  static const uint16 kHudItemLabelTiles[4][8] = {
    {
      PV(0,O,F,O,0,O,F,O),
      PV(0,0,O,F,O,F,O,0),
      PV(0,0,0,O,F,O,0,0),
      PV(0,0,0,O,F,O,0,0),
      PV(0,0,0,O,F,O,0,0),
      PV(0,0,0,0,O,0,0,0),
      PV(0,0,0,0,0,0,0,0),
      PV(0,0,0,0,0,0,0,0),
    },
    {
      PV(0,O,F,O,0,O,F,O),
      PV(0,0,O,F,O,F,O,0),
      PV(0,0,0,O,F,O,0,0),
      PV(0,0,0,O,F,O,0,0),
      PV(0,0,O,F,O,F,O,0),
      PV(0,O,F,O,0,O,F,O),
      PV(0,0,0,0,0,0,0,0),
      PV(0,0,0,0,0,0,0,0),
    },
    {
      PV(0,O,F,O,0,0,0,0),
      PV(0,O,F,O,0,0,0,0),
      PV(0,O,F,O,0,0,0,0),
      PV(0,O,F,O,0,0,0,0),
      PV(0,O,F,F,F,F,O,0),
      PV(0,0,O,O,O,O,0,0),
      PV(0,0,0,0,0,0,0,0),
      PV(0,0,0,0,0,0,0,0),
    },
    {
      PV(0,O,F,F,F,O,0,0),
      PV(0,O,F,O,O,F,O,0),
      PV(0,O,F,F,F,O,0,0),
      PV(0,O,F,O,F,O,0,0),
      PV(0,O,F,O,0,O,F,O),
      PV(0,0,O,0,0,0,O,0),
      PV(0,0,0,0,0,0,0,0),
      PV(0,0,0,0,0,0,0,0),
    },
  };
#undef F
#undef O
  for (int i = 0; i < 4; i++)
    memcpy(&g_zenv.vram[0x7000 + (kHudItemLabelTileY + i) * 8], kHudItemLabelTiles[i], sizeof(kHudItemLabelTiles[i]));
#undef PV
}

static uint16 *Hud_TopHudTilePtr(int x, int y) {
  if (Hud_RearrangeEnabled())
    return &g_wide_hud_tilemap[HUDWIDEXY(Hud_WideVisibleXToBg3Column(x), y)];
  return &hud_tile_indices_buffer[HUDXY(x, y)];
}

static void Hud_SetTopHudTile(int x, int y, uint16 tile) {
  if (Hud_RearrangeEnabled()) {
    if ((uint)x < 64 && (uint)y < 30)
      g_wide_hud_tilemap[HUDWIDEXY(x, y)] = tile;
  } else if ((uint)x < 32 && (uint)y < 6) {
    *Hud_TopHudTilePtr(x, y) = tile;
  }
}

static void Hud_DrawTopHudBlock(int x, int y, const uint16 *src, int w, int h, int src_stride) {
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++)
      Hud_SetTopHudTile(x + col, y + row, src[row * src_stride + col]);
  }
}

/* VRAM tile offsets for each item slot in the new 6x4 inventory grid layout.
 * Each entry is a HUDXY offset where the item's 2x2 tile block starts. */
static const uint16 kHudItemInVramPtr_New[24] = {
  HUDXY(2,  7), HUDXY(5,  7), HUDXY(8,  7), HUDXY(11,  7), HUDXY(14,  7), HUDXY(17, 7),
  HUDXY(2, 10), HUDXY(5, 10), HUDXY(8, 10), HUDXY(11, 10), HUDXY(14, 10), HUDXY(17, 10),
  HUDXY(2, 13), HUDXY(5, 13), HUDXY(8, 13), HUDXY(11, 13), HUDXY(14, 13), HUDXY(17, 13),
  HUDXY(2, 16), HUDXY(5, 16), HUDXY(8, 16), HUDXY(11, 16), HUDXY(14, 16), HUDXY(17, 16),
};

/* HUD item slot VRAM offsets for the legacy 20-slot inventory layout (kNewStyleInventory == 0).
 * Each entry is a HUDXY(x, y) word encoding the tile-column and tile-row of one 2×2 item icon
 * within the 32-wide BG3 tilemap.  The 20 slots are arranged as a 5-column × 4-row grid with
 * 3-tile spacing between columns and 3-tile spacing between rows.
 * Selected at run time via the kHudItemInVramPtr macro (see below). */
static const uint16 kHudItemInVramPtr_Old[20] = {
  HUDXY(4,  7), HUDXY(7,  7), HUDXY(10,  7), HUDXY(13,  7), HUDXY(16,  7),
  HUDXY(4, 10), HUDXY(7, 10), HUDXY(10, 10), HUDXY(13, 10), HUDXY(16, 10),
  HUDXY(4, 13), HUDXY(7, 13), HUDXY(10, 13), HUDXY(13, 13), HUDXY(16, 13),
  HUDXY(4, 16), HUDXY(7, 16), HUDXY(10, 16), HUDXY(13, 16), HUDXY(16, 16),
};


/* Compile-time selector that resolves to the correct VRAM-offset table for the active inventory
 * layout: kHudItemInVramPtr_New (24 slots) when kNewStyleInventory is true, otherwise the legacy
 * kHudItemInVramPtr_Old (20 slots).  All icon-drawing code uses this macro so it remains agnostic
 * of the chosen layout. */
#define kHudItemInVramPtr (kNewStyleInventory ? kHudItemInVramPtr_New : kHudItemInVramPtr_Old)

/* --- ItemBoxGfx tile tables ---
 * Each table below stores the four 2×2 VRAM tile IDs (top-left, top-right, bottom-left,
 * bottom-right) for every upgrade level of one item.  Index 0 is always the "not owned" blank
 * tile (0x20f5 = transparent).  Higher indices correspond to successive upgrade tiers.
 * These tables are referenced through kHudItemBoxGfxPtrs[] so the drawing code can look up any
 * item's icon in a single indirect call to Hud_GetIconForItem(). */

/* Bottle icon: index 0 = empty slot, 1 = empty bottle, 2 = mushroom in bottle,
 * 3 = red potion, 4 = green potion, 5 = blue potion, 6 = fairy, 7/8 = honeybee. */
static const ItemBoxGfx kHudItemBottles[9] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2044, 0x2045, 0x2046, 0x2047}},
  {{0x2837, 0x2838, 0x2cc3, 0x2cd3}},
  {{0x24d2, 0x64d2, 0x24e2, 0x24e3}},
  {{0x3cd2, 0x7cd2, 0x3ce2, 0x3ce3}},
  {{0x2cd2, 0x6cd2, 0x2ce2, 0x2ce3}},
  {{0x2855, 0x6855, 0x2c57, 0x2c5a}},
  {{0x2837, 0x2838, 0x2839, 0x283a}},
  {{0x2837, 0x2838, 0x2839, 0x283a}},
};
/* Bow icon: index 0 = not owned, 1 = wood bow (no arrows), 2 = wood bow (with arrows),
 * 3 = silver arrows bow (no arrows), 4 = silver arrows bow (with arrows). */
static const ItemBoxGfx kHudItemBow[5] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x28ba, 0x28e9, 0x28e8, 0x28cb}},
  {{0x28ba, 0x284a, 0x2849, 0x28cb}},
  {{0x28ba, 0x28e9, 0x28e8, 0x28cb}},
  {{0x28ba, 0x28bb, 0x24ca, 0x28cb}},
};
/* Boomerang icon: index 0 = not owned, 1 = wood boomerang, 2 = magic boomerang. */
static const ItemBoxGfx kHudItemBoomerang[3] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2cb8, 0x2cb9, 0x2cf5, 0x2cc9}},
  {{0x24b8, 0x24b9, 0x24f5, 0x24c9}},
};
/* Hookshot icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemHookshot[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x24f5, 0x24f6, 0x24c0, 0x24f5}},
};
/* Bombs icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemBombs[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2cb2, 0x2cb3, 0x2cc2, 0x6cc2}},
};
/* Mushroom/Magic Powder icon: index 0 = not owned, 1 = mushroom, 2 = magic powder. */
static const ItemBoxGfx kHudItemMushroom[3] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2444, 0x2445, 0x2446, 0x2447}},
  {{0x203b, 0x203c, 0x203d, 0x203e}},
};
/* Fire Rod icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemFireRod[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x24b0, 0x24b1, 0x24c0, 0x24c1}},
};
/* Ice Rod icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemIceRod[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2cb0, 0x2cbe, 0x2cc0, 0x2cc1}},
};
/* Bombos Medallion icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemBombos[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x287d, 0x287e, 0xe87e, 0xe87d}},
};
/* Ether Medallion icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemEther[2] = {
  {{0x20f5, 0x20f5,  0x20f5,  0x20f5}},
  {{0x2876, 0x2877, 0xE877, 0xE876}},
};
/* Quake Medallion icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemQuake[2] = {
  {{0x20f5, 0x20f5,  0x20f5,  0x20f5}},
  {{0x2866, 0x2867, 0xE867, 0xE866}},
};
/* Lamp / Torch icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemTorch[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x24bc, 0x24bd, 0x24cc, 0x24cd}},
};
/* Hammer icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemHammer[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x20b6, 0x20b7, 0x20c6, 0x20c7}},
};
/* Flute / Ocarina icon: index 0 = not owned, 1 = shovel, 2 = flute (bird available),
 * 3 = flute (bird summoned, same graphic as index 2). */
static const ItemBoxGfx kHudItemFlute[4] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x20d0, 0x20d1, 0x20e0, 0x20e1}},
  {{0x2cd4, 0x2cd5, 0x2ce4, 0x2ce5}},
  {{0x2cd4, 0x2cd5, 0x2ce4, 0x2ce5}},
};
/* Bug-Catching Net icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemBugNet[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x3c40, 0x3c41, 0x2842, 0x3c43}},
};
/* Book of Mudora icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemBookMudora[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x3ca5, 0x3ca6, 0x3cd8, 0x3cd9}},
};
/* Cane of Somaria icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemCaneSomaria[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x24dc, 0x24dd, 0x24ec, 0x24ed}},
};
/* Cane of Byrna icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemCaneByrna[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2cdc, 0x2cdd, 0x2cec, 0x2ced}},
};
/* Magic Cape icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemCape[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x24b4, 0x24b5, 0x24c4, 0x24c5}},
};
/* Magic Mirror icon: index 0 = not owned, 1 = magic mirror (overworld mode),
 * 2 = magic mirror (dungeon mode), 3 = magic mirror (special dark-world mode). */
static const ItemBoxGfx kHudItemMirror[4] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x28de, 0x28df, 0x28ee, 0x28ef}},
  {{0x2c62, 0x2c63, 0x2c72, 0x2c73}},
  {{0x2886, 0x2887, 0x2888, 0x2889}},
};
/* Power Gloves icon: index 0 = not owned, 1 = power gloves, 2 = titan's mitts. */
static const ItemBoxGfx kHudItemGloves[3] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2130, 0x2131, 0x2140, 0x2141}},
  {{0x28da, 0x28db, 0x28ea, 0x28eb}},
};
/* Pegasus Boots icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemBoots[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x3429, 0x342a, 0x342b, 0x342c}},
};
/* Zora's Flippers icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemFlippers[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2c9a, 0x2c9b, 0x2c9d, 0x2c9e}},
};
/* Moon Pearl icon: index 0 = not owned, 1 = owned. */
static const ItemBoxGfx kHudItemMoonPearl[2] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2433, 0x2434, 0x2435, 0x2436}},
};
/* Placeholder icon used for an unoccupied or unrecognized item slot.  All four tiles are the
 * transparent blank tile (0x20f5), rendering the slot invisible. */
static const ItemBoxGfx kHudItemEmpty[1] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
};
/* Sword icon (shown in the equipment box, not the item inventory):
 * index 0 = no sword, 1 = fighter's sword, 2 = master sword, 3 = tempered sword, 4 = golden sword.
 * Caller maps link_sword_type == 0xff to index 0 before indexing this table. */
static const ItemBoxGfx kHudItemSword[5] = {
  {{0x20f5, 0x20f5, 0x20f5, 0x20f5}},
  {{0x2c64, 0x2cce, 0x2c75, 0x3d25}},
  {{0x2c8a, 0x2c65, 0x2474, 0x3d26}},
  {{0x248a, 0x2465, 0x3c74, 0x2d48}},
  {{0x288a, 0x2865, 0x2c74, 0x2d39}},
};
/* Shield icon (shown in the equipment box):
 * index 0 = no shield (transparent), 1 = small shield, 2 = fire shield, 3 = mirror shield. */
static const ItemBoxGfx kHudItemShield[4] = {
  {{0x24f5, 0x24f5, 0x24f5, 0x24f5}},
  {{0x2cfd, 0x6cfd, 0x2cfe, 0x6cfe}},
  {{0x34ff, 0x74ff, 0x349f, 0x749f}},
  {{0x2880, 0x2881, 0x288d, 0x288e}},
};
/* Tunic (armor) icon (shown in the equipment box):
 * index 0 = green tunic, 1 = blue mail, 2 = red mail.  Array size is 5 but only 3 entries are
 * defined; the extra capacity accommodates potential future upgrade tiers. */
static const ItemBoxGfx kHudItemArmor[5] = {
  {{0x3c68, 0x7c68, 0x3c78, 0x7c78}},
  {{0x2c68, 0x6c68, 0x2c78, 0x6c78}},
  {{0x2468, 0x6468, 0x2478, 0x6478}},
};


/* Master dispatch table mapping 1-based item IDs to their ItemBoxGfx tile tables.
 * Index i-1 gives the table for item ID i (IDs are 1-based throughout the game's item system).
 * The caller indexes into the chosen table with the item's current upgrade level to retrieve
 * the four VRAM tile IDs for that item's 2×2 icon.
 * Bottle slots 1-4 (IDs kHudItem_Bottle1–kHudItem_Bottle4) all share kHudItemBottles, with the
 * correct bottle-content index looked up from link_bottle_info[] in Hud_GetIconForItem(). */
static const ItemBoxGfx * const kHudItemBoxGfxPtrs[] = {
  kHudItemBow,
  kHudItemBoomerang,
  kHudItemHookshot,
  kHudItemBombs,
  kHudItemMushroom,
  kHudItemFireRod,
  kHudItemIceRod,
  kHudItemBombos,
  kHudItemEther,
  kHudItemQuake,
  kHudItemTorch,
  kHudItemHammer,
  kHudItemFlute,
  kHudItemBugNet,
  kHudItemBookMudora,
  kHudItemBottles,
  kHudItemCaneSomaria,
  kHudItemCaneByrna,
  kHudItemCape,
  kHudItemMirror,
  kHudItemGloves,
  kHudItemBoots,
  kHudItemFlippers,
  kHudItemMoonPearl,
  kHudItemEmpty,
  kHudItemSword,
  kHudItemShield,
  kHudItemArmor,
  kHudItemBottles,
  kHudItemBottles,
  kHudItemBottles,
  kHudItemBottles,

};
/* Magic meter tilemap fragments, one row of four tiles per fill level.
 * Index = (link_magic_power + 7) >> 3, giving 17 discrete fill steps (0–16 eighths of the meter).
 * Each 4-entry row encodes the four vertical tile positions of the magic bar column (rows 1–4).
 * Tile 0x3cf5 is the empty meter segment; 0x3c5f/0x3c4c/0x3c4d/0x3c4e are progressively fuller
 * segment tiles; 0x3c5e is the fully-filled segment tile.  Written by Hud_Update_Magic(). */
static const uint16 kUpdateMagicPowerTilemap[17][4] = {
  {0x3cf5, 0x3cf5, 0x3cf5, 0x3cf5},
  {0x3cf5, 0x3cf5, 0x3cf5, 0x3c5f},
  {0x3cf5, 0x3cf5, 0x3cf5, 0x3c4c},
  {0x3cf5, 0x3cf5, 0x3cf5, 0x3c4d},
  {0x3cf5, 0x3cf5, 0x3cf5, 0x3c4e},
  {0x3cf5, 0x3cf5, 0x3c5f, 0x3c5e},
  {0x3cf5, 0x3cf5, 0x3c4c, 0x3c5e},
  {0x3cf5, 0x3cf5, 0x3c4d, 0x3c5e},
  {0x3cf5, 0x3cf5, 0x3c4e, 0x3c5e},
  {0x3cf5, 0x3c5f, 0x3c5e, 0x3c5e},
  {0x3cf5, 0x3c4c, 0x3c5e, 0x3c5e},
  {0x3cf5, 0x3c4d, 0x3c5e, 0x3c5e},
  {0x3cf5, 0x3c4e, 0x3c5e, 0x3c5e},
  {0x3c5f, 0x3c5e, 0x3c5e, 0x3c5e},
  {0x3c4c, 0x3c5e, 0x3c5e, 0x3c5e},
  {0x3c4d, 0x3c5e, 0x3c5e, 0x3c5e},
  {0x3c4e, 0x3c5e, 0x3c5e, 0x3c5e},
};
/* Dungeon floor-number indicator tile sequences for the HUD floor display.
 * Each array is a null-terminated (0x7f sentinel) list of tile IDs that form the animated
 * sprite used to show the current dungeon floor number.  Gfx0 is the first frame (outline /
 * background), Gfx1 is the second frame (number glyph).  Both are cycled by Hud_FloorIndicator()
 * to produce the blinking floor-number animation when Link moves between dungeon floors. */
static const uint16 kDungFloorIndicator_Gfx0[11] = { 0x2508, 0x2509, 0x2509, 0x250a, 0x250b, 0x250c, 0x250d, 0x251d, 0xe51c, 0x250e, 0x7f };
static const uint16 kDungFloorIndicator_Gfx1[11] = { 0x2518, 0x2519, 0xa509, 0x251a, 0x251b, 0x251c, 0x2518, 0xa51d, 0xe50c, 0xa50e, 0x7f };

static void Hud_SetLegacyIndicatorTile(int legacy_index, uint16 tile) {
  if (Hud_RearrangeEnabled()) {
    int local = legacy_index - 0xf2 / 2;
    int x = g_config.hud_floor_indicator_pos_x + (local & 31);
    int y = Hud_ClampHudBlockY(g_config.hud_floor_indicator_pos_y, 2) + (local >> 5);
    Hud_SetTopHudTile(x, y, tile);
  } else {
    hud_tile_indices_buffer[legacy_index] = tile;
  }
}


/* Re-synchronize the HUD icon with the currently equipped item and redraw the entire HUD.
 * Called after the inventory menu closes or an item is swapped, to ensure the Y-button icon and
 * all counter displays reflect the new game state.  Resets overworld_map_state to 0 to end any
 * pending map-screen animation. */
void Hud_RefreshIcon() {
  Hud_SearchForEquippedItem();
  Hud_UpdateHud();
  Hud_Rebuild();
  overworld_map_state = 0;
}

/* Returns 1 if the player owns the "key item" associated with the current palace (dungeon),
 * 0 otherwise.  Used by the equipment box to decide whether to draw the dungeon's signature
 * item icon as filled or empty.  The mapping (palace index → required item) is hard-coded:
 * e.g. Palace 2 (Eastern) requires the bow, Palace 5 (Swamp) requires the hookshot, Palace 9
 * (Misery Mire) requires any tunic, Palace 12 (Turtle Rock) requires the mirror shield, etc. */
uint8 CheckPalaceItemPosession() {
  switch (cur_palace_index_x2 >> 1) {
  case 2: return link_item_bow != 0;
  case 3: return link_item_gloves != 0;
  case 5: return link_item_hookshot != 0;
  case 6: return link_item_hammer != 0;
  case 7: return link_item_cane_somaria != 0;
  case 8: return link_item_fire_rod != 0;
  case 9: return link_armor != 0;
  case 10: return link_item_moon_pearl != 0;
  case 11: return link_item_gloves != 1;
  case 12: return link_shield_type == 3;
  case 13: return link_armor == 2;
  default:
    return 0;
  }
}

/* Returns the 0-based slot index in the inventory grid where `item` is currently displayed,
 * or -1 if `item` is 0 (no item).  When hud_inventory_order[] has been initialized to a custom
 * permutation, performs a linear scan; otherwise the slot index is simply item-1 because the
 * default inventory order is sequential.
 *
 * Returns the zero based index of the currently selected hud item, or -1 if the item is item 0. */
static int Hud_GetItemPosition(int item) {
  if (item <= 0)
    return -1;
  if (hud_inventory_order[0] != 0) {
    int i = 0;
    for (; i < kHudItemCount - 1 && hud_inventory_order[i] != item; i++) {}
    return i;
  } else {
    return item ? item - 1 : item;
  }
}

/* Move *item one slot backwards (left/up) in the inventory grid, wrapping around at the start.
 * `first_item_index` is the lowest slot index that should ever be selected:
 *   0 → wrapping is allowed past slot 0 (cursor can land on "no item")
 *   1 → wrapping skips slot 0 entirely
 * Honors the custom hud_inventory_order[] permutation when one is in effect. */
static void Hud_GotoPrevItem(uint8 *item, uint8 first_item_index) {
  if (hud_inventory_order[0] != 0) {
    int pos = Hud_GetItemPosition(*item);
    *item = (pos == 0 && !first_item_index) ? 0 :
        hud_inventory_order[(pos <= 0 ? kHudItemCount : pos) - 1];
  } else {
    *item = (*item > first_item_index) ? *item - 1 : kHudItemCount;
  }
}

/* Move *item one slot forwards (right/down) in the inventory grid, wrapping around at the end.
 * `first_item_index` is the slot to wrap to once the last slot is passed.  Honors the custom
 * hud_inventory_order[] permutation when one is active. */
static void Hud_GotoNextItem(uint8 *item, uint8 first_item_index) {
  if (hud_inventory_order[0] != 0) {
    int i = Hud_GetItemPosition(*item);
    *item = hud_inventory_order[((uint)i >= kHudItemCount - 1) ? 0 : i + 1];
  } else {
    *item = (*item < kHudItemCount) ? *item + 1 : first_item_index;
  }
}

/* Drive the dungeon floor-number indicator that briefly appears at the top of the HUD when Link
 * crosses a floor boundary.  Counts hud_floor_changed_timer up to 0xC0 frames, drawing the
 * floor number tiles into hud_tile_indices_buffer using kDungFloorIndicator_Gfx0/Gfx1.
 * Negative floor values (basements) are flipped via XOR 0xff so the same digit graphics work
 * for both above-ground (B → 1F) and below-ground (B1 → B5) floors, with k=1 selecting the
 * second tile column to display the leading "B".  Plays sound effect 3 (floor change) on entry
 * to the first room of a new floor and 5 (basement) when entering a basement.  When the timer
 * reaches zero the indicator is cleared via Hud_RemoveSuperBombIndicator(). */
void Hud_FloorIndicator() {  // 8afd0c
  uint16 a = hud_floor_changed_timer;
  if (a == 0) {
    Hud_RemoveSuperBombIndicator();
    return;
  }
  a += 1;
  if (a == 0xc0)
    a = 0;
  WORD(hud_floor_changed_timer) = a;

  Hud_SetLegacyIndicatorTile(0xf2 / 2, 0x251e);
  Hud_SetLegacyIndicatorTile(0x134 / 2, 0x251f);
  Hud_SetLegacyIndicatorTile(0x132 / 2, 0x2520);
  Hud_SetLegacyIndicatorTile(0xf4 / 2, 0x250f);

  int k = 0, j;

  if (!sign8(dung_cur_floor)) {
    if (!WORD(dung_cur_floor) && dungeon_room_index != 2 && sram_progress_indicator < 2)
      sound_effect_ambient = 3;
    j = dung_cur_floor;
  } else {
    sound_effect_ambient = 5;
    k++;
    j = dung_cur_floor ^ 0xff;
  }
  Hud_SetLegacyIndicatorTile(k + 0xf2 / 2, kDungFloorIndicator_Gfx0[j]);
  Hud_SetLegacyIndicatorTile(k + 0x132 / 2, kDungFloorIndicator_Gfx1[j]);
  flag_update_hud_in_nmi++;
}

/* Clear the four tile slots used by both the floor indicator and the super-bomb price counter,
 * leaving them as transparent space tiles (0x7f).  Called when either indicator finishes its
 * display window. */
void Hud_RemoveSuperBombIndicator() {  // 8afd90
  Hud_SetLegacyIndicatorTile(0xf2 / 2, 0x7f);
  Hud_SetLegacyIndicatorTile(0x132 / 2, 0x7f);
  Hud_SetLegacyIndicatorTile(0xf4 / 2, 0x7f);
  Hud_SetLegacyIndicatorTile(0x134 / 2, 0x7f);
}

/* Draw and animate the super-bomb price countdown that appears in the same HUD slot as the
 * floor indicator.  super_bomb_indicator_unk2 holds the remaining count (decremented every
 * 62 frames driven by super_bomb_indicator_unk1); the two-digit value is split into tens (q)
 * and ones (r) and written into the floor-indicator tile slots using the same digit graphics.
 * When the count expires the indicator is cleared via Hud_RemoveSuperBombIndicator(). */
void Hud_SuperBombIndicator() {  // 8afda8
  if (!super_bomb_indicator_unk1) {
    if (sign8(super_bomb_indicator_unk2))
      goto remove;
    super_bomb_indicator_unk2--;
    super_bomb_indicator_unk1 = 62;
  }
  super_bomb_indicator_unk1--;
  if (sign8(super_bomb_indicator_unk2)) {
remove:
    super_bomb_indicator_unk2 = 0xff;
    Hud_RemoveSuperBombIndicator();
    return;
  }

  int r = super_bomb_indicator_unk2 % 10;
  int q = super_bomb_indicator_unk2 / 10;

  int j = sign8(r - 1) ? 9 : r - 1;
  Hud_SetLegacyIndicatorTile(0xf4 / 2, kDungFloorIndicator_Gfx0[j]);
  Hud_SetLegacyIndicatorTile(0x134 / 2, kDungFloorIndicator_Gfx1[j]);

  j = sign8(q - 1) ? 10 : q - 1;
  Hud_SetLegacyIndicatorTile(0xf2 / 2, kDungFloorIndicator_Gfx0[j]);
  Hud_SetLegacyIndicatorTile(0x132 / 2, kDungFloorIndicator_Gfx1[j]);
}

/* Return the maximum allowed rupee count.  Vanilla cap is 999; the kFeatures0_CarryMoreRupees
 * compatibility flag raises the cap to 9999 to enable the larger four-digit display. */
static int MaxRupees() {
  return enhanced_features0 & kFeatures0_CarryMoreRupees ? 9999 : 999;
}

/* Per-frame refill / animation tick for the HUD's resource counters.  Suppressed when the
 * overworld map is open.  Drives, in order:
 *   1. Magic-power refill from link_magic_filler with sound effect 45 every 4 frames
 *   2. Rupee count walk toward link_rupees_goal (capped at MaxRupees(), can go up or down)
 *      with the chiming sfx 41 throttled by rupee_sfx_sound_delay
 *   3. Bomb count refill / drain toward link_bomb_filler/link_bomb_decrement with sfx 41
 *   4. Arrow count refill / drain toward link_arrow_filler/link_arrow_decrement with sfx 41
 *   5. Heart refill (Hud_RefillHealth + Hud_AnimateHeartRefill) with the heart sound
 *   6. Low-health beep (sound effect 45) when current health is at or below the danger
 *      threshold and Link is conscious.
 * After any of these change the HUD it sets flag_update_hud_in_nmi so the NMI handler reuploads
 * the new tilemap during V-Blank. */
void Hud_RefillLogic() {  // 8ddb92
  if (overworld_map_state)
    return;
  if (link_magic_filler) {
    if (link_magic_power >= 128) {
      link_magic_power = 128;
      link_magic_filler = 0;
    } else {
      link_magic_filler--;
      link_magic_power++;
      if ((frame_counter & 3) == 0 && sound_effect_1 == 0)
        sound_effect_1 = 45;
    }
  }

  uint16 a = link_rupees_actual;
  if (a != link_rupees_goal) {
    if (a >= link_rupees_goal) {
      if ((int16)--a < 0)
        link_rupees_goal = a = 0;
    } else {
      int m = MaxRupees();
      if (++a > m)
        link_rupees_goal = a = m;
    }
    link_rupees_actual = a;
    if (sound_effect_1 == 0) {
      if ((rupee_sfx_sound_delay++ & 7) == 0)
        sound_effect_1 = 41;
    } else {
      rupee_sfx_sound_delay = 0;
    }
  } else {
    rupee_sfx_sound_delay = 0;
  }

  if (link_bomb_filler) {
    link_bomb_filler--;
    if (link_item_bombs != kMaxBombsForLevel[link_bomb_upgrades])
      link_item_bombs++;
  }

  if (link_arrow_filler) {
    link_arrow_filler--;
    if (link_num_arrows != kMaxArrowsForLevel[link_arrow_upgrades])
      link_num_arrows++;
    if (link_item_bow && (link_item_bow & 1) == 1) {
      link_item_bow++;
      Hud_RefreshIcon();
    }
  }

  if (!flag_is_link_immobilized && !link_hearts_filler &&
      link_health_current < kMaxHealthForLevel[link_health_capacity >> 3]) {
    if (link_lowlife_countdown_timer_beep) {
      link_lowlife_countdown_timer_beep--;
    } else if (!sound_effect_1) {
      if (!(enhanced_features0 & kFeatures0_DisableLowHealthBeep))
        sound_effect_1 = 43;
      link_lowlife_countdown_timer_beep = 32 - 1;
    }
  }

  if (is_doing_heart_animation)
    goto doing_animation;
  if (link_hearts_filler) {
    if (link_health_current < link_health_capacity) {
      link_health_current += 8;
      if (link_health_current >= link_health_capacity)
        link_health_current = link_health_capacity;

      if (sound_effect_2 == 0)
        sound_effect_2 = 13;

      link_hearts_filler -= 8;
      is_doing_heart_animation++;
      animate_heart_refill_countdown = 7;

doing_animation:
      Hud_Update_Magic();
      Hud_Update_Inventory();
      Hud_AnimateHeartRefill();
      flag_update_hud_in_nmi++;
      return;
    }
    link_health_current = link_health_capacity;
    link_hearts_filler = 0;
  }
  Hud_Update_Hearts();
  Hud_Update_Magic();
  Hud_Update_Inventory();
  flag_update_hud_in_nmi++;
}

/* Top-level state-machine dispatcher for the open inventory ("HUD menu") screen.  Called once
 * per frame while overworld_map_state is non-zero.  Each state corresponds to one phase of the
 * pause-menu lifecycle: clearing the BG3 tilemap, initializing the menu graphics, scrolling the
 * menu down, picking the next display mode, the interactive menu loop, item-equip update, the
 * close animation, and the four bottle sub-menu phases (states 7-12). */
void Hud_Module_Run() {  // 8ddd36
  byte_7E0206++;
  switch (overworld_map_state) {
  case 0: Hud_ClearTileMap(); break;
  case 1: Hud_Init(); break;
  case 2: Hud_BringMenuDown(); break;
  case 3: Hud_ChooseNextMode(); break;
  case 4: Hud_NormalMenu(); break;
  case 5: Hud_UpdateHud(); break;
  case 6: Hud_CloseMenu(); break;
  case 7: Hud_GotoBottleMenu(); break;
  case 8: Hud_InitBottleMenu(); break;
  case 9: Hud_ExpandBottleMenu(); break;
  case 10: Hud_BottleMenu(); break;
  case 11: Hud_EraseBottleMenu(); break;
  case 12: Hud_RestoreNormalMenu(); break;
  default:
    assert(0);
  }
}

/* HUD menu state 0: blank-fill the 1024-word BG3 work tilemap (at $7E1000) with transparent
 * tile 0x207f, queue the menu-open sound effect 17, and tell the NMI handler to upload the
 * cleared tilemap to VRAM target 0x22 on the next V-Blank.  Advances overworld_map_state to 1
 * so the next frame proceeds to Hud_Init(). */
void Hud_ClearTileMap() {  // 8ddd5a
  uint16 *target = (uint16 *)&g_ram[0x1000];
  for (int i = 0; i < 1024; i++)
    target[i] = 0x207f;
  sound_effect_2 = 17;
  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
  overworld_map_state++;
}

/* Returns true if the player owns any inventory item by scanning the 20 byte-sized item flags
 * starting at link_item_bow.  Used to skip parts of the menu (e.g. the bottle sub-menu setup)
 * when the player has nothing to display.  This is a re-implementation helper added by the
 * port — the original SNES code did not need it because the menu was data-driven. */
bool Hud_HaveAnyItems() { // new
  for (int i = 0; i < 20; i++)
    if ((&link_item_bow)[i])
      return true;
  return false;
}

/* HUD menu state 1: build the entire pause-menu BG3 layer.  Calls every panel-drawing routine
 * (Y-button items, ability box, dungeon progress icons, equipment box, selected-item readout)
 * and seeds the bottle sub-menu state by finding the first non-empty bottle slot.  If the
 * currently equipped item is a bottle and we are using the legacy inventory layout, opens the
 * bottle pop-up immediately so the player can pick which bottle to use without an extra click.
 * Sets the flashing-circle timer, schedules the NMI tilemap upload, and advances the state. */
void Hud_Init() {  // 8dddab
  Hud_SearchForEquippedItem();
  Hud_DrawYButtonItems();
  Hud_DrawAbilityBox();
  Hud_DrawProgressIcons();
  Hud_DrawEquipmentBox();
  Hud_DrawSelectedYButtonItem();

  if (Hud_HaveAnyItems()) {   
    // This causes bottle flicker because it's not early enough
    int first_bottle = 0;
    while (first_bottle < 4 && link_bottle_info[first_bottle] == 0)
      first_bottle++;
    if (first_bottle == 4)
      link_item_bottle_index = 0;
    else if (link_item_bottle_index == 0)
      link_item_bottle_index = first_bottle + 1;

    if (hud_cur_item == kHudItem_BottleOld && !kNewStyleInventory) {
      timer_for_flashing_circle = 16;
      Hud_DrawBottleMenu();
    }
  }

  timer_for_flashing_circle = 16;
  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
  overworld_map_state++;
}

/* HUD menu state 2: scroll the BG3 layer down 8 pixels per frame until the menu has fully
 * descended over the gameplay screen (target VOFS == 0xff18).  Once reached, advances the
 * state machine to Hud_ChooseNextMode(). */
void Hud_BringMenuDown() {  // 8dde59
  BG3VOFS_copy2 -= 8;
  if (BG3VOFS_copy2 == 0xff18)
    overworld_map_state++;
}

/* HUD menu state 3: choose where to go after the menu has finished its slide-down animation.
 * If the player owns at least one item, redraws the selected-item readout and either jumps
 * straight into the bottle pop-up (state 10) when a bottle is currently equipped (legacy
 * layout only) or into the regular interactive menu (state 4).  If the player has no items,
 * waits for any button press to skip directly to the close animation. */
void Hud_ChooseNextMode() {  // 8dde6e
  if (Hud_HaveAnyItems()) {
    nmi_subroutine_index = 1;
    BYTE(nmi_load_target_addr) = 0x22;

    Hud_DrawSelectedYButtonItem();

    // Pick either the bottle state or normal one
    overworld_map_state = (hud_cur_item == kHudItem_BottleOld && !kNewStyleInventory) ? 10 : 4;
  } else {
    if (filtered_joypad_H)
      overworld_map_state = 5;
  }
}

/* Returns true if the player currently owns the given item slot's contents.  Item ID 0 is
 * always considered "owned" because it represents the empty slot used by the X/L/R secondary
 * buttons.  In the new-style layout, the flute and shovel share an inventory slot but are
 * gated by different link_item_flute thresholds (1 = shovel, 2 = flute).  Bottle slots check
 * link_bottle_info[] for non-empty contents; everything else indexes the byte-flag array
 * starting at link_item_bow. */
static bool Hud_DoWeHaveThisItem(uint8 item) {  // 8ddeb0
  if (item == 0)
    return true;  // for the x item, 0 is valid

  if (item == kHudItem_Flute && kNewStyleInventory)
    return link_item_flute >= 2;

  if (item == kHudItem_Shovel && kNewStyleInventory)
    return link_item_flute >= 1;

  if (item >= kHudItem_Bottle1)
    return link_bottle_info[item - kHudItem_Bottle1] != 0;

  return (&link_item_bow)[item - 1] != 0;
}

/* Move the cursor backwards by one slot, skipping empty/unowned items, until landing on a
 * valid owned item.  Used by the inventory's left/right cursor controls. */
static void Hud_EquipPrevItem(uint8 *item) {  // 8dded9
  do {
    Hud_GotoPrevItem(item, item == &hud_cur_item);
  } while (!Hud_DoWeHaveThisItem(*item));
}

/* Move the cursor forwards by one slot, skipping empty/unowned items.  Counterpart to
 * Hud_EquipPrevItem. */
static void Hud_EquipNextItem(uint8 *item) {  // 8ddee2
  do {
    Hud_GotoNextItem(item, item == &hud_cur_item);
  } while (!Hud_DoWeHaveThisItem(*item));
}

/* Move the cursor up one full row by repeatedly stepping backwards.  The row width is 5 in the
 * legacy 20-slot layout and 6 in the new 24-slot layout.  Skips unowned items. */
static void Hud_EquipItemAbove(uint8 *item) {  // 8ddeeb
  do {
    for(int i = 0; i < (kNewStyleInventory ? 6 : 5); i++)
      Hud_GotoPrevItem(item, 1);
  } while (!Hud_DoWeHaveThisItem(*item));
}

/* Move the cursor down one full row.  Special case: when starting from the empty slot 0 a
 * single forward step is taken so the cursor lands on the first owned item rather than wrapping
 * past it.  Otherwise steps forward by one row width (5 or 6). */
static void Hud_EquipItemBelow(uint8 *item) {  // 8ddf00
  int num = *item == 0 ? 1 : (kNewStyleInventory ? 6 : 5);
  do {
    for (int i = 0; i < num; i++)
      Hud_GotoNextItem(item, 1);
  } while (!Hud_DoWeHaveThisItem(*item));
}

/* Return which item-equip button the player is currently holding so multi-button item slots
 * (Y / X / L / R) can be edited independently.  Only enabled when kFeatures0_SwitchLR is set;
 * otherwise always returns 0 (Y button).  Index 0 = Y, 1 = X, 2 = L, 3 = R. */
int GetCurrentItemButtonIndex() {
  if (enhanced_features0 & kFeatures0_SwitchLR) {
    return (joypad1L_last & kJoypadL_X) ? 1 :
           (joypad1L_last & kJoypadL_L) ? 2 :
           (joypad1L_last & kJoypadL_R) ? 3 : 0;
  }
  return 0;
}

/* Return a pointer to the equipped-item byte for the requested button index (0=Y, 1=X, 2=L,
 * 3=R).  Used by the menu code to read/write the right "current item" variable depending on
 * which button the player is editing. */
uint8 *GetCurrentItemButtonPtr(int i) {
  return (i == 0) ? &hud_cur_item : 
         (i == 1) ? &hud_cur_item_x : 
         (i == 2) ? &hud_cur_item_l : &hud_cur_item_r;
}

/* HUD menu state 4: the interactive inventory loop.  Per frame:
 *   - Tick timer_for_flashing_circle so the cursor halo blinks.
 *   - Start button → exit the menu (state 5) with click sound 18.
 *   - Select button → save/exit menu via DisplaySelectMenu().
 *   - Y + D-pad with kFeatures0_SwitchLR → reorder items in the grid via Hud_ReorderItem().
 *   - Otherwise D-pad navigates the cursor (Up/Down move by a full row, Left/Right by one
 *     slot), routed through the GetCurrentItemButtonPtr() so X/L/R can be edited too.
 *     A successful move re-arms the flashing circle and plays sfx 32.
 *   - Hud_DrawYButtonItems / Hud_DrawSelectedYButtonItem repaint the icons and label.
 *   - Selecting a bottle in the legacy layout transitions to bottle pop-up (state 7).
 *   - Marks the BG3 tilemap dirty so NMI re-uploads it. */
void Hud_NormalMenu() {  // 8ddf15
  timer_for_flashing_circle++;
  if (!BYTE(joypad1H_last))
    BYTE(hud_tmp1) = 0;

  if (filtered_joypad_H & kJoypadH_Start) {
    overworld_map_state = 5;
    sound_effect_2 = 18;
    return;
  }

  // Allow select to open the save/exit thing
  if (joypad1H_last & kJoypadH_Select && sram_progress_indicator) {
    BG3VOFS_copy2 = -8;
    Hud_CloseMenu();
    DisplaySelectMenu();
    return;
  }

  if (joypad1H_last & kJoypadH_Y && !(joypad1L_last & kJoypadL_X) && (enhanced_features0 & kFeatures0_SwitchLR)) {
    if (filtered_joypad_H & kJoypadH_Up) {
      Hud_ReorderItem(kNewStyleInventory ? -6 : -5);
    } else if (filtered_joypad_H & kJoypadH_Down) {
      Hud_ReorderItem(kNewStyleInventory ? 6 : 5);
    } else if (filtered_joypad_H & kJoypadH_Left) {
      Hud_ReorderItem(-1);
    } else if (filtered_joypad_H & kJoypadH_Right) {
      Hud_ReorderItem(1);
    }
  } else if (!BYTE(hud_tmp1)) {
    // If Special Key button is down, then move their circle
    int btn_index = GetCurrentItemButtonIndex();
    uint8 *item_p = GetCurrentItemButtonPtr(btn_index);
    uint16 old_item = *item_p;
    if (filtered_joypad_H & kJoypadH_Up) {
      Hud_EquipItemAbove(item_p);
    } else if (filtered_joypad_H & kJoypadH_Down) {
      Hud_EquipItemBelow(item_p);
    } else if (filtered_joypad_H & kJoypadH_Left) {
      Hud_EquipPrevItem(item_p);
    } else if (filtered_joypad_H & kJoypadH_Right) {
      Hud_EquipNextItem(item_p);
    }
    BYTE(hud_tmp1) = filtered_joypad_H;
    if (*item_p != old_item) {
      timer_for_flashing_circle = 16;
      sound_effect_2 = 32;
    }
  }
  Hud_DrawYButtonItems();
  Hud_DrawSelectedYButtonItem();
  if (hud_cur_item == kHudItem_BottleOld && !kNewStyleInventory)
    overworld_map_state = 7;

  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
  //g_ram[0x15d0] = 0;
}

/* HUD menu state 5: leaving the inventory.  Advances the state machine, then rebuilds the
 * gameplay HUD tilemap and refreshes the equipped-item icon so the in-world HUD reflects any
 * changes the player made while the menu was open. */
void Hud_UpdateHud() {  // 8ddfa9
  overworld_map_state++;
  Hud_Rebuild();
  Hud_UpdateEquippedItem();
}

/* Translate a HUD inventory slot index (1..N) into the canonical "received item" ID used by
 * Link_ReceiveItem and the rest of the game.  The two embedded permutations encode the mapping
 * for the legacy 20-slot grid and the new 24-slot grid; the latter splits the flute/shovel into
 * separate slots and adds four bottle entries (all returning 11 = bottle). */
uint8 Hud_LookupInventoryItem(uint8 item) {
  static const uint8 kHudItemToItemOrg[21] = {
    0,
    3,  2, 14, 1,  10,  5,
    6, 15, 16, 17,  9,  4,
    8,  7, 12, 11, 18, 13,
    19, 20,
  };

  static const uint8 kHudItemToItemNew[25] = {
    0,
    3,  2, 14, 1,  10,  5,
    6, 15, 16, 17,  9,  4,
    8,  7, 12, 21, 18, 13, // 8 is ocarina / shovel combined. moved shovel to 21.
    19, 20,11, 11, 11, 11, // 11 means bottle
  };
  return kNewStyleInventory ? kHudItemToItemNew[item] : kHudItemToItemOrg[item];
}

/* Commit the cursor's current selection as the equipped Y-button item.  Bottle slots also
 * update link_item_bottle_index so the in-world bottle picker knows which bottle is active.
 * Then translates the slot index through Hud_LookupInventoryItem() into current_item_y, the
 * canonical item ID consumed by Link_Main(). */
void Hud_UpdateEquippedItem() {  // 8ddfaf
  if (hud_cur_item >= kHudItem_Bottle1)
    link_item_bottle_index = hud_cur_item - kHudItem_Bottle1 + 1;

  assert(hud_cur_item < 25);
  current_item_y = Hud_LookupInventoryItem(hud_cur_item);
}

/* HUD menu state 6: scroll BG3 back up 8 pixels per frame until the menu is fully off-screen.
 * Once finished, rebuilds the gameplay HUD, restores the saved gameplay module
 * (saved_module_for_menu), restores any dark-room torch background that was hidden by the
 * menu, and resets the menu/equip debug variables based on which item is now selected. */
void Hud_CloseMenu() {  // 8ddfba
  BG3VOFS_copy2 += 8;
  if (BG3VOFS_copy2)
    return;
  Hud_Rebuild();
  overworld_map_state = 0;
  submodule_index = 0;
  main_module_index = saved_module_for_menu;
  if (submodule_index)
    Hud_RestoreTorchBackground();
  if (current_item_y != 5 && current_item_y != 6) {
    eq_debug_variable = 2;
    link_debug_value_1 = 0;
  } else {
    assert(!link_debug_value_1);
    eq_debug_variable = 0;
  }
}

/* HUD menu state 7: prepare to open the bottle pop-up sub-menu.  Resets the row counter that
 * controls the menu-expand animation and advances to state 8 (Hud_InitBottleMenu). */
void Hud_GotoBottleMenu() {  // 8ddffb
  bottle_menu_expand_row = 0;
  overworld_map_state++;
}

/* HUD menu state 8: erase the right-side panel of the inventory one row at a time, replacing
 * the existing tiles with blank 0x207f to clear space for the bottle pop-up.  Increments the
 * row counter each frame; once 19 rows have been wiped, advances to state 9 and seeds the
 * expand counter at 17 so the box can grow back from the bottom up. */
void Hud_InitBottleMenu() {  // 8de002
  int r = bottle_menu_expand_row;
  uint16 *dst = uvram_screen.row[0].col + (kNewStyleInventory ? 1 : 0);
  for (int i = 21; i <= 30; i++)
    dst[HUDXY(i, 11 + r)] = 0x207f;

  if (++bottle_menu_expand_row == 19) {
    overworld_map_state++;
    bottle_menu_expand_row = 17;
  }
  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
}

/* HUD menu state 9: animate the bottle pop-up box growing upward.  Each frame draws one new
 * top edge row (kBottleMenuTop / kBottleMenuTop2) and a fixed bottom edge (kBottleMenuBottom),
 * decrementing bottle_menu_expand_row.  When the counter wraps negative, advances to state 10
 * (the interactive bottle picker). */
void Hud_ExpandBottleMenu() {  // 8de08c
  static const uint16 kBottleMenuTop[] = { 0x28FB, 0x28F9, 0x28F9, 0x28F9, 0x28F9, 0x28F9, 0x28F9, 0x28F9, 0x28F9, 0x68FB };
  static const uint16 kBottleMenuTop2[] = { 0x28FC, 0x24F5, 0x24F5, 0x24F5, 0x24F5, 0x24F5, 0x24F5, 0x24F5, 0x24F5, 0x68FC };
  static const uint16 kBottleMenuBottom[] = { 0xA8FB, 0xA8F9, 0xA8F9, 0xA8F9, 0xA8F9, 0xA8F9, 0xA8F9, 0xA8F9, 0xA8F9, 0xE8FB };

  int r = bottle_menu_expand_row;
  uint16 *dst = uvram_screen.row[0].col + (kNewStyleInventory ? 1 : 0);
  memcpy(&dst[HUDXY(21, 11 + r)], kBottleMenuTop, sizeof(uint16) * 10);
  memcpy(&dst[HUDXY(21, 12 + r)], kBottleMenuTop2, sizeof(uint16) * 10);
  memcpy(&dst[HUDXY(21, 29)], kBottleMenuBottom, sizeof(uint16) * 10);

  if (sign8(--bottle_menu_expand_row))
    overworld_map_state++;
  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
}

/* HUD menu state 10: interactive bottle picker.
 *   - Start exits the menu (state 5).
 *   - Left/Right move the cursor between adjacent inventory items, leaving the bottle pop-up
 *     and transitioning to state 11 (Hud_EraseBottleMenu) so the regular grid is restored.
 *   - Up/Down cycle through the four bottle slots, skipping empty bottles, and update
 *     link_item_bottle_index when the player picks a different bottle.
 *   - Hud_DrawBottleMenu_Update() repaints the four bottle thumbnails and contents. */
void Hud_BottleMenu() {  // 8de0df
  timer_for_flashing_circle++;
  if (filtered_joypad_H & kJoypadH_Start) {
    sound_effect_2 = 18;
    overworld_map_state = 5;
  } else if (filtered_joypad_H & (kJoypadH_Left | kJoypadH_Right)) {
    if (filtered_joypad_H & kJoypadH_Left) {
      Hud_EquipPrevItem(&hud_cur_item);
    } else {
      Hud_EquipNextItem(&hud_cur_item);
    }
    timer_for_flashing_circle = 16;
    sound_effect_2 = 32;
    Hud_DrawYButtonItems();
    Hud_DrawSelectedYButtonItem();
    overworld_map_state++;
    bottle_menu_expand_row = 0;
    return;
  }
  Hud_DrawBottleMenu_Update();
  if (filtered_joypad_H & (kJoypadH_Down | kJoypadH_Up)) {
    uint8 old_val = link_item_bottle_index - 1, val = old_val;

    if (filtered_joypad_H & kJoypadH_Up) {
      do {
        val = (val - 1) & 3;
      } while (!link_bottle_info[val]);
    } else {
      do {
        val = (val + 1) & 3;
      } while (!link_bottle_info[val]);
    }
    if (old_val != val) {
      link_item_bottle_index = val + 1;
      timer_for_flashing_circle = 16;
      sound_effect_2 = 32;
    }
  }
}

/* Splat one ItemBoxGfx (a 2×2 tile block) into the BG3 tilemap at the given destination word
 * pointer.  Top row goes to dst[0..1] and bottom row to dst[32..33] (one tilemap row stride).
 * "new" marker means this helper was added by the C port; the original assembly inlined this. */
static void Hud_DrawItem(uint16 *dst, const ItemBoxGfx *src) {  // new
  dst[0] = src->v[0];
  dst[1] = src->v[1];
  dst[32] = src->v[2];
  dst[33] = src->v[3];
}

/* Copy a w-wide × h-tall block of tile words from `src` (packed) into the BG3 tilemap at
 * `dst`, advancing one full tilemap row (32 words) per source row.  Used to stamp panel
 * decorations such as the ability text and pendant background. */
static void Hud_DrawNxN(uint16 *dst, const uint16 *src, int w, int h) {  // new
  for (int y = 0; y < h; y++) {
    memcpy(dst, src, sizeof(uint16) * w);
    dst += 32, src += w;
  }
}

/* Copy a 2×2 tile block from one location in the BG3 tilemap to another (both source and
 * destination use the 32-word row stride).  Used by the selected-item readout to mirror the
 * already-drawn inventory icon up next to the item label. */
static void Hud_Copy2x2(uint16 *dst, uint16 *src) {  // new
  dst[0] = src[0];
  dst[1] = src[1];
  dst[32] = src[32];
  dst[33] = src[33];
}

/* Draw a rectangular outlined frame from (x1,y1) to (x2,y2) inclusive, in the given BG3
 * palette.  Uses tile 0x20fb for the four corners (with X/Y flip flags 0x4000/0x8000/0xc000
 * for the right-side, bottom, and bottom-right corners), 0x20fc for the vertical edges, 0x20f9
 * for the horizontal edges, and 0x207f for the interior fill.  Reused for every box in the
 * pause menu (Y-button area, ability box, equipment box, item label box, etc.). */
static void Hud_DrawBox(uint16 *dst, int x1, int y1, int x2, int y2, uint8 palette) {  // new
  uint16 t;

  t = 0x20fb | palette << 10;
  dst[HUDXY(x1, y1)] = t;
  dst[HUDXY(x2, y1)] = t + 0x4000;
  dst[HUDXY(x1, y2)] = t + 0x8000;
  dst[HUDXY(x2, y2)] = t + 0xc000;

  t = 0x20fc | palette << 10;
  for (int y = y1 + 1; y < y2; y++) {
    dst[HUDXY(x1, y)] = t;
    dst[HUDXY(x2, y)] = t + 0x4000;
  }

  t = 0x20f9 | palette << 10;
  for (int x = x1 + 1; x < x2; x++) {
    dst[HUDXY(x, y1)] = t;
    dst[HUDXY(x, y2)] = t + 0x8000;
  }

  for (int y = y1 + 1; y < y2; y++) {
    for (int x = x1 + 1; x < x2; x++)
      dst[HUDXY(x, y)] = 0x24F5;
  }
}

static void Hud_NewSettingsMenu_QueueUpload() {
  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
}

static void Hud_NewSettingsMenu_FinishClose() {
  Hud_Rebuild();
  overworld_map_state = 0;
  submodule_index = 0;
  main_module_index = saved_module_for_menu;
}

typedef enum NewSettingsPage {
  kNewSettingsPage_Home,
  kNewSettingsPage_General,
  kNewSettingsPage_Graphics,
  kNewSettingsPage_Sound,
  kNewSettingsPage_Features,
  kNewSettingsPage_FeatureControls,
  kNewSettingsPage_FeatureHud,
  kNewSettingsPage_FeatureGameplay,
  kNewSettingsPage_Keymap,
  kNewSettingsPage_KeymapCheats,
  kNewSettingsPage_Gamepad,
} NewSettingsPage;

typedef struct NewSettingsFeatureRow {
  const char *label;
  const char *ini_key;
  uint32 feature_bit;
} NewSettingsFeatureRow;

typedef struct NewSettingsHudRow {
  const char *label;
  const char *ini_key;
  int16 *x;
  int16 *y;
  uint8 w;
  uint8 h;
} NewSettingsHudRow;

enum {
  kNewSettingsControlBindingCount = 12,
  kNewSettingsInputBindingCount = 13,
  kNewSettingsCheatBindingCount = 13,
  kNewSettingsInputValueLen = 32,
};

static NewSettingsPage g_new_settings_page;
static NewSettingsPage g_new_settings_back_page;
static uint8 g_new_settings_cursor;
static uint8 g_new_settings_hud_column;
static uint8 g_new_settings_dirty;
static uint8 g_new_settings_aspect_ratio;
static bool g_new_settings_extend_y;
static bool g_new_settings_aspect_dirty;
static uint8 g_new_settings_save_slot = 1;
static uint8 g_new_settings_load_slot = 1;
static char g_new_settings_keymap_values[kNewSettingsInputBindingCount][kNewSettingsInputValueLen];
static char g_new_settings_gamepad_values[kNewSettingsInputBindingCount][kNewSettingsInputValueLen];
static char g_new_settings_cheat_values[kNewSettingsCheatBindingCount][kNewSettingsInputValueLen];

static const char *const kNewSettingsInputLabels[kNewSettingsInputBindingCount] = {
  "UP", "DOWN", "LEFT", "RIGHT", "SELECT", "START", "A", "B", "X", "Y", "L", "R", "SETTINGS",
};

static const char *const kNewSettingsDefaultKeymap[kNewSettingsInputBindingCount] = {
  "Up", "Down", "Left", "Right", "Right Shift", "Return", "x", "z", "s", "a", "c", "v", "",
};

static const char *const kNewSettingsDefaultGamepad[kNewSettingsInputBindingCount] = {
  "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Back", "Start", "B", "A", "Y", "X", "Lb", "Rb", "L2",
};

static const char *const kNewSettingsCheatLabels[kNewSettingsCheatBindingCount] = {
  "CHEAT LIFE", "CHEAT KEYS", "WALK WALLS", "CLEAR LOG", "STOP REPLAY", "FULLSCREEN",
  "RESET", "PAUSE", "PAUSE DIM", "TURBO", "REPLAY TURBO", "WIN BIGGER", "WIN SMALLER",
};

static const char *const kNewSettingsCheatKeys[kNewSettingsCheatBindingCount] = {
  "CheatLife", "CheatKeys", "CheatWalkThroughWalls", "ClearKeyLog", "StopReplay", "Fullscreen",
  "Reset", "Pause", "PauseDimmed", "Turbo", "ReplayTurbo", "WindowBigger", "WindowSmaller",
};

static const char *const kNewSettingsDefaultCheats[kNewSettingsCheatBindingCount] = {
  "w", "o", "Ctrl+e", "k", "l", "Alt+Return", "Ctrl+r", "Shift+p", "p", "Tab", "t", "Ctrl+Up", "Ctrl+Down",
};

static const NewSettingsFeatureRow kNewSettingsControlFeatures[] = {
  { "ITEMSWITCHLR", "ItemSwitchLR", kFeatures0_SwitchLR },
  { "ITEMSWITCHLR LIMIT", "ItemSwitchLRLimit", kFeatures0_SwitchLRLimit },
};

static const NewSettingsFeatureRow kNewSettingsRearrangeHudFeature =
  { "REARRANGE HUD", "RearrangeHUD", kFeatures0_RearrangeHud };

static const NewSettingsFeatureRow kNewSettingsGameplayFeatures[] = {
  { "TURN WHILE DASHING", "TurnWhileDashing", kFeatures0_TurnWhileDashing },
  { "MIRROR TO DARK WORLD", "MirrorToDarkworld", kFeatures0_MirrorToDarkworld },
  { "COLLECT ITEMS WITH SWORD", "CollectItemsWithSword", kFeatures0_CollectItemsWithSword },
  { "BREAK POTS WITH SWORD", "BreakPotsWithSword", kFeatures0_BreakPotsWithSword },
  { "DISABLE LOW HEALTH BEEP", "DisableLowHealthBeep", kFeatures0_DisableLowHealthBeep },
  { "SKIP INTRO ON KEYPRESS", "SkipIntroOnKeypress", kFeatures0_SkipIntroOnKeypress },
  { "SHOW MAX ITEMS YELLOW", "ShowMaxItemsInYellow", kFeatures0_ShowMaxItemsInYellow },
  { "MORE ACTIVE BOMBS", "MoreActiveBombs", kFeatures0_MoreActiveBombs },
  { "CARRY MORE RUPEES", "CarryMoreRupees", kFeatures0_CarryMoreRupees },
  { "MISC BUG FIXES", "MiscBugFixes", kFeatures0_MiscBugFixes },
  { "GAMEPLAY BUG FIXES", "GameChangingBugFixes", kFeatures0_GameChangingBugFixes },
  { "CANCEL BIRD TRAVEL", "CancelBirdTravel", kFeatures0_CancelBirdTravel },
};

static NewSettingsHudRow kNewSettingsHudRows[] = {
  { "MAGIC FRAME", "HUDMagicFramePosition", &g_config.hud_magic_frame_pos_x, &g_config.hud_magic_frame_pos_y, 3, 6 },
  { "MAGIC METER", "HUDMagicMeterPosition", &g_config.hud_magic_meter_pos_x, &g_config.hud_magic_meter_pos_y, 3, 5 },
  { "Y ITEM BOX", "HUDItemBoxPosition", &g_config.hud_item_box_pos_x, &g_config.hud_item_box_pos_y, 4, 4 },
  { "Y ITEM ICON", "HUDItemIconPosition", &g_config.hud_item_icon_pos_x, &g_config.hud_item_icon_pos_y, 2, 2 },
  { "X ITEM BOX", "HUDItemXBoxPosition", &g_config.hud_item_x_box_pos_x, &g_config.hud_item_x_box_pos_y, 4, 4 },
  { "X ITEM ICON", "HUDItemXIconPosition", &g_config.hud_item_x_icon_pos_x, &g_config.hud_item_x_icon_pos_y, 2, 2 },
  { "L ITEM BOX", "HUDItemLBoxPosition", &g_config.hud_item_l_box_pos_x, &g_config.hud_item_l_box_pos_y, 4, 4 },
  { "L ITEM ICON", "HUDItemLIconPosition", &g_config.hud_item_l_icon_pos_x, &g_config.hud_item_l_icon_pos_y, 2, 2 },
  { "R ITEM BOX", "HUDItemRBoxPosition", &g_config.hud_item_r_box_pos_x, &g_config.hud_item_r_box_pos_y, 4, 4 },
  { "R ITEM ICON", "HUDItemRIconPosition", &g_config.hud_item_r_icon_pos_x, &g_config.hud_item_r_icon_pos_y, 2, 2 },
  { "RUPEE BG", "HUDRupeesBackdropPosition", &g_config.hud_rupees_bg_pos_x, &g_config.hud_rupees_bg_pos_y, 5, 2 },
  { "RUPEES", "HUDRupeesPosition", &g_config.hud_rupees_pos_x, &g_config.hud_rupees_pos_y, 4, 1 },
  { "BOMB BG", "HUDBombsBackdropPosition", &g_config.hud_bombs_bg_pos_x, &g_config.hud_bombs_bg_pos_y, 2, 2 },
  { "BOMBS", "HUDBombsPosition", &g_config.hud_bombs_pos_x, &g_config.hud_bombs_pos_y, 2, 1 },
  { "ARROW BG", "HUDArrowsBackdropPosition", &g_config.hud_arrows_bg_pos_x, &g_config.hud_arrows_bg_pos_y, 2, 2 },
  { "ARROW UP BG", "HUDArrowUpgradeBackdropPosition", &g_config.hud_arrow_upgrade_bg_pos_x, &g_config.hud_arrow_upgrade_bg_pos_y, 2, 1 },
  { "ARROWS", "HUDArrowsPosition", &g_config.hud_arrows_pos_x, &g_config.hud_arrows_pos_y, 2, 1 },
  { "KEY BG", "HUDKeysBackdropPosition", &g_config.hud_keys_bg_pos_x, &g_config.hud_keys_bg_pos_y, 1, 2 },
  { "KEYS", "HUDKeysPosition", &g_config.hud_keys_pos_x, &g_config.hud_keys_pos_y, 1, 1 },
  { "FLOOR IND", "HUDFloorIndicatorPosition", &g_config.hud_floor_indicator_pos_x, &g_config.hud_floor_indicator_pos_y, 2, 2 },
  { "HEART FRAME", "HUDHeartsFramePosition", &g_config.hud_hearts_frame_pos_x, &g_config.hud_hearts_frame_pos_y, 12, 5 },
  { "HEART METER", "HUDHeartMeterPosition", &g_config.hud_hearts_pos_x, &g_config.hud_hearts_pos_y, 10, 2 },
};

static char *NewSettings_Trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
    *--end = 0;
  return s;
}

static bool NewSettings_ReadIniValue(const char *section, const char *key, char *out, size_t out_size) {
  size_t len = 0;
  char *file = (char *)ReadWholeFile("zelda3.ini", &len);
  if (!file)
    return false;

  char section_header[64];
  snprintf(section_header, sizeof(section_header), "[%s]", section);
  char *section_start = strstr(file, section_header);
  if (!section_start) {
    free(file);
    return false;
  }
  char *section_end = strstr(section_start + strlen(section_header), "\n[");
  if (!section_end)
    section_end = file + len;
  char *line = strchr(section_start, '\n');
  line = line ? line + 1 : section_start;
  for (char *p = line; p < section_end;) {
    char *next = strchr(p, '\n');
    if (!next || next > section_end)
      next = section_end;
    char *name = p;
    while (name < next && (*name == ' ' || *name == '\t'))
      name++;
    char *eq = memchr(name, '=', next - name);
    if (eq) {
      char *name_end = eq;
      while (name_end > name && (name_end[-1] == ' ' || name_end[-1] == '\t'))
        name_end--;
      size_t key_len = strlen(key);
      if ((size_t)(name_end - name) == key_len && !strncasecmp(name, key, key_len)) {
        size_t value_len = next - eq - 1;
        if (value_len >= out_size)
          value_len = out_size - 1;
        memcpy(out, eq + 1, value_len);
        out[value_len] = 0;
        char *trimmed = NewSettings_Trim(out);
        if (trimmed != out)
          memmove(out, trimmed, strlen(trimmed) + 1);
        free(file);
        return true;
      }
    }
    p = (*next == '\n') ? next + 1 : next;
  }
  free(file);
  return false;
}

static void NewSettings_CopyBinding(char dst[kNewSettingsInputValueLen], const char *value) {
  snprintf(dst, kNewSettingsInputValueLen, "%s", value);
}

static void NewSettings_LoadCsvBindings(const char *section, char values[kNewSettingsInputBindingCount][kNewSettingsInputValueLen],
                                        const char *const defaults[kNewSettingsInputBindingCount]) {
  for (int i = 0; i < kNewSettingsInputBindingCount; i++)
    NewSettings_CopyBinding(values[i], defaults[i]);

  char tmp[512];
  if (NewSettings_ReadIniValue(section, "Controls", tmp, sizeof(tmp))) {
    char *p = tmp;
    for (int i = 0; i < kNewSettingsControlBindingCount; i++) {
      char *item = NextDelim(&p, ',');
      if (!item)
        break;
      NewSettings_CopyBinding(values[i], NewSettings_Trim(item));
    }
  }
  if (NewSettings_ReadIniValue(section, "NewSettingsMenu", tmp, sizeof(tmp)))
    NewSettings_CopyBinding(values[kNewSettingsControlBindingCount], NewSettings_Trim(tmp));
}

static void NewSettings_LoadInputBindings() {
  NewSettings_LoadCsvBindings("KeyMap", g_new_settings_keymap_values, kNewSettingsDefaultKeymap);
  NewSettings_LoadCsvBindings("GamepadMap", g_new_settings_gamepad_values, kNewSettingsDefaultGamepad);
  for (int i = 0; i < kNewSettingsCheatBindingCount; i++) {
    char tmp[kNewSettingsInputValueLen];
    NewSettings_CopyBinding(g_new_settings_cheat_values[i], kNewSettingsDefaultCheats[i]);
    if (NewSettings_ReadIniValue("KeyMap", kNewSettingsCheatKeys[i], tmp, sizeof(tmp)))
      NewSettings_CopyBinding(g_new_settings_cheat_values[i], NewSettings_Trim(tmp));
  }
}

static void NewSettings_WriteIniValue(const char *section, const char *key, const char *value) {
  size_t len = 0;
  char *file = (char *)ReadWholeFile("zelda3.ini", &len);
  if (!file)
    return;

  char section_header[64];
  snprintf(section_header, sizeof(section_header), "[%s]", section);
  char *section_start = strstr(file, section_header);
  if (!section_start) {
    FILE *f = fopen("zelda3.ini", "ab");
    if (f) {
      fprintf(f, "\n[%s]\n%s = %s\n", section, key, value);
      fclose(f);
    }
    free(file);
    return;
  }

  char *section_end = strstr(section_start + strlen(section_header), "\n[");
  if (!section_end)
    section_end = file + len;

  char *line = strchr(section_start, '\n');
  line = line ? line + 1 : section_start;
  char *replace_start = NULL, *replace_end = NULL;
  for (char *p = line; p < section_end;) {
    char *next = strchr(p, '\n');
    if (!next || next > section_end)
      next = section_end;
    char *name = p;
    while (name < next && (*name == ' ' || *name == '\t'))
      name++;
    char *eq = memchr(name, '=', next - name);
    if (eq) {
      char *name_end = eq;
      while (name_end > name && (name_end[-1] == ' ' || name_end[-1] == '\t'))
        name_end--;
      size_t key_len = strlen(key);
      if ((size_t)(name_end - name) == key_len && !strncasecmp(name, key, key_len)) {
        replace_start = p;
        replace_end = next;
        if (replace_end < file + len && *replace_end == '\n')
          replace_end++;
        break;
      }
    }
    p = (*next == '\n') ? next + 1 : next;
  }

  ByteArray out = {0};
  char newline[160];
  int n = snprintf(newline, sizeof(newline), "%s = %s\n", key, value);
  if (replace_start) {
    ByteArray_AppendData(&out, (uint8 *)file, replace_start - file);
    ByteArray_AppendData(&out, (uint8 *)newline, n);
    ByteArray_AppendData(&out, (uint8 *)replace_end, file + len - replace_end);
  } else {
    ByteArray_AppendData(&out, (uint8 *)file, section_end - file);
    if (section_end > file && section_end[-1] != '\n')
      ByteArray_AppendByte(&out, '\n');
    ByteArray_AppendData(&out, (uint8 *)newline, n);
    ByteArray_AppendData(&out, (uint8 *)section_end, file + len - section_end);
  }

  FILE *f = fopen("zelda3.ini", "wb");
  if (f) {
    fwrite(out.data, 1, out.size, f);
    fclose(f);
  }
  ByteArray_Destroy(&out);
  free(file);
}

static void NewSettings_WriteBool(const char *section, const char *key, bool value) {
  NewSettings_WriteIniValue(section, key, value ? "1" : "0");
}

static void NewSettings_WriteInt(const char *section, const char *key, int value) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%d", value);
  NewSettings_WriteIniValue(section, key, tmp);
}

static void NewSettings_WritePosition(const char *key, int x, int y) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%d,%d", x, y);
  NewSettings_WriteIniValue("Features", key, tmp);
}

static void NewSettings_WriteInputBindings(const char *section, char values[kNewSettingsInputBindingCount][kNewSettingsInputValueLen]) {
  char tmp[512] = {0};
  for (int i = 0; i < kNewSettingsControlBindingCount; i++) {
    if (i != 0)
      snprintf(tmp + strlen(tmp), sizeof(tmp) - strlen(tmp), ", ");
    snprintf(tmp + strlen(tmp), sizeof(tmp) - strlen(tmp), "%s", values[i]);
  }
  NewSettings_WriteIniValue(section, "Controls", tmp);
  NewSettings_WriteIniValue(section, "NewSettingsMenu", values[kNewSettingsControlBindingCount]);
}

static void NewSettings_WriteCheatBinding(int index) {
  NewSettings_WriteIniValue("KeyMap", kNewSettingsCheatKeys[index], g_new_settings_cheat_values[index]);
}

static void NewSettings_InitStagedAspect() {
  g_new_settings_aspect_ratio = g_config.extended_aspect_ratio;
  g_new_settings_extend_y = g_config.extend_y;
  g_new_settings_aspect_dirty = false;
}

static int NewSettings_AspectIndex(uint8 aspect_ratio) {
  if (aspect_ratio == 0)
    return 0;
  if (aspect_ratio <= 67)
    return 1;
  if (aspect_ratio <= 90)
    return 2;
  return 3;
}

static const char *NewSettings_AspectLabel(uint8 aspect_ratio) {
  static const char *const labels[] = { "4:3", "16:10", "16:9", "18:9" };
  return labels[NewSettings_AspectIndex(aspect_ratio)];
}

static void NewSettings_WriteAspectValue(uint8 aspect_ratio, bool extend_y) {
  const char *aspect = NewSettings_AspectLabel(aspect_ratio);
  char tmp[80];
  snprintf(tmp, sizeof(tmp), "%s%s", extend_y ? "extend_y, " : "", aspect);
  NewSettings_WriteIniValue("General", "ExtendedAspectRatio", tmp);
}

static void NewSettings_SetAspect(int step) {
  static const uint8 values[] = { 0, 51, 71, 96 };
  int idx = NewSettings_AspectIndex(g_new_settings_aspect_ratio);
  idx = (idx + step + 4) & 3;
  g_new_settings_aspect_ratio = values[idx];
  g_new_settings_aspect_dirty = true;
  NewSettings_WriteAspectValue(g_new_settings_aspect_ratio, g_new_settings_extend_y);
}

static void NewSettings_ToggleFeature(const NewSettingsFeatureRow *row) {
  g_config.features0 ^= row->feature_bit;
  enhanced_features0 = g_config.features0;
  g_wanted_zelda_features = g_config.features0;
  NewSettings_WriteBool("Features", row->ini_key, (g_config.features0 & row->feature_bit) != 0);
  Hud_Rebuild();
}

static void NewSettings_UploadValueGlyphTiles() {
#define PV(a0,a1,a2,a3,a4,a5,a6,a7)  ((a0 & 1) << 7 | (a0 >> 1 & 1) << 15 | (a1 & 1) << 6 | (a1 >> 1 & 1) << 14 | (a2 & 1) <<5 | (a2 >> 1&1) <<13 | (a3 & 1) << 4 | (a3>> 1 & 1) << 12 | (a4 & 1) << 3 | (a4 >> 1 & 1) << 11 | (a5 & 1) << 2 | (a5 >> 1 & 1) << 10 | (a6 & 1) << 1 | (a6 >> 1 & 1) << 9 | (a7 & 1) << 0 | (a7 >> 1 & 1) << 8)
  enum {
    kNewSettingsColonTile = 0x14,
    kNewSettingsDigitTile = 0x15,
    kNewSettingsCursorTile = 0x1f,
  };
#define B 3
#define W 2
  static const uint16 kValueTiles[11][8] = {
    {
      PV(B,B,B,B,B,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,B,B,B,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,B,B,B,B,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,W,W,W,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,B,W,W,B,B,B),
      PV(B,B,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,B,B,B),
      PV(B,W,B,B,B,B,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,W,W,W,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,B,B,B),
      PV(B,W,B,B,B,B,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,B,B,B),
      PV(B,W,B,B,B,B,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,W,B,B,B),
      PV(B,B,B,W,B,B,B,B),
      PV(B,B,W,B,B,B,B,B),
      PV(B,B,W,B,B,B,B,B),
      PV(B,B,W,B,B,B,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
    {
      PV(B,W,W,W,W,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,B,B,B,B,W,B,B),
      PV(B,W,W,W,W,W,B,B),
      PV(B,B,B,B,B,B,B,B),
    },
  };
  memcpy(&g_zenv.vram[0x7000 + kNewSettingsColonTile * 8], kValueTiles[0], sizeof(kValueTiles[0]));
  for (int i = 0; i < 10; i++)
    memcpy(&g_zenv.vram[0x7000 + (kNewSettingsDigitTile + i) * 8], kValueTiles[i + 1], sizeof(kValueTiles[i + 1]));

  static const uint16 kCursorTile[8] = {
    PV(B,B,B,B,B,B,B,B),
    PV(B,2,B,B,B,B,B,B),
    PV(B,2,2,B,B,B,B,B),
    PV(B,2,2,2,B,B,B,B),
    PV(B,2,2,2,2,B,B,B),
    PV(B,2,2,2,B,B,B,B),
    PV(B,2,2,B,B,B,B,B),
    PV(B,2,B,B,B,B,B,B),
  };
  memcpy(&g_zenv.vram[0x7000 + kNewSettingsCursorTile * 8], kCursorTile, sizeof(kCursorTile));
#undef W
#undef B
#undef PV
}

static void NewSettings_DrawChar(uint16 *dst, int x, int y, char c) {
  uint16 tile = 0x24f5;
  if (c >= 'a' && c <= 'z')
    c -= 32;
  if (c == ' ')
    tile = 0x24f5;
  else if (c >= 'A' && c <= 'Z')
    tile = 0x2550 + c - 'A';
  else if (c >= '0' && c <= '9')
    tile = 0x2400 | (0x15 + c - '0');
  else if (c == ':')
    tile = 0x2400 | 0x14;
  else if (c == '<')
    tile = 0x24ff;
  else if (c == '>')
    tile = 0x3400 | 0x1f;
  if ((uint)x < 32 && (uint)y < 32)
    dst[HUDXY(x, y)] = tile;
}

static void NewSettings_DrawText(uint16 *dst, int x, int y, const char *s) {
  for (; *s && x < 31; s++, x++)
    NewSettings_DrawChar(dst, x, y, *s);
}

static void NewSettings_DrawTextClipped(uint16 *dst, int x, int y, const char *s, int last_x) {
  for (; *s && x <= last_x && x < 31; s++, x++)
    NewSettings_DrawChar(dst, x, y, *s);
}

static void NewSettings_DrawNumber(uint16 *dst, int x, int y, int value) {
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%d", value);
  NewSettings_DrawText(dst, x, y, tmp);
}

static void NewSettings_DrawRow(uint16 *dst, int row, bool selected, const char *label, const char *value) {
  int y = 8 + row * 2;
  NewSettings_DrawText(dst, 3, y, selected ? ">" : " ");
  NewSettings_DrawText(dst, 5, y, label);
  if (value)
    NewSettings_DrawText(dst, 23, y, value);
}

static void NewSettings_DrawWideRow(uint16 *dst, int row, bool selected, const char *label, const char *value) {
  int y = 8 + row * 2;
  NewSettings_DrawText(dst, 1, y, selected ? ">" : " ");
  NewSettings_DrawTextClipped(dst, 3, y, label, value ? 26 : 30);
  if (value)
    NewSettings_DrawText(dst, 28, y, value);
}

static void NewSettings_DrawBoolRow(uint16 *dst, int row, bool selected, const char *label, bool value) {
  NewSettings_DrawRow(dst, row, selected, label, value ? "ON" : "OFF");
}

static void NewSettings_DrawShell(const char *title) {
  uint16 *dst = uvram_screen.row[0].col;
  NewSettings_UploadValueGlyphTiles();
  for (int i = 0; i < 1024; i++)
    dst[i] = 0x207f;
  Hud_DrawBox(dst, 0, 5, 31, 29, 4);
  NewSettings_DrawText(dst, 3, 6, title);
}

static void NewSettings_DrawHome() {
  static const char *items[] = { "GENERAL", "GRAPHICS", "SOUND", "FEATURES", "KEYMAP", "GAMEPAD" };
  NewSettings_DrawShell("NEW SETTINGS");
  for (int i = 0; i < countof(items); i++)
    NewSettings_DrawRow(uvram_screen.row[0].col, i, g_new_settings_cursor == i, items[i], NULL);
  NewSettings_DrawText(uvram_screen.row[0].col, 3, 8 + 6 * 2, g_new_settings_cursor == 6 && g_new_settings_hud_column == 0 ? ">" : " ");
  NewSettings_DrawText(uvram_screen.row[0].col, 5, 8 + 6 * 2, "SAVE STATE");
  NewSettings_DrawText(uvram_screen.row[0].col, 17, 8 + 6 * 2, ":");
  NewSettings_DrawText(uvram_screen.row[0].col, 19, 8 + 6 * 2, g_new_settings_cursor == 6 && g_new_settings_hud_column == 1 ? ">" : " ");
  NewSettings_DrawNumber(uvram_screen.row[0].col, 21, 8 + 6 * 2, g_new_settings_save_slot);
  NewSettings_DrawText(uvram_screen.row[0].col, 3, 8 + 7 * 2, g_new_settings_cursor == 7 && g_new_settings_hud_column == 0 ? ">" : " ");
  NewSettings_DrawText(uvram_screen.row[0].col, 5, 8 + 7 * 2, "LOAD STATE");
  NewSettings_DrawText(uvram_screen.row[0].col, 17, 8 + 7 * 2, ":");
  NewSettings_DrawText(uvram_screen.row[0].col, 19, 8 + 7 * 2, g_new_settings_cursor == 7 && g_new_settings_hud_column == 1 ? ">" : " ");
  NewSettings_DrawNumber(uvram_screen.row[0].col, 21, 8 + 7 * 2, g_new_settings_load_slot);
}

static void NewSettings_DrawGeneral() {
  NewSettings_DrawShell("GENERAL");
  NewSettings_DrawRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK TO HOME", NULL);
  NewSettings_DrawRow(uvram_screen.row[0].col, 1, g_new_settings_cursor == 1, "ASPECT RATIO", NewSettings_AspectLabel(g_new_settings_aspect_ratio));
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 2, g_new_settings_cursor == 2, "EXTEND Y", g_new_settings_extend_y);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 3, g_new_settings_cursor == 3, "AUTOSAVE", g_config.autosave);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 4, g_new_settings_cursor == 4, "PERF TITLE", g_config.display_perf_title);
  NewSettings_DrawText(uvram_screen.row[0].col, 3, 18, "REOPEN TO APPLY");
}

static void NewSettings_DrawGraphics() {
  NewSettings_DrawShell("GRAPHICS");
  NewSettings_DrawRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK TO HOME", NULL);
  NewSettings_DrawRow(uvram_screen.row[0].col, 1, g_new_settings_cursor == 1, "WINDOW SCALE", NULL);
  NewSettings_DrawNumber(uvram_screen.row[0].col, 25, 10, g_config.window_scale);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 2, g_new_settings_cursor == 2, "NEW RENDERER", g_config.new_renderer);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 3, g_new_settings_cursor == 3, "ENH MODE7", g_config.enhanced_mode7);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 4, g_new_settings_cursor == 4, "IGNORE ASPECT", g_config.ignore_aspect_ratio);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 5, g_new_settings_cursor == 5, "NO SPR LIMIT", g_config.no_sprite_limits);
}

static void NewSettings_DrawSound() {
  NewSettings_DrawShell("SOUND");
  NewSettings_DrawRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK TO HOME", NULL);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 1, g_new_settings_cursor == 1, "AUDIO", g_config.enable_audio);
  NewSettings_DrawRow(uvram_screen.row[0].col, 2, g_new_settings_cursor == 2, "MSU VOLUME", NULL);
  NewSettings_DrawNumber(uvram_screen.row[0].col, 25, 12, g_config.msuvolume);
  NewSettings_DrawBoolRow(uvram_screen.row[0].col, 3, g_new_settings_cursor == 3, "RESUME MSU", g_config.resume_msu);
}

static void NewSettings_DrawFeatures() {
  static const char *items[] = {
    "BACK TO HOME", "CONTROL ENHANCE", "REARRANGE HUD", "HUD SHADOW", "HUD POSITIONS", "GAMEPLAY ENHANCE",
  };
  NewSettings_DrawShell("FEATURES");
  for (int i = 0; i < countof(items); i++) {
    if (i == 2) {
      bool enabled = (g_config.features0 & kFeatures0_RearrangeHud) != 0;
      NewSettings_DrawBoolRow(uvram_screen.row[0].col, i, g_new_settings_cursor == i, items[i], enabled);
      continue;
    }
    NewSettings_DrawRow(uvram_screen.row[0].col, i, g_new_settings_cursor == i, items[i], NULL);
    if (i == 3)
      NewSettings_DrawNumber(uvram_screen.row[0].col, 25, 8 + i * 2, g_config.hud_shadow_size);
  }
}

static void NewSettings_DrawFeatureRows(const char *title, const NewSettingsFeatureRow *rows, int count) {
  NewSettings_DrawShell(title);
  NewSettings_DrawWideRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK", NULL);
  int selected = g_new_settings_cursor == 0 ? 0 : g_new_settings_cursor - 1;
  int first = selected > 9 ? selected - 9 : 0;
  for (int row = 0; row < 10 && first + row < count; row++) {
    int index = first + row;
    NewSettings_DrawWideRow(uvram_screen.row[0].col, row + 1, g_new_settings_cursor == index + 1 && g_new_settings_hud_column == 0,
                            rows[index].label, (g_config.features0 & rows[index].feature_bit) != 0 ? "ON" : "OFF");
  }
}

static void NewSettings_DrawHud() {
  NewSettings_DrawShell("HUD POSITIONS");
  NewSettings_DrawRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK", NULL);
  int first = g_new_settings_cursor > 9 ? g_new_settings_cursor - 9 : 0;
  for (int row = 0; row < 10 && first + row < countof(kNewSettingsHudRows); row++) {
    NewSettingsHudRow *item = &kNewSettingsHudRows[first + row];
    int screen_row = row + 1;
    bool selected = g_new_settings_cursor == first + row + 1;
    NewSettings_DrawRow(uvram_screen.row[0].col, screen_row, selected && g_new_settings_hud_column == 0, item->label, NULL);
    NewSettings_DrawText(uvram_screen.row[0].col, 21, 8 + screen_row * 2, selected && g_new_settings_hud_column == 1 ? ">" : " ");
    NewSettings_DrawNumber(uvram_screen.row[0].col, 22, 8 + screen_row * 2, *item->x);
    NewSettings_DrawText(uvram_screen.row[0].col, 25, 8 + screen_row * 2, selected && g_new_settings_hud_column == 2 ? ">" : " ");
    NewSettings_DrawNumber(uvram_screen.row[0].col, 26, 8 + screen_row * 2, *item->y);
  }
}

static void NewSettings_DrawPlaceholder(const char *title) {
  NewSettings_DrawShell(title);
  NewSettings_DrawRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK TO HOME", NULL);
  NewSettings_DrawText(uvram_screen.row[0].col, 5, 12, "BIND EDITOR NEXT");
}

static void NewSettings_FormatBindingForDisplay(char *dst, size_t dst_size, const char *src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
    char c = src[i];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    if (c == '+')
      c = ' ';
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')
      dst[j++] = c;
  }
  dst[j] = 0;
}

static void NewSettings_DrawInputBindings(const char *title, char values[kNewSettingsInputBindingCount][kNewSettingsInputValueLen],
                                          bool keyboard) {
  NewSettings_DrawShell(title);
  NewSettings_DrawText(uvram_screen.row[0].col, 16, 6, keyboard ? "RIGHT THEN TYPE" : "A THEN BUTTON");
  NewSettings_DrawRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK", NULL);
  int first_screen_row = 1;
  int selected = g_new_settings_cursor == 0 ? 0 : g_new_settings_cursor - 1;
  if (keyboard) {
    NewSettings_DrawRow(uvram_screen.row[0].col, 1, g_new_settings_cursor == 1, "CHEATS MODS", NULL);
    first_screen_row = 2;
    selected = g_new_settings_cursor <= 1 ? 0 : g_new_settings_cursor - 2;
  }
  int max_rows = 11 - first_screen_row;
  int first = selected > max_rows - 1 ? selected - (max_rows - 1) : 0;
  for (int row = 0; row < max_rows && first + row < kNewSettingsInputBindingCount; row++) {
    int index = first + row;
    int screen_row = row + first_screen_row;
    bool selected_row = g_new_settings_cursor == index + 1;
    if (keyboard)
      selected_row = g_new_settings_cursor == index + 2;
    char value[32];
    NewSettings_FormatBindingForDisplay(value, sizeof(value), values[index][0] ? values[index] : "NONE");
    NewSettings_DrawText(uvram_screen.row[0].col, 3, 8 + screen_row * 2, selected_row && g_new_settings_hud_column == 0 ? ">" : " ");
    NewSettings_DrawTextClipped(uvram_screen.row[0].col, 5, 8 + screen_row * 2, kNewSettingsInputLabels[index], 12);
    NewSettings_DrawText(uvram_screen.row[0].col, 13, 8 + screen_row * 2, ":");
    NewSettings_DrawText(uvram_screen.row[0].col, 15, 8 + screen_row * 2, selected_row && g_new_settings_hud_column == 1 ? ">" : " ");
    NewSettings_DrawTextClipped(uvram_screen.row[0].col, 17, 8 + screen_row * 2,
                                selected_row && g_new_settings_hud_column == 1 ? (keyboard ? "TYPE KEY" : "PRESS BTN") : value,
                                30);
  }
}

static void NewSettings_DrawCheatBindings() {
  NewSettings_DrawShell("CHEATS MODS");
  NewSettings_DrawText(uvram_screen.row[0].col, 16, 6, "RIGHT THEN TYPE");
  NewSettings_DrawRow(uvram_screen.row[0].col, 0, g_new_settings_cursor == 0, "BACK", NULL);
  int selected = g_new_settings_cursor == 0 ? 0 : g_new_settings_cursor - 1;
  int first = selected > 9 ? selected - 9 : 0;
  for (int row = 0; row < 10 && first + row < kNewSettingsCheatBindingCount; row++) {
    int index = first + row;
    int screen_row = row + 1;
    bool selected_row = g_new_settings_cursor == index + 1;
    char value[32];
    NewSettings_FormatBindingForDisplay(value, sizeof(value), g_new_settings_cheat_values[index][0] ? g_new_settings_cheat_values[index] : "NONE");
    NewSettings_DrawText(uvram_screen.row[0].col, 3, 8 + screen_row * 2, selected_row && g_new_settings_hud_column == 0 ? ">" : " ");
    NewSettings_DrawTextClipped(uvram_screen.row[0].col, 5, 8 + screen_row * 2, kNewSettingsCheatLabels[index], 16);
    NewSettings_DrawText(uvram_screen.row[0].col, 17, 8 + screen_row * 2, ":");
    NewSettings_DrawText(uvram_screen.row[0].col, 19, 8 + screen_row * 2, selected_row && g_new_settings_hud_column == 1 ? ">" : " ");
    NewSettings_DrawTextClipped(uvram_screen.row[0].col, 21, 8 + screen_row * 2,
                                selected_row && g_new_settings_hud_column == 1 ? "TYPE KEY" : value,
                                30);
  }
}

static int NewSettings_RowCount() {
  switch (g_new_settings_page) {
  case kNewSettingsPage_Home: return 8;
  case kNewSettingsPage_General: return 5;
  case kNewSettingsPage_Graphics: return 6;
  case kNewSettingsPage_Sound: return 4;
  case kNewSettingsPage_Features: return 6;
  case kNewSettingsPage_FeatureControls: return 1 + countof(kNewSettingsControlFeatures);
  case kNewSettingsPage_FeatureHud: return 1 + countof(kNewSettingsHudRows);
  case kNewSettingsPage_FeatureGameplay: return 1 + countof(kNewSettingsGameplayFeatures);
  case kNewSettingsPage_Keymap: return 2 + kNewSettingsInputBindingCount;
  case kNewSettingsPage_KeymapCheats: return 1 + kNewSettingsCheatBindingCount;
  case kNewSettingsPage_Gamepad: return 1 + kNewSettingsInputBindingCount;
  default: return 1;
  }
}

static void NewSettings_Goto(NewSettingsPage page, NewSettingsPage back) {
  g_new_settings_page = page;
  g_new_settings_back_page = back;
  g_new_settings_cursor = 0;
  g_new_settings_hud_column = 0;
  g_new_settings_dirty = 1;
}

static void NewSettings_Draw() {
  switch (g_new_settings_page) {
  case kNewSettingsPage_Home: NewSettings_DrawHome(); break;
  case kNewSettingsPage_General: NewSettings_DrawGeneral(); break;
  case kNewSettingsPage_Graphics: NewSettings_DrawGraphics(); break;
  case kNewSettingsPage_Sound: NewSettings_DrawSound(); break;
  case kNewSettingsPage_Features: NewSettings_DrawFeatures(); break;
  case kNewSettingsPage_FeatureControls:
    NewSettings_DrawFeatureRows("CONTROL ENHANCE", kNewSettingsControlFeatures, countof(kNewSettingsControlFeatures));
    break;
  case kNewSettingsPage_FeatureHud: NewSettings_DrawHud(); break;
  case kNewSettingsPage_FeatureGameplay:
    NewSettings_DrawFeatureRows("GAMEPLAY ENHANCE", kNewSettingsGameplayFeatures, countof(kNewSettingsGameplayFeatures));
    break;
  case kNewSettingsPage_Keymap: NewSettings_DrawInputBindings("KEYMAP", g_new_settings_keymap_values, true); break;
  case kNewSettingsPage_KeymapCheats: NewSettings_DrawCheatBindings(); break;
  case kNewSettingsPage_Gamepad: NewSettings_DrawInputBindings("GAMEPAD", g_new_settings_gamepad_values, false); break;
  }
  Hud_NewSettingsMenu_QueueUpload();
  g_new_settings_dirty = 0;
}

static void NewSettings_ToggleBool(const char *section, const char *key, bool *value) {
  *value = !*value;
  NewSettings_WriteBool(section, key, *value);
}

static void NewSettings_AdjustInt(const char *section, const char *key, uint8 *value, int delta, int min, int max) {
  int next = IntMax(min, IntMin(max, *value + delta));
  *value = next;
  NewSettings_WriteInt(section, key, next);
}

static void NewSettings_RunSaveLoad(int cmd, int slot);

static void NewSettings_HandleActivate() {
  switch (g_new_settings_page) {
  case kNewSettingsPage_Home:
    if (g_new_settings_cursor == 0) NewSettings_Goto(kNewSettingsPage_General, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 1) NewSettings_Goto(kNewSettingsPage_Graphics, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 2) NewSettings_Goto(kNewSettingsPage_Sound, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 3) NewSettings_Goto(kNewSettingsPage_Features, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 4) NewSettings_Goto(kNewSettingsPage_Keymap, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 5) NewSettings_Goto(kNewSettingsPage_Gamepad, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 6 && g_new_settings_hud_column == 0) NewSettings_RunSaveLoad(kSaveLoad_Save, g_new_settings_save_slot);
    else if (g_new_settings_cursor == 7 && g_new_settings_hud_column == 0) NewSettings_RunSaveLoad(kSaveLoad_Load, g_new_settings_load_slot);
    break;
  case kNewSettingsPage_Features:
    if (g_new_settings_cursor == 0) NewSettings_Goto(kNewSettingsPage_Home, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 1) NewSettings_Goto(kNewSettingsPage_FeatureControls, kNewSettingsPage_Features);
    else if (g_new_settings_cursor == 2) NewSettings_ToggleFeature(&kNewSettingsRearrangeHudFeature);
    else if (g_new_settings_cursor == 4) NewSettings_Goto(kNewSettingsPage_FeatureHud, kNewSettingsPage_Features);
    else if (g_new_settings_cursor == 5) NewSettings_Goto(kNewSettingsPage_FeatureGameplay, kNewSettingsPage_Features);
    break;
  case kNewSettingsPage_FeatureControls:
    if (g_new_settings_cursor == 0) NewSettings_Goto(kNewSettingsPage_Features, kNewSettingsPage_Home);
    else NewSettings_ToggleFeature(&kNewSettingsControlFeatures[g_new_settings_cursor - 1]);
    break;
  case kNewSettingsPage_FeatureGameplay:
    if (g_new_settings_cursor == 0) NewSettings_Goto(kNewSettingsPage_Features, kNewSettingsPage_Home);
    else NewSettings_ToggleFeature(&kNewSettingsGameplayFeatures[g_new_settings_cursor - 1]);
    break;
  case kNewSettingsPage_FeatureHud:
    if (g_new_settings_cursor == 0)
      NewSettings_Goto(kNewSettingsPage_Features, kNewSettingsPage_Home);
    break;
  case kNewSettingsPage_General:
    if (g_new_settings_cursor == 0)
      NewSettings_Goto(kNewSettingsPage_Home, kNewSettingsPage_Home);
    break;
  case kNewSettingsPage_Keymap:
    if (g_new_settings_cursor == 0)
      NewSettings_Goto(kNewSettingsPage_Home, kNewSettingsPage_Home);
    else if (g_new_settings_cursor == 1)
      NewSettings_Goto(kNewSettingsPage_KeymapCheats, kNewSettingsPage_Keymap);
    else
      g_new_settings_hud_column = 1;
    break;
  case kNewSettingsPage_KeymapCheats:
    if (g_new_settings_cursor == 0)
      NewSettings_Goto(kNewSettingsPage_Keymap, kNewSettingsPage_Home);
    else
      g_new_settings_hud_column = 1;
    break;
  case kNewSettingsPage_Gamepad:
    if (g_new_settings_cursor == 0)
      NewSettings_Goto(kNewSettingsPage_Home, kNewSettingsPage_Home);
    else
      g_new_settings_hud_column = 1;
    break;
  case kNewSettingsPage_Graphics:
  case kNewSettingsPage_Sound:
    if (g_new_settings_cursor == 0)
      NewSettings_Goto(kNewSettingsPage_Home, kNewSettingsPage_Home);
    break;
  default:
    break;
  }
  g_new_settings_dirty = 1;
}

static void NewSettings_HandleChange(int delta) {
  if (g_new_settings_page == kNewSettingsPage_General) {
    if (g_new_settings_cursor == 1) NewSettings_SetAspect(delta);
    else if (g_new_settings_cursor == 2) {
      g_new_settings_extend_y = !g_new_settings_extend_y;
      g_new_settings_aspect_dirty = true;
      NewSettings_WriteAspectValue(g_new_settings_aspect_ratio, g_new_settings_extend_y);
    } else if (g_new_settings_cursor == 3) NewSettings_ToggleBool("General", "Autosave", &g_config.autosave);
    else if (g_new_settings_cursor == 4) NewSettings_ToggleBool("General", "DisplayPerfInTitle", &g_config.display_perf_title);
  } else if (g_new_settings_page == kNewSettingsPage_Graphics) {
    if (g_new_settings_cursor == 1) {
      NewSettings_AdjustInt("Graphics", "WindowScale", &g_config.window_scale, delta, 1, 8);
      ZeldaApplyRuntimeVideoConfig();
    } else if (g_new_settings_cursor == 2) {
      NewSettings_ToggleBool("Graphics", "NewRenderer", &g_config.new_renderer);
      ZeldaApplyRuntimeVideoConfig();
    } else if (g_new_settings_cursor == 3) {
      NewSettings_ToggleBool("Graphics", "EnhancedMode7", &g_config.enhanced_mode7);
      ZeldaApplyRuntimeVideoConfig();
    } else if (g_new_settings_cursor == 4) {
      NewSettings_ToggleBool("Graphics", "IgnoreAspectRatio", &g_config.ignore_aspect_ratio);
      ZeldaApplyRuntimeVideoConfig();
    } else if (g_new_settings_cursor == 5) {
      NewSettings_ToggleBool("Graphics", "NoSpriteLimits", &g_config.no_sprite_limits);
      ZeldaApplyRuntimeVideoConfig();
    }
  } else if (g_new_settings_page == kNewSettingsPage_Sound) {
    if (g_new_settings_cursor == 1) NewSettings_ToggleBool("Sound", "EnableAudio", &g_config.enable_audio);
    else if (g_new_settings_cursor == 2) NewSettings_AdjustInt("Sound", "MSUVolume", &g_config.msuvolume, delta * 5, 0, 100);
    else if (g_new_settings_cursor == 3) NewSettings_ToggleBool("Sound", "ResumeMSU", &g_config.resume_msu);
  } else if (g_new_settings_page == kNewSettingsPage_Features) {
    if (g_new_settings_cursor == 2)
      NewSettings_ToggleFeature(&kNewSettingsRearrangeHudFeature);
    else if (g_new_settings_cursor == 3)
      NewSettings_AdjustInt("Features", "HUDShadowSize", &g_config.hud_shadow_size, delta, 0, 16);
  } else if (g_new_settings_page == kNewSettingsPage_FeatureHud && g_new_settings_cursor != 0) {
    NewSettingsHudRow *item = &kNewSettingsHudRows[g_new_settings_cursor - 1];
    *item->x = IntMax(0, IntMin(63, *item->x + delta));
    NewSettings_WritePosition(item->ini_key, *item->x, *item->y);
    Hud_Rebuild();
  }
  g_new_settings_dirty = 1;
}

static void NewSettings_AdjustHudValue(int delta) {
  if (g_new_settings_page != kNewSettingsPage_FeatureHud || g_new_settings_cursor == 0)
    return;
  NewSettingsHudRow *item = &kNewSettingsHudRows[g_new_settings_cursor - 1];
  if (g_new_settings_hud_column == 1)
    *item->x = IntMax(0, IntMin(64 - item->w, *item->x + delta));
  else if (g_new_settings_hud_column == 2)
    *item->y = IntMax(0, IntMin(30 - item->h, *item->y + delta));
  else
    return;
  NewSettings_WritePosition(item->ini_key, *item->x, *item->y);
  Hud_Rebuild();
  g_new_settings_dirty = 1;
}

static void NewSettings_AdjustStateSlot(int delta) {
  uint8 *slot = g_new_settings_cursor == 6 ? &g_new_settings_save_slot :
               g_new_settings_cursor == 7 ? &g_new_settings_load_slot : NULL;
  if (!slot)
    return;
  *slot = IntMax(1, IntMin(10, *slot + delta));
  g_new_settings_dirty = 1;
}

static void NewSettings_RunSaveLoad(int cmd, int slot) {
  ZeldaApuLock();
  SaveLoadSlot(cmd, slot - 1);
  ZeldaApuUnlock();
}

static void NewSettings_HandleInput() {
  int rows = NewSettings_RowCount();
  bool home_state_value_selected = g_new_settings_page == kNewSettingsPage_Home &&
                                   (g_new_settings_cursor == 6 || g_new_settings_cursor == 7) &&
                                   g_new_settings_hud_column != 0;
  bool hud_value_selected = g_new_settings_page == kNewSettingsPage_FeatureHud &&
                            g_new_settings_cursor != 0 && g_new_settings_hud_column != 0;
  bool key_value_selected = g_new_settings_page == kNewSettingsPage_Keymap &&
                            g_new_settings_cursor > 1 && g_new_settings_hud_column != 0;
  bool cheat_value_selected = g_new_settings_page == kNewSettingsPage_KeymapCheats &&
                              g_new_settings_cursor != 0 && g_new_settings_hud_column != 0;
  bool gamepad_value_selected = g_new_settings_page == kNewSettingsPage_Gamepad &&
                                g_new_settings_cursor != 0 && g_new_settings_hud_column != 0;
  if (filtered_joypad_H & kJoypadH_Up) {
    if (home_state_value_selected)
      NewSettings_AdjustStateSlot(1);
    else if (hud_value_selected)
      NewSettings_AdjustHudValue(-1);
    else if (key_value_selected || cheat_value_selected || gamepad_value_selected)
      g_new_settings_dirty = 1;
    else {
      g_new_settings_cursor = (g_new_settings_cursor + rows - 1) % rows;
      g_new_settings_hud_column = 0;
      g_new_settings_dirty = 1;
    }
  } else if (filtered_joypad_H & kJoypadH_Down) {
    if (home_state_value_selected)
      NewSettings_AdjustStateSlot(-1);
    else if (hud_value_selected)
      NewSettings_AdjustHudValue(1);
    else if (key_value_selected || cheat_value_selected || gamepad_value_selected)
      g_new_settings_dirty = 1;
    else {
      g_new_settings_cursor = (g_new_settings_cursor + 1) % rows;
      g_new_settings_hud_column = 0;
      g_new_settings_dirty = 1;
    }
  }

  if (g_new_settings_page == kNewSettingsPage_Home && (g_new_settings_cursor == 6 || g_new_settings_cursor == 7)) {
    if (filtered_joypad_H & kJoypadH_Left) {
      g_new_settings_hud_column = 0;
      g_new_settings_dirty = 1;
    } else if (filtered_joypad_H & kJoypadH_Right) {
      g_new_settings_hud_column = 1;
      g_new_settings_dirty = 1;
    }
  } else if ((g_new_settings_page == kNewSettingsPage_Keymap || g_new_settings_page == kNewSettingsPage_KeymapCheats ||
       g_new_settings_page == kNewSettingsPage_Gamepad) && g_new_settings_cursor != 0) {
    if (filtered_joypad_H & kJoypadH_Left) {
      g_new_settings_hud_column = 0;
      g_new_settings_dirty = 1;
    } else if (filtered_joypad_H & kJoypadH_Right) {
      g_new_settings_hud_column = 1;
      g_new_settings_dirty = 1;
    }
  } else if (g_new_settings_page == kNewSettingsPage_FeatureHud && g_new_settings_cursor != 0) {
    if (filtered_joypad_H & kJoypadH_Left) {
      if (g_new_settings_hud_column != 0)
        g_new_settings_hud_column--;
      g_new_settings_dirty = 1;
    } else if (filtered_joypad_H & kJoypadH_Right) {
      if (g_new_settings_hud_column < 2)
        g_new_settings_hud_column++;
      g_new_settings_dirty = 1;
    }
  } else {
    if (filtered_joypad_H & kJoypadH_Left)
      NewSettings_HandleChange(-1);
    else if (filtered_joypad_H & kJoypadH_Right)
      NewSettings_HandleChange(1);
  }

  if (filtered_joypad_L & kJoypadL_A)
    NewSettings_HandleActivate();
  if (filtered_joypad_H & kJoypadH_B) {
    if (home_state_value_selected || key_value_selected || cheat_value_selected || gamepad_value_selected) {
      g_new_settings_hud_column = 0;
      g_new_settings_dirty = 1;
    } else if (g_new_settings_page == kNewSettingsPage_Home)
      overworld_map_state++;
    else
      NewSettings_Goto(g_new_settings_back_page, kNewSettingsPage_Home);
  }
}

static const char *NewSettings_GamepadButtonName(int button) {
  switch (button) {
  case kGamepadBtn_A: return "A";
  case kGamepadBtn_B: return "B";
  case kGamepadBtn_X: return "X";
  case kGamepadBtn_Y: return "Y";
  case kGamepadBtn_Back: return "Back";
  case kGamepadBtn_Guide: return "Guide";
  case kGamepadBtn_Start: return "Start";
  case kGamepadBtn_L3: return "L3";
  case kGamepadBtn_R3: return "R3";
  case kGamepadBtn_L1: return "Lb";
  case kGamepadBtn_R1: return "Rb";
  case kGamepadBtn_DpadUp: return "DpadUp";
  case kGamepadBtn_DpadDown: return "DpadDown";
  case kGamepadBtn_DpadLeft: return "DpadLeft";
  case kGamepadBtn_DpadRight: return "DpadRight";
  case kGamepadBtn_L2: return "L2";
  case kGamepadBtn_R2: return "R2";
  default: return NULL;
  }
}

bool Hud_NewSettingsMenu_CaptureKey(const char *key_name) {
  if (!(main_module_index == 14 && submodule_index == 12 && overworld_map_state == 3))
    return false;
  if (g_new_settings_page == kNewSettingsPage_Keymap) {
    if (g_new_settings_cursor <= 1 || g_new_settings_hud_column != 1)
      return false;
    int index = g_new_settings_cursor - 2;
    NewSettings_CopyBinding(g_new_settings_keymap_values[index], key_name);
    NewSettings_WriteInputBindings("KeyMap", g_new_settings_keymap_values);
    g_new_settings_hud_column = 0;
    g_new_settings_dirty = 1;
    return true;
  }
  if (g_new_settings_page == kNewSettingsPage_KeymapCheats) {
    if (g_new_settings_cursor == 0 || g_new_settings_hud_column != 1)
      return false;
    int index = g_new_settings_cursor - 1;
    NewSettings_CopyBinding(g_new_settings_cheat_values[index], key_name);
    NewSettings_WriteCheatBinding(index);
    g_new_settings_hud_column = 0;
    g_new_settings_dirty = 1;
    return true;
  }
  return false;
}

bool Hud_NewSettingsMenu_CaptureGamepadButton(int button) {
  if (!(main_module_index == 14 && submodule_index == 12 && overworld_map_state == 3))
    return false;
  if (g_new_settings_page == kNewSettingsPage_Keymap || g_new_settings_page == kNewSettingsPage_KeymapCheats)
    return true;
  if (g_new_settings_page != kNewSettingsPage_Gamepad || g_new_settings_cursor == 0 || g_new_settings_hud_column != 1)
    return false;
  const char *name = NewSettings_GamepadButtonName(button);
  if (!name)
    return false;
  int index = g_new_settings_cursor - 1;
  NewSettings_CopyBinding(g_new_settings_gamepad_values[index], name);
  NewSettings_WriteInputBindings("GamepadMap", g_new_settings_gamepad_values);
  g_new_settings_hud_column = 0;
  g_new_settings_dirty = 1;
  return true;
}

bool Hud_NewSettingsMenu_BlocksGamepadInput() {
  return main_module_index == 14 && submodule_index == 12 && overworld_map_state == 3 &&
         (g_new_settings_page == kNewSettingsPage_Keymap ||
          g_new_settings_page == kNewSettingsPage_KeymapCheats ||
          (g_new_settings_page == kNewSettingsPage_Gamepad && g_new_settings_hud_column == 1));
}

bool Hud_NewSettingsMenuWantsHudPreview() {
  return main_module_index == 14 && submodule_index == 12 && overworld_map_state == 3 &&
         g_new_settings_page == kNewSettingsPage_FeatureHud;
}

static void Hud_NewSettingsMenu_DrawBlank() {
  g_new_settings_page = kNewSettingsPage_Home;
  g_new_settings_back_page = kNewSettingsPage_Home;
  g_new_settings_cursor = 0;
  g_new_settings_hud_column = 0;
  g_new_settings_dirty = 1;
  NewSettings_InitStagedAspect();
  NewSettings_LoadInputBindings();
  NewSettings_Draw();
}

void Hud_NewSettingsMenu_Run() {
  byte_7E0206++;

  switch (overworld_map_state) {
  case 0:
    Hud_ClearTileMap();
    break;
  case 1:
    Hud_NewSettingsMenu_DrawBlank();
    overworld_map_state++;
    break;
  case 2:
    BG3VOFS_copy2 -= 8;
    if (BG3VOFS_copy2 == 0xff18)
      overworld_map_state++;
    break;
  case 3:
    if (filtered_joypad_H & kJoypadH_Start) {
      sound_effect_2 = 18;
      overworld_map_state++;
    } else {
      NewSettings_HandleInput();
      if (g_new_settings_dirty)
        NewSettings_Draw();
    }
    break;
  case 4:
    if (BG3VOFS_copy2 == 0) {
      Hud_NewSettingsMenu_FinishClose();
      break;
    }
    BG3VOFS_copy2 += 8;
    if (BG3VOFS_copy2 == 0)
      Hud_NewSettingsMenu_FinishClose();
    break;
  default:
    assert(0);
  }
}

bool Hud_ToggleNewSettingsMenu() {
  if (main_module_index == 14 && submodule_index == 12) {
    if (overworld_map_state != 4) {
      sound_effect_2 = 18;
      overworld_map_state = 4;
    }
    return true;
  }

  if (main_module_index != 7 && main_module_index != 9)
    return false;
  if (submodule_index != 0 || overworld_map_state != 0)
    return false;
  if (flag_custom_spell_anim_active || flag_is_link_immobilized || flag_block_link_menu)
    return false;

  BG3VOFS_copy2 = 0;
  overworld_map_state = 0;
  submodule_index = 12;
  saved_module_for_menu = main_module_index;
  main_module_index = 14;
  return true;
}

/* Draw the 4×4 ring of border tiles surrounding a 2×2 item icon, used as the cursor halo for
 * the currently selected item.  Uses two glyphs (0x2061 corner and 0x2070 edge plus their
 * X/Y-flipped variants encoded via 0x4000/0x8000/0xc000) and applies the requested BG3 palette
 * via the high bits.  Toggled on/off every few frames by Hud_NormalMenu / Hud_BottleMenu using
 * the timer_for_flashing_circle counter to produce the blinking effect. */
static void Hud_DrawFlashingCircle(uint16 *p, uint8 palette) {  // new
  int pp = palette << 10;
  p[HUDXY(0, -1)] = pp | 0x2061;
  p[HUDXY(1, -1)] = pp | 0x2061 | 0x4000;
  p[HUDXY(-1, 0)] = pp | 0x2070;
  p[HUDXY(2, 0)] = pp | 0x2070 | 0x4000;
  p[HUDXY(-1, 1)] = pp | 0xa070;
  p[HUDXY(2, 1)] = pp | 0xa070 | 0x4000;
  p[HUDXY(0, 2)] = pp | 0xa061;
  p[HUDXY(1, 2)] = pp | 0xa061 | 0x4000;
  p[HUDXY(-1, -1)] = pp | 0x2060;
  p[HUDXY(2, -1)] = pp | 0x2060 | 0x4000;
  p[HUDXY(2, 2)] = pp | 0x2060 | 0xC000;
  p[HUDXY(-1, 2)] = pp | 0x2060 | 0x8000;
}

/* Render the bottle pop-up sub-menu: a frame from (21,11) to (30,29) containing four bottle
 * icons stacked vertically (one per slot) at column 25, with the active bottle's content also
 * mirrored into the main inventory grid at slot 15.  When the flashing-circle timer is in its
 * "on" half (bit 4), draws the cursor halo around the currently picked bottle. */
void Hud_DrawBottleMenu() {  // 8def67
  uint16 *dst = uvram_screen.row[0].col + (kNewStyleInventory ? 1 : 0);
  Hud_DrawBox(dst, 21, 11, 30, 29, 2);
  for (int i = 0; i < 4; i++)
    Hud_DrawItem(dst + HUDXY(25, 13 + i * 4), &kHudItemBottles[link_bottle_info[i]]);
  const ItemBoxGfx *p = &kHudItemBottles[link_bottle_info[link_item_bottle_index - 1]];
  Hud_DrawItem(uvram_screen.row[0].col + kHudItemInVramPtr[15], p);
  if (timer_for_flashing_circle & 0x10)
    Hud_DrawFlashingCircle(dst + HUDXY(25, 13 + (link_item_bottle_index - 1) * 4), 7);
}


/* Per-frame update for the bottle pop-up: redraws the bottle icons / cursor halo and the
 * selected-item label, then schedules the NMI tilemap upload so the changes are visible next
 * V-Blank. */
void Hud_DrawBottleMenu_Update() {  // 8de17f
  Hud_DrawBottleMenu();
  Hud_DrawSelectedYButtonItem();

  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
}

/* HUD menu state 11: animate the bottle pop-up shrinking back into the right margin by erasing
 * one row per frame from top to bottom (replacing tiles with blank 0x207f).  After all 19 rows
 * are cleared, advances to state 12 (Hud_RestoreNormalMenu). */
void Hud_EraseBottleMenu() {  // 8de2fd
  uint16 *dst = uvram_screen.row[0].col + (kNewStyleInventory ? 1 : 0);
  int r = bottle_menu_expand_row;
  for (int i = 0; i < 10; i++)
    dst[HUDXY(21 + i, 11 + r)] = 0x207f;
  if (++bottle_menu_expand_row == 19)
    overworld_map_state++;
  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
}

/* HUD menu state 12: redraw the dungeon progress icons and the equipment box that the bottle
 * pop-up was occupying, then return the state machine to the normal interactive inventory
 * loop (state 4). */
void Hud_RestoreNormalMenu() {  // 8de346
  Hud_DrawProgressIcons();
  Hud_DrawEquipmentBox();
  overworld_map_state = 4;
  nmi_subroutine_index = 1;
  BYTE(nmi_load_target_addr) = 0x22;
}

/* Validate hud_cur_item against the player's actual inventory.  If the player owns no items at
 * all, clears the cursor; otherwise, ensures hud_cur_item points at an owned item — defaulting
 * to slot 1, then walking forward via Hud_EquipNextItem if the previously selected item has
 * since been lost (e.g. an empty bottle was used up). */
void Hud_SearchForEquippedItem() {  // 8de399
  if (!Hud_HaveAnyItems()) {
    hud_cur_item = 0;
    hud_var1 = 0;
  } else {
    if (!hud_cur_item)
      hud_cur_item = 1;
    if (!Hud_DoWeHaveThisItem(hud_cur_item))
      Hud_EquipNextItem(&hud_cur_item);
  }
}

/* Resolve a HUD inventory slot index to the ItemBoxGfx pointer for that slot's current
 * graphical state.  Special-cases:
 *   - i ≤ 0 → empty placeholder.
 *   - bottle slots (Bottle1..Bottle4) → indexed by link_bottle_info[i] for the contents.
 *   - Shovel in new layout → flute icon table indexed by link_item_flute >= 1.
 *   - Bombs → boolean (owned vs not owned), since the bomb icon does not have upgrade tiers.
 *   - Old-style single bottle slot → uses link_item_bottle_index to pick which bottle to show.
 * For everything else, indexes the per-item graphics table by the byte at &link_item_bow[i-1]
 * (which holds the upgrade level for that item). */
static const ItemBoxGfx *Hud_GetIconForItem(int i) {
  if (i <= 0)
    return kHudItemEmpty;

  if (i >= kHudItem_Bottle1)
    return &kHudItemBottles[link_bottle_info[i - kHudItem_Bottle1]];
  if (i == kHudItem_Shovel && kNewStyleInventory)
    return &kHudItemFlute[link_item_flute >= 1];

  uint8 item_val = (&link_item_bow)[i - 1];
  if (i == kHudItem_Bombs) // bombs
    item_val = (item_val != 0);
  else if (i == kHudItem_BottleOld && !kNewStyleInventory)
    item_val = link_item_bottle_index ? link_bottle_info[link_item_bottle_index - 1] : 0;
  return &kHudItemBoxGfxPtrs[i - 1][item_val];
}

/* Patch the on-the-fly L/R button indicator tiles directly into VRAM at $7000 + tile*8.  When
 * the player selects the L button (switch_lr == 2) or R button (switch_lr == 3) as the active
 * item slot, this routine assembles two 8-line bitplane-packed tiles (top and bottom of the
 * letter) using the PV() macro and uploads them so the equipment-letter graphic on the menu
 * shows "L" or "R" instead of leftover tile data.  PV(p0..p7) packs eight 2-bpp pixel rows into
 * one 16-bit SNES bitplane word: each pixel index 0..3 is split into its low and high bits and
 * placed in the correct slot for SNES 2-bit-per-pixel planar tile format.  No-op for switch_lr
 * 0 (Y) and 1 (X) which use static glyphs already present in VRAM. */
static void CopyTilesForSwitchLR(int switch_lr) {
#define PV(a0,a1,a2,a3,a4,a5,a6,a7)  ((a0 & 1) << 7 | (a0 >> 1 & 1) << 15 | (a1 & 1) << 6 | (a1 >> 1 & 1) << 14 | (a2 & 1) <<5 | (a2 >> 1&1) <<13 | (a3 & 1) << 4 | (a3>> 1 & 1) << 12 | (a4 & 1) << 3 | (a4 >> 1 & 1) << 11 | (a5 & 1) << 2 | (a5 >> 1 & 1) << 10 | (a6 & 1) << 1 | (a6 >> 1 & 1) << 9 | (a7 & 1) << 0 | (a7 >> 1 & 1) << 8) 
    
  if (switch_lr == 3) {
    static const uint16 kBytesForNewTile0xC_TopOfR[8] = {
      PV(1,1,1,1,1,1,3,3),
      PV(1,1,1,1,1,1,1,3),
      PV(1,1,1,1,1,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,1,1,1,1,3)
    };
    memcpy(&g_zenv.vram[0x7000 + 0xc * 8], kBytesForNewTile0xC_TopOfR, sizeof(kBytesForNewTile0xC_TopOfR));

    static const uint16 kBytesForNewTile0xD_BottomofR[8] = {
      PV(1,1,1,1,1,1,3,3),
      PV(1,1,1,3,1,1,1,3),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1),
      PV(1,1,1,3,3,1,1,1)
    };
    memcpy(&g_zenv.vram[0x7000 + 0xd * 8], kBytesForNewTile0xD_BottomofR, sizeof(kBytesForNewTile0xD_BottomofR));
  } else if (switch_lr == 2) {
    static const uint16 kBytesForNewTile0xE_TopOfL[8] = {
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3)
    };
    memcpy(&g_zenv.vram[0x7000 + 0xe * 8], kBytesForNewTile0xE_TopOfL, sizeof(kBytesForNewTile0xE_TopOfL));

    static const uint16 kBytesForNewTile0xF_BottomofL[8] = {
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,3,3,3,3,3),
      PV(1,1,1,1,1,1,1,1),
      PV(1,1,1,1,1,1,1,1),
      PV(1,1,1,1,1,1,1,1)
    };
    memcpy(&g_zenv.vram[0x7000 + 0xf * 8], kBytesForNewTile0xF_BottomofL, sizeof(kBytesForNewTile0xF_BottomofL));
  }
#undef PV
}

/* BG3 palette indices used for the box highlight color of each equip-button slot:
 * Y → palette 7 (yellow), X → palette 3 (red), L → palette 4 (green), R → palette 4 (green).
 * Indexed by GetCurrentItemButtonIndex() to color the inventory frame and selected-item box. */
static const uint8 kSwitchLR_palettes[] = { 7, 3, 4, 4 };

/* Draw the main inventory grid panel: an outlined box (rows 5-19) holding a 5- or 6-column
 * grid of item icons.  Steps:
 *   1. Re-pack the L/R glyph tiles into VRAM in case the active button changed.
 *   2. Draw the surrounding box in the appropriate palette for the active button.
 *   3. Stamp the equipment letter (Y/X/L/R) into rows 6-7 of the legacy layout.
 *   4. Add the small "ITEM" label above the box (tiles 0x246E/0x246F).
 *   5. Loop kHudItemCount times, drawing each slot's icon either from the custom
 *      hud_inventory_order[] permutation or from the default 1-based item ID. */
void Hud_DrawYButtonItems() {  // 8de3d9
  uint16 *dst = uvram_screen.row[0].col;
  int x = kNewStyleInventory ? 0 : 1;

  int btn_index = GetCurrentItemButtonIndex();
  CopyTilesForSwitchLR(btn_index);
  Hud_DrawBox(dst, x, 5, 20 - x, 19, kSwitchLR_palettes[btn_index]);
  static const uint16 kEquipmentLetterTiles[4][2] = {
    {0x3CF0, 0x3CF1},  // Y
    {0x2CF0, 0x2CF0 | 0x8000},  // X
    {0x200E | 4 << 10, 0x200F | 4 << 10},  // L
    {0x200C | 4 << 10, 0x200D | 4 << 10}   // R
   };

  if (!kNewStyleInventory) {
    dst[HUDXY(2, 6)] = kEquipmentLetterTiles[btn_index][0];
    dst[HUDXY(2, 7)] = kEquipmentLetterTiles[btn_index][1]; 
  }
  dst[HUDXY(x + 2, 5)] = 0x246E;
  dst[HUDXY(x + 3, 5)] = 0x246F;

  for (int i = 0; i < kHudItemCount; i++) {
    int j = hud_inventory_order[i];
    Hud_DrawItem(dst + kHudItemInVramPtr[i], Hud_GetIconForItem(j == 0 ? i + 1: j) );
  }
}

/* Draw the lower-left "ability" panel that shows which Link abilities (lift, read, swim,
 * pull, talk, dash) are unlocked, plus the equipped boots/gloves/flippers/moon-pearl icons.
 * The two embedded text tables (kHudAbilityText / kHudGlovesText) hold the 5×2 tile labels for
 * each ability glyph; bits in link_ability_flags select which labels are stamped into the
 * grid.  The "A" button glyph is shown in the legacy layout to indicate this is the action
 * panel, with "DO" text rendered above the slot.  Finally, the four equipment icons are drawn
 * by indexing the per-item tables with the player's current upgrade levels. */
void Hud_DrawAbilityBox() {  // 8de6b6
  static const uint16 kHudAbilityText[80] = {
    0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2d5b, 0x2d58, 0x2d55, 0x2d63, 0x2d27,
    0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2d61, 0x2d54, 0x2d50, 0x2d53,
    0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2d63, 0x2d50, 0x2d5b, 0x2d5a,
    0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f,
    0x2cf5, 0x2cf5, 0x2c2e, 0x2cf5, 0x2cf5, 0x2d5f, 0x2d64, 0x2d5b, 0x2d5b, 0x2cf5,
    0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2d61, 0x2d64, 0x2d5d, 0x2cf5,
    0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2d62, 0x2d66, 0x2d58, 0x2d5c,
    0x2cf5, 0x2cf5, 0x2cf5, 0x207f, 0x207f, 0x2c01, 0x2c18, 0x2c28, 0x207f, 0x207f,
  };
  static const uint16 kHudGlovesText[20] = {
    0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2d5b, 0x2d58, 0x2d55, 0x2d63, 0x2d28,
    0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2cf5, 0x2d5b, 0x2d58, 0x2d55, 0x2d63, 0x2d29,
  };
  uint16 *dst = uvram_screen.row[0].col;
  int x = kNewStyleInventory ? 0 : 1;

  Hud_DrawBox(dst, x, 21, 19, 29, 1);

  uint8 flags = link_ability_flags;
  for (int i = 0; i < 2; i++, flags <<= 1) {
    for (int j = 0; j < 3; j++, flags <<= 1) {
      if (flags & 0x80)
        Hud_DrawNxN(dst + HUDXY(4 + j * 5, 22 + i * 2), kHudAbilityText + i * 40 + j * 10, 5, 2);
    }
  }
  // A
  if (!kNewStyleInventory) {
    dst[HUDXY(2, 22)] = 0xA4F0;
    dst[HUDXY(2, 23)] = 0x24F2;
  }
  // DO text
  dst[HUDXY(x + 2, 21)] = 0x2482;
  dst[HUDXY(x + 3, 21)] = 0x2483;

  Hud_DrawItem(dst + HUDXY(8, 27), &kHudItemGloves[link_item_gloves]);
  Hud_DrawItem(dst + HUDXY(4, 27), &kHudItemBoots[link_item_boots]);
  Hud_DrawItem(dst + HUDXY(12, 27), &kHudItemFlippers[link_item_flippers]);
  Hud_DrawItem(dst + HUDXY(16, 27), &kHudItemMoonPearl[link_item_moon_pearl]);
  if (link_item_gloves)
    Hud_DrawNxN(dst + HUDXY(4, 22), kHudGlovesText + (link_item_gloves != 1) * 10, 5, 2);
}

/* Dispatch to the correct progress-icon panel based on game progress.  Pendants of Virtue are
 * shown for sram_progress_indicator < 3 (light-world act); after the player crosses into the
 * dark world the panel switches to display the seven crystals instead. */
void Hud_DrawProgressIcons() {  // 8de9c8
  if (sram_progress_indicator < 3)
    Hud_DrawProgressIcons_Pendants();
  else
    Hud_DrawProgressIcons_Crystals();
}

/* Render the three Pendants of Virtue panel.  Stamps the framed background tilemap
 * (kProgressIconPendantsBg) and overlays each pendant icon in its three positions: courage at
 * the top, power and wisdom on the lower row.  link_which_pendants is a 3-bit mask whose bits
 * 0/1/2 select between the "owned" graphic and the empty placeholder for each pendant. */
void Hud_DrawProgressIcons_Pendants() {  // 8de9d3
  static const uint16 kProgressIconPendantsBg[90] = {
    0x28fb, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x68fb,
    0x28fc, 0x2521, 0x2522, 0x2523, 0x2524, 0x253f, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x24f5, 0x213b, 0x213c, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x24f5, 0x213d, 0x213e, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x213b, 0x213c, 0x24f5, 0x24f5, 0x213b, 0x213c, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x213d, 0x213e, 0x24f5, 0x24f5, 0x213d, 0x213e, 0x24f5, 0x68fc,
    0xa8fb, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xe8fb,
  };
  static const ItemBoxGfx kHudPendants0[2] = {
    {{0x313b, 0x313c, 0x313d, 0x313e}},
    {{0x252b, 0x252c, 0x252d, 0x252e}},
  };
  static const ItemBoxGfx kHudPendants1[2] = {
    {{0x313b, 0x313c, 0x313d, 0x313e}},
    {{0x2d2b, 0x2d2c, 0x2d2d, 0x2d2e}},
  };
  static const ItemBoxGfx kHudPendants2[2] = {
    {{0x313b, 0x313c, 0x313d, 0x313e}},
    {{0x3d2b, 0x3d2c, 0x3d2d, 0x3d2e}},
  };
  uint16 *dst = uvram_screen.row[0].col + (kNewStyleInventory ? HUDXY(22, 11) : HUDXY(21, 11));
  Hud_DrawNxN(dst, kProgressIconPendantsBg, 10, 9);
  Hud_DrawItem(dst + HUDXY(4, 3), &kHudPendants0[(link_which_pendants >> 0) & 1]);
  Hud_DrawItem(dst + HUDXY(2, 6), &kHudPendants1[(link_which_pendants >> 1) & 1]);
  Hud_DrawItem(dst + HUDXY(6, 6), &kHudPendants2[(link_which_pendants >> 2) & 1]);
}

/* Render the seven crystals progress panel (post-Eastern Palace dark-world act).  Stamps the
 * framed background tilemap (kProgressIconCrystalsBg), then iterates through link_has_crystals
 * bits 0..6, drawing the small numeric crystal label tiles 0x2D44/0x2D45 at the position of
 * each owned crystal in the slot grid (Misery Mire and Turtle Rock crystals are highlighted
 * separately by the bit positions in the mask). */
void Hud_DrawProgressIcons_Crystals() {  // 8dea62
  static const uint16 kProgressIconCrystalsBg[90] = {
    0x28fb, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x28f9, 0x68fb,
    0x28fc, 0x252f, 0x2534, 0x2535, 0x2536, 0x2537, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x3146, 0x3147, 0x3146, 0x3147, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x3146, 0x3147, 0x3146, 0x3147, 0x3146, 0x3147, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x68fc,
    0x28fc, 0x24f5, 0x24f5, 0x3146, 0x3147, 0x3146, 0x3147, 0x24f5, 0x24f5, 0x68fc,
    0xa8fb, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xa8f9, 0xe8fb,
  };

  uint16 *dst = uvram_screen.row[0].col + (kNewStyleInventory ? HUDXY(22, 11) : HUDXY(21, 11));
  Hud_DrawNxN(dst, kProgressIconCrystalsBg, 10, 9);

  uint8 f = link_has_crystals;
  if (f & 1) {
    dst[HUDXY(3, 3)] = 0x2D44;
    dst[HUDXY(4, 3)] = 0x2D45;
  }
  if (f & 2) {
    dst[HUDXY(5, 3)] = 0x2D44;
    dst[HUDXY(6, 3)] = 0x2D45;
  }
  if (f & 4) {
    dst[HUDXY(2, 5)] = 0x2D44;
    dst[HUDXY(3, 5)] = 0x2D45;
  }
  if (f & 8) {
    dst[HUDXY(4, 5)] = 0x2D44;
    dst[HUDXY(5, 5)] = 0x2D45;
  }
  if (f & 16) {
    dst[HUDXY(6, 5)] = 0x2D44;
    dst[HUDXY(7, 5)] = 0x2D45;
  }
  if (f & 32) {
    dst[HUDXY(3, 7)] = 0x2D44;
    dst[HUDXY(4, 7)] = 0x2D45;
  }
  if (f & 64) {
    dst[HUDXY(5, 7)] = 0x2D44;
    dst[HUDXY(6, 7)] = 0x2D45;
  }
}

/* Draw the upper-right "selected item" readout: the box that shows the name of whichever item
 * the cursor is currently hovering over (or the active L/X/R item).  Holds a giant lookup of
 * 16-tile-wide text labels for every item in the game; the bottle, mushroom, mirror, flute,
 * and bow have their own per-state label tables (different label per upgrade level), while
 * everything else indexes the unified kHudItemText table.  When item == 0, displays "BOTTLE"
 * for the Y button or "NOT ASSIGNED" for the X/L/R buttons.  Also mirrors the icon up next to
 * the label and overlays the flashing-cursor halo when the timer's bit 4 is set. */
void Hud_DrawSelectedYButtonItem() {  // 8deb3a
  static const uint16 kHudBottlesItemText[128] = {
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x255c, 0x2564, 0x2562, 0x2557, 0x2561, 0x255e, 0x255e, 0x255c,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2551, 0x255e, 0x2563, 0x2563, 0x255b, 0x2554, 0x24f5, 0x24f5,
    0x255b, 0x2558, 0x2555, 0x2554, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x255c, 0x2554, 0x2553, 0x2558, 0x2552, 0x2558, 0x255d, 0x2554,
    0x255c, 0x2550, 0x2556, 0x2558, 0x2552, 0x24f5, 0x24f5, 0x24f5, 0x255c, 0x2554, 0x2553, 0x2558, 0x2552, 0x2558, 0x255d, 0x2554,
    0x2552, 0x2564, 0x2561, 0x2554, 0x256a, 0x2550, 0x255b, 0x255b, 0x255c, 0x2554, 0x2553, 0x2558, 0x2552, 0x2558, 0x255d, 0x2554,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2555, 0x2550, 0x2554, 0x2561, 0x2558, 0x2554, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2551, 0x2554, 0x2554, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2556, 0x255e, 0x255e, 0x2553, 0x24f5, 0x2551, 0x2554, 0x2554,
  };
  static const uint16 kHudMushroomItemText[16] = {
    0x255c, 0x2550, 0x2556, 0x2558, 0x2552, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x255f, 0x255e, 0x2566, 0x2553, 0x2554, 0x2561, 0x24f5,
  };
  static const uint16 kHudFluteItemText[32] = {
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2555, 0x255b, 0x2564, 0x2563, 0x2554, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2555, 0x255b, 0x2564, 0x2563, 0x2554, 0x24f5, 0x24f5, 0x24f5
  };
  static const uint16 kHudMirrorItemText[16] = {
    0x255c, 0x2550, 0x2556, 0x2558, 0x2552, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x255c, 0x2558, 0x2561, 0x2561, 0x255e, 0x2561
  };
  static const uint16 kHudBowItemText[48] = {
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x256b, 0x256c, 0x256e, 0x256f, 0x257c, 0x257d, 0x257e, 0x257f,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x256b, 0x256c, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5,
    0x256b, 0x256c, 0x24f5, 0x256e, 0x256f, 0x24f5, 0x24f5, 0x24f5, 0x2578, 0x2579, 0x257a, 0x257b, 0x257c, 0x257d, 0x257e, 0x257f,
  };
  static const uint16 kHudItemText[320] = {
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x256b, 0x256c, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2570, 0x2571, 0x2572, 0x2573, 0x2574, 0x2575, 0x2576, 0x2577,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2557, 0x255e, 0x255e, 0x255a, 0x2562, 0x2557, 0x255e, 0x2563,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2551, 0x255e, 0x255c, 0x2551, 0x24f5, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x255c, 0x2564, 0x2562, 0x2557, 0x2561, 0x255e, 0x255e, 0x255c,
    
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2555, 0x2558, 0x2561, 0x2554, 0x2561, 0x255e, 0x2553, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2558, 0x2552, 0x2554, 0x2561, 0x255e, 0x2553, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2551, 0x255e, 0x255c, 0x2551, 0x255e, 0x2562, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2554, 0x2563, 0x2557, 0x2554, 0x2561, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2560, 0x2564, 0x2550, 0x255a, 0x2554, 0x24f5, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x255b, 0x2550, 0x255c, 0x255f, 0x24f5, 0x24f5, 0x24f5, 0x24f5,
    0x255c, 0x2550, 0x2556, 0x2558, 0x2552, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2557, 0x2550, 0x255c, 0x255c, 0x2554, 0x2561,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2562, 0x2557, 0x255e, 0x2565, 0x2554, 0x255b, 0x24f5, 0x24f5,
    0x2400, 0x2401, 0x2402, 0x2403, 0x2404, 0x2405, 0x2406, 0x2407, 0x2408, 0x2409, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5,
    0x2551, 0x255e, 0x255e, 0x255a, 0x24f5, 0x255e, 0x2555, 0x24f5, 0x255c, 0x2564, 0x2553, 0x255e, 0x2561, 0x2550, 0x24f5, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x255c, 0x2564, 0x2562, 0x2557, 0x2561, 0x255e, 0x255e, 0x255c,
    0x2552, 0x2550, 0x255d, 0x2554, 0x24f5, 0x255e, 0x2555, 0x24f5, 0x24f5, 0x2562, 0x255e, 0x255c, 0x2550, 0x2561, 0x2558, 0x2550,
    0x2552, 0x2550, 0x255d, 0x2554, 0x24f5, 0x255e, 0x2555, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2551, 0x2568, 0x2561, 0x255d, 0x2550,
    0x255c, 0x2550, 0x2556, 0x2558, 0x2552, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x2552, 0x2550, 0x255f, 0x2554, 0x24f5,
    0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5, 0x24f5,
  };

#define L(x) (x == ' ' ? 0x24f5 : 0x2550 + x - 'A')
  const uint16 kNotAssignedItemText[16] = {
    L('N'), L('O'), L('T'), L(' '), L(' '), L(' '), L(' '), L(' '),
    L('A'), L('S'), L('S'), L('I'), L('G'), L('N'), L('E'), L('D'),
  };
#undef L

  uint16 *dst_org = uvram_screen.row[0].col;
  uint16 *dst_box = dst_org + (kNewStyleInventory ? 1 : 0);
  
  int btn_index = GetCurrentItemButtonIndex();
  int item = *GetCurrentItemButtonPtr(btn_index);
  Hud_DrawBox(dst_box, 21, 5, 21 + 9, 10, kSwitchLR_palettes[btn_index]);

  // Display either the current item or the item assigned
  // to the x, l, or r key.
  if (item != 0) {
    uint16 *p = dst_org + kHudItemInVramPtr[Hud_GetItemPosition(item)];
    Hud_Copy2x2(dst_box + HUDXY(25, 6), p);
    if (timer_for_flashing_circle & 0x10)
      Hud_DrawFlashingCircle(p, kSwitchLR_palettes[btn_index]);
  }

  const uint16 *src_p;
  if (item == kHudItem_BottleOld && !kNewStyleInventory && link_item_bottle_index) {
    src_p = &kHudBottlesItemText[(link_bottle_info[link_item_bottle_index - 1] - 1) * 16];
  } else if (item == 5 && link_item_mushroom != 1) {
    src_p = &kHudMushroomItemText[(link_item_mushroom - 2) * 16];
  } else if (item == 20 && link_item_mirror != 1) {
    src_p = &kHudMirrorItemText[(link_item_mirror - 2) * 16];
  } else if (item == 13 && link_item_flute != 1) {
    src_p = &kHudFluteItemText[(link_item_flute - 2) * 16];
  } else if (item == 1 && link_item_bow != 1) {
    src_p = &kHudBowItemText[(link_item_bow - 2) * 16];
  } else if (item >= kHudItem_Bottle1 && item <= kHudItem_Bottle4) {
    src_p = &kHudBottlesItemText[(link_bottle_info[item - kHudItem_Bottle1] - 1) * 16];
  } else if (item == kHudItem_Shovel) {
    src_p = &kHudItemText[(13 - 1) * 16];
  } else if (item == 0) {
    src_p = btn_index ? kNotAssignedItemText : &kHudItemText[(20 - 1) * 16];
  } else {
    src_p = &kHudItemText[(item - 1) * 16];
  }
  Hud_DrawNxN(dst_box + HUDXY(22, 8), src_p, 8, 2);
}

/* Draw the lower-right equipment panel showing sword/shield/armor and the current dungeon's
 * map / compass / boss item / heart pieces.  Frames the panel, draws a horizontal dotted
 * separator line, then stamps the dungeon-item label text and overlays the equipped sword,
 * shield, and armor icons.  When no dungeon is active (cur_palace_index_x2 == 0xff) the
 * dungeon-item row is cleared and the heart-piece counter is shown instead.  Inside a dungeon,
 * the boss item / dungeon map / compass icons are drawn for whichever the player has acquired
 * (gated by the per-dungeon bits in link_bigkey, link_dungeon_map, and link_compass). */
void Hud_DrawEquipmentBox() {  // 8ded29
  uint16 *dst = uvram_screen.row[0].col + (kNewStyleInventory ? 1 : 0);

  Hud_DrawBox(dst, 21, 21, 30, 29, 2);

  // Draw dotted lines
  for (int i = 0; i < 8; i++)
    dst[HUDXY(22 + i, 25)] = 0x28D7;

  static const uint16 kHudEquipmentDungeonItemText[16] = {
    0x2479, 0x247a, 0x247b, 0x247c, 0x248c, 0x24f5, 0x24f5, 0x24f5,
    0x2469, 0x246a, 0x246b, 0x246c, 0x246d, 0x246e, 0x246f, 0x24f5,
  };
  memcpy(dst + HUDXY(22, 22), &kHudEquipmentDungeonItemText[0], 8 * sizeof(uint16));
  memcpy(dst + HUDXY(22, 26), &kHudEquipmentDungeonItemText[8], 8 * sizeof(uint16));

  static const ItemBoxGfx kHudItemHeartPieces[4] = {
    {{0x2484, 0x6484, 0x2485, 0x6485}},
    {{0x24ad, 0x6484, 0x2485, 0x6485}},
    {{0x24ad, 0x6484, 0x24ae, 0x6485}},
    {{0x24ad, 0x64ad, 0x24ae, 0x6485}},
  };
  if (cur_palace_index_x2 == 0xff) {
    for (int i = 0; i < 8; i++)
      dst[HUDXY(22 + i, 26)] = 0x24F5;
    Hud_DrawItem(dst + HUDXY(25, 27), &kHudItemHeartPieces[link_heart_pieces]);
  }
  Hud_DrawItem(dst + HUDXY(22, 23), &kHudItemSword[link_sword_type == 0xff ? 0 : link_sword_type]);
  Hud_DrawItem(dst + HUDXY(25, 23), &kHudItemShield[link_shield_type]);
  Hud_DrawItem(dst + HUDXY(28, 23), &kHudItemArmor[link_armor]);

  static const ItemBoxGfx kHudItemPalaceItem[2] = {
    {{0x28d6, 0x68d6, 0x28e6, 0x28e7}},
    {{0x354b, 0x354c, 0x354d, 0x354e}},
  };
  static const ItemBoxGfx kHudItemDungeonMap[1] = {
    {{0x28de, 0x28df, 0x28ee, 0x28ef}},
  };
  static const ItemBoxGfx kHudItemDungeonCompass[1] = {
    {{0x24bf, 0x64bf, 0x2ccf, 0x6ccf}},
  };
  if (cur_palace_index_x2 != 0xff &&
     (link_bigkey << (cur_palace_index_x2 >> 1)) & 0x8000) {
    Hud_DrawItem(dst + HUDXY(28, 27), &kHudItemPalaceItem[CheckPalaceItemPosession()]);
  }
  if (cur_palace_index_x2 != 0xff &&
     (link_dungeon_map << (cur_palace_index_x2 >> 1)) & 0x8000) {
    Hud_DrawItem(dst + HUDXY(22, 27), &kHudItemDungeonMap[0]);
  }
  if (cur_palace_index_x2 != 0xff &&
     (link_compass << (cur_palace_index_x2 >> 1)) & 0x8000) {
    Hud_DrawItem(dst + HUDXY(25, 27), &kHudItemDungeonCompass[0]);
  }
}

/* Convert an unsigned integer (0..9999) into four decimal digits stored as 0x90-based tile
 * IDs in out[0..3] (thousands, hundreds, tens, ones).  Adding 0x90 maps each digit to its
 * digit-glyph tile in VRAM. */
static void Hud_IntToDecimal(unsigned int number, uint8 *out) {  // 8df0f7
  out[0] = number / 1000 + 0x90;
  out[1] = (number %= 1000) / 100 + 0x90;
  out[2] = (number %= 100) / 10 + 0x90;
  out[3] = (number % 10) + 0x90;
}

/* Cap link_health_current at link_health_capacity and clear the heart-refill queue.  Returns
 * true once both refilling and the heart-fill animation are complete (so callers know it is
 * safe to continue without further drawing).  Called from item-use code (potions, fairy
 * release, heart pickups). */
bool Hud_RefillHealth() {  // 8df128
  if (link_health_current >= link_health_capacity) {
    link_health_current = link_health_capacity;
    link_hearts_filler = 0;
    return (is_doing_heart_animation == 0);
  }
  link_hearts_filler = 160;
  return false;
}

/* Per-frame tick of the heart-refill animation.  Counts down animate_heart_refill_countdown
 * and, when it reaches zero, advances the partial-heart graphic in the HUD by one frame using
 * the kAnimHeartPartial cycle of glyphs.  When the four-step subpos cycle completes the HUD is
 * fully rebuilt and is_doing_heart_animation is cleared so the next heart can begin filling. */
void Hud_AnimateHeartRefill() {  // 8df14f
  if (--animate_heart_refill_countdown)
    return;
  uint16 n = ((uint16)((link_health_current & ~7) - 1) >> 3) << 1;
  int x = Hud_GroupX(20, g_config.hud_hearts_pos_x);
  int y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(g_config.hud_hearts_pos_y, 2) : 1;
  if (n >= 20) {
    n -= 20;
    y++;
  }
  n &= 0xff;
  animate_heart_refill_countdown = 1;

  static const uint16 kAnimHeartPartial[4] = { 0x24A3, 0x24A4, 0x24A3, 0x24A0 };
  Hud_SetTopHudTile(x + (n >> 1), y, kAnimHeartPartial[animate_heart_refill_countdown_subpos]);

  animate_heart_refill_countdown_subpos = (animate_heart_refill_countdown_subpos + 1) & 3;
  if (!animate_heart_refill_countdown_subpos) {
    Hud_Rebuild();
    is_doing_heart_animation = 0;
  }
}

/* Begin a magic-power refill: returns true immediately if the meter is already full,
 * otherwise loads link_magic_filler with 0x80 (a full bar) and returns false so the per-frame
 * Hud_RefillLogic() can drain the filler counter into link_magic_power gradually.  Called from
 * item-use code such as drinking a green/blue potion. */
bool Hud_RefillMagicPower() {  // 8df1b3
  if (link_magic_power >= 0x80)
    return true;
  link_magic_filler = 0x80;
  return false;
}

/* Re-enable the dark-room lit-by-lantern background subtraction effect after the pause menu
 * closes.  Only fires if Link is carrying a lit lantern (link_item_torch), the room wants
 * lights-out (dung_want_lights_out), the lit-room flag isn't already set, and there are no
 * other lit torches in the room.  When activated, sets the dark-with-lantern flag and toggles
 * the BG2 subscreen mask via TS_copy unless BG2 is in special tile-quad mode (==2). */
void Hud_RestoreTorchBackground() {  // 8dfa33
  if (!link_item_torch || !dung_want_lights_out || hdr_dungeon_dark_with_lantern ||
      dung_num_lit_torches)
    return;
  hdr_dungeon_dark_with_lantern = 1;
  if (dung_hdr_bg2_properties != 2)
    TS_copy = 1;
}

/* Rebuild the gameplay HUD when entering an indoor (dungeon) area.  Resets the overworld fixed
 * color blend and sets link_num_keys to 0xff (sentinel meaning "hide the key counter when not
 * applicable"), then calls Hud_Rebuild() which redraws all of BG3. */
void Hud_RebuildIndoor() {  // 8dfa60
  overworld_fixed_color_plusminus = 0;
  link_num_keys = 0xff;
  Hud_Rebuild();
}

static void Hud_UpdateItemBoxIcon(uint8 item, int default_x, int default_y,
                                  int configured_x, int configured_y) {
  if (!item)
    return;
  int x = Hud_GroupX(default_x, configured_x);
  int y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(configured_y, 2) : default_y;
  Hud_DrawTopHudBlock(x, y, Hud_GetIconForItem(item)->v, 2, 2, 2);
}

static void Hud_DrawItemBoxLabel(char label, int box_x, int box_y) {
  if (!Hud_RearrangeEnabled() || !Hud_HasAssignedSwitchItem())
    return;
  int tile = label == 'Y' ? 0x10 : label == 'X' ? 0x11 : label == 'L' ? 0x12 : 0x13;
  Hud_SetTopHudTile(box_x + 3, Hud_ClampHudY(box_y + 3), 0x2400 | tile);
}

/* Refresh the in-game item boxes.  The Y item is always shown when equipped; the X/L/R quick
 * slots are shown only when the item-switch feature is active and that slot has an assignment. */
static void Hud_UpdateItemBox() {  // 8dfafd
  if (Hud_RearrangeEnabled() && Hud_HasAssignedSwitchItem())
    Hud_UploadItemBoxLabelTiles();

  Hud_UpdateItemBoxIcon(hud_cur_item, 5, 1,
                        g_config.hud_item_icon_pos_x, g_config.hud_item_icon_pos_y);
  Hud_DrawItemBoxLabel('Y', g_config.hud_item_box_pos_x, g_config.hud_item_box_pos_y);

  if (!Hud_ItemSwitchHudEnabled() || !Hud_RearrangeEnabled())
    return;

  Hud_UpdateItemBoxIcon(hud_cur_item_x, 9, 1,
                        g_config.hud_item_x_icon_pos_x, g_config.hud_item_x_icon_pos_y);
  if (hud_cur_item_x)
    Hud_DrawItemBoxLabel('X', g_config.hud_item_x_box_pos_x, g_config.hud_item_x_box_pos_y);
  Hud_UpdateItemBoxIcon(hud_cur_item_l, 13, 1,
                        g_config.hud_item_l_icon_pos_x, g_config.hud_item_l_icon_pos_y);
  if (hud_cur_item_l)
    Hud_DrawItemBoxLabel('L', g_config.hud_item_l_box_pos_x, g_config.hud_item_l_box_pos_y);
  Hud_UpdateItemBoxIcon(hud_cur_item_r, 17, 1,
                        g_config.hud_item_r_icon_pos_x, g_config.hud_item_r_icon_pos_y);
  if (hud_cur_item_r)
    Hud_DrawItemBoxLabel('R', g_config.hud_item_r_box_pos_x, g_config.hud_item_r_box_pos_y);
}

/* Inner loop used by Hud_Update_Hearts() to stamp life-meter glyphs into the HUD tilemap.
 * `n` is the remaining health budget in eighths of a heart; each iteration consumes 8 (one
 * full heart).  After 10 hearts the destination wraps to the second meter row.  src[1]/src[2]
 * select between empty/full heart graphics based on whether more than half a row remains. */
static void Hud_UpdateHearts_Inner(int base_x, int y, const uint16 *src, int n) {  // 8dfdab
  int x = 0;
  while (n > 0) {
    if (x >= 10) {
      y++;
      x = 0;
    }
    Hud_SetTopHudTile(base_x + x, y, src[n >= 5 ? 2 : 1]);
    x++;
    n -= 8;
  }
}

/* Memcpy a w-wide × h-tall block of tile words from packed source memory into the HUD work
 * tilemap, advancing one full BG3 row (32 words) per source row.  Used to stamp the static
 * decorative pieces (left/right ornament panels) of the gameplay HUD. */
static void DrawHudComponents(uint16 *dst, const uint16 *src, int w, int h) {
  do {
    memcpy(dst, src, w * sizeof(uint16));
  } while (src += w, dst += 32, --h);
}

/* Refresh the in-game life meter row of the HUD tilemap.  Two passes:
 *   1. Fill the entire meter with empty heart glyphs (kHudItemBoxTab1) up to capacity.
 *   2. Overwrite the leading hearts with full glyphs (kHudItemBoxTab2) up to current HP,
 *      rounded to the next quarter-heart boundary by the (+3 & ~3) bias. */
static void Hud_Update_Hearts() {  // 8dfb94
  static const uint16 kHudItemBoxTab1[] = { 0x24A2, 0x24A2, 0x24A2 };
  static const uint16 kHudItemBoxTab2[] = { 0x24A2, 0x24A1, 0x24A0 };
  // The life meter
  int x = Hud_GroupX(20, g_config.hud_hearts_pos_x);
  int y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(g_config.hud_hearts_pos_y, 2) : 1;
  Hud_UpdateHearts_Inner(x, y, kHudItemBoxTab1, link_health_capacity);
  Hud_UpdateHearts_Inner(x, y, kHudItemBoxTab2, (link_health_current + 3) & ~3);
}

/* Refresh the magic-power column of the HUD tilemap.  When magic consumption is in "half"
 * mode (>=1), draws the green "1/2" indicator tiles (0x28F7/0x2851/0x28FA) above the meter to
 * show the discount.  Then writes the four magic-bar segment tiles for the current
 * link_magic_power level by indexing kUpdateMagicPowerTilemap. */
static void Hud_Update_Magic() {  // 8dfc09
  int x = Hud_GroupX(2, g_config.hud_magic_meter_pos_x);
  int y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(g_config.hud_magic_meter_pos_y, 5) : 0;
  if (link_magic_consumption >= 1) {
    if (!Hud_RearrangeEnabled()) {
      Hud_SetTopHudTile(x + 0, y + 0, 0x28F7);
      Hud_SetTopHudTile(x + 1, y + 0, 0x2851);
      Hud_SetTopHudTile(x + 2, y + 0, 0x28FA);
    }
  }
  const uint16 *src = kUpdateMagicPowerTilemap[(link_magic_power + 7) >> 3];
  Hud_SetTopHudTile(x + 1, y + 1, src[0]);
  Hud_SetTopHudTile(x + 1, y + 2, src[1]);
  Hud_SetTopHudTile(x + 1, y + 3, src[2]);
  Hud_SetTopHudTile(x + 1, y + 4, src[3]);
}

/* Refresh the per-counter readouts of the HUD: rupee count, bomb count, arrow count, key
 * count, and the bow icon's "with-arrows" state.
 *   - Each counter is rendered with Hud_IntToDecimal and stamped one digit per tile, in
 *     yellow (0x3400) when at maximum and white (0x2400) otherwise.
 *   - inv_offs shifts the rupee column over by one when the rupee count exceeds 999, so the
 *     four-digit display fits.
 *   - Switching link_item_bow between values 1↔2 and 3↔4 toggles the icon between empty- and
 *     filled-quiver graphics whenever the arrow count crosses zero.
 *   - link_num_keys == 0xff renders blank, used outside of dungeons. */
static void Hud_Update_Inventory() {  // 8dfc09
  uint8 d[4];
  Hud_IntToDecimal(link_rupees_actual, d);

  const uint16 base_tiles[2] = {
    0x2400,
    (enhanced_features0 & kFeatures0_ShowMaxItemsInYellow) ? 0x3400 : 0x2400,
  };

  int inv_offs = (d[0] == 0x90) ? 1 : 0;

  static const uint16 kHudInventoryBg[26] = {
    0x207f, 0x207f, 0x3ca8, 0x207f, 0x207f, 0x2c88, 0x2c89, 0x207f, 0x20a7, 0x20a9, 0x207f, 0x2871, 0x207f,
    0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f,
  };
  static const uint16 kHudRupeeBg[5 * 2] = {
    0x207f, 0x207f, 0x3ca8, 0x207f, 0x207f,
    0x207f, 0x207f, 0x207f, 0x207f, 0x207f,
  };
  static const uint16 kHudBombBg[2 * 2] = {
    0x2c88, 0x2c89,
    0x207f, 0x207f,
  };
  static const uint16 kHudArrowBg[2 * 2] = {
    0x20a7, 0x20a9,
    0x207f, 0x207f,
  };
  static const uint16 kHudKeyBg[1 * 2] = {
    0x2871,
    0x207f,
  };

  bool show_keys = Hud_ShouldShowDungeonKeys();
  int counters_x = 8;
  int counters_y = 0;
  if (Hud_RearrangeEnabled()) {
    Hud_DrawTopHudBlock(g_config.hud_rupees_bg_pos_x, Hud_ClampHudBlockY(g_config.hud_rupees_bg_pos_y, 2),
                        kHudRupeeBg, 5, 2, 5);
    Hud_DrawTopHudBlock(g_config.hud_bombs_bg_pos_x, Hud_ClampHudBlockY(g_config.hud_bombs_bg_pos_y, 2),
                        kHudBombBg, 2, 2, 2);
    Hud_DrawTopHudBlock(g_config.hud_arrows_bg_pos_x, Hud_ClampHudBlockY(g_config.hud_arrows_bg_pos_y, 2),
                        kHudArrowBg, 2, 2, 2);
    if (show_keys) {
      Hud_DrawTopHudBlock(g_config.hud_keys_bg_pos_x, Hud_ClampHudBlockY(g_config.hud_keys_bg_pos_y, 2),
                          kHudKeyBg, 1, 2, 1);
    } else {
      Hud_SetTopHudTile(g_config.hud_keys_bg_pos_x, Hud_ClampHudBlockY(g_config.hud_keys_bg_pos_y, 2), 0x207f);
      Hud_SetTopHudTile(g_config.hud_keys_bg_pos_x, Hud_ClampHudBlockY(g_config.hud_keys_bg_pos_y, 2) + 1, 0x207f);
    }
  } else {
    Hud_DrawTopHudBlock(counters_x, counters_y + 0, kHudInventoryBg + 0 + inv_offs, 12, 1, 12);
    Hud_DrawTopHudBlock(counters_x, counters_y + 1, kHudInventoryBg + 13 + inv_offs, 12, 1, 12);
  }

  if (link_item_bow) {
    if (link_item_bow >= 3) {
      if (Hud_RearrangeEnabled()) {
        Hud_SetTopHudTile(g_config.hud_arrow_upgrade_bg_pos_x, Hud_ClampHudBlockY(g_config.hud_arrow_upgrade_bg_pos_y, 1), 0x2486);
        Hud_SetTopHudTile(g_config.hud_arrow_upgrade_bg_pos_x + 1, Hud_ClampHudBlockY(g_config.hud_arrow_upgrade_bg_pos_y, 1), 0x2487);
      } else {
        Hud_SetTopHudTile(counters_x + 7, counters_y + 0, 0x2486);
        Hud_SetTopHudTile(counters_x + 8, counters_y + 0, 0x2487);
      }
      link_item_bow = link_num_arrows ? 4 : 3;
    } else {
      link_item_bow = link_num_arrows ? 2 : 1;
    }
  }

  // Offset everything if we have many coins?
  int base_tile = base_tiles[link_rupees_actual == MaxRupees()];
  int rupees_y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(g_config.hud_rupees_pos_y, 1) : counters_y + 1;
  if (inv_offs == 0) {
    Hud_SetTopHudTile(Hud_GroupX(counters_x + 0, g_config.hud_rupees_pos_x - 1), rupees_y, base_tile | d[0]);
  }
  Hud_SetTopHudTile(Hud_GroupX(counters_x + 1, g_config.hud_rupees_pos_x), rupees_y, base_tile | d[1]);
  Hud_SetTopHudTile(Hud_GroupX(counters_x + 2, g_config.hud_rupees_pos_x + 1), rupees_y, base_tile | d[2]);
  Hud_SetTopHudTile(Hud_GroupX(counters_x + 3, g_config.hud_rupees_pos_x + 2), rupees_y, base_tile | d[3]);

  Hud_IntToDecimal(link_item_bombs, d);
  base_tile = base_tiles[link_item_bombs == kMaxBombsForLevel[link_bomb_upgrades]];
  int bombs_y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(g_config.hud_bombs_pos_y, 1) : counters_y + 1;
  Hud_SetTopHudTile(Hud_GroupX(counters_x + 5, g_config.hud_bombs_pos_x), bombs_y, base_tile | d[2]);
  Hud_SetTopHudTile(Hud_GroupX(counters_x + 6, g_config.hud_bombs_pos_x + 1), bombs_y, base_tile | d[3]);

  Hud_IntToDecimal(link_num_arrows, d);
  base_tile = base_tiles[link_num_arrows == kMaxArrowsForLevel[link_arrow_upgrades]];
  int arrows_y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(g_config.hud_arrows_pos_y, 1) : counters_y + 1;
  Hud_SetTopHudTile(Hud_GroupX(counters_x + 8, g_config.hud_arrows_pos_x), arrows_y, base_tile | d[2]);
  Hud_SetTopHudTile(Hud_GroupX(counters_x + 9, g_config.hud_arrows_pos_x + 1), arrows_y, base_tile | d[3]);

  // Show keys
  d[3] = 0x7f;
  if (show_keys)
    Hud_IntToDecimal(link_num_keys, d);
  int key_x = Hud_GroupX(counters_x + 11, g_config.hud_keys_pos_x);
  int key_y = Hud_RearrangeEnabled() ? Hud_ClampHudBlockY(g_config.hud_keys_pos_y, 1) : counters_y + 1;
  Hud_SetTopHudTile(key_x, key_y, 0x2400 | d[3]);
  if (*Hud_TopHudTilePtr(key_x, key_y) == 0x247f)
    Hud_SetTopHudTile(key_x, key_y - 1, 0x247f);
}

/* Fully rebuild the in-game HUD tilemap (the 165 word region at the top of BG3).  Stamps the
 * left-side magic meter and item box decoration, the right-side counters/hearts decoration,
 * then refreshes hearts, magic, inventory counters and the equipped-item box from current
 * game state.  Bumps flag_update_hud_in_nmi so the next NMI uploads the new tilemap to VRAM.
 * The leading "missing word" check restores tile 0x207f to all 165 slots if a previous bug or
 * uninitialized state had wiped tile (8,2). */
void Hud_Rebuild() {  // 8dfa70
  if (Hud_RearrangeEnabled()) {
    for (int i = 0; i < countof(g_wide_hud_tilemap); i++)
      g_wide_hud_tilemap[i] = 0x207f;
  } else if (hud_tile_indices_buffer[HUDXY(8, 2)] == 0) {
    for (int i = 0; i < 165; i++)
      hud_tile_indices_buffer[i] = 0x207f;
  }


  // The magic meter and item box
  static const uint16 kHudTilemapLeftPart[8 * 6] = {
    0x207f, 0x207f, 0x2850, 0xa856, 0x2852, 0x285b, 0x285b, 0x285c,
    0x207f, 0x207f, 0x2854, 0x2871, 0x2858, 0x207f, 0x207f, 0x285d,
    0x207f, 0x207f, 0x2854, 0x304e, 0x2858, 0x207f, 0x207f, 0x285d,
    0x207f, 0x207f, 0x2854, 0x305e, 0x2859, 0xa85b, 0xa85b, 0xa85c,
    0x207f, 0x207f, 0x2854, 0x305e, 0x6854, 0x207f, 0x207f, 0x207f,
    0x207f, 0x207f, 0xa850, 0x2856, 0xe850, 
  };
  if (Hud_RearrangeEnabled()) {
    static const uint16 kHudStandaloneMagicFrame[3 * 6] = {
      0x2850, 0xa856, 0x6850,
      0x2854, 0x2871, 0x6854,
      0x2854, 0x304e, 0x6854,
      0x2854, 0x305e, 0x6854,
      0x2854, 0x305e, 0x6854,
      0xa850, 0x2856, 0xe850,
    };
    static const uint16 kHudStandaloneItemBox[4 * 4] = {
      0x685c, 0x285b, 0x285b, 0x285c,
      0x685d, 0x207f, 0x207f, 0x285d,
      0x685d, 0x207f, 0x207f, 0x285d,
      0xe85c, 0xa85b, 0xa85b, 0xa85c,
    };
    Hud_DrawTopHudBlock(g_config.hud_magic_frame_pos_x, Hud_ClampHudBlockY(g_config.hud_magic_frame_pos_y, 6),
                        kHudStandaloneMagicFrame, 3, 6, 3);
    Hud_DrawTopHudBlock(g_config.hud_item_box_pos_x, Hud_ClampHudBlockY(g_config.hud_item_box_pos_y, 4),
                        kHudStandaloneItemBox, 4, 4, 4);
    if (Hud_ItemSwitchHudEnabled()) {
      if (hud_cur_item_x)
        Hud_DrawTopHudBlock(g_config.hud_item_x_box_pos_x, Hud_ClampHudBlockY(g_config.hud_item_x_box_pos_y, 4),
                            kHudStandaloneItemBox, 4, 4, 4);
      if (hud_cur_item_l)
        Hud_DrawTopHudBlock(g_config.hud_item_l_box_pos_x, Hud_ClampHudBlockY(g_config.hud_item_l_box_pos_y, 4),
                            kHudStandaloneItemBox, 4, 4, 4);
      if (hud_cur_item_r)
        Hud_DrawTopHudBlock(g_config.hud_item_r_box_pos_x, Hud_ClampHudBlockY(g_config.hud_item_r_box_pos_y, 4),
                            kHudStandaloneItemBox, 4, 4, 4);
    }
  } else {
    DrawHudComponents(hud_tile_indices_buffer, kHudTilemapLeftPart, 8, 6);
  }

  static const uint16 kHudTilemapRightPart[12 * 5] = {
    0x207f, 0x207f, 0x288b, 0x288f, 0x24ab, 0x24ac, 0x688f, 0x688b, 0x207f, 0x207f, 0x207f, 0x207f,
    0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f,
    0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f,
    0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f,
    0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f, 0x207f,
  };
  if (Hud_RearrangeEnabled()) {
    Hud_DrawTopHudBlock(g_config.hud_hearts_frame_pos_x, Hud_ClampHudBlockY(g_config.hud_hearts_frame_pos_y, 5),
                        kHudTilemapRightPart, 12, 5, 12);
  } else {
    DrawHudComponents(&hud_tile_indices_buffer[HUDXY(20, 0)], kHudTilemapRightPart, 12, 5);
  }


  Hud_Update_Hearts();
  Hud_Update_Magic();
  Hud_Update_Inventory();
  Hud_UpdateItemBox();
  flag_update_hud_in_nmi++;
}


/* Public lookup that returns a raw pointer to the four tile words for the given item's "owned"
 * graphic.  Used by external animation code (e.g. the item-acquired pop-up) that needs to draw
 * the icon outside the standard inventory grid. */
const uint16 *Hud_GetItemBoxPtr(int item) {
  return kHudItemBoxGfxPtrs[item]->v;
}

/* Quick L/R cycling through equippable items without opening the inventory menu — only active
 * when the kFeatures0_SwitchLR enhancement flag is on.  Pressing L (with hud_cur_item_l == 0)
 * cycles backwards, R (with hud_cur_item_r == 0) cycles forwards.  Skips empty/unowned items
 * via Hud_DoWeHaveThisItem and, when kFeatures0_SwitchLRLimit is enabled, also restricts the
 * cycle to the first 4 inventory positions.  Plays sfx 32 and refreshes the item box and
 * equipped item state when a switch happens. */
void Hud_HandleItemSwitchInputs() {
  if (!(enhanced_features0 & kFeatures0_SwitchLR))
    return;
  
  bool direction;
  
  if (filtered_joypad_L & kJoypadL_L && (hud_cur_item_l == 0))
    direction = (hud_cur_item_r != 0);
  else if (filtered_joypad_L & kJoypadL_R && (hud_cur_item_r == 0))
    direction = true;
  else
    return;

  uint8 item = hud_cur_item;
  for (int i = 0; i < kHudItemCount; i++) {
    if (!direction)
      Hud_GotoPrevItem(&item, 1);
    else
      Hud_GotoNextItem(&item, 1);
    if (Hud_DoWeHaveThisItem(item) && (!(enhanced_features0 & kFeatures0_SwitchLRLimit) || Hud_GetItemPosition(item) <= 3)) {
      if (item != hud_cur_item) {
        hud_cur_item = item;
        sound_effect_2 = 32;
        Hud_UpdateEquippedItem();
        Hud_UpdateItemBox();
        flag_update_hud_in_nmi++;
      }
      break;
    }
  }
}

/* Swap the currently selected item with one of its neighbors in the hud_inventory_order[]
 * permutation, allowing the player to rearrange their inventory grid by holding Y and pressing
 * a direction.  `direction` is the signed slot offset (±1 for left/right, ±5/±6 for up/down
 * depending on layout).  On first use, initializes the permutation to the default sequential
 * order (1..24) so subsequent swaps have a baseline.  Wraps the index around the slot count
 * and plays sfx 32 after the swap. */
static void Hud_ReorderItem(int direction) {
  // Initialize inventory order on first use
  if (hud_inventory_order[0] == 0) {
    for (int i = 0; i < 24; i++)
      hud_inventory_order[i] = i + 1;
  }
  int old_pos = Hud_GetItemPosition(hud_cur_item), new_pos = old_pos + direction;
  if (new_pos < 0)
    new_pos += kHudItemCount;
  else if (new_pos >= kHudItemCount)
    new_pos -= kHudItemCount;
  uint8 t = hud_inventory_order[old_pos];
  hud_inventory_order[old_pos] = hud_inventory_order[new_pos];
  hud_inventory_order[new_pos] = t;
  Hud_DrawYButtonItems();
  sound_effect_2 = 32;
}
