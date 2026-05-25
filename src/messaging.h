/*
 * messaging.h - Dialog rendering, dungeon/world map UI, game-over sequence,
 *               and save menu interface declarations.
 *
 * This header covers all in-game messaging and overlay UI systems:
 *   - Text dialog box rendering using a variable-width font (VWF) engine
 *   - Dungeon map display with floor navigation, room markers, and boss icons
 *   - Overworld/world map display with pendant/crystal tracking
 *   - Game-over death sequence (iris wipe, letter animation, revival fairy)
 *   - Save/continue menu after death
 *   - Flute fast-travel menu and bird transport
 *   - Desert Palace prayer cutscene with iris HDMA effect
 *   - Potion shop item selection menus
 *
 * The messaging module is activated via Module 0E (Interface) and Module 12
 * (Game Over). It uses HDMA for spotlight/iris effects and communicates with
 * the HUD, overworld, and dungeon systems for map state.
 *
 * Related files: messaging.c (implementation), hud.h (HUD overlay),
 *                overworld.h (map data), dungeon.h (room data)
 */
#pragma once
#include "types.h"

// -----------------------------------------------------------------------
// Dungeon Map Data Accessors
// -----------------------------------------------------------------------

const uint8 *GetDungmapFloorLayout();
// Returns dungeon map metadata for the specified count index
uint8 GetOtherDungmapInfo(int count);
// Dungeon map submodule phase 4: handles post-draw cleanup or transitions
void DungMap_4();
// Messaging submodule 6: final messaging state (close dialog or advance)
void Module_Messaging_6();
// Configures HDMA tables for the overworld map scrolling window effect
void OverworldMap_SetupHdma();
// Returns a pointer to the light world overworld tilemap data
const uint8 *GetLightOverworldTilemap();
// Serializes the current game state to the active save slot in SRAM
void SaveGameFile();
// Transfers Mode 7 character graphics for rotated/scaled map rendering
void TransferMode7Characters();

// -----------------------------------------------------------------------
// Module 0E - Interface Entry Points
// These functions are dispatched by the main module router when the game
// enters the interface/messaging state (module 0x0E).
// -----------------------------------------------------------------------

// Master entry point for Module 0E; dispatches to the active submodule
void Module0E_Interface();
// Messaging submodule 0: initializes the messaging state machine
void Module_Messaging_0();
// -----------------------------------------------------------------------
// Potion Shop Menus and Save/Spawn Selection
// These submodules handle item purchase dialogs and the save/continue UI.
// -----------------------------------------------------------------------

// Desert Palace prayer cutscene triggered by reading the tablet at entrance
void Module0E_05_DesertPrayer();
// Red potion purchase dialog at the witch's shop
void Module0E_04_RedPotion();
// Green potion purchase dialog
void Module0E_08_GreenPotion();
// Blue potion purchase dialog
void Module0E_09_BluePotion();
// Save menu presented from the pause screen or after game over
void Module0E_0B_SaveMenu();
// Spawn point selection screen used for continue-after-death placement
void Module1B_SpawnSelect();
// -----------------------------------------------------------------------
// Desert Prayer Iris HDMA Effect
// The desert prayer uses a circular iris wipe (shrinking/expanding circle)
// implemented via HDMA window registers. These functions build the per-
// scanline HDMA tables that define the circular clipping region.
// -----------------------------------------------------------------------

// Tears down messaging state and prepares HDMA for the desert prayer iris
void CleanUpAndPrepDesertPrayerHDMA();
// Sets up initial HDMA channel configuration for the iris spotlight effect
void DesertPrayer_InitializeIrisHDMA();
// Builds the per-scanline HDMA table defining the circular iris boundary
void DesertPrayer_BuildIrisHDMATable();
// Computes left/right window boundaries for one scanline of the iris circle
Pair16U DesertHDMA_CalculateIrisShapeLine();
// -----------------------------------------------------------------------
// Module 12 - Game Over Sequence
// The game-over sequence is a multi-phase animation: Link swoons, the
// screen iris-wipes to black, "GAME OVER" letters animate in one at a
// time, and the player chooses Save/Continue/Retry. If a bottled fairy
// exists, it triggers a revival animation instead of the full game over.
// -----------------------------------------------------------------------

