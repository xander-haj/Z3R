/*
 * overworld.h - Overworld map exploration, scrolling, screen transitions,
 *               and world interaction declarations.
 *
 * The overworld consists of 128 screens (64 light world + 64 dark world),
 * organized as 8x8 grids of 512x512 pixel areas. Some screens are "big"
 * (2x2 screens treated as one area, e.g., Hyrule Castle grounds). The
 * overworld system handles:
 *
 *   - Screen loading, decompression, and tilemap construction
 *   - Scroll transitions between adjacent screens (north/south/east/west)
 *   - Mosaic fade transitions for non-adjacent screen changes
 *   - Mirror warp between light and dark worlds
 *   - Entrance detection and dungeon/cave entry
 *   - Map16-to-Map8 tile decomposition for rendering
 *   - Tile interaction (lifting, bombing, hammering, powder, signs)
 *   - Overlay management (rain, secret areas, event-triggered changes)
 *   - Camera boundary enforcement and scroll locking
 *   - Flute/bird fast-travel destination loading
 *   - Whirlpool warp pads and pit fall destinations
 *   - Animated dungeon entrances (Palace of Darkness, Skull Woods, etc.)
 *   - Peg and switch puzzles on the overworld
 *
 * The overworld runs as Module 09 (gameplay) and Module 08 (loading).
 * Module 0F/10 handle spotlight close/open transitions.
 *
 * Tile hierarchy: Map32 (32x32 metatiles stored in ROM) are decomposed
 * into Map16 (16x16, the primary interaction unit), which decompose into
 * Map8 (8x8 hardware tiles written to VRAM tilemaps).
 *
 * Related files: overworld.c (implementation), dungeon.h (indoor areas),
 *                tile_detect.h (collision), variables.h (WRAM state)
 */
#pragma once
#include "zelda_rtl.h"
#include "variables.h"

// -----------------------------------------------------------------------
// Tile Mapping and Entrance Lookup Tables
// -----------------------------------------------------------------------

// Returns a pointer to the Map8-to-tile-attribute lookup table
const uint8 *GetMap8toTileAttr();
// Returns a pointer to the Map16-to-Map8 decomposition table
const uint16 *GetMap16toMap8Table();
// Checks if position (r0, r2) matches an overworld entrance; returns true if found
bool LookupInOwEntranceTab(uint16 r0, uint16 r2);
// Looks up the entrance ID at the given overworld tile position
int LookupInOwEntranceTab2(uint16 pos);
// Returns true if the player can enter entrance e while a tagalong is following
bool CanEnterWithTagalong(int e);
// Converts a directional bitmask to a 0-3 enum (up=0, down=1, left=2, right=3)
int DirToEnum(int dir);
// -----------------------------------------------------------------------
// Overworld Utility Functions
// -----------------------------------------------------------------------

// Decrements the mosaic level toward zero (used for fade-in from mosaic)
void Overworld_ResetMosaicDown();
// Overworld submodule 0x1D: post-transition cleanup
void Overworld_Func1D();
// Overworld submodule 0x1E: finalize screen after special transition
void Overworld_Func1E();
// Returns the message ID for the sign text at the given overworld area
uint16 Overworld_GetSignText(int area);
// Returns a pointer to the sprite data table for the given overworld area
const uint8 *GetOverworldSpritePtr(int area);
// Returns the BG palette index for the given overworld screen
uint8 GetOverworldBgPalette(int idx);
// Loads sprite graphics properties for the current area (both worlds)
void Sprite_LoadGraphicsProperties();
// Loads sprite graphics properties assuming light world context only
void Sprite_LoadGraphicsProperties_light_world_only();

// -----------------------------------------------------------------------
// Mirror Warp HDMA Effects
// The mirror warp uses wavy horizontal distortion implemented via HDMA
// writes to the BG horizontal scroll register. The wave amplitude
// increases during the warp-out and decreases during the warp-in.
// -----------------------------------------------------------------------

