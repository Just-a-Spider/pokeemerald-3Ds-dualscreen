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

enum { CACHE_MENU, CACHE_WIDE, CACHE_MAP, CACHE_CARD, CACHE_COUNT };
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
    if (sClipX0 >= sClipX1 || sClipY0 >= sClipY1)
        return;
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
    if (x0 >= x1 || y0 >= y1)
        return;

    u32 c32 = ((u32)c << 16) | c;
    for (int cx = x0; cx < x1; ++cx)
    {
        u16 *p = sDst + cx * H + (H - y1);
        int n = y1 - y0;
        if ((uintptr_t)p & 2)
        {
            *p++ = c;
            n--;
        }
        while (n >= 2)
        {
            *(u32 *)p = c32;
            p += 2;
            n -= 2;
        }
        if (n > 0)
            *p = c;
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
       OPT_VOXEL_STEREO, OPT_DAYNIGHT,
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

static void LoadResources(void)
{
    static const char *const cardPals[5] = {
        "trainer_card/green.gbapal", "trainer_card/bronze.gbapal", "trainer_card/copper.gbapal",
        "trainer_card/silver.gbapal", "trainer_card/gold.gbapal",
    };
    u32 size;

    PalSymbol(GetOverworldTextboxPalettePtr(), &sRes.text, 1);

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

    for (int i = 0; i < CACHE_COUNT; ++i)
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

/* The column's background: the party menu's olive frame colour. */
static void DrawColumnBackground(void)
{
    for (int ty = 0; ty < H / 8; ++ty)
        for (int tx = COL_X / 8; tx < W / 8; ++tx)
            DrawMapEntry(sRes.partyTiles, sRes.partyTileCount, 0x1002, tx * 8, ty * 8, sRes.partyPal);
}

