/* ------------------------------------------------------------------------ */
/* Drawing: the button column                                               */
/* ------------------------------------------------------------------------ */

static void DrawColumnButton(const ViewState *s, int i, bool8 on, bool8 enabled)
{
    const u8 *labels[SCR_COUNT] = {
        Ascii("MAP"), gText_MenuPokemon, gText_MenuBag, s->name, gText_MenuPokedex, gText_MenuPokenav,
        gText_MenuSave, gText_MenuOption,
    };
    const Icon *icon = &sRes.column[i];
    int y = 3 + i * 30;

    DrawBoxEx(BOX_MENU, COL_X + 4, y, 9, 3, on);
    if (icon->tiles && enabled)
        DrawSprite(icon->tiles, icon->size, icon->size, COL_X + 2, y - (icon->size == 4 ? 4 : 0), icon->pal.c);
    DrawStr(&sSmall, labels[i], COL_X + 30, y + 6, enabled ? LABEL_FG(on) : TXT_LIGHT,
            enabled ? LABEL_SH(on) : TXT_WHITE);
}

/*
 * The column changes far less often than what is beside it: it is kept,
 * unpressed, in every background cache, and a redraw only paints the chosen
 * and pressed buttons over it. The caches are repainted when which entries
 * exist changes, or the player's name does.
 */