// Sets up the HDMA channel for the mirror warp wave distortion effect
void InitializeMirrorHDMA();
// Builds the HDMA table with increasing wave amplitude (warping out)
void MirrorWarp_BuildWavingHDMATable();
// Builds the HDMA table with decreasing wave amplitude (warping in)
void MirrorWarp_BuildDewavingHDMATable();
// Applies fall damage when Link drops into a pit on the overworld
void TakeDamageFromPit();
// -----------------------------------------------------------------------
// Module 08 - Overworld Loading
// Handles the full loading sequence when entering the overworld from
// a dungeon, save file, or special area. Loads map data, tilesets,
// palettes, sprites, and camera position before revealing the screen.
// -----------------------------------------------------------------------

// Master entry point for Module 08 (overworld load from dungeon/save)
void Module08_OverworldLoad();
// Pre-loads overworld properties (palettes, tileset IDs, music) for the area
void PreOverworld_LoadProperties();
// Sets Link's bunny/human form based on Moon Pearl and current world
void AdjustLinkBunnyStatus();
// Forces Link out of bunny form (used after specific events like mirror warp)
void ForceNonbunnyStatus();
// Resets Link's position to safe ground after a drowning event
void RecoverPositionAfterDrowning();

// -----------------------------------------------------------------------
// Modules 0F/10 - Spotlight Transitions
// Iris spotlight transitions are used when entering/exiting dungeons
// and caves. Module 0F closes the iris (fade out), Module 10 opens it.
// -----------------------------------------------------------------------

// Module 0F entry: closes the iris spotlight and loads the destination area
void Module0F_SpotlightClose();
// Prepares dungeon exit state before the spotlight close begins
void Dungeon_PrepExitWithSpotlight();
// Configures the HDMA spotlight table and begins the iris animation
void Spotlight_ConfigureTableAndControl();
// Advances to the next step after the spotlight opens (gameplay resume)
void OpenSpotlight_Next2();
// Module 10 entry: opens the iris spotlight to reveal the new area
void Module10_SpotlightOpen();
// Module 10 submodule 0: performs the iris-open animation
void Module10_00_OpenIris();
// Sets the mirror/death warp destination to the Pyramid of Power
void SetTargetOverworldWarpToPyramid();
// Clears all active ancillae and resets any running cutscene state
void ResetAncillaAndCutscene();
// -----------------------------------------------------------------------
// Module 09 - Overworld Gameplay and Scroll Transitions
// This is the main overworld module. Submodule 0x00 is normal gameplay;
// higher submodules handle scroll transitions, mosaic fades, mirror
// warps, whirlpool warps, drowning recovery, and other state changes.
// -----------------------------------------------------------------------

// Master entry point for Module 09; dispatches to the active submodule
void Module09_Overworld();
// Updates the rain overlay effect (activates in light world pre-Agahnim)
void OverworldOverlay_HandleRain();
// Submodule 0x00: normal gameplay with player control and sprite updates
void Module09_00_PlayerControl();
// Checks if Link has reached a screen edge and initiates scroll transition
void OverworldHandleTransitions();
// Loads tileset graphics and computes screen size for the destination area
void Overworld_LoadGFXAndScreenSize();
void Overworld_SetMap16LoadOffsetToCamera();
// Scrolls the camera and checks for special overworld exit conditions
void ScrollAndCheckForSOWExit();
// Loads auxiliary graphics during an overworld transition
void Module09_LoadAuxGFX();
// Finalizes transitional graphics loading after the scroll completes
void Overworld_FinishTransGfx();
// Loads the new map data and graphics for the destination screen
void Module09_LoadNewMapAndGFX();
// Runs the per-frame scroll transition animation between screens
void Overworld_RunScrollTransition();
// Loads sprites for the newly revealed screen after scroll transition
void Module09_LoadNewSprites();
// Begins the scroll transition sequence (sets direction, target, speed)
void Overworld_StartScrollTransition();
// Gradually decelerates the scroll transition as it approaches the end
void Overworld_EaseOffScrollTransition();
// Auto-walks Link southward after exiting a cave/dungeon facing down
void Module09_0A_WalkFromExiting_FacingDown();
// Auto-walks Link northward after exiting a cave/dungeon facing up
void Module09_0B_WalkFromExiting_FacingUp();
// Plays the big door opening animation when exiting certain dungeons
void Module09_09_OpenBigDoorFromExiting();
// Variant of 32x32 map update used for specific overlay situations
void Overworld_DoMapUpdate32x32_B();
// Opens a big door on the overworld (Hyrule Castle, Eastern Palace, etc.)
void Module09_0C_OpenBigDoor();
// Conditionally updates a 32x32 tile area if the dirty flag is set
void Overworld_DoMapUpdate32x32_conditional();
// Updates a 32x32 pixel tile area in the overworld tilemap
void Overworld_DoMapUpdate32x32();
// -----------------------------------------------------------------------
// Mosaic Transitions and Mirror Warp
// Mosaic transitions pixelate the screen during non-adjacent screen
// changes (e.g., flute travel, falling into pits). The mirror warp
// switches between light and dark worlds with a wavy HDMA effect.
// -----------------------------------------------------------------------

