/*
 * The bottom screen, game side: what it shows and what a touch does.
 *
 * It replaces the START menu. A column of buttons on the right holds every
 * entry the menu had, plus the Hoenn map: MAP (the default), POKéMON, BAG,
 * the trainer card, POKéDEX, POKéNAV, SAVE and OPTION. The 240x240 area on
 * the left shows the chosen one; everything happens here while the top screen
 * keeps the world. The PokéNav is the game's own, run as it is: the
 * compositor draws its screens into that area (CtrVideo_BottomInUse) and a
 * tap on them becomes the buttons the PokéNav reads. The PC's boxes are
 * drawn there too, and a tap on them acts in the game directly
 * (pokemon_storage_system.c, CtrStorage_Tap). So is the bag (item_menu.c,
 * CtrBag_Touch): BAG opens the game's own, left of the column; opened from a
 * battle, a shop or the PC it has the whole screen. And the Pokédex
 * (pokedex.c, CtrPokedex_Touch), left of the column.
 *
 * Map, trainer card, summary, save and options are drawn and run here
 * directly. Switching mons, giving items or field moves need the game's own
 * logic, so the game's party menu runs *hidden*:
 * the top screen holds its last frame (CtrVideo_HoldTop), the menu is driven
 * by button presses fed through Platform_GetKeyInput, and what it shows (its
 * submenu entries, messages, yes/no questions) is mirrored here as buttons.
 * The player never sees a cursor move; the game still decides everything.
 *
 * Nothing here is new artwork. Every panel is built from the game's own
 * graphics, decoded from the same RomFS files the game loads: the party menu
 * background and slot tilemaps, the battle text box frames, the Hoenn region
 * map, the trainer card, mon/item/type/status icons, front pictures, the bag
 * sprite, the window frames and the game's fonts. Texts are the game's
 * strings where it has them.
 *
 * Cost model, chosen for an Old 3DS whose frame the top screen already fills:
 *   - every frame: a snapshot of the few values on screen (a memcmp of a few
 *     hundred bytes) and the touch state; no drawing;
 *   - when the snapshot changes: the canvas is recomposed by the CPU from
 *     pre-rendered backgrounds (a memcpy) plus the live parts, and copied to
 *     the framebuffer;
 *   - RomFS is never read inside a redraw: icons and pictures are fetched one
 *     per frame beforehand, everything else is decoded once at boot;
 *   - icon animation: only the icon rectangles are restored and redrawn.
 * No GPU time, no VRAM, no linear memory.
 */

#include <stdio.h>
#include <stdlib.h>
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
#include "constants/songs.h"
#include "constants/trainers.h"
#include "port_platform.h"

#include "3ds_data.h"
#include "3ds_bottom.h"
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


#include "bottom_ui/bottom_ui_draw.c"
#include "bottom_ui/bottom_ui_state.c"
#include "bottom_ui/bottom_ui_screens.c"

/* ------------------------------------------------------------------------ */
/* Redraw and present                                                       */
/* ------------------------------------------------------------------------ */

static void BuildBackgroundCaches(void)
{
    sOX = 0;
    /* The 240px view with the column beside it, and the whole screen. */
    sDst = sCache[CACHE_MENU];
    DrawPartyBackground(CW / 8, H / 8);
    DrawColumnBackground();
    sDst = sCache[CACHE_WIDE];
    DrawPartyBackground(W / 8, H / 8);
    BuildMapCache();
    sDst = sCanvas;
}

