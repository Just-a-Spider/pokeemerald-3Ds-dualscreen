/* ------------------------------------------------------------------------ */
/* View state                                                               */
/* ------------------------------------------------------------------------ */

enum
{
    MODE_OFF,
    MODE_FIELD,
    MODE_PARTY_MENU,
    MODE_BAG_MENU,
    /* The PokéNav, drawn by the compositor left of the column. */
    MODE_POKENAV,
    /* The PC's boxes, drawn by the compositor over the whole screen. */
    MODE_STORAGE,
    /* The game's Pokédex, drawn by the compositor left of the column. */
    MODE_POKEDEX,
    MODE_BATTLE_INFO,
    MODE_BATTLE_ACTION,
    MODE_BATTLE_MOVE,
    MODE_BATTLE_TARGET,
};

/* What the lower panel of the party and bag views offers. */
enum
{
    PANEL_NONE,
    PANEL_HINT,       /* the menu waits for a mon or an item: CANCEL */
    PANEL_ACTIONS,    /* the game's submenu, as buttons */
    PANEL_MESSAGE,    /* a message: tap to go on */
    PANEL_YESNO,      /* a question */
    PANEL_QUANTITY,   /* how many: up, down, OK, CANCEL */
};

#define MAX_MENU_ITEMS 8

typedef struct
{
    u16 species;      /* SPECIES_NONE: empty slot */
    u16 iconSpecies;
    u8 deoxys, isEgg, level, gender, ailment, fainted;
    u16 hp, maxHp;
    u8 nick[POKEMON_NAME_LENGTH + 2];
} MonView;

/* Everything a redraw reads. Zeroed before every snapshot: memcmp-safe. */
typedef struct
{
    u8 mode, screen, pressed, gender, inBattle;
    u8 enabled;                       /* bit per column screen */
    /* Party. */
    s8 partyCursor;
    MonView party[PARTY_SIZE];
    /* The lower panel: a game menu, a message or a question. */
    u8 panel, menuCount, menuCols, menuCursor;
    const u8 *menuNames[MAX_MENU_ITEMS];
    const u8 *message;
    /* Summary. */
    s8 summary;
    u16 stats[6], moves[MAX_MON_MOVES];
    u8 pp[MAX_MON_MOVES], maxPp[MAX_MON_MOVES], nature, ability, types[2];
    u16 heldItem;
    /* Region map. */
    u8 mapsec, cursorX, cursorY, pickMapsec, pickX, pickY;
    /* Bag: which of the game's is on show, BAG_VIEW_*. */
    u8 bagView;
    /* Trainer card. */
    u8 name[PLAYER_NAME_LENGTH + 1];
    u8 hasDex, stars, badges, minutes;
    u16 id, dex, hours;
    u32 money;
    /* Save and options. */
    u8 saveStep, canSave;
    u8 options[OPTION_ROWS];
    /* The OPTIONS page on show (CTR_EXTRAS_*) and the values of its extras,
     * in table order. */
    u8 optPage, optSub;
    u16 extras[24];
    /* The column's Y and RUN: the registered item; bit 0 running is the
     * default, bit 1 the player has the shoes. */
    u16 registered;
    u8 run;
    /* The keyboard focus (X): the column item, or FOCUS_NONE; inside the
     * options, the option; the ring's blink frame. */
    u8 focus, optFocus, blink;
    /* Battle: the cursor is 4 on the quick ball (actions) or CANCEL (moves). */
    u8 isDouble, safari, cursor, battler;
    u16 quickBall, quickBallCount;    /* the ball a quick throw would use, ITEM_NONE if none */
    u8 safariBalls;
    u8 partyBalls[PARTY_SIZE];        /* PARTY_BALL_*, as the game's party indicator */
    struct ChooseMoveStruct moves4;
    u8 text[96];
} ViewState;

/* Which of the game's bag is on show. */
enum { BAG_VIEW_NONE, BAG_VIEW_SECTION, BAG_VIEW_WHOLE };

static bool8 BagShown(u8 mode)
{
    return mode == MODE_BAG_MENU;
}

/* Whether the game's own Pokédex is what the area shows. */
static bool8 DexShown(u8 mode)
{
    return mode == MODE_POKEDEX;
}

static ViewState sState, sShown;
static bool8 sForceRedraw = TRUE;
static bool8 sInGame;
static u8 sScreen = SCR_MAP;
/*
 * The options in two columns of six: the game's own on the left in its order,
 * the port's on the right. A build without the FPS counter or the voxel
 * renderer has no cell for them; with the voxel overworld off its camera,
 * blur and battle cells stay where they are, greyed.
 */
#define OPT_CELL_W 112
#define OPT_CELL_H 32

static bool8 OptionExists(int row)
{
    return row < OPTION_SHOWN && !(row == OPT_FPS && !CTR_SHOW_FPS);
}

static bool8 OptionLive(int row, bool8 voxel)
{
    return OptionExists(row) && (voxel || row < OPT_VOXEL_PITCH);
}

/* OPTIONS has pages (3ds_extras.h) once any extra exists: a tab strip at the
 * top and the cells under it, rows a little closer. */
static u8 sOptPage;
/* The screen of the tab on show (CTR_EXTRAS_SCREEN). */
static u8 sOptSub;
#define CELLS_PER_PAGE 12

static bool8 OptionPages(void)
{
    return gCtrExtraCount > 0;
}

