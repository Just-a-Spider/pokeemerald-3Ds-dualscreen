/* ------------------------------------------------------------------------ */
/* Canvas                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The canvas, in framebuffer layout, is what gets copied to the screen. The
 * big static pictures behind each view (party menu background, Hoenn map,
 * trainer card) are decoded once into caches of the same layout, so a redraw
 * starts with a memcpy instead of re-decoding tens of thousands of pixels.
 * Views draw in content coordinates; sOX moves them (battle menus centre the
 * 240-wide views in the whole screen).
 */
static u16 sCanvas[W * H] __attribute__((aligned(32)));
static u16 *sDst = sCanvas;
static int sOX;

/* CACHE_BATTLE is the battle art's backdrop itself, in this layout already. */
enum { CACHE_MENU, CACHE_WIDE, CACHE_MAP, CACHE_CARD, CACHE_BATTLE, CACHE_COUNT };
static u16 *sCache[CACHE_COUNT];
static int sCardCacheKey = -1;

/*
 * A redraw that changes one part of the screen - a button pressed, a cursor
 * moved, an HP bar draining, an option's value - draws only that part: every
 * writer below keeps inside this clip (screen pixels, after sOX), the cache
 * is copied in only for it, and only it is sent to the screen. The whole
 * screen is the default.
 */
static int sClipX0, sClipY0, sClipX1 = W, sClipY1 = H;

static bool8 ClipIsFull(void)
{
    return sClipX0 == 0 && sClipY0 == 0 && sClipX1 == W && sClipY1 == H;
}

static void CopyCache(int which)
{
    if (ClipIsFull())
    {
        if (sCache[which])
            memcpy(sCanvas, sCache[which], sizeof(sCanvas));
        else
            memset(sCanvas, 0, sizeof(sCanvas));
        return;
    }
    /* A column runs bottom-to-top: rows [y0, y1) are one contiguous run. */
    for (int x = sClipX0; x < sClipX1; ++x)
    {
        u16 *dst = sCanvas + x * H + (H - sClipY1);

        if (sCache[which])
            memcpy(dst, sCache[which] + x * H + (H - sClipY1), (size_t)(sClipY1 - sClipY0) * sizeof(u16));
        else
            memset(dst, 0, (size_t)(sClipY1 - sClipY0) * sizeof(u16));
    }
}

static inline void Put(int x, int y, u16 c)
{
    x += sOX;
    if (x < sClipX0 || x >= sClipX1 || y < sClipY0 || y >= sClipY1)
        return;
    sDst[x * H + (H - 1 - y)] = c;
}

static void FillRect(int x, int y, int w, int h, u16 c)
{
    int x0 = x + sOX, x1 = x0 + w, y0 = y, y1 = y + h;

    if (x0 < sClipX0) x0 = sClipX0;
    if (x1 > sClipX1) x1 = sClipX1;
    if (y0 < sClipY0) y0 = sClipY0;
    if (y1 > sClipY1) y1 = sClipY1;
    for (int cx = x0; cx < x1; ++cx)
    {
        u16 *p = sDst + cx * H + (H - y1);
        for (int n = y1 - y0; n > 0; --n)
            *p++ = c;
    }
}

static u16 Rgb565(u16 bgr)
{
    u16 r = bgr & 31, g = (bgr >> 5) & 31, b = (bgr >> 10) & 31;
    return (r << 11) | (((g << 1) | (g >> 4)) << 5) | b;
}

/* The same colour at 55% brightness: how a pressed or chosen button looks. */
static u16 Darker(u16 c)
{
    u16 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 9 / 16) << 11) | ((g * 9 / 16) << 5) | (b * 9 / 16);
}

typedef struct { u16 c[16]; } Pal;

static void ToPals(Pal *dst, const u16 *src, int count)
{
    for (int p = 0; p < count; ++p)
        for (int i = 0; i < 16; ++i)
            dst[p].c[i] = Rgb565(src[p * 16 + i]);
}

static void DarkPal(Pal *dst, const Pal *src)
{
    for (int i = 0; i < 16; ++i)
        dst->c[i] = Darker(src->c[i]);
}

/*
 * A 4bpp 8x8 tile; colour 0 is transparent, as on the GBA. This is where a
 * redraw spends its time, so a tile fully on screen is written straight into
 * its columns: pixel (x, y) is canvas[x * H + H - 1 - y], so one step right
 * is +H and one step down is -1.
 */
static void DrawTile(const u8 *tile, int x, int y, const u16 *pal, bool8 hflip, bool8 vflip)
{
    int sx0 = x + sOX;

    if (sx0 >= sClipX1 || y >= sClipY1 || sx0 + 8 <= sClipX0 || y + 8 <= sClipY0)
        return;
    if (sx0 >= sClipX0 && y >= sClipY0 && sx0 + 8 <= sClipX1 && y + 8 <= sClipY1)
    {
        u16 *origin = sDst + sx0 * H + (H - 1 - y);
        for (int py = 0; py < 8; ++py)
        {
            const u8 *row = tile + (vflip ? 7 - py : py) * 4;
            u32 bits = row[0] | (row[1] << 8) | (row[2] << 16) | ((u32)row[3] << 24);
            u16 *p = origin - py;

            if (!bits)
                continue;
            for (int px = 0; px < 8; ++px, bits >>= 4)
            {
                u8 v = bits & 15;
                if (v)
                    p[(hflip ? 7 - px : px) * H] = pal[v];
            }
        }
        return;
    }
    for (int py = 0; py < 8; ++py)
    {
        const u8 *row = tile + (vflip ? 7 - py : py) * 4;
        for (int px = 0; px < 8; ++px)
        {
            int sx = hflip ? 7 - px : px;
            u8 v = (row[sx >> 1] >> ((sx & 1) * 4)) & 15;
            if (v)
                Put(x + px, y + py, pal[v]);
        }
    }
}

/* The single colour of a tile with no transparent or differing pixel, or -1. */
static int SolidTileColor(const u8 *tile)
{
    u8 v = tile[0] & 15;

    if (!v)
        return -1;
    for (int i = 0; i < 32; ++i)
        if (tile[i] != (v | (v << 4)))
            return -1;
    return v;
}

/* A sprite in the GBA's one-dimensional tile layout. */
static void DrawSprite(const u8 *tiles, int wt, int ht, int x, int y, const u16 *pal)
{
    for (int ty = 0; ty < ht; ++ty)
        for (int tx = 0; tx < wt; ++tx)
            DrawTile(tiles + (ty * wt + tx) * 32, x + tx * 8, y + ty * 8, pal, FALSE, FALSE);
}

/* A text background tilemap entry: tile, flips and palette bank. */
static void DrawMapEntry(const u8 *tiles, u32 tileCount, u16 e, int x, int y, const Pal *pals)
{
    u32 tile = e & 0x3FF;

    if (tiles && tile < tileCount)
        DrawTile(tiles + tile * 32, x, y, pals[e >> 12].c, (e >> 10) & 1, (e >> 11) & 1);
}

static void DrawTypeIcon(u8 type, int x, int y);

/* ------------------------------------------------------------------------ */
/* Resources                                                                */
/* ------------------------------------------------------------------------ */

static void *ReadRomfs(const char *path, u32 *outSize)
{
    char full[128];
    void *data;

    snprintf(full, sizeof(full), "graphics/%s", path);
    data = CtrData_Load(full, outSize);
    if (!data)
        CtrLog_Write(CTR_LOG_ERROR, "bottom: %s missing", full);
    return data;
}

/*
 * LZ77 data, whether a game symbol (an asset stub, resolved from RomFS) or a
 * buffer read here. The size comes from the stream's own header.
 */
static void *Unlz(const void *src, u32 *outSize)
{
    const u8 *res = src ? Port_ResolveAssetPointer(src) : NULL;
    u32 header, size;
    void *dst;

    if (!res)
        return NULL;
    header = res[0] | (res[1] << 8) | (res[2] << 16) | ((u32)res[3] << 24);
    size = header >> 8;
    if ((header & 0xFF) != 0x10 || size == 0 || size > 0x10000)
        return NULL;
    dst = malloc(size);
    if (!dst)
        return NULL;
    LZ77UnCompWram((const u32 *)res, dst);
    if (outSize)
        *outSize = size;
    return dst;
}

static void *UnlzFile(const char *path, u32 *outSize)
{
    void *packed = ReadRomfs(path, NULL);
    void *data = Unlz(packed, outSize);

    free(packed);
    return data;
}

static bool8 PalFile(const char *path, Pal *dst, int count, bool8 compressed)
{
    u32 size = 0;
    u16 *raw = compressed ? UnlzFile(path, &size) : ReadRomfs(path, &size);

    if (!raw)
        return FALSE;
    if ((int)(size / 32) < count)
        count = size / 32;
    ToPals(dst, raw, count);
    free(raw);
    return TRUE;
}

/* An uncompressed game palette; the pointer may be an asset stub. */
static void PalSymbol(const void *src, Pal *dst, int count)
{
    const u16 *res = src ? Port_ResolveAssetPointer(src) : NULL;

    if (res)
        ToPals(dst, res, count);
}

/* The screens of the button column, top to bottom. */
enum { SCR_MAP, SCR_POKEMON, SCR_BAG, SCR_CARD, SCR_POKEDEX, SCR_POKENAV, SCR_SAVE, SCR_OPTION, SCR_COUNT };
/* Below the screens' buttons: the registered item (Y) and RUN. */
enum { COL_Y = SCR_COUNT, COL_RUN, COL_ITEMS };