// Begins a mosaic pixelation transition to a new overworld area
void Overworld_StartMosaicTransition();
// Loads overlay tilemap data for the current overworld screen
void Overworld_LoadOverlays();
// Pre-loads overlays before the overworld is fully initialized
void PreOverworld_LoadOverlays();
// Secondary overlay loading pass (event-triggered overlays)
void Overworld_LoadOverlays2();
// Fades the mosaic level back to zero, revealing the new screen
void Module09_FadeBackInFromMosaic();
// Overworld submodule 0x1C: intermediate transition handler
void Overworld_Func1C();
// Loads sprite graphics and sets the mosaic register during transition
void OverworldMosaicTransition_LoadSpriteGraphicsAndSetMosaic();
// Overworld submodule 0x22: post-warp state initialization
void Overworld_Func22();
// Overworld submodule 0x18: pre-mirror-warp preparation
void Overworld_Func18();
// Overworld submodule 0x19: mirror warp intermediate state
void Overworld_Func19();
// Master mirror warp handler: runs the full light/dark world transition
void Module09_MirrorWarp();
// Completes the mirror warp by loading the destination world's data
void MirrorWarp_FinalizeAndLoadDestination();
// Redraws the screen at Link's mirror warp position in the new world
void Overworld_DrawScreenAtCurrentMirrorPosition();
// Loads sprites and palette data for the post-mirror-warp world
void MirrorWarp_LoadSpritesAndColors();
// Overworld submodule 0x2B: post-mirror-warp finalization
void Overworld_Func2B();
// Triggers the weathervane explosion animation (reveals the flute boy)
void Overworld_WeathervaneExplosion();
// Handles the whirlpool warp pad transport sequence
void Module09_2E_Whirlpool();
// Overworld submodule 0x2F: post-whirlpool arrival handler
void Overworld_Func2F();
// Initiates the drowning recovery sequence (scroll to safe land)
void Module09_2A_RecoverFromDrowning();
// Scrolls the camera to the nearest land tile during drowning recovery
void Module09_2A_00_ScrollToLand();
// -----------------------------------------------------------------------
// Camera, Boundaries, and Area Loading
// -----------------------------------------------------------------------

// Updates camera scroll position based on Link's movement
void Overworld_OperateCameraScroll();
// Checks if the camera has reached a boundary; returns adjustment needed
int OverworldCameraBoundaryCheck(int xa, int ya, int vd, int r8);
// Processes one frame of the scroll transition between screens
int OverworldScrollTransition();
// Sets the camera scroll boundaries for the given area (big or small screen)
void Overworld_SetCameraBoundaries(int big, int area);
// Completes entry onto a new screen (enables controls, starts music)
void Overworld_FinalizeEntryOntoScreen();
// Overworld submodule 0x1F: post-entry finalization handler
void Overworld_Func1F();
// Applies mosaic effect only when a transition is in progress
void ConditionalMosaicControl();
// Increases the mosaic level unconditionally (used for forced transitions)
void Overworld_ResetMosaic_alwaysIncrease();
// Selects the music track list based on the current overworld area
void Overworld_SetSongList();
// Loads the overworld state when exiting a dungeon to the outside
void LoadOverworldFromDungeon();
// Loads area properties (palette, tileset, music) for the new screen
void Overworld_LoadNewScreenProperties();
// Restores cached entrance properties for returning to a dungeon entrance
void LoadCachedEntranceProperties();
// Enters a special overworld area (Master Sword grove, Zora's domain, etc.)
void Overworld_EnterSpecialArea();
// Returns from a special overworld area to the normal overworld
void LoadOverworldFromSpecialOverworld();
// Loads the bird/duck transport animation for flute fast-travel
void FluteMenu_LoadTransport();
// Sets Link's position to bird travel destination k
void Overworld_LoadBirdTravelPos(int k);
// Loads the palette set for the screen selected on the flute menu
void FluteMenu_LoadSelectedScreenPalettes();
// Finds the exit whirlpool that pairs with the one Link entered
void FindPartnerWhirlpoolExit();
// Loads the ambient overlay (rain, mist) and optionally reloads map data
void Overworld_LoadAmbientOverlay(bool load_map_data);
// Loads the ambient overlay without reloading map data
void Overworld_LoadAmbientOverlayFalse();
// Decompresses and builds the complete screen tilemap for the current area
void Overworld_LoadAndBuildScreen();
// -----------------------------------------------------------------------
// Screen Construction and Map Stripe Loading
// When scrolling between screens, new tile columns (X stripes) or rows
// (Y stripes) are loaded incrementally as they scroll into view. The
// initial screen view functions pre-load the first visible portion of
// the destination screen in the scroll direction.
// -----------------------------------------------------------------------