#define TAB_Y 2
#define TAB_H 20

static void OptionCell(int row, int *x, int *y)
{
    bool8 right = row >= OPT_FPS;

    *x = right ? 124 : 4;
    *y = OptionPages() ? TAB_Y + TAB_H + 4 + (right ? row - OPT_FPS : row) * 36
                       : 4 + (right ? row - OPT_FPS : row) * 40;
}

/* The extra on a page's cell `row` (filled column by column, six to a
 * column, as the options are), or NULL. */
/* How many screens a tab's extras take. */
static unsigned PageSubs(unsigned page)
{
    unsigned screens = 1;

    for (unsigned i = 0; i < gCtrExtraCount; ++i)
        if (CTR_EXTRAS_TAB(gCtrExtras[i].page) == page && (gCtrExtras[i].page >> 4) + 1u > screens)
            screens = (gCtrExtras[i].page >> 4) + 1;
    return screens;
}

static const CtrExtra *PageExtra(unsigned page, unsigned row)
{
    unsigned screen = CTR_EXTRAS_SCREEN(page, page == sOptPage ? sOptSub : 0);

    if (row >= CELLS_PER_PAGE)
        return NULL;
    for (unsigned i = 0, n = 0; i < gCtrExtraCount; ++i)
        if (gCtrExtras[i].page == screen && n++ == row)
            return &gCtrExtras[i];
    return NULL;
}

/* What a cell shows, for the redraw to notice a change: its value, or for
 * one with its own text, a sum of that text. */
static u16 ExtraShownValue(const CtrExtra *extra)
{
    u16 sum = 0;

    if (!extra->text)
        return extra->step ? CtrSettings_GetInt(extra->key, extra->fallback) : CtrExtras_Value(extra);
    for (const u8 *c = extra->text(); *c != EOS; ++c)
        sum = sum * 31 + *c;
    return sum;
}

/* The column's keyboard focus (X), and where it is inside a screen. */
#define FOCUS_NONE 0xFF
enum { INSIDE_NONE, INSIDE_OPTIONS, INSIDE_SAVE };
static u8 sFocus = FOCUS_NONE, sInside, sOptFocus;
/* Leaving the focus, the buttons stay the column's until they are let go:
 * the B or A that left it must not reach the game. */
static bool8 sSwallow;
static u32 sFrames;

static u8 sAnimFrame;

/* Per screen state, kept while another screen is shown. */
#if 0 /* the old party menu's */
static s8 sPartyTapped = -1;
static s8 sSummary = -1;
#endif
static u8 sPickMapsec = MAPSEC_NONE, sPickX, sPickY;
static u8 sSaveStep;
static u8 sSaveMessage[96];

enum { SAVE_ASK, SAVE_OVERWRITE, SAVE_DONE };

/* Learned once: the party menu's running callback (static in party_menu.c). */
static MainCallback sPartyMenuCallback;

/* ------------------------------------------------------------------------ */
/* Hit zones                                                                */
/* ------------------------------------------------------------------------ */

enum
{
    HIT_NONE = 0xFF,
    HIT_COLUMN = 0x10,     /* + screen */
    HIT_SLOT = 0x20,       /* + party slot */
    HIT_CANCEL = 0x30,
    HIT_OK,
    HIT_YES,
    HIT_NO,
    HIT_PREV,
    HIT_NEXT,
    HIT_BACK,
    HIT_UP,
    HIT_DOWN,
    HIT_PANEL,             /* a message: anywhere on it */
    HIT_ROW = 0x50,        /* + visible list row */
    HIT_ACTION = 0x60,     /* + action cursor */
    HIT_MOVE = 0x70,       /* + move slot */
    HIT_TARGET_LEFT = 0x80,
    HIT_TARGET_RIGHT,
    HIT_TARGET_OK,
    HIT_QUICK_BALL,        /* throw the last ball used */
    HIT_MAP = 0x90,
    HIT_MENU = 0xB0,       /* + game menu entry */
    HIT_OPTION = 0xC0,     /* + option row; +HIT_OPTION_BACK for the left arrow */
    HIT_PAGE = 0xE0,       /* + OPTIONS page tab */
};
/* More than there are option rows, so a row and a left arrow never share an id. */
#define HIT_OPTION_BACK 16

typedef struct { s16 x, y, w, h; u8 id; } Hit;
static Hit sHits[64];
static u8 sHitCount;

static void AddHit(int x, int y, int w, int h, u8 id)
{
    if (sHitCount < ARRAY_COUNT(sHits))
        sHits[sHitCount++] = (Hit){x + sOX, y, w, h, id};
}

static u8 HitTest(int x, int y)
{
    for (int i = sHitCount - 1; i >= 0; --i)
        if (x >= sHits[i].x && y >= sHits[i].y && x < sHits[i].x + sHits[i].w && y < sHits[i].y + sHits[i].h)
            return sHits[i].id;
    return HIT_NONE;
}

static void DrawLabelButtonFont(const Font *font, int x, int y, int wt, int ht, const u8 *label, bool8 on,
                                bool8 enabled, u8 id)
{
    DrawBoxEx(BOX_MENU, x, y, wt, ht, on);
    DrawStrCentered(font, label, x + wt * 4, y + ht * 4 - font->height / 2, enabled ? LABEL_FG(on) : TXT_LIGHT,
                    enabled ? LABEL_SH(on) : TXT_WHITE);
    if (enabled)
        AddHit(x, y, wt * 8, ht * 8, id);
}

