/* ------------------------------------------------------------------------ */
/* Drawing: the button column                                               */
/* ------------------------------------------------------------------------ */

enum { PLATE_NORMAL, PLATE_CHOSEN, PLATE_PRESSED, PLATE_OFF };

static void DrawColumnButton(const ViewState *s, int i, u8 state)
{
    const u8 *labels[SCR_COUNT] = {
        Ascii("MAP"), gText_MenuPokemon, gText_MenuBag, s->name, gText_MenuPokedex, gText_MenuPokenav,
        gText_MenuSave, gText_MenuOption,
    };
    const Icon *icon = &sRes.column[i];
    int y = PLATE_Y(i), dy = state == PLATE_PRESSED;
    bool8 on = state == PLATE_CHOSEN || state == PLATE_PRESSED;
    const PlateLook *look = state == PLATE_OFF ? &sLook.off : on ? &sLook.chosen : &sLook.plate;

    DrawPlate(PLATE_X, y, PLATE_W, PLATE_H, look, PLATE_SOCKET, dy);
    /* The icon's visible pixels centred on the socket; none on a button not
     * available yet. */
    if (icon->tiles && state != PLATE_OFF)
        DrawSpriteCentred(icon->tiles, icon->size, icon->size, 2 * PLATE_X + PLATE_SOCKET, 2 * (y + dy) + 21,
                          icon->pal.c);
    DrawStrIn(&sSmall, labels[i], PLATE_X + PLATE_SOCKET, PLATE_X + PLATE_W - 1, y + 1 + dy, y + PLATE_H - 2 + dy,
              state == PLATE_OFF ? sLook.offText : on ? TXT_WHITE : TXT_DARK,
              state == PLATE_OFF ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LIGHT);
}

/* The RUN button's lamp: lit while running is the default. */
static void DrawLed(int x, int y, bool8 on)
{
    FillRect(x + 1, y, 2, 1, sLook.led[LED_EDGE]);
    FillRect(x + 1, y + 3, 2, 1, sLook.led[LED_EDGE]);
    FillRect(x, y + 1, 1, 2, sLook.led[LED_EDGE]);
    FillRect(x + 3, y + 1, 1, 2, sLook.led[LED_EDGE]);
    FillRect(x + 1, y + 1, 2, 2, sLook.led[on ? LED_ON : LED_OFF]);
    Put(x + 2, y + 1, sLook.led[on ? LED_ON_LIGHT : LED_OFF_LIGHT]);
}

/*
 * Y, the registered item (the 3DS's Y is SELECT): the item's own icon under
 * a red badge like the bag's SEL, grey with nothing registered. RUN: the
 * shoe and its lamp; on, the face takes the socket's green and the shoe its
 * speed lines. Neither is cached: they follow the save and the settings.
 */
static void DrawSquares(const ViewState *s)
{
    for (int k = 0; k < 2; ++k)
    {
        int x = SQUARE_X(k), id = HIT_COLUMN + COL_Y + k;
        bool8 pressed = s->pressed == id;
        bool8 available = k == 0 ? s->registered != ITEM_NONE : (s->run & 2) != 0;
        bool8 on = k == 1 && (s->run & 1);
        int dy = pressed;
        const PlateLook *look = !available ? &sLook.off : pressed ? &sLook.chosen : on ? &sLook.run : &sLook.plate;

        DrawPlate(x, SQUARE_Y, SQUARE_SIZE, SQUARE_SIZE, look, 0, pressed);
        if (k == 0 && available)
        {
            int slot = ItemIcon(s->registered);

            if (slot >= 0)
                DrawSpriteCentred(sItemIcons[slot].tiles, 3, 3, 2 * x + SQUARE_SIZE, 2 * (SQUARE_Y + 23 + dy),
                                  sItemIcons[slot].pal.c);
        }
        if (k == 0)
            DrawSprite(sArtBadgeY, 2, 2, x + 2, SQUARE_Y + 2 + dy, sLook.art.c);
        if (k == 1 && available)
        {
            DrawSpriteCentred(on ? sArtShoeOn : sArtShoeOff, 3, 3, 2 * x + SQUARE_SIZE, 2 * (SQUARE_Y + 23 + dy),
                              sLook.art.c);
            DrawLed(x + SQUARE_SIZE - 7, SQUARE_Y + 3 + dy, on);
        }
        if (s->focus == COL_Y + k)
            DrawRing(x, SQUARE_Y, SQUARE_SIZE, SQUARE_SIZE, s->blink);
        /* The two halves of the column below the groove. */
        if (available)
            AddHit(k == 0 ? COL_X : SQUARE_X(1) - 1, GROOVE_Y + 2,
                   k == 0 ? SQUARE_X(1) - 1 - COL_X : W - SQUARE_X(1) + 1, H - GROOVE_Y - 2, id);
    }
}

/*
 * The column changes far less often than what is beside it: it is kept,
 * unpressed, in every background cache, and a redraw only paints the chosen,
 * pressed and focused buttons over it. The caches are repainted when which
 * entries exist changes, or the player's name does.
 */
static void DrawColumn(const ViewState *s)
{
    static int cachedMask = -1;
    static u8 cachedName[PLAYER_NAME_LENGTH + 1];

    if (cachedMask != s->enabled || memcmp(cachedName, s->name, sizeof(cachedName)) != 0)
    {
        u16 *canvas = sDst;
        int clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};

        cachedMask = s->enabled;
        memcpy(cachedName, s->name, sizeof(cachedName));
        /* The caches are whole pictures, whatever part this redraw is of. */
        sClipX0 = sClipY0 = 0;
        sClipX1 = W;
        sClipY1 = H;
        for (int c = 0; c < CACHE_COUNT; ++c)
        {
            if (!sCache[c] || c == CACHE_WIDE || c == CACHE_BATTLE)
                continue;
            sDst = sCache[c];
            for (int i = 0; i < SCR_COUNT; ++i)
                DrawColumnButton(s, i, (s->enabled >> i) & 1 ? PLATE_NORMAL : PLATE_OFF);
        }
        sClipX0 = clip[0];
        sClipY0 = clip[1];
        sClipX1 = clip[2];
        sClipY1 = clip[3];
        sDst = canvas;
        /* The canvas started from the stale column: paint all of it. */
        for (int i = 0; i < SCR_COUNT; ++i)
            DrawColumnButton(s, i, (s->enabled >> i) & 1 ? PLATE_NORMAL : PLATE_OFF);
    }
    for (int i = 0; i < SCR_COUNT; ++i)
    {
        bool8 pressed = s->pressed == HIT_COLUMN + i, chosen = s->screen == i;

        if (!((s->enabled >> i) & 1))
            continue;
        if (pressed || chosen)
            DrawColumnButton(s, i, pressed ? PLATE_PRESSED : PLATE_CHOSEN);
        if (s->focus == i)
            DrawRing(PLATE_X, PLATE_Y(i), PLATE_W, PLATE_H, s->blink);
        AddHit(COL_X, PLATE_Y(i) - 1, W - COL_X, 24, HIT_COLUMN + i);
    }
    DrawSquares(s);
}

/* ------------------------------------------------------------------------ */
/* Drawing: party and summary                                               */
/* ------------------------------------------------------------------------ */

#if 0
/* The bottom screen's own party menu, summary and lower panel: replaced by the game's party menu
 * (party_menu.c, CTR_CENTRED_PARTY), which draws and takes touch itself. Kept out of the build until
 * the user agrees to delete it. */
static void SetPartyColor(Pal *pal, u8 offset, u8 id)
{
    pal->c[offset] = Rgb565(sRes.partyRaw[id]);
}

/* party_menu.c's LoadPartyBoxPalette, for the states shown here. */
static void PartyBoxPalette(Pal *pal, const MonView *m, bool8 selected)
{
    static const u8 offsets1[] = {4, 5, 6}, offsets2[] = {1, 7, 8};
    static const u8 normal1[] = {52, 53, 54}, normal2[] = {49, 55, 56};
    static const u8 sel1[] = {116, 117, 118}, sel2[] = {97, 103, 104};
    static const u8 faint1[] = {84, 85, 86}, faint2[] = {81, 87, 88};
    static const u8 selFaint1[] = {148, 149, 150};
    static const u8 noMon[] = {17, 27, 28}, noMonOffsets[] = {1, 11, 12};
    const u8 *ids1, *ids2;

    if (!m->species)
    {
        for (int i = 0; i < 3; ++i)
            SetPartyColor(pal, noMonOffsets[i], noMon[i]);
        return;
    }
    if (m->fainted)
        ids1 = selected ? selFaint1 : faint1, ids2 = selected ? sel2 : faint2;
    else
        ids1 = selected ? sel1 : normal1, ids2 = selected ? sel2 : normal2;
    for (int i = 0; i < 3; ++i)
    {
        SetPartyColor(pal, offsets1[i], ids1[i]);
        SetPartyColor(pal, offsets2[i], ids2[i]);
    }
}