/*
 * The game's six options, then the port's own, off by default and kept in
 * settings.txt rather than in the save (3ds_settings.c): the FPS counter and
 * the voxel overworld. Only a build with the counter has its row, and only
 * one with the voxel renderer has the voxel rows.
 */
#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
enum { OPT_TEXT_SPEED, OPT_BATTLE_SCENE, OPT_BATTLE_STYLE, OPT_SOUND, OPT_BUTTON_MODE, OPT_FRAME,
       OPT_FPS, OPT_VOXEL, OPT_VOXEL_PITCH, OPT_VOXEL_ZOOM, OPT_VOXEL_BLUR, OPT_VOXEL_BATTLE,
       OPTION_ROWS };
#if CTR_VOXEL_ENABLED
#define OPTION_SHOWN OPTION_ROWS
#else
#define OPTION_SHOWN OPT_VOXEL
#endif

typedef struct
{
    u8 *tiles;
    u8 size;        /* 3 for a 24x24 item icon, 4 for 32x32 */
    Pal pal;
} Icon;

static struct
{
    bool8 ready;
    Pal text;                        /* the message box text palette */
    /* Party menu. */
    u8 *partyTiles;
    u32 partyTileCount;
    u16 partyRaw[16 * 11];
    Pal partyPal[11];
    u8 *slotMain, *slotMainNoHp, *slotWide, *slotWideNoHp, *slotWideEmpty;
    u8 *ballTiles;
    Pal ballPal;
    /* Battle text box frames, and their darkened copies. */
    u8 *boxTiles;
    u32 boxTileCount;
    Pal boxPal[2], boxPalDark[2];
    /* Icons. */
    Icon column[SCR_COUNT];
    u8 *typeTiles;
    Pal typePal[3];
    u8 *statusTiles;
    Pal statusPal;
    Pal monIconPal[3];
    /* Region map. */
    u8 *mapTiles;
    u32 mapTileCount;
    u8 *mapMap;
    u16 mapPal[32];
    u8 *playerIcon[2];
    Pal playerIconPal[2];
    u8 *cursorTiles;
    Pal cursorPal;
    /* Trainer card. */
    u8 *cardTiles;
    u32 cardTileCount;
    u16 *cardFront, *cardBg;
    Pal cardPal[5][3];
    Pal cardFemaleBg, badgePal, starPal;
    u8 *badgeTiles;
    u8 *trainerPic[2];
    Pal trainerPicPal[2];
    u8 *bagTiles[2];
    Pal bagPal;
} sRes;

static void LoadItemIcon(Icon *icon, const void *tiles, const void *pal)
{
    u16 *raw = Unlz(pal, NULL);

    icon->tiles = Unlz(tiles, NULL);
    icon->size = 3;
    if (raw)
    {
        ToPals(&icon->pal, raw, 1);
        free(raw);
    }
}

/* Both players' trainer picture and bag, so nothing is decoded in a frame. */
static void LoadGenderResources(void)
{
    for (u8 gender = MALE; gender <= FEMALE; ++gender)
    {
        u16 pic = gFacilityClassToPicIndex[gender == FEMALE ? FACILITY_CLASS_MAY : FACILITY_CLASS_BRENDAN];
        u16 *pal = Unlz(gTrainerFrontPicPaletteTable[pic].data, NULL);
        u32 size = 0;
        u8 *all = Unlz(gender == FEMALE ? gBagFemaleTiles : gBagMaleTiles, &size);

        sRes.trainerPic[gender] = Unlz(gTrainerFrontPicTable[pic].data, NULL);
        if (pal)
        {
            ToPals(&sRes.trainerPicPal[gender], pal, 1);
            free(pal);
        }
        /* Only the first of the six frames, the closed bag, is shown. */
        if (all && size >= 64 * 32 && (sRes.bagTiles[gender] = malloc(64 * 32)) != NULL)
            memcpy(sRes.bagTiles[gender], all, 64 * 32);
        free(all);
    }
    {
        u16 *pal = Unlz(gBagPalette, NULL);
        if (pal)
        {
            ToPals(&sRes.bagPal, pal, 1);
            free(pal);
        }
    }
}

static void LoadBattleArt(void);
static void BuildRayquaza(void);
static void BuildTypeColours(void);
static void InitLook(void);
static void BuildBall(void);

static void LoadResources(void)
{
    static const char *const cardPals[5] = {
        "trainer_card/green.gbapal", "trainer_card/bronze.gbapal", "trainer_card/copper.gbapal",
        "trainer_card/silver.gbapal", "trainer_card/gold.gbapal",
    };
    u32 size;

    PalSymbol(GetOverworldTextboxPalettePtr(), &sRes.text, 1);
    InitLook();
    BuildBall();

    sRes.partyTiles = UnlzFile("party_menu/bg.4bpp.lz", &size);
    sRes.partyTileCount = size / 32;
    {
        u16 *raw = UnlzFile("party_menu/bg.gbapal.lz", &size);
        if (raw)
        {
            memcpy(sRes.partyRaw, raw, size < sizeof(sRes.partyRaw) ? size : sizeof(sRes.partyRaw));
            free(raw);
        }
        ToPals(sRes.partyPal, sRes.partyRaw, 11);
    }
    sRes.slotMain = ReadRomfs("party_menu/slot_main.bin", NULL);
    sRes.slotMainNoHp = ReadRomfs("party_menu/slot_main_no_hp.bin", NULL);
    sRes.slotWide = ReadRomfs("party_menu/slot_wide.bin", NULL);
    sRes.slotWideNoHp = ReadRomfs("party_menu/slot_wide_no_hp.bin", NULL);
    sRes.slotWideEmpty = ReadRomfs("party_menu/slot_wide_empty.bin", NULL);
    sRes.ballTiles = UnlzFile("party_menu/pokeball_small.4bpp.lz", NULL);
    PalFile("party_menu/pokeball.gbapal.lz", &sRes.ballPal, 1, TRUE);

    sRes.boxTiles = UnlzFile("battle_interface/textbox.4bpp.lz", &size);
    sRes.boxTileCount = size / 32;
    PalFile("battle_interface/textbox.gbapal.lz", sRes.boxPal, 2, TRUE);
    DarkPal(&sRes.boxPalDark[0], &sRes.boxPal[0]);
    DarkPal(&sRes.boxPalDark[1], &sRes.boxPal[1]);

    /* The column's icons: the game's own item icons, and the PokéNav's. */
    LoadItemIcon(&sRes.column[SCR_MAP], gItemIcon_TownMap, gItemIconPalette_TownMap);
    LoadItemIcon(&sRes.column[SCR_POKEMON], gItemIcon_PokeBall, gItemIconPalette_PokeBall);
    LoadItemIcon(&sRes.column[SCR_BAG], gItemIcon_BerryPouch, gItemIconPalette_BerryPouch);
    LoadItemIcon(&sRes.column[SCR_CARD], gItemIcon_ContestPass, gItemIconPalette_ContestPass);
    LoadItemIcon(&sRes.column[SCR_POKEDEX], gItemIcon_FameChecker, gItemIconPalette_FameChecker);
    LoadItemIcon(&sRes.column[SCR_SAVE], GetItemIconPicOrPalette(ITEM_LETTER, 0), GetItemIconPicOrPalette(ITEM_LETTER, 1));
    LoadItemIcon(&sRes.column[SCR_OPTION], gItemIcon_TeachyTV, gItemIconPalette_TeachyTV);
    sRes.column[SCR_POKENAV].tiles = UnlzFile("pokenav/nav_icon.4bpp.lz", NULL);
    sRes.column[SCR_POKENAV].size = 4;
    PalFile("pokenav/nav_icon.gbapal", &sRes.column[SCR_POKENAV].pal, 1, FALSE);

    sRes.typeTiles = UnlzFile("types/move_types.4bpp.lz", NULL);
    PalFile("types/move_types.gbapal.lz", sRes.typePal, 3, TRUE);
    sRes.statusTiles = UnlzFile("interface/status_icons.4bpp.lz", NULL);
    PalFile("interface/status_icons.gbapal.lz", &sRes.statusPal, 1, TRUE);
    for (int i = 0; i < 3; ++i)
        PalSymbol(gMonIconPaletteTable[i].data, &sRes.monIconPal[i], 1);

    sRes.mapTiles = UnlzFile("pokenav/region_map/map.8bpp.lz", &size);
    sRes.mapTileCount = size / 64;
    sRes.mapMap = UnlzFile("pokenav/region_map/map.bin.lz", NULL);
    {
        u16 *raw = ReadRomfs("pokenav/region_map/map.gbapal", &size);
        if (raw)
        {
            for (u32 i = 0; i < 32 && i < size / 2; ++i)
                sRes.mapPal[i] = Rgb565(raw[i]);
            free(raw);
        }
    }
    sRes.playerIcon[MALE] = ReadRomfs("pokenav/region_map/brendan_icon.4bpp", NULL);
    sRes.playerIcon[FEMALE] = ReadRomfs("pokenav/region_map/may_icon.4bpp", NULL);
    PalFile("pokenav/region_map/brendan_icon.gbapal", &sRes.playerIconPal[MALE], 1, FALSE);
    PalFile("pokenav/region_map/may_icon.gbapal", &sRes.playerIconPal[FEMALE], 1, FALSE);
    sRes.cursorTiles = UnlzFile("pokenav/region_map/cursor_small.4bpp.lz", NULL);
    PalFile("pokenav/region_map/cursor.gbapal", &sRes.cursorPal, 1, FALSE);

    sRes.cardTiles = UnlzFile("trainer_card/tiles.4bpp.lz", &size);
    sRes.cardTileCount = size / 32;
    sRes.cardFront = UnlzFile("trainer_card/front.bin.lz", NULL);
    sRes.cardBg = UnlzFile("trainer_card/bg.bin.lz", NULL);
    for (int i = 0; i < 5; ++i)
        PalFile(cardPals[i], sRes.cardPal[i], 3, FALSE);
    PalFile("trainer_card/female_bg.gbapal", &sRes.cardFemaleBg, 1, FALSE);
    PalFile("trainer_card/badges.gbapal", &sRes.badgePal, 1, FALSE);
    PalFile("trainer_card/star.gbapal", &sRes.starPal, 1, FALSE);
    sRes.badgeTiles = UnlzFile("trainer_card/badges.4bpp.lz", NULL);

    LoadGenderResources();
    LoadBattleArt();
    BuildTypeColours();
    BuildRayquaza();

    for (int i = 0; i < CACHE_BATTLE; ++i)
        sCache[i] = malloc(sizeof(sCanvas));
    sRes.ready = sRes.partyTiles && sRes.boxTiles && sRes.slotMain && sRes.slotWide && sRes.slotWideEmpty
              && sCache[CACHE_MENU] && sCache[CACHE_WIDE];
    if (!sRes.ready)
        CtrLog_Write(CTR_LOG_ERROR, "bottom: party menu or text box graphics missing, screen stays off");
}

