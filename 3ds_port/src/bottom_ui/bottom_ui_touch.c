/* ------------------------------------------------------------------------ */
/* Pressing buttons for the player, in the hidden menus and in battle       */
/* ------------------------------------------------------------------------ */

enum
{
    PLAN_NONE,
    PLAN_PRESS,      /* one press of `keys` */
    PLAN_KEYS,       /* `keys`, then A */
    PLAN_START,      /* open start menu entry `target` */
};

typedef struct
{
    u8 kind, steps, wait, cols, tries, maxWait;
    bool8 release;
    s16 target;
    u16 keys;
} Plan;

static Plan sPlan;
/* What follows the current plan: opening the party menu on a mon... Each
 * step waits for the game to reach the screen it needs. */
static Plan sQueue[2];
static u8 sQueued;
static u16 sInjected;

static Plan MakePlan(u8 kind, s16 target)
{
    Plan p;

    memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.target = target;
    p.maxWait = 45;
    return p;
}

static void StartPlan(u8 kind, s16 target)
{
    sPlan = MakePlan(kind, target);
    sQueued = 0;
}

#if 0 /* only the old party menu queued plans or pressed keys */
static void QueuePlan(Plan p)
{
    /* The screen it waits for is behind a fade and a menu setup. */
    p.maxWait = 150;
    if (sQueued < ARRAY_COUNT(sQueue))
        sQueue[sQueued++] = p;
}

static void Press(u16 keys)
{
    StartPlan(PLAN_PRESS, 0);
    sPlan.keys = keys;
}

#endif

static void FinishPlan(void)
{
    if (sQueued)
    {
        sPlan = sQueue[0];
        sQueue[0] = sQueue[1];
        --sQueued;
    }
    else
        sPlan.kind = PLAN_NONE;
}

static void CancelPlan(void)
{
    if (sPlan.kind == PLAN_START)
        CtrStartMenu_Request(START_NONE);
    sPlan.kind = PLAN_NONE;
    sQueued = 0;
}

/* Where the game's cursor is, or FALSE while that menu is not taking input. */
static bool8 PlanCursor(s16 *cursor)
{
    /* The game's party menu takes touch itself now (party_menu.c): no plan
     * walks a cursor. */
    (void)cursor;
    return FALSE;
}

static u16 PlanStep(s16 cur, s16 target)
{
    (void)cur;
    (void)target;
    return 0;
}

/* START with a request pending opens that entry (see start_menu.c). */
static void RunStartPlan(void)
{
    if (sPlan.tries && !CtrStartMenu_Pending())
    {
        FinishPlan();                             /* served */
        return;
    }
    if (++sPlan.wait < 8 && sPlan.tries)
        return;
    /* START only registers with the player free to move; try a few times. */
    if (sPlan.tries >= 6 || gMain.callback2 != CB2_Overworld || CtrStartMenu_Busy())
    {
        if (sPlan.tries >= 6 || ++sPlan.steps > 90)
            CancelPlan();
        return;
    }
    CtrStartMenu_Request(sPlan.target);
    sInjected = START_BUTTON;
    sPlan.release = TRUE;
    sPlan.wait = 0;
    ++sPlan.tries;
}

static void RunPlan(void)
{
    s16 cur;

    sInjected = 0;
    if (sPlan.kind == PLAN_NONE)
        return;
    /* The player's own buttons always win. */
    if (CtrInput_Get()->held & CTR_KEY_GAME)
    {
        CancelPlan();
        return;
    }
    /* A press only registers as new after a frame with the key up. */
    if (sPlan.release)
    {
        sPlan.release = FALSE;
        return;
    }
    switch (sPlan.kind)
    {
    case PLAN_PRESS:
        sInjected = sPlan.keys;
        FinishPlan();
        sPlan.release = TRUE;
        return;
    case PLAN_KEYS:
        sInjected = sPlan.keys;
        sPlan = MakePlan(PLAN_PRESS, 0);
        sPlan.keys = A_BUTTON;
        sPlan.release = TRUE;
        return;
    case PLAN_START:
        RunStartPlan();
        return;
    }
    if (!PlanCursor(&cur))
    {
        if (++sPlan.wait > sPlan.maxWait)
            CancelPlan();
        return;
    }
    if (++sPlan.steps > 24)
    {
        CancelPlan();
        return;
    }
    if (cur == sPlan.target)
    {
        sInjected = A_BUTTON;
        FinishPlan();
        /* The next step must not see this press as its own. */
        sPlan.release = TRUE;
        return;
    }
    sInjected = PlanStep(cur, sPlan.target);
    sPlan.release = TRUE;
}