static void Render(const ViewState *s)
{
    ResolveFonts();
    sHitCount = 0;
    sAnimCount = 0;
    sDst = sCanvas;
    sOX = 0;

    if (s->mode == MODE_OFF)
    {
        memset(sCanvas, 0, sizeof(sCanvas));
    }
    else if (s->mode >= MODE_BATTLE_INFO)
    {
        CopyCache(CACHE_WIDE);
        DrawBattleHeader(s);
        if (s->mode == MODE_BATTLE_ACTION)
            DrawBattleActions(s);
        else if (s->mode == MODE_BATTLE_MOVE)
            DrawBattleMoves(s);
        else if (s->mode == MODE_BATTLE_TARGET)
            DrawBattleTarget(s);
        else
            DrawBattleInfo(s);
    }
    else
    {
        /* In battle the bag and the party menu have the whole screen. */
        bool8 column = !s->inBattle && s->bagView != BAG_VIEW_WHOLE;

        if (s->screen == SCR_MAP && column)
            CopyCache(CACHE_MAP);
        else if (s->screen == SCR_CARD && column)
        {
            BuildCardCache(s->stars > 4 ? 4 : s->stars, s->gender);
            CopyCache(sCache[CACHE_CARD] ? CACHE_CARD : CACHE_MENU);
        }
        else
            CopyCache(column ? CACHE_MENU : CACHE_WIDE);
        sOX = column ? 0 : (W - CW) / 2;
        switch (s->screen)
        {
        case SCR_MAP: DrawRegionMap(s); break;
        case SCR_POKEMON:
        case SCR_BAG:
        case SCR_POKEDEX:
            /* The game's party menu, bag and Pokédex are drawn there by the compositor;
             * black until they are, as they fade in from black, and while
             * the field is on its way to opening them (OpenAsked). */
            FillRect(0, 0, CW, H, 0);
            break;
        case SCR_CARD: DrawTrainerCard(s); break;
        case SCR_SAVE: DrawSave(s); break;
        case SCR_OPTION: DrawOptions(s); break;
        }
        sOX = 0;
        if (column)
            DrawColumn(s);
        else if (s->bagView == BAG_VIEW_WHOLE)
            memset(sCanvas, 0, sizeof(sCanvas));
    }
    DrawAnimIcons();
    /* Left of the column is the PokéNav's while the compositor draws it, and
     * the whole screen the boxes'. */
    if (!ClipIsFull())
        CtrBottom_BlitRect(sCanvas, sClipX0, sClipY0, sClipX1, sClipY1);
    else if (!CtrVideo_BottomWhole())
        CtrBottom_Blit(sCanvas, CtrVideo_BottomInUse() ? CW : 0, W);
}

/* ------------------------------------------------------------------------ */
/* Partial redraws                                                          */
/* ------------------------------------------------------------------------ */

typedef struct { int x0, y0, x1, y1; } Rect;

static void RectInit(Rect *r)
{
    r->x0 = r->y0 = W;
    r->x1 = r->y1 = 0;
}

static void RectAdd(Rect *r, int x0, int y0, int x1, int y1)
{
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}

/* A button's rectangle, as the last redraw laid it out, with a little margin
 * for its shadow; every hit of the id counts. FALSE when there is none. */
static bool8 RectAddHitOf(Rect *r, u8 id)
{
    bool8 found = FALSE;

    for (int i = 0; i < sHitCount; ++i)
        if (sHits[i].id == id)
        {
            RectAdd(r, sHits[i].x - 2, sHits[i].y - 2, sHits[i].x + sHits[i].w + 2, sHits[i].y + sHits[i].h + 2);
            found = TRUE;
        }
    return found;
}

static bool8 RectAddHit(Rect *r, u8 id)
{
    if (id == HIT_NONE)
        return TRUE;
    /* An option row is lit whole, whichever of its two halves is touched. */
    if (id >= HIT_OPTION && id < HIT_OPTION + 2 * HIT_OPTION_BACK)
    {
        u8 row = (id - HIT_OPTION) % HIT_OPTION_BACK;

        return RectAddHitOf(r, HIT_OPTION + row) && RectAddHitOf(r, HIT_OPTION + HIT_OPTION_BACK + row);
    }
    return RectAddHitOf(r, id);
}

/* The pixels the header's panel of a battler covers, and a party slot's on
 * the battle info screen: where an HP bar, its numbers and the status icon
 * change. Positions from DrawBattleHeader and DrawBattleInfo. */
static void RectAddBattler(Rect *r, const ViewState *s, int i)
{
    if (s->isDouble)
    {
        int x = i & 1 ? 170 : 10, y = i & 2 ? 28 : 8;

        RectAdd(r, x + 66, y - 2, x + 150, y + 18);
    }
    else
    {
        int x = i == 0 ? 10 : 170;

        RectAdd(r, x - 2, 6, x + 148, 54);
    }
}

static void RectAddPartySlot(Rect *r, int i)
{
    int x = 12 + (i % 3) * 100, y = 100 + (i / 3) * 72;

    RectAdd(r, x - 2, y - 2, x + 98, y + 34);
}

/* The clip a redraw can be limited to, when the new view differs from the
 * shown one only in things that touch one part of the screen: the button lit
 * by a press or the battle cursor, an HP bar and its status, an option's
 * value. FALSE when anything else differs, or the part cannot be told. */