static void DrawLabelButton(int x, int y, int wt, int ht, const u8 *label, bool8 on, bool8 enabled, u8 id)
{
    DrawLabelButtonFont(&sNormal, x, y, wt, ht, label, on, enabled, id);
}

/* ------------------------------------------------------------------------ */
/* Animated icons                                                           */
/* ------------------------------------------------------------------------ */

typedef struct { s16 x, y; u16 iconSpecies; u8 deoxys, still; } AnimIcon;
static AnimIcon sAnim[8];
static u8 sAnimCount;
/* What was under each animated icon, to redraw its frames in place. */
static u16 sUnder[8][32 * 32];

static void IconRect(const AnimIcon *icon, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = icon->x < 0 ? 0 : icon->x;
    *y0 = icon->y < 0 ? 0 : icon->y;
    *x1 = icon->x + 32 > W ? W : icon->x + 32;
    *y1 = icon->y + 32 > H ? H : icon->y + 32;
}

/* Icons store screen coordinates: draw them untranslated. */
static void DrawIconFrame(const AnimIcon *icon)
{
    const u8 *tiles = MonIcon(icon->iconSpecies, icon->deoxys);
    u8 pal = gMonIconPaletteIndices[icon->iconSpecies];
    int ox = sOX;

    sOX = 0;
    if (tiles && pal < 3)
        DrawSprite(tiles + (icon->still ? 0 : sAnimFrame) * 512, 4, 4, icon->x, icon->y, sRes.monIconPal[pal].c);
    sOX = ox;
}

static bool8 RectsMeet(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1);

/* In a clipped redraw an icon outside the clip is left as it is, with what
 * was under it kept from the last time it was drawn. */
static void DrawAnimIcons(void)
{
    for (int i = 0; i < sAnimCount; ++i)
    {
        int x0, y0, x1, y1;

        IconRect(&sAnim[i], &x0, &y0, &x1, &y1);
        if (!ClipIsFull() && !RectsMeet(x0, y0, x1, y1, sClipX0, sClipY0, sClipX1, sClipY1))
            continue;
        for (int x = x0; x < x1; ++x)
            memcpy(sUnder[i] + (x - x0) * 32, sCanvas + x * H + (H - y1), (y1 - y0) * sizeof(u16));
        DrawIconFrame(&sAnim[i]);
    }
}

/* Icon frames only: restore each icon's rectangle and redraw it. */
static void AnimateIcons(void)
{
    for (int i = 0; i < sAnimCount; ++i)
    {
        int x0, y0, x1, y1;

        IconRect(&sAnim[i], &x0, &y0, &x1, &y1);
        if (sAnim[i].still || x0 >= x1 || y0 >= y1)
            continue;
        for (int x = x0; x < x1; ++x)
            memcpy(sCanvas + x * H + (H - y1), sUnder[i] + (x - x0) * 32, (y1 - y0) * sizeof(u16));
        DrawIconFrame(&sAnim[i]);
        CtrBottom_BlitRect(sCanvas, x0, y0, x1, y1);
    }
}

/* ------------------------------------------------------------------------ */
/* Game state                                                               */
/* ------------------------------------------------------------------------ */

#if 0
static bool8 PartyMenuReady(void)
{
    return FuncIsActiveTask(Task_HandleChooseMonInput) && !gPaletteFade.active;
}
#endif

/* The player stands in the field with nothing else going on. */
static bool8 FieldIdle(void)
{
    return gMain.callback2 == CB2_Overworld && !ArePlayerFieldControlsLocked() && !ScriptContext_IsEnabled()
        && gPlayerAvatar.tileTransitionState == T_NOT_MOVING && !gPaletteFade.active && !CtrStartMenu_Busy();
}

static u8 EnabledScreens(void)
{
    u8 mask = (1 << SCR_MAP) | (1 << SCR_BAG) | (1 << SCR_CARD) | (1 << SCR_OPTION);

    if (FlagGet(FLAG_SYS_POKEMON_GET))
        mask |= 1 << SCR_POKEMON;
    if (FlagGet(FLAG_SYS_POKEDEX_GET))
        mask |= 1 << SCR_POKEDEX;
    if (FlagGet(FLAG_SYS_POKENAV_GET) && CtrStartMenu_Available())
        mask |= 1 << SCR_POKENAV;
    if (CtrStartMenu_Available())
        mask |= 1 << SCR_SAVE;
    return mask;
}

/*
 * What the battle controllers wait for. Their input handlers call in here
 * every frame they run (CtrBattleMenu_*Input); last frame's call is what the
 * bottom screen shows.
 */
enum { ASK_NONE, ASK_ACTION, ASK_MOVE, ASK_TARGET };

typedef struct
{
    u8 kind, battler;
} BattleAsk;

static BattleAsk sAsk, sAsked;
static u8 sBattleTap = 0xFF;   /* a tap for the controller to take */
static bool8 sMoveCancel;      /* the move menu's cursor is on CANCEL */
static bool8 sQuickBallTap;    /* the ball button was tapped */
static bool8 sQuickBallFocus;  /* the D-pad is on the ball button (A throws) */

/*
 * The quick ball (battle_controller_player.c, an optional patch in the
 * public tree): the ball R or the ball button would throw now, or
 * ITEM_NONE. Without that patch no battle offers one.
 */