// Module 08 submodule 02: loads the overworld screen and advances state
void Module08_02_LoadAndAdvance();
// Draws all four quadrants of the screen plus any active overlays
void Overworld_DrawQuadrantsAndOverlays();
// Processes overlay changes and bomb-openable door tiles
void Overworld_HandleOverlaysAndBombDoors();
// Triggers loading of n vertical map stripes (rows) and commits to VRAM
void TriggerAndFinishMapLoadStripe_Y(int n);
// Triggers loading of n horizontal map stripes (columns) and commits to VRAM
void TriggerAndFinishMapLoadStripe_X(int n);
// Applies a pending tilemap change to the current screen
void SomeTileMapChange();
// Creates the initial tilemap for the new screen during a scroll transition
void CreateInitialNewScreenMapToScroll();
// Pre-loads the north edge of a big (2x2) screen for northward scrolling
void CreateInitialOWScreenView_Big_North();
// Pre-loads the south edge of a big screen for southward scrolling
void CreateInitialOWScreenView_Big_South();
// Pre-loads the west edge of a big screen for westward scrolling
void CreateInitialOWScreenView_Big_West();
// Pre-loads the east edge of a big screen for eastward scrolling
void CreateInitialOWScreenView_Big_East();
// Pre-loads the north edge of a small (1x1) screen for northward scrolling
void CreateInitialOWScreenView_Small_North();
// Pre-loads the south edge of a small screen for southward scrolling
void CreateInitialOWScreenView_Small_South();
// Pre-loads the west edge of a small screen for westward scrolling
void CreateInitialOWScreenView_Small_West();
// Pre-loads the east edge of a small screen for eastward scrolling
void CreateInitialOWScreenView_Small_East();
// -----------------------------------------------------------------------
// Scroll Transition Stripe Builders
// During a scroll transition, tile stripes (rows or columns) of the
// incoming screen are built into a VRAM transfer buffer. Each function
// returns the updated destination pointer after writing its stripe data.
// The "Full" variants build a complete screen-width/height stripe;
// the "Check" variants only build if new map data has scrolled into view.
// -----------------------------------------------------------------------