static void DrawColumn(const ViewState *s)
{
    static int cachedMask = -1;
    static u8 cachedName[PLAYER_NAME_LENGTH + 1];

    if (cachedMask != s->enabled || memcmp(cachedName, s->name, sizeof(cachedName)) != 0)
    {
        u16 *canvas = sDst;
        cachedMask = s->enabled;
        memcpy(cachedName, s->name, sizeof(cachedName));
        for (int c = 0; c < CACHE_COUNT; ++c)
        {
            if (!sCache[c] || c == CACHE_WIDE)
                continue;
            sDst = sCache[c];
            for (int i = 0; i < SCR_COUNT; ++i)
                DrawColumnButton(s, i, FALSE, (s->enabled >> i) & 1);
        }
        sDst = canvas;
        /* The canvas started from the stale column: paint all of it. */
        for (int i = 0; i < SCR_COUNT; ++i)
            DrawColumnButton(s, i, FALSE, (s->enabled >> i) & 1);
    }
    for (int i = 0; i < SCR_COUNT; ++i)
    {
        bool8 enabled = (s->enabled >> i) & 1;
        bool8 on = s->screen == i || s->pressed == HIT_COLUMN + i;

        if (on)
            DrawColumnButton(s, i, TRUE, enabled);
        if (enabled)
            AddHit(COL_X, 3 + i * 30 - 3, W - COL_X, 30, HIT_COLUMN + i);
    }
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

static void DrawHpBar(int x, int y, int width, u16 hp, u16 maxHp, const Pal *pal)
{
    static const u8 green[] = {57, 58}, yellow[] = {73, 74}, red[] = {89, 90};
    const u8 *ids;
    int fill;

    if (maxHp == 0)
        return;
    fill = hp * width / maxHp;
    if (hp > 0 && fill == 0)
        fill = 1;
    ids = hp * 2 > maxHp ? green : hp * 5 > maxHp ? yellow : red;
    FillRect(x, y, fill, 1, Rgb565(sRes.partyRaw[ids[1]]));
    FillRect(x, y + 1, fill, 2, Rgb565(sRes.partyRaw[ids[0]]));
    FillRect(x + fill, y, width - fill, 1, pal->c[0x0D]);
    FillRect(x + fill, y + 1, width - fill, 2, pal->c[0x02]);
}

static void DrawStatusIcon(u8 ailment, int x, int y)
{
    if (ailment == AILMENT_NONE || !sRes.statusTiles)
        return;
    DrawSprite(sRes.statusTiles + (ailment - 1) * 4 * 32, 4, 1, x, y, sRes.statusPal.c);
}

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
/* Drawing: region map                                                    */
/* ------------------------------------------------------------------------ */

#define MAP_ORIGIN_X 0
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
            if (v >= 112 && v < 144 && x + px < CW)
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
    for (int ty = -1; ty < H / 8; ++ty)
        for (int tx = 0; tx < CW / 8; ++tx)
        {
            u8 tile = (sRes.mapMap && ty >= 0 && ty < 64) ? sRes.mapMap[ty * 64 + tx] : 0;
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
    case OPT_DAYNIGHT:
    case OPT_VOXEL_STEREO:
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

/* A window frame as the game draws its message boxes, from its 3x3 tiles. */
static void DrawWindowFrame(u8 type, int x, int y, int wt, int ht)
{
    const struct TilesPal *frame = GetWindowFrameTilesPal(type);
    const u8 *tiles = frame ? Port_ResolveAssetPointer(frame->tiles) : NULL;
    const u16 *raw = frame ? Port_ResolveAssetPointer(frame->pal) : NULL;
    Pal pal;

    if (!tiles || !raw)
        return;
    ToPals(&pal, raw, 1);
    FillRect(x, y, wt * 8, ht * 8, TXT_WHITE);
    for (int ty = -1; ty <= ht; ++ty)
        for (int tx = -1; tx <= wt; ++tx)
        {
            int row = ty < 0 ? 0 : ty == ht ? 2 : 1, col = tx < 0 ? 0 : tx == wt ? 2 : 1;
            if (row == 1 && col == 1)
                continue;
            DrawTile(tiles + (row * 3 + col) * 32, x + tx * 8, y + ty * 8, pal.c, FALSE, FALSE);
        }
}

static void DrawOptions(const ViewState *s)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    const u8 *names[OPTION_ROWS] = {gText_TextSpeed, gText_BattleScene, gText_BattleStyle, gText_Sound,
                                    gText_ButtonMode, gText_Frame, Ascii("SHOW FPS"), Ascii("VOXEL 3D"),
                                    Ascii("3D ANGLE"), Ascii("3D ZOOM"), Ascii("3D BLUR"),
                                    Ascii("3D BATTLE"), Ascii("3D DEPTH"), Ascii("DAY/NIGHT")};
    /* The frame stays last, above its preview. */
    static const u8 order[OPTION_ROWS] = {OPT_TEXT_SPEED, OPT_BATTLE_SCENE, OPT_BATTLE_STYLE, OPT_SOUND,
                                          OPT_BUTTON_MODE, OPT_FPS, OPT_VOXEL, OPT_VOXEL_PITCH,
                                          OPT_VOXEL_ZOOM, OPT_VOXEL_BLUR, OPT_VOXEL_BATTLE, OPT_VOXEL_STEREO, OPT_DAYNIGHT, OPT_FRAME};
    /* The 3D rows only mean something with the voxel overworld on; with them
     * there is no room left for the frame's preview, nor for every row: the
     * list then scrolls (OptionsDrag). */
    bool8 camera = OPTION_SHOWN > OPT_VOXEL && s->options[OPT_VOXEL];
    const int pitch = OPTION_PITCH;
    int maxScroll = OptionsMaxScroll(camera);
    /* A scrolling list gives up a tile at its right for the bar, which then
     * stands clear of both the rows and the button column. */
    int tiles = maxScroll > 0 ? 29 : 30, shift = (30 - tiles) * 8;
    int preview;

    for (int slot = 0, row = 0; row < OPTION_ROWS; ++row)
    {
        int i = order[row], y;
        if (!OptionRowShown(i, camera))
            continue;
        y = 4 + slot++ * pitch - s->optionScroll;
        if (y + 24 <= 0 || y >= H)
            continue;
        bool8 on = s->pressed == HIT_OPTION + i || s->pressed == HIT_OPTION + HIT_OPTION_BACK + i;

        DrawBoxEx(BOX_MENU, 0, y, tiles, 3, on);
        DrawStr(&sSmall, names[i], 10, y + 6, LABEL_FG(on), LABEL_SH(on));
        DrawStr(&sSmall, left, 112, y + 6, LABEL_FG(on), LABEL_SH(on));
        DrawStrCentered(&sSmall, OptionValue(i, s->options[i]), 170 - shift / 2, y + 6,
                        on ? TXT_WHITE : TXT_RED, on ? TXT_DARK : TXT_LRED);
        DrawStr(&sSmall, right, 222 - shift, y + 6, LABEL_FG(on), LABEL_SH(on));
        AddHit(0, y, 136, 24, HIT_OPTION + HIT_OPTION_BACK + i);
        AddHit(136, y, 104 - shift, 24, HIT_OPTION + i);
    }
    /* Where the list is scrolled to, when it does not fit. */
    if (maxScroll > 0)
    {
        int track = H - 8, thumb = track * H / (H + maxScroll);

        FillRect(CW - 5, 4, 2, track, TXT_LIGHT);
        FillRect(CW - 5, 4 + (track - thumb) * s->optionScroll / maxScroll, 2, thumb, TXT_DARK);
    }
    /* What the chosen frame looks like, below the rows: it scrolls with
     * them when they do not all fit. */
    if (camera)
        return;
    preview = 4 + OptionRowsShown(camera) * pitch + OPTION_PREVIEW_GAP - s->optionScroll;
    DrawWindowFrame(s->options[5], 24, preview, 24 - (tiles < 30), 3);
    DrawStrCentered(&sNormal, OptionValue(5, s->options[5]), CW / 2 - shift / 2, preview + 4, TXT_DARK, TXT_LIGHT);
}

/* ------------------------------------------------------------------------ */
/* Drawing: battle                                                          */
/* ------------------------------------------------------------------------ */

#define HEADER_H 56

static void DrawTypeIcon(u8 type, int x, int y)
{
    static const u8 palettes[NUMBER_OF_MON_TYPES] = {
        [TYPE_NORMAL] = 0, [TYPE_FIGHTING] = 0, [TYPE_FLYING] = 1, [TYPE_POISON] = 1,
        [TYPE_GROUND] = 0, [TYPE_ROCK] = 0, [TYPE_BUG] = 2, [TYPE_GHOST] = 1,
        [TYPE_STEEL] = 0, [TYPE_MYSTERY] = 2, [TYPE_FIRE] = 0, [TYPE_WATER] = 1,
        [TYPE_GRASS] = 2, [TYPE_ELECTRIC] = 0, [TYPE_PSYCHIC] = 1, [TYPE_ICE] = 1,
        [TYPE_DRAGON] = 2, [TYPE_DARK] = 0,
    }; /* pokemon_summary_screen.c's sMoveTypeToOamPaletteNum, minus 13 */

    if (sRes.typeTiles && type < NUMBER_OF_MON_TYPES)
        DrawSprite(sRes.typeTiles + type * 8 * 32, 4, 2, x, y, sRes.typePal[palettes[type]].c);
}

static void DrawBattlerPanel(const BattlerView *v, int x, int y, bool8 compact)
{
    const Pal *pal = &sRes.partyPal[4];

    if (!v->present)
        return;
    if (compact)
    {
        DrawStr(&sSmall, v->nick, x, y, TXT_DARK, TXT_LIGHT);
        DrawHpBar(x + 72, y + 5, 48, v->hp, v->maxHp, pal);
        DrawStatusIcon(v->ailment, x + 124, y + 3);
        return;
    }
    AddMonIcon(v->iconSpecies, v->deoxys, x, y - 4, v->hp == 0);
    DrawStr(&sSmall, v->nick, x + 34, y, TXT_DARK, TXT_LIGHT);
    {
        u8 text[12];
        StringCopy(text, gText_LevelSymbol);
        StringAppend(text, Number(v->level, 3, STR_CONV_MODE_LEFT_ALIGN));
        DrawStrRight(&sSmall, text, x + 142, y, TXT_DARK, TXT_LIGHT);
    }
    DrawHpBar(x + 34, y + 16, 96, v->hp, v->maxHp, pal);
    DrawStatusIcon(v->ailment, x + 34, y + 23);
    /* The game shows exact HP for the player's side only. */
    if (v->side == B_SIDE_PLAYER)
    {
        u8 text[12];
        StringCopy(text, Number(v->hp, 3, STR_CONV_MODE_RIGHT_ALIGN));
        StringAppend(text, gText_Slash);
        StringAppend(text, Number(v->maxHp, 3, STR_CONV_MODE_RIGHT_ALIGN));
        DrawStrRight(&sSmall, text, x + 130, y + 21, TXT_DARK, TXT_LIGHT);
    }
}

static void DrawBattleHeader(const ViewState *s)
{
    DrawBox(BOX_MENU, 0, 0, 20, HEADER_H / 8);
    DrawBox(BOX_MENU, 160, 0, 20, HEADER_H / 8);
    if (s->isDouble)
    {
        DrawBattlerPanel(&s->battlers[0], 10, 8, TRUE);
        DrawBattlerPanel(&s->battlers[2], 10, 28, TRUE);
        DrawBattlerPanel(&s->battlers[1], 170, 8, TRUE);
        DrawBattlerPanel(&s->battlers[3], 170, 28, TRUE);
    }
    else
    {
        DrawBattlerPanel(&s->battlers[0], 10, 12, FALSE);
        DrawBattlerPanel(&s->battlers[1], 170, 12, FALSE);
    }
}

static void DrawBattleActions(const ViewState *s)
{
    static const char *const safari[4] = {"BALL", "POK*BLOCK", "GO NEAR", "RUN"};
    bool8 on[4];

    for (int i = 0; i < 4; ++i)
        on[i] = s->cursor == i || s->pressed == HIT_ACTION + i;

    /* FIGHT: the move types it leads to. */
    DrawButton(16, 60, 36, 10, on[0], HIT_ACTION + 0);
    if (s->safari)
    {
        DrawStrCentered(&sNormal, Ascii(safari[0]), 160, 90, LABEL_FG(on[0]), LABEL_SH(on[0]));
    }
    else
    {
        int count = 0;
        DrawStrCentered(&sNormal, Ascii("FIGHT"), 160, 76, LABEL_FG(on[0]), LABEL_SH(on[0]));
        for (int i = 0; i < MAX_MON_MOVES; ++i)
            if (s->moves4.moves[i] != MOVE_NONE)
                ++count;
        for (int i = 0, x = 160 - (count * 40 - 8) / 2; i < MAX_MON_MOVES; ++i)
            if (s->moves4.moves[i] != MOVE_NONE)
            {
                DrawTypeIcon(gBattleMoves[s->moves4.moves[i]].type, x, 104);
                x += 40;
            }
    }

    /* BAG, POKéMON, RUN. */
    DrawButton(16, 148, 11, 11, on[1], HIT_ACTION + 1);
    DrawButton(116, 148, 11, 11, on[2], HIT_ACTION + 2);
    DrawButton(216, 148, 11, 11, on[3], HIT_ACTION + 3);
    if (s->safari)
    {
        DrawStrCentered(&sNormal, Ascii(safari[1]), 60, 184, LABEL_FG(on[1]), LABEL_SH(on[1]));
        DrawStrCentered(&sNormal, Ascii(safari[2]), 160, 184, LABEL_FG(on[2]), LABEL_SH(on[2]));
        DrawStrCentered(&sNormal, Ascii(safari[3]), 260, 184, LABEL_FG(on[3]), LABEL_SH(on[3]));
        return;
    }
    if (sRes.bagTiles[s->gender])
        DrawSprite(sRes.bagTiles[s->gender], 8, 8, 28, 150, sRes.bagPal.c);
    DrawStrCentered(&sNormal, Ascii("BAG"), 60, 212, LABEL_FG(on[1]), LABEL_SH(on[1]));
    if (s->battlers[0].present)
        AddMonIcon(s->battlers[0].iconSpecies, s->battlers[0].deoxys, 144, 168, FALSE);
    else if (s->party[0].species)
        AddMonIcon(s->party[0].iconSpecies, s->party[0].deoxys, 144, 168, FALSE);
    DrawStrCentered(&sNormal, Ascii("POK*MON"), 160, 212, LABEL_FG(on[2]), LABEL_SH(on[2]));
    DrawItemIcon(ITEM_ESCAPE_ROPE, 248, 176);
    DrawStrCentered(&sNormal, Ascii("RUN"), 260, 212, LABEL_FG(on[3]), LABEL_SH(on[3]));
}

static void DrawBattleMoves(const ViewState *s)
{
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        int x = i & 1 ? 164 : 12, y = i & 2 ? 140 : 64;
        u16 move = s->moves4.moves[i];
        bool8 on = move != MOVE_NONE && (s->cursor == i || s->pressed == HIT_MOVE + i);
        const struct BattleMove *data = &gBattleMoves[move];
        u8 text[20];

        DrawBoxEx(BOX_MENU, x, y, 18, 9, on);
        if (move == MOVE_NONE)
        {
            DrawStrCentered(&sNormal, Ascii("-"), x + 72, y + 28, TXT_LIGHT, TXT_WHITE);
            continue;
        }
        AddHit(x, y, 144, 72, HIT_MOVE + i);
        DrawStr(&sNormal, gMoveNames[move], x + 14, y + 9, s->moves4.currentPp[i] ? LABEL_FG(on) : TXT_RED,
                s->moves4.currentPp[i] ? LABEL_SH(on) : TXT_LRED);
        DrawTypeIcon(data->type, x + 14, y + 30);
        StringCopy(text, gText_MoveInterfacePP);
        StringAppend(text, Ascii(" "));
        StringAppend(text, Number(s->moves4.currentPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
        StringAppend(text, gText_Slash);
        StringAppend(text, Number(s->moves4.maxPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
        DrawStrRight(&sSmall, text, x + 130, y + 32, LABEL_FG(on), LABEL_SH(on));
        {
            int tx = DrawStr(&sSmall, Ascii("POW "), x + 14, y + 50, LABEL_FG(on), LABEL_SH(on));
            tx = DrawStr(&sSmall, data->power > 1 ? Number(data->power, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"),
                         tx, y + 50, LABEL_FG(on), LABEL_SH(on));
            tx = DrawStr(&sSmall, Ascii("   ACC "), tx, y + 50, LABEL_FG(on), LABEL_SH(on));
            DrawStr(&sSmall, data->accuracy ? Number(data->accuracy, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"),
                    tx, y + 50, LABEL_FG(on), LABEL_SH(on));
        }
    }
    DrawLabelButton(12, 216, 37, 3, gText_Cancel2, s->pressed == HIT_CANCEL || s->cursor == MAX_MON_MOVES, TRUE,
                    HIT_CANCEL);
}

static void DrawBattleTarget(const ViewState *s)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};

    DrawLabelButton(12, 64, 18, 9, left, s->pressed == HIT_TARGET_LEFT, TRUE, HIT_TARGET_LEFT);
    DrawLabelButton(164, 64, 18, 9, right, s->pressed == HIT_TARGET_RIGHT, TRUE, HIT_TARGET_RIGHT);
    DrawLabelButton(12, 144, 18, 9, Ascii("OK"), s->pressed == HIT_TARGET_OK, TRUE, HIT_TARGET_OK);
    DrawLabelButton(164, 144, 18, 9, gText_Cancel2, s->pressed == HIT_CANCEL, TRUE, HIT_CANCEL);
}

static void DrawBattleInfo(const ViewState *s)
{
    /* The party, so a switch can be planned while the turn plays out. The
     * message is not repeated here: the top screen shows it. */
    DrawBox(BOX_MENU, 0, 64, 40, 22);
    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        const MonView *m = &s->party[i];
        int x = 12 + (i % 3) * 100, y = 100 + (i / 3) * 72;

        if (!m->species)
            continue;
        AddMonIcon(m->iconSpecies, m->deoxys, x, y, m->fainted);
        DrawStr(&sSmall, m->nick, x + 34, y + 2, TXT_DARK, TXT_LIGHT);
        if (!m->isEgg)
        {
            DrawHpBar(x + 34, y + 18, 48, m->hp, m->maxHp, &sRes.partyPal[4]);
            DrawStatusIcon(m->ailment, x + 34, y + 24);
        }
    }
}