static bool8 DirtyRect(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;
    bool8 battle = now->mode >= MODE_BATTLE_INFO;

    if (now->mode != shown->mode || (now->mode != MODE_FIELD && !battle) || shown->screen != now->screen
     || (now->mode == MODE_FIELD && now->screen != SCR_OPTION))
        return FALSE;
    probe = *now;
    probe.pressed = shown->pressed;
    if (now->mode == MODE_BATTLE_ACTION || now->mode == MODE_BATTLE_MOVE)
        probe.cursor = shown->cursor;
    for (int i = 0; i < MAX_BATTLERS_COUNT; ++i)
    {
        probe.battlers[i].hp = shown->battlers[i].hp;
        probe.battlers[i].ailment = shown->battlers[i].ailment;
    }
    if (now->mode == MODE_BATTLE_INFO)
        for (int i = 0; i < PARTY_SIZE; ++i)
        {
            probe.party[i].hp = shown->party[i].hp;
            probe.party[i].ailment = shown->party[i].ailment;
        }
    if (now->mode == MODE_FIELD)
        for (int i = 0; i < OPTION_ROWS; ++i)
            if (i != OPT_FRAME && i != OPT_VOXEL)
                probe.options[i] = shown->options[i];
    if (memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;

    RectInit(r);
    if (now->pressed != shown->pressed && !(RectAddHit(r, now->pressed) && RectAddHit(r, shown->pressed)))
        return FALSE;
    if (now->cursor != shown->cursor)
    {
        for (int k = 0; k < 2; ++k)
        {
            u8 c = k ? now->cursor : shown->cursor, id;

            if (now->mode == MODE_BATTLE_ACTION)
                id = HIT_ACTION + c;
            else
                id = c == MAX_MON_MOVES ? HIT_CANCEL : HIT_MOVE + c;
            if (!RectAddHit(r, id))
                return FALSE;
        }
    }
    for (int i = 0; i < MAX_BATTLERS_COUNT; ++i)
        if (now->battlers[i].hp != shown->battlers[i].hp || now->battlers[i].ailment != shown->battlers[i].ailment)
            RectAddBattler(r, now, i);
    for (int i = 0; i < PARTY_SIZE; ++i)
        if (now->party[i].hp != shown->party[i].hp || now->party[i].ailment != shown->party[i].ailment)
            RectAddPartySlot(r, i);
    for (int i = 0; now->mode == MODE_FIELD && i < OPTION_ROWS; ++i)
        if (now->options[i] != shown->options[i] && !RectAddHit(r, HIT_OPTION + i))
            return FALSE;
    return r->x0 < r->x1 && r->y0 < r->y1;
}

static void RenderPart(const ViewState *s, int x0, int y0, int x1, int y1);

/*
 * The options list dragged: the rows already on the screen move with the
 * finger as pixels, and only what that uncovers is drawn - the strip at the
 * edge, the bars of the party-menu pattern at the top and bottom, which do not
 * scroll, and the scroll bar's column. FALSE when anything else differs.
 */
static bool8 ScrollOptions(const ViewState *now, const ViewState *shown)
{
    (void)now;
    (void)shown;
    /* Clean full redraw avoids slice tearing and row smearing during scrolling */
    return FALSE;
}

/* Draws one part of the screen. The canvas outside it keeps what is shown. */
static void RenderPart(const ViewState *s, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1)
        return;
    /* An icon the part touches is redrawn whole: the part grows to hold it. */
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < sAnimCount; ++i)
        {
            int ix0, iy0, ix1, iy1;

            IconRect(&sAnim[i], &ix0, &iy0, &ix1, &iy1);
            if (RectsMeet(ix0, iy0, ix1, iy1, x0, y0, x1, y1))
            {
                if (ix0 < x0) x0 = ix0;
                if (iy0 < y0) y0 = iy0;
                if (ix1 > x1) x1 = ix1;
                if (iy1 > y1) y1 = iy1;
            }
        }
    sClipX0 = x0;
    sClipY0 = y0;
    sClipX1 = x1;
    sClipY1 = y1;
    Render(s);
    sClipX0 = sClipY0 = 0;
    sClipX1 = W;
    sClipY1 = H;
}


#include "bottom_ui/bottom_ui_touch.c"

/* ------------------------------------------------------------------------ */
/* Entry points                                                             */
/* ------------------------------------------------------------------------ */

void CtrBottom_Init(void)
{
    uint64_t start = CtrPlatform_Ticks();

    LoadResources();
    if (sRes.ready)
        BuildBackgroundCaches();
    sShown.mode = 0xFF;
    CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: resources %s in %.1f ms", sRes.ready ? "ready" : "MISSING",
                 CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
}

/* The PC's boxes are about to open (pokemon_storage_system.c): the top keeps
 * the world, fade included, until the field is back. */
void CtrBottom_KeepWorld(void)
{
    BeginSession(FALSE);
    CtrVideo_HoldTop(TRUE);
}