/*
 * Icons are read from RomFS, which on hardware is the slowest thing a frame
 * can do. Drawing only ever uses what is already cached; Prefetch loads at
 * most one missing icon per frame, and the screen is redrawn once they are in.
 */
static bool8 sIconBudget;
static u32 sIconClock;

/* Mon icons: two 32x32 frames each, a few species at a time. */
#define MON_ICON_SLOTS 12
static struct
{
    u16 key;
    u32 age;
    u8 tiles[1024];
} sMonIcons[MON_ICON_SLOTS];

static const u8 *MonIcon(u16 iconSpecies, bool8 deoxysForm)
{
    u16 key = iconSpecies | (deoxysForm ? 0x8000 : 0);
    int victim = 0;
    const u8 *src;

    for (int i = 0; i < MON_ICON_SLOTS; ++i)
    {
        if (sMonIcons[i].key == key && key != 0)
        {
            sMonIcons[i].age = ++sIconClock;
            return sMonIcons[i].tiles;
        }
        if (sMonIcons[i].age < sMonIcons[victim].age)
            victim = i;
    }
    if (iconSpecies >= SPECIES_EGG + 28 || iconSpecies == SPECIES_NONE || !sIconBudget)
        return NULL;
    sIconBudget = FALSE;
    src = Port_ResolveAssetPointer(gMonIconTable[iconSpecies]);
    if (!src)
        return NULL;
    /* Deoxys keeps its alternate form in the same file, 0x400 in. */
    memcpy(sMonIcons[victim].tiles, src + (deoxysForm ? 0x400 : 0), 1024);
    sMonIcons[victim].key = key;
    sMonIcons[victim].age = ++sIconClock;
    return sMonIcons[victim].tiles;
}

/* Item icons: 24x24 with their own palette. */
#define ITEM_ICON_SLOTS 24
static struct
{
    u16 item;
    u32 age;
    bool8 valid, loaded;
    u8 tiles[9 * 32];
    Pal pal;
} sItemIcons[ITEM_ICON_SLOTS];

static int ItemIcon(u16 item)
{
    int victim = 0;
    u32 size = 0;
    u8 *tiles;
    u16 *pal;

    for (int i = 0; i < ITEM_ICON_SLOTS; ++i)
    {
        if (sItemIcons[i].loaded && sItemIcons[i].item == item)
        {
            sItemIcons[i].age = ++sIconClock;
            return sItemIcons[i].valid ? i : -1;
        }
        if (sItemIcons[i].age < sItemIcons[victim].age)
            victim = i;
    }
    if (!sIconBudget)
        return -1;
    sIconBudget = FALSE;
    tiles = Unlz(GetItemIconPicOrPalette(item, 0), &size);
    pal = Unlz(GetItemIconPicOrPalette(item, 1), NULL);
    sItemIcons[victim].valid = tiles && pal && size >= sizeof(sItemIcons[victim].tiles);
    if (sItemIcons[victim].valid)
    {
        memcpy(sItemIcons[victim].tiles, tiles, sizeof(sItemIcons[victim].tiles));
        ToPals(&sItemIcons[victim].pal, pal, 1);
    }
    free(tiles);
    free(pal);
    sItemIcons[victim].item = item;
    sItemIcons[victim].loaded = TRUE;
    sItemIcons[victim].age = ++sIconClock;
    return sItemIcons[victim].valid ? victim : -1;
}

static void DrawItemIcon(u16 item, int x, int y)
{
    int slot = ItemIcon(item);

    if (slot >= 0)
        DrawSprite(sItemIcons[slot].tiles, 3, 3, x, y, sItemIcons[slot].pal.c);
}

/* ------------------------------------------------------------------------ */
/* Battle art                                                               */
/* ------------------------------------------------------------------------ */

/*
 * The battle menus' backdrop and plates are the port's own pictures,
 * drawn smooth by scripts/gen_battle_art.py into bottom/battle.bin. A plate
 * takes its colour here - FIGHT's red, a move's type - so each pixel is
 * stored as what it adds over whatever lies under it:
 *
 *     out = under * (255 - alpha) / 255 + colour * A / 255 + ring * R / 255 + C
 *
 * (R only where a focused plate has its ring). Ids as the script's.
 */
enum
{
    BTA_BACKDROP = 0,
    BTA_FIGHT_WIDE = 16, BTA_FIGHT_FULL = 19, BTA_BALL = 22, BTA_BOTTOM = 25, BTA_MOVE = 28, BTA_CANCEL = 31,
    BTA_TARGET = 34,
    BTA_CLIP_FIGHT_WIDE = 40, BTA_CLIP_FIGHT_FULL = 41,
    BTA_ICON_BAG = 48, BTA_ICON_RUN, BTA_ICON_NEAR, BTA_ICON_BLOCK,
    BTA_PARTY_OK = 56, BTA_PARTY_STATUS, BTA_PARTY_FAINT, BTA_PARTY_EMPTY,
    BTA_COUNT = 64,
};
/* A plate's id plus its state. */
enum { BTA_NORMAL, BTA_FOCUS, BTA_PRESSED };
/* How far a pressed plate sinks. */
#define BTA_SINK 2

typedef struct
{
    s16 x, y;          /* the box, from the element's anchor */
    u16 w, h;
    u8 flags;
    const u8 *alpha, *a, *r, *c;   /* c: RGB565, little endian */
} BtaElem;

static struct
{
    u8 *file;
    BtaElem e[BTA_COUNT];
} sBta;

static u8 sExpand5[32], sExpand6[64];

static void LoadBattleArt(void)
{
    FILE *f = fopen("romfs:/bottom/battle.bin", "rb");
    long size;
    const u8 *p, *end;
    u16 count;

    for (int i = 0; i < 32; ++i)
        sExpand5[i] = i * 255 / 31;
    for (int i = 0; i < 64; ++i)
        sExpand6[i] = i * 255 / 63;
    if (!f)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: bottom/battle.bin missing, battle menus stay plain");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sBta.file = size > 10 ? malloc(size) : NULL;
    if (!sBta.file || fread(sBta.file, 1, size, f) != (size_t)size || memcmp(sBta.file, "EM3DBTA1", 8) != 0)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: bottom/battle.bin unreadable");
        free(sBta.file);
        sBta.file = NULL;
        fclose(f);
        return;
    }
    fclose(f);
    p = sBta.file + 8;
    end = sBta.file + size;
    count = p[0] | (p[1] << 8);
    p += 2;
    for (u16 n = 0; n < count && p + 10 <= end; ++n)
    {
        u8 id = p[0];
        BtaElem e = {
            .flags = p[1],
            .x = (s16)(p[2] | (p[3] << 8)), .y = (s16)(p[4] | (p[5] << 8)),
            .w = p[6] | (p[7] << 8), .h = p[8] | (p[9] << 8),
        };
        u32 area = (u32)e.w * e.h;

        p += 10;
        if (e.flags & 8)
        {
            e.c = p;
            p += area * 2;
        }
        else
        {
            e.alpha = p;
            p += area;
            if (!(e.flags & 4))
            {
                e.a = p;
                e.c = p + area;
                p += area * 3;
                if (e.flags & 2)
                {
                    e.r = p;
                    p += area;
                }
            }
        }
        if (p > end)
            break;
        if (id < BTA_COUNT)
            sBta.e[id] = e;
    }
    /* The backdrop is in the canvas' layout already: it is a cache. */
    sCache[CACHE_BATTLE] = (u16 *)sBta.e[BTA_BACKDROP].c;
}

typedef struct { u8 r, g, b; } Rgb;

static inline Rgb RgbOf(u16 c)
{
    return (Rgb){sExpand5[c >> 11], sExpand6[(c >> 5) & 63], sExpand5[c & 31]};
}