u16 __attribute__((weak)) CtrBattle_QuickBallItem(void)
{
    return ITEM_NONE;
}

static u8 CurrentMode(void)
{
    if (gMain.callback2 == CB2_Overworld)
        sInGame = TRUE;
    if (!sInGame || !gSaveBlock1Ptr || !gSaveBlock2Ptr)
        return MODE_OFF;
    if (CtrPokenav_IsOpen())
        return MODE_POKENAV;
    if (CtrPokedex_IsOpen())
        return MODE_POKEDEX;
    /* Before the boxes: the bag can be opened from them, whole screen too. */
    if (gMain.callback2 == CB2_BagMenuRun && gBagMenu)
        return MODE_BAG_MENU;
    /* A summary opened from the boxes is on the whole screen too. */
    if (CtrStorage_IsOpen() || CtrVideo_BottomWhole())
        return MODE_STORAGE;
    if (FuncIsActiveTask(Task_HandleChooseMonInput))
        sPartyMenuCallback = gMain.callback2;
    if (sPartyMenuCallback && gMain.callback2 == sPartyMenuCallback)
        return MODE_PARTY_MENU;
    if (gMain.inBattle)
    {
        if (gMain.callback2 == BattleMainCB2 && !gPaletteFade.active)
        {
            if (sAsked.kind == ASK_ACTION) return MODE_BATTLE_ACTION;
            if (sAsked.kind == ASK_MOVE) return MODE_BATTLE_MOVE;
            if (sAsked.kind == ASK_TARGET) return MODE_BATTLE_TARGET;
        }
        return MODE_BATTLE_INFO;
    }
    return MODE_FIELD;
}

/* ------------------------------------------------------------------------ */
/* Hidden sessions: the game's menus running under the world              */
/* ------------------------------------------------------------------------ */

static struct
{
    bool8 active, entered, battle;
    u16 frames, away;
} sSession;

static void BeginSession(bool8 battle)
{
    if (!sSession.active)
        CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: hidden menu session (%s)", battle ? "battle" : "field");
    sSession.active = TRUE;
    sSession.entered = FALSE;
    sSession.battle = battle;
    sSession.frames = sSession.away = 0;
}

/*
 * Whether the top screen holds its frame. A session holds from the first
 * press that leads to the menu (the fade out is not shown) through the menu
 * and back until the fade in is over. A screen the session did not expect -
 * an evolution, the move to forget, the fly map - is shown after a moment.
 */
static bool8 UpdateSession(u8 mode, bool8 planRunning)
{
    bool8 inMenu = mode == MODE_PARTY_MENU || mode == MODE_BAG_MENU || mode == MODE_POKENAV
                || mode == MODE_STORAGE || mode == MODE_POKEDEX;
    bool8 home = gMain.callback2 == CB2_Overworld || gMain.callback2 == BattleMainCB2;

    if (inMenu && !sSession.active)
        BeginSession(gMain.inBattle);
    if (!sSession.active)
        return FALSE;
    ++sSession.frames;
    if (inMenu)
    {
        sSession.entered = TRUE;
        sSession.away = 0;
        return TRUE;
    }
    if (home)
    {
        sSession.away = 0;
        if ((sSession.entered || (!planRunning && sSession.frames > 30)) && !gPaletteFade.active)
        {
            sSession.active = FALSE;
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: hidden menu session over");
            return FALSE;
        }
        return TRUE;
    }
    /* A menu's setup and the map reload pass through here too: brief. */
    return ++sSession.away < 40;
}

/* Hidden menus need not wait on their fades and slow text. */
static void FastForward(void)
{
    for (int i = 0; i < 6; ++i)
    {
        if (gPaletteFade.active)
            UpdatePaletteFade();
        RunTextPrinters();
    }
}

/* ------------------------------------------------------------------------ */
/* The battle menus                                                         */
/* ------------------------------------------------------------------------ */

/*
 * The action, move and target menus are the bottom screen's: the controllers
 * do not draw theirs (the top keeps its message box) and read their keys
 * through these, which move the game's cursor in the bottom screen's order
 * and turn a tap into the key that confirms it. What each choice does stays
 * the controller's own code.
 */
bool8 CtrBattleMenu_Active(void)
{
    return sRes.ready && !(gBattleTypeFlags & (BATTLE_TYPE_LINK | BATTLE_TYPE_RECORDED | BATTLE_TYPE_WALLY_TUTORIAL));
}

void CtrBattleMenu_Begin(void)
{
    sBattleTap = HIT_NONE;
    sMoveCancel = FALSE;
    sQuickBallTap = FALSE;
    sQuickBallFocus = FALSE;
}

/* The ball button, once per tap: the action handler throws the ball. */
bool8 CtrBattleMenu_TakeQuickBall(void)
{
    bool8 tapped = sQuickBallTap;

    sQuickBallTap = FALSE;
    return tapped;
}

/* "What will X do?", in the message box: the one thing left on top. */
void CtrBattleMenu_ShowPrompt(void)
{
    /* The printer reads it while it prints: a copy of its own. */
    static u8 prompt[64];
    int i;

    for (i = 0; i < (int)sizeof(prompt) - 1 && gDisplayedStringBattle[i] != EOS; ++i)
        prompt[i] = gDisplayedStringBattle[i];
    prompt[i] = EOS;
    BattlePutTextOnWindow(prompt, B_WIN_MSG);
}