/*
 * Walking moves the player's mark on the MAP screen, and nothing else on it:
 * every move used to redraw the whole screen - the column's buttons and
 * labels, the name box - 5-15 ms on an Old 3DS, in the frame the move fell
 * in. Now the mark's old square is restored from the map's cache, the mark
 * drawn on its new one, and those two squares sent to the screen.
 */
static bool8 MapCursorOnlyMoved(const ViewState *now, const ViewState *shown)
{
    ViewState moved;

    if (now->screen != SCR_MAP || now->mode == MODE_OFF || now->mode >= MODE_BATTLE_INFO
     || now->inBattle || now->bagView == BAG_VIEW_WHOLE
     || now->mapsec == MAPSEC_NONE || shown->mapsec == MAPSEC_NONE
     || (now->cursorX == shown->cursorX && now->cursorY == shown->cursorY
         && now->mapsec == shown->mapsec))
        return FALSE;
    moved = *now;
    moved.cursorX = shown->cursorX;
    moved.cursorY = shown->cursorY;
    moved.mapsec = shown->mapsec;
    return memcmp(&moved, shown, sizeof(moved)) == 0;
}

/* The mark's 2x2 tiles at a cursor cell, clipped to the screen. */
static void MarkRect(u8 cx, u8 cy, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = MAP_ORIGIN_X + cx * 8 - 4;
    *y0 = MAP_ORIGIN_Y + cy * 8 - 4;
    *x1 = *x0 + 16;
    *y1 = *y0 + 16;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > W) *x1 = W;
    if (*y1 > H) *y1 = H;
}