/* Positions from party_menu.c's sPartyBoxInfoRects and sprite coordinates. */
typedef struct
{
    u8 nameX, nameY, levelX, levelY, genderX, genderY, hpX, hpY, maxHpX, maxHpY, barX, barY;
    s8 iconX, iconY, statusX, statusY;
} SlotLayout;

static const SlotLayout sMainLayout = {24, 11, 32, 20, 64, 20, 38, 37, 53, 37, 24, 35, -8, 0, 26, 24};
static const SlotLayout sWideLayout = {22, 3, 30, 12, 62, 12, 102, 12, 117, 12, 88, 10, -8, -6, 24, 15};
#endif

#if 0
static void DrawPartySlot(const MonView *m, int slot, int x, int y, bool8 selected)
{
    bool8 main = slot == 0;
    const SlotLayout *l = main ? &sMainLayout : &sWideLayout;
    const u8 *map;
    int w = main ? 10 : 18, h = main ? 7 : 3;
    Pal pal = sRes.partyPal[main ? 3 : 4];
    u16 fg, sh;

    if (main)
        map = m->isEgg && sRes.slotMainNoHp ? sRes.slotMainNoHp : sRes.slotMain;
    else if (!m->species)
        map = sRes.slotWideEmpty;
    else
        map = m->isEgg && sRes.slotWideNoHp ? sRes.slotWideNoHp : sRes.slotWide;
    PartyBoxPalette(&pal, m, selected);
    for (int ty = 0; ty < h; ++ty)
        for (int tx = 0; tx < w; ++tx)
            DrawTile(sRes.partyTiles + map[ty * w + tx] * 32, x + tx * 8, y + ty * 8, pal.c, FALSE, FALSE);
    if (!m->species)
        return;

    fg = pal.c[TEXT_COLOR_LIGHT_GRAY];
    sh = pal.c[TEXT_COLOR_DARK_GRAY];
    DrawStr(&sSmall, m->nick, x + l->nameX, y + l->nameY, fg, sh);
    if (!m->isEgg)
    {
        u8 text[16], *end;

        StringCopy(text, gText_LevelSymbol);
        StringAppend(text, Number(m->level, 3, STR_CONV_MODE_LEFT_ALIGN));
        DrawStr(&sSmall, text, x + l->levelX, y + l->levelY, fg, sh);
        if (m->gender == MON_MALE || m->gender == MON_FEMALE)
        {
            u8 color = m->gender == MON_MALE ? 59 : 75;
            DrawStr(&sSmall, m->gender == MON_MALE ? gText_MaleSymbol : gText_FemaleSymbol, x + l->genderX,
                    y + l->genderY, Rgb565(sRes.partyRaw[color]), Rgb565(sRes.partyRaw[color + 1]));
        }
        /* DisplayPartyPokemonHP and ...MaxHP both print a slash, overlapping. */
        StringCopy(text, Number(m->hp, 3, STR_CONV_MODE_RIGHT_ALIGN));
        end = text + StringLength(text);
        end[0] = CHAR_SLASH;
        end[1] = EOS;
        DrawStr(&sSmall, text, x + l->hpX, y + l->hpY, fg, sh);
        StringCopy(text, gText_Slash);
        StringAppend(text, Number(m->maxHp, 3, STR_CONV_MODE_RIGHT_ALIGN));
        DrawStr(&sSmall, text, x + l->maxHpX, y + l->maxHpY, fg, sh);
        DrawHpBar(x + l->barX, y + l->barY, 48, m->hp, m->maxHp, &pal);
        DrawStatusIcon(m->ailment, x + l->statusX, y + l->statusY);
    }
    /* All icons animate, as the one under the cursor does in the party
     * menu; a fainted mon's stays still, as the game keeps it. */
    AddMonIcon(m->iconSpecies, m->deoxys, x + l->iconX, y + l->iconY, m->fainted);
}

/* A message box across the lower panel: the text the hidden menu shows. */
static void DrawPanelMessage(const u8 *text, int y, int ht, bool8 tappable)
{
    DrawBox(BOX_MESSAGE, 0, y, CW / 8, ht);
    DrawStr(&sNormal, text, 18, y + 8, TXT_WHITE, TXT_DARK);
    if (tappable)
        AddHit(0, y, CW, ht * 8, HIT_PANEL);
}

/* The lower panel of the party and bag views, from y to the bottom. */
static void DrawPanel(const ViewState *s, int y, const u8 *hint)
{
    int ht = (H - y) / 8;

    switch (s->panel)
    {
    case PANEL_HINT:
        DrawBox(BOX_MESSAGE, 0, y, 20, ht);
        DrawStr(&sNormal, hint, 18, y + 8, TXT_WHITE, TXT_DARK);
        DrawLabelButton(164, y + 4, 9, ht - 1, gText_Cancel2, s->pressed == HIT_CANCEL, TRUE, HIT_CANCEL);
        break;
    case PANEL_MESSAGE:
        DrawPanelMessage(s->text, y, ht, TRUE);
        break;
    case PANEL_YESNO:
        DrawPanelMessage(s->text, y, ht - 4, FALSE);
        DrawLabelButton(0, H - 32, 15, 4, gText_Yes, s->pressed == HIT_YES, TRUE, HIT_YES);
        DrawLabelButton(120, H - 32, 15, 4, gText_No, s->pressed == HIT_NO, TRUE, HIT_NO);
        break;
    case PANEL_QUANTITY:
    {
        static const u8 up[] = {CHAR_UP_ARROW, EOS}, down[] = {CHAR_DOWN_ARROW, EOS};
        DrawPanelMessage(s->text, y, ht - 4, FALSE);
        DrawLabelButton(0, H - 32, 7, 4, up, s->pressed == HIT_UP, TRUE, HIT_UP);
        DrawLabelButton(56, H - 32, 7, 4, down, s->pressed == HIT_DOWN, TRUE, HIT_DOWN);
        DrawLabelButton(112, H - 32, 8, 4, Ascii("OK"), s->pressed == HIT_OK, TRUE, HIT_OK);
        DrawLabelButton(176, H - 32, 8, 4, gText_Cancel2, s->pressed == HIT_CANCEL, TRUE, HIT_CANCEL);
        break;
    }
    case PANEL_ACTIONS:
    {
        /* The game's submenu: two rows of buttons. */
        int cols = (s->menuCount + 1) / 2, cellW;
        if (cols < 1) cols = 1;
        cellW = (CW / cols) / 8;
        for (int i = 0; i < s->menuCount; ++i)
        {
            int cx = (i % cols) * cellW * 8, cy = y + (i / cols) * ((ht / 2) * 8);
            DrawLabelButtonFont(cellW >= 10 ? &sNormal : &sSmall, cx, cy, cellW, ht / 2, s->menuNames[i],
                                s->pressed == HIT_MENU + i, TRUE, HIT_MENU + i);
        }
        break;
    }
    }
}

static void DrawParty(const ViewState *s)
{
    /* One main slot and five wide ones, as on the GBA, in the 240px view. */
    const int mainX = 8, wideX = 94, wideTop = 8, gap = 8;

    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        int x = i == 0 ? mainX : wideX;
        int y = i == 0 ? wideTop + 16 : wideTop + (i - 1) * (24 + gap);
        int hitW = i == 0 ? 80 : 144, hitH = i == 0 ? 56 : 24;

        DrawPartySlot(&s->party[i], i, x, y, s->partyCursor == i || s->pressed == HIT_SLOT + i);
        if (s->party[i].species)
            AddHit(x - 8, y - 4, hitW + 8, hitH + 8, HIT_SLOT + i);
    }
    DrawPanel(s, 168, gText_ChoosePokemon);
}

