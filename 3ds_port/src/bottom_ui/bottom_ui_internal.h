#ifndef BOTTOM_UI_INTERNAL_H
#define BOTTOM_UI_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "global.h"
#include "main.h"
#include "money.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_controllers.h"
#include "battle_main.h"
#include "battle_message.h"
#include "contest_util.h"
#include "data.h"
#include "event_data.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "fonts.h"
#include "graphics.h"
#include "item.h"
#include "item_icon.h"
#include "item_menu.h"
#include "menu.h"
#include "new_game.h"
#include "overworld.h"
#include "palette.h"
#include "party_menu.h"
#include "pokedex.h"
#include "pokemon.h"
#include "pokemon_icon.h"
#include "pokemon_summary_screen.h"
#include "pokenav.h"
#include "region_map.h"
#include "safari_zone.h"
#include "save.h"
#include "script.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "text.h"
#include "text_window.h"
#include "util.h"
#include "constants/items.h"
#include "constants/map_types.h"
#include "constants/party_menu.h"
#include "constants/region_map_sections.h"
#include "constants/rgb.h"
#include "constants/songs.h"
#include "constants/trainers.h"
#include "port_platform.h"

#include "3ds_data.h"
#include "3ds_bottom.h"
#include "3ds_extras.h"
#include "3ds_bottom_art.h"
#include "3ds_input.h"
#include "3ds_log.h"
#include "3ds_platform.h"
#include "3ds_video.h"

/* Exported by the game under PLATFORM_3DS, or not exported by its headers. */
void CB2_BagMenuRun(void);
/* party_menu.c: the column's buttons close the game's party menu. */
bool8 CtrParty_Close(bool8 leaving);
/* menu.c: a tap on a game screen shown here, for what waits for input. */
void CtrMenu_PostTap(s16 x, s16 y);
bool8 CtrMenu_YesNoOpen(void);
void CtrStartMenu_Request(u8 action);
bool8 CtrStartMenu_Pending(void);
bool8 CtrStartMenu_Available(void);
bool8 CtrStartMenu_Busy(void);
bool8 CtrPokenav_IsOpen(void);
u32 CtrPokenav_Screen(bool8 *ready);
int CtrPokenavMenu_Options(int *cursor);
void CtrPokenavMenu_Rows(int *yStart, int *deltaY);
bool8 CtrPokenavList_View(u8 *x, u8 *y, u8 *width, u16 *top, u16 *selected, u16 *shown, u16 *count);
u8 CtrPokenavMatchCall_Input(u16 *cursor, u16 *count);
bool8 CtrPokenavRibbons_Summary(u16 *selected, u16 *normal, u16 *gift, u16 *giftStart, bool8 *expanded);
bool8 CtrRegionMap_Cursor(s16 *x, s16 *y, bool8 *zoomed, bool8 *moving);
bool8 CtrMonMarkings_Menu(s8 *cursor, s16 *x, s16 *y);
bool8 CtrPokenavCondition_Marking(void);
void CtrPokenavMenu_SetCursor(int cursor);
void CtrPokenavList_SetSelected(u16 selected);
void CtrPokenavMatchCall_SetOption(u16 cursor);
void CtrMonMarkings_SetCursor(s8 cursor);
bool8 CtrStorage_IsOpen(void);
int CtrTitleScreen_RayquazaBg(void);
void CtrStorage_Tap(s16 x, s16 y);
void CtrSummary_Tap(s16 x, s16 y);
/* item_menu.c: the bag's touches, in pixels of its picture. */
enum { BAG_TOUCH_DOWN, BAG_TOUCH_MOVE, BAG_TOUCH_UP, BAG_TOUCH_CANCEL };
void CtrBag_Touch(u8 phase, s16 x, s16 y);
bool8 CtrBag_Close(void);
/* pokedex.c: the Pokédex's touches, in pixels of its picture, as the bag's. */
bool8 CtrPokedex_IsOpen(void);
void CtrPokedex_Touch(u8 phase, s16 x, s16 y);
bool8 CtrPokedex_Close(bool8 leave);
void SetPokemonCryStereo(u32 val);
extern const struct PokedexEntry gPokedexEntries[];

/* start_menu.c's MENU_ACTION_* (the enum is private to that file). */
enum { START_POKEDEX, START_POKEMON, START_BAG, START_POKENAV, START_NONE = 0xFF };

#define W CTR_BOTTOM_WIDTH
#define H CTR_BOTTOM_HEIGHT
/* The content area left of the button column. */
#define CW 240
#define COL_X CW


#endif /* BOTTOM_UI_INTERNAL_H */