// Scrolls the map and loads new tile data during a transition
void OverworldTransitionScrollAndLoadMap();
// Builds a full north-edge row stripe during a northward scroll transition
uint16 *BuildFullStripeDuringTransition_North(uint16 *dst);
// Builds a full south-edge row stripe during a southward scroll transition
uint16 *BuildFullStripeDuringTransition_South(uint16 *dst);
// Builds a full west-edge column stripe during a westward scroll transition
uint16 *BuildFullStripeDuringTransition_West(uint16 *dst);
// Builds a full east-edge column stripe during an eastward scroll transition
uint16 *BuildFullStripeDuringTransition_East(uint16 *dst);
// Handles per-frame map scrolling and loads new tile data as needed
void OverworldHandleMapScroll();
// Checks if new map rows appeared at the north edge; builds stripe if so
uint16 *CheckForNewlyLoadedMapAreas_North(uint16 *dst);
// Checks if new map rows appeared at the south edge; builds stripe if so
uint16 *CheckForNewlyLoadedMapAreas_South(uint16 *dst);
// Checks if new map columns appeared at the west edge; builds stripe if so
uint16 *CheckForNewlyLoadedMapAreas_West(uint16 *dst);
// Checks if new map columns appeared at the east edge; builds stripe if so
uint16 *CheckForNewlyLoadedMapAreas_East(uint16 *dst);
// Buffers and builds Map16-to-Map8 column stripes (horizontal scrolling)
uint16 *BufferAndBuildMap16Stripes_X(uint16 *dst);
// Buffers and builds Map16-to-Map8 row stripes (vertical scrolling)
uint16 *BufferAndBuildMap16Stripes_Y(uint16 *dst);
// -----------------------------------------------------------------------
// Map Decompression and Tile Conversion
// Overworld map data is stored as Map32 definitions (32x32 metatiles)
// compressed in ROM. These are decompressed, then decomposed through
// Map32 -> Map16 -> Map8 to produce the final VRAM tilemap.
// -----------------------------------------------------------------------

// Decompresses and draws all four quadrants of the current screen
void Overworld_DecompressAndDrawAllQuadrants();
// Decompresses and draws one quadrant (256x256 pixels) of the screen
void Overworld_DecompressAndDrawOneQuadrant(uint16 *dst, int screen);
// Parses a Map32 definition word into four Map16 tile indices
void Overworld_ParseMap32Definition(uint16 *dst, uint16 input);
// Loads sub-overlay Map32 data for event-triggered tile changes
void OverworldLoad_LoadSubOverlayMap32();
// Loads the main overlay tilemap for the current overworld area
void LoadOverworldOverlay();
// Converts Map16 tile data to Map8 hardware tiles for VRAM upload
void Map16ToMap8(const uint8 *src, int r20);
// Copies Map16 tile data into the working buffer for tilemap construction
void OverworldCopyMap16ToBuffer(const uint8 *src, uint16 r20, int r14, uint16 *r10);
// Restores tiles that were temporarily changed by a mirror bonk
void MirrorBonk_RecoverChangedTiles();
// Decompresses the enemy damage subclass table from ROM bank 02
void DecompressEnemyDamageSubclasses();
// Decompresses data from ROM bank 02 to dst; returns decompressed size
int Decompress_bank02(uint8 *dst, const uint8 *src);
// Reads the tile attribute byte at world pixel coordinates (x, y)
uint8 Overworld_ReadTileAttribute(uint16 x, uint16 y);
// Sets the SNES fixed color register and scroll positions for the area
void Overworld_SetFixedColAndScroll();
// Records a Map16 tile change so it persists across screen transitions
void Overworld_Memorize_Map16_Change(uint16 pos, uint16 value);
// Processes peg puzzle logic when a peg at tile position pos is hammered
void HandlePegPuzzles(uint16 pos);
// Checks if the 7 crystals are placed to open Ganon's Tower entrance
void GanonTowerEntrance_Func1();
// Checks if the player is in a special switch area and handles toggling
void Overworld_CheckSpecialSwitchArea();
// Returns a pointer to Link's current Map16 tile, stride-8 aligned
const uint16 *Overworld_GetMap16OfLink_Mult8();
// -----------------------------------------------------------------------
// Palette Animation, Events, and Entrance Handling
// -----------------------------------------------------------------------

// Phase 1 of the Master Sword acquisition palette animation
void Palette_AnimGetMasterSword();
// Phase 2 of the Master Sword palette animation (brightening)
void Palette_AnimGetMasterSword2();
// Phase 3 of the Master Sword palette animation (final flash)
void Palette_AnimGetMasterSword3();
// Animates the dark world Death Mountain palette (lava glow cycling)
void Overworld_DwDeathMountainPaletteAnimation();
// Loads event-triggered tile overlays (e.g., after Agahnim is defeated)
void Overworld_LoadEventOverlay();
// Removes all active waterfall splash ancillae from the screen
void Ancilla_TerminateWaterfallSplashes();
// Determines the destination area when Link falls into an overworld pit
void Overworld_GetPitDestination();
// Processes entrance entry: loads destination data and begins transition
void Overworld_UseEntrance();
// -----------------------------------------------------------------------
// Tile Interaction - Tools, Lifting, Bombing, Secrets
// These functions handle the player's interaction with overworld tiles
// using items (hammer, shovel, powder, bombs) or bare hands (lifting).
// Each returns a result code indicating what happened to the tile.
// -----------------------------------------------------------------------

