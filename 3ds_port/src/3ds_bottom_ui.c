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
 * The column is the port's own design (an emerald rail, plates, a focus
 * ring; see "The column's look"), with the game's item icons on it and two
 * new pictures, the RUN shoe and the Y badge (3ds_bottom_art.h, from
 * assets/artwork/bottom). Everything else is the game's own graphics,
 * decoded from the same RomFS files the game loads: the party menu
 * background, the battle text box frames, the Hoenn region map, the trainer
 * card, mon/item/type/status icons, front pictures, the bag sprite, the
 * window frames and the game's fonts. Texts are the game's strings where it
 * has them. X walks the column with the buttons (ProcessKeys); the 3DS's Y
 * is SELECT (3ds_game_bridge.c).
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


#include "bottom_ui/bottom_ui_internal.h"
#include "bottom_ui/bottom_ui_draw.c"
#include "bottom_ui/bottom_ui_state.c"
#include "bottom_ui/bottom_ui_screens.c"
#include "bottom_ui/bottom_ui_touch.c"

/* ------------------------------------------------------------------------ */
/* Entry points                                                             */
/* ------------------------------------------------------------------------ */

/* field_player_avatar.c: B walks instead of running while RUN is on. */
bool8 CtrPlayer_RunAlways(void)
{
    return CtrSettings_RunAlways();
}

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
    return FALSE;
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

    sFrames = frames;
    mode = CurrentMode();
    pressed = ProcessTouch(mode);
    ProcessKeys(mode);
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
    /* The focus ring, or a press on the column: only those buttons. */
    if (!sForceRedraw && sShown.mode != 0xFF && !CtrVideo_BottomWhole() && memcmp(&sState, &sShown, sizeof(sState)) != 0)
    {
        Rect r;

        bool8 dirty = FocusDirtyRect(&sState, &sShown, &r);

        /* The column's hits reach 2px past its edge, the drawing does not. */
        if (dirty && CtrVideo_BottomInUse() && r.x0 < CW)
            r.x0 = CW;
        if (dirty && r.x0 < r.x1)
        {
            RenderPart(&sState, r.x0, r.y0, r.x1, r.y1);
            sShown = sState;
            BottomProfile(frames, mode, ticks, 3);
            return;
        }
    }
    /* A press, a cursor, an HP bar, an option's value: only the part that
     * changes is drawn. */
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
    if (mode == MODE_BATTLE_INFO)
        WarmBattleMenus();
    BottomProfile(frames, mode, ticks, 0);
}