uint16_t CtrBottom_InjectedKeys(void)
{
    return sInjected;
}

/* ------------------------------------------------------------------------ */
/* The PokéNav by touch                                                     */
/* ------------------------------------------------------------------------ */

/*
 * The PokéNav runs as it is, drawn left of the column by the compositor; a
 * tap on one of its screens becomes the buttons that screen reads, pressed
 * only while the PokéNav waits for input (CtrPokenav_Screen): an option or a
 * list entry is reached with one press of the D-pad, the cursor put next to
 * it first (NavNext), and chosen with A; a place on the map
 * is walked to, a ribbon is picked. The POKéNAV button of the column is B,
 * and any other button of it leaves the PokéNav for its own screen.
 */
enum
{
    NAV_NONE,
    NAV_PRESS,    /* keys, once */
    NAV_MENU,     /* walk the menu cursor to target */
    NAV_LIST,     /* walk the list selection to target */
    NAV_OPTION,   /* walk Match Call's options cursor to target */
    NAV_PARTY,    /* walk the condition screen's mon to target */
    NAV_MAP,      /* dx, dy presses on the map */
    NAV_RIBBON,   /* walk the ribbon cursor to target */
    NAV_MARK,     /* walk the markings menu's cursor to target */
    NAV_LEAVE,    /* B until the PokéNav closes */
};

static struct
{
    u8 kind, steps, wait;
    bool8 release;
    s16 target, dx, dy;
    u16 keys, finish;
} sNav;

/* Condition graph: the party's balls down the right edge, CANCEL below. */
#define NAV_BALL_X 212
#define NAV_BALL_TOP 8
#define NAV_BALL_STEP 20
/* Ribbon summary: 16x16 cells from here, RIBBONS_PER_ROW to a row. */
#define NAV_RIBBON_X 88
#define NAV_RIBBON_Y 32
#define NAV_RIBBONS_PER_ROW 9
/* Match Call's options: rows of its info box, left of the list. */
#define NAV_OPTION_Y 72
#define NAV_OPTION_W 88

static void NavStart(u8 kind, s16 target, u16 finish)
{
    memset(&sNav, 0, sizeof(sNav));
    sNav.kind = kind;
    sNav.target = target;
    sNav.finish = finish;
}

static void NavPress(u16 keys)
{
    NavStart(NAV_PRESS, 0, 0);
    sNav.keys = keys;
}

static struct PokenavMonList *NavMonList(void)
{
    return GetSubstructPtr(POKENAV_SUBSTRUCT_MON_LIST);
}

/*
 * The press that takes a cursor from cursor to target in one step: the
 * cursor is put right next to the target first (set), so the game's own
 * move - its sound, the option sliding out - happens once, for the target.
 */
static u16 NavNext(int cursor, int target, void (*set)(int))
{
    if (target > cursor + 1) set(target - 1);
    else if (target < cursor - 1) set(target + 1);
    return target > cursor ? DPAD_DOWN : DPAD_UP;
}

static void NavSetMenu(int cursor) { CtrPokenavMenu_SetCursor(cursor); }
static void NavSetList(int cursor) { CtrPokenavList_SetSelected((u16)cursor); }
static void NavSetOption(int cursor) { CtrPokenavMatchCall_SetOption((u16)cursor); }
static void NavSetMark(int cursor) { CtrMonMarkings_SetCursor((s8)cursor); }