static inline u16 PackRgb(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* x / 255 for x up to 255 * 255 * 3, rounded. */
static inline int Div255(int x)
{
    return (x + 128 + ((x + 128) >> 8)) >> 8;
}

/* An element at its anchor, in `colour`, its ring (if any) in `ring`. */
static void DrawBta(u8 id, int ax, int ay, Rgb colour, Rgb ring)
{
    const BtaElem *e = id < BTA_COUNT ? &sBta.e[id] : NULL;
    int x0, y0, i0, i1, j0, j1;

    if (!e || !e->alpha || !e->a)
        return;
    x0 = ax + e->x + sOX;
    y0 = ay + e->y;
    i0 = sClipX0 - x0 > 0 ? sClipX0 - x0 : 0;
    j0 = sClipY0 - y0 > 0 ? sClipY0 - y0 : 0;
    i1 = sClipX1 - x0 < e->w ? sClipX1 - x0 : e->w;
    j1 = sClipY1 - y0 < e->h ? sClipY1 - y0 : e->h;
    for (int i = i0; i < i1; ++i)
    {
        u16 *dst = sDst + (x0 + i) * H + (H - 1 - (y0 + j0));

        for (int j = j0; j < j1; ++j, --dst)
        {
            u32 q = (u32)j * e->w + i;
            int al = e->alpha[q], A = e->a[q], R = e->r ? e->r[q] : 0;
            u16 c = e->c[q * 2] | (e->c[q * 2 + 1] << 8);
            Rgb under, add;

            if (!al && !A && !R && !c)
                continue;
            under = RgbOf(*dst);
            add = RgbOf(c);
            *dst = PackRgb(Div255(under.r * (255 - al) + colour.r * A + ring.r * R) + add.r,
                           Div255(under.g * (255 - al) + colour.g * A + ring.g * R) + add.g,
                           Div255(under.b * (255 - al) + colour.b * A + ring.b * R) + add.b);
        }
    }
}

/* An alpha mask's value at a point from its anchor (0 outside it). */
static u8 BtaAlpha(u8 id, int dx, int dy)
{
    const BtaElem *e = &sBta.e[id];

    dx -= e->x;
    dy -= e->y;
    if (!e->alpha || dx < 0 || dy < 0 || dx >= e->w || dy >= e->h)
        return 0;
    return e->alpha[dy * e->w + dx];
}

/* A coverage map (0-255 per pixel, w x h) in one colour, times a clip. */
static void DrawCoverage(const u8 *cover, int w, int h, int x, int y, Rgb colour, int opacity, u8 clip, int clipX,
                         int clipY)
{
    for (int i = 0; i < w; ++i)
    {
        int sx = x + i + sOX;

        if (sx < sClipX0 || sx >= sClipX1)
            continue;
        for (int j = 0; j < h; ++j)
        {
            int sy = y + j, al = cover[j * w + i];
            u16 *dst;
            Rgb under;

            if (!al || sy < sClipY0 || sy >= sClipY1)
                continue;
            if (clip != BTA_COUNT)
                al = Div255(al * BtaAlpha(clip, x + i - clipX, y + j - clipY));
            al = Div255(al * opacity);
            if (!al)
                continue;
            dst = sDst + sx * H + (H - 1 - sy);
            under = RgbOf(*dst);
            *dst = PackRgb(Div255(under.r * (255 - al) + colour.r * al), Div255(under.g * (255 - al) + colour.g * al),
                           Div255(under.b * (255 - al) + colour.b * al));
        }
    }
}

/* An item icon's shadow: its shape in black at a quarter. */
static void DrawItemIconShadow(u16 item, int x, int y)
{
    int slot = ItemIcon(item);
    static u8 cover[24 * 24];

    if (slot < 0)
        return;
    for (int t = 0; t < 9; ++t)
        for (int py = 0; py < 8; ++py)
            for (int px = 0; px < 8; ++px)
            {
                u8 v = (sItemIcons[slot].tiles[t * 32 + py * 4 + px / 2] >> ((px & 1) * 4)) & 15;

                cover[((t / 3) * 8 + py) * 24 + (t % 3) * 8 + px] = v ? 255 : 0;
            }
    DrawCoverage(cover, 24, 24, x, y, (Rgb){0, 0, 0}, 64, BTA_COUNT, 0, 0);
}

/*
 * FIGHT's watermark: Rayquaza, from its own front picture, as a smooth
 * silhouette 1.1 times its size, with its darkest pixels (its outline and
 * inner lines) as a second mask. Each screen pixel takes 4x4 samples of the
 * picture, softened by a [1 2 1] blur and read bilinearly, against a
 * threshold: an edge that follows the shape instead of its pixel steps.
 */
#define RAY_SIZE 70
static u8 sRayBody[RAY_SIZE * RAY_SIZE], sRayLines[RAY_SIZE * RAY_SIZE];

static float RaySample(const float *m, float u, float v)
{
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0, s = 0;

    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            int x = x0 + dx, y = y0 + dy;
            float wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);

            if (x >= 0 && y >= 0 && x < 64 && y < 64)
                s += wgt * m[y * 64 + x];
        }
    return s;
}

static void BuildRayquaza(void)
{
    static float body[64 * 64], lines[64 * 64], soft[64 * 64];
    u8 *tiles = Unlz(gMonFrontPicTable[SPECIES_RAYQUAZA].data, NULL);
    u16 *pal = Unlz(gMonPaletteTable[SPECIES_RAYQUAZA].data, NULL);

    if (tiles && pal)
    {
        for (int t = 0; t < 64; ++t)
            for (int py = 0; py < 8; ++py)
                for (int px = 0; px < 8; ++px)
                {
                    u8 v = (tiles[t * 32 + py * 4 + px / 2] >> ((px & 1) * 4)) & 15;
                    int x = (t % 8) * 8 + px, y = (t / 8) * 8 + py;
                    u16 c = pal[v];
                    int sum = ((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) * 255 / 31;

                    body[y * 64 + x] = v ? 1.0f : 0.0f;
                    lines[y * 64 + x] = v && sum < 200 ? 1.0f : 0.0f;
                }
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                float s = 0, n = 0;

                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        float wgt = (dx ? 1.0f : 2.0f) * (dy ? 1.0f : 2.0f);

                        if (x + dx >= 0 && y + dy >= 0 && x + dx < 64 && y + dy < 64)
                            s += wgt * body[(y + dy) * 64 + x + dx];
                        n += wgt;
                    }
                soft[y * 64 + x] = s / n;
            }
        for (int Y = 0; Y < RAY_SIZE; ++Y)
            for (int X = 0; X < RAY_SIZE; ++X)
            {
                int inBody = 0, inLines = 0;

                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                    {
                        float u = (X + (sx + 0.5f) / 4) * (64.0f / 70.4f) - 0.5f;
                        float v = (Y + (sy + 0.5f) / 4) * (64.0f / 70.4f) - 0.5f;

                        inBody += RaySample(soft, u, v) >= 0.47f;
                        inLines += RaySample(lines, u, v) >= 0.43f;
                    }
                sRayBody[Y * RAY_SIZE + X] = inBody * 255 / 16;
                sRayLines[Y * RAY_SIZE + X] = inLines * 255 / 16;
            }
    }
    free(tiles);
    free(pal);
}

/* Which of the three type label palettes each type uses
 * (pokemon_summary_screen.c's sMoveTypeToOamPaletteNum, minus 13). */
static const u8 sTypeIconPal[NUMBER_OF_MON_TYPES] = {
    [TYPE_NORMAL] = 0, [TYPE_FIGHTING] = 0, [TYPE_FLYING] = 1, [TYPE_POISON] = 1,
    [TYPE_GROUND] = 0, [TYPE_ROCK] = 0, [TYPE_BUG] = 2, [TYPE_GHOST] = 1,
    [TYPE_STEEL] = 0, [TYPE_MYSTERY] = 2, [TYPE_FIRE] = 0, [TYPE_WATER] = 1,
    [TYPE_GRASS] = 2, [TYPE_ELECTRIC] = 0, [TYPE_PSYCHIC] = 1, [TYPE_ICE] = 1,
    [TYPE_DRAGON] = 2, [TYPE_DARK] = 0,
};

/* A move plate's colour: the main colour of the type's own label, its most
 * used one that is neither its white nor its dark outline. */
static Rgb sTypeColour[NUMBER_OF_MON_TYPES];

static void BuildTypeColours(void)
{
    for (int type = 0; type < NUMBER_OF_MON_TYPES; ++type)
    {
        const u16 *pal = sRes.typePal[sTypeIconPal[type]].c;
        int count[16] = {0}, best = -1;

        sTypeColour[type] = (Rgb){150, 150, 150};
        if (!sRes.typeTiles)
            continue;
        for (int i = 0; i < 8 * 32; ++i)
        {
            u8 byte = sRes.typeTiles[type * 8 * 32 + i];

            ++count[byte & 15];
            ++count[byte >> 4];
        }
        for (int v = 1; v < 16; ++v)
        {
            Rgb c = RgbOf(pal[v]);

            if ((c.r > 230 && c.g > 230 && c.b > 230) || c.r + c.g + c.b <= 120)
                continue;
            if (best < 0 || count[v] > count[best])
                best = v;
        }
        if (best >= 0)
            sTypeColour[type] = RgbOf(pal[best]);
    }
}