static void DrawSummary(const ViewState *s)
{
    static const char *const statNames[6] = {"HP", "ATTACK", "DEFENSE", "SP. ATK", "SP. DEF", "SPEED"};
    const MonView *m = &s->party[s->summary];
    u8 text[24];

    /* Who. */
    DrawBox(BOX_MENU, 0, 0, 30, 7);
    AddMonIcon(m->iconSpecies, m->deoxys, 6, 8, FALSE);
    DrawStr(&sNormal, m->nick, 44, 8, TXT_DARK, TXT_LIGHT);
    if (m->gender == MON_MALE || m->gender == MON_FEMALE)
        DrawStr(&sNormal, m->gender == MON_MALE ? gText_MaleSymbol : gText_FemaleSymbol,
                44 + StrWidth(&sNormal, m->nick) + 4, 8, m->gender == MON_MALE ? TXT_BLUE : TXT_RED,
                m->gender == MON_MALE ? TXT_LBLUE : TXT_LRED);
    StringCopy(text, gText_LevelSymbol);
    StringAppend(text, Number(m->level, 3, STR_CONV_MODE_LEFT_ALIGN));
    DrawStrRight(&sNormal, text, 228, 8, TXT_DARK, TXT_LIGHT);
    DrawStr(&sSmall, gSpeciesNames[m->species], 44, 26, TXT_DARK, TXT_LIGHT);
    DrawTypeIcon(s->types[0], 150, 24);
    if (s->types[1] != s->types[0])
        DrawTypeIcon(s->types[1], 186, 24);
    if (s->heldItem)
    {
        DrawItemIcon(s->heldItem, 12, 30);
        DrawStr(&sSmall, GetItemName(s->heldItem), 44, 40, TXT_DARK, TXT_LIGHT);
    }

    /* Stats, nature and ability. */
    DrawBox(BOX_MENU, 0, 56, 30, 10);
    for (int i = 0; i < 6; ++i)
    {
        int x = 12 + (i % 2) * 112, y = 64 + (i / 2) * 14;
        DrawStr(&sSmall, Ascii(statNames[i]), x, y, TXT_DARK, TXT_LIGHT);
        if (i == 0)
        {
            StringCopy(text, Number(m->hp, 3, STR_CONV_MODE_LEFT_ALIGN));
            StringAppend(text, gText_Slash);
            StringAppend(text, Number(s->stats[0], 3, STR_CONV_MODE_LEFT_ALIGN));
            DrawStrRight(&sSmall, text, x + 100, y, TXT_DARK, TXT_LIGHT);
        }
        else
            DrawStrRight(&sSmall, Number(s->stats[i], 3, STR_CONV_MODE_LEFT_ALIGN), x + 100, y, TXT_DARK, TXT_LIGHT);
    }
    DrawStr(&sSmall, gNatureNamePointers[s->nature], 12, 108, TXT_BLUE, TXT_LBLUE);
    DrawStr(&sSmall, gAbilityNames[s->ability], 124, 108, TXT_BLUE, TXT_LBLUE);

    /* Moves. */
    DrawBox(BOX_MENU, 0, 136, 30, 10);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        int y = 142 + i * 16;
        if (!s->moves[i])
            continue;
        DrawTypeIcon(gBattleMoves[s->moves[i]].type, 10, y);
        DrawStr(&sSmall, gMoveNames[s->moves[i]], 48, y + 1, TXT_DARK, TXT_LIGHT);
        StringCopy(text, gText_MoveInterfacePP);
        StringAppend(text, Ascii(" "));
        StringAppend(text, Number(s->pp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
        StringAppend(text, gText_Slash);
        StringAppend(text, Number(s->maxPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
        DrawStrRight(&sSmall, text, 228, y + 1, TXT_DARK, TXT_LIGHT);
    }

    /* Previous, back, next. */
    {
        static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
        DrawLabelButton(0, 216, 7, 3, left, s->pressed == HIT_PREV, TRUE, HIT_PREV);
        DrawLabelButton(56, 216, 16, 3, gText_Cancel2, s->pressed == HIT_BACK, TRUE, HIT_BACK);
        DrawLabelButton(184, 216, 7, 3, right, s->pressed == HIT_NEXT, TRUE, HIT_NEXT);
    }
}

#endif /* old party menu */

/* ------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------ */
/* Drawing: region map                                                    */
/* ------------------------------------------------------------------------ */

#define MAP_ORIGIN_X 8
#define MAP_ORIGIN_Y 8

static void DrawMapTile8(u8 tile, int x, int y)
{
    const u8 *src;

    if (!sRes.mapTiles || tile >= sRes.mapTileCount)
        return;
    src = sRes.mapTiles + tile * 64;
    for (int py = 0; py < 8; ++py)
        for (int px = 0; px < 8; ++px)
        {
            /* The map's colours are loaded at palette 7: indices 112 up. */
            u8 v = src[py * 8 + px];
            if (v >= 112 && v < 144)
                Put(x + px, y + py, sRes.mapPal[v - 112]);
        }
}

/* The map picture itself, once, into its cache. */
static void BuildMapCache(void)
{
    if (!sCache[CACHE_MAP])
        return;
    memcpy(sCache[CACHE_MAP], sCache[CACHE_MENU], sizeof(sCanvas));
    sDst = sCache[CACHE_MAP];
    FillRect(0, 0, CW, H, sRes.mapPal[0]);
    /* The map is a 64x64 affine map; the ocean around Hoenn is tile 0. */
    for (int ty = 0; ty < 20; ++ty)
        for (int tx = 0; tx < CW / 8; ++tx)
        {
            u8 tile = sRes.mapMap ? sRes.mapMap[ty * 64 + tx] : 0;
            DrawMapTile8(tile, MAP_ORIGIN_X + tx * 8, MAP_ORIGIN_Y + ty * 8);
        }
    sDst = sCanvas;
}

static void DrawRegionName(const ViewState *s)
{
    u8 name[32];
    bool8 picked = s->pickMapsec != MAPSEC_NONE;
    u8 mapsec = picked ? s->pickMapsec : s->mapsec;

    if (mapsec == MAPSEC_NONE)
        return;
    GetMapName(name, mapsec, 0);
    DrawBoxEx(BOX_MENU, 8, 188, 28, 5, picked);
    DrawStrCentered(&sNormal, name, CW / 2, 200, LABEL_FG(picked), LABEL_SH(picked));
}

static void DrawRegionMap(const ViewState *s)
{
    bool8 picked = s->pickMapsec != MAPSEC_NONE;

    if (s->mapsec != MAPSEC_NONE && sRes.playerIcon[s->gender])
        DrawSprite(sRes.playerIcon[s->gender], 2, 2, MAP_ORIGIN_X + s->cursorX * 8 - 4,
                   MAP_ORIGIN_Y + s->cursorY * 8 - 4, sRes.playerIconPal[s->gender].c);
    if (picked && sRes.cursorTiles)
        DrawSprite(sRes.cursorTiles, 2, 2, MAP_ORIGIN_X + s->pickX * 8 - 4, MAP_ORIGIN_Y + s->pickY * 8 - 4,
                   sRes.cursorPal.c);
    AddHit(0, 0, CW, 176, HIT_MAP);

    DrawRegionName(s);
}

/* The section under a tapped map cell, as the region map's cursor finds it. */
static void PickMapCell(int x, int y)
{
    int cx = (x - MAP_ORIGIN_X + 64) / 8 - 8, cy = (y - MAP_ORIGIN_Y + 64) / 8 - 8;

    sPickMapsec = MAPSEC_NONE;
    for (int i = 0; i < MAPSEC_NONE; ++i)
    {
        const struct RegionMapLocation *e = &gRegionMapEntries[i];
        int ex = e->x + 1, ey = e->y + 2;
        if (e->width && cx >= ex && cy >= ey && cx < ex + e->width && cy < ey + e->height)
        {
            sPickMapsec = i;
            sPickX = cx;
            sPickY = cy;
            return;
        }
    }
}
/* ------------------------------------------------------------------------ */
/* Drawing: trainer card                                                    */
/* ------------------------------------------------------------------------ */

#define CARD_X 0
#define CARD_Y 40

static void CardPals(Pal *pals, u8 stars, u8 gender)
{
    memcpy(pals, sRes.cardPal[stars], sizeof(Pal) * 3);
    if (gender)
        pals[1] = sRes.cardFemaleBg;
    pals[3] = sRes.badgePal;
    pals[4] = sRes.starPal;
}

/* The card's background stripes and front, by star count and gender. */
static void BuildCardCache(u8 stars, u8 gender)
{
    Pal pals[5];

    if (!sCache[CACHE_CARD] || sCardCacheKey == stars * 2 + gender)
        return;
    sCardCacheKey = stars * 2 + gender;
    CardPals(pals, stars, gender);
    memcpy(sCache[CACHE_CARD], sCache[CACHE_MENU], sizeof(sCanvas));
    sDst = sCache[CACHE_CARD];
    FillRect(0, 0, CW, H, pals[0].c[0]);
    if (sRes.cardBg)
        for (int ty = 0; ty < H / 8; ++ty)
            for (int tx = 0; tx < CW / 8; ++tx)
                DrawMapEntry(sRes.cardTiles, sRes.cardTileCount, sRes.cardBg[(ty % 20) * 30 + (tx % 30)],
                             tx * 8, ty * 8, pals);
    if (sRes.cardFront)
        for (int ty = 0; ty < 20; ++ty)
            for (int tx = 0; tx < 30; ++tx)
                DrawMapEntry(sRes.cardTiles, sRes.cardTileCount, sRes.cardFront[ty * 30 + tx],
                             CARD_X + tx * 8, CARD_Y + ty * 8, pals);
    if (sRes.trainerPic[gender])
        DrawSprite(sRes.trainerPic[gender], 8, 8, CARD_X + 20 * 8, CARD_Y + 5 * 8, sRes.trainerPicPal[gender].c);
    for (int i = 0; i < stars; ++i)
        DrawMapEntry(sRes.cardTiles, sRes.cardTileCount, 0x4000 | 143, CARD_X + (15 + i) * 8, CARD_Y + 7 * 8, pals);
    sDst = sCanvas;
}

static void DrawTrainerCard(const ViewState *s)
{
    u8 text[32];
    int bx = CARD_X + 8, by = CARD_Y + 8; /* WIN_CARD_TEXT is at tile (1,1) */

    /* trainer_card.c: PrintNameOnCardFront, PrintIdOnCard, PrintMoneyOnCard,
     * PrintPokedexOnCard, PrintTimeOnCard, DrawStarsAndBadgesOnCard. */
    StringCopy(StringCopy(text, gText_TrainerCardName), s->name);
    DrawStr(&sNormal, text, bx + 16, by + 33, TXT_DARK, TXT_LIGHT);

    StringCopy(StringCopy(text, gText_TrainerCardIDNo), Number(s->id, 5, STR_CONV_MODE_LEADING_ZEROS));
    DrawStr(&sNormal, text, bx + 120 + (96 - StrWidth(&sNormal, text)) / 2, by + 9, TXT_DARK, TXT_LIGHT);

    DrawStr(&sNormal, gText_TrainerCardMoney, bx + 16, by + 57, TXT_DARK, TXT_LIGHT);
    text[0] = CHAR_CURRENCY;
    StringCopy(text + 1, Number(s->money, 6, STR_CONV_MODE_LEFT_ALIGN));
    DrawStrRight(&sNormal, text, bx + 128, by + 57, TXT_DARK, TXT_LIGHT);

    if (s->hasDex)
    {
        DrawStr(&sNormal, gText_TrainerCardPokedex, bx + 16, by + 73, TXT_DARK, TXT_LIGHT);
        DrawStrRight(&sNormal, Number(s->dex, 3, STR_CONV_MODE_LEFT_ALIGN), bx + 128, by + 73, TXT_DARK, TXT_LIGHT);
    }

    DrawStr(&sNormal, gText_TrainerCardTime, bx + 16, by + 89, TXT_DARK, TXT_LIGHT);
    {
        int colon = StrWidth(&sNormal, gText_Colon2), x = bx + 128 - (colon + 30);
        DrawStr(&sNormal, Number(s->hours, 3, STR_CONV_MODE_RIGHT_ALIGN), x, by + 89, TXT_DARK, TXT_LIGHT);
        DrawStr(&sNormal, gText_Colon2, x + 18, by + 89, TXT_DARK, TXT_LIGHT);
        DrawStr(&sNormal, Number(s->minutes, 2, STR_CONV_MODE_LEADING_ZEROS), x + 18 + colon, by + 89, TXT_DARK,
                TXT_LIGHT);
    }

    if (sRes.badgeTiles)
        for (int i = 0; i < NUM_BADGES; ++i)
        {
            int x = CARD_X + (4 + 3 * i) * 8, y = CARD_Y + 15 * 8;
            if (!(s->badges & (1 << i)))
                continue;
            DrawTile(sRes.badgeTiles + (2 * i) * 32, x, y, sRes.badgePal.c, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 1) * 32, x + 8, y, sRes.badgePal.c, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 16) * 32, x, y + 8, sRes.badgePal.c, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 17) * 32, x + 8, y + 8, sRes.badgePal.c, FALSE, FALSE);
        }
}