// Processes tool/item interaction with the tile at world coords (x, y)
uint16 Overworld_ToolAndTileInteraction(uint16 x, uint16 y);
// Selects the appropriate hammer sound effect based on tile type a
void Overworld_PickHammerSfx(uint16 a);
// Returns Link's current Map16 tile index and writes pixel coords to xy
uint16 Overworld_GetLinkMap16Coords(Point16U *xy);
// Checks and handles liftable tiles at pt_arg; returns lift type or 0
uint8 Overworld_HandleLiftableTiles(Point16U *pt_arg);
// Lifts a small object (bush, pot, rock) at the given tile position
uint8 Overworld_LiftingSmallObj(uint16 a, uint16 pos, uint16 y, Point16U pt);
// Attempts to smash a dark rock pile; returns the item drop type
int Overworld_SmashRockPile(bool down_one_tile, Point16U *pt);
// Smashes a rock pile via lifting (rather than hammer); returns lift type
uint8 SmashRockPile_fromLift(uint16 a, uint16 pos, uint16 y, Point16U pt);
// Bombs a 32x32 pixel area of tiles at world coordinates (x, y)
void Overworld_BombTiles32x32(uint16 x, uint16 y);
// Bombs a single tile at tile coordinates (x, y)
void Overworld_BombTile(int x, int y);
// Modifies the weathervane tile after the flute boy event
void Overworld_AlterWeathervane();
// Opens the Gargoyle's Domain entrance on the Dark World pyramid
void OpenGargoylesDomain();
// Creates the hole in the Pyramid of Power after Ganon is knocked down
void CreatePyramidHole();
// Reveals a secret tile at position pos; returns the revealed Map16 value
uint16 Overworld_RevealSecret(uint16 pos);
// Adjusts the revealed secret when magic powder is used on it
void AdjustSecretForPowder();
// Draws a Map16 tile at pos and persists the change across transitions
void Overworld_DrawMap16_Persist(uint16 pos, uint16 value);
// Draws a Map16 tile at pos (temporary; does not persist across transitions)
void Overworld_DrawMap16(uint16 pos, uint16 value);
// Forcibly overwrites a tile at pos with value in both tilemap and Map16 buffer
void Overworld_AlterTileHardcore(uint16 pos, uint16 value);
// Computes the VRAM tilemap address for a given Map16 buffer address
uint16 Overworld_FindMap16VRAMAddress(uint16 addr);
// -----------------------------------------------------------------------
// Animated Dungeon Entrances
// Several dark world dungeons have animated entrance sequences triggered
// by specific items or events. These multi-frame animations alter the
// overworld tiles to reveal the dungeon entrance with visual fanfare.
// -----------------------------------------------------------------------

// Dispatches to the appropriate entrance animation for the current dungeon
void Overworld_AnimateEntrance();
// Palace of Darkness entrance animation (rain stops, entrance reveals)
void Overworld_AnimateEntrance_PoD();
// Skull Woods entrance animation (skull pot reveals cave opening)
void Overworld_AnimateEntrance_Skull();
// Misery Mire entrance animation (Ether medallion reveals entrance)
void Overworld_AnimateEntrance_Mire();
// Turtle Rock entrance animation (Quake medallion opens the rock)
void Overworld_AnimateEntrance_TurtleRock();
// Plays the dungeon entrance jingle sound effect
void OverworldEntrance_PlayJingle();
// Draws multiple Turtle Rock entrance tile frames in sequence
void OverworldEntrance_DrawManyTR();
// Ganon's Tower entrance animation (7 crystals open the seal)
void Overworld_AnimateEntrance_GanonsTower();
// Advances the entrance animation timer and triggers an explosion effect
void OverworldEntrance_AdvanceAndBoom();