/* ------------------------------------------------------------------------ */
/* Text                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
    const u16 *glyphs;
    const u8 *widths;
    u8 height, lineHeight;
} Font;

static Font sSmall, sNormal;

static const u16 *ResolveFontBase(const u16 *glyphs)
{
    const void *resolved = Port_ResolveFontPointer(glyphs);

    if (resolved == glyphs)
        resolved = Port_ResolveAssetPointer(glyphs);
    return resolved;
}

/* The glyph payloads live in the asset cache; resolve them per redraw. */
static void ResolveFonts(void)
{
    sSmall.glyphs = ResolveFontBase(gFontSmallLatinGlyphs);
    sSmall.widths = gFontSmallLatinGlyphWidths;
    sSmall.height = 13;
    sSmall.lineHeight = 13;
    sNormal.glyphs = ResolveFontBase(gFontNormalLatinGlyphs);
    sNormal.widths = gFontNormalLatinGlyphWidths;
    sNormal.height = 16;
    sNormal.lineHeight = 16;
}

/* Walks a game string: returns the next glyph, 0xFFFE for a line break or
 * 0xFFFF at the end. */
static u16 NextGlyph(const u8 **str)
{
    for (;;)
    {
        u8 c = *(*str)++;

        switch (c)
        {
        case EOS:
            --*str;
            return 0xFFFF;
        case CHAR_NEWLINE:
        case CHAR_PROMPT_SCROLL:
        case CHAR_PROMPT_CLEAR:
            return 0xFFFE;
        case EXT_CTRL_CODE_BEGIN:
            if (**str == EOS)
                return 0xFFFF;
            *str += GetExtCtrlCodeLength(**str);
            break;
        case PLACEHOLDER_BEGIN:
        case CHAR_DYNAMIC:
        case CHAR_KEYPAD_ICON:
            if (**str != EOS)
                ++*str;
            break;
        case CHAR_EXTRA_SYMBOL:
            if (**str == EOS)
                return 0xFFFF;
            return 0x100 | *(*str)++;
        default:
            return c;
        }
    }
}

static int StrWidth(const Font *font, const u8 *str)
{
    int width = 0, best = 0;

    if (!str)
        return 0;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        if (g == 0xFFFE)
        {
            if (width > best) best = width;
            width = 0;
            continue;
        }
        width += font->widths[g];
    }
    return width > best ? width : best;
}

static int DrawGlyph(const Font *font, u16 glyph, int x, int y, u16 fg, u16 shadow)
{
    const u16 *base = font->glyphs + glyph * 0x20;
    int width = font->widths[glyph];
    int sx = x + sOX;
    bool8 inside = sx >= sClipX0 && y >= sClipY0 && sx + width <= sClipX1 && y + font->height <= sClipY1;
    u16 *origin = sDst + (inside ? sx * H + (H - 1 - y) : 0);

    /* Wholly outside the clip: nothing to write. */
    if (sx >= sClipX1 || y >= sClipY1 || sx + width <= sClipX0 || y + font->height <= sClipY0)
        return font->widths[glyph];
    if (width > 16)
        width = 16;
    for (int row = 0; row < font->height; ++row)
    {
        for (int half = 0; half < 2 && half * 8 < width; ++half)
        {
            u16 bits = base[(row >= 8 ? 0x10 : 0) + half * 8 + (row & 7)];

            if (!bits)
                continue;
            for (int k = 0; k < 8 && half * 8 + k < width; ++k)
            {
                u8 byte = k < 4 ? bits >> 8 : bits & 0xFF;
                u8 v = (byte >> (6 - 2 * (k & 3))) & 3;
                int px = half * 8 + k;

                if (v != 1 && v != 2)
                    continue;
                if (inside)
                    origin[px * H - row] = v == 1 ? fg : shadow;
                else
                    Put(x + px, y + row, v == 1 ? fg : shadow);
            }
        }
    }
    return font->widths[glyph];
}

/* Draws a game string; returns the x where it ended. */
static int DrawStr(const Font *font, const u8 *str, int x, int y, u16 fg, u16 shadow)
{
    int left = x;

    if (!font->glyphs || !str)
        return x;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        if (g == 0xFFFE)
        {
            x = left;
            y += font->lineHeight;
            continue;
        }
        x += DrawGlyph(font, g, x, y, fg, shadow);
    }
    return x;
}

static void DrawStrRight(const Font *font, const u8 *str, int right, int y, u16 fg, u16 shadow)
{
    DrawStr(font, str, right - StrWidth(font, str), y, fg, shadow);
}

static void DrawStrCentered(const Font *font, const u8 *str, int cx, int y, u16 fg, u16 shadow)
{
    DrawStr(font, str, cx - StrWidth(font, str) / 2, y, fg, shadow);
}

/* Whether a glyph paints (glyph or shadow) the pixel at column px of row. */
static bool8 GlyphInk(const Font *font, u16 glyph, int px, int row)
{
    u16 bits = font->glyphs[glyph * 0x20 + (row >= 8 ? 0x10 : 0) + (px / 8) * 8 + (row & 7)];
    u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;
    u8 v = (byte >> (6 - 2 * (px & 3))) & 3;

    return v == 1 || v == 2;
}

/* The columns a one-line string really paints, [*x0, *x1) from its origin;
 * *x1 <= *x0 when it paints none. */
static void InkColumns(const Font *font, const u8 *str, int *x0, int *x1)
{
    int x = 0;

    *x0 = 0x7FFF;
    *x1 = 0;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF && g != 0xFFFE;)
    {
        int width = font->widths[g] > 16 ? 16 : font->widths[g];

        for (int px = 0; px < width; ++px)
            for (int row = 0; row < font->height; ++row)
                if (GlyphInk(font, g, px, row))
                {
                    if (x + px < *x0) *x0 = x + px;
                    if (x + px + 1 > *x1) *x1 = x + px + 1;
                    break;
                }
        x += font->widths[g];
    }
}

/* The rows a capital paints, [*top, *bottom): every label is centred on the
 * same band, so one with an accent (POKéMON) keeps the others' baseline. */
static void CapRows(const Font *font, int *top, int *bottom)
{
    *top = font->height;
    *bottom = 0;
    for (int row = 0; row < font->height; ++row)
        for (int px = 0; px < font->widths[CHAR_H]; ++px)
            if (GlyphInk(font, CHAR_H, px, row))
            {
                if (row < *top) *top = row;
                if (row + 1 > *bottom) *bottom = row + 1;
                break;
            }
}

/* A one-line string centred in [x0, x1) x [y0, y1): across by the pixels it
 * paints, down by the capitals' band. */
static void DrawStrIn(const Font *font, const u8 *str, int x0, int x1, int y0, int y1, u16 fg, u16 shadow)
{
    int ix0, ix1, top, bottom;

    if (!font->glyphs || !str)
        return;
    InkColumns(font, str, &ix0, &ix1);
    if (ix1 <= ix0)
        return;
    CapRows(font, &top, &bottom);
    DrawStr(font, str, x0 + ((x1 - x0) - (ix1 - ix0)) / 2 - ix0, y0 + ((y1 - y0) - (bottom - top)) / 2 - top, fg,
            shadow);
}

/* Latin labels for the few words the game has no standalone string for. */
static const u8 *Ascii(const char *text)
{
    static u8 ring[8][40];
    static u8 next;
    u8 *out = ring[next++ & 7];
    int n = 0;

    for (; *text && n < 39; ++text)
    {
        char c = *text;
        u8 v;

        if (c >= 'A' && c <= 'Z') v = CHAR_A + (c - 'A');
        else if (c >= 'a' && c <= 'z') v = CHAR_a + (c - 'a');
        else if (c >= '0' && c <= '9') v = CHAR_0 + (c - '0');
        else if (c == '/') v = CHAR_SLASH;
        else if (c == '-') v = CHAR_HYPHEN;
        else if (c == '.') v = CHAR_PERIOD;
        else if (c == ':') v = CHAR_COLON;
        else if (c == '!') v = CHAR_EXCL_MARK;
        else if (c == '?') v = CHAR_QUESTION_MARK;
        else if (c == '\'') v = CHAR_SGL_QUOTE_RIGHT;
        else if (c == '*') v = CHAR_e_ACUTE; /* POK*MON */
        else v = CHAR_SPACE;
        out[n++] = v;
    }
    out[n] = EOS;
    return out;
}

static const u8 *Number(u32 value, int digits, enum StringConvertMode mode)
{
    static u8 ring[8][12];
    static u8 next;
    u8 *out = ring[next++ & 7];

    ConvertIntToDecimalStringN(out, value, mode, digits);
    return out;
}

/*
 * The battle plates' labels: the game's glyphs, enlarged by 1, 1.5, 2 or
 * 2.5 (k = 4, 6, 8, 10 quarter pixels a glyph pixel), with a smooth round
 * outline and, for some, a soft drop shadow, as gen_battle_art.py's mockups
 * draw them. Built at 4x - glyphs, a blurred-and-hardened outline merged with
 * a dilation, the outline moved down and blurred for the shadow - then
 * averaged down to screen pixels. A label is built once and kept.
 */
#define SMOOTH_PAD 4    /* glyph pixels around the text, for outline and shadow */
#define SMOOTH_SLOTS 20