/* ------------------------------------------------------------------------ */
/* Drawing: save, options, PokéNav                                          */
/* ------------------------------------------------------------------------ */

static void DrawSave(const ViewState *s)
{
    DrawBox(BOX_MESSAGE, 0, 40, 30, 8);
    DrawStr(&sNormal, s->text, 18, 52, TXT_WHITE, TXT_DARK);
    if (s->saveStep == SAVE_DONE)
    {
        DrawLabelButton(64, 136, 14, 5, Ascii("OK"), s->pressed == HIT_OK, TRUE, HIT_OK);
        return;
    }
    DrawLabelButton(8, 136, 13, 6, gText_Yes, s->pressed == HIT_YES, s->canSave, HIT_YES);
    DrawLabelButton(128, 136, 13, 6, gText_No, s->pressed == HIT_NO, TRUE, HIT_NO);
}

static const u8 *OptionValue(int row, u8 value)
{
    static u8 frame[16];

    switch (row)
    {
    case 0: return value == 0 ? gText_TextSpeedSlow : value == 1 ? gText_TextSpeedMid : gText_TextSpeedFast;
    case 1: return value ? gText_BattleSceneOff : gText_BattleSceneOn;
    case 2: return value ? gText_BattleStyleSet : gText_BattleStyleShift;
    case 3: return value ? gText_SoundStereo : gText_SoundMono;
    case 4: return value == 0 ? gText_ButtonTypeNormal : value == 1 ? gText_ButtonTypeLR : gText_ButtonTypeLEqualsA;
    case OPT_FPS:
    case OPT_VOXEL: return value ? gText_BattleSceneOn : gText_BattleSceneOff;
    case OPT_VOXEL_BLUR:
    case OPT_VOXEL_BATTLE: return value ? gText_BattleSceneOn : gText_BattleSceneOff;
    case OPT_VOXEL_PITCH: return Number(value, 2, STR_CONV_MODE_LEFT_ALIGN);
    case OPT_VOXEL_ZOOM:
        StringCopy(frame, Number(value, 3, STR_CONV_MODE_LEFT_ALIGN));
        frame[StringLength(frame) + 1] = EOS;
        frame[StringLength(frame)] = CHAR_PERCENT;
        return frame;
    default:
        StringCopy(frame, gText_FrameType);
        StringAppend(frame, Number(value + 1, 2, STR_CONV_MODE_LEFT_ALIGN));
        return frame;
    }
}

/* A window frame as the game draws its message boxes, from its 3x3 tiles,
 * round [x, x + 8 wt) x [y, y + 8 ht) filled with `fill`. */
static void DrawWindowFrame(u8 type, int x, int y, int wt, int ht, u16 fill)
{
    const struct TilesPal *frame = GetWindowFrameTilesPal(type);
    const u8 *tiles = frame ? Port_ResolveAssetPointer(frame->tiles) : NULL;
    const u16 *raw = frame ? Port_ResolveAssetPointer(frame->pal) : NULL;
    Pal pal;

    if (!tiles || !raw)
        return;
    ToPals(&pal, raw, 1);
    FillRect(x, y, wt * 8, ht * 8, fill);
    for (int ty = -1; ty <= ht; ++ty)
        for (int tx = -1; tx <= wt; ++tx)
        {
            int row = ty < 0 ? 0 : ty == ht ? 2 : 1, col = tx < 0 ? 0 : tx == wt ? 2 : 1;
            if (row == 1 && col == 1)
                continue;
            DrawTile(tiles + (row * 3 + col) * 32, x + tx * 8, y + ty * 8, pal.c, FALSE, FALSE);
        }
}

/*
 * One option's cell: its name on the first line and its value on the second,
 * both centred, the value between the arrows; the left half of the cell
 * steps the value back and the right half on. The frame's cell is drawn with
 * the chosen frame itself, so it is its own preview: one line inside it, the
 * name in the left half and the value between its arrows in the right.
 */
/*
 * A cell's plate: the name on the first line and the value on the second,
 * both centred, the value between the arrows when it has them.
 */