// Animates the individual "GAME OVER" letter sprites dropping into place
void Animate_GAMEOVER_Letters();
// Sweeps the "GAME OVER" text leftward during the reveal animation
void GameOverText_SweepLeft();
// Unfurls the "GAME OVER" text rightward as the final positioning pass
void GameOverText_UnfurlRight();
// Master entry point for Module 12; dispatches game-over submodules
void Module12_GameOver();
// Skips the delay timer and advances the game-over state immediately
void GameOver_AdvanceImmediately();
// Death submodule 1: early death animation phase (screen darkening)
void Death_Func1();
// Pauses briefly before starting the iris wipe to let the death sink in
void GameOver_DelayBeforeIris();
// Performs the circular iris-wipe-to-black transition after death
void GameOver_IrisWipe();
// Renders the red splat effect and fades the screen during death
void GameOver_SplatAndFade();
// Death submodule 6: intermediate death animation phase
void Death_Func6();
// Death submodule 4: intermediate death animation phase
void Death_Func4();
// Bouncing animation variant for the "GAME OVER" letter drop effect
void Animate_GAMEOVER_Letters_bounce();
// Final positioning of the "GAME OVER" text after all letters land
void GameOver_Finalize_GAMEOVR();
// Presents the Save/Continue choice menu after the game-over animation
void GameOver_SaveAndOrContinue();
// Increments the death counter (if count_as_death) and resets game state
void Death_Func15(bool count_as_death);
// Animates the fairy sprite that appears during the choice menu
void GameOver_AnimateChoiceFairy();
// Spawns and initializes the revival fairy from a bottled fairy
void GameOver_InitializeRevivalFairy();
// Main animation loop for the revival fairy circling Link
void RevivalFairy_Main_bounce();
// Lifts Link slightly off the ground during the fairy revival sequence
void GameOver_RiseALittle();
// Restores submodule 0x0D state after the game-over sequence completes
void GameOver_Restore0D();
// Restores submodule 0x0E state after the game-over sequence completes
void GameOver_Restore0E();
// Repositions Link at the appropriate respawn point after continuing
void GameOver_ResituateLink();
// -----------------------------------------------------------------------
// Flute Menu / Bird Travel
// The flute menu lets the player select a fast-travel destination on the
// overworld map. After selection, a bird carries Link to the chosen spot.
// -----------------------------------------------------------------------

// Entry point for the flute destination selection screen (Module 0E, sub 0A)
void Module0E_0A_FluteMenu();
// Processes D-pad input to cycle through available flute destinations
void FluteMenu_HandleSelection();
// Loads the overworld screen data for the selected flute destination
void FluteMenu_LoadSelectedScreen();
// Loads the overlay tilemap and map data for the destination area
void Overworld_LoadOverlayAndMap();
// Fades the flute menu in and plays the duck quack sound effect
void FluteMenu_FadeInAndQuack();
// Completes the bird travel sequence by placing Link at the destination
void BirdTravel_Finish_Doit();
// -----------------------------------------------------------------------
// Overworld / World Map Display
// The world map is a Mode 7 rotated/scaled view of the entire overworld.
// It shows pendant and crystal locations as markers. The player can pan
// around the map with the D-pad.
// -----------------------------------------------------------------------

// Entry point for the overworld map messaging submodule
void Messaging_OverworldMap();
// Fades the screen to black before loading the world map graphics
void WorldMap_FadeOut();
// Loads the light world map tilemap and palette for Mode 7 display
void WorldMap_LoadLightWorldMap();
// Loads the dark world map tilemap and palette for Mode 7 display
void WorldMap_LoadDarkWorldMap();
// Loads the sprite graphics used for map markers (pendants, crystals, Link)
void WorldMap_LoadSpriteGFX();
// Brightens the screen from black after map graphics are loaded
void WorldMap_Brighten();
// Processes D-pad input for panning around the world map
void WorldMap_PlayerControl();
// Restores the pre-map background graphics when exiting the world map
void WorldMap_RestoreGraphics();
// Configures HDMA for the attract mode conclusion sequence
void Attract_SetUpConclusionHDMA();
// Tears down map state and returns to normal gameplay after map exit
void WorldMap_ExitMap();
// Initializes the HDMA window effect for the world map border
void WorldMap_SetUpHDMA();
// Fills the background tilemap with tile 0xEF (blank/border tile)
void WorldMap_FillTilemapWithEF();
// Draws all sprite overlays on the world map (Link cursor, markers)
void WorldMap_HandleSprites();
// Returns true if pendant k has been collected (for map marker display)
bool OverworldMap_CheckForPendant(int k);
// Returns true if crystal k has been collected (for map marker display)
bool OverworldMap_CheckForCrystal(int k);
// -----------------------------------------------------------------------
// Dungeon Map Display (Module 0E, Submodule 0x03)
// Shows the current dungeon's floor layout, room grid, Link's position,
// the boss room icon, and allows the player to scroll between floors.
// The map requires the map item to have been collected from a chest.
// -----------------------------------------------------------------------