typedef struct
{
    u32 age;
    u8 key[40], font, k;
    Rgb fg, outline, shadow;
    bool8 hasShadow;
    u16 w, h;
    u8 *px;            /* per pixel: coverage, then premultiplied r, g, b */
} SmoothText;

static SmoothText sSmooth[SMOOTH_SLOTS];
static u32 sSmoothClock;
/* How many labels or plates a redraw may still build (0xFF: any). The warm
 * up while the turn plays out builds one a frame (WarmBattleMenus). */
static u8 sBuildBudget = 0xFF;

static bool8 TakeBuildBudget(void)
{
    if (sBuildBudget == 0)
        return FALSE;
    if (sBuildBudget != 0xFF)
        --sBuildBudget;
    return TRUE;
}

/* Three box blurs in a row approximate a Gaussian; one pass, both axes. */
static void BoxBlur(u8 *buf, u8 *tmp, int w, int h, int radius)
{
    int span = 2 * radius + 1;

    if (radius <= 0)
        return;
    for (int y = 0; y < h; ++y)
    {
        const u8 *row = buf + y * w;
        int sum = 0;

        for (int x = -radius; x <= radius; ++x)
            sum += row[x < 0 ? 0 : (x >= w ? w - 1 : x)];
        for (int x = 0; x < w; ++x)
        {
            int add = x + radius + 1, sub = x - radius;

            tmp[y * w + x] = sum / span;
            sum += row[add >= w ? w - 1 : add] - row[sub < 0 ? 0 : sub];
        }
    }
    for (int x = 0; x < w; ++x)
    {
        int sum = 0;

        for (int y = -radius; y <= radius; ++y)
            sum += tmp[(y < 0 ? 0 : (y >= h ? h - 1 : y)) * w + x];
        for (int y = 0; y < h; ++y)
        {
            int add = y + radius + 1, sub = y - radius;

            buf[y * w + x] = sum / span;
            sum += tmp[(add >= h ? h - 1 : add) * w + x] - tmp[(sub < 0 ? 0 : sub) * w + x];
        }
    }
}

static void GaussLike(u8 *buf, u8 *tmp, int w, int h, float sigma)
{
    int radius = (int)((sqrtf(4.0f * sigma * sigma + 1.0f) - 1.0f) / 2.0f + 0.5f);

    for (int pass = 0; pass < 3; ++pass)
        BoxBlur(buf, tmp, w, h, radius);
}

/* A square dilation of a 0/255 mask, separable: a running count of the set
 * pixels in the window, so its cost does not grow with the radius. */
static void Dilate(const u8 *src, u8 *dst, u8 *tmp, int w, int h, int radius)
{
    for (int y = 0; y < h; ++y)
    {
        const u8 *row = src + y * w;
        int count = 0;

        for (int x = 0; x < radius && x < w; ++x)
            count += row[x] != 0;
        for (int x = 0; x < w; ++x)
        {
            if (x + radius < w)
                count += row[x + radius] != 0;
            if (x - radius - 1 >= 0)
                count -= row[x - radius - 1] != 0;
            tmp[y * w + x] = count ? 255 : 0;
        }
    }
    for (int x = 0; x < w; ++x)
    {
        int count = 0;

        for (int y = 0; y < radius && y < h; ++y)
            count += tmp[y * w + x] != 0;
        for (int y = 0; y < h; ++y)
        {
            if (y + radius < h)
                count += tmp[(y + radius) * w + x] != 0;
            if (y - radius - 1 >= 0)
                count -= tmp[(y - radius - 1) * w + x] != 0;
            dst[y * w + x] = count ? 255 : 0;
        }
    }
}

static bool8 BuildSmoothText(SmoothText *t, const Font *font, const u8 *str)
{
    int textW = StrWidth(font, str), k = t->k;
    int w4 = (textW + 2 * SMOOTH_PAD) * k, h4 = (font->height + 2 * SMOOTH_PAD) * k;
    int r = k > 4 ? k : 4, x = SMOOTH_PAD;
    u8 *glyph = calloc((size_t)w4 * h4, 4), *ring, *shadow, *tmp;

    if (!glyph)
        return FALSE;
    ring = glyph + w4 * h4;
    shadow = ring + w4 * h4;
    tmp = shadow + w4 * h4;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF && g != 0xFFFE;)
    {
        const u16 *base = font->glyphs + g * 0x20;
        int width = font->widths[g] > 16 ? 16 : font->widths[g];

        for (int row = 0; row < font->height; ++row)
            for (int px = 0; px < width; ++px)
            {
                u16 bits = base[(row >= 8 ? 0x10 : 0) + (px / 8) * 8 + (row & 7)];
                u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;

                if (((byte >> (6 - 2 * (px & 3))) & 3) != 1)
                    continue;
                for (int dy = 0; dy < k; ++dy)
                    memset(glyph + ((SMOOTH_PAD + row) * k + dy) * w4 + (x + px) * k, 255, k);
            }
        x += font->widths[g];
    }
    /* The outline: blurred and hardened, and never thinner than a dilation. */
    memcpy(ring, glyph, (size_t)w4 * h4);
    GaussLike(ring, tmp, w4, h4, r * 0.55f);
    for (int i = 0; i < w4 * h4; ++i)
        ring[i] = ring[i] > 18 ? 255 : ring[i] * 14;
    Dilate(glyph, shadow, tmp, w4, h4, (int)(r * 0.7f + 0.5f));
    for (int i = 0; i < w4 * h4; ++i)
        if (shadow[i] > ring[i])
            ring[i] = shadow[i];
    memset(shadow, 0, (size_t)w4 * h4);
    if (t->hasShadow)
    {
        int drop = (int)(4.8f * (k > 5 ? k / 5.0f : 1.0f) + 0.5f);

        for (int y = drop; y < h4; ++y)
            memcpy(shadow + y * w4, ring + (y - drop) * w4, w4);
        GaussLike(shadow, tmp, w4, h4, 3.2f);
        for (int i = 0; i < w4 * h4; ++i)
            shadow[i] = shadow[i] * 6 / 10;
    }
    /* Down to screen pixels: coverage and premultiplied colour. */
    t->w = (w4 + 3) / 4;
    t->h = (h4 + 3) / 4;
    free(t->px);
    t->px = calloc((size_t)t->w * t->h, 4);
    if (!t->px)
    {
        free(glyph);
        return FALSE;
    }
    for (int oy = 0; oy < t->h; ++oy)
        for (int ox = 0; ox < t->w; ++ox)
        {
            int a = 0, cr = 0, cg = 0, cb = 0;

            for (int sy = oy * 4; sy < oy * 4 + 4 && sy < h4; ++sy)
                for (int sx = ox * 4; sx < ox * 4 + 4 && sx < w4; ++sx)
                {
                    int i = sy * w4 + sx, ri = ring[i], al = ri > shadow[i] ? ri : shadow[i];
                    Rgb c;

                    if (!al)
                        continue;
                    if (glyph[i])
                        c = t->fg;
                    else
                        c = (Rgb){(u8)Div255(t->shadow.r * (255 - ri) + t->outline.r * ri),
                                  (u8)Div255(t->shadow.g * (255 - ri) + t->outline.g * ri),
                                  (u8)Div255(t->shadow.b * (255 - ri) + t->outline.b * ri)};
                    a += al;
                    cr += c.r * al;
                    cg += c.g * al;
                    cb += c.b * al;
                }
            t->px[(oy * t->w + ox) * 4 + 0] = a / 16;
            t->px[(oy * t->w + ox) * 4 + 1] = cr / (255 * 16);
            t->px[(oy * t->w + ox) * 4 + 2] = cg / (255 * 16);
            t->px[(oy * t->w + ox) * 4 + 3] = cb / (255 * 16);
        }
    free(glyph);
    return TRUE;
}

/* Draws a label with its glyph origin at (x, y); k quarter pixels a glyph pixel. */
static void DrawSmoothStr(const Font *font, const u8 *str, int x, int y, int k, Rgb fg, Rgb outline,
                          const Rgb *shadow)
{
    SmoothText *t = NULL, *victim = &sSmooth[0];
    int n = 0;

    if (!font->glyphs || !str)
        return;
    while (str[n] != EOS && n < (int)sizeof(t->key) - 1)
        ++n;
    for (int i = 0; i < SMOOTH_SLOTS && !t; ++i)
    {
        SmoothText *s = &sSmooth[i];

        if (s->px && s->font == (font == &sSmall) && s->k == k && memcmp(s->key, str, n) == 0 && s->key[n] == EOS
         && !memcmp(&s->fg, &fg, sizeof(fg)) && !memcmp(&s->outline, &outline, sizeof(outline))
         && s->hasShadow == (shadow != NULL) && (!shadow || !memcmp(&s->shadow, shadow, sizeof(*shadow))))
            t = s;
        else if (s->age < victim->age)
            victim = s;
    }
    if (!t)
    {
        if (!TakeBuildBudget())
            return;
        t = victim;
        memcpy(t->key, str, n);
        t->key[n] = EOS;
        t->font = font == &sSmall;
        t->k = k;
        t->fg = fg;
        t->outline = outline;
        t->hasShadow = shadow != NULL;
        t->shadow = shadow ? *shadow : outline;
        if (!BuildSmoothText(t, font, t->key))
        {
            t->age = 0;
            return;
        }
    }
    t->age = ++sSmoothClock;
    /* The bitmap starts SMOOTH_PAD glyph pixels up and left: k screen pixels. */
    x -= k;
    y -= k;
    for (int i = 0; i < t->w; ++i)
    {
        int sx = x + i + sOX;

        if (sx < sClipX0 || sx >= sClipX1)
            continue;
        for (int j = 0; j < t->h; ++j)
        {
            const u8 *p = t->px + (j * t->w + i) * 4;
            int sy = y + j;
            u16 *dst;
            Rgb under;

            if (!p[0] || sy < sClipY0 || sy >= sClipY1)
                continue;
            dst = sDst + sx * H + (H - 1 - sy);
            under = RgbOf(*dst);
            *dst = PackRgb(Div255(under.r * (255 - p[0])) + p[1], Div255(under.g * (255 - p[0])) + p[2],
                           Div255(under.b * (255 - p[0])) + p[3]);
        }
    }
}