static void DrawCellPlate(int x, int y, const u8 *name, const u8 *value, bool8 live, bool8 on, bool8 arrows)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    u16 nameFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_DARK;
    u16 nameSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LIGHT;
    u16 valueFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_RED;
    u16 valueSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LRED;
    int capTop, capBottom, cap, line, dy = on, aw = StrWidth(&sSmall, left);

    DrawPlate(x, y, OPT_CELL_W, OPT_CELL_H, !live ? &sLook.off : on ? &sLook.chosen : &sLook.plate, 0, on);
    /* Two lines of capitals 3px apart, centred between the outline and
     * the shade rows. */
    CapRows(&sSmall, &capTop, &capBottom);
    cap = capBottom - capTop;
    line = y + 1 + (28 - (2 * cap + 3)) / 2 + dy;
    DrawStrIn(&sSmall, name, x + 1, x + OPT_CELL_W - 1, line, line + cap, nameFg, nameSh);
    line += cap + 3;
    DrawStrIn(&sSmall, value, x + 1, x + OPT_CELL_W - 1, line, line + cap, valueFg, valueSh);
    if (!arrows)
        return;
    DrawStrIn(&sSmall, left, x + 8, x + 8 + aw, line, line + cap, nameFg, nameSh);
    DrawStrIn(&sSmall, right, x + OPT_CELL_W - 8 - aw, x + OPT_CELL_W - 8, line, line + cap, nameFg, nameSh);
}

static void DrawOptionCell(const ViewState *s, int row, const u8 *name, bool8 voxel)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    const u8 *value = OptionValue(row, s->options[row]);
    bool8 live = OptionLive(row, voxel);
    bool8 on = live && (s->pressed == HIT_OPTION + row || s->pressed == HIT_OPTION + HIT_OPTION_BACK + row);
    u16 nameFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_DARK;
    u16 nameSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LIGHT;
    u16 valueFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_RED;
    u16 valueSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LRED;
    int x, y, dy = on, aw = StrWidth(&sSmall, left);

    OptionCell(row, &x, &y);
    if (row == OPT_FRAME)
    {
        int top = y + 8 + dy, bottom = y + 24 + dy, vx0, vx1, group, gx;

        DrawWindowFrame(s->options[OPT_FRAME], x + 8, y + 8, 12, 2, on ? sLook.chosen.f : TXT_WHITE);
        DrawStrIn(&sSmall, name, x + 8, x + 56, top, bottom, nameFg, nameSh);
        InkColumns(&sSmall, value, &vx0, &vx1);
        group = aw + 2 + (vx1 - vx0) + 2 + aw;
        gx = x + 56 + (48 - group) / 2;
        DrawStrIn(&sSmall, left, gx, gx + aw, top, bottom, nameFg, nameSh);
        DrawStrIn(&sSmall, value, gx + aw + 2, gx + aw + 2 + (vx1 - vx0), top, bottom, valueFg, valueSh);
        DrawStrIn(&sSmall, right, gx + group - aw, gx + group, top, bottom, nameFg, nameSh);
    }
    else
        DrawCellPlate(x, y, name, value, live, on, TRUE);
    if (live)
    {
        AddHit(x, y, OPT_CELL_W / 2, OPT_CELL_H, HIT_OPTION + HIT_OPTION_BACK + row);
        AddHit(x + OPT_CELL_W / 2, y, OPT_CELL_W / 2, OPT_CELL_H, HIT_OPTION + row);
    }
    if (s->optFocus == row)
        DrawRing(x, y, OPT_CELL_W, OPT_CELL_H, s->blink);
}

/* An extra's cell: its name, and its value between the arrows, or for an
 * action its one text without them. */
static void DrawExtraCell(const ViewState *s, int row, const CtrExtra *extra)
{
    bool8 on = s->pressed == HIT_OPTION + row || s->pressed == HIT_OPTION + HIT_OPTION_BACK + row;
    const char *value = extra->values ? extra->values[extra->count ? s->extras[row] : 0] : "";
    bool8 arrows = extra->count != 0 || extra->step != NULL;
    int x, y;

    OptionCell(row, &x, &y);
    DrawCellPlate(x, y, Ascii(extra->name), extra->text ? extra->text() : Ascii(value), TRUE, on, arrows);
    if (arrows)
        AddHit(x, y, OPT_CELL_W / 2, OPT_CELL_H, HIT_OPTION + HIT_OPTION_BACK + row);
    AddHit(x + (arrows ? OPT_CELL_W / 2 : 0), y, arrows ? OPT_CELL_W / 2 : OPT_CELL_W, OPT_CELL_H,
           HIT_OPTION + row);
    if (s->optFocus == row)
        DrawRing(x, y, OPT_CELL_W, OPT_CELL_H, s->blink);
}

/* The tabs: SETTINGS (the options) and each page that has extras. */
static void DrawOptionTabs(const ViewState *s)
{
    static const char *const names[CTR_EXTRAS_PAGES] = {"SETTINGS", "ENHANCEMENTS", "CHEATS"};
    u8 pages[CTR_EXTRAS_PAGES], count = 0;
    int capTop, capBottom, cap;

    for (unsigned page = 0; page < CTR_EXTRAS_PAGES; ++page)
        if (page == CTR_EXTRAS_OPTIONS || CtrExtras_PageUsed(page))
            pages[count++] = page;
    CapRows(&sSmall, &capTop, &capBottom);
    cap = capBottom - capTop;
    for (unsigned i = 0; i < count; ++i)
    {
        int x0 = 4 + i * 232 / count, x1 = 4 + (i + 1) * 232 / count - 4;
        bool8 chosen = s->optPage == pages[i], pressed = s->pressed == HIT_PAGE + pages[i];
        int line = TAB_Y + 1 + (TAB_H - 4 - cap) / 2 + pressed;

        DrawPlate(x0, TAB_Y, x1 - x0, TAB_H, chosen ? &sLook.chosen : &sLook.plate, 0, pressed);
        char label[24];

        /* "CHEATS 1/2" on a page shown twelve at a time. */
        if (chosen && PageSubs(pages[i]) > 1)
            snprintf(label, sizeof(label), "%s %u/%u", names[pages[i]], s->optSub + 1, PageSubs(pages[i]));
        else
            snprintf(label, sizeof(label), "%s", names[pages[i]]);
        DrawStrIn(&sSmall, Ascii(label), x0 + 1, x1 - 1, line, line + cap,
                  chosen ? TXT_WHITE : TXT_DARK, chosen ? sLook.chosenShadow : TXT_LIGHT);
        AddHit(x0, TAB_Y, x1 - x0, TAB_H, HIT_PAGE + pages[i]);
    }
}

static void DrawOptions(const ViewState *s)
{
    /* The tabs first: their labels take turns in Ascii's few buffers, which
     * the names below then hold until the cells are drawn. */
    if (OptionPages())
        DrawOptionTabs(s);

    const u8 *names[OPTION_ROWS] = {gText_TextSpeed, gText_BattleScene, gText_BattleStyle, gText_Sound,
                                    gText_ButtonMode, gText_Frame, Ascii("SHOW FPS"), Ascii("VOXEL 3D"),
                                    Ascii("3D ANGLE"), Ascii("3D ZOOM"), Ascii("3D BLUR"),
                                    Ascii("3D BATTLE")};
    bool8 voxel = OPTION_SHOWN > OPT_VOXEL && s->options[OPT_VOXEL];

    if (s->optPage != CTR_EXTRAS_OPTIONS)
    {
        for (unsigned row = 0; row < ARRAY_COUNT(s->extras); ++row)
        {
            const CtrExtra *extra = PageExtra(s->optPage, row);

            if (extra)
                DrawExtraCell(s, row, extra);
        }
        return;
    }
    for (int row = 0; row < OPTION_ROWS; ++row)
        if (OptionExists(row))
            DrawOptionCell(s, row, names[row], voxel);
}

/* ------------------------------------------------------------------------ */
/* Drawing: battle                                                          */
/* ------------------------------------------------------------------------ */

/*
 * The battle menus, over the whole screen on the battle backdrop (see
 * "Battle art"): FIGHT and the quick ball on top, BAG, POKéMON and RUN under
 * them; the four moves two by two with CANCEL; the target choice. While the
 * turn plays out, the backdrop alone: the top screen shows the battle and its
 * message. Labels are the game's strings and glyphs (DrawSmoothStr), item
 * and type icons the game's; FIGHT's watermark is Rayquaza's own picture.
 */

static const Rgb sHueFight = {230, 52, 56}, sHueBag = {240, 178, 36}, sHueMon = {36, 172, 96},
                 sHueRun = {40, 112, 226}, sHueBall = {140, 76, 200}, sHueCancel = {88, 104, 130},
                 sHueEmpty = {150, 150, 158};
static const Rgb sCream = {252, 245, 234}, sWhite = {255, 255, 255};
static const Rgb sFocusRing[2] = {{255, 120, 40}, {255, 206, 72}};