static bool8 RectsMeet(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1)
{
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

/* False, with nothing touched, where a whole redraw is needed after all. */
static bool8 MoveMapCursor(const ViewState *from, const ViewState *to)
{
    const u8 *icon = sRes.playerIcon[to->gender];
    int r[2][4];

    if (sCache[CACHE_MAP] == NULL || icon == NULL || CtrVideo_BottomInUse() || CtrVideo_BottomWhole())
        return FALSE;
    MarkRect(from->cursorX, from->cursorY, &r[0][0], &r[0][1], &r[0][2], &r[0][3]);
    MarkRect(to->cursorX, to->cursorY, &r[1][0], &r[1][1], &r[1][2], &r[1][3]);
    /* An animated icon keeps what lies under it (DrawAnimIcons): over the
     * mark it would put an old picture back. */
    for (int i = 0; i < sAnimCount; ++i)
    {
        int x0, y0, x1, y1;

        IconRect(&sAnim[i], &x0, &y0, &x1, &y1);
        for (int k = 0; k < 2; ++k)
            if (RectsMeet(x0, y0, x1, y1, r[k][0], r[k][1], r[k][2], r[k][3]))
                return FALSE;
    }
    /* The old square as the map cache has it (the canvas is its layout). */
    for (int x = r[0][0]; x < r[0][2]; ++x)
        memcpy(sCanvas + x * H + (H - r[0][3]), sCache[CACHE_MAP] + x * H + (H - r[0][3]),
               (size_t)(r[0][3] - r[0][1]) * sizeof(u16));
    sDst = sCanvas;
    sOX = 0;
    DrawSprite(icon, 2, 2, MAP_ORIGIN_X + to->cursorX * 8 - 4, MAP_ORIGIN_Y + to->cursorY * 8 - 4,
               sRes.playerIconPal[to->gender].c);
    /* Crossing a region changes only the label below the map. Restore and
     * redraw that rectangle instead of the map and the eight-button column. */
    if (to->mapsec != from->mapsec && to->pickMapsec == MAPSEC_NONE)
    {
        for (int x = 8; x < 232; ++x)
            memcpy(sCanvas + x * H + (H - 228), sCache[CACHE_MAP] + x * H + (H - 228),
                   40 * sizeof(u16));
        ResolveFonts();
        DrawRegionName(to);
        CtrBottom_BlitRect(sCanvas, 8, 188, 232, 228);
    }
    /* A picked cell's cursor is drawn over the mark, as Render does. */
    if (to->pickMapsec != MAPSEC_NONE && sRes.cursorTiles)
        DrawSprite(sRes.cursorTiles, 2, 2, MAP_ORIGIN_X + to->pickX * 8 - 4,
                   MAP_ORIGIN_Y + to->pickY * 8 - 4, sRes.cursorPal.c);
    for (int k = 0; k < 2; ++k)
        CtrBottom_BlitRect(sCanvas, r[k][0], r[k][1], r[k][2], r[k][3]);
    return TRUE;
}

static bool8 PartialRedraw(const ViewState *now)
{
    Rect r;

    if (DirtyRect(now, &sShown, &r))
    {
        RenderPart(now, r.x0, r.y0, r.x1, r.y1);
        return TRUE;
    }
    return ScrollOptions(now, &sShown);
}

static void BottomProfile(u32 frame, u8 mode, const uint64_t ticks[3], unsigned kind)
{
    static u32 last;
    uint64_t end = CtrPlatform_Ticks();
    float total = CtrPlatform_TickMs(end - ticks[0]);
    if (total < 2.0f || (last && frame - last < 120)) return;
    last = frame;
    CtrLog_Write(CTR_LOG_VIDEO,
                 "bottom slice mode=%u kind=%u total=%.2f control=%.2f snapshot=%.2f draw=%.2f ms",
                 mode, kind, total, CtrPlatform_TickMs(ticks[1] - ticks[0]),
                 CtrPlatform_TickMs(ticks[2] - ticks[1]), CtrPlatform_TickMs(end - ticks[2]));
}

void CtrBottom_Frame(void)
{
    static u32 frames;
    static bool8 iconsPending, held;
    u8 mode, pressed;
    bool8 hold;
    uint64_t ticks[3];

    if (!sRes.ready)
        return;
    ++frames;
    ticks[0] = CtrPlatform_Ticks();
    sAsked = sAsk;
    sAsk.kind = ASK_NONE;
    if (sAsked.kind == ASK_NONE)
        sBattleTap = HIT_NONE;

    mode = CurrentMode();
    pressed = ProcessTouch(mode);
    OpenAsked(mode);
    RunPlan();
    RunNav(mode);

    /* The hidden menus: the top screen keeps the world meanwhile. */
    hold = UpdateSession(mode, sPlan.kind != PLAN_NONE);
    if (hold != held)
    {
        CtrVideo_HoldTop(hold);
        held = hold;
    }
    if (hold && mode != MODE_POKENAV && mode != MODE_STORAGE && mode != MODE_PARTY_MENU && !BagShown(mode)
     && !DexShown(mode))   /* on show */
        FastForward();

    /* The PokéNav's last frame stays left of the column until repainted. */
    {
        static bool navDrawn;
        bool drawn = CtrVideo_BottomInUse();

        if (drawn != navDrawn)
            sForceRedraw = TRUE;
        navDrawn = drawn;
    }
    ticks[1] = CtrPlatform_Ticks();
    Snapshot(&sState, mode, pressed);

    /* One RomFS read at most, and a single redraw once the icons are in. */
    if (Prefetch(&sState))
        iconsPending = TRUE;
    else if (iconsPending)
    {
        iconsPending = FALSE;
        sForceRedraw = TRUE;
    }

    ticks[2] = CtrPlatform_Ticks();
    if (!sForceRedraw && MapCursorOnlyMoved(&sState, &sShown) && MoveMapCursor(&sShown, &sState))
    {
        sShown = sState;
        BottomProfile(frames, mode, ticks, 1);
        return;
    }
    /* A press, a cursor, an HP bar, an option's value, a drag of the options
     * list: only the part that changes is drawn. */
    if (!sForceRedraw && !CtrVideo_BottomInUse() && !CtrVideo_BottomWhole() && sShown.mode != 0xFF
     && memcmp(&sState, &sShown, sizeof(sState)) != 0 && PartialRedraw(&sState))
    {
        sShown = sState;
        BottomProfile(frames, mode, ticks, 3);
        return;
    }
    if (sForceRedraw || memcmp(&sState, &sShown, sizeof(sState)) != 0)
    {
        uint64_t start = CtrPlatform_Ticks();
        static float peak;
        float ms;

        if (sState.mode != sShown.mode)
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: mode %u", sState.mode);
        sShown = sState;
        sForceRedraw = FALSE;
        sAnimFrame = (frames >> 4) & 1;
        Render(&sShown);
        ms = CtrPlatform_TickMs(CtrPlatform_Ticks() - start);
        if (ms > peak + 0.25f)
        {
            peak = ms;
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: redraw peak %.2f ms (mode %u screen %u)", ms, sShown.mode,
                         sShown.screen);
        }
        BottomProfile(frames, mode, ticks, 2);
        return;
    }
    if (sAnimCount && (u8)((frames >> 4) & 1) != sAnimFrame)
    {
        sAnimFrame = (frames >> 4) & 1;
        AnimateIcons();
    }
    BottomProfile(frames, mode, ticks, 0);
}