/* The width a label's pixels span, enlarged k quarter pixels a glyph pixel. */
static int SmoothInkWidth(const Font *font, const u8 *str, int k)
{
    int x0, x1;

    InkColumns(font, str, &x0, &x1);
    return x1 > x0 ? (x1 - x0) * k / 4 : 0;
}

/* Text colours from the message box palette. */
#define TXT(i) (sRes.text.c[(i)])
#define TXT_WHITE TXT(TEXT_COLOR_WHITE)
#define TXT_DARK TXT(TEXT_COLOR_DARK_GRAY)
#define TXT_LIGHT TXT(TEXT_COLOR_LIGHT_GRAY)
#define TXT_RED TXT(TEXT_COLOR_RED)
#define TXT_LRED TXT(TEXT_COLOR_LIGHT_RED)
#define TXT_BLUE TXT(TEXT_COLOR_BLUE)
#define TXT_LBLUE TXT(TEXT_COLOR_LIGHT_BLUE)

/* Labels on a normal button, and on one shown darker (chosen or pressed). */
#define LABEL_FG(on) ((on) ? TXT_WHITE : TXT_DARK)
#define LABEL_SH(on) ((on) ? TXT_DARK : TXT_LIGHT)

/* ------------------------------------------------------------------------ */
/* Frames and backgrounds                                                   */
/* ------------------------------------------------------------------------ */

enum { BOX_MENU, BOX_MESSAGE };

/*
 * The battle text box's two frames, stretched in whole tiles: the white menu
 * box for buttons and lists, the teal message box for what the game says.
 * A dark box is the same frame at lower brightness: chosen or pressed.
 */
static void DrawBoxEx(int kind, int x, int y, int wt, int ht, bool8 dark)
{
    static const u8 menu[3][3] = {{0x12, 0x13, 0x14}, {0x15, 0x16, 0x17}, {0x18, 0x19, 0x1A}};
    static const u8 message[3][5] = {
        {0x03, 0x04, 0x05, 0x06, 0x07},
        {0x08, 0x09, 0x0A, 0x0B, 0x0C},
        {0x0D, 0x0E, 0x0F, 0x10, 0x11},
    };
    /* Caps are one tile wide on the menu box, two on the message box. */
    int cap = kind == BOX_MENU ? 1 : 2;
    u16 bank = kind == BOX_MENU ? 0x1000 : 0;
    u8 center = kind == BOX_MENU ? menu[1][1] : message[1][2];
    int solid = center < sRes.boxTileCount ? SolidTileColor(sRes.boxTiles + center * 32) : -1;
    const Pal *pals = dark ? sRes.boxPalDark : sRes.boxPal;

    /* The interior is one flat colour: fill it, draw only the frame. */
    if (solid >= 0 && wt > 2 * cap && ht > 2)
        FillRect(x + cap * 8, y + 8, (wt - 2 * cap) * 8, (ht - 2) * 8, pals[bank >> 12].c[solid]);
    for (int ty = 0; ty < ht; ++ty)
    {
        int row = ty == 0 ? 0 : ty == ht - 1 ? 2 : 1;
        for (int tx = 0; tx < wt; ++tx)
        {
            bool8 inside = row == 1 && tx >= cap && tx < wt - cap;
            u8 tile;

            if (inside && solid >= 0)
                continue;
            if (kind == BOX_MENU)
                tile = menu[row][tx == 0 ? 0 : tx == wt - 1 ? 2 : 1];
            else
                tile = message[row][tx == 0 ? 0 : tx == 1 ? 1 : tx == wt - 2 ? 3 : tx == wt - 1 ? 4 : 2];
            DrawMapEntry(sRes.boxTiles, sRes.boxTileCount, bank | tile, x + tx * 8, y + ty * 8, pals);
        }
    }
}

static void DrawBox(int kind, int x, int y, int wt, int ht)
{
    DrawBoxEx(kind, x, y, wt, ht, FALSE);
}

/*
 * The party menu background, framed on all four sides: the GBA screen leaves
 * its right edge open because the picture runs off it, here the left border
 * is mirrored instead.
 */