/* A colour at keep/100 of its brightness. */
static Rgb Shade(Rgb c, int keep)
{
    return (Rgb){(u8)(c.r * keep / 100), (u8)(c.g * keep / 100), (u8)(c.b * keep / 100)};
}

/* A plate's labels: outlined in its dark tone, shadowed darker still. */
#define PLATE_DARK(c) Shade(c, 38)
#define PLATE_SHADOW(c) Shade(c, 27)

static void DrawTypeIcon(u8 type, int x, int y)
{
    if (sRes.typeTiles && type < NUMBER_OF_MON_TYPES)
        DrawSprite(sRes.typeTiles + type * 8 * 32, 4, 2, x, y, sRes.typePal[sTypeIconPal[type]].c);
}

static u8 PlateState(const ViewState *s, u8 hit, bool8 focused)
{
    if (s->pressed == hit && hit != HIT_NONE)
        return BTA_PRESSED;
    return focused ? BTA_FOCUS : BTA_NORMAL;
}

/*
 * A plate in a colour, worked out once: the same plate in the same colour
 * (and ring) is then copied. Stored a column at a time in the canvas' own
 * order (bottom to top), each pixel as its coverage and the colour it adds;
 * the run of each column the plate covers whole is copied with memcpy, only
 * its edges and shadow are blended. FIGHT's has Rayquaza in it already.
 */
#define TINT_SLOTS 40

typedef struct
{
    u32 age;
    u8 id;
    Rgb colour, ring;
    s16 x, y;
    u16 w, h;
    u16 *p;          /* premultiplied RGB565, w columns of h */
    u8 *a;           /* coverage, the same way */
    u8 *run;         /* per column: the opaque run's first and end index */
} Tint;

static Tint sTints[TINT_SLOTS];
static u32 sTintClock;