static u8 TakeBattleTap(u8 kind)
{
    u8 tap = sBattleTap;

    sBattleTap = HIT_NONE;
    sAsk.kind = kind;
    sAsk.battler = gActiveBattler;
    return tap;
}

void CtrBattleMenu_ActionInput(u8 *cursor, bool8 safari)
{
    u8 tap = TakeBattleTap(ASK_ACTION), next = *cursor;
    u16 dpad = gMain.newKeys & DPAD_ANY;
    /* FIGHT on top; BAG, POKéMON and RUN in a row under it. The ball
     * button, right of FIGHT and over RUN, is reached from both. */
    bool8 ball = !safari && CtrBattle_QuickBallItem() != ITEM_NONE;

    gMain.newKeys &= ~DPAD_ANY;
    if (!ball)
        sQuickBallFocus = FALSE;
    if (sQuickBallFocus)
    {
        if (dpad & (DPAD_LEFT | DPAD_DOWN))
        {
            sQuickBallFocus = FALSE;
            next = (dpad & DPAD_LEFT) ? 0 : 3;
            PlaySE(SE_SELECT);
            *cursor = next;
        }
        else if (gMain.newKeys & A_BUTTON)
        {
            gMain.newKeys &= ~A_BUTTON;
            sQuickBallTap = TRUE;
        }
        dpad = 0;
    }
    else if (ball && (((dpad & DPAD_RIGHT) && next == 0) || ((dpad & DPAD_UP) && next == 3)))
    {
        sQuickBallFocus = TRUE;
        PlaySE(SE_SELECT);
        dpad = 0;
    }
    if (dpad & DPAD_UP)
        next = 0;
    else if ((dpad & DPAD_DOWN) && next == 0)
        next = 2;
    else if ((dpad & DPAD_LEFT) && next > 1)
        --next;
    else if ((dpad & DPAD_RIGHT) && next != 0 && next < 3)
        ++next;
    if (next != *cursor)
    {
        PlaySE(SE_SELECT);
        *cursor = next;
    }
    if (tap == HIT_QUICK_BALL && !safari)
        sQuickBallTap = TRUE;
    if (tap >= HIT_ACTION && tap < HIT_ACTION + 4)
    {
        sQuickBallFocus = FALSE;
        *cursor = tap - HIT_ACTION;
        gMain.newKeys |= A_BUTTON;
    }
    /* BAG and POKéMON open menus that run hidden. */
    if ((gMain.newKeys & A_BUTTON) && !safari && (*cursor == 1 || *cursor == 2))
        BeginSession(TRUE);
}

void CtrBattleMenu_MoveInput(u8 *cursor, const u16 *moves)
{
    u8 tap = TakeBattleTap(ASK_MOVE), next = *cursor, count = 0;
    u16 dpad = gMain.newKeys & DPAD_ANY;
    bool8 cancel = sMoveCancel;

    for (int i = 0; i < MAX_MON_MOVES; ++i)
        if (moves[i] != MOVE_NONE)
            ++count;
    /* The moves two by two, CANCEL under them. No reordering (SELECT). */
    gMain.newKeys &= ~(DPAD_ANY | SELECT_BUTTON);
    if (cancel)
    {
        if (dpad & DPAD_UP)
            cancel = FALSE;
    }
    else if (dpad & DPAD_UP)
    {
        if (next & 2)
            next ^= 2;
    }
    else if (dpad & DPAD_DOWN)
    {
        if (!(next & 2) && (next ^ 2) < count)
            next ^= 2;
        else
            cancel = TRUE;
    }
    else if (dpad & DPAD_LEFT)
    {
        if (next & 1)
            next ^= 1;
    }
    else if (dpad & DPAD_RIGHT)
    {
        if (!(next & 1) && (next ^ 1) < count)
            next ^= 1;
    }
    if (next != *cursor || cancel != sMoveCancel)
        PlaySE(SE_SELECT);
    *cursor = next;
    sMoveCancel = cancel;
    if (tap >= HIT_MOVE && tap < HIT_MOVE + MAX_MON_MOVES && moves[tap - HIT_MOVE] != MOVE_NONE)
    {
        *cursor = tap - HIT_MOVE;
        sMoveCancel = FALSE;
        gMain.newKeys |= A_BUTTON;
    }
    else if (tap == HIT_CANCEL)
        gMain.newKeys |= B_BUTTON;
    /* A on CANCEL is B. */
    if (sMoveCancel && (gMain.newKeys & A_BUTTON))
        gMain.newKeys = (gMain.newKeys & ~A_BUTTON) | B_BUTTON;
}

void CtrBattleMenu_TargetInput(void)
{
    u8 tap = TakeBattleTap(ASK_TARGET);

    if (tap == HIT_TARGET_LEFT) gMain.newKeys |= DPAD_LEFT;
    else if (tap == HIT_TARGET_RIGHT) gMain.newKeys |= DPAD_RIGHT;
    else if (tap == HIT_TARGET_OK) gMain.newKeys |= A_BUTTON;
    else if (tap == HIT_CANCEL) gMain.newKeys |= B_BUTTON;
}

/* ------------------------------------------------------------------------ */
/* Snapshots                                                                */
/* ------------------------------------------------------------------------ */