// Master entry point for the dungeon map submodule
void Module0E_03_DungeonMap();
// Orchestrates the multi-step dungeon map drawing sequence
void Module0E_03_01_DrawMap();
// Step 0: Loads and prepares map-specific graphics into VRAM
void Module0E_03_01_00_PrepMapGraphics();
// Step 1: Draws the "LEVEL" text label and dungeon name on the map
void Module0E_03_01_01_DrawLEVEL();
// Step 2: Draws the floor selector backdrop (the column of floor numbers)
void Module0E_03_01_02_DrawFloorsBackdrop();
// Builds the visual boxes for each floor in the floor list sidebar
void DungeonMap_BuildFloorListBoxes(uint8 t5, uint16 r14);
// Step 3: Draws the room grid for the currently selected floor
void Module0E_03_01_03_DrawRooms();
// Draws the border frame around the room grid area
void DungeonMap_DrawBorderForRooms(uint16 pd, uint16 mask);
// Draws floor number labels adjacent to each room row
void DungeonMap_DrawFloorNumbersByRoom(uint16 pd, uint16 r8);
// Renders the dungeon layout grid showing visited/unvisited rooms
void DungeonMap_DrawDungeonLayout(int pd);
// Draws a single horizontal row of room tiles within the dungeon grid
void DungeonMap_DrawSingleRowOfRooms(int i, int arg_x);
// Places chest and key markers on rooms that contain them
void DungeonMap_DrawRoomMarkers();
// Combined handler for player input processing and sprite rendering
void DungeonMap_HandleInputAndSprites();
// Reads controller input for map navigation (D-pad to scroll floors)
void DungeonMap_HandleInput();
// Processes directional input for panning within the map view
void DungeonMap_HandleMovementInput();
// Handles up/down input to select a different dungeon floor
void DungeonMap_HandleFloorSelect();
// Smoothly scrolls the map view when changing between floors
void DungeonMap_ScrollFloors();
// Draws all OAM sprites on the dungeon map (Link, boss, markers)
void DungeonMap_DrawSprites();
// Draws Link's pointing-hand sprite at the given OAM position
void DungeonMap_DrawLinkPointing(int spr_pos, uint8 r2, uint8 r3);
// Draws the blinking indicator for the current room; returns next spr_pos
int DungeonMap_DrawBlinkingIndicator(int spr_pos);
// Draws the location marker showing Link's position; returns next spr_pos
int DungeonMap_DrawLocationMarker(int spr_pos, uint16 r14);
// Draws the floor number sprites in the sidebar; returns next spr_pos
int DungeonMap_DrawFloorNumberObjects(int spr_pos);
// Animates the blinking effect on the currently selected floor number
void DungeonMap_DrawFloorBlinker();
// Draws the boss skull icon on the boss floor; returns next spr_pos
int DungeonMap_DrawBossIcon(int spr_pos);
// Draws the boss icon only if the selected floor matches the boss floor
int DungeonMap_DrawBossIconByFloor(int spr_pos);
// Restores the background graphics that were overwritten by the map display
void DungeonMap_RecoverGFX();
// -----------------------------------------------------------------------
// Miscellaneous Messaging Utilities
// -----------------------------------------------------------------------