static bool8 SameRgb(Rgb a, Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static bool8 BuildTint(Tint *t, u8 id, Rgb colour, Rgb ring)
{
    const BtaElem *e = &sBta.e[id];
    bool8 wide = id >= BTA_FIGHT_WIDE && id < BTA_FIGHT_WIDE + 3, full = id >= BTA_FIGHT_FULL && id < BTA_FIGHT_FULL + 3;
    u8 clip = wide ? BTA_CLIP_FIGHT_WIDE : BTA_CLIP_FIGHT_FULL;
    int sink = (wide && id - BTA_FIGHT_WIDE == BTA_PRESSED) || (full && id - BTA_FIGHT_FULL == BTA_PRESSED) ? BTA_SINK : 0;
    int plateW = wide ? 224 : 284;
    const Rgb body = Shade(sHueFight, 82), lines = Shade(sHueFight, 66);

    free(t->p);
    t->p = malloc((size_t)e->w * e->h * 3 + e->w * 2);
    if (!t->p)
        return FALSE;
    t->a = (u8 *)(t->p + e->w * e->h);
    t->run = t->a + e->w * e->h;
    t->id = id;
    t->colour = colour;
    t->ring = ring;
    t->x = e->x;
    t->y = e->y;
    t->w = e->w;
    t->h = e->h;
    for (int i = 0; i < e->w; ++i)
    {
        int best0 = 0, best1 = 0, start = -1;

        for (int k = 0; k <= e->h; ++k)
        {
            int al = 0;

            if (k < e->h)
            {
                int j = e->h - 1 - k, o = i * e->h + k;
                u32 q = (u32)j * e->w + i;
                int A = e->a[q], R = e->r ? e->r[q] : 0;
                Rgb add = RgbOf(e->c[q * 2] | (e->c[q * 2 + 1] << 8));
                int r = Div255(colour.r * A + ring.r * R) + add.r, g = Div255(colour.g * A + ring.g * R) + add.g,
                    b = Div255(colour.b * A + ring.b * R) + add.b;

                al = e->alpha[q];
                /* FIGHT's watermark, over the colour, inside it. */
                if (wide || full)
                {
                    int px = e->x + i, py = e->y + j, rx = px - (plateW - 84), ry = py - (sink - 1);

                    if (rx >= 0 && ry >= 0 && rx < RAY_SIZE && ry < RAY_SIZE)
                    {
                        int c = BtaAlpha(clip, px, py - sink);
                        int cb = Div255(Div255(sRayBody[ry * RAY_SIZE + rx] * c) * 217);
                        int cl = Div255(Div255(sRayLines[ry * RAY_SIZE + rx] * c) * 153);

                        r = Div255(r * (255 - cb) + body.r * cb);
                        g = Div255(g * (255 - cb) + body.g * cb);
                        b = Div255(b * (255 - cb) + body.b * cb);
                        r = Div255(r * (255 - cl) + lines.r * cl);
                        g = Div255(g * (255 - cl) + lines.g * cl);
                        b = Div255(b * (255 - cl) + lines.b * cl);
                    }
                }
                t->p[o] = PackRgb(r, g, b);
                t->a[o] = al;
            }
            if (k < e->h && al == 255)
            {
                if (start < 0)
                    start = k;
            }
            else if (start >= 0)
            {
                if (k - start > best1 - best0)
                    best0 = start, best1 = k;
                start = -1;
            }
        }
        /* Columns are at most a plate's height: the run fits a byte each. */
        t->run[i * 2] = best0;
        t->run[i * 2 + 1] = best1;
    }
    return TRUE;
}

static const Tint *GetTint(u8 id, Rgb colour, Rgb ring)
{
    Tint *victim = &sTints[0];

    if (id >= BTA_COUNT || !sBta.e[id].alpha || !sBta.e[id].a || sBta.e[id].h > 255)
        return NULL;
    for (int i = 0; i < TINT_SLOTS; ++i)
    {
        Tint *t = &sTints[i];

        /* The ring only shows on a focused plate. */
        if (t->p && t->id == id && SameRgb(t->colour, colour) && (!sBta.e[id].r || SameRgb(t->ring, ring)))
        {
            t->age = ++sTintClock;
            return t;
        }
        if (t->age < victim->age)
            victim = t;
    }
    if (!TakeBuildBudget())
        return NULL;
    if (!BuildTint(victim, id, colour, ring))
    {
        victim->age = 0;
        return NULL;
    }
    victim->age = ++sTintClock;
    return victim;
}

static void DrawTint(const Tint *t, int ax, int ay)
{
    int x0 = ax + t->x + sOX, y0 = ay + t->y;
    int i0 = sClipX0 - x0 > 0 ? sClipX0 - x0 : 0, i1 = sClipX1 - x0 < t->w ? sClipX1 - x0 : t->w;
    /* Rows y0 .. y0 + h - 1 are column indices h - 1 .. 0 (bottom up). */
    int k0 = y0 + t->h - sClipY1, k1 = y0 + t->h - sClipY0;

    if (k0 < 0) k0 = 0;
    if (k1 > t->h) k1 = t->h;
    for (int i = i0; i < i1; ++i)
    {
        u16 *col = sDst + (x0 + i) * H + (H - y0 - t->h);
        const u16 *p = t->p + i * t->h;
        const u8 *a = t->a + i * t->h;
        int r0 = t->run[i * 2], r1 = t->run[i * 2 + 1];

        if (r0 < k0) r0 = k0;
        if (r1 > k1) r1 = k1;
        if (r0 < r1)
            memcpy(col + r0, p + r0, (r1 - r0) * sizeof(u16));
        else
            r0 = r1 = k1;
        for (int k = k0; k < k1; ++k)
        {
            Rgb under, add;
            int al;

            if (k == r0)
            {
                k = r1 - 1;
                continue;
            }
            al = a[k];
            if (!al && !p[k])
                continue;
            if (al == 255)
            {
                col[k] = p[k];
                continue;
            }
            under = RgbOf(col[k]);
            add = RgbOf(p[k]);
            col[k] = PackRgb(Div255(under.r * (255 - al)) + add.r, Div255(under.g * (255 - al)) + add.g,
                             Div255(under.b * (255 - al)) + add.b);
        }
    }
}

/* A plate, its hit added; returns the y its contents start at (lower when
 * pressed). Without the art, a flat box in its colour. */
static int DrawBattlePlate(u8 id, int x, int y, int w, int h, Rgb colour, u8 state, u8 blink, u8 hit)
{
    const Tint *t = GetTint(id + state, colour, sFocusRing[blink & 1]);

    if (t)
        DrawTint(t, x, y);
    else if (!sBta.e[id + state].alpha)
        FillRect(x, y, w, h, PackRgb(colour.r, colour.g, colour.b));
    if (hit != HIT_NONE)
        AddHit(x, y, w, h, hit);
    return y + (state == BTA_PRESSED ? BTA_SINK : 0);
}

/* The index-th of the four words of the game's action menu text, split where
 * the game moves on to the next ({CLEAR_TO} or a new line). */
static const u8 *ActionLabel(bool8 safari, int index)
{
    static u8 out[4][24];
    const u8 *src = safari ? gText_SafariZoneMenu : gText_BattleMenu;
    int part = 0, n = 0;

    while (*src != EOS && part <= index)
    {
        if (*src == EXT_CTRL_CODE_BEGIN || *src == CHAR_NEWLINE)
        {
            if (*src++ == EXT_CTRL_CODE_BEGIN && *src != EOS)
                src += GetExtCtrlCodeLength(*src);
            if (n || part < index)
                ++part;
            continue;
        }
        if (part == index && n < (int)sizeof(out[0]) - 1)
            out[index][n++] = *src;
        ++src;
    }
    out[index][n] = EOS;
    return out[index];
}

/* "×N", the game's way of counting items. */
static const u8 *Times(u32 n)
{
    static u8 out[2][8];
    static u8 next;
    u8 *text = out[next++ & 1];

    text[0] = CHAR_MULT_SIGN;
    StringCopy(text + 1, Number(n, 3, STR_CONV_MODE_LEFT_ALIGN));
    return text;
}

#define ACT_Y 32
#define ACT_H 74
#define ROW_Y 138
#define ROW_W 100
#define ROW_H 72

static void DrawFightPlate(const ViewState *s, bool8 wide)
{
    int x = wide ? 6 : 18, w = wide ? 224 : 284;
    u8 state = PlateState(s, HIT_ACTION + 0, s->cursor == 0);
    int oy = DrawBattlePlate(wide ? BTA_FIGHT_WIDE : BTA_FIGHT_FULL, x, ACT_Y, w, ACT_H, sHueFight, state, s->blink,
                             HIT_ACTION + 0);
    Rgb dark = PLATE_DARK(sHueFight), shadow = PLATE_SHADOW(sHueFight);
    const u8 *label = ActionLabel(s->safari, 0);

    /* Rayquaza is in the plate's tint (BuildTint). */
    if (s->safari)
    {
        DrawItemIcon(ITEM_SAFARI_BALL, x + w / 2 - 34, oy + ACT_H / 2 - 15);
        DrawSmoothStr(&sNormal, label, x + w / 2 - 4, oy + ACT_H / 2 - 16, 8, sCream, dark, &shadow);
        DrawSmoothStr(&sSmall, Times(s->safariBalls), x + w / 2 + 52, oy + ACT_H / 2 + 4, 4, sWhite, dark, NULL);
        return;
    }
    DrawSmoothStr(&sNormal, label, x + (60 + w - 64) / 2 - SmoothInkWidth(&sNormal, label, 10) / 2,
                  oy + ACT_H / 2 - 21, 10, sCream, dark, &shadow);
}

static void DrawQuickBall(const ViewState *s)
{
    int x = 236, w = 78;
    u8 state = PlateState(s, HIT_QUICK_BALL, s->cursor == 4);
    int oy = DrawBattlePlate(BTA_BALL, x, ACT_Y, w, ACT_H, sHueBall, state, s->blink, HIT_QUICK_BALL);
    Rgb dark = PLATE_DARK(sHueBall), shadow = PLATE_SHADOW(sHueBall);
    const u8 *name = GetItemName(s->quickBall), *count = Times(s->quickBallCount);

    DrawItemIconShadow(s->quickBall, x + 4, oy + ACT_H / 2 - 11);
    DrawItemIcon(s->quickBall, x + 3, oy + ACT_H / 2 - 13);
    DrawSmoothStr(&sSmall, name, x + w - 5 - SmoothInkWidth(&sSmall, name, 4), oy + 9, 4, sWhite, dark, NULL);
    DrawSmoothStr(&sNormal, count, x + w - 8 - SmoothInkWidth(&sNormal, count, 4), oy + ACT_H - 28, 4, sCream, dark,
                  &shadow);
}

static void DrawBattleActions(const ViewState *s)
{
    static const s16 rowX[3] = {6, 110, 214};
    const Rgb hues[3] = {sHueBag, sHueMon, sHueRun};
    bool8 ball = !s->safari && s->quickBall != ITEM_NONE;

    DrawFightPlate(s, ball);
    if (ball)
        DrawQuickBall(s);
    for (int k = 0; k < 3; ++k)
    {
        int x = rowX[k], idx = k + 1;
        u8 state = PlateState(s, HIT_ACTION + idx, s->cursor == idx);
        int oy = DrawBattlePlate(BTA_BOTTOM, x, ROW_Y, ROW_W, ROW_H, hues[k], state, s->blink, HIT_ACTION + idx);
        const u8 *label = ActionLabel(s->safari, idx);

        if (k == 1 && !s->safari)
            for (int b = 0; b < PARTY_SIZE; ++b)
                DrawBta(BTA_PARTY_OK + s->partyBalls[b], x + ROW_W - 82 + b * 14, oy + 18, hues[k], hues[k]);
        else if (s->safari)
            DrawBta(k == 0 ? BTA_ICON_BLOCK : k == 1 ? BTA_ICON_NEAR : BTA_ICON_RUN, x, oy, hues[k], hues[k]);
        else
            DrawBta(k == 0 ? BTA_ICON_BAG : BTA_ICON_RUN, x, oy, hues[k], hues[k]);
        DrawSmoothStr(&sNormal, label, x + 9, oy + ROW_H - 28, 6, PLATE_DARK(hues[k]), sCream, NULL);
    }
}

/* The PP's colour as the game warns: at 0 red, then orange to a quarter,
 * yellow to half. */
static void PpColours(u8 pp, u8 maxPp, u16 *fg, u16 *shadow)
{
    if (pp == 0)
        *fg = TXT_RED, *shadow = TXT_LRED;
    else if (pp <= maxPp / 4)
        *fg = PackRgb(224, 112, 32), *shadow = PackRgb(248, 200, 152);
    else if (pp <= maxPp / 2)
        *fg = PackRgb(200, 160, 16), *shadow = PackRgb(248, 232, 152);
    else
        *fg = TXT_DARK, *shadow = TXT_LIGHT;
}

#define MOVE_W 150
#define MOVE_H 80

static void DrawMovePlate(const ViewState *s, int i)
{
    int x = i & 1 ? 164 : 6, y = i & 2 ? 106 : 20, oy;
    u16 move = s->moves4.moves[i], fg, sh;
    const struct BattleMove *data = &gBattleMoves[move];
    Rgb colour, dark, shadow;
    u8 text[20];

    if (move == MOVE_NONE)
    {
        DrawBattlePlate(BTA_MOVE, x, y, MOVE_W, MOVE_H, sHueEmpty, BTA_NORMAL, 0, HIT_NONE);
        return;
    }
    colour = data->type < NUMBER_OF_MON_TYPES ? sTypeColour[data->type] : sHueEmpty;
    dark = PLATE_DARK(colour);
    shadow = PLATE_SHADOW(colour);
    oy = DrawBattlePlate(BTA_MOVE, x, y, MOVE_W, MOVE_H, colour, PlateState(s, HIT_MOVE + i, s->cursor == i),
                         s->blink, HIT_MOVE + i);
    DrawSmoothStr(&sNormal, gMoveNames[move], x + 10, oy + 5, 4, sWhite, dark, &shadow);
    DrawTypeIcon(data->type, x + 11, oy + 33);
    StringCopy(text, gText_MoveInterfacePP);
    StringAppend(text, Number(s->moves4.currentPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    StringAppend(text, gText_Slash);
    StringAppend(text, Number(s->moves4.maxPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    PpColours(s->moves4.currentPp[i], s->moves4.maxPp[i], &fg, &sh);
    DrawStrRight(&sNormal, text, x + MOVE_W - 12, oy + 32, fg, sh);
    {
        int tx = DrawStr(&sSmall, Ascii("POW "), x + 12, oy + 56, TXT_DARK, TXT_LIGHT);

        DrawStr(&sSmall, data->power > 1 ? Number(data->power, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"), tx,
                oy + 56, TXT_DARK, TXT_LIGHT);
        StringCopy(text, Ascii("ACC "));
        StringAppend(text, data->accuracy ? Number(data->accuracy, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"));
        DrawStrRight(&sSmall, text, x + MOVE_W - 12, oy + 56, TXT_DARK, TXT_LIGHT);
    }
}

static void DrawBattleMoves(const ViewState *s)
{
    int oy;

    for (int i = 0; i < MAX_MON_MOVES; ++i)
        DrawMovePlate(s, i);
    oy = DrawBattlePlate(BTA_CANCEL, 84, 192, 152, 26, sHueCancel,
                         PlateState(s, HIT_CANCEL, s->cursor == MAX_MON_MOVES), s->blink, HIT_CANCEL);
    DrawSmoothStr(&sNormal, gText_Cancel2, 160 - SmoothInkWidth(&sNormal, gText_Cancel2, 4) / 2, oy + 5, 4, sWhite,
                  PLATE_DARK(sHueCancel), NULL);
}

/* Left and right move the game's target cursor; OK is what A does. */
static void DrawBattleTarget(const ViewState *s)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS}, ok[] = {CHAR_O, CHAR_K, EOS};
    const struct { s16 x, y; Rgb hue; const u8 *label; u8 hit; } plates[4] = {
        {6, ACT_Y, sHueRun, left, HIT_TARGET_LEFT},
        {164, ACT_Y, sHueRun, right, HIT_TARGET_RIGHT},
        {6, ROW_Y, sHueFight, ok, HIT_TARGET_OK},
        {164, ROW_Y, sHueCancel, gText_Cancel2, HIT_CANCEL},
    };

    for (int i = 0; i < 4; ++i)
    {
        Rgb dark = PLATE_DARK(plates[i].hue), shadow = PLATE_SHADOW(plates[i].hue);
        int oy = DrawBattlePlate(BTA_TARGET, plates[i].x, plates[i].y, MOVE_W, ROW_H, plates[i].hue,
                                 PlateState(s, plates[i].hit, plates[i].hit == HIT_TARGET_OK), s->blink, plates[i].hit);

        DrawSmoothStr(&sNormal, plates[i].label, plates[i].x + 85 - SmoothInkWidth(&sNormal, plates[i].label, 8) / 2,
                      oy + 22, 8, sCream, dark, &shadow);
    }
}

/*
 * While the turn plays out (the backdrop alone) the next menus are made
 * ready, one plate or label a frame: the action menu as it will come and the
 * moves of the mon on the left, with the focus ring's two blink tones. The
 * menus are walked with an empty clip, so nothing is drawn: only the one
 * piece missing is built. When they come they are copied together instead
 * of worked out in the frame they appear in. A pressed plate is built when
 * it is first pressed.
 */
static u32 sWarmKey = 0xFFFFFFFF;

static void WarmBattleMenus(void)
{
    static ViewState v;
    u8 battler = GetBattlerAtPosition(B_POSITION_PLAYER_LEFT);
    int ox = sOX, hits = sHitCount, clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
    u32 key;

    if (!sBta.file || battler >= MAX_BATTLERS_COUNT || sBuildBudget != 0xFF)
        return;
    memset(&v, 0, sizeof(v));
    v.safari = (gBattleTypeFlags & BATTLE_TYPE_SAFARI) != 0;
    v.quickBall = v.safari ? ITEM_NONE : CtrBattle_QuickBallItem();
    v.pressed = HIT_NONE;
    for (int i = 0; i < MAX_MON_MOVES; ++i)
        v.moves4.moves[i] = gBattleMons[battler].moves[i];
    key = (v.safari ? 1 : 0) | (v.quickBall != ITEM_NONE ? 2 : 0);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
        key = key * 31 + v.moves4.moves[i];
    if (key == sWarmKey)
        return;
    ResolveFonts();
    sOX = 0;
    sClipX0 = sClipY0 = sClipX1 = sClipY1 = 0;
    sBuildBudget = 1;
    SnapshotPartyBalls(&v);
    for (v.blink = 0; v.blink < 2 && sBuildBudget; ++v.blink)
    {
        v.mode = MODE_BATTLE_ACTION;
        DrawBattleActions(&v);
        v.mode = MODE_BATTLE_MOVE;
        DrawBattleMoves(&v);
    }
    /* A pass that built nothing: all of it is ready. */
    if (sBuildBudget == 1)
        sWarmKey = key;
    sBuildBudget = 0xFF;
    sOX = ox;
    sHitCount = hits;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
}

/* ------------------------------------------------------------------------ */
/* Redraw and present                                                       */
/* ------------------------------------------------------------------------ */

static void BuildBackgroundCaches(void)
{
    sOX = 0;
    /* The 240px view with the column beside it: the sections' light green
     * and the rail. The whole screen (battle) keeps the party menu's. */
    sDst = sCache[CACHE_MENU];
    DrawSection(0, 0, CW, H);
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
        CopyCache(sCache[CACHE_BATTLE] ? CACHE_BATTLE : CACHE_WIDE);
        if (s->mode == MODE_BATTLE_ACTION)
            DrawBattleActions(s);
        else if (s->mode == MODE_BATTLE_MOVE)
            DrawBattleMoves(s);
        else if (s->mode == MODE_BATTLE_TARGET)
            DrawBattleTarget(s);
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

/* A battle plate's hit, with the room its ring and shadow take. */
static bool8 RectAddPlate(Rect *r, u8 id)
{
    Rect plate;

    RectInit(&plate);
    if (!RectAddHitOf(&plate, id))
        return FALSE;
    RectAdd(r, plate.x0 - 4, plate.y0 - 4, plate.x1 + 4, plate.y1 + 6);
    return TRUE;
}

/* The battle cursor's plate: the action (or the quick ball, 4), the move
 * (or CANCEL), or OK when choosing a target. */
static u8 BattleCursorHit(const ViewState *s, u8 cursor)
{
    if (s->mode == MODE_BATTLE_ACTION)
        return cursor == 4 ? HIT_QUICK_BALL : HIT_ACTION + cursor;
    if (s->mode == MODE_BATTLE_MOVE)
        return cursor == MAX_MON_MOVES ? HIT_CANCEL : HIT_MOVE + cursor;
    return HIT_TARGET_OK;
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
    if (battle)
    {
        probe.cursor = shown->cursor;
        probe.blink = shown->blink;
    }
    if (now->mode == MODE_FIELD)
        for (int i = 0; i < OPTION_ROWS; ++i)
            if (i != OPT_VOXEL)
                probe.options[i] = shown->options[i];
    if (memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;

    RectInit(r);
    if (battle)
    {
        /* A plate pressed or let go, the cursor moved, its ring blinked. */
        if (now->pressed != shown->pressed
         && !((now->pressed == HIT_NONE || RectAddPlate(r, now->pressed))
              && (shown->pressed == HIT_NONE || RectAddPlate(r, shown->pressed))))
            return FALSE;
        if ((now->cursor != shown->cursor || now->blink != shown->blink)
         && !(RectAddPlate(r, BattleCursorHit(now, now->cursor)) && RectAddPlate(r, BattleCursorHit(now, shown->cursor))))
            return FALSE;
        return r->x0 < r->x1 && r->y0 < r->y1;
    }
    if (now->pressed != shown->pressed && !(RectAddHit(r, now->pressed) && RectAddHit(r, shown->pressed)))
        return FALSE;
    for (int i = 0; now->mode == MODE_FIELD && i < OPTION_ROWS; ++i)
        if (now->options[i] != shown->options[i] && !RectAddHit(r, HIT_OPTION + i))
            return FALSE;
    return r->x0 < r->x1 && r->y0 < r->y1;
}

static void RenderPart(const ViewState *s, int x0, int y0, int x1, int y1);

/* The focus ring moved or blinked, or a button was pressed or let go, and
 * nothing else changed: the rectangles of the buttons it touches. Beside the
 * game's own screens (the PokéNav, the bag) only the column's part is ours. */
static bool8 FocusDirtyRect(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;
    u8 focus[2] = {now->focus, shown->focus}, opt[2] = {now->optFocus, shown->optFocus};

    probe = *now;
    probe.focus = shown->focus;
    probe.optFocus = shown->optFocus;
    probe.blink = shown->blink;
    probe.pressed = shown->pressed;
    if (now->mode == MODE_OFF || now->mode >= MODE_BATTLE_INFO || memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;
    RectInit(r);
    for (int k = 0; k < 2; ++k)
    {
        if (focus[k] != FOCUS_NONE && !RectAddHit(r, HIT_COLUMN + focus[k]))
            return FALSE;
        if (opt[k] != FOCUS_NONE && !RectAddHit(r, HIT_OPTION + opt[k]))
            return FALSE;
    }
    if (now->pressed != shown->pressed && !(RectAddHit(r, now->pressed) && RectAddHit(r, shown->pressed)))
        return FALSE;
    return r->x0 < r->x1 && r->y0 < r->y1;
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