#if 0
static void SnapshotMon(MonView *view, struct Pokemon *mon)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES);

    if (species == SPECIES_NONE)
        return;
    view->species = species;
    view->isEgg = GetMonData(mon, MON_DATA_IS_EGG);
    view->iconSpecies = view->isEgg ? SPECIES_EGG : GetIconSpecies(species, GetMonData(mon, MON_DATA_PERSONALITY));
    view->deoxys = species == SPECIES_DEOXYS;
    view->level = GetMonData(mon, MON_DATA_LEVEL);
    view->hp = GetMonData(mon, MON_DATA_HP);
    view->maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    view->ailment = view->isEgg ? AILMENT_NONE : GetMonAilment(mon);
    view->fainted = !view->isEgg && view->hp == 0;
    GetMonNickname(mon, view->nick);
    view->gender = GetMonGender(mon);
    /* The party menu leaves the symbol off a Nidoran still named after its
     * species: the name already says it. */
    if ((species == SPECIES_NIDORAN_M || species == SPECIES_NIDORAN_F)
     && StringCompare(view->nick, gSpeciesNames[species]) == 0)
        view->gender = MON_GENDERLESS;
}

static void SnapshotSummary(ViewState *s, u8 slot)
{
    struct Pokemon *mon = &gPlayerParty[slot];
    static const u8 stats[6] = {MON_DATA_MAX_HP, MON_DATA_ATK, MON_DATA_DEF, MON_DATA_SPATK, MON_DATA_SPDEF,
                                MON_DATA_SPEED};
    u8 bonuses = GetMonData(mon, MON_DATA_PP_BONUSES);

    s->summary = slot;
    for (int i = 0; i < 6; ++i)
        s->stats[i] = GetMonData(mon, stats[i]);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        s->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i);
        s->pp[i] = GetMonData(mon, MON_DATA_PP1 + i);
        s->maxPp[i] = s->moves[i] ? CalculatePPWithBonus(s->moves[i], bonuses, i) : 0;
    }
    s->nature = GetNature(mon);
    s->ability = GetAbilityBySpecies(s->party[slot].species, GetMonData(mon, MON_DATA_ABILITY_NUM));
    s->types[0] = gSpeciesInfo[s->party[slot].species].types[0];
    s->types[1] = gSpeciesInfo[s->party[slot].species].types[1];
    s->heldItem = GetMonData(mon, MON_DATA_HELD_ITEM);
}

#endif

static u8 PlayerRegionPosition(u8 *outX, u8 *outY)
{
    /* region_map.c's InitMapBasedOnPlayerLocation, without its UI state. */
    const struct MapHeader *header = &gMapHeader;
    u16 mapWidth, mapHeight, x, y, scale;
    u8 mapsec;

    switch (GetMapTypeByGroupAndId(gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum))
    {
    case MAP_TYPE_UNDERGROUND:
    case MAP_TYPE_UNKNOWN:
        if (gMapHeader.allowEscaping)
        {
            header = Overworld_GetMapHeaderByGroupAndId(gSaveBlock1Ptr->escapeWarp.mapGroup,
                                                        gSaveBlock1Ptr->escapeWarp.mapNum);
            x = gSaveBlock1Ptr->escapeWarp.x;
            y = gSaveBlock1Ptr->escapeWarp.y;
            mapsec = header->regionMapSectionId;
        }
        else
        {
            mapsec = gMapHeader.regionMapSectionId;
            header = NULL;
            x = y = 1;
        }
        break;
    case MAP_TYPE_SECRET_BASE:
        header = Overworld_GetMapHeaderByGroupAndId(gSaveBlock1Ptr->dynamicWarp.mapGroup,
                                                    gSaveBlock1Ptr->dynamicWarp.mapNum);
        x = gSaveBlock1Ptr->dynamicWarp.x;
        y = gSaveBlock1Ptr->dynamicWarp.y;
        mapsec = header->regionMapSectionId;
        break;
    case MAP_TYPE_INDOOR:
    {
        const struct WarpData *warp = gMapHeader.regionMapSectionId != MAPSEC_DYNAMIC
                                    ? &gSaveBlock1Ptr->escapeWarp : &gSaveBlock1Ptr->dynamicWarp;
        header = Overworld_GetMapHeaderByGroupAndId(warp->mapGroup, warp->mapNum);
        mapsec = gMapHeader.regionMapSectionId != MAPSEC_DYNAMIC ? gMapHeader.regionMapSectionId
                                                                 : header->regionMapSectionId;
        x = warp->x;
        y = warp->y;
        break;
    }
    default:
        mapsec = gMapHeader.regionMapSectionId;
        x = gSaveBlock1Ptr->pos.x;
        y = gSaveBlock1Ptr->pos.y;
        break;
    }
    if (mapsec >= MAPSEC_NONE)
        return MAPSEC_NONE;
    mapWidth = header && header->mapLayout ? header->mapLayout->width : 1;
    mapHeight = header && header->mapLayout ? header->mapLayout->height : 1;
    if (gRegionMapEntries[mapsec].width && gRegionMapEntries[mapsec].height)
    {
        scale = mapWidth / gRegionMapEntries[mapsec].width;
        x /= scale ? scale : 1;
        if (x >= gRegionMapEntries[mapsec].width) x = gRegionMapEntries[mapsec].width - 1;
        scale = mapHeight / gRegionMapEntries[mapsec].height;
        y /= scale ? scale : 1;
        if (y >= gRegionMapEntries[mapsec].height) y = gRegionMapEntries[mapsec].height - 1;
    }
    else
    {
        x = y = 0;
    }
    /* 1 and 2 are region_map.c's MAPCURSOR_X_MIN / MAPCURSOR_Y_MIN. */
    *outX = gRegionMapEntries[mapsec].x + x + 1;
    *outY = gRegionMapEntries[mapsec].y + y + 2;
    return mapsec;
}