/* One step of the plan, or 0 while the cursor is not known. */
static u16 NavStep(bool8 *done)
{
    int cursor = 0, count;
    u16 top, selected, shown, total, option, options, normal, gift, giftStart;
    u8 x, y, width;
    bool8 zoomed, moving, expanded;
    s16 cx, cy;
    struct PokenavMonList *mons;

    *done = FALSE;
    switch (sNav.kind)
    {
    case NAV_PRESS:
        *done = TRUE;
        return sNav.keys;
    case NAV_MENU:
        count = CtrPokenavMenu_Options(&cursor);
        if (sNav.target >= count) break;
        if (cursor == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(cursor, sNav.target, NavSetMenu);
    case NAV_LIST:
        if (!CtrPokenavList_View(&x, &y, &width, &top, &selected, &shown, &total) || sNav.target >= total) break;
        if (selected == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(selected, sNav.target, NavSetList);
    case NAV_OPTION:
        if (CtrPokenavMatchCall_Input(&option, &options) != 1 || sNav.target >= options) break;
        if (option == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(option, sNav.target, NavSetOption);
    case NAV_PARTY:
        if (!(mons = NavMonList()) || sNav.target >= mons->listCount) break;
        if (mons->currIndex == sNav.target) { *done = TRUE; return sNav.finish; }
        return sNav.target > mons->currIndex ? DPAD_DOWN : DPAD_UP;
    case NAV_MAP:
        if (!CtrRegionMap_Cursor(&cx, &cy, &zoomed, &moving) || moving) return 0;
        if (sNav.dx > 0) { --sNav.dx; return DPAD_RIGHT; }
        if (sNav.dx < 0) { ++sNav.dx; return DPAD_LEFT; }
        if (sNav.dy > 0) { --sNav.dy; return DPAD_DOWN; }
        if (sNav.dy < 0) { ++sNav.dy; return DPAD_UP; }
        *done = TRUE;
        return sNav.finish;
    case NAV_RIBBON:
        if (!CtrPokenavRibbons_Summary(&selected, &normal, &gift, &giftStart, &expanded)) break;
        if (!expanded) return A_BUTTON;
        if (selected == sNav.target) { *done = TRUE; return 0; }
        if (selected / NAV_RIBBONS_PER_ROW != sNav.target / NAV_RIBBONS_PER_ROW)
            return sNav.target > selected ? DPAD_DOWN : DPAD_UP;
        return sNav.target > selected ? DPAD_RIGHT : DPAD_LEFT;
    case NAV_MARK:
    {
        s8 mark;
        s16 mx, my;

        if (!CtrPokenavCondition_Marking() || !CtrMonMarkings_Menu(&mark, &mx, &my)) break;
        if (mark == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(mark, sNav.target, NavSetMark);
    }
    case NAV_LEAVE:
        return B_BUTTON;
    }
    /* What the plan was for is gone. */
    sNav.kind = NAV_NONE;
    return 0;
}

static void RunNav(u8 mode)
{
    bool8 ready, done;
    u16 keys;

    if (sNav.kind == NAV_NONE)
        return;
    if (mode != MODE_POKENAV)
    {
        sNav.kind = NAV_NONE;
        return;
    }
    if (CtrInput_Get()->held & CTR_KEY_GAME)
    {
        sNav.kind = NAV_NONE;
        return;
    }
    /* A press only registers as new after a frame with the key up. */
    if (sNav.release)
    {
        sNav.release = FALSE;
        return;
    }
    /* A single press also answers what waits inside a task: a call's text. */
    CtrPokenav_Screen(&ready);
    keys = ready || sNav.kind == NAV_PRESS ? NavStep(&done) : 0;
    if (!keys)
    {
        /* Waiting for the PokéNav to take input, or a move to end. */
        if (++sNav.wait > 240)
            sNav.kind = NAV_NONE;
        return;
    }
    if (done && sNav.kind != NAV_LEAVE)
        sNav.kind = NAV_NONE;
    if (++sNav.steps > 64)
        sNav.kind = NAV_NONE;
    sInjected = keys;
    sNav.release = TRUE;
    sNav.wait = 0;
}

/* A tap at (x, y) of the PokéNav's picture. */
static void NavTap(int x, int y)
{
    bool8 ready, zoomed, moving, expanded;
    u32 screen = CtrPokenav_Screen(&ready);
    int yStart, deltaY, cursor, count, row;
    u16 top, selected, shown, total, option, options, normal, gift, giftStart;
    u8 lx, ly, width;
    s16 cx, cy;
    struct PokenavMonList *mons;

    if (y < 0 || y >= 160)
        return;
    switch (screen)
    {
    case POKENAV_MAIN_MENU:
    case POKENAV_MAIN_MENU_CURSOR_ON_MAP:
    case POKENAV_CONDITION_MENU:
    case POKENAV_CONDITION_SEARCH_MENU:
    case POKENAV_MAIN_MENU_CURSOR_ON_MATCH_CALL:
    case POKENAV_MAIN_MENU_CURSOR_ON_RIBBONS:
        count = CtrPokenavMenu_Options(&cursor);
        CtrPokenavMenu_Rows(&yStart, &deltaY);
        row = (y - yStart + deltaY / 2 + deltaY) / deltaY - 1;
        if (x >= 112 && row >= 0 && row < count)
            NavStart(NAV_MENU, row, A_BUTTON);
        else if (x < 88 && y >= 16 && y < 40)
            NavPress(B_BUTTON);         /* the header: back */
        else
            NavPress(A_BUTTON);         /* a message waiting (no ribbons yet) */
        break;
    case POKENAV_REGION_MAP:
        if (y >= 144)
        {
            /* The help bar: "A ZOOM", then "B CANCEL". */
            if (x < 40) NavPress(A_BUTTON);
            else if (x < 112) NavPress(B_BUTTON);
            break;
        }
        if (!CtrRegionMap_Cursor(&cx, &cy, &zoomed, &moving))
            break;
        {
            int step = zoomed ? 16 : 8;
            int dx = x - cx, dy = y - cy;

            NavStart(NAV_MAP, 0, 0);
            sNav.dx = (dx + (dx >= 0 ? step / 2 : -step / 2)) / step;
            sNav.dy = (dy + (dy >= 0 ? step / 2 : -step / 2)) / step;
            /* On the cursor: the place is chosen, zoom in or out on it. */
            if (!sNav.dx && !sNav.dy)
                sNav.finish = A_BUTTON;
        }
        break;
    case POKENAV_CONDITION_GRAPH_PARTY:
    case POKENAV_CONDITION_GRAPH_SEARCH:
        mons = NavMonList();
        {
            s8 mark;
            s16 mx, my;

            /* The markings menu, open over the graph: a row, or out of it. */
            if (CtrPokenavCondition_Marking() && CtrMonMarkings_Menu(&mark, &mx, &my))
            {
                row = (y - my - 8) / 16;
                if (x >= mx && x < mx + 64 && y >= my + 8 && row < 6)
                    NavStart(NAV_MARK, row, A_BUTTON);
                else
                    NavPress(B_BUTTON);
                break;
            }
        }
        if (x < 88 && y < 40)
            NavPress(B_BUTTON);
        else if (screen == POKENAV_CONDITION_GRAPH_PARTY && x >= NAV_BALL_X && mons)
        {
            row = (y - NAV_BALL_TOP + NAV_BALL_STEP / 2 + NAV_BALL_STEP) / NAV_BALL_STEP - 1;
            if (row == PARTY_SIZE)
                NavStart(NAV_PARTY, mons->listCount - 1, A_BUTTON);   /* CANCEL */
            else if (row >= 0 && row < mons->listCount - 1)
                NavStart(NAV_PARTY, row, 0);
        }
        else if (x < 80 && y >= 56)
            NavPress(y < 100 ? DPAD_UP : DPAD_DOWN);   /* the picture: previous, next */
        else if (screen == POKENAV_CONDITION_GRAPH_SEARCH)
            NavPress(A_BUTTON);                       /* markings */
        break;
    case POKENAV_CONDITION_SEARCH_RESULTS:
    case POKENAV_MATCH_CALL:
    case POKENAV_RIBBONS_MON_LIST:
        if (screen == POKENAV_MATCH_CALL)
        {
            switch (CtrPokenavMatchCall_Input(&option, &options))
            {
            case 1:
                row = (y - NAV_OPTION_Y) / 16;
                if (x < NAV_OPTION_W && y >= NAV_OPTION_Y && row < options)
                    NavStart(NAV_OPTION, row, A_BUTTON);
                else
                    NavPress(B_BUTTON);
                return;
            case 2:
                NavPress(B_BUTTON);
                return;
            case 3:
                NavPress(A_BUTTON);
                return;
            }
        }
        if (x < 88 && y < 32)
        {
            NavPress(B_BUTTON);
            break;
        }
        if (!CtrPokenavList_View(&lx, &ly, &width, &top, &selected, &shown, &total) || x < lx || x >= lx + width)
            break;
        if (y < ly)
            NavPress(DPAD_LEFT);        /* a page up */
        else if (y >= ly + 16 * shown)
            NavPress(DPAD_RIGHT);       /* a page down */
        else if (top + (y - ly) / 16 < total)
            NavStart(NAV_LIST, top + (y - ly) / 16, A_BUTTON);
        break;
    case POKENAV_RIBBONS_SUMMARY_SCREEN:
        if (!CtrPokenavRibbons_Summary(&selected, &normal, &gift, &giftStart, &expanded))
            break;
        if (x >= NAV_RIBBON_X && x < NAV_RIBBON_X + 16 * NAV_RIBBONS_PER_ROW && y >= NAV_RIBBON_Y)
        {
            int pos = (y - NAV_RIBBON_Y) / 16 * NAV_RIBBONS_PER_ROW + (x - NAV_RIBBON_X) / 16;

            if (pos < normal || (pos >= giftStart && pos < giftStart + gift))
            {
                NavStart(NAV_RIBBON, pos, 0);
                break;
            }
        }
        if (expanded)
            NavPress(B_BUTTON);
        else if (x < 88 && y < 32)
            NavPress(B_BUTTON);
        else if (x < 80 && y >= 64)
            NavPress(y < 104 ? DPAD_UP : DPAD_DOWN);   /* the picture: previous, next */
        break;
    }
}

/* A drag across the PokéNav's picture: the next or previous page or mon. */
static void NavSwipe(int dy)
{
    bool8 ready;
    u32 screen = CtrPokenav_Screen(&ready);
    bool8 next = dy < 0;

    switch (screen)
    {
    case POKENAV_CONDITION_SEARCH_RESULTS:
    case POKENAV_MATCH_CALL:
    case POKENAV_RIBBONS_MON_LIST:
        NavPress(next ? DPAD_RIGHT : DPAD_LEFT);
        break;
    case POKENAV_CONDITION_GRAPH_PARTY:
    case POKENAV_CONDITION_GRAPH_SEARCH:
    case POKENAV_RIBBONS_SUMMARY_SCREEN:
        NavPress(next ? DPAD_DOWN : DPAD_UP);
        break;
    }
}


/* ------------------------------------------------------------------------ */
/* What a tap does                                                          */
/* ------------------------------------------------------------------------ */

/* A drag up or down the options list scrolls it, within its length. */
static void OptionsDrag(int dy)
{
    int max = OptionsMaxScroll(CtrSettings_Voxel());

    sOptionScroll = sOptionScrollStart - dy;
    if (sOptionScroll < 0)
        sOptionScroll = 0;
    if (sOptionScroll > max)
        sOptionScroll = max;
}

static struct
{
    bool8 active, dragged;
    s16 startX, startY, lastX, lastY;
    u8 pressed;
    bool8 bag;   /* on the game's bag or Pokédex, which take it themselves */
} sTouch;

/* The save, done here as start_menu.c's SaveDoSaveCallback does it. */
static void DoSave(void)
{
    u8 status;

    SaveMapView();
    IncrementGameStat(GAME_STAT_SAVED_GAME);
    if (gDifferentSaveFile == TRUE)
    {
        status = TrySavingData(SAVE_OVERWRITE_DIFFERENT_FILE);
        gDifferentSaveFile = FALSE;
    }
    else
    {
        status = TrySavingData(SAVE_NORMAL);
    }
    StringExpandPlaceholders(sSaveMessage, status == SAVE_STATUS_OK ? gText_PlayerSavedGame : gText_SaveError);
    if (status == SAVE_STATUS_OK)
        PlaySE(SE_SAVE);
    sSaveStep = SAVE_DONE;
}

static void OpenSave(void)
{
    sSaveStep = SAVE_ASK;
    StringExpandPlaceholders(sSaveMessage, gText_ConfirmSave);
}

#if 0 /* The old party menu's buttons; the game's own takes touch itself. */
/* A game menu entry: SUMMARY is shown here, the rest runs in the game. */
static void ChooseMenuEntry(u8 index)
{
    if (sShown.mode == MODE_PARTY_MENU && index < sShown.menuCount && sShown.menuNames[index] == gText_Summary5)
    {
        sSummary = gPartyMenu.slotId;
        Press(B_BUTTON); /* close the submenu; the summary is drawn here */
        return;
    }
    StartPlan(PLAN_MENU, index);
    sPlan.cols = sShown.menuCols;
}

static void Answer(u8 id)
{
    if (id == HIT_YES)
    {
        /* Some questions default to NO: go up to YES first. */
        StartPlan(PLAN_KEYS, 0);
        sPlan.keys = DPAD_UP;
    }
    else if (id == HIT_NO || id == HIT_CANCEL)
        Press(B_BUTTON);
    else if (id == HIT_OK || id == HIT_PANEL)
        Press(A_BUTTON);
    else if (id == HIT_UP)
        Press(DPAD_UP);
    else if (id == HIT_DOWN)
        Press(DPAD_DOWN);
}

static void ActivateSummary(u8 id)
{
    if (id == HIT_BACK)
        sSummary = -1;
    else if (id == HIT_PREV || id == HIT_NEXT)
    {
        for (int n = 0; n < PARTY_SIZE; ++n)
        {
            sSummary = (sSummary + (id == HIT_NEXT ? 1 : PARTY_SIZE - 1)) % PARTY_SIZE;
            if (GetMonData(&gPlayerParty[sSummary], MON_DATA_SPECIES) != SPECIES_NONE
             && !GetMonData(&gPlayerParty[sSummary], MON_DATA_IS_EGG))
                break;
        }
    }
}

static void ActivatePokemon(u8 id, u8 mode)
{
    if (sSummary >= 0)
    {
        ActivateSummary(id);
        return;
    }
    if (id >= HIT_SLOT && id < HIT_SLOT + PARTY_SIZE)
    {
        u8 slot = id - HIT_SLOT;

        sPartyTapped = slot;
        if (mode == MODE_PARTY_MENU)
        {
            if (PartyMenuReady())
                StartPlan(PLAN_PARTY, slot);
        }
        else if (FieldIdle() && CtrStartMenu_Available())
        {
            /* The party menu, hidden, on this mon, with its menu open. */
            BeginSession(FALSE);
            StartPlan(PLAN_START, START_POKEMON);
            QueuePlan(MakePlan(PLAN_PARTY, slot));
        }
        return;
    }
    if (mode != MODE_PARTY_MENU)
        return;
    if (id >= HIT_MENU && id < HIT_MENU + MAX_MENU_ITEMS)
        ChooseMenuEntry(id - HIT_MENU);
    else
        Answer(id);
}

#endif

static void ActivateOption(u8 id)
{
    bool8 back = id >= HIT_OPTION + HIT_OPTION_BACK;
    u8 row = (id - HIT_OPTION) % HIT_OPTION_BACK;
    static const u8 counts[OPTION_ROWS] = {3, 2, 2, 2, 3, WINDOW_FRAMES_COUNT, 2, 2, 0, 0, 2, 2, 2};
    u8 value, step = back ? counts[row] - 1 : 1;

    if (!OptionRowShown(row, TRUE))
        return;
    if (row == OPT_FPS)
    {
        CtrSettings_SetShowFps(!CtrSettings_ShowFps());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL)
    {
        CtrSettings_SetVoxel(!CtrSettings_Voxel());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL_BLUR)
    {
        if (!CtrSettings_Voxel())
            return;
        CtrSettings_SetVoxelBlur(!CtrSettings_VoxelBlur());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL_BATTLE)
    {
        if (!CtrSettings_Voxel())
            return;
        CtrSettings_SetVoxelBattle(!CtrSettings_VoxelBattle());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL_STEREO)
    {
        if (!CtrSettings_Voxel())
            return;
        CtrSettings_SetVoxelStereo(!CtrSettings_VoxelStereo());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_DAYNIGHT)
    {
        if (!CtrSettings_Voxel())
            return;
        CtrSettings_SetDayNight(!CtrSettings_DayNight());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL_PITCH || row == OPT_VOXEL_ZOOM)
    {
        if (!CtrSettings_Voxel())
            return;
        if (row == OPT_VOXEL_PITCH)
            CtrSettings_StepVoxelPitch(back ? -1 : 1);
        else
            CtrSettings_StepVoxelZoom(back ? -1 : 1);
        PlaySE(SE_SELECT);
        return;
    }
    switch (row)
    {
    case 0: value = gSaveBlock2Ptr->optionsTextSpeed; break;
    case 1: value = gSaveBlock2Ptr->optionsBattleSceneOff; break;
    case 2: value = gSaveBlock2Ptr->optionsBattleStyle; break;
    case 3: value = gSaveBlock2Ptr->optionsSound; break;
    case 4: value = gSaveBlock2Ptr->optionsButtonMode; break;
    default: value = gSaveBlock2Ptr->optionsWindowFrameType; break;
    }
    value = (value + step) % counts[row];
    switch (row)
    {
    case 0: gSaveBlock2Ptr->optionsTextSpeed = value; break;
    case 1: gSaveBlock2Ptr->optionsBattleSceneOff = value; break;
    case 2: gSaveBlock2Ptr->optionsBattleStyle = value; break;
    case 3: gSaveBlock2Ptr->optionsSound = value; SetPokemonCryStereo(value); break;
    case 4: gSaveBlock2Ptr->optionsButtonMode = value; break;
    default: gSaveBlock2Ptr->optionsWindowFrameType = value; break;
    }
    PlaySE(SE_SELECT);
}

/*
 * The game's bag, party menu or Pokédex for BAG, POKéMON or POKéDEX, opened from the field as the
 * start menu opens it. Chosen while another screen was up - the Pokédex, the
 * bag, the party menu, the PokéNav - it opens once that one has closed and
 * the field is idle again (OpenAsked); until then the area is black.
 */
static bool8 OpenGameScreen(u8 screen)
{
    if (!FieldIdle() || !CtrStartMenu_Available())
        return FALSE;
    StartPlan(PLAN_START, screen == SCR_BAG ? START_BAG : screen == SCR_POKEMON ? START_POKEMON : START_POKEDEX);
    BeginSession(FALSE);
    return TRUE;
}

static void OpenAsked(u8 mode)
{
    static u8 lastMode = MODE_OFF;
    static u16 waited;

    /* Closed by its own button or B: back to the map, not opened again. */
    if (mode != lastMode)
    {
        if ((lastMode == MODE_BAG_MENU && sScreen == SCR_BAG) || (lastMode == MODE_POKEDEX && sScreen == SCR_POKEDEX)
         || (lastMode == MODE_PARTY_MENU && sScreen == SCR_POKEMON))
            sScreen = SCR_MAP;
        lastMode = mode;
        waited = 0;
    }
    if (mode != MODE_FIELD || (sScreen != SCR_BAG && sScreen != SCR_POKEDEX && sScreen != SCR_POKEMON) || sSession.active
     || sPlan.kind != PLAN_NONE)
        return;
    if (!(EnabledScreens() & (1 << sScreen)))
        sScreen = SCR_MAP;
    else if (OpenGameScreen(sScreen))
        waited = 0;
    /* Two seconds without the field coming back idle: given up. */
    else if (++waited > 120)
        sScreen = SCR_MAP;
}

static void Activate(u8 id, u8 mode)
{
    if (id == HIT_NONE)
        return;
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: tap %02x (mode %u screen %u panel %u)", id, mode, sShown.screen,
                 sShown.panel);
    if (mode >= MODE_BATTLE_INFO)
    {
        /* The controller waiting for it takes it on its next frame. */
        if (mode != MODE_BATTLE_INFO)
            sBattleTap = id;
        return;
    }

    if (id >= HIT_COLUMN && id < HIT_COLUMN + SCR_COUNT)
    {
        u8 screen = id - HIT_COLUMN;

        if (mode == MODE_POKENAV)
        {
            /* Its own button is its B; any other leaves it for that screen. */
            if (screen == SCR_POKENAV)
                NavPress(B_BUTTON);
            else
            {
                sScreen = screen;
                NavStart(NAV_LEAVE, 0, 0);
            }
            return;
        }
        /* The bag on show: its own button closes it, as B does; any other
         * closes it for that screen. */
        if (mode == MODE_BAG_MENU)
        {
            if (CtrBag_Close() && screen != SCR_BAG)
                sScreen = screen;
            return;
        }
        /* The Pokédex's is its B; any other leaves it for that screen, B
         * after B, as the PokéNav's do. */
        if (mode == MODE_POKEDEX)
        {
            if (screen == SCR_POKEDEX)
                CtrPokedex_Close(FALSE);
            else if (CtrPokedex_Close(TRUE))
                sScreen = screen;
            return;
        }
        /* The game's party menu on show: its own button is B, as the bag's
         * and the Pokédex's are; any other closes it for that screen. */
        if (mode == MODE_PARTY_MENU)
        {
            if (screen == SCR_POKEMON)
                CtrParty_Close(FALSE);
            else if (CtrParty_Close(TRUE))
                sScreen = screen;
            return;
        }
        if (mode != MODE_FIELD)
            return;
        /* The PokéNav takes the area when it opens; the top keeps the world
         * from the moment it is asked for, fade included. */
        if (screen == SCR_POKENAV)
        {
            if (FieldIdle())
            {
                StartPlan(PLAN_START, START_POKENAV);
                BeginSession(FALSE);
            }
            return;
        }
        /* The game's bag and Pokédex, as the PokéNav: they take the area
         * when they open. */
        if (screen == SCR_BAG || screen == SCR_POKEDEX || screen == SCR_POKEMON)
        {
            OpenGameScreen(screen);
            return;
        }
        if (screen == SCR_SAVE && sScreen != SCR_SAVE)
            OpenSave();
        sScreen = screen;
        sPickMapsec = MAPSEC_NONE;
        return;
    }

    switch (sShown.screen)
    {
    case SCR_MAP:
        if (id == HIT_MAP)
            PickMapCell(sTouch.lastX, sTouch.lastY);
        break;
    case SCR_SAVE:
        if (id == HIT_YES && FieldIdle())
        {
            if (sSaveStep == SAVE_ASK && gSaveFileStatus != SAVE_STATUS_EMPTY && gSaveFileStatus != SAVE_STATUS_CORRUPT)
            {
                sSaveStep = SAVE_OVERWRITE;
                StringExpandPlaceholders(sSaveMessage, gDifferentSaveFile ? gText_DifferentSaveFile
                                                                         : gText_AlreadySavedFile);
            }
            else
                DoSave();
        }
        else if (id == HIT_NO || id == HIT_OK)
        {
            OpenSave();
            sScreen = SCR_MAP;
        }
        break;
    case SCR_OPTION:
        if (id >= HIT_OPTION && id < HIT_OPTION + 2 * HIT_OPTION_BACK)
            ActivateOption(id);
        break;
    }
}

/* Returns the id to show as pressed. */
static u8 ProcessTouch(u8 mode)
{
    const CtrInput *in = CtrInput_Get();

    if (mode != sShown.mode)
    {
        /* A touch that began on another screen does not act on this one. */
        if (sTouch.bag)
        {
            CtrBag_Touch(BAG_TOUCH_CANCEL, 0, 0);
            CtrPokedex_Touch(BAG_TOUCH_CANCEL, 0, 0);
        }
        sTouch.active = FALSE;
        sTouch.bag = FALSE;
        return HIT_NONE;
    }
    /* The game's bag: its picture in the middle of its area, the touch in
     * pixels of it, as it goes. */
    if ((in->touchDown && BagShown(mode) && (sShown.bagView == BAG_VIEW_WHOLE || in->touchX < CW))
     || (sTouch.bag && sTouch.active && BagShown(mode)))
    {
        int ox = sShown.bagView == BAG_VIEW_WHOLE ? (W - 240) / 2 : 0, oy = (H - 160) / 2;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrBag_Touch(BAG_TOUCH_DOWN, in->touchX - ox, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrBag_Touch(BAG_TOUCH_MOVE, in->touchX - ox, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrBag_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    /* The game's Pokédex, the same way: its picture in the middle of the
     * area left of the column. */
    if ((in->touchDown && DexShown(mode) && in->touchX < CW) || (sTouch.bag && sTouch.active && DexShown(mode)))
    {
        int oy = (H - 160) / 2;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrPokedex_Touch(BAG_TOUCH_DOWN, in->touchX, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrPokedex_Touch(BAG_TOUCH_MOVE, in->touchX, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrPokedex_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    if (in->touchDown)
    {
        sTouch.active = TRUE;
        sTouch.dragged = FALSE;
        sTouch.startX = sTouch.lastX = in->touchX;
        sTouch.startY = sTouch.lastY = in->touchY;
        sTouch.pressed = HitTest(in->touchX, in->touchY);
        sOptionScrollStart = sOptionScroll;
    }
    else if (in->touchActive && sTouch.active)
    {
        int dy = in->touchY - sTouch.startY, dx = in->touchX - sTouch.startX;

        sTouch.lastX = in->touchX;
        sTouch.lastY = in->touchY;
        if (!sTouch.dragged && (dy > 8 || dy < -8 || dx > 8 || dx < -8))
            sTouch.dragged = TRUE;
        if (sTouch.dragged && sScreen == SCR_OPTION && sTouch.startX < CW)
            OptionsDrag(dy);
    }
    else if (in->touchUp && sTouch.active)
    {
        sTouch.active = FALSE;
        /* The game's party menu, its picture in the middle of its area: the
         * tap goes to whatever waits for input in it (a mon, the buttons, its
         * menu, a question, a message), in pixels of that picture. The
         * column's buttons are ours. */
        if (mode == MODE_PARTY_MENU && (sShown.bagView == BAG_VIEW_WHOLE || sTouch.startX < CW))
        {
            int ox = sShown.bagView == BAG_VIEW_WHOLE ? (W - 240) / 2 : 0, oy = (H - 160) / 2;

            if (!sTouch.dragged)
                CtrMenu_PostTap(sTouch.lastX - ox, sTouch.lastY - oy);
            return HIT_NONE;
        }
        /* The boxes have the whole screen, their picture in the middle of it,
         * and take the tap themselves, in pixels of that picture. */
        if (mode == MODE_STORAGE)
        {
            int x = sTouch.lastX - (W - 240) / 2, y = sTouch.lastY - (H - 160) / 2;

            /* Or a summary opened from them, the same way. */
            if (!sTouch.dragged && CtrStorage_IsOpen())
                CtrStorage_Tap(x, y);
            else if (!sTouch.dragged)
                CtrSummary_Tap(x, y);
            return HIT_NONE;
        }
        if (mode == MODE_POKENAV && sTouch.startX < CW)
        {
            if (!sTouch.dragged)
                NavTap(sTouch.lastX, CtrVideo_BottomPictureY(sTouch.lastY));
            else if (sTouch.lastY - sTouch.startY > 24 || sTouch.lastY - sTouch.startY < -24)
                NavSwipe(sTouch.lastY - sTouch.startY);
            return HIT_NONE;
        }
        if ((!sTouch.dragged || sTouch.pressed == HIT_MAP) && HitTest(sTouch.lastX, sTouch.lastY) == sTouch.pressed)
            Activate(sTouch.pressed, mode);
        return HIT_NONE;
    }
    if (!sTouch.active || sTouch.dragged)
        return HIT_NONE;
    return sTouch.pressed;
}