static void DrawPartyBackground(int cols, int rows)
{
    FillRect(0, 0, cols * 8, rows * 8, sRes.partyPal[0].c[0]);
    for (int ty = 0; ty < rows; ++ty)
    {
        for (int tx = 0; tx < cols; ++tx)
        {
            int edge = tx < cols / 2 ? tx : cols - 1 - tx;
            u16 flip = tx < cols / 2 ? 0 : 0x400, t;

            if (edge < 2) t = 0x002, flip = 0;
            else if (edge > 2) t = ty == 0 ? 0x005 : ty == rows - 1 ? 0x008 : 0x00E, flip = 0;
            else if (ty == 0) t = 0x004;
            else if (ty == 1) t = 0x006;
            else if (ty == rows - 2) t = 0x009;
            else if (ty == rows - 1) t = 0x007;
            else t = 0x00E;
            DrawMapEntry(sRes.partyTiles, sRes.partyTileCount, 0x1000 | flip | t, tx * 8, ty * 8, sRes.partyPal);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The column's look, and the sections' light green                         */
/* ------------------------------------------------------------------------ */

/*
 * The column is the port's own: an emerald rail with a diamond facet, the
 * buttons on it as plates (outline with clipped corners, a highlight row,
 * two shade rows, a tinted socket for the game's icon), a keyboard focus
 * ring, and below a groove the Y and RUN buttons. Native screens on the left
 * (map, save, options) and the party menu's and bag's backgrounds sit on one
 * light green with a thin frame line and a faint Poké Ball. The only new
 * pictures, the RUN shoe and the Y badge, are in 3ds_bottom_art.h.
 */

/* A plate's colours: outline, highlight row, body, shade rows, the icon's
 * socket and the socket's shade. */
typedef struct { u16 o, h, f, s, t, u; } PlateLook;

enum { RAIL_DEEP, RAIL_SHADE, RAIL_BODY, RAIL_FACET, RAIL_LIGHT, RAIL_COUNT };
enum { SECTION_BASE, SECTION_LINE, SECTION_LIGHT, SECTION_BALL, SECTION_BALL_TOP, SECTION_COUNT };
enum { LED_EDGE, LED_OFF, LED_OFF_LIGHT, LED_ON, LED_ON_LIGHT, LED_COUNT };

static struct
{
    u16 rail[RAIL_COUNT];
    u16 section[SECTION_COUNT];
    /* Normal, chosen or pressed, not available, and RUN turned on. */
    PlateLook plate, chosen, off, run;
    /* The focus ring's two blink frames: outer ring, inner line. */
    u16 ring[2], ringIn[2];
    u16 chosenShadow, offText, offShadow;
    u16 led[LED_COUNT];
    Pal art;
} sLook;

#define LOOK(r, g, b) Rgb565(RGB(r, g, b))

static void InitLook(void)
{
    static const u16 section[SECTION_COUNT] = {
        CTR_SECTION_BASE, CTR_SECTION_LINE, CTR_SECTION_LIGHT, CTR_SECTION_BALL, CTR_SECTION_BALL_TOP,
    };

    sLook.rail[RAIL_DEEP] = LOOK(3, 9, 7);
    sLook.rail[RAIL_SHADE] = LOOK(5, 14, 10);
    sLook.rail[RAIL_BODY] = LOOK(6, 17, 12);
    sLook.rail[RAIL_FACET] = LOOK(9, 21, 15);
    sLook.rail[RAIL_LIGHT] = LOOK(20, 28, 23);
    for (int i = 0; i < SECTION_COUNT; ++i)
        sLook.section[i] = Rgb565(section[i]);
    sLook.plate = (PlateLook){LOOK(7, 9, 13), LOOK(31, 31, 31), LOOK(30, 30, 31), LOOK(25, 26, 28), LOOK(26, 30, 27),
                              LOOK(20, 26, 22)};
    sLook.chosen = (PlateLook){LOOK(3, 11, 7), LOOK(21, 30, 23), LOOK(11, 24, 16), LOOK(7, 19, 12), LOOK(8, 21, 13),
                               LOOK(6, 17, 11)};
    sLook.off = (PlateLook){LOOK(17, 18, 20), LOOK(29, 29, 29), LOOK(27, 27, 27), LOOK(24, 24, 25), LOOK(25, 25, 25),
                            LOOK(23, 23, 23)};
    sLook.run = sLook.plate;
    sLook.run.h = LOOK(29, 31, 29);
    sLook.run.f = sLook.plate.t;
    sLook.run.s = sLook.plate.u;
    sLook.ring[0] = LOOK(31, 13, 6);
    sLook.ring[1] = LOOK(31, 25, 9);
    sLook.ringIn[0] = LOOK(31, 22, 15);
    sLook.ringIn[1] = LOOK(31, 30, 20);
    sLook.chosenShadow = LOOK(4, 14, 9);
    sLook.offText = LOOK(18, 18, 19);
    sLook.offShadow = LOOK(29, 29, 29);
    sLook.led[LED_EDGE] = LOOK(2, 6, 5);
    sLook.led[LED_OFF] = LOOK(12, 14, 13);
    sLook.led[LED_OFF_LIGHT] = LOOK(19, 21, 20);
    sLook.led[LED_ON] = LOOK(8, 29, 12);
    sLook.led[LED_ON_LIGHT] = LOOK(26, 31, 26);
    ToPals(&sLook.art, sArtPalette, 1);
}

/*
 * A plate: 1px outline with its corners cut and its inner corners filled, a
 * highlight row, two shade rows, and the first `socket` columns tinted for an
 * icon (0: none). Pressed, the highlight goes and the shade starts a row
 * lower, so the face looks pushed in.
 */
static void DrawPlate(int x, int y, int w, int h, const PlateLook *p, int socket, bool8 pressed)
{
    int shade = h - 3 + (pressed ? 1 : 0);

    FillRect(x + 1, y, w - 2, 1, p->o);
    FillRect(x + 1, y + h - 1, w - 2, 1, p->o);
    for (int j = 1; j < h - 1; ++j)
    {
        bool8 top = j == 1 && !pressed, low = j >= shade;
        u16 body = top ? p->h : low ? p->s : p->f;

        Put(x, y + j, p->o);
        Put(x + w - 1, y + j, p->o);
        if (socket > 0)
        {
            FillRect(x + 1, y + j, socket - 2, 1, top ? p->h : low ? p->u : p->t);
            Put(x + socket - 1, y + j, p->u);
            FillRect(x + socket, y + j, w - 1 - socket, 1, body);
        }
        else
        {
            FillRect(x + 1, y + j, w - 2, 1, body);
        }
    }
    Put(x + 1, y + 1, p->o);
    Put(x + w - 2, y + 1, p->o);
    Put(x + 1, y + h - 2, p->o);
    Put(x + w - 2, y + h - 2, p->o);
}

/* The keyboard focus: the plate's edge turns warm and a ring stands 1px
 * outside it, in the gap between plates. */
static void DrawRing(int x, int y, int w, int h, u8 frame)
{
    u16 a = sLook.ring[frame & 1], b = sLook.ringIn[frame & 1];

    FillRect(x + 1, y - 1, w - 2, 1, a);
    FillRect(x + 1, y + h, w - 2, 1, a);
    FillRect(x - 1, y + 1, 1, h - 2, a);
    FillRect(x + w, y + 1, 1, h - 2, a);
    FillRect(x + 1, y, w - 2, 1, b);
    FillRect(x + 1, y + h - 1, w - 2, 1, b);
    FillRect(x, y + 1, 1, h - 2, b);
    FillRect(x + w - 1, y + 1, 1, h - 2, b);
    Put(x, y, a);
    Put(x + w - 1, y, a);
    Put(x, y + h - 1, a);
    Put(x + w - 1, y + h - 1, a);
}

/* The column's geometry: eight plates, then a groove and the Y and RUN
 * buttons. A plate's hit is its whole 24px row of the column. */
#define PLATE_X (COL_X + 4)
#define PLATE_W 74
#define PLATE_H 22
#define PLATE_Y(i) (3 + (i) * 24)
#define PLATE_SOCKET 25
#define GROOVE_Y 196
#define SQUARE_Y 201
#define SQUARE_SIZE 36
#define SQUARE_X(k) (COL_X + 4 + (k) * 38)

/* The rail: the diamond facet over the whole column, the seam that cuts it
 * from whatever the section draws, and the groove above Y and RUN. */
static void DrawColumnBackground(void)
{
    for (int x = COL_X; x < W; ++x)
        for (int y = 0; y < H; ++y)
        {
            int d = abs(2 * (x & 7) - 7) + abs(2 * (y & 7) - 7);
            Put(x, y, sLook.rail[d == 8 ? RAIL_FACET : RAIL_BODY]);
        }
    FillRect(COL_X, 0, 1, H, sLook.rail[RAIL_DEEP]);
    FillRect(COL_X + 1, 0, 1, H, sLook.rail[RAIL_LIGHT]);
    FillRect(COL_X + 2, 0, 1, H, sLook.rail[RAIL_SHADE]);
    FillRect(COL_X + 3, GROOVE_Y, W - COL_X - 3, 1, sLook.rail[RAIL_DEEP]);
    FillRect(COL_X + 3, GROOVE_Y + 1, W - COL_X - 3, 1, sLook.rail[RAIL_LIGHT]);
}

/*
 * The faint Poké Ball in the middle of the light green: 2px rim, 2px band,
 * the button's ring with nothing inside, the top half half a step lighter.
 * Symmetric about the pixel grid: distances from pixel centres, doubled so
 * they stay integers. 0 nothing, 1 the line colour, 2 the top half.
 */
#define BALL_R CTR_SECTION_BALL_RADIUS
static u8 sBall[2 * BALL_R * 2 * BALL_R];

static void BuildBall(void)
{
    for (int y = 0; y < 2 * BALL_R; ++y)
        for (int x = 0; x < 2 * BALL_R; ++x)
        {
            int dx = 2 * x + 1 - 2 * BALL_R, dy = 2 * y + 1 - 2 * BALL_R, d2 = dx * dx + dy * dy;
            u8 v = 0;

            if (d2 >= 4 * BALL_R * BALL_R)
                v = 0;
            else if (d2 >= 4 * (BALL_R - 2) * (BALL_R - 2))
                v = 1;
            else if (d2 < 4 * 15 * 15)
                v = (d2 >= 4 * 13 * 13 || (d2 >= 4 * 7 * 7 && d2 < 4 * 9 * 9)) ? 1 : 0;
            else if (dy > -4 && dy < 4)
                v = 1;
            else if (dy < 0)
                v = 2;
            sBall[y * 2 * BALL_R + x] = v;
        }
}

const uint8_t *CtrBottom_BallMask(void)
{
    return sBall;
}

/* The light green of a section over [x0, x1) x [y0, y1): flat, a frame line
 * one pixel in (and its light line), the Poké Ball in the middle. */
static void DrawSection(int x0, int y0, int x1, int y1)
{
    int w = x1 - x0, h = y1 - y0, bx = (x0 + x1) / 2 - BALL_R, by = (y0 + y1) / 2 - BALL_R;

    FillRect(x0, y0, w, h, sLook.section[SECTION_BASE]);
    FillRect(x0 + 1, y0 + 1, w - 2, 1, sLook.section[SECTION_LINE]);
    FillRect(x0 + 1, y1 - 2, w - 2, 1, sLook.section[SECTION_LINE]);
    FillRect(x0 + 1, y0 + 2, w - 2, 1, sLook.section[SECTION_LIGHT]);
    FillRect(x0 + 1, y1 - 1, w - 2, 1, sLook.section[SECTION_LIGHT]);
    FillRect(x0 + 1, y0 + 1, 1, h - 2, sLook.section[SECTION_LINE]);
    FillRect(x1 - 2, y0 + 1, 1, h - 2, sLook.section[SECTION_LINE]);
    FillRect(x0 + 2, y0 + 1, 1, h - 2, sLook.section[SECTION_LIGHT]);
    for (int y = 0; y < 2 * BALL_R; ++y)
        for (int x = 0; x < 2 * BALL_R; ++x)
        {
            u8 v = sBall[y * 2 * BALL_R + x];

            if (v)
                Put(bx + x, by + y, sLook.section[v == 1 ? SECTION_BALL : SECTION_BALL_TOP]);
        }
}

/* n / 2 rounded half to even, as the mockups' layout rounds. */
static int Half(int n)
{
    int q = n >= 0 ? n / 2 : -((1 - n) / 2);

    if ((n & 1) && (q & 1))
        ++q;
    return q;
}

/* The box of a sprite's visible pixels, [x0, x1) x [y0, y1). */
static void SpriteBox(const u8 *tiles, int wt, int ht, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = wt * 8;
    *y0 = ht * 8;
    *x1 = *y1 = 0;
    for (int y = 0; y < ht * 8; ++y)
        for (int x = 0; x < wt * 8; ++x)
        {
            const u8 *tile = tiles + ((y / 8) * wt + x / 8) * 32;
            int px = x & 7, py = y & 7;

            if (!((tile[py * 4 + px / 2] >> ((px & 1) * 4)) & 15))
                continue;
            if (x < *x0) *x0 = x;
            if (y < *y0) *y0 = y;
            if (x + 1 > *x1) *x1 = x + 1;
            if (y + 1 > *y1) *y1 = y + 1;
        }
}

/* A sprite with its visible pixels centred on the point whose doubled
 * coordinates are (cx2, cy2). */
static void DrawSpriteCentred(const u8 *tiles, int wt, int ht, int cx2, int cy2, const u16 *pal)
{
    int x0, y0, x1, y1;

    SpriteBox(tiles, wt, ht, &x0, &y0, &x1, &y1);
    if (x1 > x0)
        DrawSprite(tiles, wt, ht, Half(cx2 - x0 - x1), Half(cy2 - y0 - y1), pal);
}