static void SnapshotCard(ViewState *s)
{
    static u16 dex, frames;
    static u8 stars;

    StringCopy(s->name, gSaveBlock2Ptr->playerName);
    s->id = gSaveBlock2Ptr->playerTrainerId[0] | (gSaveBlock2Ptr->playerTrainerId[1] << 8);
    s->money = GetMoney(&gSaveBlock1Ptr->money);
    s->hours = gSaveBlock2Ptr->playTimeHours > 999 ? 999 : gSaveBlock2Ptr->playTimeHours;
    s->minutes = gSaveBlock2Ptr->playTimeMinutes > 59 ? 59 : gSaveBlock2Ptr->playTimeMinutes;
    s->hasDex = FlagGet(FLAG_SYS_POKEDEX_GET);
    /* Counting the Pokédex walks every species; twice a second is plenty. */
    if ((frames++ % 30) == 0)
    {
        dex = !s->hasDex ? 0 : IsNationalPokedexEnabled() ? GetNationalPokedexCount(FLAG_GET_CAUGHT)
                                                         : GetHoennPokedexCount(FLAG_GET_CAUGHT);
        /* trainer_card.c's GetRubyTrainerStars as Emerald fills it in. */
        stars = (GetGameStat(GAME_STAT_ENTERED_HOF) != 0) + (HasAllHoennMons() != 0)
              + (CountPlayerMuseumPaintings() >= CONTEST_CATEGORIES_COUNT);
    }
    s->dex = dex;
    s->stars = stars;
    for (int i = 0; i < NUM_BADGES; ++i)
        if (FlagGet(FLAG_BADGE01_GET + i))
            s->badges |= 1 << i;
}

/* The party ball states, as battle_interface.c's party indicator has them. */
enum { PARTY_BALL_OK, PARTY_BALL_STATUS, PARTY_BALL_FAINT, PARTY_BALL_EMPTY };

static void SnapshotBattle(ViewState *s)
{
    s->isDouble = (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0;
    s->safari = (gBattleTypeFlags & BATTLE_TYPE_SAFARI) != 0;
    if (s->mode == MODE_BATTLE_INFO)
        return;
    s->blink = (sFrames >> 4) & 1;
}

/* The six party balls: the mons in party order, then the empty places (an
 * egg is one, as in the game's indicator). */
static void SnapshotPartyBalls(ViewState *s)
{
    int n = 0;

    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);

        if (species == SPECIES_NONE || species == SPECIES_EGG)
            continue;
        if (GetMonData(mon, MON_DATA_HP) == 0)
            s->partyBalls[n++] = PARTY_BALL_FAINT;
        else if (GetMonData(mon, MON_DATA_STATUS) != 0)
            s->partyBalls[n++] = PARTY_BALL_STATUS;
        else
            s->partyBalls[n++] = PARTY_BALL_OK;
    }
    while (n < PARTY_SIZE)
        s->partyBalls[n++] = PARTY_BALL_EMPTY;
}