// Toggles the star tile puzzle state and advances the dungeon submodule
void ToggleStarTilesAndAdvance();
// Sets up OAM entries for each "GAME OVER" letter sprite before animation
void Death_InitializeGameOverLetters();
// Copies save data from SRAM into the working WRAM buffer for editing
void CopySaveToWRAM();
// -----------------------------------------------------------------------
// Variable-Width Font (VWF) Text Rendering Engine
// The text engine renders dialog boxes with a proportional-width font.
// Characters are rendered into a tile buffer at sub-tile pixel offsets,
// then transferred to VRAM. The system supports multi-line text with
// scrolling, player name substitution, and branching choice menus.
// -----------------------------------------------------------------------

// Main text rendering entry point; called each frame during active dialog
void RenderText();
// Renders the Save/Continue/Retry text options after the death sequence
void RenderText_PostDeathSaveOptions();
// Initializes the text engine state for a new dialog message
void Text_Initialize();
// Loops to initialize the messaging module state registers
void Text_Initialize_initModuleStateLoop();
// Resets the VWF cursor position and tile buffer for fresh rendering
void Text_InitVwfState();
// Loads the next batch of characters from the message data into the buffer
void Text_LoadCharacterBuffer();
// Writes the player's name into the output buffer at pointer p; returns end
uint8 *Text_WritePlayerName(uint8 *p);
// Maps raw character bytes to the font tileset index for the player's name
uint8 Text_FilterPlayerNameCharacters(uint8 a);
// Advances the text rendering state machine by one step
void Text_Render();
// Draws the complete dialog box border frame in one pass
void RenderText_Draw_Border();
// Draws the dialog box border incrementally (animated expansion effect)
void RenderText_Draw_BorderIncremental();
// Copies the rendered character tilemap into the dialog box area
void RenderText_Draw_CharacterTilemap();
// Renders message characters one at a time with the typewriter effect
void RenderText_Draw_MessageCharacters();
// Finalizes text rendering and waits for player input to dismiss
void RenderText_Draw_Finish();
// Renders a single character glyph using the variable-width font engine
void VWF_RenderSingle();
// Presents a 2-low-or-3 choice menu (e.g., Yes/No with item context)
void RenderText_Draw_Choose2LowOr3();
// Presents an item selection choice within a dialog
void RenderText_Draw_ChooseItem();
// Moves the Y-item selection cursor to the previous option
void RenderText_FindYItem_Previous();
// Moves the Y-item selection cursor to the next option
void RenderText_FindYItem_Next();
// Highlights the currently selected Y-item in the choice list
void RenderText_DrawSelectedYItem();
// Presents a 2-high-or-3 choice menu variant
void RenderText_Draw_Choose2HiOr3();
// Presents a 3-option choice menu
void RenderText_Draw_Choose3();
// Presents a 1-or-2 option choice menu
void RenderText_Draw_Choose1Or2();
// Scrolls the text box up by one line; returns true when scroll is complete
bool RenderText_Draw_Scroll();
// Positions the dialog window at the default screen location
void RenderText_SetDefaultWindowPosition();
// Initializes border tile data before the dialog box is drawn
void RenderText_DrawBorderInitialize();
// Draws one horizontal row of the dialog border; returns next write pointer
uint16 *RenderText_DrawBorderRow(uint16 *d, int y);
// Converts rendered VWF characters into the VRAM tilemap format
void Text_BuildCharacterTilemap();
// Refreshes the text display after a scroll or choice change
void RenderText_Refresh();
// Pre-computes message pointers from the compressed message table
void Text_GenerateMessagePointers();
// -----------------------------------------------------------------------
// Dungeon Map Palette Transitions
// -----------------------------------------------------------------------

// Brightens the dungeon map palette from black (fade-in effect on open)
void DungMap_LightenUpMap();
// Saves the current BG palette so it can be restored when the map closes
void DungMap_Backup();
// Fades the dungeon map palette to black (fade-out effect on close)
void DungMap_FadeMapToBlack();
// Restores the pre-map BG palette from the backup saved by DungMap_Backup
void DungMap_RestoreOld();

// -----------------------------------------------------------------------
// Death Animation Helpers
// -----------------------------------------------------------------------

// Plays Link's swooning/spinning death animation before the game over
void Death_PlayerSwoon();
// Prepares the screen state for the fainting transition (disables sprites)
void Death_PrepFaint();
// Renders the post-death Save/Continue/Retry selection menu
void DisplaySelectMenu();
// Returns true if the player pressed the button to open the map
bool DidPressButtonForMap();
