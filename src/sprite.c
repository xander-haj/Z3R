/*
 * sprite.c — Core sprite subsystem framework for Zelda: A Link to the Past
 *
 * This file implements the foundational layer of the sprite system shared by all
 * 243 enemy/NPC types. It is responsible for:
 *
 *   - Per-frame sprite dispatch loop (Sprite_Main → Sprite_ExecuteSingle)
 *   - 12-state sprite state machine (inactive, fall1/2, poof, drown, explode,
 *     die, burn, initialize, active, carried, stunned)
 *   - Coordinate system: 16-bit world positions split into lo/hi bytes, with an
 *     8-bit sub-pixel accumulator giving 1/16-pixel velocity resolution
 *   - Tile collision detection (horizontal/vertical sensor probes, sloped tiles,
 *     pits, water, conveyors) for both dungeon and overworld layers
 *   - Hitbox overlap: 32 preset hitbox geometries (kSpriteHitbox_*) + Link's
 *     body and action hitboxes; central CheckIfHitBoxesOverlap test
 *   - Damage pipeline: sword damage class table (kSprite_Func14_Damage) →
 *     kEnemyDamages matrix lookup → Sprite_GiveDamage → HP deduction/stun/death
 *   - Drop/prize system: kPrizeItems rotation table, ForcePrizeDrop, PrepareEnemyDrop
 *   - Carry and throw mechanics (SpriteModule_Carried, CarriedSprite_CheckForThrow)
 *   - Stun/recoil animation (SpriteModule_Stunned, Sprite_ReturnIfRecoiling)
 *   - OAM rendering: six region-based OAM allocators, sprite draw helpers
 *     (SingleLarge, SingleSmall, ThinTall, DrawMultiple, Shadow, poof/fall/drown)
 *   - Garnish effects: 22 visual trail/effect types (fire, sparkle, water, lightning…)
 *   - Sprite loading: dungeon room lists (Dungeon_LoadSprites), overworld proximity
 *     activation (Sprite_ActivateWhenProximal), cached-sprite transitions
 *     (Dungeon_CacheTransSprites / ExecuteCachedSprites)
 *
 * The actual per-sprite AI logic for each of the 243 enemy types lives in
 * sprite_main.c (SpriteActive_Main dispatch table). This file provides all
 * the infrastructure that sprite_main.c calls into.
 *
 * Key globals used throughout:
 *   sprite_state[k]        — 0=inactive, 8=init, 9=active, 10=carried, 11=stunned
 *   sprite_type[k]         — sprite ID (0–242), indexes into kSpriteInit_* tables
 *   cur_sprite_x/y         — world coordinate snapshot used during current tick
 *   BG2HOFS_copy2/VOFS     — camera scroll position (sprite coords are world-space)
 *   oam_cur_ptr/ext_cur_ptr— current write position into the OAM DMA buffer
 */
#include "sprite.h"
#include "dungeon.h"
#include "hud.h"
#include "load_gfx.h"
#include "overworld.h"
#include "variables.h"
#include "tagalong.h"
#include "overlord.h"
#include "ancilla.h"
#include "player.h"
#include "misc.h"
#include "overlord.h"
#include "tile_detect.h"
#include "sprite_main.h"
#include "assets.h"

// Upper bounds for each of the six OAM regions. When a region's base pointer
// reaches this value, the allocator overflows and cycles through kOamGetBufferPos_Tab1.
static const uint16 kOamGetBufferPos_Tab0[6] = {0x171, 0x201, 0x31, 0xc1, 0x141, 0x1d1};
// Fallback OAM slot positions used when a region overflows. Arranged as 6 groups
// of 8 entries (one per region). The allocator cycles through these round-robin
// using oam_alloc_arr1[region]++, spreading overflow sprites across reserved slots.
static const uint16 kOamGetBufferPos_Tab1[48] = {
   0x30,  0x50,  0x80,  0xb0,  0xe0, 0x110, 0x140, 0x170, 0x1d0, 0x1d4, 0x1dc, 0x1e0, 0x1e4, 0x1ec, 0x1f0, 0x1f8,
      0,     4,     8,   0xc,  0x10,  0x14,  0x18,  0x1c,  0x30,  0x38,  0x50,  0x68,  0x80,  0x98,  0xb0,  0xc8,
  0x120, 0x124, 0x128, 0x12c, 0x130, 0x134, 0x138, 0x13c, 0x140, 0x150, 0x160, 0x170, 0x180, 0x190, 0x1a0, 0x1b8,
};
// Frame-skip masks applied to the recoil movement. Indexed by (sprite_F>>2).
// A value of 0 means move every frame; 3 means move every 4th frame (slow recoil).
static const uint8 kSprite2_ReturnIfRecoiling_Masks[6] = {3, 1, 0, 0, 0xc, 3};

// 32 hitbox presets indexed by sprite_flags4[k] & 0x1f.
// XLo/XHi together form a signed 9-bit offset added to sprite_x_lo/x_hi.
static const int8 kSpriteHitbox_XLo[32] = {
  2, 3, 0, -3, -6, 0, 2, -8, 0, -4, -8, 0, -8, -16, 2, 2,
  2, 2, 2, -8, 2, 2, -16, -8, -12, 4, -4, -12, 5, -32, -2, 4,
};
// High byte of the X hitbox offset (sign-extension byte, 0 or -1).
static const int8 kSpriteHitbox_XHi[32] = {
  0, 0, 0, -1, -1, 0, 0, -1, 0, -1, -1, 0, -1, -1, 0, 0,
  0, 0, 0, -1, 0, 0, -1, -1, -1, 0, -1, -1, 0, -1, -1, 0,
};
// Width of the hitbox in pixels for each preset.
static const uint8 kSpriteHitbox_XSize[32] = {
  12, 1, 16, 20, 20, 8, 4, 32, 48, 24, 32, 32, 32, 48, 12, 12,
  60, 124, 12, 32, 4, 12, 48, 32, 40, 8, 24, 24, 5, 80, 4, 8,
};
// Low byte of the Y hitbox offset (added to sprite_y_lo after subtracting sprite_z).
static const int8 kSpriteHitbox_YLo[32] = {
  0, 3, 4, -4, -8, 2, 0, -16, 12, -4, -8, 0, -10, -16, 2, 2,
  2, 2, -3, -12, 2, 10, 0, -12, 16, 4, -4, -12, 3, -16, -8, 10,
};
// High byte of the Y hitbox offset (sign-extension byte, 0 or -1).
static const int8 kSpriteHitbox_YHi[32] = {
  0, 0, 0, -1, -1, 0, 0, -1, 0, -1, -1, 0, -1, -1, 0, 0,
  0, 0, -1, -1, 0, 0, 0, -1, 0, 0, -1, -1, 0, -1, -1, 0,
};
// Height of the hitbox in pixels for each preset.
static const uint8 kSpriteHitbox_YSize[32] = {
  14, 1, 16, 21, 24, 4, 8, 40, 20, 24, 40, 29, 36, 48, 60, 124,
  12, 12, 17, 28, 4, 2, 28, 20, 10, 4, 24, 16, 5, 48, 8, 12,
};
// Shield-block direction helpers. Tab2[dir>>1] gives the "dash direction code"
// compared against Tab3[sprite_D] to determine if the shield is facing the sprite.
static const uint8 kSpriteDamage_Tab2[4] = {6, 4, 0, 0};
static const uint8 kSpriteDamage_Tab3[4] = {4, 6, 0, 2};

// Sound effect IDs for throwable terrain destruction. Indexed by sprite_C (terrain
// sub-type: 0=bush, 1=pot, 2=skull, 3=mushroom, 4=sign, 5=statue, 6-8=rock variants).
static const uint8 kSprite_Func21_Sfx[9] = {0x1f, 0x1f, 0x1e, 0x1e, 0x1e, 0x1f, 0x1f, 0x1f, 0x1f};

// X/Y offsets used when spawning a sparkle garnish on a stunned/frozen sprite.
// A random index 0-3 is chosen for each axis independently.
static const int8 kSparkleGarnish_XY[4] = {-4, 12, 3, 8};
// Frame-skip masks for the stun sparkle effect. Indexed by sprite_delay_main>>4.
// Higher delay values (early stun) spawn sparkles less frequently (more zeros in mask).
// At delay tier 4-6 (mask=0), a sparkle is spawned every frame.
static const uint8 kSpriteStunned_Main_Func1_Masks[7] = {0x7f, 0xf, 3, 1, 0, 0, 0};

// Wall collision bitmask for each of the 4 probe directions.
// Bits: 1=right, 2=left, 4=down, 8=up. Returned in sprite_wallcoll[k].
static const uint8 kSprite_Func7_Tab[4] = {8, 4, 2, 1};
// Tile-probe sensor offsets, indexed by j=(direction_code/2). Each entry is a
// pair (center_x_offset, probe_x_offset) and similarly for Y. The probe samples
// the tile at (sprite_x + kSprite_Func5_X[j*2], sprite_y + kSprite_Func5_Y[j*2])
// to determine tile type for collision. 27 probe directions are defined (×2 = 54).
static const int8 kSprite_Func5_X[54] = {
  8, 8, 2, 14, 8, 8, -2, 10, 8, 8, 1, 14, 4, 4, 4, 4,
  4, 4, -2, 10, 8, 8, -25, 40, 8, 8, 2, 14, 8, 8, -8, 23,
  8, 8, -20, 36, 8, 8, -1, 16, 8, 8, -1, 16, 8, 8, -8, 24,
  8, 8, -8, 24, 8, 3,
};
static const int8 kSprite_Func5_Y[54] = {
  6, 20, 13, 13, 0, 8, 4, 4, 1, 14, 8, 8, 4, 4, 4, 4,
  -2, 10, 4, 4, -25, 40, 8, 8, 3, 16, 10, 10, -8, 25, 8, 8,
  -20, 36, 8, 8, -1, 16, 8, 8, 14, 3, 8, 8, -8, 24, 8, 8,
  -8, 32, 8, 8, 12, 4,
};
// Simplified tile attribute for sprites with defl_bits&8 (simplified collision).
// Values: 0=passable, 1=solid wall, 3=water (enters sprite_E=4 in OW), 4=pit/hole.
// Indexed by the raw tile attribute byte from the dungeon or overworld map.
static const uint8 kSprite_SimplifiedTileAttr[256] = {
  0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 3, 3, 3,
  0, 0, 0, 0, 0, 0, 1, 1, 4, 4, 4, 4, 4, 4, 4, 4,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 4,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
// Full tile collision response table. Indexed by raw tile attribute byte.
//   0 = passable (no collision)
//   1 = solid wall (block movement)
//   2 = slope — triggers Entity_CheckSlopedTileCollision for precise geometry
//  -1 = hole/pit — sprite falls (state → SpriteModule_Fall or Drown)
// Other values are treated as solid by the general collision code path.
static const int8 kSprite_Func5_Tab3[256] = {
  0, 1, 2, 3, 2, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1,
  1, 1, 1, 0, 0, 0, 1, 2, -1, -1, -1, -1, -1, -1, -1, -1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1,
  0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, -1, -1, -1, -1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
// Y-threshold lookup for sloped tiles. Indexed by (tiletype-0x10)*8 + (x&7).
// The four slope types (0x10–0x13) each have 8 horizontal positions; the value
// is the Y pixel within the tile cell at which the slope boundary falls.
// Entity_CheckSlopedTileCollision compares the sprite's Y-within-cell against
// this threshold to determine solid vs. passable for that pixel column.
static const int8 kSlopedTile[32] = {
  7, 6, 5, 4, 3, 2, 1, 0,
  0, 1, 2, 3, 4, 5, 6, 7,
  0, 1, 2, 3, 4, 5, 6, 7,
  7, 6, 5, 4, 3, 2, 1, 0,
};
// Recoil timer values assigned to the sprite when Link's sword is parried by a
// guard. Indexed by GetRandomNumber()&7. Controls how far the sprite is pushed back.
static const uint8 kSprite_Func1_Tab[8] = {15, 15, 24, 15, 15, 19, 15, 15};

// Link incapacitation timer values assigned when a guard successfully parries.
// Indexed by GetRandomNumber()&7. Controls how long Link is stunned.
static const uint8 kSprite_Func1_Tab2[8] = {6, 6, 6, 12, 6, 6, 6, 15};

// Damage class index lookup for sword attacks. Indexed by (sword_tier-1)|swing_flags.
// Indices 0-3: light tap (tier 1-4). Indices 4-7: full swing (tier 1-4).
// Indices 8-11: spin-attack (tier 1-4). The result selects a column in kEnemyDamages.
static const uint8 kSprite_Func14_Damage[12] = {1, 2, 3, 4, 2, 3, 4, 5, 1, 1, 2, 3};
// Damage lookup table. Dimensions: 16 damage classes × 8 enemy damage categories.
// Indexed as kEnemyDamages[damage_type_determiner * 8 | enemy_damage_data[type*16 | deter]].
// Special values: 0=no effect (tink), 249=transmute to cukeman, 250=instant kill special,
// 251=stun (lift-ready), 252=stun (not frozen), 253=mini-moldorm stun, 254=freeze,
// 255=instant death. All other values are HP reduction amounts.
static const uint8 kEnemyDamages[128] = {
  0, 1, 32, 255, 252, 251, 0, 0, 0, 2, 64, 4, 0, 0, 0, 0,
  0, 4, 64, 2, 3, 0, 0, 0, 0, 8, 64, 4, 0, 0, 0, 0,
  0, 16, 64, 8, 0, 0, 0, 0, 0, 16, 64, 8, 0, 0, 0, 0,
  0, 4, 64, 16, 0, 0, 0, 0, 0, 255, 64, 255, 252, 251, 0, 0,
  0, 4, 64, 255, 252, 251, 32, 0, 0, 100, 24, 100, 0, 0, 0, 0,
  0, 249, 250, 255, 100, 0, 0, 0, 0, 8, 64, 253, 4, 16, 0, 0,
  0, 8, 64, 254, 4, 0, 0, 0, 0, 16, 64, 253, 0, 0, 0, 0,
  0, 254, 64, 16, 0, 0, 0, 0, 0, 32, 64, 255, 0, 0, 0, 250,
};
// Per-sprite-type initialization tables. All arrays are indexed by sprite_type[k]
// (values 0–242). Loaded by SpritePrep_LoadProperties during spawn or reset.

// OAM slot count field: bits 0-4 encode ((num_oam_entries-1)), bit 5=???,
// bit 6=use 16×16 tiles, bit 7=use 8×8 tiles. Multiplied by 4 in Sprite_TimersAndOam.
static const uint8 kSpriteInit_Flags2[243] = {
     1,    2,    1, 0x82, 0x81, 0x84, 0x84, 0x84,    2,  0xf,    2,    1, 0x20,    3,    4, 0x84,
     1,    5,    4,    1, 0x80,    4, 0xa2, 0x83,    4,    2, 0x82, 0x62, 0x82, 0x80, 0x80, 0x85,
     1, 0xa5,    3,    4,    4, 0x83,    2,    1, 0x82, 0xa2, 0xa2, 0xa3, 0xaa, 0xa3, 0xa4, 0x82,
  0x82, 0x83, 0x82, 0x80, 0x82, 0x82, 0xa5, 0x80, 0xa4, 0x82, 0x81, 0x82, 0x82, 0x82, 0x81,    6,
     8,    8,    8,    8,    6,    8,    8,    8,    6,    7,    7,    2,    2, 0x22,    1,    1,
  0x20, 0x82,    7, 0x85,  0xf, 0x21,    5, 0x83,    2,    1,    1,    1,    1,    7,    7,    7,
     7,    0, 0x85, 0x83,    3, 0xa4,    0,    0,    0,    0,    9,    4, 0xa0,    0,    1,    0,
     0,    3, 0x8b, 0x86, 0xc2, 0x82, 0x81,    4, 0x82, 0x21,    6,    3,    1,    3,    3,    3,
     0,    0,    4,    5,    5,    3,    1,    2,    0,    0,    0,    2,    7,    0,    1,    1,
  0x87,    6,    0, 0x83,    2, 0x22, 0x22, 0x22, 0x22,    4,    3,    5,    1,    1,    4,    1,
     2,    8,    8, 0x80, 0x21,    3,    3,    3,    2,    2,    8, 0x8f, 0xa1, 0x81, 0x80, 0x80,
  0x80, 0x80, 0xa1, 0x80, 0x81, 0x81, 0x86, 0x81, 0x82, 0x82, 0x80, 0x80, 0x83,    6,    0,    0,
     5,    4,    6,    5,    2,    0,    0,    5,    4,    4,    7,  0xb,  0xc,  0xc,    6,    6,
     3, 0xa4,    4, 0x82, 0x81, 0x83, 0x10, 0x10, 0x81, 0x82, 0x82, 0x82, 0x83, 0x83, 0x83, 0x81,
  0x82, 0x83, 0x83, 0x81, 0x82, 0x81, 0x82, 0xa0, 0xa1, 0xa3, 0xa1, 0xa1, 0xa1, 0x83, 0x85, 0x83,
  0x83, 0x83, 0x83,
};
// Starting HP for each sprite type. 255 means the sprite is indestructible by
// normal damage (e.g., Agahnim barrier, certain invincible NPCs).
static const uint8 kSpriteInit_Health[243] = {
   12,   6, 255,   3,  3,   3,   3,   3,   2,  12,  4, 255,   0,   3,  12,   2,
    0,  20,   4,   4,  0, 255,   0,   2,   3,   8,  0,   0,   0,   0,   0,   0,
    8,   3,   8,   2,  2,   0,   3, 255,   0,   3,  3,   3,   3,   3,   3,   3,
    3,   0,   3,   0,  3,   3,   3,   0,   3,   0,  0,   0,   0,   3,   2, 255,
    2,   6,   4,   8,  6,   8,   6,   4,   8,   8,  8,   4,   4,   2,   2,   2,
  255,   8, 255,  48, 16,   8,   8, 255,   2,   0,  0, 255, 255, 255, 255, 255,
  255, 255, 255, 255,  4,   4, 255, 255, 255, 255, 16,   3,   0,   2,   4,   1,
  255,   4, 255,   0,  0,   0,   0, 255,   0,   0, 96, 255,  24, 255, 255, 255,
    3,   4, 255,  16,  8,   8,   0, 255,  32,  32, 32,  32,  32,   8,   8,   4,
    8,  64,  48, 255,  2, 255, 255, 255, 255,  16,  4,   2,   4,   4,   8,   8,
    8,  16,  64,  64,  8,   4,   8,   4,   4,   8, 12,  16,   0,   0,   0,   0,
    0,   0,   0,   0,  0,   0,   0,   0,   0,   0,  0,   0,   0, 128,  48, 255,
  255, 255, 255,   8,  0,   0,   0,  32,   0,   8,  5,  40,  40,  40,  90,  16,
   24,  64,   0,   4,  0,   0, 255, 255,   0,   0,  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,  0,   0,   0,   0,   0,   0,  0,   0,   0,   0,   0,   0,
    0,   0,   0,
};
// Contact damage dealt to Link per sprite type. The effective heart damage is
// kPlayerDamages[3 * (bump_damage & 0xf) + link_armor]. High bit (0x80) = no
// contact damage. Bits 4-6 encode special hit class flags.
const uint8 kSpriteInit_BumpDamage[243] = {
  0x83, 0x83, 0x81,    2,    2,    2,    2,    2,    1, 0x13,    1,    1,    1,    1,    8,    1,
     1,    8,    5,    3, 0x40,    4,    0,    2,    3, 0x85,    0,    1,    0, 0x40,    0,    0,
     6,    0,    5,    3,    1,    0,    0,    3,    0,    0,    0,    0,    0,    0,    0,    0,
     0,    0,    0,    0,    0,    0,    0, 0x40,    0,    0,    0,    0,    0,    0,    2,    2,
     0,    1,    1,    3,    1,    3,    1,    1,    3,    3,    3,    1,    3,    1,    1,    1,
     1,    1,    1, 0x11, 0x14,    1,    1,    2,    5,    0,    0,    4,    4,    8,    8,    8,
     8,    4,    0,    4,    3,    2,    2,    2,    2,    2,    3,    1,    0,    0,    1, 0x80,
     5,    1,    0,    0,    0, 0x40,    0,    4,    0,    0, 0x14,    4,    6,    4,    4,    4,
     4,    3,    4,    4,    4,    1,    4,    4, 0x15,    5,    4,    5, 0x15, 0x15,    3,    5,
     0,    5, 0x15,    5,    5,    6,    6,    6,    6,    5,    3,    6,    5,    5,    3,    3,
     3,    6, 0x17, 0x15, 0x15,    5,    5,    1, 0x85, 0x83,    5,    4,    0,    0,    0,    0,
     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0x17, 0x17,    5,
     5,    5,    4,    3,    2, 0x10,    0,    6,    0,    5,    7, 0x17, 0x17, 0x17, 0x15,    7,
     6, 0x10,    0,    3,    3,    0, 0x19, 0x19,    0,    0,    0,    0,    0,    0,    0,    0,
     0,    0,    0, 0x10,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
     0,    0,    0,
};
// Palette/shadow/misc flags. Low nibble (bits 0-3) = OAM palette index (copied
// to sprite_oam_flags[k]). Bit 4 = draws ground shadow. Bit 5 = uses small shadow.
// Bit 6 = immune to sword damage. Bit 7 = dark-world sprite.
static const uint8 kSpriteInit_Flags3[243] = {
  0x19,  0xb, 0x1b, 0x4b, 0x41, 0x41, 0x41, 0x4d, 0x1d,    1, 0x1d, 0x19, 0x8d, 0x1b,    9, 0x9d,
  0x3d,    1,    9, 0x11, 0x40,    1, 0x4d, 0x19,    7, 0x1d, 0x59, 0x80, 0x4d, 0x40,    1, 0x49,
  0x1b, 0x41,    3, 0x13, 0x15, 0x41, 0x18, 0x1b, 0x41, 0x47,  0xf, 0x49, 0x4b, 0x4d, 0x41, 0x47,
  0x49, 0x4d, 0x49, 0x40, 0x4d, 0x47, 0x49, 0x41, 0x74, 0x47, 0x5b, 0x58, 0x51, 0x49, 0x1d, 0x5d,
     3, 0x19, 0x1b, 0x17, 0x19, 0x17, 0x19, 0x1b, 0x17, 0x17, 0x17, 0x1b,  0xd,    9, 0x19, 0x19,
  0x49, 0x5d, 0x5b, 0x49,  0xd,    3, 0x13, 0x41, 0x1b, 0x5b, 0x5d, 0x43, 0x43, 0x4d, 0x4d, 0x4d,
  0x4d, 0x4d, 0x49,    1,    0, 0x41, 0x4d, 0x4d, 0x4d, 0x4d, 0x1d,    9, 0xc4,  0xd,  0xd,    9,
     3,    3, 0x4b, 0x47, 0x47, 0x49, 0x49, 0x41, 0x47, 0x36, 0x8b, 0x49, 0x1d, 0x49, 0x43, 0x43,
  0x43,  0xb, 0x41,  0xd,    7,  0xb, 0x1d, 0x43,  0xd, 0x43,  0xd, 0x1d, 0x4d, 0x4d, 0x1b, 0x1b,
   0xa,  0xb,    0,    5,  0xd,    1,    1,    1,    1,  0xb,    5,    1,    1,    1,    7, 0x17,
  0x19,  0xd,  0xd, 0x80, 0x4d, 0x19, 0x17, 0x19,  0xb,    9,  0xd, 0x4a, 0x12, 0x49, 0xc3, 0xc3,
  0xc3, 0xc3, 0x76, 0x40, 0x59, 0x41, 0x58, 0x4f, 0x73, 0x5b, 0x44, 0x41, 0x51,  0xa,  0xb,  0xb,
  0x4b,    0, 0x40, 0x5b,  0xd,    0,    0,  0xd, 0x4b,  0xb, 0x59, 0x41,  0xb,  0xd,    1,  0xd,
   0xd,    0, 0x50, 0x4c, 0x44, 0x51,    1,    1, 0xf2, 0xf8, 0xf4, 0xf2, 0xd4, 0xd4, 0xd4, 0xf8,
  0xf8, 0xf4, 0xf4, 0xd8, 0xf8, 0xd8, 0xdf, 0xc8, 0x69, 0xc1, 0xd2, 0xd2, 0xdc, 0xc7, 0xc1, 0xc7,
  0xc7, 0xc7, 0xc1,
};
// Hitbox preset index (bits 0-4) and miscellaneous flags (bits 5-7).
// Bits 0-4: index into kSpriteHitbox_* tables used by Sprite_SetupHitBox.
// Bit 5 = allow off-screen (skip Sprite_KillSelf). Bit 6 = ??? Bit 7 = two-layer.
static const uint8 kSpriteInit_Flags4[243] = {
     0,    0,    0, 0x43, 0x43, 0x43, 0x43, 0x43,    0,    0,    0,    0, 0x1c,    0,    0,    2,
     1,    3,    0,    0,    3, 0xc0,    7,    0,    0,    0,    7, 0x45, 0x43,    0, 0x40,  0xd,
     0,    0,    0,    0,    0,    0,    0,    0,    7,    7,    7,    7,    7,    7,  0xd,    7,
     7,    7,    7,    3,    7,    7,    7, 0x40,    3,    7,  0xd,    0,    7,    7,    0,    0,
     9, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12,    0,    0,    0,    0,
  0x80, 0x12,    9,    9,    0, 0x40,    0,  0xc,    0,    0,    0, 0x40, 0x40, 0x10, 0x10, 0x2e,
  0x2e, 0x40, 0x1e, 0x53,    0,  0xa,    0,    0,    0,    0, 0x12, 0x12, 0x40,    0,    0, 0x40,
  0x19,    0,    0,  0xa,  0xd,  0xa,  0xa, 0x80,  0xa, 0x41,    0, 0x40,    0, 0x49,    0,    0,
  0xc0,    0, 0x40,    0,    0, 0x40,    0,    0,    9, 0x80, 0xc0,    0, 0x40,    0,    0, 0x80,
     0,    0, 0x18, 0x5a,    0, 0xd4, 0xd4, 0xd4, 0xd4,    0, 0x40,    0, 0x80, 0x80, 0x40, 0x40,
  0x40,    0,    9, 0x1d,    0,    0,    0,    0,    0,    0,    0,    0,    0,  0xa, 0x1b, 0x1b,
  0x1b, 0x1b, 0x41,    0,    3,    7,    7,    3,  0xa,    0,    1,  0xa,  0xa,    9,    0,    0,
     0,    0,    9,    0,    0, 0x40, 0x40,    0,    0,    0,    0, 0x89, 0x80, 0x80,    0, 0x1c,
     0, 0x40,    0,    0, 0x1c,    7,    3,    3, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
  0x44, 0x44, 0x44, 0x43, 0x44, 0x43, 0x40, 0xc0, 0xc0, 0xc7, 0xc3, 0xc3, 0xc0, 0x1b,    8, 0x1b,
  0x1b, 0x1b,    3,
};
// General behavior flags. Bit 0 = can fall into pits. Bit 1 = immune to shield.
// Bit 2 = reflected by shield. Bit 4 = can be lifted/thrown by Link. Bits 5-7 = ???
static const uint8 kSpriteInit_Flags[243] = {
     0,    0,    0,    0,    0,    0,    0,    0,    0,  0xa,    0,    1, 0x30,    0, 0, 0x20,
  0x10,    0,    0,    1,    0,    0,    0,    0,    0,    0,    0,    8, 0x20,    0, 4,    0,
     0,    0,    0,    0,    0,    0,    1,    4,    0,    0,    0,    0,    0,    0, 0,    0,
     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0x68,
  0x60, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,    0,    0, 0,    0,
     0,    0,    2,    2,    2,    0,    0, 0x70,    0,    0,    0, 0x90, 0x90,    0, 0,    0,
     0,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0x60, 0x60,    0,    0, 0,    0,
     0,    0,    0,    0,    0,    0,    0, 0x80,    0,    0,    2,    0,    0, 0x70, 0,    0,
     0,    0,    0,    0,    0,    0, 0xb0,    0, 0xc2,    0, 0x20,    0,    2,    0, 0,    0,
     0,    0,    2,    0, 0xb0,    0,    0,    0,    0,    0,    0,    0, 0xa0, 0xa0, 0,    0,
     0,    4,    2,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0,    0,
     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0xc2, 0,    0,
     0,    0,    0,    0,    4,    0,    0,    0,    0,    0,    0,    2,    2,    2, 2,    0,
     0,    0,    0,    0,    0,    0,  0xa,  0xa, 0x10, 0x10, 0x10, 0x10,    0,    0, 0, 0x10,
  0x10, 0x10, 0x10,    0, 0x10,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0,    0,
     0,    0,    0,
};
// Deflection and drop behavior flags. Bit 7 = ignore pause (runs during submodule).
// Bits 4-6 = item prize tier index (0=no drop). Bits 0-3 = ??? resistances.
// Values 0x80+ = pause-immune enemy classes (Agahnim bats, flying enemies, etc.).
static const uint8 kSpriteInit_Flags5[243] = {
  0x83, 0x96, 0x84, 0x80, 0x80, 0x80, 0x80, 0x80,    2,    0,    2, 0x80, 0xa0, 0x83, 0x97, 0x80,
  0x80, 0x94, 0x91,    7,    0, 0x80,    0, 0x80, 0x92, 0x96, 0x80, 0xa0,    0,    0,    0, 0x80,
     4, 0x80, 0x82,    6,    6,    0,    0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,    0,    0, 0x80, 0x80, 0x90,
  0x80, 0x91, 0x91, 0x91, 0x97, 0x91, 0x95, 0x95, 0x93, 0x97, 0x14, 0x91, 0x92, 0x81, 0x82, 0x82,
  0x80, 0x85, 0x80, 0x80, 0x80,    4,    4, 0x80, 0x91, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80,    0, 0x80, 0x80, 0x82, 0x8a, 0x80, 0x80, 0x80, 0x80, 0x92, 0x91, 0x80, 0x82, 0x81, 0x81,
  0x80, 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x97, 0x80, 0x80, 0x80,
  0x80, 0xc2, 0x80, 0x15, 0x15, 0x17,    6,    0, 0x80,    0, 0xc0, 0x13, 0x40,    0,    2,    6,
  0x10, 0x14,    0,    0, 0x40,    0,    0,    0,    0, 0x13, 0x46, 0x11, 0x80, 0x80,    0,    0,
     0, 0x10,    0,    0,    0, 0x16, 0x16, 0x16, 0x81, 0x87, 0x82,    0, 0x80, 0x80,    0,    0,
     0,    0, 0x80, 0x80,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
     0,    0,    0, 0x80,    0,    0,    0, 0x17,    0, 0x12,    0,    0,    0,    0,    0, 0x10,
  0x17,    0, 0x40,    1,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
     0,    0,    0, 0x40,    0,    0,    0,    0,    0,    0,    0,    0, 0x80,    0,    0,    0,
     0,    0,    0,
};
// Deflection bits controlling projectile/ancilla interaction.
// Bit 0 = no death-flag (respawns on re-entry). Bit 1 = bounces rumblings.
// Bit 2 = immune to ancilla projectiles. Bit 3 = uses simplified tile collision.
// Bit 4 = immune to boomerang stun. Bit 5 = immune to hookshot.
// Bit 6 = despawns when off screen (OW). Bit 7 = immune to all pause (runs always).
static const uint8 kSpriteInit_DeflBits[243] = {
     0,    0, 0x44, 0x20, 0x20, 0x20, 0x20, 0x20,    0, 0x81,    0,    0, 0x48,    0,    0,    0,
     0,    0,    0,    0,    0,    0,    4,    0,    0,    0,    0, 0x48, 0x24, 0x80,    0,    0,
     0, 0x20,    0,    0,    0, 0x80,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0x80,
  0x80,    0,    0,    0,    0,    0,    0, 0x80,    0, 0x80,    0,    2,    0,    0,    0,    4,
  0x80,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
  0x84,    0, 0x81,    5,    1, 0x40,    8, 0xa0,    0,    0,    0,    0,    0, 0x84, 0x84, 0x84,
  0x84,    8, 0x80, 0x80, 0x80,    0, 0x80, 0x80, 0x80, 0x80,    0,    8, 0x80,    0,    0,    0,
  0x40,    0,    0,    0,    0,    0,    0,    0,    0,    2,    1,    0,    0,    4,    0,    0,
     0,    0, 0x80,    4,    4,    0,    0, 0x48,    0,    0,    4,    0,    1,    1,    0,    0,
  0x80,    0,    0,    0, 0x40,    8,    8,    8,    8,    0,    0,    0, 0x80, 0x80,    0,    0,
     0,    4,    1,    5,    0,    0,    0,    0,    0,    0,    0,    4,    2,    0, 0x80, 0x80,
  0x80, 0x80, 0x82, 0x80,    0,    0, 0x80,    0,    0, 0x80, 0x80,    0,    0,    1,    1, 0x40,
     0,    0,    4,    0,    0,    0,    0,    0,    0,    0,    4,    5,    5,    5, 0x80, 0x80,
     0,    0,    0,    0,    0,    0,    0,    0,    2,    2,    2,    2,    2,    2,    2,    2,
     2,    2,    2,    2,    2,    2,    2,    2,    2,    0, 0x82, 0x82,    8, 0x80, 0x20, 0x80,
  0x80, 0x80, 0x20,
};
// Heart damage dealt to Link per sprite contact. Indexed as [3*bump_class + armor].
// bump_class = sprite_bump_damage[k] & 0xf (0-9). armor = link_armor (0=none, 1=green, 2=red).
// Higher armor values reduce damage taken. bump_class 0 = 0 damage (floor hazard only).
static const int8 kPlayerDamages[30] = {
  2, 1, 1, 4, 4, 4, 0, 0, 0, 8, 4, 2, 8, 8, 8, 16,
  8, 4, 32, 16, 8, 32, 24, 16, 24, 16, 8, 64, 48, 24,
};
// Hitbox offsets for Link's dash run attack. Indexed by link_direction_facing>>1.
// The run hitbox is a large 16×16 box centered in front of Link.
static const uint8 kPlayerActionBoxRun_YHi[4] = {0xff, 0, 0, 0};
static const uint8 kPlayerActionBoxRun_YLo[4] = {(uint8)-8, 16, 8, 8};
static const uint8 kPlayerActionBoxRun_XHi[4] = {0, 0, 0xff, 0};
static const uint8 kPlayerActionBoxRun_XLo[4] = {0, 0, (uint8)-8, 8};

// Sword action hitbox tables. Indexed by t = direction*8 + button_b_frames + 1.
// Each entry gives an {X, W, Y, H} rectangle offset from Link's world position
// that represents the active sword/item strike area for that frame of the swing.
static const int8 kPlayer_SetupActionHitBox_X[65] = {
  0, 2, 0, 0, -8, 0, 2, 0, 2, 2, 1, 1, 0, 0, 0, 0,
  0, 2, 4, 4, 0, 0, -4, -4, -6, 2, 1, 1, 0, 0, 0, 0,
  0, 0, 0, 0, 2, 2, 4, 4, 2, 2, 2, 2, 0, 0, 0, 0,
  0, 0, 0, 0, -4, -4, -10, 0, 2, 2, 0, 0, 0, 0, 0, 0,
  0,
};
static const int8 kPlayer_SetupActionHitBox_W[65] = {
  15, 4, 8, 8, 8, 8, 12, 8, 4, 4, 6, 6, 0, 0, 0, 0,
  0, 4, 16, 12, 8, 8, 12, 11, 12, 4, 6, 6, 0, 0, 0, 0,
  0, 8, 8, 8, 10, 14, 15, 4, 4, 4, 6, 6, 0, 0, 0, 0,
  0, 8, 8, 8, 10, 14, 15, 4, 4, 4, 6, 6, 0, 0, 0, 0,
  0,
};
static const int8 kPlayer_SetupActionHitBox_Y[65] = {
  0, 2, 0, 2, 4, 4, 4, 7, 2, 2, 1, 1, 0, 0, 0, 0,
  0, 2, 0, 2, -4, -3, -8, 0, 0, 2, 1, 1, 0, 0, 0, 0,
  0, 0, 0, 0, -2, 0, -4, 1, 2, 2, 1, 1, 0, 0, 0, 0,
  0, 0, 0, 0, -2, 0, -4, 1, 2, 2, 1, 1, 0, 0, 0, 0,
  0,
};
static const int8 kPlayer_SetupActionHitBox_H[65] = {
  15, 4, 8, 2, 12, 8, 12, 8, 4, 4, 6, 6, 0, 0, 0, 0,
  0, 4, 8, 4, 12, 12, 12, 4, 8, 4, 6, 4, 0, 0, 0, 0,
  0, 8, 8, 8, 8, 8, 12, 4, 4, 4, 6, 6, 0, 0, 0, 0,
  0, 8, 8, 8, 8, 8, 12, 4, 4, 4, 6, 6, 0, 0, 0, 0,
  0,
};
// Flags per button_b_frames value (0-12). 1 = this frame of the swing has no
// active hitbox yet (early frames before the sword extends). Used to skip hitbox
// setup during the wind-up frames of a sword swing.
static const int8 kPlayer_SetupActionHitBox_Tab4[13] = {
  1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1
};
// Maps sprite_type → base character index in kSprite_PrepAndDrawSingleLarge_Tab2.
// Sprite OAM character = Tab2[Tab1[sprite_type] + sprite_graphics[k]].
// 0 entries indicate sprite types that do not use the standard large-sprite path.
static const uint8 kSprite_PrepAndDrawSingleLarge_Tab1[236] = {
  200, 0, 107, 0, 0, 0, 0, 0, 0, 203, 0, 8, 10, 11, 0, 0,
  13, 0, 0, 86, 0, 0, 15, 17, 0, 19, 0, 0, 0, 0, 20, 0,
  21, 27, 0, 42, 42, 248, 0, 182, 0, 0, 0, 170, 0, 0, 28, 0,
  0, 0, 0, 0, 0, 0, 0, 243, 243, 0, 187, 39, 0, 0, 66, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 15, 63, 0, 0, 0, 64, 64,
  68, 0, 0, 0, 0, 71, 70, 0, 0, 72, 74, 101, 101, 0, 0, 0,
  0, 0, 143, 0, 0, 76, 78, 78, 78, 78, 0, 48, 36, 50, 56, 60,
  129, 0, 82, 0, 0, 0, 0, 0, 0, 92, 0, 98, 94, 0, 0, 0,
  101, 102, 0, 0, 0, 0, 110, 14, 0, 59, 66, 0, 0, 117, 120, 123,
  0, 0, 207, 0, 132, 141, 141, 141, 141, 0, 148, 117, 160, 0, 0, 162,
  166, 0, 0, 0, 177, 0, 181, 0, 189, 0, 0, 0, 105, 0, 0, 0,
  0, 0, 92, 0, 214, 230, 0, 0, 0, 219, 218, 233, 0, 0, 190, 192,
  106, 0, 249, 215, 0, 0, 0, 216, 0, 0, 222, 227, 0, 0, 0, 235,
  0, 0, 0, 0, 0, 0, 244, 244, 29, 31, 31, 31, 32, 32, 32, 33,
  34, 35, 35, 37, 40, 106, 246, 41, 0, 0, 205, 206,
};
// VRAM character tile numbers for all sprite animation frames. Each sprite type
// indexes into this array via Tab1 + sprite_graphics to get the tile to display.
static const uint8 kSprite_PrepAndDrawSingleLarge_Tab2[251] = {
  0xa0, 0xa2, 0xa0, 0xa2, 0x80, 0x82, 0x80, 0x82, 0xea, 0xec, 0x84, 0x4e, 0x61, 0xbd, 0x8c, 0x20,
  0x22, 0xc0, 0xc2, 0xe6, 0xe4, 0x82, 0xaa, 0x84, 0xac, 0x80, 0xa0, 0xca, 0xaf, 0x29, 0x39, 0xb,
  0x6e, 0x60, 0x62, 0x63, 0x4c, 0xea, 0xec, 0x24, 0x6b, 0x24, 0x22, 0x24, 0x26, 0x20, 0x30, 0x21,
  0x2a, 0x24, 0x86, 0x88, 0x8a, 0x8c, 0x8e, 0xa2, 0xa4, 0xa6, 0xa8, 0xaa, 0x84, 0x80, 0x82, 0x6e,
  0x40, 0x42, 0xe6, 0xe8, 0x80, 0x82, 0xc8, 0x8d, 0xe3, 0xe5, 0xc5, 0xe1, 4, 0x24, 0xe, 0x2e,
  0xc, 0xa, 0x9c, 0xc7, 0xb6, 0xb7, 0x60, 0x62, 0x64, 0x66, 0x68, 0x6a, 0xe4, 0xf4, 2, 2,
  0, 4, 0xc6, 0xcc, 0xce, 0x28, 0x84, 0x82, 0x80, 0xe5, 0x24, 0, 2, 4, 0xa0, 0xaa,
  0xa4, 0xa6, 0xac, 0xa2, 0xa8, 0xa6, 0x88, 0x86, 0x8e, 0xae, 0x8a, 0x42, 0x44, 0x42, 0x44, 0x64,
  0x66, 0xcc, 0xcc, 0xca, 0x87, 0x97, 0x8e, 0xae, 0xac, 0x8c, 0x8e, 0xaa, 0xac, 0xd2, 0xf3, 0x84,
  0xa2, 0x84, 0xa4, 0xe7, 0x8a, 0xa8, 0x8a, 0xa8, 0x88, 0xa0, 0xa4, 0xa2, 0xa6, 0xa6, 0xa6, 0xa6,
  0x7e, 0x7f, 0x8a, 0x88, 0x8c, 0xa6, 0x86, 0x8e, 0xac, 0x86, 0xbb, 0xac, 0xa9, 0xb9, 0xaa, 0xba,
  0xbc, 0x8a, 0x8e, 0x8a, 0x86, 0xa, 0xc2, 0xc4, 0xe2, 0xe4, 0xc6, 0xea, 0xec, 0xff, 0xe6, 0xc6,
  0xcc, 0xec, 0xce, 0xee, 0x4c, 0x6c, 0x4e, 0x6e, 0xc8, 0xc4, 0xc6, 0x88, 0x8c, 0x24, 0xe0, 0xae,
  0xc0, 0xc8, 0xc4, 0xc6, 0xe2, 0xe0, 0xee, 0xae, 0xa0, 0x80, 0xee, 0xc0, 0xc2, 0xbf, 0x8c, 0xaa,
  0x86, 0xa8, 0xa6, 0x2c, 0x28, 6, 0xdf, 0xcf, 0xa9, 0x46, 0x46, 0xea, 0xc0, 0xc2, 0xe0, 0xe8,
  0xe2, 0xe6, 0xe4, 0xb, 0x8e, 0xa0, 0xec, 0xea, 0xe9, 0x48, 0x58,
};
// Sprite collision tile map size per overworld area (in 256-tile-cell units).
// Values of 2 mean the area spans 2×256 = 512 tile cells per axis (one screen).
// Values of 4 mean 4×256 = 1024 tile cells (two screens wide/tall for large areas).
// Used to set sprcoll_x_size and sprcoll_y_size which bound tile-collision probes.
static const uint8 kOverworldAreaSprcollSizes[192] = {
  4, 4, 2, 4, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4,
  2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 2, 4, 4, 2, 4, 4,
  4, 4, 2, 4, 4, 2, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2,
  4, 4, 2, 2, 2, 4, 4, 2, 4, 4, 2, 2, 2, 4, 4, 2,
  4, 4, 2, 4, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4,
  2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 2, 4, 4, 2, 4, 4,
  4, 4, 2, 4, 4, 2, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2,
  4, 4, 2, 2, 2, 4, 4, 2, 4, 4, 2, 2, 2, 4, 4, 2,
  4, 4, 2, 4, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4,
  2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 2, 4, 4, 2, 4, 4,
  4, 4, 2, 4, 4, 2, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2,
  4, 4, 2, 2, 2, 4, 4, 2, 4, 4, 2, 2, 2, 4, 4, 2,
};
// Starting OAM slot base addresses for the six drawing regions A-F.
// Region A (0x30): general sprites. B (0x1d0): sprites above Link. C (0): sprites below Link.
// D (0x30): lower-floor sprites (sort mode). E (0x120): ??? F (0x140): lower-floor upper slots.
static const uint16 kOam_ResetRegionBases[6] = {0x30, 0x1d0, 0, 0x30, 0x120, 0x140};
// OAM slot count (in bytes) reserved per garnish entry. Index 0 is a sentinel.
// Most garnishes use 4 bytes (one 8×8 tile). Types 17 and 21 use 8 bytes (two tiles).
// Type 22 (Garnish16_ThrownItemDebris) uses 16 bytes (four tiles) for debris scatter.
static const uint8 kGarnish_OamMemSize[23] = {
  0, 4, 4, 4, 4, 4,  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  8, 4, 4, 4, 8, 16,
};

// Dispatch table for 22 garnish (visual trail/effect) types. Indexed by garnish_type[k]-1.
// NULL at index 12 means type 0xD is undefined and will crash if spawned.
static HandlerFuncK *const kGarnish_Funcs[22] = {
  &Garnish01_FireSnakeTail,
  &Garnish02_MothulaBeamTrail,
  &Garnish03_FallingTile,
  &Garnish04_LaserTrail,
  &Garnish_SimpleSparkle,
  &Garnish06_ZoroTrail,
  &Garnish07_BabasuFlash,
  &Garnish08_KholdstareTrail,
  &Garnish09_LightningTrail,
  &Garnish0A_CannonSmoke,
  &Garnish_WaterTrail,
  &Garnish0C_TrinexxIceBreath,
  NULL,
  &Garnish0E_TrinexxFireBreath,
  &Garnish0F_BlindLaserTrail,
  &Garnish10_GanonBatFlame,
  &Garnish11_WitheringGanonBatFlame,
  &Garnish12_Sparkle,
  &Garnish13_PyramidDebris,
  &Garnish14_KakKidDashDust,
  &Garnish15_ArrghusSplash,
  &Garnish16_ThrownItemDebris,
};
// Fall animation frame index tables for SpriteModule_Fall2.
// Tab1: humanoid fall frame index (0-13), indexed by delay>>1 (0-31, decreasing frame index).
// Tab2: Helma/beetle fall frame index (0-5), same indexing.
static const uint8 kSpriteFall_Tab1[32] = {
  13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 3, 3, 3, 3,
  3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8 kSpriteFall_Tab2[32] = {
  5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 3, 3, 3, 3,
  3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
};
// Frame mask for fall XY movement. Indexed by delay>>3. Higher masks mean less
// frequent XY position updates (movement slows as the sprite falls deeper).
static const uint8 kSpriteFall_Tab3[16] = {0xff, 0x3f, 0x1f, 0xf, 0xf, 7, 3, 1, 0xff, 0x3f, 0x1f, 0xf, 7, 3, 1, 0};
// Humanoid fall frame OAM row offset per facing direction (0=up,1=down,2=left,3=right).
static const int8 kSpriteFall_Tab4[4] = {0, 4, 8, 0};
// DrawMultipleData entries for falling Helma/Beetle sprites (SpriteModule_Fall2).
// Two groups of 6 frames: [0-5] = Helma falling, [6-11] = Beetle falling.
// Each frame uses a single large (16×16) tile.
static const DrawMultipleData kSpriteDrawFall0Data[12] = {
  {0, 0, 0x0146, 2},
  {0, 0, 0x0148, 2},
  {0, 0, 0x014a, 2},
  {4, 4, 0x014c, 0},
  {4, 4, 0x00b7, 0},
  {4, 4, 0x0080, 0},
  {0, 0, 0x016c, 2},
  {0, 0, 0x016e, 2},
  {0, 0, 0x014e, 2},
  {4, 4, 0x015c, 0},
  {4, 4, 0x00b7, 0},
  {4, 4, 0x0080, 0},
};
// Per-tile OAM data for humanoid fall animation. 14 frames × 4 tiles each = 56 entries.
// Rows 0-3 = facing right, rows 4-7 = facing left, rows 8-13 = front/back.
// Unused tile slots within a frame are set to charnum=0 (rendered as transparent).
static const int8 kSpriteDrawFall1_X[56] = {
  -4, 4, -4, 12, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
  -4, 12, -4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
  -4, 12, -4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
  4, 0, 0, 0, 4, 0, 0, 0,
};
static const int8 kSpriteDrawFall1_Y[56] = {
  -4, -4, 4, 12, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
  -4, -4, 12, 4, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
  -4, -4, 12, 4, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
  4, 0, 0, 0, 4, 0, 0, 0,
};
static const uint8 kSpriteDrawFall1_Char[56] = {
  0xae, 0xa8, 0xa6, 0xaf, 0xaa, 0, 0, 0, 0xac, 0, 0, 0, 0xbe, 0, 0, 0,
  0xa8, 0xae, 0xaf, 0xa6, 0xaa, 0, 0, 0, 0xac, 0, 0, 0, 0xbe, 0, 0, 0,
  0xa6, 0xaf, 0xae, 0xa8, 0xaa, 0, 0, 0, 0xac, 0, 0, 0, 0xbe, 0, 0, 0,
  0xb6, 0, 0, 0, 0x80, 0, 0, 0,
};
static const uint8 kSpriteDrawFall1_Flags[56] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0x40, 0x40, 0x40, 0x40, 0x40, 0, 0, 0, 0x40, 0, 0, 0, 0x40, 0, 0, 0,
  0x80, 0x80, 0x80, 0x80, 0x80, 0, 0, 0, 0x80, 0, 0, 0, 0x80, 0, 0, 0,
  1, 0, 0, 0, 1, 0, 0, 0,
};
static const uint8 kSpriteDrawFall1_Ext[56] = {
  0, 2, 2, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
  2, 0, 0, 2, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
  2, 0, 0, 2, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,
};
// X/Y offsets for the four dots in the distress/warning indicator drawn above a
// sprite that is about to fall into a pit (SpriteModule_Fall2 at delay>=0x40).
static const int8 kSpriteDistress_X[4] = {-3, 2, 7, 11};
static const int8 kSpriteDistress_Y[4] = {-5, -7, -7, -5};
// Items that Pikit (the thieving flying creature) drops when killed at each steal level.
// Indexed by sprite_E[k]-1 (number of items stolen so far).
static const uint8 kPikitDropItems[4] = {0xdc, 0xe1, 0xd9, 0xe6};
// Random drop frequency masks per prize tier (0-6). AND'd with GetRandomNumber():
// if result is 0, the drop occurs. Mask 0 = always drop; mask 1 = 50% chance.
static const uint8 kPrizeMasks[7] = { 1, 1, 1, 0, 1, 1, 1 };

// Prize item rotation table. 7 tiers × 8 slots = 56 entries. Each slot holds the
// sprite_type of the item to drop. prizes_arr1[tier] tracks the current slot position
// and advances by 1 each kill, cycling through the 8 variants for each tier.
static const uint8 kPrizeItems[56] = {
  0xd8, 0xd8, 0xd8, 0xd8, 0xd9, 0xd8, 0xd8, 0xd9, 0xda, 0xd9, 0xda, 0xdb, 0xda, 0xd9, 0xda, 0xda,
  0xe0, 0xdf, 0xdf, 0xda, 0xe0, 0xdf, 0xd8, 0xdf, 0xdc, 0xdc, 0xdc, 0xdd, 0xdc, 0xdc, 0xde, 0xdc,
  0xe1, 0xd8, 0xe1, 0xe2, 0xe1, 0xd8, 0xe1, 0xe2, 0xdf, 0xd9, 0xd8, 0xe1, 0xdf, 0xdc, 0xd9, 0xd8,
  0xd8, 0xe3, 0xe0, 0xdb, 0xde, 0xd8, 0xdb, 0xe2,
};
// Initial Z encoding for each absorbable type (0xD8 + index). High nibble = z_vel
// upward kick on spawn (in units of 16). Low nibble = X offset added to spawn position.
static const uint8 kPrizeZ[15] = {0, 0x24, 0x24, 0x24, 0x20, 0x20, 0x20, 0x24, 0x24, 0x24, 0x24, 0, 0x24, 0x20, 0x20};
// Poof (death cloud) animation OAM data. 8 phases × 4 tiles = 32 entries.
// The animation plays backward (sprite_delay_main counts down); each phase
// shows expanding smoke puffs. X/Y are offsets from the sprite's center.
static const int8 kPerishOverlay_X[32] = {
  0, 0, 0, 8, 0, 8, 0, 8, 8, 8, 0, 8, 0, 8, 0, 8,
  0, 8, 0, 8, 0, 8, 0, 8, -3, 11, -3, 11, -6, 14, -6, 14,
};
static const int8 kPerishOverlay_Y[32] = {
  0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8,
  0, 0, 8, 8, 0, 0, 8, 8, -3, -3, 11, 11, -6, -6, 14, 14,
};
static const uint8 kPerishOverlay_Char[32] = {
  0, 0xb9, 0, 0, 0xb4, 0xb5, 0xb5, 0xb4, 0xb9, 0, 0, 0, 0xb5, 0xb4, 0xb4, 0xb5,
  0xa8, 0xa8, 0xb8, 0xb8, 0xa8, 0xa8, 0xb8, 0xb8, 0xa9, 0xa9, 0xa9, 0xa9, 0x9b, 0x9b, 0x9b, 0x9b,
};
static const uint8 kPerishOverlay_Flags[32] = {
  4, 4, 4, 4, 4, 4, 0xc4, 0xc4, 0x44, 4, 4, 4, 0x44, 0x44, 0x84, 0x84,
  4, 0x44, 4, 0x44, 4, 0x44, 4, 0x44, 0x44, 4, 0xc4, 0x84, 4, 0x44, 0x84, 0xc4,
};
// Sprite state machine dispatch table. Indexed by sprite_state[k] (0-11).
// State 0=inactive, 1=fall (simple), 2=poof, 3=drown, 4=explode, 5=fall (complex),
// 6=die, 7=burn, 8=initialize, 9=active (enemy AI), 10=carried, 11=stunned/thrown.
static HandlerFuncK *const kSprite_ExecuteSingle[12] = {
  &Sprite_inactiveSprite,
  &SpriteModule_Fall1,
  &SpriteModule_Poof,
  &SpriteModule_Drown,
  &SpriteModule_Explode,
  &SpriteModule_Fall2,
  &SpriteModule_Die,
  &SpriteModule_Burn,
  &SpriteModule_Initialize,
  &SpriteActive_Main,
  &SpriteModule_Carried,
  &SpriteModule_Stunned,
};
// it's not my job to tell you what to think, my job is to think about what you tell me'
// Shorthand macros for two scratch 16-bit RAM words used as temporaries during
// Sprite_PrepOamCoordOrDoubleRet (screen X/Y intermediate calculations).
#define R0 WORD(g_ram[0])
#define R2 WORD(g_ram[2])
// Tables for spawning the hidden item from broken terrain (pots, rocks, bushes).
// All arrays are indexed by dung_secrets_unk1 - 1 (value 1-22 after terrain breaks).

// Sprite type to spawn as the secret item. 0 = no secret item for this slot.
static const uint8 kSpawnSecretItems[22] = {
  0xd9, 0x3e, 0x79, 0xd9, 0xdc, 0xd8, 0xda, 0xe4, 0xe1, 0xdc, 0xd8, 0xdf, 0xe0, 0xb, 0x42, 0xd3,
  0x41, 0xd4, 0xd9, 0xe3, 0xd8, 0,
};
// Initial sprite_ai_state value for the spawned secret item (some items need a
// non-zero initial state to behave as a "rising out of broken pot" drop).
static const uint8 kSpawnSecretItem_SpawnFlag[22] = {
  0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 1, 0, 0,
  0, 0, 0, 0, 0, 0,
};
// X pixel offset added to the terrain's position when spawning the secret item.
static const uint8 kSpawnSecretItem_XLo[22] = {
  4, 0, 4, 4, 0, 4, 4, 4, 4, 0, 4, 4, 4, 0, 0, 0,
  0, 0, 4, 0, 4, 4,
};
// Initial sprite_ignore_projectile value: 1 = item is temporarily immune to
// being picked up by ancilla (prevents instant hookshot grab on spawn).
static const uint8 kSpawnSecretItem_IgnoreProj[22] = {
  1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0,
  0, 0, 1, 1, 1, 1,
};
// Initial upward Z velocity (height) for the secret item to pop out of the terrain.
// Items with ZVel=0 are placed directly on the ground without a bounce.
static const uint8 kSpawnSecretItem_ZVel[22] = {
  16, 0, 0, 16, 0, 0, 16, 16, 16, 16, 0, 16, 10, 16, 0, 0,
  0, 0, 16, 0, 0, 0,
};
// Maps the direction-to-face-link result (0-3) to the link_direction_facing value
// (0=up/4, 1=down/6, 2=left/0, 3=right/2) set when Link picks up a sprite.
static const uint8 kSprite_ReturnIfLifted_Dirs[4] = {4, 6, 0, 2};

// Draw mode per absorbable type (sprite_type - 0xD8). 0=small 8×8, 1=large 16×16,
// 2=thin-and-tall (stacked 8×8 pair for keys). Used by SpriteDraw_AbsorbableTransient.
static const uint8 kAbsorbable_Tab1[15] = {0, 1, 1, 1, 2, 2, 2, 0, 1, 1, 2, 2, 1, 2, 2};

// Numbered absorb table. Non-zero values select a multi-tile digit display mode
// (for stacked rupees and bomb packs). 0 = use standard sprite draw path.
// Values index into kNumberedAbsorbable_* with offset (value-1)*3.
static const uint8 kAbsorbable_Tab2[19] = {0, 0, 0, 0, 1, 2, 3, 0, 0, 4, 5, 0, 0, 0, 0, 2, 4, 6, 2};
// OAM data for numbered absorbable items (stacked rupees, bomb packs with digit labels).
// Each entry is a group of 3 tiles: groups 0-2 for rupee stacks, groups 3-5 for bomb packs.
// The count displayed (1,5,20 / 1,4,8) appears as a small digit to the right of the item.
static const int16 kNumberedAbsorbable_X[18] = { 0, 0, 8, 0, 0, 8, 0, 0, 8, 0, 0, 2, 0, 0, 2, 0, 0, 0, };
static const int16 kNumberedAbsorbable_Y[18] = { 0, 0, 8, 0, 0, 8, 0, 0, 8, 0, 8, 8, 0, 8, 8, 0, 8, 8, };
static const uint8 kNumberedAbsorbable_Char[18] = { 0x6e, 0x6e, 0x68, 0x6e, 0x6e, 0x78, 0x6e, 0x6e, 0x79, 0x63, 0x73, 0x69, 0x63, 0x73, 0x6a, 0x63, 0x73, 0x73, };
static const uint8 kNumberedAbsorbable_Ext[18] = { 2, 2, 0, 2, 2, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, };
// Rupee amounts awarded by green, blue, and red rupee absorbables respectively.
static const uint8 kRupeesAbsorption[3] = {1, 5, 20};
// Sound effect IDs played when each absorbable type is collected (t = type - 0xD8).
const uint8 kAbsorptionSfx[15] = {0xb, 0xa, 0xa, 0xa, 0xb, 0xb, 0xb, 0xb, 0xb, 0xb, 0xb, 0xb, 0x2f, 0x2f, 0xb};
// Bomb amounts awarded by small, medium, and large bomb pack absorbables respectively.
static const uint8 kBombsAbsorption[3] = {1, 4, 8};
// Bit masks for the big-key death-flag in dung_savegame_state_bits.
// Indexed by sprite_die_action[k]: 0x40 for standard big key, 0x20 for alternate.
const uint8 kAbsorbBigKey[2] = {0x40, 0x20};
static int AllocOverlord();
static int Overworld_AllocSprite(uint8 type);

// Returns the full 16-bit world X coordinate for sprite slot k by combining the
// low and high bytes stored in sprite_x_lo[k] and sprite_x_hi[k].
uint16 Sprite_GetX(int k) {
  return sprite_x_lo[k] | sprite_x_hi[k] << 8;
}

// Returns the full 16-bit world Y coordinate for sprite slot k by combining the
// low and high bytes stored in sprite_y_lo[k] and sprite_y_hi[k].
uint16 Sprite_GetY(int k) {
  return sprite_y_lo[k] | sprite_y_hi[k] << 8;
}

// Writes a 16-bit world X coordinate into the split lo/hi byte fields of sprite slot k.
void Sprite_SetX(int k, uint16 x) {
  sprite_x_lo[k] = x;
  sprite_x_hi[k] = x >> 8;
}

// Writes a 16-bit world Y coordinate into the split lo/hi byte fields of sprite slot k.
void Sprite_SetY(int k, uint16 y) {
  sprite_y_lo[k] = y;
  sprite_y_hi[k] = y >> 8;
}

// Nudges sprite k's X and Y velocities one unit closer to the target values x and y.
// Called each frame to smoothly accelerate toward a desired speed without snapping.
// Parameters:
//   k — sprite slot index
//   x — target X velocity (unsigned, treated as signed via sign8 comparison)
//   y — target Y velocity
void Sprite_ApproachTargetSpeed(int k, uint8 x, uint8 y) {
  if (sprite_x_vel[k] - x)
    sprite_x_vel[k] += sign8(sprite_x_vel[k] - x) ? 1 : -1;
  if (sprite_y_vel[k] - y)
    sprite_y_vel[k] += sign8(sprite_y_vel[k] - y) ? 1 : -1;
}

// Adds signed pixel offsets xv and yv directly to the sprite's 16-bit world position.
// Used for wall pushback and conveyor nudges where sub-pixel precision is not needed.
void SpriteAddXY(int k, int xv, int yv) {
  Sprite_SetX(k, Sprite_GetX(k) + xv);
  Sprite_SetY(k, Sprite_GetY(k) + yv);
}

// Applies Z, X, and Y sub-pixel velocity integration in that order for sprite k.
// Z is applied first so that the height affects Y rendering correctly in the same frame.
void Sprite_MoveXYZ(int k) {
  Sprite_MoveZ(k);
  Sprite_MoveX(k);
  Sprite_MoveY(k);
}

// Reverses both the X and Y velocity components. Used for wall ricochets and
// direction reversals when a sprite should bounce back toward where it came from.
void Sprite_Invert_XY_Speeds(int k) {
  sprite_x_vel[k] = -sprite_x_vel[k];
  sprite_y_vel[k] = -sprite_y_vel[k];
}

// Allocates a sparkle garnish (type 5) from slots ≤ limit, positioned at the sprite's
// world location offset by (x, y) minus the current Z height plus 16.
// The spawned sparkle fades over 31 frames.
// Parameters:
//   k     — sprite slot whose position and floor are used for the sparkle origin
//   x, y  — local offset from the sprite's world position
//   limit — maximum garnish slot index to allocate from (caps the sparkle count)
// Returns the allocated garnish slot index, or -1 if none was available.
// Also writes the slot index to g_ram[15] as a side effect.
int Sprite_SpawnSimpleSparkleGarnishEx(int k, uint16 x, uint16 y, int limit) {
  int j = GarnishAllocLimit(limit);
  if (j >= 0) {
    garnish_type[j] = 5;
    garnish_active = 5;
    Garnish_SetX(j, Sprite_GetX(k) + x);
    Garnish_SetY(j, Sprite_GetY(k) + y - sprite_z[k] + 16);
    garnish_countdown[j] = 31;
    garnish_sprite[j] = k;
    garnish_floor[j] = sprite_floor[k];
  }
  g_ram[15] = j;
  return j;
}

// Finds the highest-indexed free overlord slot (0-7). Overlords are persistent
// area entities (bomb traps, armos armies, etc.). Returns -1 if all 8 are occupied.
static int AllocOverlord() {
  int i = 7;
  while (i >= 0 && overlord_type[i] != 0)
    i--;
  return i;
}

// Finds a free sprite slot for an overworld sprite of the given type.
// Special types are restricted to specific high-numbered slots so that common
// sprites do not accidentally occupy slots reserved for bosses/NPCs:
//   0x58 (Agahnim) and 0xD0 → slots ≤ 4 or ≤ 5 respectively.
//   0xEB (wandering NPC), 0x53 (Agahnim boss), 0xF3 → slots ≤ 14.
//   All others → slots ≤ 13.
// Also allows reuse of slot if it holds a type 0x41 with C!=0 (inactive guard).
// Returns the slot index, or -1 if no free slot was found.
static int Overworld_AllocSprite(uint8 type) {
  int i = (type == 0x58) ? 4 :
          (type == 0xd0) ? 5 :
          (type == 0xeb || type == 0x53 || type  == 0xf3) ? 14 : 13;
  for (; i >= 0; i--) {
    if (sprite_state[i] == 0 || sprite_type[i] == 0x41 && sprite_C[i] != 0)
      break;
  }
  return i;
}

// Returns the full 16-bit world X coordinate of garnish slot k.
uint16 Garnish_GetX(int k) {
  return garnish_x_lo[k] | garnish_x_hi[k] << 8;
}

// Returns the full 16-bit world Y coordinate of garnish slot k.
uint16 Garnish_GetY(int k) {
  return garnish_y_lo[k] | garnish_y_hi[k] << 8;
}

// Shared drawing logic for sparkle garnishes (types 5 and 12).
// Draws one 8×8 OAM tile cycling through 4 frames based on garnish_countdown >> shift.
// Parameters:
//   k     — garnish slot index
//   shift — right-shift applied to countdown to slow animation (3=slow, 2=fast)
void Garnish_SparkleCommon(int k, uint8 shift) {
  static const uint8 kGarnishSparkle_Char[4] = {0x83, 0xc7, 0x80, 0xb7};
  uint8 t = garnish_countdown[k] >> shift;
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  OamEnt *oam = GetOamCurPtr();
  int j = garnish_sprite[k];
  SetOamPlain(oam, pt.x, pt.y, kGarnishSparkle_Char[t],
               (sprite_oam_flags[j] | sprite_obj_prio[j]) & 0xf0 | 4, 0);
}

// Shared drawing logic for dust-trail garnishes (types 11/water-trail, 14/dash-dust).
// Draws one 8×8 OAM tile cycling through 3 dust cloud frames based on countdown >> shift.
// Parameters:
//   k     — garnish slot index
//   shift — right-shift applied to countdown for frame rate control
void Garnish_DustCommon(int k, uint8 shift) {
  static const uint8 kRunningManDust_Char[3] = {0xdf, 0xcf, 0xa9};
  tmp_counter = garnish_countdown[k] >> shift;
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  OamEnt *oam = GetOamCurPtr();
  SetOamPlain(oam, pt.x, pt.y, kRunningManDust_Char[tmp_counter], 0x24, 0);
}

// State 4 sprite handler — boss explosion death animation.
// Two phases controlled by sprite_A[k]:
//   sprite_A[k] != 0 (second phase, started from sub-explosions spawned below):
//     Counts down sprite_delay_main; draws a 4-tile explosion frame. On reaching 0,
//     kills the sub-sprite. When all state-4 sprites are cleared, resets load_chr and
//     potentially un-blocks the link menu.
//   sprite_A[k] == 0 (main boss, first phase):
//     Runs SpriteActive_Main to allow late AI execution. Spawns sub-explosion sprites
//     (type 0x1C) at random offsets every 4/8 frames (type 0x92 = faster rate).
//     At delay == 32, stops movement and triggers either dungeon exit, boss-room ceremony,
//     or direct music change depending on sprite type.
// The 8-frame explosion animation loops through 4-tile DrawMultipleData entries using
// ((delay>>2) ^ 7) as the frame index (counts down from 7 to 0).
void SpriteModule_Explode(int k) {
  static const DrawMultipleData kSpriteExplode_Dmd[32] = {
    { 0,  0, 0x0060, 2},
    { 0,  0, 0x0060, 2},
    { 0,  0, 0x0060, 2},
    { 0,  0, 0x0060, 2},
    {-5, -5, 0x0062, 2},
    { 5, -5, 0x4062, 2},
    {-5,  5, 0x8062, 2},
    { 5,  5, 0xc062, 2},
    {-8, -8, 0x0062, 2},
    { 8, -8, 0x4062, 2},
    {-8,  8, 0x8062, 2},
    { 8,  8, 0xc062, 2},
    {-8, -8, 0x0064, 2},
    { 8, -8, 0x4064, 2},
    {-8,  8, 0x8064, 2},
    { 8,  8, 0xc064, 2},
    {-8, -8, 0x0066, 2},
    { 8, -8, 0x4066, 2},
    {-8,  8, 0x8066, 2},
    { 8,  8, 0xc066, 2},
    {-8, -8, 0x0068, 2},
    { 8, -8, 0x0068, 2},
    {-8,  8, 0x0068, 2},
    { 8,  8, 0x0068, 2},
    {-8, -8, 0x006a, 2},
    { 8, -8, 0x406a, 2},
    {-8,  8, 0x806a, 2},
    { 8,  8, 0xc06a, 2},
    {-8, -8, 0x004e, 2},
    { 8, -8, 0x404e, 2},
    {-8,  8, 0x804e, 2},
    { 8,  8, 0xc04e, 2},
  };

  if (sprite_A[k]) {
    if (!sprite_delay_main[k]) {
      sprite_state[k] = 0;
      for (int j = 15; j >= 0; j--) {
        if (sprite_state[j] == 4)
          return;
      }
      load_chr_halfslot_even_odd = 1;
      if (!Sprite_CheckIfScreenIsClear())
        flag_block_link_menu = 0;
    } else {
      Sprite_DrawMultiple(k, &kSpriteExplode_Dmd[((sprite_delay_main[k] >> 2) ^ 7) * 4], 4, NULL);
    }
    return;
  }
  sprite_floor[k] = 2;

  if (sprite_delay_main[k] == 32) {
    sprite_state[k] = 0;
    flag_is_link_immobilized = 0;
    if (player_near_pit_state != 2 && Sprite_CheckIfScreenIsClear()) {
      if (sprite_type[k] >= 0xd6) {
        music_control = 0x13;
      } else if (sprite_type[k] == 0x7a) {
        PrepareDungeonExitFromBossFight();
      } else {
        SpriteExplode_SpawnEA(k);
        return;
      }
    }
  }

  if (sprite_delay_main[k] >= 64 && (sprite_delay_main[k] >= 0x70 || !(sprite_delay_main[k] & 1)))
    SpriteActive_Main(k);

  uint8 type = sprite_type[k];
  if (sprite_delay_main[k] >= 0xc0)
    return;
  if ((sprite_delay_main[k] & 3) == 0)
    SpriteSfx_QueueSfx2WithPan(k, 0xc);
  if (sprite_delay_main[k] & ((type == 0x92) ? 3 : 7))
    return;

  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(k, 0x1c, &info);
  if (j >= 0) {
    static const int8 kSpriteExplode_RandomXY[16] = {0, 4, 8, 12, -4, -8, -12, 0, 0, 8, 16, 24, -24, -16, -8, 0};
    load_chr_halfslot_even_odd = 11;
    sprite_state[j] = 4;
    sprite_flags2[j] = 3;
    sprite_oam_flags[j] = 0xc;
    int xoff = kSpriteExplode_RandomXY[(GetRandomNumber() & 7) | ((type == 0x92) ? 8 : 0)];
    int yoff = kSpriteExplode_RandomXY[(GetRandomNumber() & 7) | ((type == 0x92) ? 8 : 0)];
    Sprite_SetX(j, info.r0_x + xoff);
    Sprite_SetY(j, info.r2_y + yoff - info.r4_z);
    sprite_delay_main[j] = 31;
    sprite_A[j] = 31;
  }
  // endif_1
}

// Core death-poof animation handler, shared by state 6 (SpriteModule_Die) and the
// thrown-sprite path. When second_entry is false (normal death):
//   - Routes special types: throwable scenery debris to scatter, certain bosses to
//     SpriteActive_Main (they handle their own death); type 0x40 (evil barrier)
//     handles its own death separately.
//   - If delay==0, calls Sprite_DoTheDeath for final cleanup.
//   Otherwise advances the poof frame every 4 game ticks (while not paused),
//   draws SpriteDeath_DrawPoof, then calls SpriteActive_Main with a temporarily
//   reduced flags2 priority (so the dying sprite still runs AI during the poof).
// When second_entry is true (called from ThrownSprite path): skips the type checks
// and goes directly to SpriteDeath_DrawPoof + SpriteActive_Main.
void SpriteDeath_MainEx(int k, bool second_entry) {
  if (!second_entry) {
    uint8 type = sprite_type[k];
    if (type == 0xec) {
      ThrowableScenery_ScatterIntoDebris(k);
      return;
    }
    if (type == 0x53 || type == 0x54 || type == 0x92 || type == 0x4a && sprite_C[k] >= 2) {
      SpriteActive_Main(k);
      return;
    }
    if (sprite_delay_main[k] == 0) {
      Sprite_DoTheDeath(k);
      return;
    }
  }
  if (sign8(sprite_flags3[k])) {
    SpriteActive_Main(k);
    return;
  }
  if (!((frame_counter & 3) | submodule_index | flag_unk1))
    sprite_delay_main[k]++;
  SpriteDeath_DrawPoof(k);

  if (sprite_type[k] != 0x40 && sprite_delay_main[k] < 10)
    return;
  oam_cur_ptr += 16;
  oam_ext_cur_ptr += 4;
  uint8 bak = sprite_flags2[k];
  sprite_flags2[k] -= 4;
  SpriteActive_Main(k);
  sprite_flags2[k] = bak;
}

// State 7 sprite handler — sprite being burned alive.
// Plays a 4-frame fire animation by overriding sprite_graphics and drawing with
// Flame_Draw. Simultaneously runs SpriteActive_Main (so a burning enemy can still
// lurch toward Link). When delay reaches 0, finalizes death via Sprite_DoTheDeath.
// The kFlame_Gfx table maps delay>>3 to one of 4 flame frames (0-3).
void SpriteModule_Burn(int k) {
  static const uint8 kFlame_Gfx[32] = {
    5, 4, 3, 1, 2, 0, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0,
    1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0,
  };
  sprite_hit_timer[k] = 0;
  int j = sprite_delay_main[k] - 1;
  if (j == 0) {
    Sprite_DoTheDeath(k);
    return;
  }
  uint8 bak = sprite_graphics[k];
  uint8 bak1 = sprite_oam_flags[k];
  sprite_graphics[k] = kFlame_Gfx[j >> 3];
  sprite_oam_flags[k] = 3;
  Flame_Draw(k);
  sprite_oam_flags[k] = bak1;
  sprite_graphics[k] = bak;

  oam_cur_ptr += 8;
  oam_ext_cur_ptr += 2;
  if (sprite_delay_main[k] >= 0x10) {
    uint8 bak = sprite_flags2[k];
    sprite_flags2[k] -= 2;
    SpriteActive_Main(k);
    sprite_flags2[k] = bak;
  }
}

// Called when sprite k's hit_timer counts down to 31 (one frame before damage is
// applied). For Agahnim (type 0x7A) in the light world only: if the pending damage
// would reduce his HP to zero or below, show the "I'm going to destroy you" dialogue
// (message 0x140) to give the player a warning before the final hit kills him.
void Sprite_HitTimer31(int k) {
  if (sprite_type[k] != 0x7a || is_in_dark_world)
    return;
  if (sprite_health[k] <= sprite_give_damage[k]) {
    dialogue_message_index = 0x140;
    Sprite_ShowMessageMinimal();
  }
}

// State 11 sprite handler — stunned, frozen, or being thrown.
// When second_entry is false (normal stunned entry via SpriteModule_Stunned):
//   Draws a water ripple if submerged, runs SpriteStunned_Main_Func1 (sparkles/jitter),
//   checks damage from Link, checks recoil, moves by X/Y velocity, then checks tile
//   collision. On wall contact applies Sprite_ApplyRicochet and plays a bounce SFX if
//   state is still 11 (thrown). Also checks special tile properties for conveyor/fall.
// When second_entry is true (thrown terrain tile/sprite interaction):
//   Jumps directly to the wall-collision check (skips sparkle/damage).
// sprite_E[k] nonzero: skip tile collision (sprite is hidden under a rock or hookshot-held).
void SpriteStunned_MainEx(int k, bool second_entry) {
  if (second_entry)
    goto ThrownSprite_TileAndSpriteInteraction;
  Sprite_DrawRippleIfInWater(k);
  SpriteStunned_Main_Func1(k);
  if (Sprite_ReturnIfPaused(k))
    return;
  if (sprite_F[k]) {
    if (sign8(sprite_F[k]))
      sprite_F[k] = 0;
    sprite_x_vel[k] = sprite_y_vel[k] = 0;
  }
  if (sprite_delay_main[k] < 0x20)
    Sprite_CheckDamageFromLink(k);
  if (Sprite_ReturnIfRecoiling(k))
    return;
  Sprite_MoveXY(k);
  if (!sprite_E[k]) {
    Sprite_CheckTileCollision(k);
    if (!sprite_state[k])
      return;
  ThrownSprite_TileAndSpriteInteraction:
    if (sprite_wallcoll[k] & 0xf) {
      Sprite_ApplyRicochet(k);
      if (sprite_state[k] == 11)
        SpriteSfx_QueueSfx2WithPan(k, 5);
    }
  }
  Sprite_CheckTileProperty(k, 0x68);

  if (kSpriteInit_Flags3[sprite_type[k]] & 0x10) {
    sprite_flags3[k] |= 0x10;
    if (sprite_tiletype == 32)
      sprite_flags3[k] &= ~0x10;
  }
  Sprite_MoveZ(k);
  sprite_z_vel[k] -= 2;
  uint8 z = sprite_z[k] - 1;
  if (z >= 0xf0) {
    sprite_z[k] = 0;
    if (sprite_type[k] == 0xe8 && sign8(sprite_z_vel[k] - 0xe8)) {
      sprite_state[k] = 6;
      sprite_delay_main[k] = 8;
      sprite_flags2[k] = 3;
      return;
    }
    ThrowableScenery_TransmuteIfValid(k);
    uint8 a = sprite_tiletype;
    if (sprite_tiletype == 32 && !(a = sprite_flags[k] >> 1, sprite_flags[k] & 1)) {  // wtf
      Sprite_Func8(k);
      return;
    }
    if (a == 9) {
      z = sprite_z_vel[k];
      sprite_z_vel[k] = 0;
      int j;
      SpriteSpawnInfo info;

      if (sign8(z - 0xf0) && (j = Sprite_SpawnDynamically(k, 0xec, &info)) >= 0) {
        Sprite_SetSpawnedCoordinates(j, &info);
        Sprite_Func22(j);
      }
    } else if (a == 8) {
      if (sprite_type[k] == 0xd2 || (GetRandomNumber() & 1))
        Sprite_SpawnLeapingFish(k);
      Sprite_Func22(k);
      return;
    }
    z = sprite_z_vel[k];
    if (sign8(z)) {
      z = (uint8)(-z) >> 1;
      sprite_z_vel[k] = z < 9 ? 0 : z;
    }
    sprite_x_vel[k] = (int8)sprite_x_vel[k] >> 1;
    if (sprite_x_vel[k] == 255)
      sprite_x_vel[k] = 0;
    sprite_y_vel[k] = (int8)sprite_y_vel[k] >> 1;
    if (sprite_y_vel[k] == 255)
      sprite_y_vel[k] = 0;
  }
  if (sprite_state[k] != 11 || sprite_unk5[k] != 0) {
    if (Sprite_ReturnIfLifted(k))
      return;
    if (sprite_type[k] != 0x4a)
      ThrownSprite_CheckDamageToSprites(k);
  }
}

// Spawns a falling prize ancilla (type 0x29) for the given item type at slot 4.
// Used when a boss (e.g., Vitreous) is defeated and drops a pendant/crystal reward.
int Ancilla_SpawnFallingPrize(uint8 item) {  // 85a51d
  return AncillaAdd_FallingPrize(0x29, item, 4);
}

// Convenience wrapper: checks both damage directions for sprite k in one call.
// First checks whether Link has hit the sprite (damage from Link), then checks
// whether the sprite has hit Link (damage to Link). Returns the result of the
// second check (true = sprite successfully dealt damage to Link this frame).
bool Sprite_CheckDamageToAndFromLink(int k) {  // 85ab93
  Sprite_CheckDamageFromLink(k);
  return Sprite_CheckDamageToLink(k);
}

// Wrapper around Sprite_CheckTileCollision2 that runs the full tile-collision
// test for sprite k and returns the resulting wall-collision bitmask.
// Bits 0-3 of the return value indicate contact with left/right/up/down walls.
uint8 Sprite_CheckTileCollision(int k) {  // 85b88d
  Sprite_CheckTileCollision2(k);
  return sprite_wallcoll[k];
}

// Smoothly rotates a multi-segment enemy's body direction (sprite_D[k]) toward
// the head's current facing (sprite_head_dir[k]) for follow-the-leader motion.
// Returns true only when the body direction matches the head, signalling that
// the segment has caught up. While mismatched:
//   - Only updates once per 32-frame window (frame_counter & 0x1f == 0).
//   - If the mismatch involves only the axis bit (bit 1 equal), picks an
//     intermediate direction using the slot index and frame counter to stagger
//     multi-segment updates and prevent all segments turning simultaneously.
// Used by Lanmolas and other worm-type bosses to animate their trailing segments.
bool Sprite_TrackBodyToHead(int k) {  // 85dca2
  if (sprite_head_dir[k] != sprite_D[k]) {
    if (frame_counter & 0x1f)
      return false;
    if (!((sprite_head_dir[k] ^ sprite_D[k]) & 2)) {
      sprite_D[k] = (((k ^ frame_counter) >> 5 | 2) & 3) ^ (sprite_head_dir[k] & 2);
      return false;
    }
  }
  sprite_D[k] = sprite_head_dir[k];
  return true;
}

// Draws n OAM entries for sprite k from a DrawMultipleData array src.
// Calls Sprite_PrepOamCoordOrDoubleRet to compute the base screen coordinates;
// returns immediately if the sprite is off-screen or should double-return.
// word_7E0CFE controls palette priority override:
//   - If the sprite is being carried (state 10), use sprite_unk4[k] as the state
//     check to inherit the carrier's visual priority.
//   - If in state 11 (stunned/thrown) and sprite_unk5[k] is set, force the OAM
//     palette to 0x400 (clears the upper three palette bits) for the frozen look.
// Each entry combines the DrawMultipleData char_flags with the prepared OAM
// flags from info->r4, applying the optional palette override before writing.
void Sprite_DrawMultiple(int k, const DrawMultipleData *src, int n, PrepOamCoordsRet *info) {  // 85df6c
  PrepOamCoordsRet info_buf;
  if (!info)
    info = &info_buf;
  if (Sprite_PrepOamCoordOrDoubleRet(k, info))
    return;
  word_7E0CFE = 0;
  uint8 a = sprite_state[k];
  if (a == 10)
    a = sprite_unk4[k];
  if (a == 11)
    BYTE(word_7E0CFE) = sprite_unk5[k];
  OamEnt *oam = GetOamCurPtr();
  do {
    uint16 d = src->char_flags ^ WORD(info->r4);
    if (word_7E0CFE >= 1)
      d = d & ~0xE00 | 0x400;
    SetOamHelper0(oam, src->x + info->x, src->y + info->y, d, d >> 8, src->ext);

  } while (src++, oam++, --n);
}

// Variant of Sprite_DrawMultiple that ensures the sprite is rendered behind Link
// by allocating its OAM entries in the deferred-to-player region first.
// Used for sprites that should appear underneath Link (e.g., floor items, shadows).
void Sprite_DrawMultiplePlayerDeferred(int k, const DrawMultipleData *src, int n, PrepOamCoordsRet *info) {  // 85df75
  Oam_AllocateDeferToPlayer(k);
  Sprite_DrawMultiple(k, src, n, info);
}

// Shows a solicited dialogue message when Link presses the Y button while facing
// sprite k. Sets dialogue_message_index to msg, then performs a series of guards:
//   - Link must be in the same layer/hitbox as the sprite.
//   - Link must not be busy (carrying, swimming, falling, etc.).
//   - The Y button (0x80 in filtered_joypad_L) must have just been pressed.
//   - sprite_delay_aux4[k] must be 0 (cooldown between repeated talks).
//   - link_auxiliary_state must not be 2 (hanging on a ledge).
// Then checks that Link is facing the sprite before calling ShowMessageUnconditional.
// Returns the new facing direction (XOR 0x103 = flip direction bit + set talk flag),
// or sprite_D[k] unchanged if none of the conditions were met.
int Sprite_ShowSolicitedMessage(int k, uint16 msg) {  // 85e1a7
  static const uint8 kShowMessageFacing_Tab0[4] = {4, 6, 0, 2};
  dialogue_message_index = msg;
  if (!Sprite_CheckDamageToLink_same_layer(k) ||
      Sprite_CheckIfLinkIsBusy() ||
      !(filtered_joypad_L & 0x80) ||
      sprite_delay_aux4[k] || link_auxiliary_state == 2)
    return sprite_D[k];
  uint8 dir = Sprite_DirectionToFaceLink(k, NULL);
  if (link_direction_facing != kShowMessageFacing_Tab0[dir])
    return sprite_D[k];
  Sprite_ShowMessageUnconditional(dialogue_message_index);
  sprite_delay_aux4[k] = 64;
  return dir ^ 0x103;
}

// Shows a dialogue message whenever sprite k physically contacts Link (no button
// press required). Guards only against Link being on a different layer and
// link_auxiliary_state == 2 (ledge hang). On contact, calls ShowMessageUnconditional
// and returns the direction the sprite should face toward Link (with talk flag).
int Sprite_ShowMessageOnContact(int k, uint16 msg) {  // 85e1f0
  dialogue_message_index = msg;
  if (!Sprite_CheckDamageToLink_same_layer(k) || link_auxiliary_state == 2)
    return sprite_D[k];
  Sprite_ShowMessageUnconditional(dialogue_message_index);
  return Sprite_DirectionToFaceLink(k, NULL) ^ 0x103;
}

// Unconditionally launches the messaging module with the given message index.
// Freezes Link in place (speed = 0, cancels dash, clears incapacitation timer),
// resets NPC hookshot drag, sets main_module_index = 14 (messaging), and saves
// the current module for return. Also resets link_auxiliary_state and, if Link
// was recoiling from a wall, returns him to the Ground state so he doesn't keep
// sliding during the dialogue.
void Sprite_ShowMessageUnconditional(uint16 msg) {  // 85e219
  dialogue_message_index = msg;
  byte_7E0223 = 0;
  messaging_module = 0;
  submodule_index = 2;
  saved_module_for_menu = main_module_index;
  main_module_index = 14;
  Sprite_NullifyHookshotDrag();
  link_speed_setting = 0;
  Link_CancelDash();
  link_auxiliary_state = 0;
  link_incapacitated_timer = 0;
  if (link_player_handler_state == kPlayerState_RecoilWall)
    link_player_handler_state = kPlayerState_Ground;
}

// Contact-triggered message for the castle tutorial guard, using a wider hitbox
// than the guard's actual sprite dimensions. Temporarily overrides sprite_flags2
// to 0x80 (large hitbox marker) and sprite_flags4 low bits to 0x07 (widest
// hitbox preset) so that the contact check catches Link even when not pixel-perfect.
// Restores original flags after the check. On contact: stops Link's run, zeroes
// speed, and calls ShowMessageMinimal (which does not freeze input like the full
// version). Returns whether contact was detected so the caller can branch on it.
bool Sprite_TutorialGuard_ShowMessageOnContact(int k, uint16 msg) {  // 85fa59
  dialogue_message_index = msg;
  uint8 bak2 = sprite_flags2[k];
  uint8 bak4 = sprite_flags4[k];
  sprite_flags2[k] = 0x80;
  sprite_flags4[k] = 0x07;
  bool rv = Sprite_CheckDamageToLink_same_layer(k);
  sprite_flags2[k] = bak2;
  sprite_flags4[k] = bak4;
  if (!rv)
    return rv;
  Sprite_NullifyHookshotDrag();
  link_is_running = 0;
  link_speed_setting = 0;
  if (!link_auxiliary_state)
    Sprite_ShowMessageMinimal();
  return rv;
}

// Minimal version of the messaging launch: sets module 14 and submodule 2 to
// start a dialogue without the full Link-freeze logic of ShowMessageUnconditional.
// Used in contexts where Link should still be able to move (e.g., guard nudge)
// or where speed/state have already been handled by the caller.
void Sprite_ShowMessageMinimal() {  // 85fa8e
  byte_7E0223 = 0;
  messaging_module = 0;
  submodule_index = 2;
  saved_module_for_menu = main_module_index;
  main_module_index = 14;
}

// Builds an earthquake shockwave hitbox in front of Link and calls
// Entity_ApplyRumbleToSprites to push all overlapping sprites.
// kApplyRumble_X/Y offsets the hitbox centre based on Link's facing direction
// (link_direction_facing >> 1 → 0=up, 1=left, 2=down, 3=right).
// kApplyRumble_WH provides separate width/height dimensions for horizontal vs
// vertical facings so the shockwave is wider in the direction of travel.
// The resulting SpriteHitBox is passed to Entity_ApplyRumbleToSprites which
// applies a velocity impulse to every active sprite within the box.
void Prepare_ApplyRumbleToSprites() {  // 8680fa
  static const int8 kApplyRumble_X[4] = { -32, -32, -32, 16 };
  static const int8 kApplyRumble_Y[4] = { -32, 32, -24, -24 };
  static const uint8 kApplyRumble_WH[6] = { 0x50, 0x50, 0x20, 0x20, 0x50, 0x50 };
  int j = link_direction_facing >> 1;
  SpriteHitBox hb;
  uint16 x = link_x_coord + kApplyRumble_X[j];
  uint16 y = link_y_coord + kApplyRumble_Y[j];
  hb.r0_xlo = x;
  hb.r8_xhi = x >> 8;
  hb.r1_ylo = y;
  hb.r9_yhi = y >> 8;
  hb.r2 = kApplyRumble_WH[j];
  hb.r3 = kApplyRumble_WH[j + 2];
  Entity_ApplyRumbleToSprites(&hb);
}

// Spawns a throwable terrain tile (type 0xEC) at (x, y) and immediately
// converts it to debris via ThrowableScenery_TransmuteToDebris. Used when
// the game needs to play the "smashed pot/skull" scatter animation without
// ever showing the intact object. Saves and restores the global pick-up flag
// and byte_7E0FB2 so the spawned sprite does not corrupt Link's carry state.
void Sprite_SpawnImmediatelySmashedTerrain(uint8 what, uint16 x, uint16 y) {  // 86812d
  uint8 bak1 = flag_is_sprite_to_pick_up;
  uint8 bak2 = byte_7E0FB2;
  int k = Sprite_SpawnThrowableTerrain_silently(what, x, y);
  if (k >= 0)
    ThrowableScenery_TransmuteToDebris(k);
  byte_7E0FB2 = bak2;
  flag_is_sprite_to_pick_up = bak1;
}

// Spawns a throwable terrain tile (type 0xEC) at (x, y) with a pick-up sound
// effect (sound_effect_1 = pan | 29). The sound gives audio feedback at the
// moment Link lifts a pot, bush, or skull. Delegates to the silent variant.
void Sprite_SpawnThrowableTerrain(uint8 what, uint16 x, uint16 y) {  // 86814b
  sound_effect_1 = Link_CalculateSfxPan() | 29;
  Sprite_SpawnThrowableTerrain_silently(what, x, y);
}

// Core throwable terrain spawner: finds the first free sprite slot (scanning
// down from slot 15), sets it up as a type 0xEC (throwable scenery) at (x, y),
// calls SpritePrep_LoadProperties to fill in the standard init tables, then
// applies what-specific appearance flags from kThrowableScenery_Flags[what].
//   what: tile variety index (0=bush, 1=pot, 2=skull, 3=heavy rock, 4=sign,
//         5=ice block; values ≥ 6 set sprite_flags2 to 0xa6 for heavy objects).
// If dung_secrets_unk1 is not 0xFF, calls Sprite_SpawnSecret to place a hidden
// item (rupee, heart, bomb, etc.) beneath the terrain. Also handles the alternate
// overworld secret substitution for outdoor tiles.
// Returns the slot index k, or -1 if all 16 slots were occupied.
int Sprite_SpawnThrowableTerrain_silently(uint8 what, uint16 x, uint16 y) {  // 868156
  int k = 15;
  for (; k >= 0 && sprite_state[k] != 0; k--);
  if (k < 0)
    return k;
  sprite_state[k] = 10;
  sprite_type[k] = 0xEC;
  Sprite_SetX(k, x);
  Sprite_SetY(k, y);
  SpritePrep_LoadProperties(k);
  sprite_floor[k] = link_is_on_lower_level;
  sprite_C[k] = what;
  if (what >= 6)
    sprite_flags2[k] = 0xa6;
  // oob read, this array has only 6 elements.
  uint8 flags = kThrowableScenery_Flags[what];
  if (what == 2) {
    if (player_is_indoors)
      sprite_oam_flags[k] = 0x80, flags = 0x50;  // wtf
  }
  sprite_oam_flags[k] = flags;
  sprite_unk4[k] = 9;
  flag_is_sprite_to_pick_up = 2;
  byte_7E0FB2 = 2;
  sprite_delay_main[k] = 16;
  sprite_floor[k] = link_is_on_lower_level;
  sprite_graphics[k] = 0;
  if (BYTE(dung_secrets_unk1) != 255) {
    if (!(BYTE(dung_secrets_unk1) | player_is_indoors) && (uint8)(sprite_C[k] - 2) < 2)
      Overworld_SubstituteAlternateSecret();
    if (dung_secrets_unk1 & 0x80) {
      sprite_graphics[k] = dung_secrets_unk1 & 0x7f;
      BYTE(dung_secrets_unk1) = 0;
    }
    Sprite_SpawnSecret(k);
  }
  return k;
}

// Spawns the hidden item concealed beneath the terrain tile at slot k.
// dung_secrets_unk1 encodes the item type (1-based index into kSpawnSecretItems).
// Type 4 is "random rupee" — replaced by one of secret item types 20-22.
// Outdoors, there is a 50 % chance (GetRandomNumber() & 8) of no drop.
// After spawning, configures the child sprite's AI state, ignore-projectile flag,
// Z velocity, X/Y position, and graphics from the kSpawnSecretItem_* parallel arrays.
// Type-specific post-init:
//   0xE4 (small key): calls SpritePrep_SmallKey and marks it immediately available.
//   0x0B (bomb)     : plays sound 0x30; inside room 1 uses subtype 1 (super bomb).
//   0x41/0x42 (rupee variants): plays sound 4, sets invulnerability timer to 160.
//   0x3E (heart piece): sets a specific OAM palette flag.
//   0x79 (faerie)   : sets A field = 32 for faerie wander range.
void Sprite_SpawnSecret(int k) {  // 868264
  if (!player_is_indoors && (GetRandomNumber() & 8))
    return;
  int b = BYTE(dung_secrets_unk1);
  if (b == 0)
    return;
  if (b == 4)
    b = 19 + (GetRandomNumber() & 3);
  if (!kSpawnSecretItems[b - 1])
    return;
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(k, kSpawnSecretItems[b - 1], &info);
  if (j < 0)
    return;
  sprite_ai_state[j] = kSpawnSecretItem_SpawnFlag[b - 1];
  sprite_ignore_projectile[j] = kSpawnSecretItem_IgnoreProj[b - 1];
  sprite_z_vel[j] = kSpawnSecretItem_ZVel[b - 1];
  Sprite_SetX(j, info.r0_x + kSpawnSecretItem_XLo[b - 1]);
  Sprite_SetY(j, info.r2_y);
  sprite_z[j] = info.r4_z;
  sprite_graphics[j] = 0;
  sprite_delay_aux4[j] = 32;
  sprite_delay_aux2[j] = 48;
  uint8 type = sprite_type[j];
  if (type == 0xe4) {
    SpritePrep_SmallKey(j);
    sprite_stunned[j] = 255;
  } else if (type == 0xb) {
    sound_effect_1 = 0x30;
    if (BYTE(dungeon_room_index2) == 1)
      sprite_subtype[j] = 1;
    sprite_stunned[j] = 255;
  } else if (type == 0x41 || type == 0x42) {
    sound_effect_2 = 4;
    sprite_give_damage[j] = 0;
    sprite_hit_timer[j] = 160;
  } else if (type == 0x3e) {
    sprite_oam_flags[j] = 9;
  } else {
    sprite_stunned[j] = 255;
    if (type == 0x79)
      sprite_A[j] = 32;
  }
}

// Master per-frame sprite system entry point, called once per game tick from the
// NMI/vblank handler after the PPU update. Execution order:
//   1. Overworld only: reset the five ancilla floor bytes and call
//      Sprite_ProximityActivation to activate any sprites now near the screen edge.
//   2. Refresh is_in_dark_world from savegame_is_darkworld.
//   3. Zero drag accumulators if not in a submodule (submodule_index == 0).
//   4. Oam_ResetRegionBases — restore the six OAM region pointers.
//   5. Garnish_ExecuteUpperSlots — run garnish slots 15-29 (drawn above sprites).
//   6. Follower_Main — update the player's follower (Zelda, Blind, etc.).
//   7. Capture and clear flag_is_sprite_to_pick_up for this frame's carry logic.
//   8. Reset dungmap_var8 high byte, decrement set_when_damaging_enemies,
//      zero byte_7E0379, link_unk_master_sword, link_prevent_from_moving.
//   9. Decrement sprite_alert_flag if set.
//  10. Ancilla_Main — update all ancilla (projectiles, items, effects).
//  11. Overlord_Main — update all overlords (room event coordinators).
//  12. Reset archery_game_out_of_arrows flag.
//  13. Main sprite loop: for i = 15 down to 0, set cur_object_index = i and
//      call Sprite_ExecuteSingle(i) to dispatch each sprite through its state.
//  14. Garnish_ExecuteLowerSlots — run garnish slots 0-14 (drawn below sprites).
//  15. Clear byte_7E069E[0..1] (per-frame collision scratch).
//  16. ExecuteCachedSprites — run door-transition cached sprites.
//  17. If load_chr_halfslot_even_odd is set, copy it to byte_7E0FC6 for the
//      CHR half-slot swap request.
void Sprite_Main() {  // 868328
  if (!player_is_indoors) {
    ancilla_floor[0] = 0;
    ancilla_floor[1] = 0;
    ancilla_floor[2] = 0;
    ancilla_floor[3] = 0;
    ancilla_floor[4] = 0;
    Sprite_ProximityActivation();
  }
  is_in_dark_world = (savegame_is_darkworld != 0);
  if (submodule_index == 0)
    drag_player_x = drag_player_y = 0;
  Oam_ResetRegionBases();
  Garnish_ExecuteUpperSlots();
  Follower_Main();
  byte_7E0FB2 = flag_is_sprite_to_pick_up;
  flag_is_sprite_to_pick_up = 0;
  HIBYTE(dungmap_var8) = 0x80;

  if (set_when_damaging_enemies & 0x7f)
    set_when_damaging_enemies--;
  else
    set_when_damaging_enemies = 0;
  byte_7E0379 = 0;
  link_unk_master_sword = 0;
  link_prevent_from_moving = 0;
  if (sprite_alert_flag)
    sprite_alert_flag--;
  Ancilla_Main();
  Overlord_Main();
  archery_game_out_of_arrows = 0;
  for (int i = 15; i >= 0; i--) {
    cur_object_index = i;
    Sprite_ExecuteSingle(i);
  }
  Garnish_ExecuteLowerSlots();
  byte_7E069E[0] = byte_7E069E[1] = 0;
  ExecuteCachedSprites();
  if (load_chr_halfslot_even_odd)
    byte_7E0FC6 = load_chr_halfslot_even_odd;
}

// Restores all six OAM region base pointers to their defaults from the constant
// kOam_ResetRegionBases table. Called at the start of every Sprite_Main to undo
// any per-sprite region adjustments made during the previous frame.
void Oam_ResetRegionBases() {  // 8683d3
  memcpy(oam_region_base, kOam_ResetRegionBases, 12);
}

// Per-sprite housekeeping called by Sprite_ExecuteSingle for every active sprite.
// Performs three duties each frame:
//
// 1. Coordinate snapshot: calls Sprite_Get16BitCoords to assemble cur_sprite_x/y
//    from the lo/hi byte pairs for use by OAM placement routines.
//
// 2. OAM slot reservation: computes the number of 8×8 OAM entries this sprite
//    needs — ((sprite_flags2[k] & 0x1f) + 1) * 4 — and allocates them from the
//    appropriate OAM region based on sort_sprites_setting and sprite_floor[k]:
//      - Region A: unsorted mode (always, regardless of floor).
//      - Region D: sorted mode, lower floor (floor == 0).
//      - Region F: sorted mode, upper floor (floor != 0).
//
// 3. Timer countdown (only when neither submodule_index nor flag_unk1 is set):
//      - Decrements sprite_delay_main, delay_aux1, delay_aux2, delay_aux3 if > 0.
//      - Manages sprite_hit_timer: at 31 calls Sprite_HitTimer31 (Agahnim warning);
//        at 24 calls Sprite_MiniMoldorm_Recoil (small Moldorm recoil check).
//        While hit_timer > 0, updates sprite_obj_prio with a flash value
//        (hit_timer * 2 & 0xe) for the damage-flash effect, then decrements.
//      - Decrements sprite_delay_aux4 (talk-message cooldown) if > 0.
//
// 4. Priority merge: applies the floor-based OAM priority bits from kSpritePrios
//    into sprite_obj_prio, preserving any hit-flash bits in the low nibble.
//    If link_is_on_lower_level == 3 (cutscene floor override), uses that value
//    rather than sprite_floor[k].
void Sprite_TimersAndOam(int k) {  // 8683f2
  Sprite_Get16BitCoords(k);

  uint8 num = ((sprite_flags2[k] & 0x1f) + 1) * 4;

  if (sort_sprites_setting) {
    if (sprite_floor[k])
      Oam_AllocateFromRegionF(num);
    else
      Oam_AllocateFromRegionD(num);
  } else {
    Oam_AllocateFromRegionA(num);
  }

  if (!(submodule_index | flag_unk1)) {
    if (sprite_delay_main[k])
      sprite_delay_main[k]--;
    if (sprite_delay_aux1[k])
      sprite_delay_aux1[k]--;
    if (sprite_delay_aux2[k])
      sprite_delay_aux2[k]--;
    if (sprite_delay_aux3[k])
      sprite_delay_aux3[k]--;

    uint8 timer = sprite_hit_timer[k] & 0x7f;
    if (timer) {
      if (sprite_state[k] >= 9) {
        if (timer == 31) {
          Sprite_HitTimer31(k);
        } else if (timer == 24) {
          Sprite_MiniMoldorm_Recoil(k);
        }
      }
      if (sprite_give_damage[k] < 251)
        sprite_obj_prio[k] = sprite_hit_timer[k] * 2 & 0xe;
      sprite_hit_timer[k]--;
    } else {
      sprite_hit_timer[k] = 0;
      sprite_obj_prio[k] = 0;
    }
    if (sprite_delay_aux4[k])
      sprite_delay_aux4[k]--;
  }

  static const uint8 kSpritePrios[4] = {0x20, 0x10, 0x30, 0x30};
  int floor = link_is_on_lower_level;
  if (floor != 3)
    floor = sprite_floor[k];
  sprite_obj_prio[k] = sprite_obj_prio[k] & 0xcf | kSpritePrios[floor];
}

// Assembles the full 16-bit world coordinates for sprite k into the globals
// cur_sprite_x and cur_sprite_y from the split lo/hi byte arrays. These globals
// are used by OAM placement and screen-space culling routines throughout the frame.
void Sprite_Get16BitCoords(int k) {  // 8684c1
  cur_sprite_x = sprite_x_lo[k] | sprite_x_hi[k] << 8;
  cur_sprite_y = sprite_y_lo[k] | sprite_y_hi[k] << 8;
}

// Per-frame dispatch for a single sprite slot k. If the sprite is not inactive
// (state != 0), calls Sprite_TimersAndOam for housekeeping and OAM allocation
// first, then dispatches through kSprite_ExecuteSingle[state] — a 12-entry
// function pointer table that maps each state (0-11) to its handler:
//   0  = Sprite_inactiveSprite   (slot unused)
//   1  = SpriteModule_Fall1      (falling into pit, first phase)
//   2  = SpriteModule_Poof       (poof/appear animation)
//   3  = SpriteModule_Drown      (drowning in water/lava)
//   4  = SpriteModule_Explode    (boss explosion sequence)
//   5  = SpriteModule_Fall2      (falling into pit, second phase)
//   6  = SpriteModule_Die        (death poof wrapper)
//   7  = SpriteModule_Burn       (fire death animation)
//   8  = SpriteModule_Init       (first-frame initialisation)
//   9  = SpriteActive_Main       (normal active AI tick)
//  10  = SpriteModule_Carried    (Link is carrying the sprite)
//  11  = SpriteModule_Stunned    (stunned / thrown)
void Sprite_ExecuteSingle(int k) {  // 8684e2
  uint8 st = sprite_state[k];
  if (st != 0)
    Sprite_TimersAndOam(k);
  kSprite_ExecuteSingle[st](k);
}

// State 0 handler — sprite slot is inactive (not in use).
// Resets the proximity-tracking word (or byte) so the overworld activation
// scan can reload the slot when Link walks close enough again:
//   Outdoors: sprite_N_word[k] = 0xFFFF (marks both bytes as unoccupied).
//   Indoors : sprite_N[k]      = 0xFF   (marks the single room-list byte).
void Sprite_inactiveSprite(int k) {  // 868510
  if (!player_is_indoors) {
    sprite_N_word[k] = 0xffff;
  } else {
    sprite_N[k] = 0xff;
  }
}

// State 1 handler — sprite is falling into a pit (first phase).
// When sprite_delay_main[k] reaches 0, deactivates the sprite (state = 0)
// and calls Sprite_ManuallySetDeathFlagUW to mark the slot as dead in the
// dungeon room flags so the sprite does not respawn on re-entry.
// While delay is still counting down, calls SpriteFall_Draw to show the
// shrinking/rotating fall animation tiles at the pit's screen position.
void SpriteModule_Fall1(int k) {  // 86852e
  if (!sprite_delay_main[k]) {
    sprite_state[k] = 0;
    Sprite_ManuallySetDeathFlagUW(k);
  } else {
    PrepOamCoordsRet info;
    if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
      return;
    SpriteFall_Draw(k, &info);
  }
}

// State 3 handler — sprite is drowning in water, quicksand, or lava.
// Two-phase animation controlled by sprite_ai_state[k]:
//
// Phase 0 (ai_state == 0): sink animation — the sprite's tile ripple plays.
//   kSpriteDrown_Dmd[0..7] holds 4 pairs of wave tiles (selected by delay << 1 & 0xf8).
//   Every other frame (frame_counter & 1), delay_main counts up.
//   When delay_main == 0 the sprite is deactivated. sprite_hit_timer and oam_flags
//   are zeroed so the sinking sprite cannot deal contact damage.
//
// Phase 1 (ai_state != 0): splash/bounce — sprite arcs upward then falls back.
//   sprite_A[k] == 6 allocates extra OAM space (region C, 8 bytes).
//   sprite_flags3[k] bit 4 is toggled each frame to flash the tile.
//   SpriteDraw_SingleLarge draws the sprite tile; oam->charnum is overridden from
//   kSpriteDrown_Oam_Char[delay >> 1] to show progressively larger splash rings.
//   kSpriteDrown_Oam_Flags selects the horizontal flip for the spinning effect
//   (based on sprite_subtype2[k] >> 2 & 3 as a slow rotation counter).
//   Gravity is applied each frame (z_vel -= 2); when sprite_z goes negative the
//   sprite lands (z = 0), switches to the sink timer (delay_main = 18) and
//   clears the shadow-enable flag.
void SpriteModule_Drown(int k) {  // 86859c
  static const DrawMultipleData kSpriteDrown_Dmd[8] = {
    {-7, -7, 0x0480, 0},
    {14, -6, 0x0483, 0},
    {-6, -6, 0x04cf, 0},
    {13, -5, 0x04df, 0},
    {-4, -4, 0x04ae, 0},
    {12, -4, 0x44af, 0},
    { 0,  0, 0x04e7, 2},
    { 0,  0, 0x04e7, 2},
  };
  // Horizontal flip variants for the four 90-degree rotation steps of the splash.
  static const uint8 kSpriteDrown_Oam_Flags[4] = {0, 0x40, 0xc0, 0x80};
  // 11-entry char table indexed by delay >> 1; early frames reuse 0xC0, later
  // frames advance to 0xCD then 0xCB for the concentric ring progression.
  static const uint8 kSpriteDrown_Oam_Char[11] = {0xc0, 0xc0, 0xc0, 0xc0, 0xcd, 0xcd, 0xcd, 0xcb, 0xcb, 0xcb, 0xcb};

  if (sprite_ai_state[k]) {
    if (sprite_A[k] == 6)
      Oam_AllocateFromRegionC(8);
    sprite_flags3[k] ^= 16;
    SpriteDraw_SingleLarge(k);
    OamEnt *oam = GetOamCurPtr();
    int j = sprite_delay_main[k];
    if (j == 1)
      sprite_state[k] = 0;
    if (j != 0) {
      assert((j >> 1) < 11);
      oam->charnum = kSpriteDrown_Oam_Char[j >> 1];
      oam->flags = 0x24;
      return;
    }
    oam->charnum = 0x8a;
    oam->flags = kSpriteDrown_Oam_Flags[sprite_subtype2[k] >> 2 & 3] | 0x24;

    if (Sprite_ReturnIfPaused(k))
      return;
    sprite_subtype2[k]++;
    Sprite_MoveXY(k);
    Sprite_MoveZ(k);
    sprite_z_vel[k] -= 2;
    if (sign8(sprite_z[k])) {
      sprite_z[k] = 0;
      sprite_delay_main[k] = 18;
      sprite_flags3[k] &= ~0x10;
    }
  } else {
    if (Sprite_ReturnIfPaused(k))
      return;
    if (!(frame_counter & 1))
      sprite_delay_main[k]++;
    sprite_oam_flags[k] = 0;
    sprite_hit_timer[k] = 0;
    if (!sprite_delay_main[k])
      sprite_state[k] = 0;
    Sprite_DrawMultiple(k, &kSpriteDrown_Dmd[(sprite_delay_main[k] << 1 & 0xf8) >> 2], 2, NULL);
  }
}

// Draws a four-tile distress indicator (exclamation/alert marks) at world position
// (xin, yin) using the kSpriteDistress_X/Y offset tables. Only renders when
// time & 0x18 is non-zero, providing a blinking effect at the caller's pace.
// Used for alert markers above guards or enemies that spot Link.
void Sprite_DrawDistress_custom(uint16 xin, uint16 yin, uint8 time) {  // 86a733
  Oam_AllocateFromRegionA(0x10);
  if (!(time & 0x18))
    return;
  int i = 3;
  OamEnt *oam = GetOamCurPtr();
  do {
    SetOamHelper0(oam, xin + kSpriteDistress_X[i], yin + kSpriteDistress_Y[i],
                  0x83, 0x22, 0);
  } while (oam++, --i >= 0);
}

// Thin wrapper that calls the permissive variant of the lifted-check helper.
// The permissive version allows the sprite to be lifted even under conditions
// (e.g., during submodule) that the strict variant would reject.
void Sprite_CheckIfLifted_permissive(int k) {  // 86aa0c
  Sprite_ReturnIfLiftedPermissive(k);
}

// Applies a velocity impulse to every sprite that is within the earthquake
// shockwave hitbox hb and has the rumble-susceptible flag set (sprite_defl_bits[j] & 2).
// Only sprites with sprite_E[j] != 0 (grounded / not already airborne) are affected.
// If byte_7E0FC6 == 0x0E (special CHR mode used during the Earthquake cutscene),
// skips the individual AABB check and flings every eligible sprite regardless of
// position (total-screen rumble). For each hit sprite:
//   - Clears sprite_E[j] (launches it into the air).
//   - Plays sound 0x30 (thud / heavy impact).
//   - Sets z_vel = 0x30 (upward launch), x_vel = 0x10 (rightward drift),
//     delay_aux3 = 0x30, and stunned = 255 (indefinite stun).
//   - If the sprite is type 0xD8 (bomb), transmutes it to a live bomb.
void Entity_ApplyRumbleToSprites(SpriteHitBox *hb) {  // 86ad03
  for (int j = 15; j >= 0; j--) {
    if (!(sprite_defl_bits[j] & 2) || sprite_E[j] == 0)
      continue;
    if (byte_7E0FC6 != 0xe) {
      Sprite_SetupHitBox(j, hb);
      if (!CheckIfHitBoxesOverlap(hb))
        continue;
    }
    sprite_E[j] = 0;
    sound_effect_2 = 0x30;
    sprite_z_vel[j] = 0x30;
    sprite_x_vel[j] = 0x10;
    sprite_delay_aux3[j] = 0x30;
    sprite_stunned[j] = 255;
    if (sprite_type[j] == 0xd8)
      Sprite_TransmuteToBomb(j);
  }
}

// Immediately zeroes both the X and Y velocity components of sprite k.
// Used to stop a sprite in place when it should halt without a bounce or slide.
void Sprite_ZeroVelocity_XY(int k) {  // 86cf5d
  sprite_x_vel[k] = sprite_y_vel[k] = 0;
}

// Updates sprite k's position to track the ancilla (hookshot / boomerang) that
// is currently dragging it, identified by sprite_B[k] - 1.
// If sprite_B[k] == 0, there is no active drag and the function returns false.
// If the dragging ancilla has been destroyed (type == 0), triggers absorption by
// the player (the sprite was pulled to Link and collected).
// Otherwise, copies the ancilla's X/Y coordinates into the sprite's lo/hi pairs
// and zeroes sprite_z so the sprite stays flat on the ground while being reeled in.
// Returns true if drag was active (caller should skip normal movement logic).
bool Sprite_HandleDraggingByAncilla(int k) {  // 86cf64
  int j = sprite_B[k];
  if (j-- == 0)
    return false;
  if (ancilla_type[j] == 0) {
    Sprite_HandleAbsorptionByPlayer(k);
  } else {
    sprite_x_lo[k] = ancilla_x_lo[j];
    sprite_x_hi[k] = ancilla_x_hi[j];
    sprite_y_lo[k] = ancilla_y_lo[j];
    sprite_y_hi[k] = ancilla_y_hi[j];
    sprite_z[k] = 0;
  }
  return true;
}

// Returns true and drives a "phasing out" (dissolve) visual effect while
// sprite_stunned[k] counts down to zero. Used for item sprites that disappear
// after being collected or time out (e.g., faerie bottles, keys).
// Each even frame the stunned counter decrements by 1 (half-speed countdown).
// While stunned >= 0x28 (first 40 counts) or odd, the sprite is fully visible
// (returns false so the caller continues drawing normally).
// Below 0x28 and on even counts, calls Sprite_PrepOamCoordOrDoubleRet to blank
// the OAM slot (creates the flickering disappear effect) and returns true.
// When stunned reaches 0, deactivates the sprite (state = 0).
bool Sprite_ReturnIfPhasingOut(int k) {  // 86d0ed
  if (!sprite_stunned[k] || (submodule_index | flag_unk1))
    return false;
  if (!(frame_counter & 1))
    sprite_stunned[k]--;
  uint8 a = sprite_stunned[k];
  if (a == 0)
    sprite_state[k] = 0;
  else if (a >= 0x28 || (a & 1) != 0)
    return false;
  PrepOamCoordsRet info;
  Sprite_PrepOamCoordOrDoubleRet(k, &info);
  return true;
}

// Checks whether Link is close enough to collect absorbable sprite k (no button
// press needed). sprite_delay_aux4[k] acts as a brief post-spawn immunity window
// so the sprite cannot be immediately absorbed before it lands. When both the
// immunity and the contact check pass, calls Sprite_HandleAbsorptionByPlayer.
void Sprite_CheckAbsorptionByPlayer(int k) {  // 86d116
  if (!sprite_delay_aux4[k] && Sprite_CheckDamageToPlayer_1(k))
    Sprite_HandleAbsorptionByPlayer(k);
}

// Handles Link collecting an absorbable pickup sprite (rupee, heart, bomb, etc.).
// Deactivates the sprite (state = 0), plays its type-specific pick-up SFX from
// kAbsorptionSfx, then adds the appropriate resource to Link's inventory:
//   case 0  (heart drop)      : link_hearts_filler += 8
//   case 1-3 (rupees 1/5/20)  : link_rupees_goal  += kRupeesAbsorption[t-1]
//   case 4-6 (bombs 1/4/8)    : link_bomb_filler  += kBombsAbsorption[t-4]
//   case 7  (magic small)     : link_magic_filler += 0x10
//   case 8  (magic full)      : link_magic_filler = 0x80
//   case 9  (arrows variable) : link_arrow_filler += sprite_head_dir or 1/5 min
//   case 10 (arrows 10)       : link_arrow_filler += 10
//   case 11 (big heart)       : queues SFX 0x31, link_hearts_filler += 56
//   case 12 (small key)       : link_num_keys++; marks room flag; sets dungeon save bit
//   case 13 (big key)         : Link_ReceiveItem(0x32), marks room flag + dungeon save
//   case 14 (shield)          : link_shield_type = sprite_subtype[k]; reloads palette
void Sprite_HandleAbsorptionByPlayer(int k) {  // 86d13c
  sprite_state[k] = 0;
  int t = sprite_type[k] - 0xd8;
  SpriteSfx_QueueSfx3WithPan(k, kAbsorptionSfx[t]);
  switch(t) {
  case 0:
    link_hearts_filler += 8;
    break;
  case 1: case 2: case 3:
    link_rupees_goal += kRupeesAbsorption[t - 1];
    break;
  case 4: case 5: case 6:
    link_bomb_filler += kBombsAbsorption[t - 4];
    break;
  case 7:
    link_magic_filler += 0x10;
    break;
  case 8:
    link_magic_filler = 0x80;
    break;
  case 9:
    link_arrow_filler += (sprite_head_dir[k] == 0) ? 5 : sprite_head_dir[k];
    break;
  case 10:
    link_arrow_filler += 10;
    break;
  case 11:
    SpriteSfx_QueueSfx2WithPan(k, 0x31);
    link_hearts_filler += 56;
    break;
  case 12:
    link_num_keys += 1;
    goto after_getkey;
  case 13:
    item_receipt_method = 0;
    Link_ReceiveItem(0x32, 0);
  after_getkey:
    sprite_N[k] = sprite_subtype[k];
    dung_savegame_state_bits |= kAbsorbBigKey[sprite_die_action[k]] << 8;
    Sprite_ManuallySetDeathFlagUW(k);
    break;
  case 14:
    link_shield_type = sprite_subtype[k];
    // Shield needs to have the right palette after pikit
    if (enhanced_features0 & kFeatures0_MiscBugFixes)
      Palette_Load_Shield();
    break;
  }
}

// Draws an absorbable pickup sprite (rupee, heart, bomb, key, etc.) with optional
// transient (phase-out) support.
//   transient == true: calls Sprite_ReturnIfPhasingOut; if phasing, returns false
//     (skip further drawing — the sprite is flickering out).
// Priority override: in unsorted indoor mode, force OAM priority 0x30 (foreground)
// so items appear above floor tiles.
// Returns false immediately if byte_7E0FC6 >= 3 (CHR-swap in progress; skip draw).
// If sprite_delay_aux2[k] != 0 (bounce/spawn delay active), allocates 12 extra OAM
// bytes from region C (for the entrance animation shadow).
// If sprite_E[k] != 0, the pickup is hidden (under a rock, hookshot-lifted):
//   - With MiscBugFixes: clears sprite_B[k] to cancel any stale hookshot reference.
//   - Returns true (skip OAM write but keep the sprite active).
// Otherwise dispatches to a draw sub-function based on kAbsorbable_Tab2 / Tab1:
//   Tab2 nonzero → Sprite_DrawNumberedAbsorbable (rupee with digit overlay).
//   Tab1 == 0    → SpriteDraw_SingleSmall (8×8 OAM tile).
//   Tab1 == 2    → SpriteDraw_SingleLarge (16×16 OAM tile); type 0xE6 sub 0 sets
//                  sprite_graphics = 1 for the alternate large-rupee tile.
//   Tab1 == other → Sprite_DrawThinAndTall (two vertically-stacked 8×8 tiles, for keys).
bool SpriteDraw_AbsorbableTransient(int k, bool transient) {  // 86d22f
  if (transient && Sprite_ReturnIfPhasingOut(k))
    return false;
  if (sort_sprites_setting == 0 && player_is_indoors != 0)
    sprite_obj_prio[k] = 0x30;
  if (byte_7E0FC6 >= 3)
    return false;
  if (sprite_delay_aux2[k] != 0)
    Oam_AllocateFromRegionC(12);
  if (sprite_E[k] != 0) {
    // This code runs when an absorbable is hidden under say a rock.
    // sprite_B holds the sprite that grabbed us with a hookshot.
    // Cancel the grab if we're hidden.
    if (enhanced_features0 & kFeatures0_MiscBugFixes)
      sprite_B[k] = 0;
    return true;
  }
  uint8 j = sprite_type[k];
  assert(j >= 0xd8 && j < 0xd8 + 19);
  uint8 a = kAbsorbable_Tab2[j - 0xd8];
  if (a != 0) {
    Sprite_DrawNumberedAbsorbable(k, a);
    return false;
  }
  uint8 t = kAbsorbable_Tab1[j - 0xd8];
  if (t == 0) {
    SpriteDraw_SingleSmall(k);
    return false;
  }
  if (t == 2) {
    if (sprite_type[k] == 0xe6) {
      if (sprite_subtype[k] == 1)
        goto draw_key;
      sprite_graphics[k] = 1;
    }
    SpriteDraw_SingleLarge(k);
    return false;
  }
draw_key:
  Sprite_DrawThinAndTall(k);
  return false;
}

// Draws an absorbable pickup that has a number overlay (e.g., "5" on a 5-rupee).
// a is the 1-based kAbsorbable_Tab2 variant index; multiplied by 3 to index into
// the parallel kNumberedAbsorbable_X/Y/Char/Ext tables which hold the multi-tile
// layout. sprite_head_dir[k] == 0 selects the two-tile (n=2) variant; nonzero
// uses only the single prominent tile (n=1). Draws a ground shadow after the tiles.
void Sprite_DrawNumberedAbsorbable(int k, int a) {  // 86d2fa
  a = (a - 1) * 3;
  PrepOamCoordsRet info;
  if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
    return;
  OamEnt *oam = GetOamCurPtr();
  int n = (sprite_head_dir[k] < 1) ? 2 : 1;
  do {
    int j = n + a;
    SetOamHelper0(oam,
                  info.x + kNumberedAbsorbable_X[j], info.y + kNumberedAbsorbable_Y[j],
                  kNumberedAbsorbable_Char[j], info.flags,
                  kNumberedAbsorbable_Ext[j]);
  } while (oam++, --n >= 0);
  SpriteDraw_Shadow(k, &info);
}

// Reflects sprite k's X velocity on left/right wall contact and Y velocity on
// up/down wall contact. sprite_wallcoll[k] bits 0-1 = horizontal walls,
// bits 2-3 = vertical walls. Used for simple elastic bouncing enemies.
void Sprite_BounceOffWall(int k) {  // 86d9c0
  if (sprite_wallcoll[k] & 3)
    sprite_x_vel[k] = -sprite_x_vel[k];
  if (sprite_wallcoll[k] & 12)
    sprite_y_vel[k] = -sprite_y_vel[k];
}

// Inverts both X and Y velocity simultaneously, reversing direction 180 degrees.
// Used for sprites that reverse completely on wall hit or player contact.
void Sprite_InvertSpeed_XY(int k) {  // 86d9d5
  sprite_x_vel[k] = -sprite_x_vel[k];
  sprite_y_vel[k] = -sprite_y_vel[k];
}

// Returns true if the sprite should not be processed this frame for any reason:
//   - Not in active state 9.
//   - flag_unk1 or submodule_index is set (game is paused / in a menu).
//   - sprite_defl_bits[k] bit 7 is clear AND sprite_pause[k] is nonzero
//     (sprite is individually paused, e.g., frozen by Cane of Somaria).
bool Sprite_ReturnIfInactive(int k) {  // 86d9ec
  return (sprite_state[k] != 9 || flag_unk1 || submodule_index || !(sprite_defl_bits[k] & 0x80) && sprite_pause[k]);
}

// Returns true if the sprite should skip movement and logic this frame because
// the game is in a paused/submodule state or the sprite is individually paused.
// Unlike Sprite_ReturnIfInactive, does not require state == 9, so it is safe
// to call from non-active states (e.g., during stunned/carried handlers).
bool Sprite_ReturnIfPaused(int k) {  // 86d9f3
  return (flag_unk1 || submodule_index || !(sprite_defl_bits[k] & 0x80) && sprite_pause[k]);
}

// Draws sprite k as a single 16×16 OAM tile. Calls Sprite_PrepOamCoordOrDoubleRet
// to compute screen coordinates; returns immediately if off-screen. The tile
// character number is looked up from the two-level table
// kSprite_PrepAndDrawSingleLarge_Tab2[Tab1[type] + graphics]. Extended OAM byte
// is set to 2 (16×16 size) with the X-overflow bit. Draws a ground shadow if
// sprite_flags3[k] bit 4 (shadow flag) is set.
void SpriteDraw_SingleLarge(int k) {  // 86dc10
  PrepOamCoordsRet info;
  if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
    return;
  Sprite_PrepAndDrawSingleLargeNoPrep(k, &info);
}

// Writes the OAM entry for a single 16×16 tile using a pre-computed PrepOamCoordsRet.
// The Y bounds check ((info->y + 0x10) < 0x100) suppresses writing when the
// sprite is above the top of the visible screen. Extended OAM bit 1 = 16×16 size;
// bit 0 = X coordinate overflow (x >= 256). Draws shadow when flags3 bit 4 set.
void Sprite_PrepAndDrawSingleLargeNoPrep(int k, PrepOamCoordsRet *info) {  // 86dc13
  OamEnt *oam = GetOamCurPtr();
  oam->x = info->x;
  if ((uint16)(info->y + 0x10) < 0x100) {
    oam->y = info->y;
    oam->charnum = kSprite_PrepAndDrawSingleLarge_Tab2[kSprite_PrepAndDrawSingleLarge_Tab1[sprite_type[k]] + sprite_graphics[k]];
    oam->flags = info->flags;
  }
  bytewise_extended_oam[oam - oam_buf] = 2 | ((info->x >= 256) ? 1: 0);
  if (sprite_flags3[k] & 0x10)
    SpriteDraw_Shadow(k, info);
}

// Draws a ground shadow for sprite k at a custom vertical offset a below the
// sprite's world Y position. The shadow is placed at oam_cur_ptr + sprite_flags2
// (i.e., the last OAM slot in the sprite's block, after all body tiles).
// If sprite_flags3[k] bit 5 is set, draws a small circular shadow (char 0x38,
// size 8×8); otherwise draws the standard oval shadow (char 0x6C, size 16×16).
// The shadow is clipped when sprite is paused or in the lifted-high carry pose
// (state 10 and unk3 == 3), and when the shadow Y falls outside the visible screen.
void SpriteDraw_Shadow_custom(int k, PrepOamCoordsRet *info, uint8 a) {  // 86dc5c
  uint16 y = Sprite_GetY(k) + a;
  info->y = y;
  if (sprite_pause[k] || sprite_state[k] == 10 && sprite_unk3[k] == 3)
    return;
  y -= BG2VOFS_copy2;
  info->y = y;
  if ((uint16)(y + 0x10) >= 0x100)
    return;
  OamEnt *oam = GetOamCurPtr() + (sprite_flags2[k] & 0x1f);
  if (sprite_flags3[k] & 0x20) {
    SetOamHelper1(oam, info->x, y + 1, 0x38, (info->flags & 0x30) | 8, 0);
  } else {
    SetOamHelper1(oam, info->x, y, 0x6c, (info->flags & 0x30) | 8, 2);
  }
}

// Standard shadow draw with a fixed +10 pixel vertical offset below the sprite Y.
// The offset is chosen so the shadow appears at the sprite's feet for the default
// 16-pixel-tall enemy tile.
void SpriteDraw_Shadow(int k, PrepOamCoordsRet *oam) {  // 86dc64
  SpriteDraw_Shadow_custom(k, oam, 10);
}

// Draws sprite k as a single 8×8 OAM tile. Same tile lookup logic as SingleLarge
// but sets the extended OAM size bit to 0 (8×8). The shadow vertical offset is
// reduced to +2 pixels (SpriteDraw_Shadow_custom with a=2) so the shadow hugs the
// smaller sprite. Used for small collectibles (arrows, magic jars, etc.).
void SpriteDraw_SingleSmall(int k) {  // 86dcef
  PrepOamCoordsRet info;
  if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
    return;
  OamEnt *oam = GetOamCurPtr();
  oam->x = info.x;
  if ((uint16)(info.y + 0x10) < 0x100) {
    oam->y = info.y;
    oam->charnum = kSprite_PrepAndDrawSingleLarge_Tab2[kSprite_PrepAndDrawSingleLarge_Tab1[sprite_type[k]] + sprite_graphics[k]];
    oam->flags = info.flags;
  }
  bytewise_extended_oam[oam - oam_buf] = 0 | (info.x >= 256);
  if (sprite_flags3[k] & 0x10)
    SpriteDraw_Shadow_custom(k, &info, 2);
}

// Draws sprite k as two vertically stacked 8×8 tiles (thin and tall, e.g., a key).
// The top tile uses char offset +0x00 and the bottom uses +0x10 (next tile row).
// Both tiles share the same X and the base Y; the second is drawn at Y+8.
// Draws a standard ground shadow if the shadow flag is set.
void Sprite_DrawThinAndTall(int k) {  // 86dd40
  PrepOamCoordsRet info;
  if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
    return;
  OamEnt *oam = GetOamCurPtr();
  uint8 a = kSprite_PrepAndDrawSingleLarge_Tab2[kSprite_PrepAndDrawSingleLarge_Tab1[sprite_type[k]] + sprite_graphics[k]];
  SetOamHelper0(oam + 0, info.x, info.y + 0, a + 0x00, info.flags, 0);
  SetOamHelper0(oam + 1, info.x, info.y + 8, a + 0x10, info.flags, 0);
  if (sprite_flags3[k] & 0x10)
    SpriteDraw_Shadow(k, &info);
}

// State 10 handler — Link is currently carrying (holding above head) sprite k.
// Each frame, locks the sprite's position to Link's head:
//   X: link_x_coord + kSpriteHeld_X[facing*2 + unk3], with a wobble offset r0
//      (derived from sprite_delay_aux4 - 1 to create a sway effect during lift).
//   Y: link_y_coord + 8 - (link_z_coord + 1 + kSpriteHeld_ZForFrame[anim_step])
//      so the sprite rises with Link's lift animation.
//   Z: kSpriteHeld_Z[facing*2 + unk3] — fixed height above Link's reference plane.
//
// sprite_unk3[k] tracks the lift phase (0-3); phases 0-2 advance every
// sprite_delay_main[k] ticks (4 frames normally, 8 for heavy objects, type C==6).
// Phase 3 = fully lifted — shadow flag is cleared (shadow renders at ground level).
//
// If sprite_unk4[k] != 11 (not a frozen/stunned carry): calls SpriteActive_Main so
// the sprite can still run its AI while carried (used by living enemies like Cuccos).
// When sprite_delay_aux4[k] reaches 1 (throw release timer), transitions back to
// state 9 (active), clears the hookshot drag, sets z_vel=32 (pops upward on release),
// restores the shadow flag, and sets link_picking_throw_state=2 to unlock Link.
//
// If sprite_unk4[k] == 11 (stunned carry): calls SpriteStunned_Main_Func1 for the
// sparkle effect only (no AI, just visual).
//
// CarriedSprite_CheckForThrow is called every frame to detect a Y-button throw.
void SpriteModule_Carried(int k) {  // 86de83


  // kSpriteHeld_ZForFrame: per-animation-step Z offset (cycles 3→2→1→3→2→1)
  // to make the held sprite bob slightly as Link walks.
  static const uint8 kSpriteHeld_ZForFrame[6] = {3, 2, 1, 3, 2, 1};
  static const int8 kSpriteHeld_X[16] = {0, 0, 0, 0, 0, 0, 0, 0, -13, -10, -5, 0, 13, 10, 5, 0};
  static const uint8 kSpriteHeld_Z[16] = {13, 14, 15, 16, 0, 10, 22, 16, 8, 11, 14, 16, 8, 11, 14, 16};
  sprite_room[k] = overworld_area_index;
  if (sprite_unk3[k] != 3) {
    if (sprite_delay_main[k] == 0) {
      sprite_delay_main[k] = (sprite_C[k] == 6) ? 8 : 4;
      sprite_unk3[k]++;
    }
  } else {
    sprite_flags3[k] &= ~0x10;
  }

  uint8 t = sprite_delay_aux4[k] - 1;
  uint8 r0 = t < 63 && (t & 2);
  int j = link_direction_facing * 2 + sprite_unk3[k];

  int t0 = (uint8)link_x_coord + (uint8)kSpriteHeld_X[j];
  int t1 = (uint8)t0 + (t0 >> 8 & 1) + r0;
  int t2 = HIBYTE(link_x_coord) + (t1 >> 8 & 1) + (t0 >> 8 & 1) + (uint8)(kSpriteHeld_X[j]>>8);
  sprite_x_lo[k] = t1;
  sprite_x_hi[k] = t2;

 // Sprite_SetX(k, link_x_coord + kSpriteHeld_X[j] + r0);
  sprite_z[k] = kSpriteHeld_Z[j];
  int an = link_animation_steps < 6 ? link_animation_steps : 0;
  uint16 z = link_z_coord + 1 + kSpriteHeld_ZForFrame[an];
  Sprite_SetY(k, link_y_coord + 8 - z);
  sprite_floor[k] = link_is_on_lower_level & 1;
  CarriedSprite_CheckForThrow(k);
  Sprite_Get16BitCoords(k);
  if (sprite_unk4[k] != 11) {
    SpriteActive_Main(k);
    if (sprite_delay_aux4[k] == 1) {
      sprite_state[k] = 9;
      sprite_B[k] = 0;
      sprite_delay_aux4[k] = 96;
      sprite_z_vel[k] = 32;
      sprite_flags3[k] |= 0x10;
      link_picking_throw_state = 2;
    }
  } else {
    SpriteStunned_Main_Func1(k);
  }
}

// Checks whether the carried sprite k should be thrown (released) this frame and,
// if so, launches it in Link's current facing direction.
// Throw is triggered when ALL of the following hold:
//   - Not in messaging module (main_module_index != 14).
//   - Not near a pit OR Link is in a forced-throw condition (near_pit_state == 2).
//   - No other Link state that implies automatic release:
//       link_auxiliary_state bit 0 (ledge hang), deep water, bunny/mirror form,
//       item-use pose, or incapacitation timer (unless link_disable_sprite_damage).
//   - Either sprite is fully lifted (unk3 == 3) AND Y button (filtered_joypad_L bit 7)
//     was just pressed, OR a forced throw condition overrides the button check.
// On throw: plays SFX 0x13 (throw sound), sets sprite state back to sprite_unk4[k]
// (either 9 active or 11 stunned), assigns facing-specific velocities from
// kSpriteHeld_Throw_Xvel/Yvel and a fixed upward z_vel = 4 for the arc.
// Restores sprite_flags3 shadow bit from the init table (so flying sprites can cast
// a shadow while airborne) and clears sprite_delay_aux4 (no post-throw immunity).
void CarriedSprite_CheckForThrow(int k) {  // 86df6d
  // X/Y throw velocity table: indexed by link_direction_facing >> 1 (0=up,1=down,2=left,3=right).
  static const int8 kSpriteHeld_Throw_Xvel[4] = {0, 0, -62, 63};
  static const int8 kSpriteHeld_Throw_Yvel[4] = {-62, 63, 0, 0};
  static const uint8 kSpriteHeld_Throw_Zvel[4] = {4, 4, 4, 4};

  if (main_module_index == 14)
    return;

  if (player_near_pit_state != 2) {
    uint8 t = (link_auxiliary_state & 1) | link_is_in_deep_water | link_is_bunny_mirror |
              link_pose_for_item | (link_disable_sprite_damage ? 0 : link_incapacitated_timer);
    if (!t) {
      if (sprite_unk3[k] != 3 || !((filtered_joypad_H | filtered_joypad_L) & 0x80))
        return;
      filtered_joypad_L &= 0x7f;
    }
  }

  SpriteSfx_QueueSfx3WithPan(k, 0x13);
  link_picking_throw_state = 2;
  sprite_state[k] = sprite_unk4[k];
  sprite_z_vel[k] = 0;
  sprite_unk3[k] = 0;
  sprite_flags3[k] = sprite_flags3[k] & ~0x10 | kSpriteInit_Flags3[sprite_type[k]] & 0x10;
  int j = link_direction_facing >> 1;
  sprite_x_vel[k] = kSpriteHeld_Throw_Xvel[j];
  sprite_y_vel[k] = kSpriteHeld_Throw_Yvel[j];
  sprite_z_vel[k] = kSpriteHeld_Throw_Zvel[j];
  sprite_delay_aux4[k] = 0;
}

// State 11 entry point — forwards to SpriteStunned_MainEx(k, false) for the
// normal stunned/thrown flow (damage checks, tile collision, bounce, Z gravity).
void SpriteModule_Stunned(int k) {  // 86dffa
  SpriteStunned_MainEx(k, false);
}

// Entry point for throwable terrain tile wall/sprite interaction only.
// Skips the opening damage/sparkle checks and jumps directly to the wall-collision
// branch inside SpriteStunned_MainEx via the second_entry path.
void ThrownSprite_TileAndSpriteInteraction(int k) {  // 86e02a
  SpriteStunned_MainEx(k, true);
}

// Transitions sprite k into state 1 (fall into pit, first phase).
// Sets delay_main = 0x1F (31 frames of fall animation), silences the main
// sound channel, and plays the "fall" SFX 0x20 panned to the sprite's position.
void Sprite_Func8(int k) {  // 86e0ab
  sprite_state[k] = 1;
  sprite_delay_main[k] = 0x1f;
  sound_effect_1 = 0;
  SpriteSfx_QueueSfx2WithPan(k, 0x20);
}

// Transitions sprite k into state 3 (drowning / falling into water).
// Queues the splash SFX (0x28 | pan) for spatial positioning. Sets
// sprite_delay_main = 15 (sink countdown), ai_state = 0 (sink phase),
// and sprite_flags2 = 3 (minimum OAM slot count for the splash tiles).
// The stray GetRandomNumber() call has no effect but matches the original SNES
// code sequence (likely a junk register read from an earlier version).
void Sprite_Func22(int k) {  // 86e0f6
  sound_effect_1 = Sprite_CalculateSfxPan(k) | 0x28;
  sprite_state[k] = 3;
  sprite_delay_main[k] = 15;
  sprite_ai_state[k] = 0;
  GetRandomNumber(); // wtf
  sprite_flags2[k] = 3;
}

// Returns true while Lost Woods throwable terrain is airborne. Its center
// terrain probe can see the overhead canopy as solid before the normal landing
// code has a chance to finish the throw, so only suppress that false tile hit
// while Z height says the object is still in flight.
static bool ThrowableScenery_ShouldSkipLostWoodsAirborneCollision(int k) {
  return sprite_type[k] == 0xec && !player_is_indoors && sprite_z[k] &&
         (BYTE(overlay_index) == 0x97 || BYTE(overlay_index) == 0x9d ||
          BYTE(overlay_index) == 0x9e);
}

// Moves a throwable terrain tile (type 0xEC) one frame: applies X/Y velocity,
// runs tile collision if not hookshot-lifted (sprite_E == 0), then calls
// ThrownSprite_TileAndSpriteInteraction to handle wall bounces and sprite hits.
void ThrowableScenery_InteractWithSpritesAndTiles(int k) {  // 86e164
  Sprite_MoveXY(k);
  if (!sprite_E[k]) {
    if (ThrowableScenery_ShouldSkipLostWoodsAirborneCollision(k))
      sprite_wallcoll[k] = 0;
    else
      Sprite_CheckTileCollision(k);
  }
  ThrownSprite_TileAndSpriteInteraction(k);
}

// Checks whether thrown sprite k should deal contact damage to other active sprites.
// Guards: skips if sprite_delay_aux4[k] is set (16-frame post-hit cooldown) or if
// both X and Y velocities are zero (sprite has come to rest).
// Iterates all 16 sprite slots looking for candidates that are:
//   - A different slot than k (no self-hit).
//   - Not a leaping fish (type 0xD2 cannot hit other sprites by throw).
//   - In active/stunned/carried state (>= 9).
//   - On the same floor as k.
//   - Not protected: the ((i ^ frame_counter) & 3) stagger ensures only one slot
//     is checked per group of 4 frames, reducing CPU load and preventing double-hits.
//   - sprite_ignore_projectile[i] == 0 and sprite_hit_timer[i] == 0 (not immune).
void ThrownSprite_CheckDamageToSprites(int k) {  // 86e172
  if (sprite_delay_aux4[k] || !(sprite_x_vel[k] | sprite_y_vel[k]))
    return;
  for (int i = 15; i >= 0; i--) {
    if (i != cur_object_index && sprite_type[k] != 0xd2 && sprite_state[i] >= 9 &&
      ((i ^ frame_counter) & 3 | sprite_ignore_projectile[i] | sprite_hit_timer[i]) == 0 && sprite_floor[k] == sprite_floor[i])
      ThrownSprite_CheckDamageToSingleSprite(k, i);
  }
}

// Tests AABB overlap between thrown sprite k and target sprite j, then applies
// damage and recoil if they collide. The hitbox for k is a 16×8 rectangle centred
// at (sprite_x, sprite_y - sprite_z + 8) — Z subtraction ensures the projectile
// height is included so thrown pots don't hit ground-level enemies from above.
// On hit:
//   - Type 0x3F (anti-fairy): calls Sprite_PlaceWeaponTink (spark effect only, no damage).
//   - All others: calls Ancilla_CheckDamageToSprite_preset with damage class 3
//     (or 1 for an outdoor skull, sprite_C == 2), sets recoil velocities on j to
//     twice k's velocity, and starts the 16-frame hit-immunity on k.
// After any hit, calls Sprite_ApplyRicochet to bounce and potentially smash k.
void ThrownSprite_CheckDamageToSingleSprite(int k, int j) {  // 86e1b2
  SpriteHitBox hb;
  hb.r0_xlo = sprite_x_lo[k];
  hb.r8_xhi = sprite_x_hi[k];
  hb.r2 = 15;
  int t = sprite_y_lo[k] - sprite_z[k];
  int u = (t & 0xff) + 8;
  hb.r1_ylo = u;
  hb.r9_yhi = sprite_y_hi[k] + (u >> 8) - (t < 0);
  hb.r3 = 8;
  Sprite_SetupHitBox(j, &hb);
  if (!CheckIfHitBoxesOverlap(&hb))
    return;
  if (sprite_type[j] == 0x3f) {
    Sprite_PlaceWeaponTink(k);
  } else {
    uint8 a = (sprite_type[k] == 0xec && sprite_C[k] == 2 && !player_is_indoors) ? 1 : 3;
    Ancilla_CheckDamageToSprite_preset(j, a);

    sprite_x_recoil[j] = sprite_x_vel[k] * 2;
    sprite_y_recoil[j] = sprite_y_vel[k] * 2;
    sprite_delay_aux4[k] = 16;
  }
  Sprite_ApplyRicochet(k);
}

// On wall or sprite contact, bounces sprite k by inverting both velocity components
// (180-degree ricochet), halves the resulting speed, and transmutes the sprite to
// debris if it is a throwable tile (type 0xEC) that can break.
void Sprite_ApplyRicochet(int k) {  // 86e229
  Sprite_InvertSpeed_XY(k);
  Sprite_HalveSpeed_XY(k);
  ThrowableScenery_TransmuteIfValid(k);
}

// Transmutes sprite k to debris if it is a breakable throwable tile (type 0xEC).
// Resets repulsespark_timer = 0 to suppress the spark effect that normally fires
// on wall contact (the debris animation provides its own visual). Delegates to
// ThrowableScenery_TransmuteToDebris for the actual state change.
void ThrowableScenery_TransmuteIfValid(int k) {  // 86e22f
  if (sprite_type[k] != 0xec)
    return;
  repulsespark_timer = 0;
  ThrowableScenery_TransmuteToDebris(k);
}

// Converts a throwable tile sprite (type 0xEC) into its broken debris animation.
// If sprite_graphics[k] != 0, the tile had a custom graphic override (e.g., a pot
// with a special item inside) — restores dung_secrets_unk1 and spawns the secret
// item, then resets the flag. Selects the break SFX from kSprite_Func21_Sfx using
// sprite_C[k] (tile variety index) outdoors, or index 0 (default smash) indoors.
// Calls Sprite_ScheduleForBreakage to start the 31-frame poof death animation.
void ThrowableScenery_TransmuteToDebris(int k) {  // 86e239
  uint8 a = sprite_graphics[k];
  if (a != 0) {
    BYTE(dung_secrets_unk1) = a;
    Sprite_SpawnSecret(k);
    BYTE(dung_secrets_unk1) = 0;
  }
  a = player_is_indoors ? 0 : sprite_C[k];
  sound_effect_1 = 0;
  SpriteSfx_QueueSfx2WithPan(k, kSprite_Func21_Sfx[a]);
  Sprite_ScheduleForBreakage(k);
}

// Schedules sprite k for the break/death poof animation by setting:
//   state = 6 (SpriteModule_Die / SpriteDeath_MainEx), delay = 31 (poof duration),
//   and sprite_flags2 += 4 to increase the OAM slot count for the larger poof tiles.
void Sprite_ScheduleForBreakage(int k) {  // 86e25a
  sprite_delay_main[k] = 31;
  sprite_state[k] = 6;
  sprite_flags2[k] += 4;
}

// Arithmetic right-shifts both X and Y velocity by 1 (signed), halving the speed.
// Preserves sign. Used after a ricochet to decelerate thrown objects progressively.
void Sprite_HalveSpeed_XY(int k) {  // 86e26e
  sprite_x_vel[k] = (int8)sprite_x_vel[k] >> 1;
  sprite_y_vel[k] = (int8)sprite_y_vel[k] >> 1;
}

// Spawns a leaping fish sprite (type 0xD2) at slot k's current position.
// Sets ai_state = 2 (leap arc phase) and delay_main = 48 (jump duration).
// If the parent sprite is itself a fish (type 0xD2), sets sprite_A[j] = 0xD2
// to mark the child as a re-spawned fish so it won't re-spawn recursively.
// Used when a thrown sprite lands in water to produce the water-entry fish jump.
void Sprite_SpawnLeapingFish(int k) {  // 86e286
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(k, 0xd2, &info);
  if (j < 0)
    return;
  Sprite_SetSpawnedCoordinates(j, &info);
  sprite_ai_state[j] = 2;
  sprite_delay_main[j] = 48;
  if (sprite_type[k] == 0xd2)
    sprite_A[j] = 0xd2;
}

// Shared tick function called from the stunned/carried states to run both the
// sprite's own AI (SpriteActive_Main) and apply the visual stun/freeze effect.
//
// If sprite_unk5[k] is nonzero (frozen by Ice Rod / Cane of Somaria):
//   - Below delay_main 32: overrides the OAM palette bits (0xf1 & | 4) to show
//     the blue frozen palette.
//   - Spawn rate: kSpriteStunned_Main_Func1_Masks[delay >> 4] is a bitmask;
//     sparkles only spawn when ((k<<4) ^ frame_counter) & mask == 0, giving
//     different spawn densities across the freeze duration.
//   - Sparkle positions: random offsets from kSparkleGarnish_XY[rand & 3].
//
// If sprite_unk5[k] == 0 (normal stun, e.g., from hammer or boomerang):
//   - Every 2 frames and when not in submodule/pause: decrements sprite_stunned[k].
//   - While stunned >= 0x38 (first 56 counts): no jitter.
//   - Below 0x38: alternates x_vel ±8 each frame (jitter effect); calls Sprite_MoveX
//     to apply the jitter without affecting the main velocity.
//   - When stunned reaches 0: restores state to 9 (active) and clears recoil.
void SpriteStunned_Main_Func1(int k) {  // 86e2ba
  SpriteActive_Main(k);
  if (sprite_unk5[k]) {
    // Frozen (Ice Rod / Cane): apply blue palette and spawn ice sparkle garnishes.
    if (sprite_delay_main[k] < 32)
      sprite_oam_flags[k] = sprite_oam_flags[k] & 0xf1 | 4;
    uint8 t = ((k << 4) ^ frame_counter) | submodule_index;
    if (t & kSpriteStunned_Main_Func1_Masks[sprite_delay_main[k] >> 4])
      return;
    uint16 x = kSparkleGarnish_XY[GetRandomNumber() & 3];
    uint16 y = kSparkleGarnish_XY[GetRandomNumber() & 3];
    Sprite_GarnishSpawn_Sparkle(k, x, y);
  } else {
    if ((frame_counter & 1) | submodule_index | flag_unk1)
      return;
    uint8 t = sprite_stunned[k];
    if (t) {
      sprite_stunned[k]--;
      if (t < 0x38) {
        sprite_x_vel[k] = (t & 1) ? -8 : 8;
        Sprite_MoveX(k);
      }
      return;
    }
    sprite_state[k] = 9;
    sprite_x_recoil[k] = 0;
    sprite_y_recoil[k] = 0;
  }
}

// State 2 handler — sprite appears or despawns in a "poof" cloud.
// Also handles the "hammer crushes frozen buzz blob" special case.
//
// While sprite_delay_main[k] > 0 (animation running):
//   Draws 4 OAM tiles per frame selected from the parallel kSpritePoof_X/Y/Char/
//   Flags/Ext tables. Frame index j = ((delay >> 1) & ~3) + 3 selects groups of
//   four entries in reverse order as delay counts down.
//     Frames 0-7  (j 3-0): tiles use char 0x9B (small puff, 8×8) clustered tightly.
//     Frames 8-11 (j 7-4): tiles use char 0xB3 (medium smoke, 8×8) spread wider.
//     Frames 12-15(j 11-8): tiles use char 0x8A (16×16 cloud) at maximum scatter.
//   Position is taken from dungmap_var7 (set by PrepOamCoordOrDoubleRet earlier).
//   Sprite_CorrectOamEntries clamps the 3 visible OAM slots for proper Y wrapping.
//
// When delay == 0 (animation complete):
//   - Frozen buzz blob (type 0x0D, head_dir != 0): reverts to normal buzz blob by
//     calling PrepareEnemyDrop, restoring X and clearing z_vel and projectile immunity.
//   - All others: if sprite_die_action[k] == 0, calls ForcePrizeDrop (force-spawn
//     a prize at priority 2, override 2); otherwise calls Sprite_DoTheDeath for
//     the standard item-drop / death-flag cleanup.
void SpriteModule_Poof(int k) {  // 86e393
  // Per-frame tile offsets for the 4-sprite poof cloud (tightly clustered at first).
  static const int8 kSpritePoof_X[16] = {-6, 10, 1, 13, -6, 10, 1, 13, -7, 4, -5, 6, -1, 1, -2, 0};
  static const int8 kSpritePoof_Y[16] = {-6, -4, 10, 9, -6, -4, 10, 9, -8, -10, 4, 3, -1, -2, 0, 1};
  // Char numbers: 0x9B = small puff (8×8), 0xB3 = medium smoke, 0x8A = large cloud (16×16).
  static const uint8 kSpritePoof_Char[16] = {0x9b, 0x9b, 0x9b, 0x9b, 0xb3, 0xb3, 0xb3, 0xb3, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a};
  static const uint8 kSpritePoof_Flags[16] = {0x24, 0xa4, 0x24, 0xa4, 0xe4, 0x64, 0xa4, 0x24, 0x24, 0xe4, 0xe4, 0xe4, 0x24, 0xe4, 0xe4, 0xe4};
  // Ext byte: 0 = 8×8, 2 = 16×16.
  static const uint8 kSpritePoof_Ext[16] = {0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2};
  // Frozen sprite pulverized by hammer — special revival or death logic.
  if (sprite_delay_main[k] == 0) {
    if (sprite_type[k] == 0xd && sprite_head_dir[k] != 0) {
      // buzz blob?
      int bakx = Sprite_GetX(k);
      PrepareEnemyDrop(k, 0xd);
      Sprite_SetX(k, bakx);
      sprite_z_vel[k] = 0;
      sprite_ignore_projectile[k] = 0;
    } else {
      if (sprite_die_action[k] == 0) {
        ForcePrizeDrop(k, 2, 2);
      } else {
        Sprite_DoTheDeath(k);
      }
    }
  } else {
    PrepOamCoordsRet info;
    if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
      return;
    OamEnt *oam = GetOamCurPtr();
    int j = ((sprite_delay_main[k] >> 1) & ~3) + 3;
    for (int i = 3; i >= 0; i--, j--, oam++) {
      SetOamPlain(oam, kSpritePoof_X[j] + BYTE(dungmap_var7), kSpritePoof_Y[j] + HIBYTE(dungmap_var7),
                   kSpritePoof_Char[j], kSpritePoof_Flags[j], kSpritePoof_Ext[j]);
    }
    Sprite_CorrectOamEntries(k, 3, 0xff);
  }
}

// Thin wrapper around Sprite_PrepOamCoordOrDoubleRet that discards the return value.
// Used in contexts where the caller does not need the out-of-bounds flag.
void Sprite_PrepOamCoord(int k, PrepOamCoordsRet *ret) {  // 86e416
  Sprite_PrepOamCoordOrDoubleRet(k, ret);
}

// Computes the screen-space OAM coordinates for sprite k and returns true if the
// sprite is off-screen (caller should skip drawing). This is the core culling and
// coordinate-preparation function used by every drawing helper.
//
// Steps:
//   1. Clear sprite_pause[k] (assume on-screen by default).
//   2. Subtract BG2 scroll offsets from the 16-bit world position to get screen X/Y.
//   3. Subtract sprite_z[k] from Y so elevated sprites appear higher on screen.
//   4. Build ret->flags from sprite_oam_flags XOR sprite_obj_prio (merges hit-flash
//      priority bits into the OAM attribute byte).
//   5. Off-screen test: sprite is out of bounds if screen X is outside [-0x40, 0x130]
//      (or [-0x80, 0x170] in the 64-pixel extended screen feature mode), OR if Y
//      is outside [-0x40, 0x130] AND the sprite does not have the "render off-screen
//      Y" flag (sprite_flags4 bit 5).
//   6. On OOB: set sprite_pause[k]++ (marks it paused for input checks).
//      If defl_bits bit 7 is clear (not a persistent sprite), call Sprite_KillSelf.
//   7. Store final screen coordinates into ret->x / ret->y AND into the low/high
//      bytes of dungmap_var7 for use by the Poof drawing code.
bool Sprite_PrepOamCoordOrDoubleRet(int k, PrepOamCoordsRet *ret) {  // 86e41e
  sprite_pause[k] = 0;
  uint16 x = cur_sprite_x - BG2HOFS_copy2;
  uint16 y = cur_sprite_y - BG2VOFS_copy2;
  bool out_of_bounds = false;
  R0 = x;
  R2 = y - sprite_z[k];
  ret->flags = sprite_oam_flags[k] ^ sprite_obj_prio[k];
  ret->r4 = 0;
  int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;

  if ((uint16)(x + 0x40 + xt) >= (0x170 + xt * 2) ||
      (uint16)(y + 0x40) >= 0x170 && !(sprite_flags4[k] & 0x20)) {
    sprite_pause[k]++;
    if (!(sprite_defl_bits[k] & 0x80))
      Sprite_KillSelf(k);
    out_of_bounds = true;
  }
  ret->x = R0;
  ret->y = R2;
  BYTE(dungmap_var7) = ret->x;
  HIBYTE(dungmap_var7) = ret->y;
  return out_of_bounds;
}

// Runs tile collision for sprite k, handling the two-layer dungeon case.
// Clears sprite_wallcoll[k] first, then:
//   - Overworld / sprites ignoring floors (flags4 bit 7 set, or no collision header):
//     runs a single-layer check and returns.
//   - Two-layer dungeon (dung_hdr_collision != 0 and != 4): temporarily forces
//     sprite_floor = 1 (upper layer), checks that layer, then restores floor = 0
//     and checks the lower layer, storing the lower-layer tiletype in byte_7FFABC[k]
//     so callers can inspect what tile type the sprite is standing on.
//   - dung_hdr_collision == 4: only checks upper layer (saves byte_7E0FB6 and restores).
void Sprite_CheckTileCollision2(int k) {  // 86e4ab
  sprite_wallcoll[k] = 0;
  if (sign8(sprite_flags4[k]) || !dung_hdr_collision) {
    Sprite_CheckTileCollisionSingleLayer(k);
    return;
  }
  byte_7E0FB6 = sprite_floor[k];
  sprite_floor[k] = 1;
  Sprite_CheckTileCollisionSingleLayer(k);
  if (dung_hdr_collision == 4) {
    sprite_floor[k] = byte_7E0FB6;
    return;
  }
  sprite_floor[k] = 0;
  Sprite_CheckTileCollisionSingleLayer(k);
  byte_7FFABC[k] = sprite_tiletype;
}

// Runs tile collision for a single floor layer of sprite k.
//
// Two modes controlled by sprite_flags2[k] bit 5:
//   - Set (flying / above walls): only checks the floor tile property (Sprite_CheckTileProperty
//     0x6A — conveyor/suction), incrementing wallcoll[k] if present.
//   - Clear (ground-walking): checks directional wall sensors using
//     Sprite_CheckForTileInDirection_vertical/horizontal.
//     If flags4 bit 7 is set OR no collision header:
//       Only probes the velocity-aligned directions (saves CPU for fast movers).
//     Otherwise (slow walkers in dungeons):
//       Always probes all four directions so the sprite pushes out of walls
//       it was placed inside (prevents embedding).
//
// After directional checks (when flags5 bit 7 is clear and sprite_z == 0):
//   Calls Sprite_CheckTileProperty(0x68) to read the floor tile type into
//   sprite_tiletype and sprite_I[k]. Special tile reactions:
//     0x1C (upper/lower floor seam): if sort mode + stunned, sets floor = 1.
//     0x20 (pit): if flags bit 0 set and overworld, calls Overworld_SubstituteAlternateSecret.
//              Otherwise starts the pit-fall sequence via Sprite_Func8 or sets to drown state.
//     0x22/0x23 (sloped tiles): checks kSlopedTile[32] threshold for floor transitions.
//     0x28 (warp tile): checks floor and position for warping to upper layer.
void Sprite_CheckTileCollisionSingleLayer(int k) {  // 86e4db
  if (sprite_flags2[k] & 0x20) {
    if (Sprite_CheckTileProperty(k, 0x6a))
      sprite_wallcoll[k]++;
    return;
  }

  if (sign8(sprite_flags4[k]) || dung_hdr_collision == 0) {
    if (sprite_y_vel[k])
      Sprite_CheckForTileInDirection_vertical(k, sign8(sprite_y_vel[k]) ? 0 : 1);
    if (sprite_x_vel[k])
      Sprite_CheckForTileInDirection_horizontal(k, sign8(sprite_x_vel[k]) ? 2 : 3);
  } else {
    Sprite_CheckForTileInDirection_vertical(k, 1);
    Sprite_CheckForTileInDirection_vertical(k, 0);
    Sprite_CheckForTileInDirection_horizontal(k, 3);
    Sprite_CheckForTileInDirection_horizontal(k, 2);
  }

  if (sign8(sprite_flags5[k]) || sprite_z[k])
    return;

  Sprite_CheckTileProperty(k, 0x68);
  sprite_I[k] = sprite_tiletype;
  if (sprite_tiletype == 0x1c) {
    if (sort_sprites_setting && sprite_state[k] == 11)
      sprite_floor[k] = 1;
  } else if (sprite_tiletype == 0x20) {
    if (sprite_flags[k] & 1) {
      if (!player_is_indoors) {
        Sprite_Func8(k);
      } else {
        sprite_state[k] = 5;
        if (sprite_type[k] == 0x13 || sprite_type[k] == 0x26) {
          sprite_oam_flags[k] &= ~1;
          sprite_delay_main[k] = 63;
        } else {
          sprite_delay_main[k] = 95;
        }
      }
    }
  } else if (sprite_tiletype == 0xc) {
    if (byte_7FFABC[k] == 0x1c) {
      SpriteFall_AdjustPosition(k);
      sprite_wallcoll[k] |= 0x20;
    }
  } else if (sprite_tiletype >= 0x68 && sprite_tiletype < 0x6c) {
    Sprite_ApplyConveyor(k, sprite_tiletype);
  } else if (sprite_tiletype == 8) {
    if (dung_hdr_collision == 4)
      Sprite_ApplyConveyor(k, 0x6a);
  }
}

// Probes a horizontal wall sensor for sprite k in direction yy (2=left, 3=right).
// If a wall tile is detected, sets the corresponding bit in sprite_wallcoll[k]
// from kSprite_Func7_Tab[yy] and nudges the sprite out of the wall by ±n pixels
// (n=3 if sprite_F[k] is set for fast bounce, else 1), but only when subtype & 7 < 5
// (sprites with subtype & 7 >= 5 are not physically pushed by walls, e.g., ghosts).
void Sprite_CheckForTileInDirection_horizontal(int k, int yy) {  // 86e5b8
  if (!Sprite_CheckTileInDirection(k, yy))
    return;
  sprite_wallcoll[k] |= kSprite_Func7_Tab[yy];
  if ((sprite_subtype[k] & 7) < 5) {
    int8 n = sprite_F[k] ? 3 : 1;
    SpriteAddXY(k, (yy & 1) ? -n : n, 0);
  }
}

// Probes a vertical wall sensor for sprite k in direction yy (0=up, 1=down).
// Same logic as the horizontal variant but applies the nudge on the Y axis.
void Sprite_CheckForTileInDirection_vertical(int k, int yy) {  // 86e5ee
  if (!Sprite_CheckTileInDirection(k, yy))
    return;
  sprite_wallcoll[k] |= kSprite_Func7_Tab[yy];
  if ((sprite_subtype[k] & 7) < 5) {
    int8 n = sprite_F[k] ? 3 : 1;
    SpriteAddXY(k, 0, (yy & 1) ? -n : n);
  }
}

// Moves sprite k by the dungeon's moving-floor velocity (dung_floor_x/y_vel).
// Used when the sprite is standing on a conveyor tile that is itself moving (e.g.,
// the falling Ganon's Tower platform), so the sprite rides along with the floor.
void SpriteFall_AdjustPosition(int k) {  // 86e624
  SpriteAddXY(k, dung_floor_x_vel, dung_floor_y_vel);
}

// Computes the tile sensor index for sprite k in direction yy (0-3) and calls
// Sprite_CheckTileProperty. The sprite's flags[k] high nibble encodes a hitbox
// size offset: t = (flags & 0xF0); the raw direction is scaled to a sensor table
// index as 2 * ((t >> 2) + yy). Returns whether a wall tile is present.
bool Sprite_CheckTileInDirection(int k, int yy) {  // 86e72f
  uint8 t = (sprite_flags[k] & 0xf0);
  yy = 2 * ((t >> 2) + yy);
  return Sprite_CheckTileProperty(k, yy);
}

// Reads the tile attribute at a specific sensor offset from sprite k's centre,
// stores it in sprite_tiletype, and returns whether the tile blocks movement.
//
// j selects the sensor offset from kSprite_Func5_X/Y[j>>1]:
//   j = 0x68..0x6B: floor tile sensors (centre point).
//   j = 0x00..0x07: directional wall sensors (four corners).
//   j = 0x6A: "fly over wall" sensor (uses a single centre probe).
//
// Coordinate computation:
//   Indoors: wraps within 0x200×0x200 (room wrap-around for seamless corridors).
//   Outdoors: checks against sprcoll_x/y_base/size bounds (active camera region).
//   Out-of-bounds: if flags2 bit 6 is set, kills the sprite; otherwise treats as wall.
//
// Tile attribute interpretation depends on sprite_defl_bits[k]:
//   - Bit 3 set (simplified collision): uses kSprite_SimplifiedTileAttr[b] for a
//     coarser pass (only distinguishes solid / pit / deep water). Sloped tiles route
//     to Entity_CheckSlopedTileCollision.
//   - Bit 6 set (water/air swimmer flag): fish and leaping sprites ignore water (b==9)
//     tiles; certain enemy types ignore both water and floor transitions (b==8 or 9).
//   - Default: uses kSprite_Func5_Tab3[b] for the full solid-tile classification.
//     Handles tile 0x44 (fire bar): deals fire damage to susceptible sprites.
//     Handles tile 0x20 (pit): allows sprites with flags bit 0 to fall in.
bool Sprite_CheckTileProperty(int k, int j) {  // 86e73c
  uint16 x, y;
  bool in_bounds;
  j >>= 1;

  if (player_is_indoors) {
    x = (cur_sprite_x + 8 & 0x1ff) + kSprite_Func5_X[j] - 8;
    y = (cur_sprite_y + 8 & 0x1ff) + kSprite_Func5_Y[j] - 8;
    in_bounds = (x < 0x200) && (y < 0x200);
  } else {
    x = cur_sprite_x + kSprite_Func5_X[j];
    y = cur_sprite_y + kSprite_Func5_Y[j];
    in_bounds = (uint16)(x - sprcoll_x_base) < sprcoll_x_size &&
                (uint16)(y - sprcoll_y_base) < sprcoll_y_size;
  }
  if (!in_bounds) {
    if (sprite_flags2[k] & 0x40) {
      sprite_state[k] = 0;
      return false;
    } else {
      return true;
    }
  }

  int b = Sprite_GetTileAttribute(k, &x, y);

  if (sprite_defl_bits[k] & 8) {
    uint8 a = kSprite_SimplifiedTileAttr[b];
    if (a == 4) {
      if (!player_is_indoors)
        sprite_E[k] = 4;
    } else if (a >= 1) {
      return (sprite_tiletype >= 0x10 && sprite_tiletype < 0x14) ? Entity_CheckSlopedTileCollision(x, y) : true;
    }
    return false;
  }

  if (sprite_flags5[k] & 0x40) {
    uint8 type = sprite_type[k];
    if ((type == 0xd2 || type == 0x8a) && b == 9)
      return false;
    if (type == 0x94 && !sprite_E[k] || type == 0xe3 || type == 0x8c || type == 0x9a || type == 0x81)
      return (b != 8) && (b != 9);
  }

  if (kSprite_Func5_Tab3[b] == 0)
    return false;

  if (sprite_tiletype >= 0x10 && sprite_tiletype < 0x14)
    return Entity_CheckSlopedTileCollision(x, y);

  if (sprite_tiletype == 0x44) {
    if (sprite_F[k] && !sign8(sprite_give_damage[k])) {

      // Some mothula bug fix because we changed damage class 4.
      if (sprite_type[k] == 0x88 && (enhanced_features0 & kFeatures0_MiscBugFixes)) {
        if (sprite_hit_timer[k] == 0)
          Ancilla_CheckDamageToSprite_preset(k, 6);
      } else {
        Ancilla_CheckDamageToSprite_preset(k, 4);
      }
      if (sprite_hit_timer[k]) {
        sprite_hit_timer[k] = 153;
        sprite_F[k] = 0;
      }
    }
  } else if (sprite_tiletype == 0x20) {
    return !(sprite_flags[k] & 1) || !sprite_F[k];
  }
  return true;
}

// Reads the tile attribute byte at the given grid coordinate on the specified floor
// layer (0=lower, 1=upper) and stores it in the global sprite_tiletype.
// Indoors: indexes dung_bg2_attr_table using the 6-bit tile column (x & 0x1F8 >> 3)
// and 6-bit tile row (y & 0x1F8 << 3), with a +0x1000 offset for upper-floor tiles.
// Outdoors: delegates to Overworld_GetTileAttributeAtLocation after right-shifting x
// by 3 to convert from pixel to tile coordinates (modifying *x in-place).
uint8 GetTileAttribute(uint8 floor, uint16 *x, uint16 y) {  // 86e87b
  uint8 tiletype;
  if (player_is_indoors) {
    int t = (floor >= 1) ? 0x1000 : 0;
    t += (*x & 0x1f8) >> 3;
    t += (y & 0x1f8) << 3;
    tiletype = dung_bg2_attr_table[t];
  } else {
    tiletype = Overworld_GetTileAttributeAtLocation(*x >>= 3, y);
  }
  sprite_tiletype = tiletype;
  return tiletype;
}

// Per-sprite wrapper around GetTileAttribute that supplies sprite_floor[k] as
// the layer selector. Used by all sprite tile-property checks.
uint8 Sprite_GetTileAttribute(int k, uint16 *x, uint16 y) {  // 86e883
  return GetTileAttribute(sprite_floor[k], x, y);
}

// Returns true if the sprite's sensor point (x, y) intersects the solid portion
// of a sloped tile. sprite_tiletype must have been set to the sloped type (0x10-0x13)
// before calling. Uses the kSlopedTile[32] table which stores for each of the 4 slope
// varieties × 8 sub-column positions the threshold Y sub-pixel at which the slope
// becomes solid:
//   r6 = tiletype - 0x10 (0=gentle slope SW, 1=gentle slope SE, 2=steep SW, 3=steep SE)
//   b  = kSlopedTile[r6*8 + (x & 7)] — height threshold for this column.
//   r6 < 2 (south-rising slopes): solid if sub-Y >= threshold (b >= a).
//   r6 >= 2 (north-rising slopes): solid if sub-Y <= threshold (a >= b).
bool Entity_CheckSlopedTileCollision(uint16 x, uint16 y) {  // 86e8fe
  uint8 a = y & 7;
  uint8 r6 = sprite_tiletype - 0x10;
  uint8 b = kSlopedTile[r6 * 8 + (x & 7)];
  return (r6 < 2) ? (b >= a) : (a >= b);
}

// Advances sprite k's X and Y world position by one frame of velocity integration.
void Sprite_MoveXY(int k) {  // 86e92c
  Sprite_MoveX(k);
  Sprite_MoveY(k);
}

// Integrates the X velocity into sprite k's world position using sub-pixel precision.
// The full 24-bit position (subpixel | lo | hi) is treated as a fixed-point value;
// velocity is left-shifted by 4 before addition, giving 1/16 pixel resolution.
// This means velocity 1 = 1/16 px/frame; velocity 16 = 1 px/frame.
void Sprite_MoveX(int k) {  // 86e932
  if (sprite_x_vel[k] != 0) {
    uint32 t = sprite_x_subpixel[k] + (sprite_x_lo[k] << 8) + (sprite_x_hi[k] << 16) + ((int8)sprite_x_vel[k] << 4);
    sprite_x_subpixel[k] = t, sprite_x_lo[k] = t >> 8, sprite_x_hi[k] = t >> 16;
  }
}

// Integrates the Y velocity into sprite k's world position with the same fixed-point
// scheme as Sprite_MoveX. Y increases downward (positive vel → moves down the screen).
void Sprite_MoveY(int k) {  // 86e93e
  if (sprite_y_vel[k] != 0) {
    uint32 t = sprite_y_subpixel[k] + (sprite_y_lo[k] << 8) + (sprite_y_hi[k] << 16) + ((int8)sprite_y_vel[k] << 4);
    sprite_y_subpixel[k] = t, sprite_y_lo[k] = t >> 8, sprite_y_hi[k] = t >> 16;
  }
}

// Integrates the Z velocity into sprite k's height (sprite_z, sprite_z_subpos) using
// the same 1/16-pixel sub-pixel accumulator scheme as the XY movement functions.
// Z increases upward (positive z → sprite is higher above the ground, rendering
// higher on screen). Gravity is applied by callers via sprite_z_vel[k] -= 2.
void Sprite_MoveZ(int k) {  // 86e96c
  uint16 z = (sprite_z[k] << 8 | sprite_z_subpos[k]) + ((int8)sprite_z_vel[k] << 4);
  sprite_z_subpos[k] = z;
  sprite_z[k] = z >> 8;
}

// Projects a speed magnitude (vel, in 1/16-pixel units per frame) into X and Y
// velocity components aimed directly at Link's position.
// Uses Bresenham's line algorithm to split vel across the two axes proportionally
// to the X and Y distances (right.b and below.b). The axis with greater distance
// gets the larger component; yvel counts up by the smaller-distance increment and
// xvel absorbs the remainder. Signs are applied based on right.a / below.a
// (1 = Link is to the right/below, 0 = left/above).
// Returns a ProjectSpeedRet with x/y velocities and the raw distance bytes.
ProjectSpeedRet Sprite_ProjectSpeedTowardsLink(int k, uint8 vel) {  // 86e991
  if (vel == 0) {
    ProjectSpeedRet rv = { 0, 0, 0, 0 };
    return rv;
  }
  PairU8 below = Sprite_IsBelowLink(k);
  uint8 r12 = sign8(below.b) ? -below.b : below.b;

  PairU8 right = Sprite_IsRightOfLink(k);
  uint8 r13 = sign8(right.b) ? -right.b : right.b;
  uint8 t;
  bool swapped = false;
  if (r13 < r12) {
    swapped = true;
    t = r12, r12 = r13, r13 = t;
  }
  uint8 xvel = vel, yvel = 0;
  t = 0;
  do {
    t += r12;
    if (t >= r13)
      t -= r13, yvel++;
  } while (--vel);
  if (swapped)
    t = xvel, xvel = yvel, yvel = t;
  ProjectSpeedRet rv = {
    (uint8)(right.a ? -xvel : xvel),
    (uint8)(below.a ? -yvel : yvel),
    right.b,
    below.b

  };
  return rv;
}

// Convenience wrapper that calls Sprite_ProjectSpeedTowardsLink and writes the
// resulting X/Y velocities directly into sprite k's velocity fields.
void Sprite_ApplySpeedTowardsLink(int k, uint8 vel) {  // 86ea04
  ProjectSpeedRet pt = Sprite_ProjectSpeedTowardsLink(k, vel);
  sprite_x_vel[k] = pt.x;
  sprite_y_vel[k] = pt.y;
}

// Projects speed magnitude vel toward an arbitrary world target (x, y) instead of
// Link. Same Bresenham decomposition algorithm as Sprite_ProjectSpeedTowardsLink.
// Used by boss sub-projectiles and guided missiles that track a specific point.
ProjectSpeedRet Sprite_ProjectSpeedTowardsLocation(int k, uint16 x, uint16 y, uint8 vel) {  // 86ea2d
  if (vel == 0) {
    ProjectSpeedRet rv = { 0, 0, 0, 0 };
    return rv;
  }
  PairU8 below = Sprite_IsBelowLocation(k, y);
  uint8 r12 = sign8(below.b) ? -below.b : below.b;

  PairU8 right = Sprite_IsRightOfLocation(k, x);
  uint8 r13 = sign8(right.b) ? -right.b : right.b;
  uint8 t;
  bool swapped = false;
  if (r13 < r12) {
    swapped = true;
    t = r12, r12 = r13, r13 = t;
  }
  uint8 xvel = vel, yvel = 0;
  t = 0;
  do {
    t += r12;
    if (t >= r13)
      t -= r13, yvel++;
  } while (--vel);
  if (swapped)
    t = xvel, xvel = yvel, yvel = t;
  ProjectSpeedRet rv = {
    (uint8)(right.a ? -xvel : xvel),
    (uint8)(below.a ? -yvel : yvel),
    right.b,
    below.b
  };
  return rv;
}

// Determines which cardinal direction sprite k should face to look at Link.
// Computes the absolute horizontal (xm) and vertical (ym) screen distances and
// returns whichever axis is dominant:
//   xm >= ym → right.a (0=right, 1=left) — face left or right.
//   xm <  ym → below.a + 2 (2=down, 3=up) — face up or down.
// Also stores ym in tmp_counter (used by some callers for distance-based logic).
// If coords_out is non-null, fills it with the raw signed delta bytes (right.b / below.b)
// for callers that need the actual displacement as well as the direction.
uint8 Sprite_DirectionToFaceLink(int k, PointU8 *coords_out) {  // 86eaa4
  PairU8 below = Sprite_IsBelowLink(k);
  PairU8 right = Sprite_IsRightOfLink(k);
  uint8 ym = sign8(below.b) ? -below.b : below.b;
  tmp_counter = ym;
  uint8 xm = sign8(right.b) ? -right.b : right.b;
  if (coords_out)
    coords_out->x = right.b, coords_out->y = below.b;
  return (xm >= ym) ? right.a : below.a + 2;
}

// Returns the horizontal relationship between sprite k and Link:
//   PairU8.a = 0 if Link is to the right of sprite (x delta positive).
//   PairU8.a = 1 if Link is to the left (x delta negative / high bit set).
//   PairU8.b = low 8 bits of (link_x_coord - sprite_x), i.e. the signed pixel delta.
PairU8 Sprite_IsRightOfLink(int k) {  // 86ead1
  uint16 x = link_x_coord - Sprite_GetX(k);
  PairU8 rv = { (uint8)(sign16(x) ? 1 : 0), (uint8)x };
  return rv;
}

// Returns the vertical relationship between sprite k and Link, accounting for Z height.
// Link's effective Y is link_y_coord + 8 (centre of the 16-pixel player sprite).
// sprite_z[k] is subtracted from sprite Y to align the sprite's ground plane with
// Link's feet regardless of how high the sprite is hovering.
//   PairU8.a = 0 if Link is below sprite (delta positive).
//   PairU8.a = 1 if Link is above sprite (delta negative).
//   PairU8.b = low 8 bits of the signed Y delta.
PairU8 Sprite_IsBelowLink(int k) {  // 86eae8
  int t = BYTE(link_y_coord) + 8;
  int u = (t & 0xff) + sprite_z[k];
  int v = (u & 0xff) - sprite_y_lo[k];
  int w = HIBYTE(link_y_coord) - sprite_y_hi[k] - (v < 0);
  uint8 y = (w & 0xff) + (t >> 8) + (u >> 8);
  PairU8 rv = { (uint8)(sign8(y) ? 1 : 0), (uint8)v };
  return rv;
}

// Horizontal relationship between sprite k and an arbitrary world X coordinate.
// Same as Sprite_IsRightOfLink but compares against the supplied x rather than Link.
PairU8 Sprite_IsRightOfLocation(int k, uint16 x) {  // 86eb0a
  uint16 xv = x - Sprite_GetX(k);
  PairU8 rv = { (uint8)(sign16(xv) ? 1 : 0), (uint8)xv };
  return rv;
}

// Vertical relationship between sprite k and an arbitrary world Y coordinate.
// Same as Sprite_IsBelowLink but without the Z-height adjustment (used for
// projectile targets that don't have a Z offset).
PairU8 Sprite_IsBelowLocation(int k, uint16 y) {  // 86eb1d
  uint16 yv = y - Sprite_GetY(k);
  PairU8 rv = { (uint8)(sign16(yv) ? 1 : 0), (uint8)yv };
  return rv;
}

// Variant of Sprite_DirectionToFaceLink that targets an arbitrary world coordinate
// instead of Link. Returns a cardinal direction index (0-3) and sets tmp_counter to
// the vertical distance magnitude for the caller's use.
uint8 Sprite_DirectionToFaceLocation(int k, uint16 x, uint16 y) {  // 86eb30
  PairU8 below = Sprite_IsBelowLocation(k, y);
  PairU8 right = Sprite_IsRightOfLocation(k, x);
  uint8 ym = sign8(below.b) ? -below.b : below.b;
  tmp_counter = ym;
  uint8 xm = sign8(right.b) ? -right.b : right.b;
  return (xm >= ym) ? right.a : below.a + 2;
}

// Handles the sword-parry behaviour for shield-bearing guards (soldiers, knights).
// If Link is on a different floor, is incapacitated, in auxiliary state, or the
// guard still has its hit-timer set, bail out.
//
// The function distinguishes three outcomes based on the sword-swing context:
//   1. Link is in a special position mode (levitated / on a bridge at Y==0x80):
//      → Attempt direct damage (guard hits Link regardless of facing).
//   2. Link is not thrusting (button_b_frames < 0 = no B press) OR the guard's
//      hitbox does not overlap Link's action hitbox:
//      → If guard's body hitbox overlaps Link: apply electro-zap damage.
//      → Otherwise: attempt standard damage to Link.
//   3. Sword fully connected (b_frames >= 0 and action hitbox overlap):
//      → Guard successfully parries: sets a recoil timer on the guard (unless it is
//        type 0x6A — anti-fairy, which can't be recoiled), incapacitates Link for
//        a random duration, bounces Link and the guard back via Sprite_ProjectSpeedTowardsLink,
//        plays a "tink" spark effect, and marks the sword delay timer (set_when_damaging = 0x90).
//        Strong thrusts (b_frames < 9) produce larger recoil (32 vs 24 guard, 8 vs 16 Link).
void Guard_ParrySwordAttacks(int k) {  // 86eb5e
  if (link_is_on_lower_level != sprite_floor[k] || link_incapacitated_timer | link_auxiliary_state || sign8(sprite_hit_timer[k]))
    return;
  SpriteHitBox hb;
  Sprite_DoHitBoxesFast(k, &hb);
  if (link_position_mode & 0x10 || player_oam_y_offset == 0x80) {
    Sprite_AttemptDamageToLinkWithCollisionCheck(k);
    return;
  }
  Player_SetupActionHitBox(&hb);
  if (sign8(button_b_frames) || !CheckIfHitBoxesOverlap(&hb)) {
    Sprite_SetupHitBox(k, &hb);
    if (!CheckIfHitBoxesOverlap(&hb))
      Sprite_AttemptDamageToLinkWithCollisionCheck(k);
    else
      Sprite_AttemptZapDamage(k);
    return;
  }
  if (sprite_type[k] != 0x6a)
    sprite_F[k] = kSprite_Func1_Tab[GetRandomNumber() & 7];
  link_incapacitated_timer = kSprite_Func1_Tab2[GetRandomNumber() & 7];
  ProjectSpeedRet pt = Sprite_ProjectSpeedTowardsLink(k, sign8(button_b_frames - 9) ? 32 : 24);
  sprite_x_recoil[k] = -pt.x;
  sprite_y_recoil[k] = -pt.y;
  Sprite_ApplyRecoilToLink(k, sign8(button_b_frames - 9) ? 8 : 16);
  Link_PlaceWeaponTink();
  set_when_damaging_enemies = 0x90;
}

// Applies electro-zap or sword damage based on sprite type and current link state.
// Electro-zap (applies shock to Link) is triggered when:
//   - Sprite is Agahnim (0x7A), OR a buzz-blob (0x0D) with non-Master Sword, OR
//     a spark (0x24/0x23) that is still animating (delay_main != 0).
//   AND the sprite is active (state == 9) AND countdown_for_blink == 0.
//   In this case, sets link_electrocute_on_touch = 64 and deals contact damage.
// All other cases: calculates pushback from the link sword swing strength and
// calls Sprite_CalculateSwordDamage (so the player's sword actually damages the sprite).
void Sprite_AttemptZapDamage(int k) {  // 86ec02
  uint8 a = sprite_type[k];
  if ((a == 0x7a || a == 0xd && (a = link_sword_type) < 4 || (a == 0x24 || a == 0x23) && sprite_delay_main[k] != 0) && sprite_state[k] == 9) {
    if (!countdown_for_blink) {
      sprite_delay_aux1[k] = 64;
      link_electrocute_on_touch = 64;
      Sprite_AttemptDamageToLinkPlusRecoil(k);
    }
  } else {
    ProjectSpeedRet pt = Sprite_ProjectSpeedTowardsLink(k, sign8(button_b_frames - 9) ? 0x50 : 0x40);
    sprite_x_recoil[k] = -pt.x;
    sprite_y_recoil[k] = -pt.y;
    Sprite_CalculateSwordDamage(k);
  }
}

// Applies a preset damage class a to sprite k (used by ancilla weapons: boomerang,
// hookshot, arrows, bombs, etc.). Special cases:
//   a == 15 (fire rod): skip if sprite is airborne (z != 0) — fire doesn't hit flying.
//   a == 0 or 7 (arrow tip / zero-damage hit): after applying damage, if no damage
//     was scored AND no repulse-spark is already pending, trigger a "tink" spark
//     effect at the ancilla's position (byte_7E0FB6 = last ancilla slot index).
// All other values call Sprite_Func15 directly without the tink fallback.
void Ancilla_CheckDamageToSprite_preset(int k, int a) {  // 86ece0
  if (a == 15 && sprite_z[k] != 0)
    return;

  if (a != 0 && a != 7) {
    Sprite_Func15(k, a);
    return;
  }
  Sprite_Func15(k, a);
  if (sprite_give_damage[k] || repulsespark_timer)
    return;
  // Called when hitting enemy which is frozen
  repulsespark_timer = 5;
  int j = byte_7E0FB6;
  repulsespark_x_lo = ancilla_x_lo[j] + 4;
  repulsespark_y_lo = ancilla_y_lo[j];
  repulsespark_floor_status = link_is_on_lower_level;
  sound_effect_1 = 0;
  SpriteSfx_QueueSfx2WithPan(k, 5);
}

// Sets damage_type_determiner = a and calls Sprite_ApplyCalculatedDamage.
// The hit-timer initial value differs: a == 8 (hammer) uses 0x35 for longer stun;
// all other classes use 0x20 (standard stun duration).
void Sprite_Func15(int k, int a) {  // 86ed25
  damage_type_determiner = a;
  Sprite_ApplyCalculatedDamage(k, a == 8 ? 0x35 : 0x20);
}

// Determines the sword damage class from Link's current sword/swing state and
// applies it to sprite k. Guards against invulnerable sprites (flags3 bit 6).
// Damage class selection (a = link_sword_type - 1, then OR with swing modifier):
//   If running (spin attack or dash): damage class = sword_type - 1 (base class).
//   Normal thrust: button_b_frames < 0 → +4 (light tap); < 9 → +0 (medium);
//                  else → +8 (long press / held swing).
//   kSprite_Func14_Damage[a] maps these to the actual damage class index.
//   Fire Shield or Cape (link_item_in_hand & 10): forces class 3 (hammer-equivalent).
// Sets link_sword_delay_timer = 4 and set_when_damaging_enemies = 16.
void Sprite_CalculateSwordDamage(int k) {  // 86ed3f
  if (sprite_flags3[k] & 0x40)
    return;
  sprite_unk1[k] = link_is_running;
  uint8 a = link_sword_type - 1;
  if (!link_is_running)
    a |= sign8(button_b_frames) ? 4 : sign8(button_b_frames - 9) ? 0 : 8;
  damage_type_determiner = kSprite_Func14_Damage[a];
  if (link_item_in_hand & 10)
    damage_type_determiner = 3;
  link_sword_delay_timer = 4;
  set_when_damaging_enemies = 16;
  Sprite_ApplyCalculatedDamage(k, 0x9d);
}

// Applies pre-computed damage_type_determiner to sprite k using the enemy damage
// matrix. Guards against invulnerable sprites (flags3 bit 6) and item pickups
// (sprite_type >= 0xD8). Looks up the damage value as:
//   kEnemyDamages[damage_type_determiner * 8 | enemy_damage_data[type * 16 | dtd]]
// The inner table (enemy_damage_data) maps each enemy type × damage class to a
// "deterrence index" (0-7); the outer table (kEnemyDamages) maps that to HP lost.
// Then calls Sprite_GiveDamage(k, dmg, r0_hit_timer) where r0_hit_timer is the
// initial hit-timer value passed in (controls stun duration / flash length).
void Sprite_ApplyCalculatedDamage(int k, int a) {  // 86ed89
  if ((sprite_flags3[k] & 0x40) || sprite_type[k] >= 0xD8)
    return;
  uint8 dmg = kEnemyDamages[damage_type_determiner * 8 | enemy_damage_data[sprite_type[k] * 16 | damage_type_determiner]];
  Sprite_GiveDamage(k, dmg, a);
}

// Final stage of the damage pipeline: applies the computed damage value dmg to
// sprite k and sets the initial hit-timer to r0_hit_timer (controls flash duration
// and recoil window). Special dmg values:
//   249 → Cukeman conversion: transmutes sprite to type 0xE3 (cukeman NPC).
//   250 → Instant-kill special (e.g., hammered Wizzrobe): transmutes to type 0x8F,
//         sets ai_state=2, z_vel=32, health=0, bump_damage=1, flags5=1 for death arc.
//   0   → No damage (immune / shield parry):
//           - If damage_type != 10 (not arrow): zero sword delay unless flags bit 2 set.
//           - Clears hit_timer and give_damage — no stun, no flash.
//   251-255 → Special stun/freeze effects handled downstream by Sprite_MiniMoldorm_Recoil.
//           dmg 254 = freeze (ice rod), 253 = burn, 252/251 = varying stun grades.
//   Normal  → Updates give_damage to the maximum seen so far (damage accumulates
//             across multiple stacked hits). Sets sprite_F[k] to a type-specific recoil
//             duration and plays a hit SFX (metallic 0x21, ring 0x1C, or default 0x08).
// Special flag4 path: sprites with flags[k] bit 2 set skip the zero-damage bail and
// still get their sprite_F set, allowing "immortal" enemies to visually recoil.
void Sprite_GiveDamage(int k, uint8 dmg, uint8 r0_hit_timer) {  // 86edc5
  if (dmg == 249) {
    Sprite_Func18(k, 0xe3);
    return;
  }
  if (dmg == 250) {
    Sprite_Func18(k, 0x8f);
    sprite_ai_state[k] = 2;
    sprite_z_vel[k] = 32;
    sprite_oam_flags[k] = 8;
    sprite_F[k] = 0;
    sprite_hit_timer[k] = 0;
    sprite_health[k] = 0;
    sprite_bump_damage[k] = 1;
    sprite_flags5[k] = 1;
    return;
  }
  if (dmg >= sprite_give_damage[k])
    sprite_give_damage[k] = dmg;
  if (dmg == 0) {
    if (damage_type_determiner != 10) {
      if (sprite_flags[k] & 4)
        goto flag4;
      link_sword_delay_timer = 0;
    }
    sprite_hit_timer[k] = 0;
    sprite_give_damage[k] = 0;
    return;
  }
  if (dmg >= 254 && sprite_state[k] == 11) {
    sprite_hit_timer[k] = 0;
    sprite_give_damage[k] = 0;
    return;
  }
  if (sprite_type[k] == 0x9a && sprite_give_damage[k] < 0xf0) {
    sprite_state[k] = 9;
    sprite_ai_state[k] = 4;
    sprite_delay_main[k] = 15;
    SpriteSfx_QueueSfx2WithPan(k, 0x28);
    return;
  }
  if (sprite_type[k] == 0x1b) {
    SpriteSfx_QueueSfx2WithPan(k, 5);
    Sprite_ScheduleForBreakage(k);
    Sprite_PlaceWeaponTink(k);
    return;
  }
  sprite_hit_timer[k] = r0_hit_timer;
  if (sprite_type[k] != 0x92 || sprite_C[k] >= 3) {
    uint8 sfx = sprite_flags[k] & 2 ? 0x21 :
                sprite_flags5[k] & 0x10 ? 0x1c : 8;
    sound_effect_2 = sfx | Sprite_CalculateSfxPan(k);
  }
  uint8 type;
flag4:
  type = sprite_type[k];
  sprite_F[k] = (damage_type_determiner >= 13) ? 0 :
                (type == 9) ? 20 :
                (type == 0x53 || type == 0x18) ? 11 : 15;
}

// Transmutes sprite k to a new type in-place (used for cukeman conversion and
// Wizzrobe instant-kill). Reloads all properties from the init tables for the
// new type, spawns a poof garnish to signal the transformation, plays SFX 0x32
// (transmutation sound), and clears hit_timer and give_damage so the newly
// transformed sprite starts fresh without any pending damage state.
void Sprite_Func18(int k, uint8 new_type) {  // 86edcb
  sprite_type[k] = new_type;
  SpritePrep_LoadProperties(k);
  Sprite_SpawnPoofGarnish(k);
  sound_effect_2 = 0;
  SpriteSfx_QueueSfx3WithPan(k, 0x32);
  sprite_hit_timer[k] = 0;
  sprite_give_damage[k] = 0;
}

// Processes the deferred hit at hit_timer == 24 (applied 7 frames after the
// initial hit at timer == 31). At this point sprite_give_damage[k] contains the
// final accumulated damage for this hit. Handles four distinct outcomes:
//
//   dmg == 253 (fire/burn): transitions to state 7 (burn death), delay = 0x70,
//     flags2 += 2 for extra OAM, plays SFX 9.
//
//   dmg >= 251 (stun/freeze values 251-254):
//     Clears give_damage and skips if sprite is already thrown (state 11).
//     dmg == 254 (freeze): sets sprite_unk5[k] = 1 (frozen flag), enables
//       simplified tile collision (defl_bits |= 8), clears the air-flag (flags5 ~0x80),
//       plays ice SFX, adds upward arc (z_vel=24), clears bump_damage carry bit,
//       zeroes XY velocities.
//     All stun variants: set state = 11 (stunned), delay_main = 64.
//     kHitTimer24StunValues[(uint8)(dmg+5)] selects sprite_stunned duration:
//       254 → 0x20 (32 frames), 253 → 0x80 (128 frames), others → 0 or 255.
//     Type 0x23 (beam/spark) is morphed to 0x24 on stun.
//
//   HP lethal (health - give_damage <= 0): begins the death sequence:
//     Adjusts die_action for thrown sprites; clears spin-attack flag;
//     handles type-specific death effects (Agahnim splitting, Kholdstare melting,
//     Vitreous bubble decrement, evil barrier, throwable skull, etc.);
//     sets state = 6 and delay/flags for poof death animation;
//     boss types (flags & 2) switch to state 4 explosion and block the menu.
//
//   Normal (survive): no action (function returns after give_damage deduction).
void Sprite_MiniMoldorm_Recoil(int k) {  // 86eec8
  if (sprite_state[k] < 9)
    return;
  tmp_counter = sprite_state[k];

  uint8 dmg = sprite_give_damage[k];
  if (dmg == 253) {
    sprite_give_damage[k] = 0;
    SpriteSfx_QueueSfx3WithPan(k, 9);
    sprite_state[k] = 7;
    sprite_delay_main[k] = 0x70;
    sprite_flags2[k] += 2;
    sprite_give_damage[k] = 0;
    return;
  }

  if (dmg >= 251) {
    sprite_give_damage[k] = 0;
    if (sprite_state[k] == 11)
      return;
    sprite_unk5[k] = (dmg == 254);
    if (sprite_unk5[k]) {
      sprite_defl_bits[k] |= 8;
      sprite_flags5[k] &= ~0x80;
      SpriteSfx_QueueSfx2WithPan(k, 15);
      sprite_z_vel[k] = 24;
      sprite_bump_damage[k] &= ~0x80;
      Sprite_ZeroVelocity_XY(k);
    }
    sprite_state[k] = 11;
    sprite_delay_main[k] = 64;
    static const uint8 kHitTimer24StunValues[5] = {0x20, 0x80, 0, 0, 0xff};
    sprite_stunned[k] = kHitTimer24StunValues[(uint8)(dmg + 5)];
    if (sprite_type[k] == 0x23)
      sprite_type[k] = 0x24;
    return;
  }

  int t = sprite_health[k] - sprite_give_damage[k];
  sprite_health[k] = t;
  sprite_give_damage[k] = 0;
  if (t > 0)
    return;

  if (sprite_die_action[k] == 0) {
    if (sprite_state[k] == 11)
      sprite_die_action[k] = 3;
    if (sprite_unk1[k] != 0) {
      sprite_unk1[k] = 0;
      sprite_flags5[k] = 0;
    }
  }

  uint8 type = sprite_type[k];
  if (type != 0x1b)
    SpriteSfx_QueueSfx3WithPan(k, 9);

  if (type == 0x40)
    save_ow_event_info[BYTE(overworld_screen_index)] |= 0x40;
  else if (type == 0xec) {
    if (sprite_C[k] == 2)
      ThrowableScenery_TransmuteToDebris(k);
    return;
  }

  if (sprite_state[k] == 10) {
    link_state_bits = 0;
    link_picking_throw_state = 0;
  }
  sprite_state[k] = 6;

  if (type == 0xc) {
    Sprite_Func3(k);
  } else if (type == 0x92) {
    Sprite_KillFriends();
    sprite_delay_main[k] = 255;
    goto out_common;
  } else if (type == 0xcb) {
    sprite_ai_state[k] = 128;
    sprite_delay_main[k] = 128;
    sprite_state[k] = 9;
    goto out_common;
  } else if (type == 0xcc || type == 0xcd) {
    sprite_ai_state[k] = 128;
    sprite_delay_main[k] = 96;
    sprite_state[k] = 9;
    goto out_common;
  } else if (type == 0x53) {
    sprite_delay_main[k] = 35;
    sprite_hit_timer[k] = 0;
    goto out_common2;
  } else if (type == 0x54) {
    sprite_ai_state[k] = 5;
    sprite_delay_main[k] = 0xc0;
    sprite_hit_timer[k] = 0xc0;
    goto out_common;
  } else if (type == 0x9) {
    sprite_ai_state[k] = 3;
    sprite_delay_aux4[k] = 160;
    sprite_state[k] = 9;
    goto out_common;
  } else if (type == 0x7a) {
    Sprite_KillFriends();
    sprite_state[k] = 9;
    sprite_ignore_projectile[k] = 9;
    if (is_in_dark_world == 0) {
      sprite_ai_state[k] = 10;
      sprite_delay_main[k] = 255;
      sprite_z_vel[k] = 32;
    } else {
      sprite_delay_main[k] = 255;
      sprite_ai_state[k] = 8;
      sprite_ai_state[1] = 9;
      sprite_ai_state[2] = 9;
      sprite_graphics[1] = 0;
      sprite_graphics[2] = 0;
    }
    goto out_common;
  } else if (type == 0x23 && sprite_C[k] == 0) {
    sprite_ai_state[k] = 2;
    sprite_delay_main[k] = 32;
    sprite_state[k] = 9;
    sprite_hit_timer[k] = 0;
  } else if (type == 0xf) {
    sprite_hit_timer[k] = 0;
    sprite_delay_main[k] = 15;
  } else if (!(sprite_flags[k] & 2)) {
    sprite_delay_main[k] = sprite_hit_timer[k] & 0x80 ? 31 : 15;
    sprite_flags2[k] += 4;
    if (tmp_counter == 11)
      sprite_flags5[k] = 1;
  } else {
    if (type != 0xa2)
      Sprite_KillFriends();
    sprite_state[k] = 4;
    sprite_A[k] = 0;
    sprite_delay_main[k] = 255;
    sprite_hit_timer[k] = 255;
  out_common:
    flag_block_link_menu++;
  out_common2:
    sound_effect_2 = 0;
    SpriteSfx_QueueSfx3WithPan(k, 0x22);
  }
}

// Minimal death initialiser used for certain minor enemies (e.g., small enemies
// in multi-component bosses). Sets state = 6 (die), delay = 31 (standard poof
// duration), and resets flags2 to 3 (minimum two-slot OAM for the poof animation).
void Sprite_Func3(int k) {  // 86efda
  sprite_state[k] = 6;
  sprite_delay_main[k] = 31;
  sprite_flags2[k] = 3;
}

// Returns true if the sprite's contact hitbox currently overlaps Link's hitbox
// AND link_disable_sprite_damage is clear (player is vulnerable).
// The per-frame stagger check inside Sprite_CheckDamageToPlayer_1 limits this test
// to one in every 4 frames per slot to avoid multiple damage events from a single
// contact, and only runs when the sprite's hit_timer is zero.
bool Sprite_CheckDamageToLink(int k) {  // 86f145
  if (link_disable_sprite_damage)
    return false;
  return Sprite_CheckDamageToPlayer_1(k);
}

// Stagger-checks the sprite: ((k ^ frame_counter) & 3) ensures each slot is tested
// only on frames where (frame_counter mod 4) == (k mod 4), spreading the checks
// evenly across the 16 slots. Also guards against hit_timer (mid-stun immunity).
bool Sprite_CheckDamageToPlayer_1(int k) {  // 86f14a
  if ((k ^ frame_counter) & 3 | sprite_hit_timer[k])
    return false;
  return Sprite_CheckDamageToLink_same_layer(k);
}

// Returns false if sprite and Link are on different floors (upper vs lower dungeon
// level). This prevents sprites on one platform from damaging Link on another.
bool Sprite_CheckDamageToLink_same_layer(int k) {  // 86f154
  if (link_is_on_lower_level != sprite_floor[k])
    return false;
  return Sprite_CheckDamageToLink_ignore_layer(k);
}

// Core contact-damage check: tests AABB overlap between Link and sprite k, then
// handles the shield-block logic.
//
// Hitbox setup:
//   If sprite_flags4[k] != 0: builds a full SpriteHitBox and Link hitbox, calls
//     CheckIfHitBoxesOverlap for a proper AABB test. With MiscBugFixes enabled,
//     absorbable item sprites (0xD8-0xE6) also test against the sword action box
//     so Link can collect them with a sword swing.
//   If sprite_flags4[k] == 0: uses the simplified Sprite_SetupHitBox00 inline test
//     (±11px X, ±24px Y from sprite centre — no table lookup, just distance check).
//
// When flags2 bit 7 is set (large-hitbox override): return carry directly.
// Otherwise, check for shield parry: if Link has a shield, is not in bunny/mirror
// form, is not mid-spin, and the sprite has the "blockable" flag (flags5 bit 5),
// test whether Link is facing the sprite. If so, kill the sprite (state = 0) — it
// was blocked. If not, fall through to Sprite_AttemptDamageToLinkPlusRecoil.
// Special cases for type 0xC (portal trap), 0x95 (anti-fairy), 0x9B (rolling skull),
// 0x1B (breakable arrow). Each has custom shield-blocked reactions.
bool Sprite_CheckDamageToLink_ignore_layer(int k) {  // 86f15c
  uint8 carry, t;
  if (sprite_flags4[k]) {
    SpriteHitBox hitbox;
    Link_SetupHitBox(&hitbox);

    // Set hitbox to the sword hitbox if the item type is an absorbable
    if (sprite_type[k] >= 0xd8 && sprite_type[k] <= 0xe6 && (enhanced_features0 & kFeatures0_CollectItemsWithSword))
      Link_UpdateHitBoxWithSword(&hitbox);

    Sprite_SetupHitBox(k, &hitbox);
    carry = CheckIfHitBoxesOverlap(&hitbox);
  } else {
    carry = Sprite_SetupHitBox00(k);
  }

  if (sign8(sprite_flags2[k]))
    return carry;

  if (!carry || link_auxiliary_state)
    return false;

  if (link_is_bunny_mirror || sign8(link_state_bits) || !(sprite_flags5[k] & 0x20) || !link_shield_type)
    goto if_3;
  sprite_state[k] = 0;

  t = button_b_frames ? kSpriteDamage_Tab2[link_direction_facing >> 1] : link_direction_facing;
  if (t != kSpriteDamage_Tab3[sprite_D[k]]) {
if_3:
    Sprite_AttemptDamageToLinkPlusRecoil(k);
    if (sprite_type[k] == 0xc)
      Sprite_Func3(k);
    return true;
  }
  SpriteSfx_QueueSfx2WithPan(k, 6);
  Sprite_PlaceRupulseSpark_2(k);
  if (sprite_type[k] == 0x95) {
    SpriteSfx_QueueSfx3WithPan(k, 0x26);
    return false;
  } else if (sprite_type[k] == 0x9B) {
    Sprite_Invert_XY_Speeds(k);
    sprite_D[k] ^= 1;
    sprite_ai_state[k]++;
    sprite_state[k] = 9;
    return false;
  } else if (sprite_type[k] == 0x1B) { // arrow
    Sprite_ScheduleForBreakage(k);
    return false;  // unk ret val
  } else if (sprite_type[k] == 0xc) {
    Sprite_Func3(k);
    return true;
  } else {
    return false;  // unk ret val
  }
}

// Simplified hitbox test when sprite_flags4[k] == 0 (no hitbox table entry).
// Tests whether Link's X is within ±11 pixels and Link's Y (adjusted for sprite Z)
// is within ±12 pixels (24 pixel window) of the sprite centre. Uses 16-bit unsigned
// arithmetic to handle wrap-around correctly with a single comparison per axis.
bool Sprite_SetupHitBox00(int k) {  // 86f1f6
  return (uint16)(link_x_coord - cur_sprite_x + 11) < 23 &&
         (uint16)(link_y_coord - cur_sprite_y + sprite_z[k] + 16) < 24;
}

// Tests whether the sprite can be picked up this frame by a strict set of conditions,
// then delegates to Sprite_ReturnIfLiftedPermissive for the actual lift logic.
// Strict conditions that block lifting:
//   - Currently in a submodule, B is held, or flag_unk1 (game paused).
//   - Different floor level.
//   - Another sprite is already in the carried state (state 10).
//   - Sprite has non-zero XY velocity (unless it is a bomb or cane platform).
//   - Link is running (spin attack / dash).
bool Sprite_ReturnIfLifted(int k) {  // 86f228
  if (submodule_index | button_b_frames | flag_unk1 || sprite_floor[k] != link_is_on_lower_level)
    return false;
  for (int j = 15; j >= 0; j--)
    if (sprite_state[j] == 10)
      return false;
  if (sprite_type[k] != 0xb && sprite_type[k] != 0x4a && (sprite_x_vel[k] | sprite_y_vel[k]) != 0)
    return false;
  if (link_is_running)
    return false;
  return Sprite_ReturnIfLiftedPermissive(k);
}

// Permissive lift check — skips the running restriction and motion check.
// Two-phase logic:
//   Phase 1 (flag_is_sprite_to_pick_up_cached - 1 != cur_object_index):
//     Link is not yet committed to lifting this sprite. Tests the carry hitbox;
//     if overlapping, records this slot as the candidate (flag_is_sprite_to_pick_up
//     = k+1) but does not yet initiate the lift. Returns false.
//   Phase 2 (matches): The sprite was already registered as the pick-up target.
//     Clears the Y-button input to consume the press, clears sprite_E (ground flag),
//     plays pick-up SFX 0x1D, saves the current state into sprite_unk4 (so the sprite
//     returns to the right state on throw), transitions to state 10 (carried), sets
//     delay = 16 for the lift arc, and turns Link to face the sprite.
//     Returns true, signalling the caller to skip the normal AI tick this frame.
bool Sprite_ReturnIfLiftedPermissive(int k) {  // 86f257
  if (link_is_running)
    return false;
  if ((uint8)(flag_is_sprite_to_pick_up_cached - 1) != cur_object_index) {
    SpriteHitBox hb;
    Link_SetupHitBox_conditional(&hb);
    Sprite_SetupHitBox(k, &hb);
    if (CheckIfHitBoxesOverlap(&hb))
      byte_7E0FB2 = flag_is_sprite_to_pick_up = k + 1;
    return false;
  } else {
    BYTE(filtered_joypad_L) = 0;
    sprite_E[k] = 0;
    SpriteSfx_QueueSfx2WithPan(k, 0x1d);
    sprite_unk4[k] = sprite_state[k];
    sprite_state[k] = 10;
    sprite_delay_main[k] = 16;
    sprite_unk3[k] = 0;
    sprite_I[k] = 0;
    link_direction_facing = kSprite_ReturnIfLifted_Dirs[Sprite_DirectionToFaceLink(k, NULL)];
    return true;
  }
}

// Tests whether Link's action box (sword swing, spin, dash, bow, hookshot, etc.)
// overlaps sprite k and, if so, applies the appropriate damage or interaction.
// Returns a bitmask: kCheckDamageFromPlayer_Carry (bit set = Link can still carry),
// kCheckDamageFromPlayer_Ne (bit set = hit scored), or 0 for no interaction.
//
// Early bail conditions:
//   - sprite_hit_timer high bit set (sprite recently staggered, still immune).
//   - Different floor or Link in the upper-bridge pose (player_oam_y_offset == 0x80).
//   - link_position_mode bit 4 (levitated): returns carry flag without damage.
//
// Weapon special handling before the default damage path:
//   - Cape/Fire Shield (link_item_in_hand & 10): crushes frozen sprites (state 11 +
//     unk5 set) into poof. Skips boss-tier sprites (type >= 0xD6).
//   - Type 0x7B (Wizzrobe) requires a fast heavy swing (button_b_frames < 9).
//   - Type 0x09 (Armos Knight) with A==0: deflects with spark + link recoil.
//   - Type 0x92 (multi-ball): routes to "is_many" (large recoil, no damage).
//   - Types 0x26/0x13/0x02 (various sparks): apply zap + recoil + optional tink.
//   - Types 0xCB/0xCC/0xCD/0xD6/0xD7/0xCE/0x54 (large bosses): large recoil only.
//
// Default path: calls Sprite_AttemptZapDamage (which calls Sprite_CalculateSwordDamage
// for most enemies). If sprite_defl_bits[k] bit 2 is set (indestructible shell),
// skips to the "getting_out" tink/recoil path instead.
uint8 Sprite_CheckDamageFromLink(int k) {  // 86f2b4
  if (sprite_hit_timer[k] & 0x80 || sprite_floor[k] != link_is_on_lower_level || player_oam_y_offset == 0x80)
    return 0;

  SpriteHitBox hb;
  Player_SetupActionHitBox(&hb);
  Sprite_SetupHitBox(k, &hb);
  if (!CheckIfHitBoxesOverlap(&hb))
    return 0;

  set_when_damaging_enemies = 0;
  if (link_position_mode & 0x10)
    return kCheckDamageFromPlayer_Carry | kCheckDamageFromPlayer_Ne;

  if (link_item_in_hand & 10) {
    if (sprite_type[k] >= 0xd6)
      return 0;
    if (sprite_state[k] == 11 && sprite_unk5[k] != 0) {
      sprite_state[k] = 2;
      sprite_delay_main[k] = 32;
      sprite_flags2[k] = (sprite_flags2[k] & 0xe0) | 3;
      SpriteSfx_QueueSfx2WithPan(k, 0x1f);
      return kCheckDamageFromPlayer_Carry | kCheckDamageFromPlayer_Ne;
    }
  }
  uint8 type = sprite_type[k];
  if (type == 0x7b) {
    if (!sign8(button_b_frames - 9))
      return 0;
  } else if (type == 9) {
    if (!sprite_A[k]) {
      Sprite_ApplyRecoilToLink(k, 48);
      set_when_damaging_enemies = 144;
      link_incapacitated_timer = 16;
      SpriteSfx_QueueSfx2WithPan(k, 0x21);
      sprite_delay_aux1[k] = 48;
      sound_effect_2 = Sprite_CalculateSfxPan(k) | (enhanced_features0 & kFeatures0_MiscBugFixes ? 0x32 : 0);
      Link_PlaceWeaponTink();
      return kCheckDamageFromPlayer_Carry;
    }
  } else if (type == 0x92) {
    if (sprite_C[k] >= 3)
      goto is_many;
    goto getting_out;
  } else if (type == 0x26 || type == 0x13 || type == 2) {
    bool cond = (type == 0x13 && kSpriteDamage_Tab3[sprite_D[k]] == link_direction_facing) || (type == 2);
    Sprite_AttemptZapDamage(k);
    Sprite_ApplyRecoilToLink(k, 32);
    set_when_damaging_enemies = 16;
    link_incapacitated_timer = 16;
    if (cond) {
      sprite_hit_timer[k] = 0;
      Link_PlaceWeaponTink();
    }
    return 0; // what return value?
  } else if (type == 0xcb || type == 0xcd || type == 0xcc || type == 0xd6 || type == 0xd7 || type == 0xce || type == 0x54) {
is_many:
    Sprite_ApplyRecoilToLink(k, 32);
    set_when_damaging_enemies = 144;
    link_incapacitated_timer = 16;
  }
  if (!(sprite_defl_bits[k] & 4)) {
    Sprite_AttemptZapDamage(k);
    return kCheckDamageFromPlayer_Carry;
  }
getting_out:
  if (!set_when_damaging_enemies) {
    Sprite_ApplyRecoilToLink(k, 4);
    link_incapacitated_timer = 16;
    set_when_damaging_enemies = 16;
  }
  Link_PlaceWeaponTink();
  return kCheckDamageFromPlayer_Carry;
}

// Fast contact-damage check with a 2-frame stagger ((k ^ frame_counter) & 1).
// Uses Sprite_DoHitBoxesFast (which caches the sprite hitbox in dungmap_var8)
// rather than the full Sprite_SetupHitBox path to avoid the table lookup.
// Only calls Sprite_AttemptDamageToLinkPlusRecoil if the overlap test passes.
void Sprite_AttemptDamageToLinkWithCollisionCheck(int k) {  // 86f3ca
  if ((k ^ frame_counter) & 1)
    return;
  SpriteHitBox hb;
  Sprite_DoHitBoxesFast(k, &hb);
  Link_SetupHitBox_conditional(&hb);
  if (CheckIfHitBoxesOverlap(&hb))
    Sprite_AttemptDamageToLinkPlusRecoil(k);
}

// Applies contact damage and knockback to Link. Guards against the post-hit blink
// window (countdown_for_blink) and link_disable_sprite_damage.
// Sets incapacitation timer to 19 frames (standard knockback duration), pushes
// Link away from the sprite at speed 24 via Sprite_ApplyRecoilToLink, sets
// link_auxiliary_state = 1 (tumbling), and reads damage from kPlayerDamages using
// sprite_bump_damage[k] and link_armor as indices.
// Type 0x61 with sprite_C (moving platform): overrides link velocity to the
// platform's velocity × 2 so Link is physically carried by the platform.
void Sprite_AttemptDamageToLinkPlusRecoil(int k) {  // 86f3db
  if (countdown_for_blink | link_disable_sprite_damage)
    return;
  link_incapacitated_timer = 19;
  Sprite_ApplyRecoilToLink(k, 24);
  link_auxiliary_state = 1;
  link_give_damage = kPlayerDamages[3 * (sprite_bump_damage[k] & 0xf) + link_armor];
  if (sprite_type[k] == 0x61 && sprite_C[k]) {
    link_actual_vel_x = sprite_x_vel[k] * 2;
    link_actual_vel_y = sprite_y_vel[k] * 2;
  }
}

// Builds Link's action (weapon swing) hitbox into hb based on the current action state.
// If Link is running (dash/spin): uses kPlayerActionBoxRun tables for a 16×16 box
// placed ahead of Link in the current facing direction.
// Otherwise:
//   - Cape/Fire Shield or levitated: returns a zeroed hitbox (no weapon active).
//   - B not pressed (button_b_frames < 0): 44×45 px full-circle hitbox around Link.
//   - button_b_frames in dead-zone (kPlayer_SetupActionHitBox_Tab4 nonzero): sets
//     r8_xhi = 0x80 (sentinel = hitbox invalid), returns immediately.
//   - Normal thrust: indexes kPlayer_SetupActionHitBox_X/Y/W/H by
//     (facing * 8 + b_frames + 1) for the direction and frame of the sword arc.
// X/Y positions add the player OAM offsets for the crouch/jump/shadow animations.
void Player_SetupActionHitBox(SpriteHitBox *hb) {  // 86f5e0
  if (link_is_running) {
    int j = link_direction_facing >> 1;
    int x = link_x_coord + (kPlayerActionBoxRun_XLo[j] | kPlayerActionBoxRun_XHi[j] << 8);
    int y = link_y_coord + (kPlayerActionBoxRun_YLo[j] | kPlayerActionBoxRun_YHi[j] << 8);
    hb->r0_xlo = x;
    hb->r8_xhi = x >> 8;
    hb->r1_ylo = y;
    hb->r9_yhi = y >> 8;
    hb->r2 = hb->r3 = 16;
  } else {
    int t = 0;
    if (!(link_item_in_hand & 10) && !(link_position_mode & 0x10)) {
      if (sign8(button_b_frames)) {
        int x = link_x_coord - 14;
        int y = link_y_coord - 10;
        hb->r0_xlo = x;
        hb->r8_xhi = x >> 8;
        hb->r1_ylo = y;
        hb->r9_yhi = y >> 8;
        hb->r2 = 44;
        hb->r3 = 45;
        return;
      } else if (kPlayer_SetupActionHitBox_Tab4[button_b_frames]) {
        hb->r8_xhi = 0x80;
        return;
      }
      t = link_direction_facing * 8 + button_b_frames + 1;
    }
    int x = link_x_coord + (int8)(kPlayer_SetupActionHitBox_X[t] + player_oam_x_offset);
    int y = link_y_coord + (int8)(kPlayer_SetupActionHitBox_Y[t] + player_oam_y_offset);
    hb->r0_xlo = x;
    hb->r8_xhi = x >> 8;
    hb->r1_ylo = y;
    hb->r9_yhi = y >> 8;
    hb->r2 = kPlayer_SetupActionHitBox_W[t];
    hb->r3 = kPlayer_SetupActionHitBox_H[t];
  }
}

// Extension for the "collect items with sword" feature: shrinks the active sword
// hitbox slightly (up to 6px per side, then re-centred) and writes it over the
// body hitbox so that items inside the sword's reach can be collected with a swing.
// Returns immediately (no change) if Link is spinning, not pressing B, or B is in
// the dead-zone where no sword arc is active.
void Link_UpdateHitBoxWithSword(SpriteHitBox *hb) {  // new
  if (link_spin_attack_step_counter != 0 || sign8(button_b_frames) ||
      kPlayer_SetupActionHitBox_Tab4[button_b_frames])
    return;
  int t = link_direction_facing * 8 + button_b_frames + 1, r;
  int x = link_x_coord + (int8)(kPlayer_SetupActionHitBox_X[t] + player_oam_x_offset);
  int y = link_y_coord + (int8)(kPlayer_SetupActionHitBox_Y[t] + player_oam_y_offset);
  // Reduce size of hitbox if 'too' big.
  hb->r2 = kPlayer_SetupActionHitBox_W[t];
  if (hb->r2 - 2 >= 0)
    r = IntMin(6, hb->r2 - 2), hb->r2 -= r, x += r >> 1;
  hb->r3 = kPlayer_SetupActionHitBox_H[t];
  if (hb->r3 - 2 >= 0)
    r = IntMin(6, hb->r3 - 2), hb->r3 -= r, y += r >> 1;
  hb->r0_xlo = x;
  hb->r8_xhi = x >> 8;
  hb->r1_ylo = y;
  hb->r9_yhi = y >> 8;
}

// Fast sprite hitbox builder that uses the cached displacement stored in dungmap_var8
// (written by Sprite_PrepOamCoordOrDoubleRet earlier in the frame) instead of the
// full kSpriteHitbox_* table lookup. HIBYTE(dungmap_var8) == 0x80 means the sprite
// is off-screen; in that case, sets r10_spr_xhi = 0x80 (invalid sentinel) and returns.
// Size is hard-coded to 3 for most sprites (6×6 tight box); type 0x6A (anti-fairy)
// uses size 16 for a wider deflection zone.
void Sprite_DoHitBoxesFast(int k, SpriteHitBox *hb) {  // 86f645
  if (HIBYTE(dungmap_var8) == 0x80) {
    hb->r10_spr_xhi = 0x80;
    return;
  }
  int t;
  t = Sprite_GetX(k) + (int8)HIBYTE(dungmap_var8);
  hb->r4_spr_xlo = t;
  hb->r10_spr_xhi = t >> 8;
  t = Sprite_GetY(k) + (int8)BYTE(dungmap_var8);
  hb->r5_spr_ylo = t;
  hb->r11_spr_yhi = t >> 8;
  hb->r6_spr_xsize = hb->r7_spr_ysize = (sprite_type[k] == 0x6a) ? 16 : 3;
}

// Launches Link away from sprite k at the given velocity. Uses Bresenham projection
// to split vel into X/Y components pointed away from the sprite, then applies them
// to link_actual_vel_x/y. Also sets link_actual_vel_z to vel/2 (a small upward arc)
// and zeros link_z_coord so the bounce starts from ground level.
void Sprite_ApplyRecoilToLink(int k, uint8 vel) {  // 86f688
  ProjectSpeedRet pt = Sprite_ProjectSpeedTowardsLink(k, vel);
  link_actual_vel_x = pt.x;
  link_actual_vel_y = pt.y;
  g_ram[0xc7] = link_actual_vel_z = vel >> 1;
  link_z_coord = 0;
}

// Places the repulse-spark at Link's current weapon position (body + OAM offsets)
// and plays SFX 5 (clang). repulsespark_timer = 5 limits one spark per 5 frames.
// Note: the Y coordinate computation adds the carry of the X offset calculation —
// this matches original SNES 6502 behaviour where carry from the X add spills into Y.
void Link_PlaceWeaponTink() {  // 86f69f
  if (repulsespark_timer)
    return;
  repulsespark_timer = 5;
  int t = (uint8)link_x_coord + player_oam_x_offset;
  repulsespark_x_lo = t;
  t = (uint8)link_y_coord + player_oam_y_offset + (t >> 8);  // carry wtf
  repulsespark_y_lo = t;
  repulsespark_floor_status = link_is_on_lower_level;
  sound_effect_1 = Link_CalculateSfxPan() | 5;
}

// Places the repulse-spark at sprite k's position (queues SFX 5 then places spark).
// Used when Link's sword is repelled by a sprite (e.g., anti-fairy, mirror shield).
void Sprite_PlaceWeaponTink(int k) {  // 86f6ca
  if (repulsespark_timer)
    return;
  SpriteSfx_QueueSfx2WithPan(k, 5);
  Sprite_PlaceRupulseSpark_2(k);
}

// Records the sprite k's screen position as the origin for the repulse-spark visual.
// Only fires if the sprite is within the visible 256×256 screen area (both x and y
// are within 0-255 after scroll subtraction) — off-screen sparks are suppressed.
void Sprite_PlaceRupulseSpark_2(int k) {  // 86f6d5
  uint16 x = Sprite_GetX(k) - BG2HOFS_copy2;
  uint16 y = Sprite_GetY(k) - BG2VOFS_copy2;
  if (x & ~0xff || y & ~0xff)
    return;
  repulsespark_x_lo = sprite_x_lo[k];
  repulsespark_y_lo = sprite_y_lo[k];
  repulsespark_timer = 5;
  repulsespark_floor_status = sprite_floor[k];
}

// Wrapper that produces an invalid hitbox (r9_yhi = 0x80) when damage is globally
// disabled (e.g., during an item-get cutscene), so all collision checks fail silently.
// Otherwise delegates to the normal Link_SetupHitBox.
void Link_SetupHitBox_conditional(SpriteHitBox *hb) {  // 86f705
  if (link_disable_sprite_damage)
    hb->r9_yhi = 0x80;
  else
    Link_SetupHitBox(hb);
}

// Builds Link's body hitbox: an 8×8 pixel box centred at (link_x + 4, link_y + 8).
// The +4 / +8 offsets align the box with the centre of Link's 16×16 tile rather
// than the top-left corner. Used as the target for enemy contact-damage checks.
void Link_SetupHitBox(SpriteHitBox *hb) {  // 86f70a
  hb->r3 = hb->r2 = 8;
  uint16 x = link_x_coord + 4;
  hb->r0_xlo = x;
  hb->r8_xhi = x >> 8;
  uint16 y = link_y_coord + 8;
  hb->r1_ylo = y;
  hb->r9_yhi = y >> 8;
}

// Builds the sprite hitbox for sprite k into hb using the precomputed kSpriteHitbox_*
// tables indexed by sprite_flags4[k] & 0x1F (0-31 preset geometries).
// The hitbox position is: (sprite_x + XLo[i], sprite_y - sprite_z + YLo[i])
// with hi-byte carries handled via 16-bit arithmetic.
// If sprite_z is negative (sign8(sprite_z[k])): sets r10_spr_xhi = 0x80 (invalid)
// to suppress damage from sprites that are underground or in a pit.
void Sprite_SetupHitBox(int k, SpriteHitBox *hb) {  // 86f7ef
  if (sign8(sprite_z[k])) {
    hb->r10_spr_xhi = 0x80;
    return;
  }
  int i = sprite_flags4[k] & 0x1f;
  int t, u;

  t = sprite_x_lo[k] + (uint8)kSpriteHitbox_XLo[i];
  hb->r4_spr_xlo = t;
  t = sprite_x_hi[k] + (uint8)kSpriteHitbox_XHi[i] + (t >> 8);
  hb->r10_spr_xhi = t;

  t = sprite_y_lo[k] + (uint8)kSpriteHitbox_YLo[i];
  u = t >> 8;
  t = (t & 0xff) - sprite_z[k];
  hb->r5_spr_ylo = t;
  t = sprite_y_hi[k] - (t < 0);
  hb->r11_spr_yhi = t + u + (uint8)kSpriteHitbox_YHi[i];

  hb->r6_spr_xsize = kSpriteHitbox_XSize[i];
  hb->r7_spr_ysize = kSpriteHitbox_YSize[i];
}

// AABB overlap test for the fully populated SpriteHitBox.
// Returns true if both the Y and X intervals intersect.
// Sentinel check: if either xhi is 0x80 (off-screen / invalid), returns false.
//
// For each axis the test uses 8-bit signed arithmetic:
//   t = sprite_pos - link_pos (signed delta)
//   r15 = t + sprite_size (sprite's far edge relative to link origin)
//   r12 = (sprite_hi - link_hi - borrow) as the high byte of the overlap check
// The combined expression r12 + ((t & 0xff + 0x80) >> 8) produces the high byte
// of (t + 0x80), implementing the overlap test: the intervals overlap iff
// that quantity is 0 AND link_size + sprite_size > r15 (their combined widths
// exceed the gap). Otherwise, r12 != 0 resolves the cross-page case.
bool CheckIfHitBoxesOverlap(SpriteHitBox *hb) {  // 86f836
  int t;
  uint8 r15, r12;

  if (hb->r8_xhi == 0x80 || hb->r10_spr_xhi == 0x80)
    return false;

  t = hb->r5_spr_ylo - hb->r1_ylo;
  r15 = t + hb->r7_spr_ysize;
  r12 = hb->r11_spr_yhi - hb->r9_yhi - (t < 0);
  t = r12 + (((t & 0xff) + 0x80) >> 8);
  if (t & 0xff)
    return (t >= 0x100);
  if ((uint8)(hb->r3 + hb->r7_spr_ysize) < r15)
    return false;

  t = hb->r4_spr_xlo - hb->r0_xlo;
  r15 = t + hb->r6_spr_xsize;
  r12 = hb->r10_spr_xhi - hb->r8_xhi - (t < 0);
  t = r12 + (((t & 0xff) + 0x80) >> 8);
  if (t & 0xff)
    return (t >= 0x100);
  if ((uint8)(hb->r2 + hb->r6_spr_xsize) < r15)
    return false;

  return true;
}

// Adjusts the OAM draw order for sprite k so it renders in front of or behind Link
// based on their relative screen positions. Only affects sprites on the same floor
// and within a 32×72 pixel proximity to Link (|x| < 16, |y+16| < 36 px).
// If the sprite is below Link (below.a set): allocates from region C (behind Link).
// If the sprite is above Link: allocates from region B (in front of Link).
// This ensures items held above Link's head appear behind him, and enemies walking
// in front of Link occlude him correctly.
void Oam_AllocateDeferToPlayer(int k) {  // 86f86c
  if (sprite_floor[k] != link_is_on_lower_level)
    return;
  PairU8 right = Sprite_IsRightOfLink(k);
  if ((uint8)(right.b + 0x10) >= 0x20)
    return;
  PairU8 below = Sprite_IsBelowLink(k);
  if ((uint8)(below.b + 0x20) >= 0x48)
    return;
  uint8 nslots = ((sprite_flags2[k] & 0x1f) + 1) << 2;
  if (below.a)
    Oam_AllocateFromRegionC(nslots);
  else
    Oam_AllocateFromRegionB(nslots);
}

// State 6 entry point — forwards to SpriteDeath_MainEx(k, false) for the death poof.
void SpriteModule_Die(int k) {  // 86f8a2
  SpriteDeath_MainEx(k, false);
}

// Final cleanup for a sprite that has run out of HP or reached its death condition.
// Handles type-specific death events before deactivating the sprite:
//   - Type 0xBE (Vitreous eye): decrements the boss's eye count (sprite_G[0]--).
//   - Type 0xAA (Pikit): if holding a stolen item (sprite_E != 0), drops it via
//     PrepareEnemyDrop with the appropriate item type from kPikitDropItems.
//   - Type 0x45 (crazy guard) in the village when sram_progress == 2: restores music.
//
// Drop item logic (sprite_die_action[k]):
//   0 = prize drop: uses the standard prize table (handled below).
//   1 = small key (0xE4), 3 = 5-rupee (0xD9), other = big key (0xE5).
//
// Prize logic: if sprite_flags5[k] & 0xF is non-zero (prize tier), rolls the random
// prize. item_drop_luck path: first 10 kills guarantee a drop from luck; afterwards
// reverts to random. Random: !(GetRandomNumber() & kPrizeMasks[prize]) determines
// if a drop occurs. On drop, calls ForcePrizeDrop; otherwise deactivates directly.
void Sprite_DoTheDeath(int k) {  // 86f923
  uint8 type = sprite_type[k];
  // This is how Vitreous knows whether to come out of his slime pool
  if (type == 0xBE)
    sprite_G[0]--;

  if (type == 0xaa && sprite_E[k] != 0) {
    uint8 bak = sprite_subtype[k];
    PrepareEnemyDrop(k, kPikitDropItems[sprite_E[k] - 1]);
    sprite_subtype[k] = bak;
    if (bak == 1) {
      sprite_oam_flags[k] = 9;
      sprite_flags3[k] = 0xf0;
    }
    sprite_head_dir[k]++;
    return;
  }

  // Resets the music in the village when the crazy green guards are killed.
  if (type == 0x45 && sram_progress_indicator == 2 && BYTE(overworld_area_index) == 0x18)
    music_control = 7;

  uint8 drop_item = sprite_die_action[k];
  if (drop_item != 0) {
    sprite_subtype[k] = sprite_N[k];
    sprite_N[k] = 255;
    uint8 arg = (drop_item == 1) ? 0xe4 : // small key, big key or rupee
                (drop_item == 3) ? 0xd9 : 0xe5;
    PrepareEnemyDrop(k, arg);
    return;
  }

  uint8 prize = sprite_flags5[k] & 0xf;
  if (prize-- != 0) {
    uint8 luck = item_drop_luck;
    if (luck != 0) {
      if (++luck_kill_counter >= 10)
        item_drop_luck = 0;
      if (luck == 1) {
        ForcePrizeDrop(k, prize, 1);
        return;
      }
    } else {
      if (!(GetRandomNumber() & kPrizeMasks[prize])) {
        ForcePrizeDrop(k, prize, prize);
        return;
      }
    }
  }
  sprite_state[k] = 0;
  SpriteDeath_Func4(k);
}

// Forces a specific prize tier drop from the circular rotation table.
//   prize: tier index (0-6), selects a column in the 7×8 kPrizeItems matrix.
//   slot : which rotation counter to use (prizes_arr1[slot] = 0-7, cycles through
//          the 8 items in that tier). After reading, increments the counter mod 8.
// The final item type is kPrizeItems[prize * 8 | rotation], then calls PrepareEnemyDrop.
void ForcePrizeDrop(int k, uint8 prize, uint8 slot) {  // 86f9bc
  prize = prize * 8 | prizes_arr1[slot];
  prizes_arr1[slot] = (prizes_arr1[slot] + 1) & 7;
  PrepareEnemyDrop(k, kPrizeItems[prize]);
}

// Transmutes sprite k from enemy to a pickup item by overwriting its type with item
// and reloading all properties from the init tables. Special pre-init:
//   0xE5 (big key): calls SpritePrep_BigKey_load_graphics for the dungeon-specific gfx.
//   0xE4 (small key): calls SpritePrep_KeySetItemDrop for small-key-specific setup.
// After init: sets sprite_ignore_projectile++ (brief immunity so the item won't be
// immediately absorbed), reads the upward launch velocity from kPrizeZ (high nibble =
// z_vel, low nibble = X offset for spread), preserves the old Z, sets delay_aux4 = 21
// (post-spawn immunity window), marks stunned = 255 (permanently available), then
// calls SpriteDeath_Func4 to set the death flag and increment kill counter.
void PrepareEnemyDrop(int k, uint8 item) {  // 86f9d1
  sprite_type[k] = item;
  if (item == 0xe5)
    SpritePrep_BigKey_load_graphics(k);
  else if (item == 0xe4)
    SpritePrep_KeySetItemDrop(k);

  sprite_state[k] = 9;
  uint8 zbak = sprite_z[k];
  SpritePrep_LoadProperties(k);
  sprite_ignore_projectile[k]++;

  uint8 pz = kPrizeZ[sprite_type[k] - 0xd8];
  sprite_z_vel[k] = pz & 0xf0;
  Sprite_SetX(k, Sprite_GetX(k) + (pz & 0xf));
  sprite_z[k] = zbak;
  sprite_delay_aux4[k] = 21;
  sprite_stunned[k] = 255;
  SpriteDeath_Func4(k);
}

// Post-death bookkeeping: sets the dungeon room's "dead" flag for this slot so
// the sprite does not respawn, increments the global kill counter.
// Special cases:
//   - Type 0xA2 (Agahnim) when screen is clear: spawns the Triforce falling prize
//     (ancilla type 0x29 at slot 4) as the Agahnim win condition reward.
//   - Type 0x40 (Evil Barrier / ball of energy): enters a special "scatter into debris"
//     poof by transitioning back to state 9 with graphics 4 and calling SpriteDeath_MainEx
//     in second_entry mode to trigger the ball scatter directly.
void SpriteDeath_Func4(int k) {  // 86fa25
  if (sprite_type[k] == 0xa2 && Sprite_CheckIfScreenIsClear())
    Ancilla_SpawnFallingPrize(4);
  Sprite_ManuallySetDeathFlagUW(k);
  num_sprites_killed++;
  if (sprite_type[k] == 0x40) {
    // evil barrier
    sprite_state[k] = 9;
    sprite_graphics[k] = 4;
    SpriteDeath_MainEx(k, true);
  }
}

// Draws the four-tile death poof overlay used by SpriteDeath_MainEx (state 6).
// On a two-layer collision room (dung_hdr_collision == 4): forces priority to 0x30
// (foreground) so the poof renders above both BG layers.
// Selects 4 OAM entries from the kPerishOverlay_* tables. The frame index is:
//   i = ((delay_main & 0x1C) ^ 0x1C) + 3 — counts DOWN as delay increases, so
//   the poof starts at the outermost ring (i=3) and collapses inward as it fades.
// r12 = (flags3 & 0x20) >> 3: if bit 5 of flags3 is set (small sprite), shifts the
// poof 4 pixels to centre it on a smaller sprite tile.
// Entries with kPerishOverlay_Char[i] == 0 are skipped (no tile for this frame).
// Calls Sprite_CorrectOamEntries(k, 3, 0) to fix Y clipping for 8×8 tiles.
void SpriteDeath_DrawPoof(int k) {  // 86fb2a
  if (dung_hdr_collision == 4)
    sprite_obj_prio[k] = 0x30;
  PrepOamCoordsRet info;
  if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
    return;
  OamEnt *oam = GetOamCurPtr();
  uint8 r12 = (sprite_flags3[k] & 0x20) >> 3;
  int i = ((sprite_delay_main[k] & 0x1c) ^ 0x1c) + 3, n = 3;
  do {
    if (kPerishOverlay_Char[i]) {
      oam->charnum = kPerishOverlay_Char[i];
      oam->y = HIBYTE(dungmap_var7) - r12 + kPerishOverlay_Y[i];
      oam->x = BYTE(dungmap_var7) - r12 + kPerishOverlay_X[i];
      oam->flags = (info.flags & 0x30) | kPerishOverlay_Flags[i];
    }
  } while (oam++, i--, --n >= 0);
  Sprite_CorrectOamEntries(k, 3, 0);
}

// State 5 handler — sprite falling into a pit (second phase / humanoid tumble).
// When delay == 0: deactivates (state = 0) and marks the slot dead in dungeon flags.
//
// High delay (>= 0x40): "warning" phase — sprite is near the pit but still active.
//   If oam_flags != 5 (not yet tumbling): runs SpriteActive_Main (enemy still moves),
//     draws a distress indicator (Sprite_DrawDistress_custom) above the sprite,
//     plays SFX 0x31 every 8 frames. Falls through to delay = 63 once oam_flags == 5.
//   Resets delay to 63 to start the actual fall animation.
//
// Low delay (< 0x40): fall animation phase.
//   delay 61: plays the fall start SFX 0x20.
//   j = delay >> 1 (0-31 frame index).
//   Helma/Beetle types (0x26, 0x13): uses kSpriteFall_Tab2 for graphics and
//     SpriteDraw_FallingHelmaBeetle.
//   All others: looks up kSpriteFall_Tab1[j]; if < 12, adds a facing offset from
//     kSpriteFall_Tab4[facing] for directional spin. Draws with SpriteDraw_FallingHumanoid.
//   Horizontal drift: every kSpriteFall_Tab3[delay >> 3] frames, checks the floor tile;
//     if not over a pit, zeroes recoil velocity; then applies recoil / 4 to actual
//     velocity and calls Sprite_MoveXY for a slight tumble drift.
void SpriteModule_Fall2(int k) {  // 86fbea
  uint8 delay = sprite_delay_main[k];
  if (!delay) {
    sprite_state[k] = 0;
    Sprite_ManuallySetDeathFlagUW(k);
    return;
  }

  if (delay >= 0x40) {
    if (sprite_oam_flags[k] != 5) {
      if (!(delay & 7 | submodule_index | flag_unk1))
        SpriteSfx_QueueSfx3WithPan(k, 0x31);
      SpriteActive_Main(k);
      PrepOamCoordsRet info;
      if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
        return;
      Sprite_DrawDistress_custom(info.x, info.y - 8, delay + 20);
      return;
    }
    sprite_delay_main[k] = delay = 63;
  }

  if (delay == 61)
    SpriteSfx_QueueSfx2WithPan(k, 0x20);

  int j = delay >> 1;

  if (sprite_type[k] == 0x26 || sprite_type[k] == 0x13) {
    sprite_graphics[k] = kSpriteFall_Tab2[j];
    SpriteDraw_FallingHelmaBeetle(k);
  } else {
    uint8 t = kSpriteFall_Tab1[j];
    if (t < 12)
      t += kSpriteFall_Tab4[sprite_D[k]];
    sprite_graphics[k] = t;
    SpriteDraw_FallingHumanoid(k);
  }
  if (frame_counter & kSpriteFall_Tab3[sprite_delay_main[k] >> 3] | submodule_index)
    return;
  Sprite_CheckTileProperty(k, 0x68);
  if (sprite_tiletype != 0x20) {
    sprite_y_recoil[k] = 0;
    sprite_x_recoil[k] = 0;
  }
  sprite_y_vel[k] = (int8)sprite_y_recoil[k] >> 2;
  sprite_x_vel[k] = (int8)sprite_x_recoil[k] >> 2;
  Sprite_MoveXY(k);
}

// Draws a single OAM entry for a falling Helma or Beetle sprite.
// Indexes kSpriteDrawFall0Data by sprite_graphics[k] for Helma (type 0x26);
// adds +6 entries for Beetle (type 0x13, which uses the second half of the table).
void SpriteDraw_FallingHelmaBeetle(int k) {  // 86fd17
  PrepOamCoordsRet info;
  const DrawMultipleData *src = kSpriteDrawFall0Data + sprite_graphics[k];
  if (sprite_type[k] == 0x13)
    src += 6;
  Sprite_DrawMultiple(k, src, 1, &info);
}

// Draws 1 or 4 OAM tiles for a falling humanoid enemy (soldiers, guards, etc.).
// sprite_graphics[k] is a frame index 0-23 set by the fall animation.
//   - Frames 0,4,8 (q < 12 and q & 3 == 0): draws 4 tiles (n = 3) for the wide
//     tumbling pose using kSpriteDrawFall1_* tables at index q*4 + n.
//   - All other frames: draws a single tile (n = 0).
// Sprite_CorrectOamEntries(k, nn, 0xFF) fixes Y and extended OAM for 16×16 size.
void SpriteDraw_FallingHumanoid(int k) {  // 86fe5b
  PrepOamCoordsRet info;
  if (Sprite_PrepOamCoordOrDoubleRet(k, &info))
    return;

  int q = sprite_graphics[k];
  OamEnt *oam = GetOamCurPtr();
  int n = (q < 12 && (q & 3) == 0) ? 3 : 0, nn = n;
  do {
    int i = q * 4 + n;
    SetOamPlain(oam, info.x + kSpriteDrawFall1_X[i], info.y + kSpriteDrawFall1_Y[i],
                 kSpriteDrawFall1_Char[i], info.flags ^ kSpriteDrawFall1_Flags[i], kSpriteDrawFall1_Ext[i]);
  } while (oam++, --n >= 0);
  Sprite_CorrectOamEntries(k, nn, 0xff);
}

// Corrects n+1 OAM entries starting at the current OAM pointer for proper clipping.
// Converts the stored (scrollx, scrolly) screen coordinates back to world-relative
// values by using the sprite's current world X/Y as a reference. For each entry:
//   x = sprite_world_x + signed(entry->x - scroll_x_byte) — recovers world pixel X.
//   Recalculates the extended OAM size bit:
//     islarge == 0xFF: preserves existing size bit (from entry); otherwise forces islarge.
//   Sets the X-overflow bit in ext based on screen X >= 256.
//   Suppresses off-screen Y entries by setting oam->y = 0xF0 when the tile would
//   appear above or below the visible screen area.
void Sprite_CorrectOamEntries(int k, int n, uint8 islarge) {  // 86febc
  OamEnt *oam = GetOamCurPtr();
  uint8 *extp = &g_ram[oam_ext_cur_ptr];
  uint16 spr_x = Sprite_GetX(k);
  uint16 spr_y = Sprite_GetY(k);
  uint8 scrollx = spr_x - BG2HOFS_copy2;
  uint8 scrolly = spr_y - BG2VOFS_copy2;
  do {
    uint16 x = spr_x + (int8)(oam->x - scrollx);
    uint16 y = spr_y + (int8)(oam->y - scrolly);
    uint8 ext = sign8(islarge) ? (*extp & 2) : islarge;
    *extp = ext + ((uint16)(x - BG2HOFS_copy2) >= 0x100);
    if ((uint16)(y + 0x10 - BG2VOFS_copy2) >= 0x100)
      oam->y = 0xf0;
  } while (oam++, extp++, --n >= 0);
}

// Drives the sprite's recoil (knockback) animation when sprite_F[k] is set.
// sprite_F[k] is a countdown timer (set by Sprite_GiveDamage to 11-20 frames).
// Returns true to signal the caller to skip normal AI this frame (sprite is in recoil).
// Returns false immediately if sprite_F == 0 (not recoiling).
//
// Recoil mechanics:
//   - High bit (0x80) set in sprite_F: special "persistent recoil" mode (value 144);
//     this path is entered when the recoil velocity is large (|recoil_x/y| >= 0x20)
//     and prevents the sprite from escaping.
//   - Decrements sprite_F each frame; if it reaches 0 AND the recoil velocity is
//     still large, resets to 144 (creates a sustained push-back oscillation).
//   - Only applies movement on frames where (frame_counter & mask) == 0, where mask
//     comes from kSprite2_ReturnIfRecoiling_Masks[F>>2] — faster recoil at the start
//     (mask = 0) and slower later (mask = 3 = every 4 frames).
//   - Temporarily overwrites x/y_vel with the stored recoil velocities, checks tile
//     collision; if a wall is hit, zeroes the recoil component for that axis.
//   - If not wall-blocked (or bump_damage high bit set), calls Sprite_MoveXY.
//   - Restores original x/y_vel after movement.
//   - Returns false for Agahnim (type 0x7A) specifically — he is never "locked out"
//     of his AI by recoil, allowing phase-transition logic to run.
bool Sprite_ReturnIfRecoiling(int k) {  // 86ff78
  if (!sprite_F[k])
    return false;
  if (!(sprite_F[k] & 0x7f)) {
    sprite_F[k] = 0;
    return false;
  }
  uint8 yvbak = sprite_y_vel[k];
  uint8 xvbak = sprite_x_vel[k];
  if (!--sprite_F[k] && ((uint8)(sprite_x_recoil[k] + 0x20) >= 0x40 || (uint8)(sprite_y_recoil[k] + 0x20) >= 0x40))
    sprite_F[k] = 144;

  int i = sprite_F[k];
  uint8 t;
  if (!sign8(i) && !(frame_counter & kSprite2_ReturnIfRecoiling_Masks[i>>2])) {
    sprite_y_vel[k] = sprite_y_recoil[k];
    sprite_x_vel[k] = sprite_x_recoil[k];
    if (!sign8(sprite_bump_damage[k]) && (t = (Sprite_CheckTileCollision(k) & 0xf))) {
      if (t < 4)
        sprite_x_recoil[k] = sprite_x_vel[k] = 0;
      else
        sprite_y_recoil[k] = sprite_y_vel[k] = 0;
    } else {
      Sprite_MoveXY(k);
    }
  }
  sprite_y_vel[k] = yvbak;
  sprite_x_vel[k] = xvbak;
  return sprite_type[k] != 0x7a;
}

// Returns true if Link is too busy to receive a solicited (Y-button) dialogue:
//   - link_auxiliary_state set (mid-action: ledge hang, fall, etc.).
//   - link_pose_for_item set (mid item-get animation).
//   - link_state_bits bit 7 set (submerged / other blocking state).
//   - An active "message" ancilla (type 0x27) in slots 0-4 (message already up).
bool Sprite_CheckIfLinkIsBusy() {  // 87f4d0
  if (link_auxiliary_state | link_pose_for_item | (link_state_bits & 0x80))
    return true;
  for (int i = 4; i >= 0; i--) {
    if (ancilla_type[i] == 0x27)
      return true;
  }
  return false;
}

// Writes a SpriteSpawnInfo's X/Y/Z position data into sprite slot k's arrays.
// Used after Sprite_SpawnDynamically to copy the spawn-point coordinates
// (originally read from the parent sprite) into the new child slot.
void Sprite_SetSpawnedCoordinates(int k, SpriteSpawnInfo *info) {  // 89ae64
  sprite_x_lo[k] = info->r0_x;
  sprite_x_hi[k] = info->r0_x >> 8;
  sprite_y_lo[k] = info->r2_y;
  sprite_y_hi[k] = info->r2_y >> 8;
  sprite_z[k] = info->r4_z;
}

// Returns true when no active sprite is currently visible on screen AND all relevant
// overlords have cleared. Skips sprites with sprite_flags4 bit 6 set (non-blocking
// invisible sprites such as triggers or doors). Used to detect "room cleared" state
// for boss victories and post-fight cutscenes.
bool Sprite_CheckIfScreenIsClear() {  // 89af32
  for (int i = 15; i >= 0; i--) {
    if (sprite_state[i] && !(sprite_flags4[i] & 0x40)) {
      uint16 x = Sprite_GetX(i) - BG2HOFS_copy2;
      uint16 y = Sprite_GetY(i) - BG2VOFS_copy2;
      if (x < 256 && y < 256)
        return false;
    }
  }
  return Sprite_CheckIfOverlordsClear();
}

// Returns true when no active sprite remains in the room (regardless of screen position)
// AND all relevant overlords have cleared. Used for dungeon room-completion checks
// (e.g., opening locked doors when all enemies are dead).
bool Sprite_CheckIfRoomIsClear() {  // 89af61
  for (int i = 15; i >= 0; i--) {
    if (sprite_state[i] && !(sprite_flags4[i] & 0x40))
      return false;
  }
  return Sprite_CheckIfOverlordsClear();
}

// Returns true if no overlord of type 0x14 (Curtain / Agahnim barrier) or
// 0x18 (electric barrier) is active. These two overlords must be gone before
// a room can be considered "cleared" — they keep enemies alive until dispelled.
bool Sprite_CheckIfOverlordsClear() {  // 89af76
  for (int i = 7; i >= 0; i--) {
    if (overlord_type[i] == 0x14 || overlord_type[i] == 0x18)
      return false;
  }
  return true;
}

// Spawns the Magic Mirror warp portal sprite (type 0x6C) at the mirror tile position.
// Kills any pre-existing portal first (clears all slots with type 0x6C).
// Reads the spawn point from bird_travel_x/y_lo/hi[15] — the last entry in the
// bird-travel table, which the mirror-warp code writes with the link_x/y at the
// moment the mirror was used. The portal Y is offset by +8 to centre it vertically.
// Marks the portal on floor 0 and sets ignore_projectile = 1 (cannot be hit).
void Sprite_InitializeMirrorPortal() {  // 89af89
  for (int k = 15; k >= 0; k--) {
    if (sprite_state[k] && sprite_type[k] == 0x6c)
      sprite_state[k] = 0;
  }

  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(0xff, 0x6c, &info);
  if (j < 0)
    j = 0;

  Sprite_SetX(j, bird_travel_x_hi[15] << 8 | bird_travel_x_lo[15]);
  Sprite_SetY(j, (bird_travel_y_hi[15] << 8 | bird_travel_y_lo[15]) + 8);

  sprite_floor[j] = 0;
  sprite_ignore_projectile[j] = 1;
}

// Cleans up sprite slots when transitioning to a new overworld area:
//   - Slots in state 10 (carried): if type is not throwable scenery (0xEC) or fish
//     (0xD2), release Link's carry state and deactivate the slot.
//   - All other active slots: deactivate any sprite that doesn't belong to the
//     current area (sprite_room[k] != current area index), except the mirror portal
//     (type 0x6C) which persists across area transitions.
//   - Overlord slots: clears any overlord not spawned in the current area.
void Sprite_InitializeSlots() {  // 89afd6
  for (int k = 15; k >= 0; k--) {
    uint8 st = sprite_state[k], ty = sprite_type[k];
    if (st != 0) {
      if (st == 10) {
        if (ty != 0xec && ty != 0xd2) {
          link_picking_throw_state = 0;
          link_state_bits = 0;
          sprite_state[k] = 0;
        }
      } else {
        if (ty != 0x6c && sprite_room[k] != BYTE(overworld_area_index))
          sprite_state[k] = 0;
      }
    }
  }
  for (int k = 7; k >= 0; k--) {
    if (overlord_type[k] && overlord_spawned_in_area[k] != BYTE(overworld_area_index))
      overlord_type[k] = 0;
  }
}

// Runs the upper 15 garnish slots (15-29) which are drawn above sprites.
// Also calls HandleScreenFlash each frame for the global screen-flash effect
// (used for sword beams, item fanfares, etc.). Upper slots are used for effects
// that should render on top of enemies — e.g., sparkles, boss hit flashes.
void Garnish_ExecuteUpperSlots() {  // 89b08c
  HandleScreenFlash();

  if (garnish_active) {
    for (int i = 29; i >= 15; i--)
      Garnish_ExecuteSingle(i);
  }
}

// Runs the lower 15 garnish slots (0-14) which are drawn below sprites.
// Lower slots are used for ground effects — dust clouds, water trails, shadow sparks.
void Garnish_ExecuteLowerSlots() {  // 89b097
  if (garnish_active) {
    for (int i = 14; i >= 0; i--)
      Garnish_ExecuteSingle(i);
  }
}

// Per-frame dispatch for a single garnish slot k. Sets cur_object_index = k,
// then checks type: 0 = inactive, skip.
// Countdown management: type 5 (sparkle) decrements every frame regardless of
// submodule/pause; all other types only decrement when the game is unpaused.
// When the countdown reaches 0, the slot is deactivated (type = 0) and returns.
// OAM allocation: reserves kGarnish_OamMemSize[type] bytes from region F (sorted
// upper floor), D (sorted lower floor), or A (unsorted) depending on settings.
// Dispatch: kGarnish_Funcs[type - 1](k) — a 22-entry function pointer table that
// routes to the specific visual effect handler (e.g., sparkle trail, dust cloud,
// Ganon bat flame, Trinexx ice breath, Arrghus splash, cannon smoke, etc.).
void Garnish_ExecuteSingle(int k) {  // 89b0b6
  cur_object_index = k;
  uint8 type = garnish_type[k];
  if (type == 0)
    return;
  if ((type == 5 || (submodule_index | flag_unk1) == 0) && garnish_countdown[k] != 0 && --garnish_countdown[k] == 0) {
    garnish_type[k] = 0;
    return;
  }
  uint8 sprsize = kGarnish_OamMemSize[garnish_type[k]];
  if (sort_sprites_setting) {
    if (garnish_floor[k])
      Oam_AllocateFromRegionF(sprsize);
    else
      Oam_AllocateFromRegionD(sprsize);
  } else {
    Oam_AllocateFromRegionA(sprsize);
  }
  kGarnish_Funcs[garnish_type[k] - 1](k);
}

// Garnish type 16 — Arrghus water splash animation.
// Draws 2 OAM tiles per frame selected by (garnish_countdown >> 1) & 6 to produce
// a 4-step expanding ripple. Tiles use decreasing X offsets (wide → narrow) as g
// advances and alternate left/right flip for mirrored splash arms.
void Garnish15_ArrghusSplash(int k) {  // 89b178
  // Paired X offsets: first entry left arm, second right arm for each ripple frame.
  static const int8 kArrghusSplash_X[8] = {-12, 20, -10, 10, -8, 8, -4, 4};
  static const int8 kArrghusSplash_Y[8] = {-4, -4, -2, -2, 0, 0, 0, 0};
  static const uint8 kArrghusSplash_Char[8] = {0xae, 0xae, 0xae, 0xae, 0xae, 0xae, 0xac, 0xac};
  static const uint8 kArrghusSplash_Flags[8] = {0x34, 0x74, 0x34, 0x74, 0x34, 0x74, 0x34, 0x74};
  static const uint8 kArrghusSplash_Ext[8] = {0, 0, 2, 2, 2, 2, 2, 2};

  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  OamEnt *oam = GetOamCurPtr();
  int g = (garnish_countdown[k] >> 1) & 6;
  for (int i = 1; i >= 0; i--, oam++) {
    int j = i + g;
    SetOamPlain(oam, pt.x + kArrghusSplash_X[j], pt.y + kArrghusSplash_Y[j], kArrghusSplash_Char[j], kArrghusSplash_Flags[j], kArrghusSplash_Ext[j]);
  }
}

// Garnish type 20 — gravity-affected debris particle ejected from the Pyramid of Power.
// Integrates sub-pixel X/Y positions each frame using velocity (<<4 = 1/16 px resolution).
// Y velocity gains +3 each frame for gravity, causing a ballistic arc downward.
// Deactivates (type = 0) when the screen-relative X or Y goes out of [0, 248) / [0, 240).
// Draws a single 8×8 tile (char 0x5c) with palette 3; uses (frame_counter << 3) & 0xc0
// for the upper 2 flip bits so the particle rotates over time.
void Garnish13_PyramidDebris(int k) {  // 89b216
  OamEnt *oam = GetOamCurPtr();

  int y = (garnish_y_lo[k] << 8) + garnish_y_subpixel[k] + ((int8)garnish_y_vel[k] << 4);
  garnish_y_subpixel[k] = y;
  garnish_y_lo[k] = y >> 8;

  int x = (garnish_x_lo[k] << 8) + garnish_x_subpixel[k] + ((int8)garnish_x_vel[k] << 4);
  garnish_x_subpixel[k] = x;
  garnish_x_lo[k] = x >> 8;

  garnish_y_vel[k] = garnish_y_vel[k] + 3;
  uint8 t;
  if ((t = garnish_x_lo[k] - BG2HOFS_copy2) >= 248) {
    garnish_type[k] = 0;
    return;
  }
  oam->x = t;
  if ((t = garnish_y_lo[k] - BG2VOFS_copy2) >= 240) {
    garnish_type[k] = 0;
    return;
  }
  oam->y = t;
  oam->charnum = 0x5c;
  oam->flags = (frame_counter << 3) & 0xc0 | 0x34;
  bytewise_extended_oam[oam - oam_buf] = 0;
}

// Garnish type 18 — Ganon bat-flame spark that rises as it withers during the Agahnim 2 fight.
// Each unpaused frame, decrements the garnish world Y by 1 (slow upward drift).
// Draws two horizontally adjacent 8×8 tiles (chars 0xa4/0xa5) at the current position
// with palette 2 (blue-white). Used for the dissipating flames that remain after Ganon
// transforms. The rising drift distinguishes this from the non-withering Ganon bat flame.
void Garnish11_WitheringGanonBatFlame(int k) {  // 89b2b2
  if ((submodule_index | flag_unk1) == 0) {
    Garnish_SetY(k, Garnish_GetY(k) - 1);
  }
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  OamEnt *oam = GetOamCurPtr();
  SetOamPlain(oam + 0, pt.x + 0, pt.y, 0xa4, 0x22, 0);
  SetOamPlain(oam + 1, pt.x + 8, pt.y, 0xa5, 0x22, 0);
}

// Garnish type 17 — active Ganon bat-flame visual with player collision.
// kGanonBatFlame_Idx maps countdown[k]>>3 (0-31) to a frame index (0-6) that selects
// one of 7 tiles: 4 pairs of small alternating tiles (0xac/0xac, 0x66/0x66) for the
// flicker animation, then larger bright tiles (0x8e, 0xa0, 0xa2) for the flash peak.
// At countdown == 8: transitions the slot to type 0x11 (WitheringGanonBatFlame) so it
// fades away instead of abruptly disappearing.
// After drawing, calls Garnish_CheckPlayerCollision to deal 16 damage if Link is in range.
void Garnish10_GanonBatFlame(int k) {  // 89b306
  static const uint8 kGanonBatFlame_Idx[32] = {
    7, 6, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4,
    5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4,
  };
  static const uint8 kGanonBatFlame_Char[7] = {0xac, 0xac, 0x66, 0x66, 0x8e, 0xa0, 0xa2};
  static const uint8 kGanonBatFlame_Flags[7] = {1, 0x41, 1, 0x41, 0, 0, 0};

  if (garnish_countdown[k] == 8)
    garnish_type[k] = 0x11;
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = kGanonBatFlame_Idx[garnish_countdown[k] >> 3];
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, kGanonBatFlame_Char[j], kGanonBatFlame_Flags[j] | 0x22, 2);
  Garnish_CheckPlayerCollision(k, pt.x, pt.y);
}

// Garnish type 13 — Trinexx ice-head breath blast with tile-freeze effect.
// At countdown == 0x50 (exactly once, only when unpaused): calls
// Dungeon_UpdateTileMapWithCommonTile at the garnish's position to replace the tile
// directly above (y-16) with tile type 18 (frozen/ice tile), permanently freezing
// that floor tile for the battle.
// Drawing: kTrinexxIce_Char[countdown>>4] selects between three widening ice sprites
// (0xe8 big → 0xe6 medium → 0xe4 small) as the blast fades, giving a shrinking effect.
// kTrinexxIce_Flags[(countdown>>2)&3] applies an oscillating H/V-flip to simulate
// a crackling frost animation. Priority flags | 0x35 (layer 2, high priority).
void Garnish0C_TrinexxIceBreath(int k) {  // 89b34f
  static const uint8 kTrinexxIce_Char[12] = {0xe8, 0xe8, 0xe6, 0xe6, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4, 0xe4};
  static const uint8 kTrinexxIce_Flags[4] = {0, 0x40, 0xc0, 0x80};

  if (garnish_countdown[k] == 0x50 && (submodule_index | flag_unk1) == 0) {
    Dungeon_UpdateTileMapWithCommonTile(Garnish_GetX(k), Garnish_GetY(k) - 16, 18);
  }
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, kTrinexxIce_Char[garnish_countdown[k] >> 4],
               kTrinexxIce_Flags[(garnish_countdown[k] >> 2) & 3] | 0x35, 2);
}

// Garnish type 21 — dash-dust puff spawned when the Kakariko kid (follower) runs.
// Thin wrapper around Garnish_DustCommon with dust variant 2 (small puff tile set).
void Garnish14_KakKidDashDust(int k) {  // 89b3bc
  Garnish_DustCommon(k, 2);
}

// Garnish type for water splash trail — spawned when a sprite moves through shallow water.
// Thin wrapper around Garnish_DustCommon with dust variant 3 (water ripple tile set).
void Garnish_WaterTrail(int k) {  // 89b3c2
  Garnish_DustCommon(k, 3);
}

// Garnish type 11 — puff of smoke spawned when a cannon fires a cannonball.
// kGarnish_CannonPoof_Char selects between two expanding smoke tiles based on
// countdown>>3: starts with a large poof (0x8a) that shrinks to a wisp (0x86).
// garnish_sprite[k] holds the cannon's facing direction (0-3); kGarnish_CannonPoof_Flags
// picks H/V flip accordingly so the smoke billows away from the barrel.
// Size is forced to 16×16 (ext flag 4 | priority bit 2 in flags).
void Garnish0A_CannonSmoke(int k) {  // 89b3ee
  static const uint8 kGarnish_CannonPoof_Char[2] = { 0x8a, 0x86 };
  static const uint8 kGarnish_CannonPoof_Flags[4] = { 0x20, 0x10, 0x30, 0x30 };
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, kGarnish_CannonPoof_Char[garnish_countdown[k] >> 3], kGarnish_CannonPoof_Flags[j] | 4, 2);
}

// Garnish type 10 — lightning trail spark (Agahnim charge-up / electric floor hazard).
// garnish_sprite[k] selects one of 8 tile variants from kLightningTrail_Char and
// kLightningTrail_Flags to position the spark along the bolt.
// Room-specific tweak: if the current dungeon room is 0x20 (Agahnim 1 arena), all
// char indices are offset by -0x80 to use an alternate palette bank that matches the
// room's darker lightning palette.
// (frame_counter << 1) & 0xe blends animated palette cycling into the OAM flags to
// make the spark pulse rapidly.
// Calls Garnish_CheckPlayerCollision to deal electric damage to Link if he stands on it.
void Garnish09_LightningTrail(int k) {  // 89b429
  static const uint8 kLightningTrail_Char[8] = {0xcc, 0xec, 0xce, 0xee, 0xcc, 0xec, 0xce, 0xee};
  static const uint8 kLightningTrail_Flags[8] = {0x31, 0x31, 0x31, 0x31, 0x71, 0x71, 0x71, 0x71};
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y,
               kLightningTrail_Char[j] - (BYTE(dungeon_room_index2) == 0x20 ? 0x80 : 0),
               (frame_counter << 1) & 0xe | kLightningTrail_Flags[j], 2);
  Garnish_CheckPlayerCollision(k, pt.x, pt.y);
}

// Tests whether Link's screen position overlaps the garnish hitbox at (x, y) and
// deals environmental damage if so. Used by Ganon bat flames and lightning trails.
// Throttling: only runs when (k ^ frame_counter) & 7 == 0 (once per 8 frames, slot-
// staggered) to spread the CPU cost and avoid hitting every frame.
// Also skips if Link is blinking (invincible from recent hit) or damage is disabled.
// Hitbox: 24×28 pixels centered on the garnish tile, checked as screen-space AABB.
// On hit: sets auxiliary state 1 (brief stun), incapacitated_timer = 16, give_damage = 16,
// and inverts both actual velocity components to produce a knockback bounce.
void Garnish_CheckPlayerCollision(int k, int x, int y) {  // 89b459
  if ((k ^ frame_counter) & 7 | countdown_for_blink | link_disable_sprite_damage)
    return;

  if ((uint8)(link_x_coord - BG2HOFS_copy2 - x + 12) < 24 &&
      (uint8)(link_y_coord - BG2VOFS_copy2 - y + 22) < 28) {
    link_auxiliary_state = 1;
    link_incapacitated_timer = 16;
    link_give_damage = 16;
    link_actual_vel_x ^= 255;
    link_actual_vel_y ^= 255;
  }
}

// Garnish type 8 — hit-flash effect for the Babasu (Arrghus popper) enemy.
// kBabusuFlash_Char[countdown>>3] cycles through 4 tiles (0xa8 bright → 0x8a medium →
// 0x86 small × 2) for a shrinking flash animation over the enemy's position.
// Palette 2, drawn as a 16×16 sprite (ext 2) at the garnish screen coordinates.
void Garnish07_BabasuFlash(int k) {  // 89b49e
  static const uint8 kBabusuFlash_Char[4] = {0xa8, 0x8a, 0x86, 0x86};
  static const uint8 kBabusuFlash_Flags[4] = {0x2d, 0x2c, 0x2c, 0x2c};
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = garnish_countdown[k] >> 3;
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, kBabusuFlash_Char[j], kBabusuFlash_Flags[j], 2);
}

// Garnish type 9 — condensation/nebule trail left by a Kholdstare snowflake.
// kGarnish_Nebule_Char[countdown>>2] steps through 3 ice-crystal tiles (0x9c → 0x9d → 0x8d)
// as the trail fades, with a small (-1, -1) pixel offset on the first two frames.
// The OAM flags are derived from the parent sprite slot stored in garnish_sprite[k]:
// combines oam_flags | obj_prio, but clears bit 0 (priority mode) so the trail renders
// at a consistent depth behind foreground tiles.
void Garnish08_KholdstareTrail(int k) {  // 89b4c6
  static const int8 kGarnish_Nebule_XY[3] = { -1, -1, 0 };
  static const uint8 kGarnish_Nebule_Char[3] = { 0x9c, 0x9d, 0x8d };

  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int i = garnish_countdown[k] >> 2;
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), pt.x + kGarnish_Nebule_XY[i], pt.y + kGarnish_Nebule_XY[i], kGarnish_Nebule_Char[i],
               (sprite_oam_flags[j] | sprite_obj_prio[j]) & ~1, 0);
}

// Garnish type 7 — afterimage trail tile for the Zoro (phantom bat duplicate).
// Draws a single 8×8 tile (char 0x75, the bat silhouette) at the garnish position
// using the parent sprite's OAM flags and priority, making the trail look like
// a fading copy of the original Zoro at its previous screen coordinates.
void Garnish06_ZoroTrail(int k) {  // 89b4fb
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, 0x75, sprite_oam_flags[j] | sprite_obj_prio[j], 0);
}

// Garnish type 19 — standard sparkle effect (used for glowing items, boss intros, etc.).
// Delegates to Garnish_SparkleCommon with variant 2 (larger multi-tile sparkle ring).
void Garnish12_Sparkle(int k) {  // 89b520
  Garnish_SparkleCommon(k, 2);
}

// Simple single-tile sparkle garnish (used for smaller decorative glints).
// Delegates to Garnish_SparkleCommon with variant 3 (smallest sparkle; 1 tile).
void Garnish_SimpleSparkle(int k) {  // 89b526
  Garnish_SparkleCommon(k, 3);
}

// Garnish type 15 — Trinexx fire-head breath blast (lava bubble/fire tile sequence).
// kTrinexxLavaBubble_Char[countdown>>3] cycles through 4 tiles: 0x83 (large fireball),
// 0xc7 (mid flash), 0x80 (diffuse glow), 0x9d (final ember), producing a dissipating flame.
// OAM flags inherit the parent sprite's flags | obj_prio (palette/priority match)
// with the lower nibble forced to 0xe for fire palette row, ext = 0 (8×8 tile).
void Garnish0E_TrinexxFireBreath(int k) {  // 89b55d
  static const uint8 kTrinexxLavaBubble_Char[4] = {0x83, 0xc7, 0x80, 0x9d};
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, kTrinexxLavaBubble_Char[garnish_countdown[k] >> 3],
               (sprite_oam_flags[j] | sprite_obj_prio[j]) & 0xf0 | 0xe, 0);
}

// Garnish type 16 — laser-beam trail segment for Blind the Thief's eye-beam attack.
// garnish_oam_flags[k] encodes the beam segment index (7–10); subtracting 7 gives
// a 0–3 index into kBlindLaserTrail_Char, selecting one of four beam-segment tiles
// (0x61, 0x71, 0x70, 0x60) to form the beam's 4-part visual.
// Flags inherit the parent sprite's palette | priority so the beam matches the eye color.
void Garnish0F_BlindLaserTrail(int k) {  // 89b591
  static const uint8 kBlindLaserTrail_Char[4] = {0x61, 0x71, 0x70, 0x60};
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, kBlindLaserTrail_Char[garnish_oam_flags[k] - 7], sprite_oam_flags[j] | sprite_obj_prio[j], 0);
}

// Garnish type 5 — laser-beam segment for Agahnim's energy ball and similar straight beams.
// garnish_oam_flags[k] selects 0 or 1, choosing between two beam tiles: 0xd2 (bright core)
// or 0xf3 (dimmer trail) to assemble the beam's two visual segments.
// Always drawn with OAM flags 0x25 (palette 5, sprite layer 2, large tile).
void Garnish04_LaserTrail(int k) {  // 89b5bb
  static const uint8 kLaserBeamTrail_Char[2] = {0xd2, 0xf3};
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, kLaserBeamTrail_Char[garnish_oam_flags[k]], 0x25, 0);
}

// Converts the garnish's world position to screen-relative coordinates and tests
// whether the slot is on-screen. Returns true (caller should return early) if
// the garnish is off-screen in either axis (screen-relative x/y >= 256), in which
// case the slot is also deactivated (type = 0) to free it for reuse.
// On success writes the screen-space (x, y-16) into *pt, where the -16 accounts for
// the standard OAM vertical bias used by the sprite subsystem.
bool Garnish_ReturnIfPrepFails(int k, Point16U *pt) {  // 89b5de
  uint16 x = Garnish_GetX(k) - BG2HOFS_copy2;
  uint16 y = Garnish_GetY(k) - BG2VOFS_copy2;

  if (x >= 256 || y >= 256) {
    garnish_type[k] = 0;
    return true;
  }
  pt->x = x;
  pt->y = y - 16;
  return false;
}

// Garnish type 4 — crumbling dungeon floor tile animation.
// At countdown == 0x1e (start of collapse, only when game is unpaused):
//   calls Dungeon_UpdateTileMapWithCommonTile to replace the floor tile at (x, y-16)
//   with tile type 4 (open pit), permanently opening the pit in the room map.
// j = countdown>>3 selects one of 5 animation stages (0-4):
//   Stage 0: 4-pixel offset (kCrumbleTile_XY) centres the tile during shaking.
//   Stages 1-4: no offset, tile shrinks through chars 0xcc → 0xea → 0xca.
// If the screen-relative position is out of bounds the tile is skipped (not deactivated,
// so the pit still opens even if the player scrolled away mid-animation).
void Garnish03_FallingTile(int k) {  // 89b627
  static const uint8 kCrumbleTile_XY[5] = {4, 0, 0, 0, 0};
  static const uint8 kCrumbleTile_Char[5] = {0x80, 0xcc, 0xcc, 0xea, 0xca};
  static const uint8 kCrumbleTile_Flags[5] = {0x30, 0x31, 0x31, 0x31, 0x31};
  static const uint8 kCrumbleTile_Ext[5] = {0, 2, 2, 2, 2};

  int j;
  if ((j = garnish_countdown[k]) == 0x1e && (j = (submodule_index | flag_unk1)) == 0)
    Dungeon_UpdateTileMapWithCommonTile(Garnish_GetX(k), Garnish_GetY(k) - 16, 4);
  j >>= 3;
  uint16 x = Garnish_GetX(k) + kCrumbleTile_XY[j] - BG2HOFS_copy2;
  uint16 y = Garnish_GetY(k) + kCrumbleTile_XY[j] - BG2VOFS_copy2;
  if (x < 256 && y < 256)
    SetOamPlain(GetOamCurPtr(), x, y - 16, kCrumbleTile_Char[j], kCrumbleTile_Flags[j], kCrumbleTile_Ext[j]);
}

// Garnish type 2 — single-tile tail segment for the Fire Snake enemy's body.
// Draws a 16×16 tile (char 0x28, the snake body ring) at the garnish position,
// using the parent sprite's OAM flags and priority (so it matches the head's palette).
// garnish_sprite[k] identifies the parent Fire Snake slot for the flag lookup.
void Garnish01_FireSnakeTail(int k) {  // 89b6c0
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), pt.x, pt.y, 0x28, sprite_oam_flags[j] | sprite_obj_prio[j], 2);
}

// Garnish type 3 — beam-trail tile for Mothula's spinning beam attack.
// Uses the raw garnish_x_lo/y_lo (screen-relative, no PrepFail culling) since the beam
// can be partially off-screen. Draws char 0xaa (beam segment, 16×16) with the parent
// Mothula sprite's palette/priority flags. garnish_sprite[k] is the parent slot index.
void Garnish02_MothulaBeamTrail(int k) {  // 89b6e1
  int j = garnish_sprite[k];
  SetOamPlain(GetOamCurPtr(), garnish_x_lo[k] - BG2HOFS_copy2, garnish_y_lo[k] - BG2VOFS_copy2, 0xaa,
               sprite_oam_flags[j] | sprite_obj_prio[j], 2);
}

// Resets the sprite system for a dungeon room transition and reloads the new room's sprites.
// Steps:
//   1. Caches active sprites to the alt-sprite buffer (for transition animations).
//   2. Clears Link's carry/throw state and bit-flags so he isn't stuck post-transition.
//   3. Deactivates all sprite and ancilla slots via Sprite_DisableAll.
//   4. Resets the sprite collision box dimensions to "max" (0xffff = no constraint).
//   5. Updates dungeon_room_history (4-entry circular log): if the new room isn't already
//      in history, rotates old entries and sets slot 0 to dungeon_room_index2; clears the
//      death-flag word for the oldest evicted room so its sprites can respawn if revisited.
//   6. Loads the new room's sprites via Dungeon_LoadSprites.
void Dungeon_ResetSprites() {  // 89c114
  Dungeon_CacheTransSprites();
  link_picking_throw_state = 0;
  link_state_bits = 0;
  Sprite_DisableAll();
  sprcoll_x_size = sprcoll_y_size = 0xffff;
  int j = FindInWordArray(dungeon_room_history, dungeon_room_index2, 4);
  if (j < 0) {
    uint16 blk = dungeon_room_history[3];
    dungeon_room_history[3] = dungeon_room_history[2];
    dungeon_room_history[2] = dungeon_room_history[1];
    dungeon_room_history[1] = dungeon_room_history[0];
    dungeon_room_history[0] = dungeon_room_index2;
    if (blk != 0xffff)
      sprite_where_in_room[blk] = 0;
  }
  Dungeon_LoadSprites();
}

// Snapshots the active sprite state into the alt-sprite arrays before a room transition.
// Only runs when player_is_indoors (not on the overworld).
// Sets alt_sprites_flag = player_is_indoors to signal ExecuteCachedSprites that there
// are cached sprites to draw during the transition animation.
// For each slot k (0-15):
//   Always copies: state (forced to 0 in alt), type, x, graphics, x_hi, y_lo, y_hi.
//   Skips full copy if: sprite_pause != 0 (frozen), state == 4 (explode), state == 10 (carried).
//   Full copy for active, non-paused, non-explode/carry slots includes: A, head_dir,
//   oam_flags, obj_prio, D, flags2, floor, ai_state (→ alt_spawned_flag), flags3,
//   B, C, E, subtype2, z (→ alt_height_above_shadow), delay_main, I, ignore_projectile.
// Paused/exploding/carried sprites show their visual but do not run AI during the transition.
void Dungeon_CacheTransSprites() {  // 89c176
  if (!player_is_indoors)
    return;
  alt_sprites_flag = player_is_indoors;
  for (int k = 15; k >= 0; k--) {
    alt_sprite_state[k] = 0;
    alt_sprite_type[k] = sprite_type[k];
    alt_sprite_x_lo[k] = sprite_x_lo[k];
    alt_sprite_graphics[k] = sprite_graphics[k];
    alt_sprite_x_hi[k] = sprite_x_hi[k];
    alt_sprite_y_lo[k] = sprite_y_lo[k];
    alt_sprite_y_hi[k] = sprite_y_hi[k];
    if (sprite_pause[k] != 0 || sprite_state[k] == 4 || sprite_state[k] == 10)
      continue;
    alt_sprite_state[k] = sprite_state[k];
    alt_sprite_A[k] = sprite_A[k];
    alt_sprite_head_dir[k] = sprite_head_dir[k];
    alt_sprite_oam_flags[k] = sprite_oam_flags[k];
    alt_sprite_obj_prio[k] = sprite_obj_prio[k];
    alt_sprite_D[k] = sprite_D[k];
    alt_sprite_flags2[k] = sprite_flags2[k];
    alt_sprite_floor[k] = sprite_floor[k];
    alt_sprite_spawned_flag[k] = sprite_ai_state[k];
    alt_sprite_flags3[k] = sprite_flags3[k];
    alt_sprite_B[k] = sprite_B[k];
    alt_sprite_C[k] = sprite_C[k];
    alt_sprite_E[k] = sprite_E[k];
    alt_sprite_subtype2[k] = sprite_subtype2[k];
    alt_sprite_height_above_shadow[k] = sprite_z[k];
    alt_sprite_delay_main[k] = sprite_delay_main[k];
    alt_sprite_I[k] = sprite_I[k];
    alt_sprite_maybe_ignore_projectile[k] = sprite_ignore_projectile[k];
  }
}

// Deactivates all sprite, ancilla, garnish, and overlord slots, and clears a wide set of
// global state flags. Called at room transitions, game-over, and warp events.
// - Sprite slots: sets state = 0 for all active slots, except type 0x6C (Magic Mirror portal)
//   on the overworld — the portal persists across overworld area transitions.
// - Ancilla slots 0-9: cleared entirely (type = 0), removing all projectiles and effects.
// - Global flags reset: flag_is_ancilla_to_pick_up, sprite_limit_instance,
//   byte_7E0B9B/0B88/0B9E, archery_game_arrows_left, garnish_active,
//   activate_bomb_trap_overlord, intro_times_pal_flash, byte_7E0FF8/FFB/FFD/FC6,
//   flag_block_link_menu, is_archer_or_shovel_game.
// - Overlord slots 0-7: cleared.
// - Garnish slots 0-29: cleared.
void Sprite_DisableAll() {  // 89c22f
  for (int k = 15; k >= 0; k--) {
    if (sprite_state[k] && (player_is_indoors || sprite_type[k] != 0x6c))
      sprite_state[k] = 0;
  }
  for (int k = 9; k >= 0; k--)
    ancilla_type[k] = 0;
  flag_is_ancilla_to_pick_up = 0;
  sprite_limit_instance = 0;
  byte_7E0B9B = 0;
  byte_7E0B88 = 0;
  archery_game_arrows_left = 0;
  garnish_active = 0;
  byte_7E0B9E = 0;
  activate_bomb_trap_overlord = 0;
  intro_times_pal_flash = 0;
  byte_7E0FF8 = 0;
  byte_7E0FFB = 0;
  flag_block_link_menu = 0;
  byte_7E0FFD = 0;
  byte_7E0FC6 = 0;
  is_archer_or_shovel_game = 0;
  for (int k = 7; k >= 0; k--)
    overlord_type[k] = 0;
  for (int k = 29; k >= 0; k--)
    garnish_type[k] = 0;
}

// Loads all sprites for the current dungeon room from the kDungeonSprites table.
// kDungeonSpriteOffs[dungeon_room_index2] gives the byte offset into kDungeonSprites for
// this room's sprite list. The first byte of each room's entry is sort_sprites_setting,
// which enables per-floor sorted OAM rendering when non-zero.
// Then reads 3-byte records [y, x, type] until the sentinel byte 0xff is reached.
// For each record: calls Dungeon_LoadSingleSprite(k, src) to initialize the slot and
// increments k (slot index) by 1 after each successful placement.
// Note: Dungeon_LoadSingleSprite may return k-1 for overlord records, so k is always
// set from the return value + 1.
void Dungeon_LoadSprites() {  // 89c290
  const uint8 *src = kDungeonSprites + kDungeonSpriteOffs[dungeon_room_index2];
  byte_7E0FB1 = dungeon_room_index2 >> 3 & 0xfe;
  byte_7E0FB0 = (dungeon_room_index2 & 0xf) << 1;
  sort_sprites_setting = *src++;
  for (int k = 0; *src != 0xff; src += 3)
    k = Dungeon_LoadSingleSprite(k, src) + 1;
}

// Records the death of sprite slot k in the dungeon room's persistent death-flag bitmask.
// Three conditions prevent marking: not in a dungeon (overworld sprites don't persist),
// sprite_defl_bits bit 0 set (respawnable sprite; intended to come back), or sprite_N[k]
// is 0xFF (slot N is invalid — dynamically spawned child with no room-table entry).
// Sets bit (1 << sprite_N[k]) in sprite_where_in_room[dungeon_room_index2] so that
// Dungeon_LoadSingleSprite will skip this slot when the room is revisited.
void Sprite_ManuallySetDeathFlagUW(int k) {  // 89c2f5
  if (!player_is_indoors || sprite_defl_bits[k] & 1 || sign8(sprite_N[k]))
    return;
  sprite_where_in_room[dungeon_room_index2] |= 1 << sprite_N[k];
}

// Initializes a single sprite or overlord slot from a 3-byte dungeon room record.
// src[0] = packed Y: high bit = floor (0/1), bits 6-4 = subtype bits 2-0 from Y.
// src[1] = packed X: bits 7-5 = subtype bits 5-3.
// src[2] = sprite type.
//
// Special cases before normal load:
//   type == 0xE4 (small key) with y == 0xFE/0xFD: sets die_action on the PREVIOUS slot
//     (k-1) to 1 or 2 (prize-drop override), then returns k-1 (no new slot consumed).
//   x >= 0xE0: this is an overlord record, not a sprite — routes to Dungeon_LoadSingleOverlord
//     and returns k-1 (slot index unchanged).
//
// Death-flag check: if kSpriteInit_DeflBits[type] bit 0 is clear AND the room's death-flag
//   has this slot's bit set, the sprite was already killed — skip it (return k unchanged).
//
// Normal load: sets state = 8 (init), extracts floor from y[7], world Y from bits [4:1] of y
// combined with byte_7E0FB1 page offset, similarly for X. Computes subtype from y[6:5] | x[7:5].
// Sets sprite_N[k] = k (room-slot index for death-flag tracking), clears die_action.
int Dungeon_LoadSingleSprite(int k, const uint8 *src) {  // 89c327
  uint8 y = src[0], x = src[1], type = src[2];
  if (type == 0xe4) {
    if (y == 0xfe || y == 0xfd) {
      sprite_die_action[k - 1] = (y == 0xfe) ? 1 : 2;
      return k - 1;
    }
  } else if (x >= 0xe0) {
    Dungeon_LoadSingleOverlord(src);
    return k - 1;
  }
  if (!(kSpriteInit_DeflBits[type] & 1) && (sprite_where_in_room[dungeon_room_index2] & (1 << k)))
    return k;
  sprite_state[k] = 8;
  tmp_counter = y;
  sprite_floor[k] = (y >> 7);
  Sprite_SetY(k, ((y << 4) & 0x1ff) + (byte_7E0FB1 << 8));
  byte_7E0FB6 = x;
  Sprite_SetX(k, ((x << 4) & 0x1ff) + (byte_7E0FB0 << 8));
  sprite_type[k] = type;
  tmp_counter = (tmp_counter & 0x60) >> 2;
  sprite_subtype[k] = tmp_counter | byte_7E0FB6 >> 5;
  sprite_N[k] = k;
  sprite_die_action[k] = 0;
  return k;
}

// Allocates and initializes a single overlord from a 3-byte dungeon room record.
// Calls AllocOverlord() to find a free overlord slot (returns early if none available).
// Decodes the same packed Y/X format as Dungeon_LoadSingleSprite: floor from y[7],
// world Y from bits [4:1] combined with byte_7E0FB1 page, X similarly with byte_7E0FB0.
// Sets overlord_spawned_in_area[k] = overworld_area_index for area-transition filtering.
// Two overlord types get special initial state:
//   Types 10 and 11 (floor/wall switch triggers): gen2 = 160 (arm delay).
//   Type 3 (moving platform): gen2 = 255 (max travel), x_lo -= 8 (aligns to platform center).
void Dungeon_LoadSingleOverlord(const uint8 *src) {  // 89c3e8
  int k = AllocOverlord();
  if (k < 0)
    return;
  uint8 y = src[0], x = src[1], type = src[2];
  overlord_type[k] = type;
  overlord_floor[k] = (y >> 7);
  int t = ((y << 4) & 0x1ff) + (byte_7E0FB1 << 8);
  overlord_y_lo[k] = t;
  overlord_y_hi[k] = t >> 8;
  t = ((x << 4) & 0x1ff) + (byte_7E0FB0 << 8);
  overlord_x_lo[k] = t;
  overlord_x_hi[k] = t >> 8;
  overlord_spawned_in_area[k] = overworld_area_index;
  overlord_gen2[k] = 0;
  overlord_gen1[k] = 0;
  overlord_gen3[k] = 0;
  if (overlord_type[k] == 10 || overlord_type[k] == 11) {
    overlord_gen2[k] = 160;
  } else if (overlord_type[k] == 3) {
    overlord_gen2[k] = 255;
    overlord_x_lo[k] -= 8;
  }
}

// Full sprite system reset: disables all active sprite/ancilla/garnish slots via
// Sprite_DisableAll, then clears all persistent per-room death/load tracking via
// Sprite_ResetAll_noDisable. Used for new-game starts and full area resets.
void Sprite_ResetAll() {  // 89c44e
  Sprite_DisableAll();
  Sprite_ResetAll_noDisable();
}

// Resets all persistent sprite tracking state without touching the active slot arrays.
// Clears: byte_7E0FDD, sprite_alert_flag, byte_7E0FFD/02F0/0FC6, sprite_limit_instance,
// sort_sprites_setting, and super_bomb_indicator_unk2 (to 0xFE, unless follower is type 13).
// Zero-fills: sprite_where_in_room (0x1000 bytes — all room death flags), and
// overworld_sprite_was_loaded (0x200 bytes — "already spawned" bits for overworld tiles).
// Resets dungeon_room_history to all 0xFF (no rooms visited).
void Sprite_ResetAll_noDisable() {  // 89c452
  byte_7E0FDD = 0;
  sprite_alert_flag = 0;
  byte_7E0FFD = 0;
  byte_7E02F0 = 0;
  byte_7E0FC6 = 0;
  sprite_limit_instance = 0;
  sort_sprites_setting = 0;
  if (follower_indicator != 13)
    super_bomb_indicator_unk2 = 0xfe;
  memset(sprite_where_in_room, 0, 0x1000);
  memset(overworld_sprite_was_loaded, 0, 0x200);
  memset(dungeon_room_history, 0xff, 8);
}

// Reloads all overworld sprites after returning from a dungeon or mirror warp.
// Clears all active slots via Sprite_DisableAll, then rebuilds from the overworld
// sprite table for the current area via Sprite_OverworldReloadAll_justLoad.
void Sprite_ReloadAll_Overworld() {  // 89c499
  Sprite_DisableAll();
  Sprite_OverworldReloadAll_justLoad();
}

// Initializes the overworld sprite system without first clearing active slots.
// Resets persistent tracking state (Sprite_ResetAll_noDisable), reads the overworld
// sprite placement table for the current area (Overworld_LoadSprites), then scans
// the visible columns and activates any sprites within proximity range
// (Sprite_ActivateAllProxima). Called separately from Sprite_ReloadAll_Overworld
// when the active sprite arrays are already empty.
void Sprite_OverworldReloadAll_justLoad() {  // 89c49d
  Sprite_ResetAll_noDisable();
  Overworld_LoadSprites();
  Sprite_ActivateAllProxima();
}

// Populates the overworld sprite placement map for the current area.
// Sets up the sprite-collision bounding box (sprcoll_x/y_base, sprcoll_x/y_size) from
// the area index so proximity checks are correctly clamped to this area's world extent.
// Reads the packed overworld sprite list from GetOverworldSpritePtr(overworld_area_index):
//   - Each record is 3 bytes: [tile_row/col_packed, tile_col/offset, type].
//   - type == 0xF4: increments byte_7E0FFD (hidden-secret counter) and skips placement.
//   - Otherwise: computes a 16-bit map key (r5 | r6<<8) from the tile coordinates and
//     writes (type + 1) into sprite_where_in_overworld[key] to register the sprite at
//     that overworld tile for proximity-based activation later.
// Terminates on the 0xFF sentinel byte in the first record byte.
void Overworld_LoadSprites() {  // 89c4ac
  sprcoll_x_base = (overworld_area_index & 7) << 9;
  sprcoll_y_base = ((overworld_area_index & 0x3f) >> 2 & 0xe) << 8;
  sprcoll_x_size = sprcoll_y_size = kOverworldAreaSprcollSizes[BYTE(overworld_area_index)] << 8;
  const uint8 *src = GetOverworldSpritePtr(overworld_area_index);
  uint8 b;

  for (; (b = src[0]) != 0xff; src += 3) {
    if (src[2] == 0xf4) {
      byte_7E0FFD++;
      continue;
    }
    uint8 r2 = (src[0] >> 4) << 2;
    uint8 r6 = (src[1] >> 4) + r2;
    uint8 r5 = src[1] & 0xf | src[0] << 4;
    sprite_where_in_overworld[r5 | r6 << 8] = src[2] + 1;
  }
}

// Extended-aspect preload pass: reproduce the vanilla visible-column sweep first,
// then immediately sweep the north/south rows that widescreen/240-line views can expose
// before Link moves far enough to trigger the alternating per-frame row scan.
static void Sprite_ActivateExtendedProximaGrid() {
  uint16 bak_x = BG2HOFS_copy2;
  uint16 bak_y = BG2VOFS_copy2;
  uint8 bak_h = byte_7E069E[1];
  uint8 bak_v = byte_7E069E[0];
  int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;

  byte_7E069E[1] = 0xff;
  BG2HOFS_copy2 -= xt;
  for (int i = 21 + (xt >> 3); i >= 0; i--) {
    Sprite_ActivateWhenProximal();
    BG2HOFS_copy2 += 16;
  }

  BG2HOFS_copy2 = bak_x;
  byte_7E069E[1] = bak_h;
  byte_7E069E[0] = 1;
  for (int i = 0; i < 8; i++) {
    Sprite_ActivateWhenProximalBig();
    BG2VOFS_copy2 += 16;
  }

  BG2VOFS_copy2 = bak_y;
  byte_7E069E[0] = 0xff;
  for (int i = 0; i < 4; i++) {
    Sprite_ActivateWhenProximalBig();
    BG2VOFS_copy2 -= 16;
  }

  byte_7E069E[0] = bak_v;
  BG2HOFS_copy2 = bak_x;
  BG2VOFS_copy2 = bak_y;
}

// Activates all overworld sprites that fall within the current screen's proximity window.
// Called after Overworld_LoadSprites to seed the sprite slots for the initial view.
// Extended-aspect mode prioritizes the vanilla visible-column sweep, then runs row
// scans for the north/south bands that become visible without a vanilla transition.
// Vanilla mode keeps the original proximity-column sweep:
// Saves and restores BG2HOFS_copy2 and the scroll-edge trigger byte (byte_7E069E[1])
// so the scan does not permanently alter scroll state.
// If kFeatures0_ExtendScreen64 is set (wide-screen mode), starts the scan 0x40 pixels
// earlier and runs 8 extra column steps to cover the wider visible area.
// Steps BG2HOFS_copy2 by 16 each iteration, calling Sprite_ActivateWhenProximal at each
// position to load any sprite registered within proximity of that column.
void Sprite_ActivateAllProxima() {  // 89c55e
  if (enhanced_features0 & (kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes)) {
    Sprite_ActivateExtendedProximaGrid();
    return;
  }

  uint16 bak0 = BG2HOFS_copy2;
  uint8 bak1 = byte_7E069E[1];
  byte_7E069E[1] = 0xff;

  int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;
  BG2HOFS_copy2 -= xt;
  for (int i = 21 + (xt >> 3); i >= 0; i--) {
    Sprite_ActivateWhenProximal();
    BG2HOFS_copy2 += 16;
  }
  byte_7E069E[1] = bak1;
  BG2HOFS_copy2 = bak0;
}

// Per-frame proximity activation dispatcher called from the main game loop.
// When submodule_index != 0 (a screen transition or submodule is running): runs both
//   Sprite_ActivateWhenProximal (vertical column scan) and
//   Sprite_ActivateWhenProximalBig (horizontal row scan) every frame.
// When in normal play (submodule_index == 0): alternates between the two scans based on
//   spr_ranged_based_toggler's parity (even → vertical column, odd → horizontal row),
//   then increments the toggler. This splits the work across two frames to reduce CPU load.
void Sprite_ProximityActivation() {  // 89c58f
  if (submodule_index != 0) {
    Sprite_ActivateWhenProximal();
    Sprite_ActivateWhenProximalBig();
  } else {
    if (!(spr_ranged_based_toggler & 1))
      Sprite_ActivateWhenProximal();
    if (spr_ranged_based_toggler & 1)
      Sprite_ActivateWhenProximalBig();
    spr_ranged_based_toggler++;
  }
}

// Scans a vertical column of overworld tiles at the leading (or trailing) horizontal
// screen edge and activates any sprites found in the proximity map.
// byte_7E069E[1] encodes the scroll direction: negative = left edge (x = scroll - 0x10 - xt),
// positive = right edge (x = scroll + 0x110 + xt). Zero means no horizontal scroll event.
// Sweeps 22 rows (y from scroll_y - 0x30, stepping +16 each iteration),
// calling Sprite_Overworld_ProximityMotivatedLoad(x, y) at each tile position.
void Sprite_ActivateWhenProximal() {  // 89c5bb
  if (byte_7E069E[1]) {
    int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;
    uint16 x = BG2HOFS_copy2 + (sign8(byte_7E069E[1]) ? -0x10 - xt : 0x110 + xt);
    uint16 y = BG2VOFS_copy2 - 0x30;
    for (int i = 21; i >= 0; i--, y += 16)
      Sprite_Overworld_ProximityMotivatedLoad(x, y);
  }
}

// Scans a horizontal row of overworld tiles at the leading (or trailing) vertical
// screen edge and activates any sprites found in the proximity map.
// byte_7E069E[0] encodes the vertical scroll direction: negative = top edge (y = scroll - 0x10),
// positive = bottom edge (y = scroll + 0x110). Zero means no vertical scroll event.
// Sweeps (22 + xt/8) columns (x from scroll_x - 0x30 - xt, stepping +16 each iteration),
// calling Sprite_Overworld_ProximityMotivatedLoad(x, y) at each tile position.
void Sprite_ActivateWhenProximalBig() {  // 89c5fa
  if (byte_7E069E[0]) {
    int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;
    uint16 x = BG2HOFS_copy2 - 0x30 - xt;
    uint16 y = BG2VOFS_copy2 + (sign8(byte_7E069E[0]) ? -0x10 : 0x110);
    for (int i = 21 + (xt >> 3); i >= 0; i--, x += 16)
      Sprite_Overworld_ProximityMotivatedLoad(x, y);
  }
}

// Converts a world (x, y) position to the overworld sprite-map key and activates
// the sprite registered at that tile, if any.
// First checks that (x - sprcoll_x_base) and (y - sprcoll_y_base) are within the
// current area's extent (sprcoll_x/y_size); out-of-bounds positions are silently ignored
// to prevent loading sprites from adjacent areas.
// Computes the 16-bit map key:
//   r1 = coarse tile: (yt>>8)*4 | (xt>>8)   — which 512×512 quadrant
//   r0 = fine tile:   (y & 0xF0) | (x >> 4 & 0xF) — 16-pixel grid within quadrant
// Then calls Overworld_LoadProximaSpriteIfAlive(r1<<8 | r0).
void Sprite_Overworld_ProximityMotivatedLoad(uint16 x, uint16 y) {  // 89c6f5
  uint16 xt = (uint16)(x - sprcoll_x_base);
  uint16 yt = (uint16)(y - sprcoll_y_base);
  if (xt >= sprcoll_x_size || yt >= sprcoll_y_size)
    return;

  uint8 r1 = (yt >> 8) * 4 | (xt >> 8);
  uint8 r0 = y & 0xf0 | x >> 4 & 0xf;
  Overworld_LoadProximaSpriteIfAlive(r1 << 8 | r0);
}

// Looks up overworld tile blk in the proximity map and spawns the sprite or overlord
// registered there, if it has not yet been loaded this session.
// If sprite_where_in_overworld[blk] == 0: nothing registered at this tile, returns.
// Checks and sets a per-tile "already loaded" bit in overworld_sprite_was_loaded[blk>>3]
// using mask (0x80 >> (blk & 7)) to prevent double-spawning on repeated proximity passes.
//
// sprite_to_spawn >= 0xF4: spawns an overlord (AllocOverlord). Decodes the blk key into
//   x_lo, y_lo, x_hi, y_hi and assigns overlord_type = sprite_to_spawn - 0xf3.
// Otherwise: spawns a regular sprite (Overworld_AllocSprite). Sets state = 8 (init),
//   type = sprite_to_spawn - 1, floor = 0, subtype = 0, die_action = 0.
//   Stores blk in sprite_N_word[k] for deactivation tracking when the sprite leaves range.
void Overworld_LoadProximaSpriteIfAlive(uint16 blk) {  // 89c739
  uint8 *p5 = sprite_where_in_overworld + blk;
  uint8 sprite_to_spawn = *p5;
  if (!sprite_to_spawn)
    return;

  uint8 loadedmask = (0x80 >> (blk & 7));
  uint8 *loadedp = &overworld_sprite_was_loaded[blk >> 3];

  if (*loadedp & loadedmask)
    return;

  if (sprite_to_spawn >= 0xf4) {
    // load overlord
    int k = AllocOverlord();
    if (k < 0)
      return;
    *loadedp |= loadedmask;
    overlord_offset_sprite_pos[k] = blk;
    overlord_type[k] = sprite_to_spawn - 0xf3;
    overlord_x_lo[k] = (blk << 4 & 0xf0) + (overlord_type[k] == 1 ? 8 : 0);
    overlord_y_lo[k] = blk & 0xf0;
    overlord_x_hi[k] = (blk >> 8 & 3) + HIBYTE(sprcoll_x_base);
    overlord_y_hi[k] = (blk >> 10) + HIBYTE(sprcoll_y_base);
    overlord_floor[k] = 0;
    overlord_spawned_in_area[k] = overworld_area_index;
    overlord_gen2[k] = 0;
    overlord_gen1[k] = 0;
    overlord_gen3[k] = 0;
  } else {
    // load regular sprite
    int k = Overworld_AllocSprite(sprite_to_spawn);
    if (k < 0)
      return;
    *loadedp |= loadedmask;

    sprite_N_word[k] = blk;
    sprite_type[k] = sprite_to_spawn - 1;
    sprite_state[k] = 8;
    sprite_x_lo[k] = blk << 4 & 0xf0;
    sprite_y_lo[k] = blk & 0xf0;
    sprite_x_hi[k] = (blk >> 8 & 3) + HIBYTE(sprcoll_x_base);
    sprite_y_hi[k] = (blk >> 10) + HIBYTE(sprcoll_y_base);
    sprite_floor[k] = 0;
    sprite_subtype[k] = 0;
    sprite_die_action[k] = 0;
  }
}

// Spawns a type 0xEA (exploding scatter bomb) child sprite from parent slot k.
// Saves the parent type in tmp_counter so it survives across the slot initialization.
// Calls Sprite_SpawnDynamicallyEx with j=14 (search slots 14 down to 0) to avoid
// clobbering the boss slot 15.
// Assigns the spawn coordinates from the parent's position, sets z_vel = 32 (strong upward
// launch), copies parent's floor. spawn slot A: slot 9 gets value 2 (reduced spread),
// all others get 6 (wide explosive spread). Adjusts Y + 3 to centre the explosion.
// Parent-type overrides:
//   0xCE (Geno): shifts Y down by +16 to spawn at the chest, not the head.
//   0xCB (Cannon Ball): forces x_lo = y_lo = 0x78, x_hi/y_hi from link_x/y — spawn at Link.
void SpriteExplode_SpawnEA(int k) {  // 89ee4c
  tmp_counter = sprite_type[k];
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamicallyEx(k, 0xea, &info, 14);
  Sprite_SetSpawnedCoordinates(j, &info);
  sprite_z_vel[j] = 32;
  sprite_floor[j] = link_is_on_lower_level;
  sprite_A[j] = (j == 9) ? 2 : 6;
  Sprite_SetY(j, info.r2_y + 3);
  if (tmp_counter == 0xce) {
    Sprite_SetY(j, info.r2_y + 16);
    return;
  }
  if (tmp_counter == 0xcb) {
    sprite_y_lo[j] = sprite_x_lo[j] = 0x78;
    sprite_x_hi[j] = HIBYTE(link_x_coord);
    sprite_y_hi[j] = HIBYTE(link_y_coord);
  }
}

// Forces all non-Agahnim, non-allied, non-boss sprites (other than the caller) into
// death state. Used when a boss-phase transition requires clearing all minions simultaneously.
// Skips: cur_object_index (the calling boss), inactive slots (state == 0), sprites with
// defl_bits bit 1 set (allied/neutral), and type 0x7A (Agahnim himself).
// For each killed sprite: sets state = 6 (die), delay_main = 15 (death animation duration),
// clears flags3 and flags5 (removes shields/special properties), and sets flags2 = 3
// (marks for prize/death processing).
void Sprite_KillFriends() {  // 89ef56
  for(int j = 15; j >= 0; j--) {
    if (j != cur_object_index && sprite_state[j] && !(sprite_defl_bits[j] & 2) && sprite_type[j] != 0x7a) {
      sprite_state[j] = 6;
      sprite_delay_main[j] = 15;
      sprite_flags3[j] = 0;
      sprite_flags5[j] = 0;
      sprite_flags2[j] = 3;
    }
  }
}

// Garnish type 23 — scattered debris animation when a thrown item (pot, bush, etc.) breaks.
// Each frame draws 4 OAM tiles selected by the countdown phase. The frame index base is
// computed as ((countdown>>2) ^ 7) << 2, which counts 16 phases downward from 28→0.
// garnish_sprite[k] encodes the item category:
//   0: generic tile; uses char 0x4E (small square).
//   >= 0x80: magic item; uses char 0xF2.
//   2 (outdoors): base shifted +0x20 for alternate outdoor tile set.
//   4: base shifted +0x20 for cave/dungeon tile set.
//   3: routes to ScatterDebris_Draw for a 3-tile "explosion scatter" sequence.
// Skips drawing if byte_7E0FC6 >= 3 (too many active debris effects; throttle for performance).
// kScatterDebris_Draw_X/Y/Char/Flags tables (64 entries each) define the 4×16 animation
// frames as per-tile offsets, character indices, and H/V flip bits.
void Garnish16_ThrownItemDebris(int k) {  // 89f0cb
  static const int16 kScatterDebris_Draw_X[64] = {
     0,  8,  0,  8, -2,  9, -1,  9, -4,  9, -1, 10, -6,  9, -1, 12,
    -7,  9, -2, 13, -9,  9, -3, 14, -4, -4,  9, 15, -3, -3, -3,  9,
    -4,  4,  6, 10, -1,  4,  6,  7,  0,  2,  4,  7,  1,  1,  5,  7,
     0, -2,  8,  9, -1, -6,  9, 10, -2, -7, 12, 11, -3, -9,  4,  6,
  };
  static const int8 kScatterDebris_Draw_Y[64] = {
      0,  0,  8,  8,   0, -1, 10, 10,   0, -3, 11,  7,   1, -4, 12,  8,
      1, -4, 13,  9,   2, -4, 16, 10,  14, 14, -4, 11,  16, 16, 16, -1,
      2, -5,  5,  1,   3, -7,  8,  2,   4, -8,  4, 10,  -9,  4,  4, 12,
    -10,  4,  8, 14, -12,  4,  8, 15, -15,  3,  8, 17, -17,  1, 18, 15,
  };
  static const int8 kScatterDebris_Draw_Char[64] = {
    0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58,
    0x48, 0x58, 0x58, 0x58, 0x48, 0x58, 0x58, 0x48, 0x48, 0x48, 0x58, 0x48, 0x48, 0x48, 0x48, 0x48,
    0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59,
    0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59,
  };
  static const uint8 kScatterDebris_Draw_Flags[64] = {
    0x80,    0, 0x80, 0x40, 0x80, 0x40, 0x80,    0,    0, 0xc0,    0, 0x80, 0x80, 0x40, 0x80,    0,
    0x80, 0xc0,    0, 0x80,    0,    0, 0x80,    0, 0x80, 0x80, 0x80, 0x80,    0,    0,    0,    0,
    0x40, 0x40, 0x40,    0, 0x40, 0x40, 0x40,    0, 0x40, 0x40,    0,    0, 0x80,    0, 0x40, 0x40,
    0x40,    0, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,    0,    0, 0x40,    0,    0,    0,
  };
  Point16U pt;
  if (Garnish_ReturnIfPrepFails(k, &pt))
    return;
  uint8 r5 = garnish_oam_flags[k];
  if (byte_7E0FC6 >= 3)
    return;
  if (garnish_sprite[k] == 3) {
    ScatterDebris_Draw(k, pt);
    return;
  }
  OamEnt *oam = GetOamCurPtr();
  tmp_counter = garnish_sprite[k];
  uint8 base = ((garnish_countdown[k] >> 2) ^ 7) << 2;
  if (tmp_counter == 4 || tmp_counter == 2 && !player_is_indoors)
    base += 0x20;
  for (int i = 3; i >= 0; i--, oam++) {
    int j = i + base;
    SetOamHelper1(oam, pt.x + kScatterDebris_Draw_X[j], pt.y + kScatterDebris_Draw_Y[j],
                  (tmp_counter == 0) ? 0x4E : (tmp_counter >= 0x80) ? 0xF2 : kScatterDebris_Draw_Char[j],
                  kScatterDebris_Draw_Flags[j] | r5, 0);
  }
}

// Draws the "explosion scatter" sub-animation for Garnish16 when garnish_sprite[k] == 3
// (breakable wall / large scenery pieces).
// Deactivates the garnish when countdown reaches 16 (half-way), since the scatter
// resolves in the first 16 frames.
// Draws 3 tiles per frame. The frame group index = ((countdown & 0xf) >> 2) * 3 picks
// one of 4 groups of 3 tiles (base indices 0, 3, 6, 9) from kScatterDebris_Draw_X2/Y2/
// Char2/Flags2. Tile chars alternate between 0xe2 (large chunk) and 0xf2 (small shard),
// with H/V flip bits for rotation variety. Palette 2, 8×8 size (ext 0).
void ScatterDebris_Draw(int k, Point16U pt) {  // 89f198
  static const int8 kScatterDebris_Draw_X2[12] = {-8, 8, 16, -5, 8, 15, -1, 7, 11, 1, 3, 8};
  static const int8 kScatterDebris_Draw_Y2[12] = {7, 2, 12, 9, 2, 10, 11, 2, 11, 7, 3, 8};
  static const uint8 kScatterDebris_Draw_Char2[12] = {0xe2, 0xe2, 0xe2, 0xe2, 0xf2, 0xf2, 0xf2, 0xe2, 0xe2, 0xf2, 0xe2, 0xe2};
  static const uint8 kScatterDebris_Draw_Flags2[12] = {0, 0, 0, 0, 0x80, 0x40, 0, 0x80, 0x40, 0, 0, 0};

  if (garnish_countdown[k] == 16)
    garnish_type[k] = 0;

  OamEnt *oam = GetOamCurPtr();
  int base = ((garnish_countdown[k] & 0xf) >> 2) * 3;

  for (int i = 2; i >= 0; i--, oam++) {
    int j = i + base;
    SetOamHelper1(oam, pt.x + kScatterDebris_Draw_X2[j], pt.y + kScatterDebris_Draw_Y2[j],
                  kScatterDebris_Draw_Char2[j], kScatterDebris_Draw_Flags2[j] | 0x22, 0);
  }
}

// Deactivates an overworld sprite that has left proximity range (walked off-screen).
// If the sprite is flagged as permanent (defl_bits bit 6 set) and player_is_indoors is
// true (shouldn't happen but defensive check), bail out without deactivating.
// Clears state, then calculates the overworld sprite-map key from sprite_N_word[k] and
// clears the "loaded" bit in overworld_sprite_was_loaded so the sprite can respawn the
// next time the player scrolls back into range.
// Indoor sprites: sets sprite_N[k] = 0xFF (marks slot as dynamic / no room-table entry).
// Overworld sprites: sets sprite_N_word[k] = 0xFFFF to mark slot as unowned.
// Note: g_ram[0..2] are written with blk data as a side-effect of the address calculation;
// this is a known quirk (comment in original source: "Sprite_PrepOamCoordOrDoubleRet reads this!").
void Sprite_KillSelf(int k) {  // 89f1f8
  if (!(sprite_defl_bits[k] & 0x40) && player_is_indoors)
    return;
  sprite_state[k] = 0;
  uint16 blk = sprite_N_word[k];
  g_ram[0] = blk;  // Sprite_PrepOamCoordOrDoubleRet reads this!
  WORD(g_ram[1]) = (blk >> 3) + 0xef80; // Sprite_PrepOamCoordOrDoubleRet reads this!
  uint8 loadedmask = (0x80 >> (blk & 7));
  uint16 addr = 0xEF80 + (blk >> 3);  // warning: blk may be bad, seen with cannon balls in 2nd dungeon

  uint8 *loadedp = &g_ram[addr + 0x10000];

  if (blk < 0xffff)
    *loadedp &= ~loadedmask;
  if (!player_is_indoors)
    sprite_N_word[k] = 0xffff;
  else
    sprite_N[k] = 0xff;
}

// Initializes all gameplay properties for sprite slot k from the static init tables
// indexed by sprite_type[k]. Called when transitioning a sprite into active state (state 9)
// from state 8 (init) via SpriteActive_Main, and also by PrepareEnemyDrop when re-typing
// a dead sprite to spawn a prize item.
// First zeros all fields via SpritePrep_ResetProperties, then populates:
//   flags2 (movement/interaction flags), health, flags4 (hitbox/AI flags),
//   flags5 (death/prize flags), defl_bits (deflection/respawn flags),
//   bump_damage (contact damage value), flags (state-machine index & misc),
//   room (current dungeon room or overworld area), flags3 (palette/size),
//   oam_flags (lower nibble of flags3 = base OAM attribute bits).
void SpritePrep_LoadProperties(int k) {  // 8db818
  SpritePrep_ResetProperties(k);
  int j = sprite_type[k];
  sprite_flags2[k] = kSpriteInit_Flags2[j];
  sprite_health[k] = kSpriteInit_Health[j];
  sprite_flags4[k] = kSpriteInit_Flags4[j];
  sprite_flags5[k] = kSpriteInit_Flags5[j];
  sprite_defl_bits[k] = kSpriteInit_DeflBits[j];
  sprite_bump_damage[k] = kSpriteInit_BumpDamage[j];
  sprite_flags[k] = kSpriteInit_Flags[j];
  sprite_room[k] = player_is_indoors ? dungeon_room_index2 : overworld_area_index;
  sprite_flags3[k] = kSpriteInit_Flags3[j];
  sprite_oam_flags[k] = kSpriteInit_Flags3[j] & 0xf;
}

// Refreshes only the palette/size fields of sprite slot k from the init table.
// Used when a sprite changes visual form (e.g., Agahnim phase 2, prize transmutation)
// without a full property reset. Updates flags3 (palette row + size flag) and
// oam_flags (lower 4 bits — palette index for OAM attribute byte).
void SpritePrep_LoadPalette(int k) {  // 8db85c
  int f = kSpriteInit_Flags3[sprite_type[k]];
  sprite_flags3[k] = f;
  sprite_oam_flags[k] = f & 15;
}

// Zeros all per-sprite transient state fields in slot k, preparing it for fresh
// initialization by SpritePrep_LoadProperties. Fields cleared include:
//   pause, E (special action), x/y/z velocities, x/y/z sub-pixel accumulators,
//   ai_state, graphics, facing (D), delay timers (main, aux1, aux2, aux4), head_dir,
//   anim_clock, G (general counter), hit_timer, wall-collision flags, z (height),
//   health, recoil timer (F), recoil velocities (x/y), general purpose registers A–C,
//   unk2/subtype2, ignore_projectile, obj_prio, oam_flags, stunned, give_damage,
//   unk3/4/5, unk1, and I (interaction state).
// Does NOT clear: type, state, position (x/y lo/hi), floor, room, N, subtype, die_action,
// or any persistent death/aggro tracking — those are managed by the caller.
void SpritePrep_ResetProperties(int k) {  // 8db871
  sprite_pause[k] = 0;
  sprite_E[k] = 0;
  sprite_x_vel[k] = 0;
  sprite_y_vel[k] = 0;
  sprite_z_vel[k] = 0;
  sprite_x_subpixel[k] = 0;
  sprite_y_subpixel[k] = 0;
  sprite_z_subpos[k] = 0;
  sprite_ai_state[k] = 0;
  sprite_graphics[k] = 0;
  sprite_D[k] = 0;
  sprite_delay_main[k] = 0;
  sprite_delay_aux1[k] = 0;
  sprite_delay_aux2[k] = 0;
  sprite_delay_aux4[k] = 0;
  sprite_head_dir[k] = 0;
  sprite_anim_clock[k] = 0;
  sprite_G[k] = 0;
  sprite_hit_timer[k] = 0;
  sprite_wallcoll[k] = 0;
  sprite_z[k] = 0;
  sprite_health[k] = 0;
  sprite_F[k] = 0;
  sprite_x_recoil[k] = 0;
  sprite_y_recoil[k] = 0;
  sprite_A[k] = 0;
  sprite_B[k] = 0;
  sprite_C[k] = 0;
  sprite_unk2[k] = 0;
  sprite_subtype2[k] = 0;
  sprite_ignore_projectile[k] = 0;
  sprite_obj_prio[k] = 0;
  sprite_oam_flags[k] = 0;
  sprite_stunned[k] = 0;
  sprite_give_damage[k] = 0;
  sprite_unk3[k] = 0;
  sprite_unk4[k] = 0;
  sprite_unk5[k] = 0;
  sprite_unk1[k] = 0;
  sprite_I[k] = 0;
}

// Allocates num bytes from OAM region A (unsorted sprites, region index 0).
uint8 Oam_AllocateFromRegionA(uint8 num) {  // 8dba80
  return Oam_GetBufferPosition(num, 0);
}

// Allocates num bytes from OAM region B (lower-floor sorted sprites, region index 1).
uint8 Oam_AllocateFromRegionB(uint8 num) {  // 8dba84
  return Oam_GetBufferPosition(num, 2);
}

// Allocates num bytes from OAM region C (lower-floor sorted sprites, region index 2).
uint8 Oam_AllocateFromRegionC(uint8 num) {  // 8dba88
  return Oam_GetBufferPosition(num, 4);
}

// Allocates num bytes from OAM region D (lower-floor sorted sprites, region index 3).
uint8 Oam_AllocateFromRegionD(uint8 num) {  // 8dba8c
  return Oam_GetBufferPosition(num, 6);
}

// Allocates num bytes from OAM region E (upper-floor sorted sprites, region index 4).
uint8 Oam_AllocateFromRegionE(uint8 num) {  // 8dba90
  return Oam_GetBufferPosition(num, 8);
}

// Allocates num bytes from OAM region F (upper-floor sorted sprites, region index 5).
uint8 Oam_AllocateFromRegionF(uint8 num) {  // 8dba94
  return Oam_GetBufferPosition(num, 10);
}

// Core OAM region allocator: advances oam_region_base[region] by num bytes and returns
// the resulting OAM buffer pointer (oam_cur_ptr) for use with GetOamCurPtr().
// y is the raw region selector (0/2/4/6/8/10 from the region A–F wrappers); >>1 converts
// it to a 0–5 region index into oam_region_base.
// Overflow handling: if the new base would exceed kOamGetBufferPos_Tab0[region] (the
// region's end boundary), the allocation wraps around using a fallback pointer from
// kOamGetBufferPos_Tab1[region*8 + (oam_alloc_arr1[region]++ & 7)] — a set of 8 overflow
// slots that cycle round-robin to handle more sprites than the region was sized for.
// Sets both oam_cur_ptr (main OAM byte: 0x800 + ptr) and oam_ext_cur_ptr (extended OAM
// byte: 0xa20 + ptr/4) so callers can write both the main and extended OAM fields.
uint8 Oam_GetBufferPosition(uint8 num, uint8 y) {  // 8dbb0a
  y >>= 1;
  uint16 p = oam_region_base[y], pstart = p;
  p += num;
  if (p >= kOamGetBufferPos_Tab0[y]) {
    int j = oam_alloc_arr1[y]++ & 7;
    pstart = kOamGetBufferPos_Tab1[y * 8 + j];
  } else {
    oam_region_base[y] = p;
  }
  oam_ext_cur_ptr = 0xa20 + (pstart >> 2);
  oam_cur_ptr = 0x800 + pstart;
  return oam_cur_ptr;
}

// Cancels any active hookshot drag that would pull a sprite toward Link, then restores
// Link's position to a safe snapshot and triggers the camera/door update.
// Checks ancilla slots 0-4 for a hookshot (type with lower 5 bits == 0); if found,
// clears related_to_hookshot to disconnect the pulled entity. This prevents sprites or
// blocks from continuing to fly toward Link when the drag is forcibly interrupted.
// After clearing drag: copies link_x/y_coord from link_x/y_coord_prev (the pre-drag
// saved position) and calls HandleIndoorCameraAndDoors to re-sync the camera.
void Sprite_NullifyHookshotDrag() {  // 8ff540
  for (int i = 4; i >= 0; i--) {
    if (!(ancilla_type[i] & 0x1f) && related_to_hookshot) {
      related_to_hookshot = 0;
      break;
    }
  }
  link_x_coord_safe_return_hi = link_x_coord >> 8;
  link_y_coord_safe_return_hi = link_y_coord >> 8;
  link_x_coord = link_x_coord_prev;
  link_y_coord = link_y_coord_prev;
  HandleIndoorCameraAndDoors();
}

// Probabilistically overrides the hidden-secret tile type for the current overworld area
// with an alternate reward, providing a mild form of drop variety on revisit.
// Skips 50% of the time (GetRandomNumber() & 1).
// Skips if too many sprites already active (n >= 4 after counting non-portal sprites), or
// if sram_progress_indicator < 2 (early in the game; alternate secrets not yet enabled).
// Selects a slot j from the 16-entry substitution table using overworld_secret_subst_ctr
// (incremented each call), offset by 8 for the dark world. kSecretSubst_Tab0[area & 0x3f]
// is a bitmask of which substitution slots are valid for this area; kSecretSubst_Tab1[j]
// is the bit to test; kSecretSubst_Tab2[j] is the replacement secret type index written
// into dung_secrets_unk1 when the test passes.
void Overworld_SubstituteAlternateSecret() {  // 9afbdb
  static const uint8 kSecretSubst_Tab0[64] = {
    0,  0, 0, 0, 0, 0, 0, 4,  0,  0, 0, 0, 0, 0, 0, 0,
    4,  4, 6, 4, 4, 6, 0, 0, 15, 15, 4, 5, 5, 4, 6, 6,
    15, 15, 4, 5, 5, 7, 6, 6, 31, 31, 4, 7, 7, 4, 6, 6,
    6,  7, 2, 0, 0, 0, 0, 0,  6,  6, 2, 0, 0, 0, 0, 0,
  };
  static const uint8 kSecretSubst_Tab2[16] = { 1, 1, 1, 1, 15, 1, 1, 18, 16, 1, 1, 1, 17, 1, 1, 3 };
  static const uint8 kSecretSubst_Tab1[16] = { 0, 0, 0, 0, 2, 0, 0, 8, 16, 0, 0, 0, 1, 0, 0, 0 };
  if (GetRandomNumber() & 1)
    return;
  int n = 0;
  for (int j = 15; j >= 0; j--) {
    if (sprite_state[j] && sprite_type[j] != 0x6c)
      n++;
  }
  if (n >= 4 || sram_progress_indicator < 2)
    return;
  int j = (overworld_secret_subst_ctr++ & 7) + (is_in_dark_world ? 8 : 0);
  if (!(kSecretSubst_Tab0[BYTE(overworld_area_index) & 0x3f] & kSecretSubst_Tab1[j]))
    BYTE(dung_secrets_unk1) = kSecretSubst_Tab2[j];
}

// Nudges sprite k one pixel per 2 frames in the direction of a conveyor belt tile.
// Skips every other frame (frame_counter & 1) so the effective speed is 0.5 pixels/frame.
// j is the conveyor tile type (0x68-0x6B): subtracting 0x68 gives a 0-3 direction index
// that selects the appropriate X/Y offset from kConveyorAdjustment_X/Y:
//   0 = north (-Y), 1 = south (+Y), 2 = west (-X), 3 = east (+X).
void Sprite_ApplyConveyor(int k, int j) {  // 9d8010
  if (!(frame_counter & 1))
    return;
  static const int8 kConveyorAdjustment_X[] = {0, 0, -1, 1};
  static const int8 kConveyorAdjustment_Y[] = {-1, 1, 0, 0};
  Sprite_SetX(k, Sprite_GetX(k) + kConveyorAdjustment_X[j - 0x68]);
  Sprite_SetY(k, Sprite_GetY(k) + kConveyorAdjustment_Y[j - 0x68]);
}

// Reverses the velocity component that caused a wall collision and counts the bounce.
// Calls Sprite_CheckTileCollision; bits 0-1 of the result indicate left/right wall hits
// (negates x_vel, increments sprite_G[k]); bits 2-3 indicate top/bottom ceiling/floor
// hits (negates y_vel, increments G, returns G as the return value).
// Returns 0 if there was no Y-axis collision (only X collision or no collision at all).
// sprite_G[k] acts as a bounce counter; callers may use it to detect the number of ricochets.
uint8 Sprite_BounceFromTileCollision(int k) {  // 9dc751
  int j = Sprite_CheckTileCollision(k);
  if (j & 3) {
    sprite_x_vel[k] = -sprite_x_vel[k];
    sprite_G[k]++;
  }
  if (j & 12) {
    sprite_y_vel[k] = -sprite_y_vel[k];
    sprite_G[k]++;
    return sprite_G[k]; // wtf
  }
  return 0;
}

// Runs the visual/AI tick for sprites cached in the alt-sprite buffer during room transitions.
// Only active when: player_is_indoors, a room-transition submodule is running (submodule_index
// != 0 and != 14), AND alt_sprites_flag is set (sprites were cached by Dungeon_CacheTransSprites).
// Otherwise clears alt_sprites_flag and returns immediately.
// Iterates all 16 sprite slots and calls UncacheAndExecuteSprite(i) for each with a non-zero
// alt_sprite_state, so that cached sprites continue to animate (draw their OAM tiles)
// during the transition scroll, giving the impression that the previous room's enemies
// remain visible as the screen slides away.
void ExecuteCachedSprites() {  // 9de9da
  if (!player_is_indoors || submodule_index == 0 || submodule_index == 14 || alt_sprites_flag == 0) {
    alt_sprites_flag = 0;
    return;
  }
  for (int i = 15; i >= 0; i--) {
    cur_object_index = i;
    if (alt_sprite_state[i])
      UncacheAndExecuteSprite(i);
  }
}

// Temporarily swaps the live sprite slot k with its cached alt-sprite counterpart,
// runs Sprite_ExecuteSingle (state-machine dispatch), then restores the live data.
// This allows the previous room's sprites to animate their OAM tiles during the transition
// without corrupting the live slot state (health, timers, etc.) for the new room.
// The swap covers 24 fields: state, type, x/y lo/hi, graphics, A, head_dir, oam_flags,
// obj_prio, D, flags2, floor, ai_state, flags3, B, C, E, subtype2, z, delay_main, I,
// and ignore_projectile.
// After executing: if sprite_pause was set during execution, clears alt_sprite_state[k]
// so the cached sprite does not render again (it finished its transition phase).
void UncacheAndExecuteSprite(int k) {  // 9dea00
  uint8 bak0 = sprite_state[k];
  uint8 bak1 = sprite_type[k];
  uint8 bak2 = sprite_x_lo[k];
  uint8 bak3 = sprite_x_hi[k];
  uint8 bak4 = sprite_y_lo[k];
  uint8 bak5 = sprite_y_hi[k];
  uint8 bak6 = sprite_graphics[k];
  uint8 bak7 = sprite_A[k];
  uint8 bak8 = sprite_head_dir[k];
  uint8 bak9 = sprite_oam_flags[k];
  uint8 bak10 = sprite_obj_prio[k];
  uint8 bak11 = sprite_D[k];
  uint8 bak12 = sprite_flags2[k];
  uint8 bak13 = sprite_floor[k];
  uint8 bak14 = sprite_ai_state[k];
  uint8 bak15 = sprite_flags3[k];
  uint8 bak16 = sprite_B[k];
  uint8 bak17 = sprite_C[k];
  uint8 bak18 = sprite_E[k];
  uint8 bak19 = sprite_subtype2[k];
  uint8 bak20 = sprite_z[k];
  uint8 bak21 = sprite_delay_main[k];
  uint8 bak22 = sprite_I[k];
  uint8 bak23 = sprite_ignore_projectile[k];
  sprite_state[k] = alt_sprite_state[k];
  sprite_type[k] = alt_sprite_type[k];
  sprite_x_lo[k] = alt_sprite_x_lo[k];
  sprite_x_hi[k] = alt_sprite_x_hi[k];
  sprite_y_lo[k] = alt_sprite_y_lo[k];
  sprite_y_hi[k] = alt_sprite_y_hi[k];
  sprite_graphics[k] = alt_sprite_graphics[k];
  sprite_A[k] = alt_sprite_A[k];
  sprite_head_dir[k] = alt_sprite_head_dir[k];
  sprite_oam_flags[k] = alt_sprite_oam_flags[k];
  sprite_obj_prio[k] = alt_sprite_obj_prio[k];
  sprite_D[k] = alt_sprite_D[k];
  sprite_flags2[k] = alt_sprite_flags2[k];
  sprite_floor[k] = alt_sprite_floor[k];
  sprite_ai_state[k] = alt_sprite_spawned_flag[k];
  sprite_flags3[k] = alt_sprite_flags3[k];
  sprite_B[k] = alt_sprite_B[k];
  sprite_C[k] = alt_sprite_C[k];
  sprite_E[k] = alt_sprite_E[k];
  sprite_subtype2[k] = alt_sprite_subtype2[k];
  sprite_z[k] = alt_sprite_height_above_shadow[k];
  sprite_delay_main[k] = alt_sprite_delay_main[k];
  sprite_I[k] = alt_sprite_I[k];
  sprite_ignore_projectile[k] = alt_sprite_maybe_ignore_projectile[k];
  Sprite_ExecuteSingle(k);
  if (sprite_pause[k] != 0)
    alt_sprite_state[k] = 0;
  sprite_ignore_projectile[k] = bak23;
  sprite_I[k] = bak22;
  sprite_delay_main[k] = bak21;
  sprite_z[k] = bak20;
  sprite_subtype2[k] = bak19;
  sprite_E[k] = bak18;
  sprite_C[k] = bak17;
  sprite_B[k] = bak16;
  sprite_flags3[k] = bak15;
  sprite_ai_state[k] = bak14;
  sprite_floor[k] = bak13;
  sprite_flags2[k] = bak12;
  sprite_D[k] = bak11;
  sprite_obj_prio[k] = bak10;
  sprite_oam_flags[k] = bak9;
  sprite_head_dir[k] = bak8;
  sprite_A[k] = bak7;
  sprite_graphics[k] = bak6;
  sprite_y_hi[k] = bak5;
  sprite_y_lo[k] = bak4;
  sprite_x_hi[k] = bak3;
  sprite_x_lo[k] = bak2;
  sprite_type[k] = bak1;
  sprite_state[k] = bak0;

}

// Converts a 2D velocity vector (x, y) into a discrete 16-direction angle (0–15).
// The return value maps to compass directions in 22.5° increments: 0 = east, 4 = north,
// 8 = west, 12 = south (approximate; exact mapping depends on the table entries).
// Algorithm:
//   s = sign quadrant index: (sign(y) + sign(x)*2) * 8 selects one of 4 octant groups.
//   x and y are made positive (abs) for magnitude comparison.
//   If |x| >= |y|: X dominates — use kConvertVelocityToAngle_Tab0[(y>>2) + s].
//   If |x| < |y|: Y dominates — use kConvertVelocityToAngle_Tab1[(x>>2) + s].
// The >>2 reduces each axis to a 0–7 sub-octant index, giving 8 graduated angles per quadrant.
uint8 Sprite_ConvertVelocityToAngle(uint8 x, uint8 y) {  // 9df614
  static const uint8 kConvertVelocityToAngle_Tab0[32] = {
    0, 0, 1, 1, 1, 2, 2, 2, 0, 0, 15, 15, 15, 14, 14, 14,
    8, 8, 7, 7, 7, 6, 6, 6, 8, 8,  9,  9,  9, 10, 10, 10,
  };
  static const uint8 kConvertVelocityToAngle_Tab1[32] = {
    4, 4, 3, 3, 3, 2, 2, 2, 12, 12, 13, 13, 13, 14, 14, 14,
    4, 4, 5, 5, 5, 6, 6, 6, 12, 12, 11, 11, 11, 10, 10, 10,
  };
  int s = ((y >> 7) + (x >> 7) * 2) * 8;
  if (sign8(x)) x = -x;
  if (sign8(y)) y = -y;
  if (x >= y) {
    return kConvertVelocityToAngle_Tab0[(y >> 2) + s];
  } else {
    return kConvertVelocityToAngle_Tab1[(x >> 2) + s];
  }
}

// Spawns a new sprite of type what from parent slot k, searching all 16 slots (15 down to 0)
// for an inactive slot. Thin wrapper around Sprite_SpawnDynamicallyEx with start index j=15.
// Returns the allocated slot index, or -1 if all slots are occupied.
int Sprite_SpawnDynamically(int k, uint8 what, SpriteSpawnInfo *info) {  // 9df65d
  return Sprite_SpawnDynamicallyEx(k, what, info, 15);
}

// Core dynamic sprite spawner: searches slots j downward to 0 for the first inactive slot
// (state == 0). On success:
//   - Sets type = what, state = 9 (active), inherits floor and facing (D) from parent k.
//   - Fills SpriteSpawnInfo *info with parent's world X/Y/Z and overlord X/Y for use by
//     the caller to set the child's position after returning.
//   - Calls SpritePrep_LoadProperties(j) to zero and repopulate all property fields.
//   - Sets sprite_N[j] = 0xFF (indoor) or sprite_N_word[j] = 0xFFFF (overworld) to mark
//     the child as dynamically spawned (no room-table death flag).
//   - Clears die_action and subtype.
// Returns j (the new slot index), or -1 if the search exhausted all slots.
int Sprite_SpawnDynamicallyEx(int k, uint8 what, SpriteSpawnInfo *info, int j) {  // 9df65f
  do {
    if (sprite_state[j] == 0) {
      sprite_type[j] = what;
      sprite_state[j] = 9;
      info->r0_x = Sprite_GetX(k);
      info->r2_y = Sprite_GetY(k);
      info->r4_z = sprite_z[k];
      info->r5_overlord_x = overlord_x_lo[k] | overlord_x_hi[k] << 8;
      info->r7_overlord_y = overlord_y_lo[k] | overlord_y_hi[k] << 8;
      SpritePrep_LoadProperties(j);
      if (!player_is_indoors) {
        sprite_N_word[j] = 0xffff;
      } else {
        sprite_N[j] = 0xff;
      }
      sprite_floor[j] = sprite_floor[k];
      sprite_D[j] = sprite_D[k];
      sprite_die_action[j] = 0;
      sprite_subtype[j] = 0;
      break;
    }
  } while (--j >= 0);
  return j;
}

// Draws the visual for a sprite falling into a pit (state 1 — SpriteModule_Fall1).
// kSpriteFall_Char[delay_main>>2] selects one of 8 tiles that cycle through three stages:
//   0x83 (large dark blob, 3 frames) → 0x80 (mid-size, 3 frames) → 0xb7 (small dot, 2 frames)
// giving a shrinking silhouette as the sprite drops away.
// Draws a single OAM entry at (info->x + 4, info->y + 4) — centred within the 8×8 tile cell.
// OAM flags: inherits palette/priority from info->flags & 0x30, forces ext flag 4 (no rotation).
// Calls Sprite_CorrectOamEntries(k, 0, 0) to clamp Y and fix the X-overflow bit.
void SpriteFall_Draw(int k, PrepOamCoordsRet *info) {  // 9dffc5
  static const uint8 kSpriteFall_Char[8] = {0x83, 0x83, 0x83, 0x80, 0x80, 0x80, 0xb7, 0xb7};
  OamEnt *oam = GetOamCurPtr();
  oam->x = info->x + 4;
  oam->y = info->y + 4;
  oam->charnum = kSpriteFall_Char[sprite_delay_main[k] >> 2];
  oam->flags = info->flags & 0x30 | 0x04;
  Sprite_CorrectOamEntries(k, 0, 0);
}

// Spawns a sparkle garnish effect at world position (x, y) associated with sprite slot k,
// using only the lower garnish slots 0-14 (below sprites). Used when sparkles should appear
// behind the sprite (e.g., floor sparkles, item glints that stay under the pickup).
void Sprite_GarnishSpawn_Sparkle_limited(int k, uint16 x, uint16 y) {  // 9ea001
  Sprite_SpawnSimpleSparkleGarnishEx(k, x, y, 14);
}

// Spawns a sparkle garnish effect at world position (x, y) associated with sprite slot k,
// using all 30 garnish slots (0-29). Upper slots render above sprites, so this variant
// is used for sparkles that should appear in front of enemies (e.g., hit sparks, magic glints).
// Returns the allocated garnish slot index, or -1 if all slots are occupied.
int Sprite_GarnishSpawn_Sparkle(int k, uint16 x, uint16 y) {  // 9ea007
  return Sprite_SpawnSimpleSparkleGarnishEx(k, x, y, 29);
}

// Makes sprite k act as an impassable barrier that halts all of Link's movement on contact.
// Temporarily clears sprite_flags4[k] (so the collision test ignores any special flags
// that would normally suppress the hit), runs Sprite_CheckDamageToLink_same_layer, then
// restores flags4. If contact is detected, calls Sprite_HaltAllMovement to zero Link's
// speed, cancel his dash, and cancel any hookshot drag. Used by wall/barrier entity types
// that should block movement without dealing damage.
void Sprite_BehaveAsBarrier(int k) {  // 9ef4f3
  uint8 bak = sprite_flags4[k];
  sprite_flags4[k] = 0;
  if (Sprite_CheckDamageToLink_same_layer(k))
    Sprite_HaltAllMovement();
  sprite_flags4[k] = bak;
}

// Immediately stops all of Link's movement by zeroing speed, cancelling the dash, and
// releasing any active hookshot drag. Called by barrier sprites and wall-push logic.
void Sprite_HaltAllMovement() {  // 9ef508
  Sprite_NullifyHookshotDrag();
  link_speed_setting = 0;
  Link_CancelDash();
}

// Spawns a fairy (type 0xE3) at Link's current position to revive him from death.
// Uses slot 0 as the "parent" for the spawn call (no true parent; slot 0 provides
// floor context only). Positions the fairy at (link_x + 8, link_y + 16) — slightly
// right of and below Link's centre, matching the fairy-bottle release animation.
// Sets D = 0 (facing right), delay_aux4 = 96 (immunity window before the fairy will
// move away and can be re-caught). Returns the allocated slot index, or -1 if full.
int ReleaseFairy() {  // 9efe33
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(0, 0xe3, &info);
  if (j >= 0) {
    sprite_floor[j] = link_is_on_lower_level;
    Sprite_SetX(j, link_x_coord + 8);
    Sprite_SetY(j, link_y_coord + 16);
    sprite_D[j] = 0;
    sprite_delay_aux4[j] = 96;
  }
  return j;
}

// Draws a water ripple effect for sprite k when it is standing in shallow water or a puddle.
// Only executes when sprite_I[k] is 8 (shallow water) or 9 (puddle/fountain), which are the
// two water-surface tile interaction classes set by Sprite_CheckTileProperty.
// If the sprite is small (flags3 bit 5 set): shifts cur_sprite_x left by 4 pixels to
// centre the ripple under the narrower hitbox; additionally shifts cur_sprite_y up by 7
// pixels for type 0xDF (small water creature, e.g., Zora).
// Calls SpriteDraw_WaterRipple(k) to emit the OAM ripple tile, then re-calls
// Sprite_Get16BitCoords and Oam_AllocateFromRegionA to re-establish the OAM pointer
// for the sprite's main tile set drawn afterward.
void Sprite_DrawRippleIfInWater(int k) {  // 9eff8d
  if (sprite_I[k] != 8 && sprite_I[k] != 9)
    return;

  if (sprite_flags3[k] & 0x20) {
    cur_sprite_x -= 4;
    if (sprite_type[k] == 0xdf)
      cur_sprite_y -= 7;
  }
  SpriteDraw_WaterRipple(k);
  Sprite_Get16BitCoords(k);
  Oam_AllocateFromRegionA(((sprite_flags2[k] & 0x1f) + 1) * 4);
}