static void CopyText(u8 *dst, int size, const u8 *src)
{
    int n = 0;

    while (src && n < size - 1 && src[n] != EOS)
    {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = EOS;
}

#if 0
/* What the hidden party menu is asking, for the lower panel. */
static void SnapshotPartyPanel(ViewState *s)
{
    s->menuCount = CtrPartyMenu_GetActions(s->menuNames, MAX_MENU_ITEMS);
    s->menuCols = 1;
    s->message = CtrPartyMenu_GetMessage();
    if (CtrMenu_YesNoOpen())
        s->panel = PANEL_YESNO;
    else if (s->menuCount)
    {
        s->panel = PANEL_ACTIONS;
        s->menuCursor = Menu_GetCursorPos();
    }
    else if (PartyMenuReady())
        s->panel = PANEL_HINT;
    else if (s->message)
        s->panel = PANEL_MESSAGE;
    CopyText(s->text, sizeof(s->text), s->message);
}

#endif

static void Snapshot(ViewState *s, u8 mode, u8 pressed)
{
    memset(s, 0, sizeof(*s));
    s->mode = mode;
    s->pressed = pressed;
    s->summary = -1;
    s->partyCursor = -1;
    s->pickMapsec = MAPSEC_NONE;
    if (s->mode == MODE_OFF)
        return;
    s->gender = gSaveBlock2Ptr->playerGender ? FEMALE : MALE;
    s->inBattle = gMain.inBattle;
    s->enabled = EnabledScreens();
    s->screen = sScreen;
    /* The hidden menus take over the view they belong to. */
    if (mode == MODE_PARTY_MENU)
    {
        s->screen = SCR_POKEMON;
        /* The party menu from the field is left of the column; from a
         * battle, a contest or a facility it covers it (CtrCentredParty). */
        s->bagView = !gMain.inBattle && gPartyMenu.menuType == PARTY_MENU_TYPE_FIELD ? BAG_VIEW_SECTION
                                                                                       : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_BAG_MENU)
    {
        s->screen = SCR_BAG;
        /* The bag from the field is left of the column; from anything else
         * it covers the column. */
        s->bagView = gBagPosition.location == ITEMMENULOCATION_FIELD ? BAG_VIEW_SECTION : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_POKENAV)
        s->screen = SCR_POKENAV;
    else if (mode == MODE_POKEDEX)
        s->screen = SCR_POKEDEX;
    else if (mode == MODE_STORAGE)
        s->screen = SCR_COUNT;   /* the boxes cover the column */
    StringCopy(s->name, gSaveBlock2Ptr->playerName);
    /* The column's Y and RUN, and the focus ring on it or in the options. */
    s->registered = gSaveBlock1Ptr->registeredItem;
    s->run = (CtrSettings_RunAlways() ? 1 : 0) | (FlagGet(FLAG_SYS_B_DASH) ? 2 : 0);
    s->focus = sFocus;
    s->optFocus = FOCUS_NONE;
    if (sFocus != FOCUS_NONE || sInside == INSIDE_OPTIONS)
        s->blink = (sFrames >> 4) & 1;

    switch (s->mode)
    {
    case MODE_FIELD:
    case MODE_PARTY_MENU:
    case MODE_BAG_MENU:
        switch (s->screen)
        {
        case SCR_MAP:
            s->mapsec = PlayerRegionPosition(&s->cursorX, &s->cursorY);
            s->pickMapsec = sPickMapsec;
            s->pickX = sPickX;
            s->pickY = sPickY;
            break;
        case SCR_POKEMON:
            /* The game's party menu is drawn by the compositor. */
            break;
        case SCR_CARD:
            SnapshotCard(s);
            break;
        case SCR_SAVE:
            s->saveStep = sSaveStep;
            s->canSave = FieldIdle();
            CopyText(s->text, sizeof(s->text), sSaveMessage);
            break;
        case SCR_OPTION:
            s->options[0] = gSaveBlock2Ptr->optionsTextSpeed;
            s->options[1] = gSaveBlock2Ptr->optionsBattleSceneOff;
            s->options[2] = gSaveBlock2Ptr->optionsBattleStyle;
            s->options[3] = gSaveBlock2Ptr->optionsSound;
            s->options[4] = gSaveBlock2Ptr->optionsButtonMode;
            s->options[5] = gSaveBlock2Ptr->optionsWindowFrameType;
            s->options[OPT_FPS] = CtrSettings_ShowFps();
            s->optPage = sOptPage;
            s->optSub = sOptSub;
            for (unsigned row = 0; sOptPage != CTR_EXTRAS_OPTIONS && row < ARRAY_COUNT(s->extras); ++row)
            {
                const CtrExtra *extra = PageExtra(sOptPage, row);

                if (extra)
                    s->extras[row] = ExtraShownValue(extra);
            }
            s->options[OPT_VOXEL] = CtrSettings_Voxel();
            s->options[OPT_VOXEL_PITCH] = CtrSettings_VoxelPitch();
            s->options[OPT_VOXEL_ZOOM] = CtrSettings_VoxelZoom();
            s->options[OPT_VOXEL_BLUR] = CtrSettings_VoxelBlur();
            s->options[OPT_VOXEL_BATTLE] = CtrSettings_VoxelBattle();
            if (sInside == INSIDE_OPTIONS)
                s->optFocus = sOptFocus;
            break;
        }
        break;
    case MODE_BATTLE_ACTION:
    {
        u8 b = sAsked.battler;
        SnapshotBattle(s);
        SnapshotPartyBalls(s);
        s->battler = b;
        s->cursor = sQuickBallFocus ? 4 : gActionSelectionCursor[b];
        if (s->safari)
            s->safariBalls = gNumSafariBalls;
        else
            s->quickBall = CtrBattle_QuickBallItem();
        if (s->quickBall != ITEM_NONE)
            s->quickBallCount = CountTotalItemQuantityInBag(s->quickBall);
        break;
    }
    case MODE_BATTLE_MOVE:
    case MODE_BATTLE_TARGET:
    {
        u8 b = sAsked.battler;
        SnapshotBattle(s);
        s->battler = b;
        s->cursor = sMoveCancel ? MAX_MON_MOVES : gMoveSelectionCursor[b];
        memcpy(&s->moves4, &gBattleBufferA[b][4], sizeof(s->moves4));
        break;
    }
    case MODE_BATTLE_INFO:
        SnapshotBattle(s);
        break;
    }
}

/* Loads one icon or picture the snapshot needs; TRUE if it did. */
static bool8 Prefetch(const ViewState *s)
{
    sIconBudget = TRUE;
    for (int i = 0; i < PARTY_SIZE && sIconBudget; ++i)
        if (s->party[i].species)
            MonIcon(s->party[i].iconSpecies, s->party[i].deoxys);
    if (sIconBudget && s->summary >= 0 && s->heldItem)
        ItemIcon(s->heldItem);
    if (sIconBudget && s->registered != ITEM_NONE && s->mode < MODE_BATTLE_INFO)
        ItemIcon(s->registered);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->quickBall != ITEM_NONE)
        ItemIcon(s->quickBall);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->safari)
        ItemIcon(ITEM_SAFARI_BALL);
    if (sIconBudget)
    {
        sIconBudget = FALSE;
        return FALSE;
    }
    return TRUE;
}

