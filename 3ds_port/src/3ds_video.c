/* GBA compatibility compositor on PICA200. CPU work only decodes dirty tiles
 * into a texture atlas and submits geometry. All rasterization, transforms,
 * transparency, priority composition and scaling happen on the GPU. */
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "3ds_platform.h"
#include "3ds_video.h"
#include "3ds_data.h"

/* The voxel overworld replaces this compositor's output while the player is
 * walking around; every other game state keeps the 2D path below. */
#ifndef CTR_VOXEL_ENABLED
#define CTR_VOXEL_ENABLED 0
#endif
#if CTR_VOXEL_ENABLED
#include "voxel/ctr_voxel.h"
#endif

/*
 * The game lays itself out over the whole 400x240 viewport, so screen and
 * logical coordinates are the same. A background only covers the part of the
 * screen its tilemap actually spans; see DrawTextBg.
 *
 * A GBA stage (CtrVideo_SetStage) is the exception: its 240x160 picture sits
 * 1:1 at (CTR_STAGE_X, CTR_STAGE_Y) and the rest of the screen is filled around
 * it. sViewX/Y is where the GBA origin lands on the screen; everything else
 * stays in GBA coordinates, so the space around the picture is negative or
 * past 240x160.
 *
 * A centred GBA screen (CtrVideo_SetCentred) sits at the same place, but its
 * margins are not filled from the picture: only the layers that wrap on the
 * GBA (an affine map with wrap-around) and sprites reach into them, and text
 * layers stop at the edge of the 240x160 screen - except the backgrounds a
 * screen names in sCentredFills, which carry on to the edges.
 *
 * The battle scene (CtrVideo_SetBattle) sits centred across and on the bottom
 * edge, so its text box is the bottom of the top screen. The text box is
 * stretched to the whole width, and the scene above and beside it comes from
 * its own layers as the GBA would wrap them (DrawBattleBg).
 */
static int sViewX, sViewY;
/*
 * Magnification of the frame being composed: a screen point is
 * (gba + view) * zoom + offset. The battle scene is composed at
 * CTR_BATTLE_ZOOM and its text box at 1 (DrawBattleTextLayer).
 */
static float sZoom = 1.0f, sOffX, sOffY;
/* Size of the surface being composed: the screen, or the battle scene's. */
static int sTargetW = CTR_GAME_WIDTH, sTargetH = CTR_GAME_HEIGHT;
/* Height of the texture behind it: scissor rows count from its far edge. */
static int sSurfaceH = 256;
/*
 * The battle scene at 1.5 is not composed at 1.5: nearest sampling at a
 * fractional scale makes GBA pixels alternately one and two screen pixels
 * wide, which shimmers on hardware as anything moves. It is composed at 2,
 * every GBA pixel an exact 2x2 block, into a surface of its own, and that
 * surface is drawn at 0.75 with bilinear filtering: every pixel the same
 * size, edges a pixel soft ("sharp bilinear"). The text box is composed
 * after it at 1:1, crisp. See RenderBattleScene.
 */
#define SCENE_ZOOM 2.0f
#define SCENE_W 1024
#define SCENE_H 256
static C3D_Tex sSceneTex;
static C3D_RenderTarget *sScene;
static bool sSceneFailed;
static uint32_t sSceneUsedFrame, sSceneFailFrame;
/* Layers (Layers mask bits) the current composition pass leaves out. */
static unsigned sLayerExclude;
/* Screen pixels per GBA pixel that the stereo displacement is measured in. */
static float sShiftZoom = 1.0f;
#define CTR_VIEW_X sViewX
#define CTR_VIEW_Y sViewY
/* Requested by the game bridge, and what the current frame is composed as. */
static bool sStageRequested, sStage;
static bool sCentred;
static unsigned sCentredRequested, sCentredScreen;
static bool sBattleRequested, sBattle;
/* Visible extent in GBA coordinates. */
#define VIEW_LEFT ((int)floorf(-sOffX / sZoom) - sViewX)
#define VIEW_TOP ((int)floorf(-sOffY / sZoom) - sViewY)
#define VIEW_RIGHT ((int)ceilf((sTargetW - sOffX) / sZoom) - sViewX)
#define VIEW_BOTTOM ((int)ceilf((sTargetH - sOffY) / sZoom) - sViewY)

/*
 * Scanline scroll (CtrVideo_SetLineScroll): the eight BG scroll registers,
 * one value per line, and which of them the frame drives line by line.
 */
#define LINE_MAX 240
static uint16_t sLineScroll[8][LINE_MAX];
static unsigned sLineMask, sLineCount;

#define ATLAS_SIZE 1024
#define CACHE_COUNT 16384
#define HASH_COUNT 32768
#define MAX_DRAWS 16384

typedef struct
{
    uint32_t key, checked, paletteVersion;
    uint8_t bytes[64];
    uint32_t colors[8];
    /* Bumped on every upload (new bytes or new palette), never reused. */
    uint32_t serial;
    bool valid, visible;
} Tile;
static uint32_t sTileSerial;

static uint32_t sOamAnchored[4];
static int16_t sOamAnchor[128];

void CtrVideo_ClearOamAnchors(void)
{
    memset(sOamAnchored, 0, sizeof(sOamAnchored));
}

void CtrVideo_SetOamAnchor(unsigned first, unsigned end, int y)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
    {
        sOamAnchored[i >> 5] |= 1u << (i & 31);
        sOamAnchor[i] = (int16_t)y;
    }
}

/* The OAM y (8 bits) moved by whole turns of 256 to lie nearest its sprite's
 * true y: an entry of a sprite is never 128 lines from where it is anchored. */
static int OamUnwrapY(unsigned index, int y)
{
    int anchor = sOamAnchor[index];

    while (y - anchor > 128) y -= 256;
    while (anchor - y > 128) y += 256;
    return y;
}

#if CTR_VOXEL_ENABLED
static uint32_t sVoxelWeatherOam[4];
static bool sVoxelWeatherOnly;

void CtrVideo_ClearVoxelWeatherOam(void)
{
    memset(sVoxelWeatherOam, 0, sizeof(sVoxelWeatherOam));
}

void CtrVideo_MarkVoxelWeatherOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sVoxelWeatherOam[i >> 5] |= 1u << (i & 31);
}
#else
void CtrVideo_ClearVoxelWeatherOam(void) {}
void CtrVideo_MarkVoxelWeatherOam(unsigned first, unsigned end)
{
    (void)first;
    (void)end;
}
#endif

static CtrVideoMemory sMemory;

const uint8_t *CtrVideo_GetBgVram(void) { return sMemory.vram; }

void CtrVideo_NotifyTilesetAnimWrite(const void *dest, unsigned bytes)
{
#if CTR_VOXEL_ENABLED
    uintptr_t base = (uintptr_t)sMemory.vram;
    uintptr_t address = (uintptr_t)dest;
    if (base == 0 || address < base || address - base >= 0x8000 || bytes == 0)
        return;
    unsigned offset = (unsigned)(address - base);
    if (bytes > 0x8000 - offset) bytes = 0x8000 - offset;
    CtrVoxel_NotifyTilesetAnimWrite(offset / 32, (offset % 32 + bytes + 31) / 32);
#else
    (void)dest;
    (void)bytes;
#endif
}
static CtrVideoStats sStats;
static C3D_Tex sAtlas, sSurface;
static C3D_RenderTarget *sLogical, *sTop, *sTopRight;
static bool sStereo;
static Tile sTiles[CACHE_COUNT];
static uint16_t sHash[HASH_COUNT];
static uint16_t sPalette[512];
static uint16_t sTexturePalette[512];
static uint8_t sMorton[64];
static C2D_ImageTint sTint;
static uint32_t sPaletteVersion[34];
static uint32_t sPaletteChanges[34][8];
static unsigned sUsed;
static bool sC3d, sC2d;
static uint64_t sFpsStart;
static unsigned sFpsFrames;
static uint32_t sReported;

/*
 * Current clip rectangle. With windows enabled the frame is composed as a
 * partition of rectangles and every layer is visited once per rectangle, so
 * geometry outside the current one must not be submitted at all: the scissor
 * would discard it after paying for it, which is what pushed a windowed
 * overworld frame to three times the draw calls it needs.
 */
static int sClipX0, sClipY0, sClipX1 = CTR_GAME_WIDTH, sClipY1 = CTR_GAME_HEIGHT;

static void ClipToView(void)
{
    sClipX0 = VIEW_LEFT;
    sClipY0 = VIEW_TOP;
    sClipX1 = VIEW_RIGHT;
    sClipY1 = VIEW_BOTTOM;
}

/* Per-frame time in the layer walk and in the sprites, reported with fps. */
static uint64_t sBgTicks, sObjTicks;

/*
 * Stereoscopic depth. The GBA already orders everything by priority, 0 nearest
 * and 3 furthest, so that is the depth scale: priority 3 stays at the screen
 * plane and the rest come forward, which puts interface windows in front of
 * the map and the sprites between the two.
 *
 * The frame is composed once per eye with each layer displaced by whole
 * pixels; a fractional displacement would resample the very pixel grid this
 * renderer exists to preserve. At the widest slider setting the nearest layer
 * separates by CTR_STEREO_PIXELS in each eye.
 */
#define CTR_STEREO_PIXELS 1.0f

/* Displacement per depth unit for the eye being composed, and the resulting
 * displacement of the layer being drawn, both in whole screen pixels. */
static float sParallax, sLayerShift;
/* Fixed horizontal placement of every layer, on top of the depth parallax.
 * Zero for the 2D compositor, which reproduces the GBA frame as it is. */
static float sLayerOrigin;

/*
 * The text layer is a 32-column tilemap, so it only addresses the left 256px
 * of the 400px viewport, and widening it does not fit in background VRAM (see
 * the note on sStandardTextBox_WindowTemplates in src/menu.c). That is a
 * limit on where the *game* can put a window, not on where this compositor can
 * draw one: on the field BG0 carries nothing but windows, so it is drawn moved
 * to where it belongs. The standard text box spans x=16..232 within the band,
 * so its centre reaches the middle of the viewport at +76. The voxel overlay
 * does the same (ComposeVoxelOverlay); sFieldUi asks for it in the 2D field.
 */
#define CTR_FIELD_UI_SHIFT 76.0f
static bool sFieldUi;
/* The 2D field this frame: its text backgrounds are drawn from layer
 * textures kept up to date cell by cell (LayerRenderCells). */
static bool sFieldLayers;

/*
 * Composing the whole frame twice costs twice the CPU, which is a frame an Old
 * 3DS does not have. Instead each depth plane is composed once into its own
 * surface and the two eyes are then four textured quads each: the layer walk,
 * which is where the time goes, runs once per frame however many eyes there
 * are.
 *
 * A layer that blends with what is underneath it needs that underneath in the
 * same surface, so the planes are cut only where nothing blends across, and
 * the last one holds every remaining priority. A frame where everything blends
 * ends up as a single plane: no depth, but no extra cost either.
 */
/*
 * Three planes rather than one per priority: each one costs a surface to clear
 * and a textured quad per eye, and the overworld has no frame to spare for a
 * fourth. The two furthest priorities share the last plane, which still leaves
 * the interface, the near layer and the background at three depths.
 */
#define CTR_PRIORITIES 4
#define CTR_BANDS 3
static C3D_Tex sBandTex[CTR_BANDS];
static C3D_RenderTarget *sBand[CTR_BANDS];
static bool sBandsReady, sBandsFailed;
/* A failed allocation is tried again this many frames later, not never: the
 * VRAM it needs may have been in use by the overworld only for a while. */
#define CTR_BANDS_RETRY_FRAMES 300u
static uint32_t sBandsRetryFrame;
/* Whether each plane drew anything; an empty one is not worth a quad. */
static bool sBandUsed[CTR_BANDS];
/* Depth planes the last frame used, 0 when it was composed per eye. */
static unsigned sPlanes;
/* Priorities the current composition pass may draw. */
static unsigned sPriorityMask = 15;

/*
 * Last blend configuration submitted. Reset whenever something else may have
 * changed the GPU state, which is once per eye pass.
 */
static unsigned sBlendKey = ~0u;
static void BlendForget(void) { sBlendKey = ~0u; }

static unsigned Reg(unsigned offset) { return sMemory.regs[offset / 2]; }
static unsigned Min(unsigned a, unsigned b) { return a < b ? a : b; }

static void Error(unsigned bit, const char *reason)
{
    if (!(sReported & (1u << bit)))
    {
        sReported |= 1u << bit;
        ++sStats.errors;
        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: %s", reason);
    }
}

static unsigned Read16(unsigned offset)
{
    if (offset >= 0x10000)
    {
        Error(0, "BG tilemap outside BG VRAM");
        return 0;
    }
    return sMemory.vram[offset] | (sMemory.vram[offset + 1] << 8);
}

static void UpdatePalette(void)
{
    bool bgChanged = false, objChanged = false;
    for (unsigned bank = 0; bank < 32; ++bank)
        if (memcmp(sPalette + bank * 16, sMemory.palette + bank * 16, 32))
        {
            unsigned group = bank < 16 ? 32 : 33;
            if (bank < 16 ? !bgChanged : !objChanged)
                memset(sPaletteChanges[group], 0, sizeof(sPaletteChanges[group]));
            memset(sPaletteChanges[bank], 0, sizeof(sPaletteChanges[bank]));
            for (unsigned index = 0; index < 16; ++index)
            {
                unsigned p = bank * 16 + index;
                if (sPalette[p] == sMemory.palette[p]) continue;
                sPalette[p] = sMemory.palette[p];
                sTexturePalette[p] = CtrVideo_RGBA5551(sPalette[p]);
                sPaletteChanges[bank][0] |= 1u << index;
                unsigned entry = p & 255;
                sPaletteChanges[group][entry / 32] |= 1u << (entry & 31);
            }
            ++sPaletteVersion[bank];
            if (bank < 16) bgChanged = true; else objChanged = true;
        }
    if (bgChanged) ++sPaletteVersion[32];
    if (objChanged) ++sPaletteVersion[33];
}

static bool TilePaletteChanged(const Tile *tile, unsigned paletteId)
{
    uint32_t version = sPaletteVersion[paletteId];
    if (tile->paletteVersion == version) return false;
    /* The last change mask is sufficient only for the next version. Tiles
     * returning after multiple palette changes are conservatively rebuilt. */
    if (tile->paletteVersion + 1 != version) return true;
    for (unsigned i = 0; i < (paletteId < 32 ? 1u : 8u); ++i)
        if (tile->colors[i] & sPaletteChanges[paletteId][i]) return true;
    return false;
}

/*
 * Resolves a tile to its atlas slot, uploading it if its bytes or its palette
 * changed. Returns -1 for a tile that is entirely transparent, which draws
 * nothing. The caller builds the subtexture: a slot is a fixed square of the
 * atlas, so that is four multiplications and no shared state.
 */
static int GetTileSlot(unsigned address, unsigned bank, bool color256)
{
    if (address + (color256 ? 64 : 32) > 0x18000)
    {
        Error(1, "tile outside logical VRAM");
        address = 0;
    }
    unsigned paletteId = color256 ? 32 + (bank >= 16) : bank;
    uint32_t key = (address / 32) * 34 + paletteId + 1;
    unsigned hash = (key * 2654435761u) & (HASH_COUNT - 1);
    while (sHash[hash] && sTiles[sHash[hash] - 1].key != key)
        hash = (hash + 1) & (HASH_COUNT - 1);
    if (!sHash[hash])
    {
        if (sUsed == CACHE_COUNT)
            CtrPlatform_Fatal("VIDEO tile cache exhausted in a single frame");
        sHash[hash] = ++sUsed;
        sTiles[sUsed - 1] = (Tile){.key = key};
    }
    unsigned slot = sHash[hash] - 1;
    Tile *tile = &sTiles[slot];
    if (!tile->valid || tile->checked != sStats.frames + 1)
    {
        unsigned bytes = color256 ? 64 : 32;
        if (!tile->valid || TilePaletteChanged(tile, paletteId)
            || memcmp(tile->bytes, sMemory.vram + address, bytes))
        {
            memcpy(tile->bytes, sMemory.vram + address, bytes);
            unsigned paletteBase = color256 ? (bank >= 16 ? 256 : 0) : bank * 16;
            uint16_t *dest = (uint16_t *)sAtlas.data + slot * 64;
            memset(tile->colors, 0, sizeof(tile->colors));
            tile->visible = false;
            for (unsigned pixel = 0; pixel < 64; ++pixel)
            {
                unsigned index = color256 ? tile->bytes[pixel]
                    : (tile->bytes[pixel / 2] >> ((pixel & 1) * 4)) & 15;
                dest[sMorton[pixel]] = index ? sTexturePalette[paletteBase + index] : 0;
                if (index)
                {
                    tile->visible = true;
                    tile->colors[index / 32] |= 1u << (index & 31);
                }
            }
            /* C3D_FrameEnd(0) flushes linear memory once before submitting
             * the queue, including this atlas and Citro2D's geometry. A
             * separate GSP service call per tile costs hundreds of calls
             * per palette-animation frame without improving visibility. */
            tile->valid = true;
            tile->serial = ++sTileSerial;
            ++sStats.uploads;
        }
        tile->paletteVersion = sPaletteVersion[paletteId];
        tile->checked = sStats.frames + 1;
    }
    return tile->visible ? (int)slot : -1;
}

static void DrawSlotTinted(int slot, float x, float y, bool flipX, bool flipY, const C2D_ImageTint *tint)
{
    unsigned tileX = ((unsigned)slot % (ATLAS_SIZE / 8)) * 8;
    unsigned tileY = ((unsigned)slot / (ATLAS_SIZE / 8)) * 8;
    Tex3DS_SubTexture sub = {8, 8, tileX / (float)ATLAS_SIZE,
        1.0f - tileY / (float)ATLAS_SIZE,
        (tileX + 8) / (float)ATLAS_SIZE, 1.0f - (tileY + 8) / (float)ATLAS_SIZE};

    if (++sStats.tiles >= MAX_DRAWS - 32)
    {
        Error(2, "GPU geometry budget exceeded");
        return;
    }
    if (!C2D_DrawImageAt((C2D_Image){&sAtlas, &sub}, x, y, 0, tint,
                         flipX ? -1 : 1, flipY ? -1 : 1))
        Error(3, "Citro2D geometry submission failed");
}

static void DrawSlot(int slot, float x, float y, bool flipX, bool flipY)
{
    DrawSlotTinted(slot, x, y, flipX, flipY, &sTint);
}

static void Blend(unsigned layer, bool effects, bool semiTransparent)
{
    /*
     * This flushes the batch and rewrites the blend state, so it costs a GPU
     * draw call. The registers it reads are fixed for the frame, and nothing
     * between two calls touches the blend state, so asking for the same
     * configuration again is a no-op: an overworld frame with forty sprites
     * asked for it forty times.
     */
    unsigned key = layer | (effects << 8) | (semiTransparent << 9);

    if (key == sBlendKey) return;
    sBlendKey = key;

    C2D_Flush();
    C3D_AlphaTest(true, GPU_GREATER, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
                  GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    C2D_PlainImageTint(&sTint, C2D_Color32(255, 255, 255, 255), 0);
    if (!effects) return;
    if ((effect == 1 && (control & (1u << layer))) || semiTransparent)
    {
        /* GPU implements independently clamped EVA/EVB (not 1-EVA).
         * Valid for the title's BG1 over BG0/backdrop; arbitrary interleaved
         * target-2 masks are a phase-6 per-pixel effect, reported below. */
        if ((control >> 8) & 63)
        {
            unsigned eva = Min(Reg(0x52) & 31, 16) * 255 / 16;
            unsigned evb = Min((Reg(0x52) >> 8) & 31, 16) * 255 / 16;
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR,
                          GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
            C3D_BlendingColor(C2D_Color32(eva, eva, eva, evb));
        }
    }
    else if ((control & (1u << layer)) && effect >= 2)
    {
        unsigned c = effect == 2 ? 255 : 0;
        C2D_PlainImageTint(&sTint, C2D_Color32(c, c, c, 255), Min(Reg(0x54), 16) / 16.0f);
    }
}

static void DrawTile(unsigned address, unsigned bank, bool color256,
                     float x, float y, bool flipX, bool flipY)
{
    int slot = GetTileSlot(address, bank, color256);
    if (slot >= 0) DrawSlot(slot, x, y, flipX, flipY);
}

/*
 * Walks the tiles of a text background that cover [left, right) x [top, bottom)
 * of the screen. With a mirror axis (given doubled, -1 for none) each tile is
 * drawn reflected about it instead, flipped, so the region it lands on becomes
 * the mirror image of the one walked.
 */
/* Extra horizontal displacement of what DrawTextSpanAt draws, for a band that
 * is moved as a whole (the battle text box, DrawBattleText). */
static int sSpanShiftX, sSpanShiftY;

/* The base transform of everything composed: the current zoom. */
static void ViewBase(void)
{
    C2D_ViewReset();
    if (sZoom != 1.0f)
    {
        C2D_ViewTranslate(sOffX, sOffY);
        C2D_ViewScale(sZoom, sZoom);
    }
}

/* An affine layer's or sprite's own matrix, on top of the zoom. */
static void ViewAffine(C3D_Mtx *matrix)
{
    for (unsigned r = 0; r < 2; ++r)
        matrix->r[r] = FVec4_Scale(matrix->r[r], sZoom);
    matrix->r[0].w += sOffX;
    matrix->r[1].w += sOffY;
    C2D_ViewRestore(matrix);
}

static void DrawTextSpanAt(unsigned bg, int left, int right, int top, int bottom,
                           int mirrorX2, int mirrorY2)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;

    /*
     * A tilemap entry repeats across the layer: the field overlays are mostly
     * one blank entry, and a viewport of 400x240 walks 1500 of them per layer
     * per frame. This memo turns a repeat into one comparison instead of a
     * hash probe and a revalidation, which is what a 60fps overworld needs on
     * an Old 3DS. The stamp makes it valid for this call only, so every entry
     * is still revalidated once per layer per frame.
     */
    static uint32_t sMemoStamp[1024];
    static uint16_t sMemoEntry[1024];
    static int16_t sMemoSlot[1024];
    static uint32_t sStamp;
    uint32_t stamp = ++sStamp;

    ViewBase();
    for (int y = top / 8 - 1; y * 8 <= bottom; ++y)
    {
        int py = y * 8 - (int)(scrollY & 7);
        unsigned row = (unsigned)(y + (int)(scrollY / 8));
        unsigned rowBase;

        if (py + 8 <= top || py >= bottom) continue;
        rowBase = map + CtrVideo_TextMapOffset(0, row, size);
        for (int x = left / 8 - 1; x * 8 <= right; ++x)
        {
            int px = x * 8 - (int)(scrollX & 7);
            unsigned column = (unsigned)(x + (int)(scrollX / 8)) & ((size & 1) ? 63u : 31u);
            unsigned entry, index;
            int slot;

            /* Inside the GBA area the wrap-around is kept exactly as on
             * hardware; only the margins are denied to a narrow background. */
            if (px + 8 <= left || px >= right) continue;
            /* Columns past 31 live in the next screenblock, 1024 entries on. */
            entry = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
            index = entry & 1023;
            if (sMemoStamp[index] == stamp && sMemoEntry[index] == entry)
            {
                slot = sMemoSlot[index];
            }
            else
            {
                unsigned address = chars + index * (color256 ? 64 : 32);
                if (address >= 0x10000) { Error(4, "BG character address exceeds 64 KiB"); continue; }
                slot = GetTileSlot(address, entry >> 12, color256);
                sMemoStamp[index] = stamp;
                sMemoEntry[index] = (uint16_t)entry;
                sMemoSlot[index] = (int16_t)slot;
            }
            if (slot < 0) continue;
            {
                bool flipX = (entry & 1024) != 0, flipY = (entry & 2048) != 0;
                int dx = px, dy = py;

                if (mirrorX2 >= 0) { dx = mirrorX2 - px - 8; flipX = !flipX; }
                if (mirrorY2 >= 0) { dy = mirrorY2 - py - 8; flipY = !flipY; }
                DrawSlot(slot, dx + sSpanShiftX + CTR_VIEW_X + sLayerShift, dy + sSpanShiftY + CTR_VIEW_Y,
                         flipX, flipY);
            }
        }
    }
}

static void DrawTextSpan(unsigned bg, int left, int right, int top, int bottom)
{
    DrawTextSpanAt(bg, left, right, top, bottom, -1, -1);
}

/*
 * The scissor the window partition set for the rectangle being composed, so a
 * pass that narrows it can put it back.
 */
static bool sScissored;

/*
 * The PokéNav on the bottom screen has 240 lines for the GBA's 160, and it
 * spends them as a phone screen would: its header stays on the top edge, its
 * footer (the help bar) goes down to the bottom edge, and the rest - its
 * body - sits in the middle, the background at the back carried on in the
 * space between them. The main menu is a header (the POKéMON NAVIGATOR bar)
 * and a body; the screens under it slide that bar down as their help bar,
 * and their header is the tab of sprites naming the screen at the top.
 *
 * Each band is composed as a frame of its own (NavCompose), moved down by
 * its shift, scissored to the part of the screen it owns: the backgrounds
 * only for its lines of the picture, the backmost one (CentredLayers) over
 * all of that part, and the sprites that belong to it (NavObjectBand) whole.
 */
typedef struct
{
    /* The picture's lines [top, bottom), shown shift lines lower, and the
     * part of the bottom screen [screenTop, screenBottom) the band owns. */
    int top, bottom, shift, screenTop, screenBottom;
} NavBand;

#define NAV_BAR_LINES 32
#define NAV_HELP_TOP 144
#define NAV_BODY_SHIFT ((240 - 160) / 2)

static const NavBand sNavMain[] =
{
    {0, NAV_BAR_LINES, 0, 0, NAV_BAR_LINES + NAV_BODY_SHIFT},
    {NAV_BAR_LINES, 160, NAV_BODY_SHIFT, NAV_BAR_LINES + NAV_BODY_SHIFT, 240},
};
static const NavBand sNavSubmenu[] =
{
    {0, 0, 0, 0, NAV_BODY_SHIFT},
    {0, NAV_HELP_TOP, NAV_BODY_SHIFT, NAV_BODY_SHIFT, 240 - (160 - NAV_HELP_TOP)},
    {NAV_HELP_TOP, 160, 240 - 160, 240 - (160 - NAV_HELP_TOP), 240},
};
/* The bands of the last PokéNav frame, and the one being composed. */
static const NavBand *sNavBands = sNavMain;
static unsigned sNavBandCount = 2;
static const NavBand *sNavBand;
/* The sprite tiles of the header tab, [first, end): pokenav_main_menu.c. */
static unsigned sNavHeaderTiles[2];
/* The scissor's lines while a band is composed, on screen. */
static int sNavClip0, sNavClip1;
unsigned char CtrPokenav_HeaderTiles(unsigned short *first, unsigned short *end);

static void Scissor(int x0, int y0, int x1, int y1)
{
    float sx0 = (x0 + CTR_VIEW_X) * sZoom + sOffX, sx1 = (x1 + CTR_VIEW_X) * sZoom + sOffX;
    float sy0 = (y0 + CTR_VIEW_Y) * sZoom + sOffY, sy1 = (y1 + CTR_VIEW_Y) * sZoom + sOffY;

    if (sNavBand)
    {
        if (sy0 < sNavClip0) sy0 = (float)sNavClip0;
        if (sy1 > sNavClip1) sy1 = (float)sNavClip1;
        if (sy1 < sy0) sy1 = sy0;
    }
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)roundf(sx0), (unsigned)(sSurfaceH - roundf(sy1)),
                   (unsigned)roundf(sx1), (unsigned)(sSurfaceH - roundf(sy0)));
}

static void RestoreScissor(void)
{
    if (sScissored) Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
    else C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

/*
 * How a stage fills the screen around its 240x160 picture. A GBA background
 * is one picture drawn for 240x160, so outside that area there is nothing the
 * game drew for it: the port fills it from the picture itself.
 *
 * - Across a background that scrolls sideways - the scenery of the bike ride,
 *   drifting clouds - the tilemap is continuous art made to wrap around, so
 *   the margins show the wrap-around, exactly as the GBA brings those columns
 *   onto its own screen a moment later.
 * - A still picture - the leaves of the Game Freak logo, the legendaries, the
 *   title screen - is carried outwards from its own edge tiles and faded into
 *   black (DrawEdgeRegion), like the iris of a film frame. Mirroring was
 *   tried and rejected: it shows every character on an edge twice.
 *
 * Whether a background scrolls is decided from the last STILL_FRAMES frames
 * and held for SCROLL_HOLD_FRAMES after it stops, so a scene does not switch
 * filling halfway through. A new control register starts the judgement over.
 */
#define STILL_FRAMES 32
#define STILL_SPAN 4
static uint16_t sScrollHistory[4][STILL_FRAMES];
static uint32_t sSceneKey[4];
static bool sScrolls[4];
/* The last frame each layer was seen moving. */
static uint32_t sMovedFrame[4];
/*
 * How long a layer that moved keeps being treated as scrolling: long enough
 * that a scene pausing its scenery for a moment does not switch filling, short
 * enough that the next scene on the same registers starts from what it does.
 */
#define SCROLL_HOLD_FRAMES 120
/* When each still background was last measured (MeasureStill). */
static uint32_t sPictureFrame[4];

static void RecordScroll(void)
{
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        uint32_t key = Reg(8 + bg * 2) | ((Reg(0) & 7) << 16) | ((Reg(0) >> (8 + bg) & 1) << 20);
        /* A horizontal scroll driven line by line is a wave, not a pan: the
         * register only holds the first line of it. */
        unsigned scroll = (sLineMask & (1u << (bg * 2))) ? 0 : Reg(0x10 + bg * 4) & 511;
        int low = 0, high = 0;

        if (key != sSceneKey[bg])
        {
            sSceneKey[bg] = key;
            sScrolls[bg] = false;
            sPictureFrame[bg] = 0;
            for (unsigned i = 0; i < STILL_FRAMES; ++i) sScrollHistory[bg][i] = scroll;
        }
        sScrollHistory[bg][sStats.frames % STILL_FRAMES] = scroll;
        for (unsigned i = 0; i < STILL_FRAMES; ++i)
        {
            /* Unwrapped against the current value, so 511 -> 0 is one step. */
            int delta = (((int)sScrollHistory[bg][i] - (int)scroll + 256) & 511) - 256;

            if (delta < low) low = delta;
            if (delta > high) high = delta;
        }
        /* A shake of a few pixels is not scrolling; a pan is. */
        if (high - low > STILL_SPAN)
        {
            sScrolls[bg] = true;
            sMovedFrame[bg] = sStats.frames;
        }
        else if (sScrolls[bg] && sStats.frames - sMovedFrame[bg] > SCROLL_HOLD_FRAMES)
        {
            sScrolls[bg] = false;
        }
    }
}

/* Clamps a cell to the clip rectangle; false when nothing of it is left. */
static bool ClipCell(int *x0, int *x1, int *y0, int *y1)
{
    if (*x0 < sClipX0) *x0 = sClipX0;
    if (*x1 > sClipX1) *x1 = sClipX1;
    if (*y0 < sClipY0) *y0 = sClipY0;
    if (*y1 > sClipY1) *y1 = sClipY1;
    return *x0 < *x1 && *y0 < *y1;
}

/*
 * The rows a background's picture really covers. The intro's cinematic scenes
 * are drawn only between their letterbox bars - the clouds and Rayquaza have
 * nothing above line 32 or below 128 - so when window 0 is a full-width band
 * that hides this layer outside it, the band is the picture, and the space
 * the stage opens above and below it is filled from the band's own edges.
 */
static void PictureRows(unsigned bg, int *top, int *bottom)
{
    unsigned display = Reg(0), across = Reg(0x40), rows = Reg(0x44);

    *top = 0;
    *bottom = 160;
    if (!(display & 0x2000) || (Reg(0x4a) & (1u << bg)) || !(Reg(0x48) & (1u << bg))) return;
    if ((across >> 8) != 0 || ((across & 255) < 240 && (across & 255) != 0)) return;
    if ((rows >> 8) >= (rows & 255) || (rows & 255) > 160) return;
    *top = (int)(rows >> 8);
    *bottom = (int)(rows & 255);
}

/*
 * One cell of the 3x3 partition of a stage around its picture: the part of
 * the current clip rectangle inside [x0, x1) x [y0, y1), drawn from the source
 * the cell calls for. A mirror axis is given doubled; -1 means drawn directly.
 * A mirrored cell only ever takes from the picture [0, 240) x [top, bottom).
 */
static void DrawStageCell(unsigned bg, int x0, int x1, int y0, int y1,
                          int mirrorX2, int mirrorY2, int top, int bottom)
{
    int sx0, sx1, sy0, sy1;

    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    sx0 = mirrorX2 < 0 ? x0 : mirrorX2 - x1;
    sx1 = mirrorX2 < 0 ? x1 : mirrorX2 - x0;
    sy0 = mirrorY2 < 0 ? y0 : mirrorY2 - y1;
    sy1 = mirrorY2 < 0 ? y1 : mirrorY2 - y0;
    if (mirrorX2 >= 0) { if (sx0 < 0) sx0 = 0; if (sx1 > 240) sx1 = 240; }
    if (mirrorY2 >= 0) { if (sy0 < top) sy0 = top; if (sy1 > bottom) sy1 = bottom; }
    if (sx0 >= sx1 || sy0 >= sy1) return;
    /* Whole tiles overhang a cell; the scissor keeps each cell to itself. */
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    DrawTextSpanAt(bg, sx0, sx1, sy0, sy1, mirrorX2, mirrorY2);
    C2D_Flush();
    RestoreScissor();
}

/*
 * Whether a still layer is a panorama: art in the columns the GBA screen never
 * reaches (30 and 31 of a 32-column map), as dense as just inside the edge,
 * means a picture made to wrap - the sky of the bike ride, which holds still
 * while the scenery in front of it scrolls. Measured every few frames, which
 * is plenty for art that does not move.
 */
static bool sPanorama[4];

static void MeasureStill(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned beyond = 0, inside = 0;

    if (sPictureFrame[bg] && sStats.frames - sPictureFrame[bg] < 8) return;
    sPictureFrame[bg] = sStats.frames | 1;
    for (unsigned y = 0; y < 20 && !(size & 1); ++y)
    {
        unsigned rowBase = map + CtrVideo_TextMapOffset(0, y + scrollY / 8, size);

        for (unsigned x = 28; x < 32; ++x)
        {
            unsigned entry = Read16(rowBase + ((x + scrollX / 8) & 31) * 2);
            unsigned address = chars + (entry & 1023) * (color256 ? 64 : 32);

            if (address < 0x10000 && GetTileSlot(address, entry >> 12, color256) >= 0)
                ++*(x < 30 ? &inside : &beyond);
        }
    }
    sPanorama[bg] = beyond && beyond * 4 >= inside * 3;
}

/*
 * The margins of a still scene: the picture's own edge carried outwards and
 * faded into black, like the iris of a film frame. Each margin tile repeats
 * the tile at the edge of the picture in its row (or column, or corner), and
 * its four corners are tinted towards black by their distance from the
 * picture, so the fade is a smooth gradient, not steps of eight pixels.
 * Nothing is mirrored or duplicated, so a character on the edge of the art is
 * never seen twice.
 */
#define FADE_ACROSS 56.0f
#define FADE_DOWN 28.0f

static float EdgeFade(int x, int y, int top, int bottom)
{
    float across = x < 0 ? -x / FADE_ACROSS : x > 240 ? (x - 240) / FADE_ACROSS : 0.0f;
    float down = y < top ? (top - y) / FADE_DOWN : y > bottom ? (y - bottom) / FADE_DOWN : 0.0f;
    float fade = across > down ? across : down;

    return fade > 1.0f ? 1.0f : fade;
}

/*
 * The zones past the fade are black whatever the still layers hold there, so
 * those layers skip them and the frame starts with them black instead: a
 * rectangle each rather than a column of fully faded tiles per layer. Layers
 * that wrap and sprites still draw over them.
 */
static bool sUnderlaid;

static void StageUnderlay(void)
{
    unsigned display = Reg(0), mode = display & 7;
    bool still = false;
    u32 black = C2D_Color32(0, 0, 0, 255);

    sUnderlaid = false;
    /* Under windows the margins may be the backdrop on purpose (the
     * letterbox, which flashes with it), so nothing is assumed there. */
    if (!sStage || (display & 128) || (display & 0x6000)) return;
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && !sScrolls[bg] && mode != 2 && !(mode == 1 && bg == 2))
            still = true;
    if (!still) return;
    ViewBase();
    C2D_DrawRectSolid(0, 0, 0, CTR_VIEW_X - FADE_ACROSS, CTR_GAME_HEIGHT, black);
    C2D_DrawRectSolid(CTR_VIEW_X + 240 + FADE_ACROSS, 0, 0, CTR_VIEW_X - FADE_ACROSS, CTR_GAME_HEIGHT, black);
    C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, CTR_VIEW_Y - FADE_DOWN, black);
    C2D_DrawRectSolid(0, CTR_VIEW_Y + 160 + FADE_DOWN, 0, CTR_GAME_WIDTH, CTR_VIEW_Y - FADE_DOWN, black);
    sUnderlaid = true;
}

/*
 * The GBA brightness effect a layer is under (BLDCNT effect 2 or 3), as the
 * colour it moves towards and how far; 0 when there is none.
 */
static float LayerBrightness(unsigned layer, bool *white)
{
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;

    *white = effect == 2;
    if (!(control & (1u << layer)) || effect < 2) return 0.0f;
    return Min(Reg(0x54) & 31, 16) / 16.0f;
}

/*
 * One corner of a fading tile. The fade to black f and the layer's own
 * brightness effect k are one tint: darkening gives c(1-k)(1-f), which is a
 * blend of 1-(1-k)(1-f) towards black; brightening gives
 * (c(1-k) + k)(1-f), a blend of the same amount towards a grey of
 * k(1-f) / blend. Either way a fully faded corner is black.
 */
static void FadeCorner(C2D_ImageTint *tint, C2D_Corner corner, float f, float k, bool white)
{
    float blend = 1.0f - (1.0f - k) * (1.0f - f);
    float grey = white && blend > 0.0f ? k * (1.0f - f) / blend : 0.0f;
    unsigned level = (unsigned)(grey * 255.0f + 0.5f);

    C2D_SetImageTint(tint, corner, C2D_Color32(level, level, level, 255), blend);
}

/*
 * Above and below a still picture each margin column repeats the tile on the
 * picture's edge row - unless that row is mostly one tile, a plain band with
 * something drawn over part of it: the title screen's bottom row is its
 * backdrop with the tail and coils of Rayquaza across the middle, and those
 * repeated downwards are streaks. Such a row carries on as its plain tile,
 * and the objects stay in the picture. The entry that fills at least 60% of
 * the row, or ~0u when none does.
 */
static unsigned EdgeRowPlain(unsigned bg, int y)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rowBase = map + CtrVideo_TextMapOffset(0, (unsigned)(y + (int)scrollY) >> 3, size);
    unsigned entries[31], best = ~0u, bestCount = 0;

    if (scrollX & 7) return ~0u;
    for (unsigned c = 0; c < 30; ++c)
    {
        unsigned column = ((scrollX >> 3) + c) & ((size & 1) ? 63u : 31u);
        entries[c] = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
    }
    for (unsigned c = 0; c < 30; ++c)
    {
        unsigned count = 0;

        for (unsigned k = 0; k < 30; ++k) count += entries[k] == entries[c];
        if (count > bestCount) { bestCount = count; best = entries[c]; }
    }
    return bestCount * 10 >= 30 * 6 ? best : ~0u;
}

static void DrawEdgeRegion(unsigned bg, int x0, int x1, int y0, int y1, int top, int bottom)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    /* The grid positions of the tiles on the picture's edges. */
    int firstX = -ox, lastX = ((239 + ox) & ~7) - ox;
    int firstY = ((top + oy) & ~7) - oy, lastY = ((bottom - 1 + oy) & ~7) - oy;
    bool white;
    float bright = LayerBrightness(bg, &white);
    /* A margin row repeats one edge tile, so the last lookup usually answers. */
    unsigned lastEntry = ~0u;
    int lastSlot = -1;
    unsigned plainTop = EdgeRowPlain(bg, top), plainBottom = EdgeRowPlain(bg, bottom - 1);

    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    ViewBase();
    for (int py = ((y0 + oy) & ~7) - oy - 8; py < y1; py += 8)
    {
        int sy = py < firstY ? firstY : py > lastY ? lastY : py;
        unsigned rowBase = map + CtrVideo_TextMapOffset(0, (unsigned)(sy + (int)scrollY) >> 3, size);

        if (py + 8 <= y0) continue;
        for (int px = ((x0 + ox) & ~7) - ox - 8; px < x1; px += 8)
        {
            int sx = px < firstX ? firstX : px > lastX ? lastX : px;
            unsigned column = ((unsigned)(sx + (int)scrollX) >> 3) & ((size & 1) ? 63u : 31u);
            unsigned entry = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
            unsigned address;
            int slot;

            if (py < firstY && plainTop != ~0u) entry = plainTop;
            else if (py > lastY && plainBottom != ~0u) entry = plainBottom;
            address = chars + (entry & 1023) * (color256 ? 64 : 32);
            if (px + 8 <= x0 || address >= 0x10000) continue;
            if (entry != lastEntry)
            {
                lastEntry = entry;
                lastSlot = GetTileSlot(address, entry >> 12, color256);
            }
            slot = lastSlot;
            if (slot < 0) continue;
            /* Past the fade the tile would be black: StageUnderlay drew that. */
            if (sUnderlaid && EdgeFade(px, py, top, bottom) >= 1.0f && EdgeFade(px + 8, py, top, bottom) >= 1.0f
                && EdgeFade(px, py + 8, top, bottom) >= 1.0f && EdgeFade(px + 8, py + 8, top, bottom) >= 1.0f)
                continue;
            {
                C2D_ImageTint fade;

                FadeCorner(&fade, C2D_TopLeft, EdgeFade(px, py, top, bottom), bright, white);
                FadeCorner(&fade, C2D_TopRight, EdgeFade(px + 8, py, top, bottom), bright, white);
                FadeCorner(&fade, C2D_BotLeft, EdgeFade(px, py + 8, top, bottom), bright, white);
                FadeCorner(&fade, C2D_BotRight, EdgeFade(px + 8, py + 8, top, bottom), bright, white);
                DrawSlotTinted(slot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y,
                               entry & 1024, entry & 2048, &fade);
            }
        }
    }
    C2D_Flush();
    RestoreScissor();
}

static bool DrawLeavesMargins(unsigned bg);
static bool LayerDrawable(unsigned bg);

static void DrawStageBg(unsigned bg)
{
    int top, bottom;
    bool wraps = sScrolls[bg];

    PictureRows(bg, &top, &bottom);
    if (!wraps)
    {
        MeasureStill(bg);
        /* A still panorama wraps too when the scene around it scrolls. */
        if (sPanorama[bg])
            for (unsigned other = 0; other < 4; ++other)
                if (other != bg && sScrolls[other] && (Reg(0) & (0x100u << other))) wraps = true;
    }
    if (wraps)
    {
        /*
         * A layer that scrolls - the bike ride - is continuous art made to
         * wrap around: across, one span wrapping as on the GBA; above and
         * below, the mirror image of the picture's own edge rows.
         */
        const int rows[3][3] = {{-512, top, 2 * top}, {top, bottom, -1}, {bottom, 512, 2 * bottom}};

        for (unsigned r = 0; r < 3; ++r)
            DrawStageCell(bg, VIEW_LEFT, VIEW_RIGHT, rows[r][0], rows[r][1], -1, rows[r][2],
                          top, bottom);
        return;
    }
    DrawStageCell(bg, 0, 240, top, bottom, -1, -1, top, bottom);
    if (DrawLeavesMargins(bg)) return;
    DrawEdgeRegion(bg, -512, 512, -512, top, top, bottom);
    DrawEdgeRegion(bg, -512, 512, bottom, 512, top, bottom);
    DrawEdgeRegion(bg, -512, 0, top, bottom, top, bottom);
    DrawEdgeRegion(bg, 240, 512, top, bottom, top, bottom);
}

/*
 * The battle scene. Its picture is 240x160 like any GBA screen, but it is
 * made of a scene that the GBA itself carries past the screen - the terrain is
 * a 512-pixel map whose sky continues on both sides, the entry grass and the
 * move backgrounds are tiles that repeat - and a text box that is one frame
 * of caps and a middle. So nothing is faded or mirrored here:
 *
 * - The text box (the last six tile rows of BG0) is moved to the left edge of
 *   the screen and its middle repeated up to a right cap on the right edge.
 *   Everything in it - the message, the prompt arrow, a menu page - keeps its
 *   place from the left cap, as it does on the GBA.
 * - A layer that wraps on its own - a 64-column map, or a 32-column one whose
 *   hidden columns hold art as dense as the visible ones (MeasureStill) - is
 *   drawn as the GBA wraps it across the sides. Above the picture, each tile
 *   row is taken where the map wraps it; where that row is empty, from the
 *   same row of a 32-row picture repeated in a 64-row map; and on the terrain,
 *   from the picture's own top row, which is what its map repeats above it
 *   (rows 30 and 31 of every battle terrain are its row 0).
 * - Anything else - windows, a battler copied into a background - stays in
 *   the 240x160 picture, where the game placed it.
 */
#define BATTLE_BAND_TOP 112
#define BATTLE_BAND_CAP 16

static unsigned MapEntry(unsigned map, unsigned size, unsigned column, unsigned row)
{
    column &= (size & 1) ? 63u : 31u;
    return Read16(map + CtrVideo_TextMapOffset(0, row, size) + (column & 31) * 2 + ((column >> 5) & 1) * 2048);
}

static int EntrySlot(unsigned chars, unsigned entry, bool color256)
{
    unsigned address = chars + (entry & 1023) * (color256 ? 64 : 32);

    if (address >= 0x10000) return -1;
    return GetTileSlot(address, entry >> 12, color256);
}

/* Part of the picture's own tiles drawn moved by shift, kept to [x0, x1) x [y0, y1). */
static void DrawBattleCut(unsigned bg, int x0, int x1, int y0, int y1, int shift)
{
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    sSpanShiftX = shift;
    DrawTextSpanAt(bg, x0 - shift, x1 - shift, y0, y1, -1, -1);
    sSpanShiftX = 0;
    C2D_Flush();
    RestoreScissor();
}

static void DrawBattleText(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rows = (size & 2) ? 64 : 32;
    /* The column under the left edge of the picture: the left cap. */
    unsigned first = scrollX >> 3;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    int fill0 = 240 - BATTLE_BAND_CAP + VIEW_LEFT, fill1 = VIEW_RIGHT - BATTLE_BAND_CAP;
    int x0 = fill0, x1 = fill1, y0 = BATTLE_BAND_TOP, y1 = 160;

    /* Above the text box: windows, where the game put them. */
    DrawBattleCut(bg, 0, 240, 0, BATTLE_BAND_TOP, 0);
    /* The box from its left cap, on the left edge of the screen... */
    DrawBattleCut(bg, VIEW_LEFT, fill0, BATTLE_BAND_TOP, 160, VIEW_LEFT);
    /* ...its right cap on the right edge... */
    DrawBattleCut(bg, fill1, VIEW_RIGHT, BATTLE_BAND_TOP, 160, VIEW_RIGHT - 240);
    /*
     * ...and its middle in between. A row is a frame of caps and a middle:
     * the five-tile rows of the message box have two cap tiles and then the
     * middle (numbered one after the cap), the three-tile rows of the menu
     * boxes one cap and then the middle itself. Column 2 tells which, since
     * the message window starts there and never holds the middle tile.
     */
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    C2D_Flush();
    Scissor(x0, y0, x1, y1);
    ViewBase();
    for (int py = ((y0 + oy) & ~7) - oy; py < y1; py += 8)
    {
        unsigned row = ((unsigned)(py + (int)scrollY) >> 3) & (rows - 1);
        unsigned cap = MapEntry(map, size, first, row), inner = MapEntry(map, size, first + 1, row);
        unsigned middle, next = MapEntry(map, size, first + 2, row);
        int slot;

        if (!cap && !inner) continue;
        middle = next == inner ? inner : (inner & ~1023u) | ((inner + 1) & 1023);
        slot = EntrySlot(chars, middle, color256);
        if (slot < 0) continue;
        for (int px = ((x0 + ox) & ~7) - ox; px < x1; px += 8)
            DrawSlot(slot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y, middle & 1024, middle & 2048);
    }
    C2D_Flush();
    RestoreScissor();
}

/*
 * The text box is not magnified: it keeps its own pixels on the bottom 48
 * lines, under a scene composed at CTR_BATTLE_ZOOM. The rectangle being
 * composed is carried from the scene's coordinates to the box's through the
 * screen, drawn there, and put back.
 */
static void DrawBattleTextLayer(unsigned bg)
{
    int viewX = sViewX, viewY = sViewY;
    float zoom = sZoom, offX = sOffX, offY = sOffY;
    int clipX0 = sClipX0, clipY0 = sClipY0, clipX1 = sClipX1, clipY1 = sClipY1;

    float shift = sLayerShift;

    sZoom = 1.0f;
    sOffX = sOffY = 0.0f;
    sLayerShift = shift * sShiftZoom;
    sViewX = CTR_BATTLE_X;
    sViewY = CTR_BATTLE_Y;
    sClipX0 = (int)roundf((clipX0 + viewX) * zoom + offX) - sViewX;
    sClipX1 = (int)roundf((clipX1 + viewX) * zoom + offX) - sViewX;
    sClipY0 = (int)roundf((clipY0 + viewY) * zoom + offY) - sViewY;
    sClipY1 = (int)roundf((clipY1 + viewY) * zoom + offY) - sViewY;
    DrawBattleText(bg);
    C2D_Flush();
    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sLayerShift = shift;
    sViewX = viewX;
    sViewY = viewY;
    sClipX0 = clipX0;
    sClipY0 = clipY0;
    sClipX1 = clipX1;
    sClipY1 = clipY1;
    RestoreScissor();
    ViewBase();
}

/* Whether a tile row of a background has anything to show across the screen. */
static bool BattleRowShown(unsigned map, unsigned size, unsigned chars, bool color256,
                           unsigned row, unsigned column, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (EntrySlot(chars, MapEntry(map, size, column + i, row), color256) >= 0)
            return true;
    return false;
}

static void DrawBattleBg(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);
    int x0 = sClipX0, x1 = sClipX1, y0 = sClipY0, y1 = sClipY1;
    unsigned firstColumn, count;
    unsigned lastEntry = ~0u;
    int lastSlot = -1;
    bool wraps = columns == 64;

    if (!wraps)
    {
        MeasureStill(bg);
        wraps = sPanorama[bg];
    }
    if (!wraps)
    {
        DrawBattleCut(bg, 0, 240, 0, 160, 0);
        return;
    }
    if (!ClipCell(&x0, &x1, &y0, &y1)) return;
    firstColumn = (unsigned)(((x0 + ox) & ~7) - ox + (int)scrollX) >> 3;
    count = (unsigned)(x1 - (((x0 + ox) & ~7) - ox) + 7) / 8 + 1;
    /*
     * Above the picture a layer only carries on if its art reaches the top
     * edge. The entry grass of the intro is a band that slides down out of
     * the picture: its map wraps it round to the top, which the GBA never
     * shows and which here repeated the grass along the top of the screen.
     */
    bool above = bg == 3 || BattleRowShown(map, size, chars, color256,
                                           (scrollY >> 3) & (rows - 1), firstColumn, count);

    ViewBase();
    for (int py = ((y0 + oy) & ~7) - oy; py < y1; py += 8)
    {
        int row = (int)(((unsigned)(py + (int)scrollY) >> 3) & (rows - 1));

        if (py + 8 <= 0 && !above) continue;
        /* A row wholly above the picture: where its art comes from. */
        if (py + 8 <= 0 && !BattleRowShown(map, size, chars, color256, row, firstColumn, count))
        {
            if (rows == 64 && BattleRowShown(map, size, chars, color256, row ^ 32, firstColumn, count))
                row ^= 32;
            else if (bg == 3)
                row = (int)((scrollY >> 3) & (rows - 1));
            else
                continue;
        }
        for (int px = ((x0 + ox) & ~7) - ox; px < x1; px += 8)
        {
            unsigned column = ((unsigned)(px + (int)scrollX) >> 3) & (columns - 1);
            unsigned entry = MapEntry(map, size, column, (unsigned)row);

            if (entry != lastEntry)
            {
                lastEntry = entry;
                lastSlot = EntrySlot(chars, entry, color256);
            }
            if (lastSlot >= 0)
                DrawSlot(lastSlot, px + CTR_VIEW_X + sLayerShift, py + CTR_VIEW_Y, entry & 1024, entry & 2048);
        }
    }
}

/*
 * How a centred screen fills the top screen around its 240x160 picture.
 *
 * - layers: the backgrounds carried out to the edges, each margin tile
 *   repeating the tilemap's edge tile in its row, column or corner - moved
 *   inwards by inset (left, top, right, bottom, in tiles) past a border, so
 *   what repeats is the pattern inside it. The edge is the tilemap's, not the
 *   screen's: when the speech slides its background (the platform moving
 *   aside for the player), the margins keep showing the empty side of it
 *   rather than the platform that slid to the edge. across keeps a fill to
 *   the sides. Those screens' backgrounds are a
 *   plain field or a pattern of one tile, so it carries on as if the screen
 *   were wider: the clock's teal, the naming screen's stripes and title bar,
 *   the sky of the professor's speech.
 * - skyRows: the speech's sky, the first rows of its background, drawn from
 *   the top of the screen rather than the top of the picture, so it starts at
 *   the top-left corner and carries on to the right as the rest does.
 * - band: the text box of the professor's speech (BG0 from bandRow down)
 *   drawn moved by (bandX, bandY), to where the overworld shows its text box
 *   (menu.c, sStandardTextBox_WindowTemplates, and CTR_FIELD_UI_SHIFT). It
 *   shares its callback and its BG0 with the title menu, whose windows go
 *   down to the bottom of the screen; the speech is told apart by the
 *   character base its BG0 uses (main_menu.c, sBirchBgTemplate).
 */
typedef struct
{
    uint8_t layers;
    int8_t inset[4];
    /* Only beside the picture, nothing above or below it. */
    bool across;
    /* Instead of layers: the visible background drawn behind the others. */
    bool backmost;
    uint8_t skyRows, bandRow, bandChars;
    int8_t bandX, bandY;
} CentredFill;

static const CentredFill sCentredFills[CTR_CENTRED_SCREENS] =
{
    [CTR_CENTRED_MAIN_MENU] = {.layers = 1u << 1, .across = true, .skyRows = 4, .bandRow = 14, .bandChars = 3,
                               .bandX = -4, .bandY = 32},
    /* Inside the yellow border; the title bar is its top row. */
    [CTR_CENTRED_NAMING] = {.layers = 1u << 3, .inset = {2, 0, 2, 1}},
    [CTR_CENTRED_CLOCK] = {.layers = 1u << 3},
    /* Whatever background is at the back of the PokéNav screen on show
     * (CentredLayers): the dots, the Hoenn sea, the ribbons' wood. */
    [CTR_CENTRED_POKENAV] = {.backmost = true},
};

/* The backgrounds a centred screen carries out to its edges. */
static unsigned CentredLayers(const CentredFill *fill)
{
    unsigned display = Reg(0), best = 4, priority = 0;

    if (!fill->backmost) return fill->layers;
    if ((display & 7) > 2) return 0;
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && (Reg(8 + bg * 2) & 3) >= priority
         && !((display & 7) == 1 && bg == 3) && !((display & 7) == 2 && bg < 2))
        {
            priority = Reg(8 + bg * 2) & 3;
            best = bg;
        }
    return best < 4 ? 1u << best : 0;
}

static void DrawCentredSpan(unsigned bg, int left, int right, int top, int bottom)
{
    if (left < sClipX0 - sSpanShiftX) left = sClipX0 - sSpanShiftX;
    if (right > sClipX1 - sSpanShiftX) right = sClipX1 - sSpanShiftX;
    if (top < sClipY0 - sSpanShiftY) top = sClipY0 - sSpanShiftY;
    if (bottom > sClipY1 - sSpanShiftY) bottom = sClipY1 - sSpanShiftY;
    if (left < right && top < bottom) DrawTextSpan(bg, left, right, top, bottom);
}

static int FloorDiv8(int value)
{
    return value >= 0 ? value / 8 : -((7 - value) / 8);
}

/* The margins of the picture's rows from, up to to (beside it: across). */
static void DrawCentredMargins(unsigned bg, const CentredFill *fill, int from, int to)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned mask = (size & 1) ? 63u : 31u;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);

    ViewBase();
    if (from < FloorDiv8(sClipY0 - sSpanShiftY + oy)) from = FloorDiv8(sClipY0 - sSpanShiftY + oy);
    for (int w = from; w < to && w * 8 - oy + sSpanShiftY < sClipY1; ++w)
    {
        bool beside = w >= 0 && w < 20;
        unsigned row = w < 0 ? (unsigned)fill->inset[1] : w >= 20 ? 19u - (unsigned)fill->inset[3]
                     : (unsigned)(w + (int)(scrollY >> 3));
        unsigned rowBase;

        if (!beside && fill->across) continue;
        rowBase = map + CtrVideo_TextMapOffset(0, row, size);
        for (int v = FloorDiv8(sClipX0 + ox); v * 8 - ox < sClipX1; ++v)
        {
            bool inside = v >= 0 && v < 30;
            unsigned column, entry, address;
            int slot;

            if (inside && beside) continue;
            column = v < 0 ? (unsigned)fill->inset[0] : v >= 30 ? 29u - (unsigned)fill->inset[2]
                   : ((unsigned)v + (scrollX >> 3)) & mask;
            entry = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
            address = chars + (entry & 1023) * (color256 ? 64 : 32);
            if (address >= 0x10000) continue;
            slot = GetTileSlot(address, entry >> 12, color256);
            if (slot >= 0)
                DrawSlot(slot, v * 8 - ox + CTR_VIEW_X + sLayerShift, w * 8 - oy + sSpanShiftY + CTR_VIEW_Y,
                         entry & 1024, entry & 2048);
        }
    }
}

/*
 * The backmost layer in a band of the PokéNav: the band's lines of the
 * picture, and over the rest of the band's part of the screen the picture's
 * bottom row, the one the GBA screen ends on below its help bar - the dots,
 * the sea, the wood. With texture it is cut from the layer's texture.
 */
static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,
                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom);

static void DrawNavBackmost(unsigned bg, bool texture)
{
    const NavBand *band = sNavBand;

    if (band->top < band->bottom)
    {
        if (texture) DrawLayerRect(bg, 0, 240, band->top, band->bottom, 0, (float)band->top, 1, 1, false, 0, 0);
        else DrawCentredSpan(bg, 0, 240, band->top, band->bottom);
    }
    for (int y = band->screenTop - band->shift; y < band->screenBottom - band->shift; y += 8)
    {
        if (y + 8 > band->top && y < band->bottom) continue;
        if (texture)
            DrawLayerRect(bg, 0, 240, y, y + 8, 0, 152, 1, 1, false, 0, 0);
        else
        {
            sSpanShiftY = y - 152;
            DrawCentredSpan(bg, 0, 240, 152, 160);
            sSpanShiftY = 0;
        }
    }
}

/* True when the centred screen draws this layer itself. */
static bool DrawCentredBg(unsigned bg)
{
    const CentredFill *fill = &sCentredFills[sCentredScreen];

    bool speech = fill->bandRow && ((Reg(8) >> 2) & 3) == fill->bandChars;

    if (sNavBand && (CentredLayers(fill) & (1u << bg)))
    {
        DrawNavBackmost(bg, false);
        return true;
    }
    if (CentredLayers(fill) & (1u << bg))
    {
        int sky = speech ? fill->skyRows : 0;

        if (sky)
        {
            sSpanShiftY = (int)(Reg(0x12 + bg * 4) & 511) - CTR_VIEW_Y;
            DrawCentredSpan(bg, 0, 240, 0, sky * 8);
            DrawCentredMargins(bg, fill, 0, sky);
            sSpanShiftY = 0;
        }
        DrawCentredSpan(bg, 0, 240, sky * 8, 160);
        DrawCentredMargins(bg, fill, sky ? sky : -64, 64);
        return true;
    }
    if (bg == 0 && speech)
    {
        DrawCentredSpan(0, 0, 240, 0, fill->bandRow * 8);
        sSpanShiftX = fill->bandX;
        sSpanShiftY = fill->bandY;
        DrawCentredSpan(0, 0, 240, fill->bandRow * 8, 160);
        sSpanShiftX = sSpanShiftY = 0;
        return true;
    }
    return false;
}

static void DrawTextBg(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    /*
     * A background covers as many pixels as its tilemap spans: 256 for the
     * 32-column size the fixed screens use, 512 for the 64-column one the port
     * widened for the field camera. Inside that band the GBA wrap-around
     * applies exactly, so scrolling behaves as on hardware; past it there is
     * nothing to show on a 400px screen, and the line stays backdrop rather
     * than repeating the same image.
     */
    int right = (int)Min((size & 1) ? 512 : 256, CTR_GAME_WIDTH);
    int bottom = (int)Min((size & 2) ? 512 : 256, CTR_GAME_HEIGHT);
    int left = 0, top = 0;

    if (sStage)
    {
        DrawStageBg(bg);
        return;
    }
    if (sBattle)
    {
        if (bg == 0) DrawBattleText(bg);
        else DrawBattleBg(bg);
        return;
    }
    if (sCentred)
    {
        if (DrawCentredBg(bg)) return;
        if (right > 240) right = 240;
        if (bottom > 160) bottom = 160;
    }
    if (right > sClipX1) right = sClipX1;
    if (bottom > sClipY1) bottom = sClipY1;
    if (left < sClipX0) left = sClipX0;
    if (top < sClipY0) top = sClipY0;
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    DrawTextSpan(bg, left, right, top, bottom);
}

/*
 * Layer textures. A text background is composed once, unscrolled, into a
 * texture of its own that repeats exactly like its tilemap wraps, and kept
 * there for as long as its tilemap, its tiles and its palettes stay the same
 * (LayerHash). Drawing it is then a handful of quads cut from that texture
 * instead of one quad per 8x8 tile:
 *
 * - a background that scrolls per line (the waves of the intro and the title)
 *   is one quad per run of lines sharing a scroll;
 * - a stage background (the intro, the title) is its picture plus the bands
 *   that fill the screen around it, about 35 quads for a whole 400x240 layer.
 *
 * On an Old 3DS a tile quad costs ~4.6 us of CPU, and the intro's leaves are
 * four 400x240 layers: 4500 quads and 22 ms a frame walked tile by tile. The
 * leaves never change while they are on screen, so the tilemap walk now
 * happens once for the scene.
 *
 * 16-bit colour is enough: the GBA has 15 bits and a transparent one.
 */
#define LAYER_IDLE_FRAMES 180
#define LAYER_RETRY_FRAMES 300u
typedef struct
{
    C3D_Tex tex;
    C3D_RenderTarget *target;
    uint32_t hash, usedFrame;
    bool valid;
} LayerTexture;
static LayerTexture sLayers[4];
/* Whether each background is drawn from its texture this frame. */
static bool sLayerReady[4];
static uint32_t sLayerFailFrame;

static unsigned LineValue(unsigned reg, int y, unsigned base)
{
    if (!(sLineMask & (1u << reg))) return base;
    /* A stage's picture has 160 lines; the lines above and below it carry
     * the wave on with the same period rather than stopping it flat. */
    if (sStage) y = ((y % 160) + 160) % 160;
    /* Around the battle scene, the lines next to it: the intro's two halves
     * slide in from the edges of the screen. */
    if (sBattle) y = y < 0 ? 0 : y > 159 ? 159 : y;
    if (y >= 0 && (unsigned)y < sLineCount)
        return sLineScroll[reg][y] & 511;
    return base;
}

/* The shown text backgrounds of the current display mode. */
static unsigned TextBackgrounds(void)
{
    unsigned display = Reg(0), mode = display & 7, found = 0;

    if (mode > 1) return 0;
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        if (!(display & (0x100u << bg))) continue;
        if (mode == 1 && bg >= 2) continue;
        found |= 1u << bg;
    }
    return found;
}

/* Which text backgrounds this frame scrolls per line. */
static unsigned LineBackgrounds(void)
{
    unsigned found = 0, text = TextBackgrounds();

    for (unsigned bg = 0; bg < 4; ++bg)
        if ((text & (1u << bg)) && (sLineMask & (3u << (bg * 2)))) found |= 1u << bg;
    return found;
}

static void LayerRelease(unsigned bg)
{
    LayerTexture *layer = &sLayers[bg];

    if (layer->target) C3D_RenderTargetDelete(layer->target);
    if (layer->tex.data) C3D_TexDelete(&layer->tex);
    memset(layer, 0, sizeof(*layer));
}

static void LayersRelease(void)
{
    for (unsigned bg = 0; bg < 4; ++bg) LayerRelease(bg);
}

/*
 * Outside the frame, where creating or deleting a target cannot stall one in
 * flight: a texture for every background drawn from one this frame, as large
 * as its tilemap. One that cannot be placed falls back to the tile walk and
 * is retried after a while, logged once.
 */
static bool LineWindows(void);

static void LayersPrepare(void)
{
    unsigned want = LineBackgrounds() | (sStage || sFieldLayers || LineWindows() ? TextBackgrounds() : 0);

    for (unsigned bg = 0; bg < 4; ++bg)
    {
        LayerTexture *layer = &sLayers[bg];
        unsigned size = Reg(8 + bg * 2) >> 14;
        unsigned width = (size & 1) ? 512 : 256, height = (size & 2) ? 512 : 256;

        sLayerReady[bg] = false;
        if (!(want & (1u << bg)))
        {
            if (layer->tex.data && sStats.frames - layer->usedFrame > LAYER_IDLE_FRAMES)
                LayerRelease(bg);
            continue;
        }
        if (layer->tex.data && (layer->tex.width != width || layer->tex.height != height))
            LayerRelease(bg);
        if (!layer->tex.data)
        {
            if (sLayerFailFrame && sStats.frames - sLayerFailFrame < LAYER_RETRY_FRAMES) continue;
            if (!C3D_TexInitVRAM(&layer->tex, width, height, GPU_RGBA5551)
                || !(layer->target = C3D_RenderTargetCreateFromTex(&layer->tex, GPU_TEXFACE_2D, 0, -1)))
            {
                if (!sLayerFailFrame)
                    CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for a %ux%u layer texture (free=%lu); "
                                 "tile walk", width, height, (unsigned long)vramSpaceFree());
                LayerRelease(bg);
                sLayerFailFrame = sStats.frames | 1;
                continue;
            }
            C3D_TexSetFilter(&layer->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&layer->tex, GPU_REPEAT, GPU_REPEAT);
        }
        layer->usedFrame = sStats.frames;
        sLayerReady[bg] = true;
    }
}

/*
 * Everything a layer texture is made of: its control register, its tilemap,
 * the tiles the tilemap uses and the version of every palette it can use.
 * Any change among them recomposes the texture; nothing else does.
 */
static uint32_t LayerHash(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned words = (size == 0 ? 1024 : size == 3 ? 4096 : 2048) / 2;
    const uint32_t *entries = (const uint32_t *)(sMemory.vram + map);
    uint32_t hash = 2166136261u ^ control;
    unsigned last = 0, bytes;

    if (map + words * 4 > 0x10000) words = (0x10000 - map) / 4;
    for (unsigned i = 0; i < words; ++i)
    {
        uint32_t pair = entries[i];

        hash = (hash ^ pair) * 16777619u;
        if ((pair & 1023) > last) last = pair & 1023;
        if (((pair >> 16) & 1023) > last) last = (pair >> 16) & 1023;
    }
    bytes = (last + 1) * (color256 ? 64 : 32);
    if (chars + bytes > 0x10000) bytes = 0x10000 - chars;
    {
        const uint32_t *tiles = (const uint32_t *)(sMemory.vram + chars);

        for (unsigned i = 0; i < bytes / 4; ++i) hash = (hash ^ tiles[i]) * 16777619u;
    }
    if (color256) hash = (hash ^ sPaletteVersion[32]) * 16777619u;
    else for (unsigned bank = 0; bank < 16; ++bank) hash = (hash ^ sPaletteVersion[bank]) * 16777619u;
    return hash;
}

/*
 * The field's backgrounds, kept in their textures cell by cell.
 *
 * Walked tile by tile, the 2D field is three 400x240 layers, about 4700 quads
 * a frame: 20 ms of CPU on an Old 3DS, so it never made 60 fps. Composed into
 * textures it is a quad per layer, but the whole-layer hash would recompose
 * all three every time a tile animates (water, flowers), since they share
 * their tiles. So each cell keeps what was drawn in it - the upload serial of
 * its tile (which covers the tile's bytes and palette) and its flips - and
 * only the cells whose tile, tilemap entry or palette changed are cleared and
 * drawn again: a row or a column as the camera moves, the animated tiles when
 * they animate. A frame where most cells changed (a new map, a palette fade)
 * is recomposed whole.
 */
#define LAYER_CELLS 4096
static uint32_t sCellSig[4][LAYER_CELLS];
static unsigned sCellControl[4];

static bool LayerDrawable(unsigned bg);

static bool LayerRenderCells(unsigned bg)
{
    static uint16_t dirty[LAYER_CELLS];
    static int16_t cellSlot[LAYER_CELLS];
    static uint16_t cellEntry[LAYER_CELLS];
    static uint32_t memoStamp[1024], stamp;
    static uint16_t memoEntry[1024];
    static int16_t memoSlot[1024];
    LayerTexture *layer = &sLayers[bg];
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    unsigned cells = rows * columns;
    bool full = !layer->valid || sCellControl[bg] != control;
    unsigned count = 0;

    ++stamp;
    for (unsigned row = 0; row < rows; ++row)
    {
        unsigned rowBase = map + CtrVideo_TextMapOffset(0, row, size);

        for (unsigned column = 0; column < columns; ++column)
        {
            unsigned cell = row * columns + column;
            unsigned entry = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
            unsigned index = entry & 1023;
            int slot;
            uint32_t sig;

            if (memoStamp[index] == stamp && memoEntry[index] == entry)
                slot = memoSlot[index];
            else
            {
                unsigned address = chars + index * (color256 ? 64 : 32);

                slot = address < 0x10000 ? GetTileSlot(address, entry >> 12, color256) : -1;
                memoStamp[index] = stamp;
                memoEntry[index] = (uint16_t)entry;
                memoSlot[index] = (int16_t)slot;
            }
            cellSlot[cell] = (int16_t)slot;
            cellEntry[cell] = (uint16_t)entry;
            /* Nothing drawn is 0; anything drawn has the top bit set. */
            sig = slot < 0 ? 0 : (((sTiles[slot].serial << 2) | ((entry >> 10) & 3)) | 1u << 31);
            if (!full && sCellSig[bg][cell] == sig) continue;
            sCellSig[bg][cell] = sig;
            dirty[count++] = (uint16_t)cell;
        }
    }
    if (count == 0) return false;
    layer->valid = true;
    sCellControl[bg] = control;

    BlendForget();
    if (full || count > cells / 2)
    {
        C2D_TargetClear(layer->target, 0);
        C2D_SceneBegin(layer->target);
        C2D_ViewReset();
        count = 0;
        for (unsigned cell = 0; cell < cells; ++cell)
            dirty[count++] = (uint16_t)cell;
    }
    else
    {
        /* Clear the changed cells to transparent first: a tile's transparent
         * pixels must not keep what was drawn there before. */
        C2D_SceneBegin(layer->target);
        C2D_ViewReset();
        C2D_Flush();
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        for (unsigned i = 0; i < count; ++i)
            C2D_DrawRectSolid((dirty[i] % columns) * 8, (dirty[i] / columns) * 8, 0, 8, 8, 0);
        C2D_Flush();
        BlendForget();
    }
    Blend(bg, false, false);
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned cell = dirty[i];

        if (cellSlot[cell] >= 0)
            DrawSlot(cellSlot[cell], (cell % columns) * 8, (cell / columns) * 8,
                     cellEntry[cell] & 1024, cellEntry[cell] & 2048);
    }
    C2D_Flush();
    return true;
}

/* A field background from its texture: one quad, wrapping as its tilemap
 * does, over the band the tilemap spans (DrawTextBg's limits). */
static bool DrawFieldBgTex(unsigned bg)
{
    LayerTexture *layer = &sLayers[bg];
    float width, height, sx, sy;
    int x0 = sClipX0 > 0 ? sClipX0 : 0, y0 = sClipY0 > 0 ? sClipY0 : 0, x1, y1;

    if (!sFieldLayers || !LayerDrawable(bg)) return false;
    width = layer->tex.width;
    height = layer->tex.height;
    x1 = sClipX1 < (int)width ? sClipX1 : (int)width;
    y1 = sClipY1 < (int)height ? sClipY1 : (int)height;
    if (x0 >= x1 || y0 >= y1) return true;
    sx = Reg(0x10 + bg * 4) & 511;
    sy = Reg(0x12 + bg * 4) & 511;
    ViewBase();
    {
        const Tex3DS_SubTexture run = {(u16)(x1 - x0), (u16)(y1 - y0),
            (sx + x0) / width, 1.0f - (sy + y0) / height,
            (sx + x1) / width, 1.0f - (sy + y1) / height};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){&layer->tex, &run}, x0 + CTR_VIEW_X + sLayerShift,
                        y0 + CTR_VIEW_Y, 0, &sTint, 1, 1);
    }
    return true;
}

/* Inside the frame, before the logical surface: the maps that changed. */
static void LayersRender(void)
{
    bool any = false;

    for (unsigned bg = 0; bg < 4; ++bg)
    {
        LayerTexture *layer = &sLayers[bg];
        unsigned control = Reg(8 + bg * 2), size = control >> 14;
        unsigned map = ((control >> 8) & 31) * 0x800;
        unsigned chars = ((control >> 2) & 3) * 0x4000;
        bool color256 = (control & 128) != 0;
        unsigned columns = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
        uint32_t hash;

        if (!sLayerReady[bg]) continue;
        if (sFieldLayers)
        {
            if (LayerRenderCells(bg)) any = true;
            continue;
        }
        hash = LayerHash(bg);
        if (layer->valid && layer->hash == hash) continue;
        layer->hash = hash;
        layer->valid = true;
        any = true;
        BlendForget();
        C2D_TargetClear(layer->target, 0);
        C2D_SceneBegin(layer->target);
        C2D_ViewReset();
        Blend(bg, false, false);
        for (unsigned row = 0; row < rows; ++row)
        {
            unsigned rowBase = map + CtrVideo_TextMapOffset(0, row, size);

            for (unsigned column = 0; column < columns; ++column)
            {
                unsigned entry = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
                unsigned address = chars + (entry & 1023) * (color256 ? 64 : 32);
                int slot;

                if (address >= 0x10000) continue;
                slot = GetTileSlot(address, entry >> 12, color256);
                if (slot >= 0)
                    DrawSlot(slot, column * 8, row * 8, entry & 1024, entry & 2048);
            }
        }
        C2D_Flush();
    }
    /* Rendering to a texture and then sampling it needs a command split. */
    if (any) C3D_FrameSplit(0);
}

static bool LayerDrawable(unsigned bg)
{
    return sLayerReady[bg] && sLayers[bg].valid;
}

static bool DrawBandBgTex(unsigned bg);

/*
 * One rectangle [x0, x1) x [y0, y1) of the screen, in GBA coordinates, cut
 * from a layer texture: the screen point (x0, y0) shows the picture point
 * (sx, sy), and each screen pixel steps (dx, dy) through the picture - 1 to
 * copy, -1 to mirror, a fraction to stretch. Clipped to the current clip
 * rectangle by moving its texture coordinates, so it needs no scissor. With
 * fade, its corners are tinted by EdgeFade.
 */
static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,
                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom)
{
    LayerTexture *layer = &sLayers[bg];
    int cx0 = x0 > sClipX0 ? x0 : sClipX0, cx1 = x1 < sClipX1 ? x1 : sClipX1;
    int cy0 = y0 > sClipY0 ? y0 : sClipY0, cy1 = y1 < sClipY1 ? y1 : sClipY1;
    float width = layer->tex.width, height = layer->tex.height;
    float ox = Reg(0x10 + bg * 4) & 511, oy = Reg(0x12 + bg * 4) & 511;
    float s0, s1, t0, t1;
    C2D_ImageTint tint = sTint;

    if (cx0 >= cx1 || cy0 >= cy1) return;
    s0 = sx + (cx0 - x0) * dx + ox;
    s1 = sx + (cx1 - x0) * dx + ox;
    t0 = sy + (cy0 - y0) * dy + oy;
    t1 = sy + (cy1 - y0) * dy + oy;
    if (fade)
    {
        bool white;
        float bright = LayerBrightness(bg, &white);

        FadeCorner(&tint, C2D_TopLeft, EdgeFade(cx0, cy0, top, bottom), bright, white);
        FadeCorner(&tint, C2D_TopRight, EdgeFade(cx1, cy0, top, bottom), bright, white);
        FadeCorner(&tint, C2D_BotLeft, EdgeFade(cx0, cy1, top, bottom), bright, white);
        FadeCorner(&tint, C2D_BotRight, EdgeFade(cx1, cy1, top, bottom), bright, white);
    }
    {
        /*
         * A subtexture whose top is below its bottom means "rotated" to
         * Tex3DS, so a mirror is drawn as the plain cut flipped by a negative
         * scale, the same way flipped tiles are. (Only unfaded cuts mirror.)
         */
        bool flipY = t1 < t0;
        float ta = flipY ? t1 : t0, tb = flipY ? t0 : t1;
        const Tex3DS_SubTexture cut = {(u16)(cx1 - cx0), (u16)(cy1 - cy0),
            s0 / width, 1.0f - ta / height, s1 / width, 1.0f - tb / height};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){&layer->tex, &cut}, cx0 + CTR_VIEW_X + sLayerShift,
                        cy0 + CTR_VIEW_Y, 0, &tint, 1, flipY ? -1 : 1);
    }
}

/* One corner cell: the corner block stretched to it, faded, skipped if black. */
static void DrawCornerCell(unsigned bg, int x0, int x1, int y0, int y1, float sx, float sy,
                           int top, int bottom)
{
    if (y0 < VIEW_TOP) y0 = VIEW_TOP;
    if (y1 > VIEW_BOTTOM) y1 = VIEW_BOTTOM;
    if (y0 < top && y1 > top) y1 = top;
    if (y0 < bottom && y1 > bottom) y0 = bottom;
    if (x0 >= x1 || y0 >= y1) return;
    if (sUnderlaid && EdgeFade(x0, y0, top, bottom) >= 1.0f && EdgeFade(x1, y0, top, bottom) >= 1.0f
        && EdgeFade(x0, y1, top, bottom) >= 1.0f && EdgeFade(x1, y1, top, bottom) >= 1.0f)
        return;
    DrawLayerRect(bg, x0, x1, y0, y1, sx, sy, 8.0f / (x1 - x0), 8.0f / (y1 - y0), true, top, bottom);
}

/*
 * One 8-line band above or below a still picture, from its texture: the
 * picture's row src repeated, or, when that row is plain (EdgeRowPlain), a
 * column of its plain tile wherever the row holds something else.
 */
static void DrawEdgeBand(unsigned bg, int y0, int y1, int src, int top, int bottom)
{
    unsigned plain = EdgeRowPlain(bg, src);
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800;
    unsigned scrollX = Reg(0x10 + bg * 4) & 511, scrollY = Reg(0x12 + bg * 4) & 511;
    unsigned rowBase = map + CtrVideo_TextMapOffset(0, (unsigned)(src + (int)scrollY) >> 3, size);
    int plainColumn = -1, run = 0;

    bool isPlain[30];

    if (plain == ~0u)
    {
        DrawLayerRect(bg, 0, 240, y0, y1, 0, src, 1, 1, true, top, bottom);
        return;
    }
    for (int c = 0; c < 30; ++c)
    {
        unsigned column = ((scrollX >> 3) + (unsigned)c) & ((size & 1) ? 63u : 31u);

        isPlain[c] = Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048) == plain;
        if (isPlain[c] && plainColumn < 0) plainColumn = c;
    }
    for (int c = 0; c <= 30; ++c)
    {
        if (c < 30 && isPlain[c]) continue;
        /* The plain columns up to here as one cut, then this one as plain. */
        if (c > run) DrawLayerRect(bg, run * 8, c * 8, y0, y1, run * 8, src, 1, 1, true, top, bottom);
        if (c < 30)
            DrawLayerRect(bg, c * 8, c * 8 + 8, y0, y1, plainColumn * 8, src, 1, 1, true, top, bottom);
        run = c + 1;
    }
}

/*
 * The intro's leaves scene, widened with art of its own.
 *
 * Behind the Game Freak logo the intro pans up a still scene of four layers.
 * Its art ends at the GBA screen's edges, and neither repeating the edge
 * tiles nor mirroring them gives a scene: they read as copies. So its
 * margins are new art made for them (scripts/gen_intro_margins.py): every
 * shape the edge cuts carried on from the edge's own pixels and closed, new
 * grass, bushes, plants and hills behind, and the plants layer's darker first
 * eight columns - the GBA screen's edge, where the pit ends - drawn anew. It
 * is made by the builder from the player's ROM, like the voxel data, and read
 * here from stage/leaves.bin: per layer the colour above the map's first row
 * and two strips of palette entries, 64x256 on the left (screen x -56..7) and
 * 56x256 on the right (x 240..295), a row per map row.
 *
 * Each strip becomes a texture of the current palette, remade when a palette
 * the scene uses changes (the fades in and out), and is drawn beside the
 * picture moving with the layer's scroll, faded like any stage margin. Above
 * and below the picture the layers' own rows show - the art the GBA scrolls
 * in as the scene pans - and above the sky's first row, its colour.
 *
 * The scene is known by its backgrounds' control registers (intro.c,
 * Task_Scene1_Load): four 256x512 4bpp maps on character base 0 at screen
 * bases 16, 18, 20 and 22, BG n at priority n.
 */
#define LEAVES_LEFT 64
#define LEAVES_RIGHT 56
#define LEAVES_ROWS 256
#define LEAVES_LAYER (1 + (LEAVES_LEFT + LEAVES_RIGHT) * LEAVES_ROWS)
#define LEAVES_BYTES (8 + 4 * LEAVES_LAYER)
static uint8_t *sLeaves;
static bool sLeavesTried;
/* Every drawn pixel of a layer's strips: its texel and its palette entry, so
 * a palette change is one store per pixel (a fade changes it every frame). */
typedef struct { uint16_t texel; uint8_t color; } LeavesPixel;
static LeavesPixel *sLeavesPixels[4];
static unsigned sLeavesCount[4];
static C3D_Tex sLeavesTex[4];
static uint32_t sLeavesKey[4], sLeavesUsed;

static bool LeavesScene(void)
{
    if (!sStage) return false;
    for (unsigned bg = 0; bg < 4; ++bg)
        if (Reg(8 + bg * 2) != (bg | ((16 + 2 * bg) << 8) | 0x8000)) return false;
    if (!sLeavesTried)
    {
        uint32_t size = 0;

        sLeavesTried = true;
        sLeaves = CtrData_Load("stage/leaves.bin", &size);
        if (sLeaves && (size != LEAVES_BYTES || memcmp(sLeaves, "EM3DLVS1", 8)))
        {
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: stage/leaves.bin is not what this build reads (%lu bytes)",
                         (unsigned long)size);
            free(sLeaves);
            sLeaves = NULL;
        }
        for (unsigned bg = 0; sLeaves && bg < 4; ++bg)
        {
            const uint8_t *layer = sLeaves + 8 + bg * LEAVES_LAYER + 1;
            unsigned count = 0;

            for (unsigned pass = 0; pass < 2; ++pass)
            {
                for (unsigned i = 0; i < (LEAVES_LEFT + LEAVES_RIGHT) * LEAVES_ROWS; ++i)
                {
                    unsigned color = layer[i], x, y;

                    if (color == 0xFF || !(color & 15)) continue;
                    if (pass == 0) { ++count; continue; }
                    if (i < LEAVES_LEFT * LEAVES_ROWS) { x = i % LEAVES_LEFT; y = i / LEAVES_LEFT; }
                    else
                    {
                        unsigned j = i - LEAVES_LEFT * LEAVES_ROWS;

                        x = 64 + j % LEAVES_RIGHT;
                        y = j / LEAVES_RIGHT;
                    }
                    sLeavesPixels[bg][sLeavesCount[bg]++] = (LeavesPixel){(uint16_t)CtrVideo_Texel(x, y, 128),
                                                                          (uint8_t)color};
                }
                if (pass == 0 && !(sLeavesPixels[bg] = malloc(count * sizeof(LeavesPixel))))
                {
                    free(sLeaves);
                    sLeaves = NULL;
                    break;
                }
            }
        }
    }
    return sLeaves != NULL;
}

static void LeavesRelease(void)
{
    for (unsigned bg = 0; bg < 4; ++bg)
        if (sLeavesTex[bg].data)
        {
            C3D_TexDelete(&sLeavesTex[bg]);
            memset(&sLeavesTex[bg], 0, sizeof(sLeavesTex[bg]));
        }
}

/* The layer's strips in the current palette; false without memory for them. */
static bool LeavesTexture(unsigned bg)
{
    C3D_Tex *tex = &sLeavesTex[bg];
    uint32_t key = 1;
    uint16_t *data;

    sLeavesUsed = sStats.frames;
    for (unsigned bank = 0; bank < 16; ++bank) key = key * 31 + sPaletteVersion[bank];
    if (!tex->data)
    {
        if (!C3D_TexInit(tex, 128, LEAVES_ROWS, GPU_RGBA5551))
        {
            memset(tex, 0, sizeof(*tex));
            return false;
        }
        C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        memset(tex->data, 0, 128 * LEAVES_ROWS * 2);
        sLeavesKey[bg] = key + 1;
    }
    if (sLeavesKey[bg] == key) return true;
    sLeavesKey[bg] = key;
    data = tex->data;
    for (unsigned i = 0; i < sLeavesCount[bg]; ++i)
        data[sLeavesPixels[bg][i].texel] = sTexturePalette[sLeavesPixels[bg][i].color];
    C3D_TexFlush(tex);
    return true;
}

/* A faded cut of a strip texture: screen [x0, x1) x [y0, y1), texel (s, t). */
static void DrawLeavesCut(C3D_Tex *tex, unsigned bg, int x0, int x1, int y0, int y1, float s, float t)
{
    bool white;
    float bright = LayerBrightness(bg, &white);
    C2D_ImageTint tint;

    if (x0 < sClipX0) { s += sClipX0 - x0; x0 = sClipX0; }
    if (y0 < sClipY0) { t += sClipY0 - y0; y0 = sClipY0; }
    if (x1 > sClipX1) x1 = sClipX1;
    if (y1 > sClipY1) y1 = sClipY1;
    if (x0 >= x1 || y0 >= y1) return;
    if (sUnderlaid && EdgeFade(x0, y0, 0, 160) >= 1.0f && EdgeFade(x1, y0, 0, 160) >= 1.0f
        && EdgeFade(x0, y1, 0, 160) >= 1.0f && EdgeFade(x1, y1, 0, 160) >= 1.0f)
        return;
    FadeCorner(&tint, C2D_TopLeft, EdgeFade(x0, y0, 0, 160), bright, white);
    FadeCorner(&tint, C2D_TopRight, EdgeFade(x1, y0, 0, 160), bright, white);
    FadeCorner(&tint, C2D_BotLeft, EdgeFade(x0, y1, 0, 160), bright, white);
    FadeCorner(&tint, C2D_BotRight, EdgeFade(x1, y1, 0, 160), bright, white);
    {
        const Tex3DS_SubTexture cut = {(u16)(x1 - x0), (u16)(y1 - y0),
            s / 128.0f, 1.0f - t / LEAVES_ROWS, (s + x1 - x0) / 128.0f, 1.0f - (t + y1 - y0) / LEAVES_ROWS};

        ++sStats.tiles;
        C2D_DrawImageAt((C2D_Image){tex, &cut}, x0 + CTR_VIEW_X + sLayerShift, y0 + CTR_VIEW_Y, 0, &tint, 1, 1);
    }
}

/* The sky above the map's first row, faded, over [x0, x1) x [y0, y1). */
static void DrawLeavesSky(unsigned bg, uint8_t sky, int x0, int x1, int y0, int y1)
{
    bool white;
    float bright = LayerBrightness(bg, &white);
    uint32_t rgb = CtrVideo_RGBA8(sPalette[sky], true);
    float corners[4];
    u32 colors[4];

    if (x0 < sClipX0) x0 = sClipX0;
    if (y0 < sClipY0) y0 = sClipY0;
    if (x1 > sClipX1) x1 = sClipX1;
    if (y1 > sClipY1) y1 = sClipY1;
    if (x0 >= x1 || y0 >= y1) return;
    corners[0] = EdgeFade(x0, y0, 0, 160);
    corners[1] = EdgeFade(x1, y0, 0, 160);
    corners[2] = EdgeFade(x0, y1, 0, 160);
    corners[3] = EdgeFade(x1, y1, 0, 160);
    if (sUnderlaid && corners[0] >= 1.0f && corners[1] >= 1.0f && corners[2] >= 1.0f && corners[3] >= 1.0f)
        return;
    for (unsigned i = 0; i < 4; ++i)
    {
        float c[3] = {(rgb >> 24) / 255.0f, ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f};

        for (unsigned k = 0; k < 3; ++k)
        {
            c[k] = white ? c[k] + (1.0f - c[k]) * bright : c[k] * (1.0f - bright);
            c[k] *= 1.0f - corners[i];
        }
        colors[i] = C2D_Color32((u8)(c[0] * 255.0f), (u8)(c[1] * 255.0f), (u8)(c[2] * 255.0f), 255);
    }
    ++sStats.tiles;
    C2D_DrawRectangle(x0 + CTR_VIEW_X + sLayerShift, y0 + CTR_VIEW_Y, 0, x1 - x0, y1 - y0,
                      colors[0], colors[1], colors[2], colors[3]);
}

/*
 * The margins of a leaves layer, the picture itself already drawn: in cells a
 * quarter of the fade apart (the fade is linear inside each), cut where the
 * map's rows wrap. False when the scene is not up or has no strips.
 */
static bool DrawLeavesMargins(unsigned bg)
{
    static const int xs[] = {-56, -42, -28, -14, 0, 8, 240, 254, 268, 282, 296};
    static const int ys[] = {-40, -28, -21, -14, -7, 0, 160, 167, 174, 181, 188, 200};
    unsigned scrollY = Reg(0x12 + bg * 4) & 511;
    uint8_t sky;

    if (!LeavesScene() || !LeavesTexture(bg)) return false;
    sky = sLeaves[8 + bg * LEAVES_LAYER];
    ViewBase();
    for (unsigned j = 0; j + 1 < sizeof(ys) / sizeof(ys[0]); ++j)
    {
        int y0 = ys[j], y1 = ys[j + 1];

        /* The layer's own rows above and below the picture. */
        if ((y1 <= 0 || y0 >= 160) && LayerDrawable(bg))
            DrawLayerRect(bg, 0, 240, y0, y1, 0, y0, 1, 1, true, 0, 160);
        for (int a = y0; a < y1;)
        {
            /* Map rows 0..255 hold the art, 256..511 only the sky. */
            unsigned row = (unsigned)(a + (int)scrollY) & 511;
            int b = a + (int)((row < 256 ? 256 : 512) - row);

            if (b > y1) b = y1;
            if (row < 256)
            {
                for (unsigned i = 0; i + 1 < sizeof(xs) / sizeof(xs[0]); ++i)
                {
                    int x0 = xs[i], x1 = xs[i + 1];

                    if (x0 == 8) continue;
                    DrawLeavesCut(&sLeavesTex[bg], bg, x0, x1, a, b, x0 < 240 ? x0 + 56 : x0 - 240 + 64,
                                  (float)row);
                }
            }
            else if (sky != 0xFF)
            {
                for (unsigned i = 0; i + 1 < sizeof(xs) / sizeof(xs[0]); ++i)
                    if (xs[i] != 8) DrawLeavesSky(bg, sky, xs[i], xs[i + 1], a, b);
                if (a < 0) DrawLeavesSky(bg, sky, 8, 240, a, b < 0 ? b : 0);
            }
            a = b;
        }
    }
    return true;
}

/*
 * A stage background from its texture, filled around the picture exactly as
 * DrawStageBg does it tile by tile: a layer that scrolls wraps across and is
 * mirrored above and below; a still one is carried out from its edges - the
 * 8-pixel band along each edge repeated outwards, the corners stretched from
 * the corner block - and faded to black.
 */
static bool DrawStageBgTex(unsigned bg)
{
    int top, bottom;
    bool wraps;

    if (!sStage || !LayerDrawable(bg)) return false;
    PictureRows(bg, &top, &bottom);
    wraps = sScrolls[bg];
    if (!wraps)
    {
        MeasureStill(bg);
        /* A still panorama wraps too when the scene around it scrolls. */
        if (sPanorama[bg])
            for (unsigned other = 0; other < 4; ++other)
                if (other != bg && sScrolls[other] && (Reg(0) & (0x100u << other))) wraps = true;
    }
    ViewBase();
    if (wraps)
    {
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, top, bottom, VIEW_LEFT, top, 1, 1, false, top, bottom);
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, VIEW_TOP, top, VIEW_LEFT, 2 * top - VIEW_TOP, 1, -1,
                      false, top, bottom);
        DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, bottom, VIEW_BOTTOM, VIEW_LEFT, bottom, 1, -1,
                      false, top, bottom);
        return true;
    }
    DrawLayerRect(bg, 0, 240, top, bottom, 0, top, 1, 1, false, top, bottom);
    if (DrawLeavesMargins(bg)) return true;
    for (int x = 0; x > VIEW_LEFT; x -= 8)
        DrawLayerRect(bg, x - 8, x, top, bottom, 0, top, 1, 1, true, top, bottom);
    for (int x = 240; x < VIEW_RIGHT; x += 8)
        DrawLayerRect(bg, x, x + 8, top, bottom, 232, top, 1, 1, true, top, bottom);
    for (int y = top; y > VIEW_TOP; y -= 8)
        DrawEdgeBand(bg, y - 8, y, top, top, bottom);
    for (int y = bottom; y < VIEW_BOTTOM; y += 8)
        DrawEdgeBand(bg, y, y + 8, bottom - 8, top, bottom);
    /*
     * The corners: the 8x8 corner block of the picture, in 16-pixel cells
     * each with its own fade, so the gradient bends round the corner instead
     * of being smeared across one quad. Cells already black are left to
     * StageUnderlay when it drew.
     */
    for (int y = top; y > VIEW_TOP; y -= 16)
        for (int x = 0; x > VIEW_LEFT; x -= 16)
        {
            DrawCornerCell(bg, x - 16, x, y - 16, y, 0, top, top, bottom);
            DrawCornerCell(bg, 240 - x, 256 - x, y - 16, y, 232, top, top, bottom);
        }
    for (int y = bottom; y < VIEW_BOTTOM; y += 16)
        for (int x = 0; x > VIEW_LEFT; x -= 16)
        {
            DrawCornerCell(bg, x - 16, x, y, y + 16, 0, bottom - 8, top, bottom);
            DrawCornerCell(bg, 240 - x, 256 - x, y, y + 16, 232, bottom - 8, top, bottom);
        }
    return true;
}

static bool DrawLineBg(unsigned bg)
{
    unsigned baseX = Reg(0x10 + bg * 4) & 511, baseY = Reg(0x12 + bg * 4) & 511;
    /* A still stage fades its margins (DrawEdgeRegion); so do these layers. */
    bool fade = sStage && !sScrolls[bg];
    int top = 0, bottom = 160;
    int clipX0 = sClipX0, clipX1 = sClipX1, clipY0 = sClipY0, clipY1 = sClipY1;
    /* The battle terrain repeats its top tile row above the picture, as
     * DrawBattleBg does; this is the line where that starts. */
    bool clampTop = sBattle && bg == 3;
    C3D_Tex *tex;
    float width, height;

    if (!(LineBackgrounds() & (1u << bg)) || !LayerDrawable(bg)) return false;
    tex = &sLayers[bg].tex;
    width = tex->width;
    height = tex->height;
    if (fade) PictureRows(bg, &top, &bottom);
    if (sBattle && !(Reg(8 + bg * 2) & 0x4000))
    {
        /* A 32-column layer only wraps into the sides when it is made to. */
        MeasureStill(bg);
        if (!sPanorama[bg])
        {
            if (clipX0 < 0) clipX0 = 0;
            if (clipX1 > 240) clipX1 = 240;
            if (clipY0 < 0) clipY0 = 0;
            if (clipY1 > 160) clipY1 = 160;
            clampTop = false;
        }
    }
    ViewBase();
    for (int y = clipY0; y < clipY1;)
    {
        unsigned x = LineValue(bg * 2, y, baseX), v = LineValue(bg * 2 + 1, y, baseY);
        int end = y + 1;
        /* The texture row line y shows. */
        int ty = (int)v + y;
        int rowTop = -(int)(v & 7);

        if (clampTop && y < rowTop)
        {
            /* The tile row under line 0, once per eight lines above it. */
            int phase = ((y - rowTop) % 8 + 8) % 8;

            ty = (int)(v - (v & 7)) + phase;
            end = y + 8 - phase;
            if (end > clipY1) end = clipY1;
        }
        else while (end < clipY1 && LineValue(bg * 2, end, baseX) == x
               && LineValue(bg * 2 + 1, end, baseY) == v)
        {
            /* The fade is linear between these lines, so a run stops at them. */
            if (fade && (end == top || end == bottom || end == top - (int)FADE_DOWN
                         || end == bottom + (int)FADE_DOWN))
                break;
            ++end;
        }
        /* Left margin, picture, right margin: each piece gets its own fade. */
        for (unsigned piece = 0; piece < 3; ++piece)
        {
            int x0 = fade ? (piece == 0 ? clipX0 : piece == 1 ? 0 : 240) : clipX0;
            int x1 = fade ? (piece == 0 ? 0 : piece == 1 ? 240 : clipX1) : clipX1;

            if (x0 < clipX0) x0 = clipX0;
            if (x1 > clipX1) x1 = clipX1;
            if (x0 >= x1) { if (!fade) break; continue; }
            {
                /* The texture repeats, so coordinates past its edges wrap. */
                const Tex3DS_SubTexture run = {(u16)(x1 - x0), (u16)(end - y),
                    ((int)x + x0) / width, 1.0f - ty / height,
                    ((int)x + x1) / width, 1.0f - (ty + end - y) / height};
                C2D_ImageTint tint = sTint;

                if (fade)
                {
                    bool white;
                    float bright = LayerBrightness(bg, &white);

                    FadeCorner(&tint, C2D_TopLeft, EdgeFade(x0, y, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_TopRight, EdgeFade(x1, y, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_BotLeft, EdgeFade(x0, end, top, bottom), bright, white);
                    FadeCorner(&tint, C2D_BotRight, EdgeFade(x1, end, top, bottom), bright, white);
                }
                ++sStats.tiles;
                C2D_DrawImageAt((C2D_Image){tex, &run}, x0 + CTR_VIEW_X + sLayerShift,
                                y + CTR_VIEW_Y, 0, &tint, 1, 1);
            }
            if (!fade) break;
        }
        y = end;
    }
    return true;
}

static void DrawAffineBg(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), base = bg == 2 ? 0x20 : 0x30;
    float a = (int16_t)Reg(base) / 256.0f, b = (int16_t)Reg(base + 2) / 256.0f;
    float c = (int16_t)Reg(base + 4) / 256.0f, d = (int16_t)Reg(base + 6) / 256.0f;
    float rx = CtrVideo_AffineReference(Reg(base + 8) | (Reg(base + 10) << 16)) / 256.0f;
    float ry = CtrVideo_AffineReference(Reg(base + 12) | (Reg(base + 14) << 16)) / 256.0f;
    float det = a * d - b * c;
    if (fabsf(det) < 0.00001f) { Error(5, "singular affine BG matrix"); return; }
    C3D_Mtx matrix;
    Mtx_Identity(&matrix);
    matrix.r[0] = FVec4_New(d / det, -b / det, 0, (b * ry - d * rx) / det + CTR_VIEW_X + sLayerShift);
    matrix.r[1] = FVec4_New(-c / det, a / det, 0, (c * rx - a * ry) / det + CTR_VIEW_Y);
    ViewAffine(&matrix);
    float minX = rx, maxX = rx, minY = ry, maxY = ry;
    for (unsigned corner = 1; corner < 4; ++corner)
    {
        float x = (corner & 1) ? sClipX1 : sClipX0;
        float y = (corner & 2) ? sClipY1 : sClipY0;
        float sx = rx + a * x + b * y, sy = ry + c * x + d * y;
        minX = fminf(minX, sx); maxX = fmaxf(maxX, sx);
        minY = fminf(minY, sy); maxY = fmaxf(maxY, sy);
    }
    int firstX = (int)floorf(minX / 8), lastX = (int)floorf(maxX / 8);
    int firstY = (int)floorf(minY / 8), lastY = (int)floorf(maxY / 8);
    unsigned tiles = 16u << (control >> 14), mask = tiles - 1;
    unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
    /*
     * A centred GBA screen fills its margins with its affine map repeated,
     * wrap-around or not: without it the space above and left of the picture
     * would be empty while the map's own sea shows below and to the right.
     */
    bool wrap = (control & 0x2000) || sCentred;
    /*
     * Under minification the screen maps to far more of the map than exists.
     * Without wrap-around nothing outside the map is drawn anyway, so clamping
     * the scan to it bounds the work by the map instead of by how small the
     * layer was scaled: the title logo shrinks enough to blow any fixed budget
     * and would disappear for those frames.
     */
    if (!wrap)
    {
        if (firstX < 0) firstX = 0;
        if (firstY < 0) firstY = 0;
        if (lastX >= (int)tiles) lastX = (int)tiles - 1;
        if (lastY >= (int)tiles) lastY = (int)tiles - 1;
    }
    if (lastX < firstX || lastY < firstY) return;
    if ((lastX - firstX + 1) * (lastY - firstY + 1) > 16384)
    { Error(6, "affine BG minification exceeds baseline budget"); return; }
    for (int y = firstY; y <= lastY; ++y)
        for (int x = firstX; x <= lastX; ++x)
        {
            if (!wrap && ((unsigned)x >= tiles || (unsigned)y >= tiles)) continue;
            unsigned entry = map + ((unsigned)y & mask) * tiles + ((unsigned)x & mask);
            if (entry >= 0x10000) { Error(0, "affine map outside BG VRAM"); continue; }
            DrawTile(chars + sMemory.vram[entry] * 64, 0, true, x * 8, y * 8, false, false);
        }
    ViewBase();
}

/* The PokéNav band a sprite belongs to: its top line's, or the header's for
 * the tab naming a screen under the main menu. */
static const NavBand *NavObjectBand(unsigned tile, int y)
{
    if (sNavBands == sNavSubmenu && tile >= sNavHeaderTiles[0] && tile < sNavHeaderTiles[1])
        return &sNavBands[0];
    for (unsigned b = sNavBandCount; b-- > 1;)
        if (y >= sNavBands[b].top && sNavBands[b].top < sNavBands[b].bottom) return &sNavBands[b];
    return sNavBands == sNavSubmenu ? &sNavBands[1] : &sNavBands[0];
}

static void DrawObjects(unsigned priority, bool effects)
{
    static const uint8_t dimensions[3][4][2] = {
        {{8,8},{16,16},{32,32},{64,64}},
        {{16,8},{32,8},{32,16},{64,32}},
        {{8,16},{8,32},{16,32},{32,64}}
    };
    for (int i = 127; i >= 0; --i)
    {
#if CTR_VOXEL_ENABLED
        if (sVoxelWeatherOnly && !(sVoxelWeatherOam[i >> 5] & (1u << (i & 31))))
            continue;
#endif
        const uint16_t *obj = sMemory.oam + i * 4;
        unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2];
        bool affine = (attr0 & 0x100) != 0, color256 = (attr0 & 0x2000) != 0;
        if ((!affine && (attr0 & 0x200)) || ((attr2 >> 10) & 3) != priority) continue;
        unsigned mode = (attr0 >> 10) & 3, shape = attr0 >> 14;
        if (mode == 2) { Error(7, "OBJ windows not supported"); continue; }
        if (mode == 3 || shape == 3) continue;
        if (attr0 & 0x1000) Error(8, "OBJ mosaic not supported");
        unsigned width = dimensions[shape][attr1 >> 14][0], height = dimensions[shape][attr1 >> 14][1];
        unsigned boxW = affine && (attr0 & 0x200) ? width * 2 : width;
        unsigned boxH = affine && (attr0 & 0x200) ? height * 2 : height;
        int x = attr1 & 511, y = attr0 & 255;
        if (sStage || sCentred || sBattle)
        {
            /*
             * The GBA's own reading: a sprite only comes back from the other
             * side when it crosses the end of the coordinate range. Unused
             * entries sit at y=240 x=464, which the wide view would otherwise
             * turn into a column of blank tiles at (-48, -16), in its margin.
             */
            if (x + (int)boxW > 512) x -= 512;
            if (y + (int)boxH > 256) y -= 256;
        }
        else
        {
            if (x >= VIEW_RIGHT) x -= 512;
            if (sOamAnchored[i >> 5] & (1u << (i & 31))) y = OamUnwrapY((unsigned)i, y);
            else if (y >= VIEW_BOTTOM) y -= 256;
#if CTR_VOXEL_ENABLED
            if (sVoxelWeatherOnly)
            {
                x += CTR_STAGE_X;
                y += CTR_STAGE_Y;
            }
#endif
        }
        /* Below the PokeNav's picture is its background, not the space the
         * GBA parks its unused sprites in. */
        if (sCentredScreen == CTR_CENTRED_POKENAV && y >= 160) continue;
        if (sNavBand && NavObjectBand(attr2 & 1023, y) != sNavBand) continue;
        if (x >= sClipX1 || x + (int)boxW <= sClipX0
         || y >= sClipY1 || y + (int)boxH <= sClipY0) continue;
        ++sStats.sprites;
        Blend(4, effects, mode == 1);
        ViewBase();
        if (affine)
        {
            unsigned index = ((attr1 >> 9) & 31) * 16;
            float a = (int16_t)sMemory.oam[index + 3] / 256.0f;
            float b = (int16_t)sMemory.oam[index + 7] / 256.0f;
            float c = (int16_t)sMemory.oam[index + 11] / 256.0f;
            float d = (int16_t)sMemory.oam[index + 15] / 256.0f;
            float det = a * d - b * c;
            if (fabsf(det) < 0.00001f) { Error(9, "singular OBJ affine matrix"); continue; }
            C3D_Mtx matrix;
            Mtx_Identity(&matrix);
            matrix.r[0] = FVec4_New(d / det, -b / det, 0, x + CTR_VIEW_X + sLayerShift + boxW / 2.0f - (d * width - b * height) / (2 * det));
            matrix.r[1] = FVec4_New(-c / det, a / det, 0, y + CTR_VIEW_Y + boxH / 2.0f - (a * height - c * width) / (2 * det));
            ViewAffine(&matrix);
        }
        bool flipX = !affine && (attr1 & 0x1000), flipY = !affine && (attr1 & 0x2000);
        for (unsigned ty = 0; ty < height / 8; ++ty)
            for (unsigned tx = 0; tx < width / 8; ++tx)
            {
                unsigned sx = flipX ? width / 8 - 1 - tx : tx;
                unsigned sy = flipY ? height / 8 - 1 - ty : ty;
                unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
                DrawTile(0x10000 + tile * 32, 16 + (attr2 >> 12), color256,
                         (affine ? 0 : x + CTR_VIEW_X + sLayerShift) + (int)tx * 8,
                         (affine ? 0 : y + CTR_VIEW_Y) + (int)ty * 8, flipX, flipY);
            }
    }
    ViewBase();
}

/*
 * An affine backmost layer in a band of the PokéNav (the Hoenn map, its sea
 * and all): as DrawNavBackmost does, the band's lines, then the picture's
 * bottom row repeated over the rest, each row moved into place by the view.
 */
static void NavScissor(int top, int bottom);

static void DrawNavBackmostAffine(unsigned bg)
{
    const NavBand *band = sNavBand;
    int clipY0 = sClipY0, clipY1 = sClipY1, viewY = sViewY;

    if (band->top < band->bottom)
    {
        if (sClipY0 < band->top) sClipY0 = band->top;
        if (sClipY1 > band->bottom) sClipY1 = band->bottom;
        NavScissor(band->top + band->shift, band->bottom + band->shift);
        if (sClipY0 < sClipY1) DrawAffineBg(bg);
    }
    for (int y = band->screenTop - band->shift; y < band->screenBottom - band->shift; y += 8)
    {
        if (y + 8 > band->top && y < band->bottom) continue;
        sViewY = viewY + y - 152;
        sClipY0 = clipY0 > y ? clipY0 - (y - 152) : 152;
        sClipY1 = clipY1 < y + 8 ? clipY1 - (y - 152) : 160;
        NavScissor(y + band->shift, y + band->shift + 8);
        if (sClipY0 < sClipY1) DrawAffineBg(bg);
    }
    sViewY = viewY;
    sClipY0 = clipY0;
    sClipY1 = clipY1;
    NavScissor(band->screenTop, band->screenBottom);
}

/* Narrows the scissor of a PokéNav band to lines [top, bottom) of the screen. */
static void NavScissor(int top, int bottom)
{
    C2D_Flush();
    sNavClip0 = top > sNavBand->screenTop ? top : sNavBand->screenTop;
    sNavClip1 = bottom < sNavBand->screenBottom ? bottom : sNavBand->screenBottom;
    RestoreScissor();
}

static void Layers(unsigned mask)
{
    unsigned display = Reg(0), mode = display & 7;
    mask &= ~sLayerExclude;
    if (mode > 2) { Error(10, "bitmap display modes are not part of baseline"); return; }
    for (int priority = 3; priority >= 0; --priority)
    {
        /* One depth plane at a time while composing them separately. */
        if (!(sPriorityMask & (1u << priority))) continue;
        /* Priority is the depth scale: 3 stays at the screen plane. */
        sLayerShift = (sLayerOrigin + sParallax * (3 - priority)) / sShiftZoom;
        for (int bg = 3; bg >= 0; --bg)
        {
            if (!(mask & (1u << bg)) || !(display & (0x100u << bg)) || (Reg(8 + bg * 2) & 3) != (unsigned)priority) continue;
            if ((mode == 1 && bg == 3) || (mode == 2 && bg < 2)) continue;
            if (Reg(8 + bg * 2) & 0x40) Error(11, "BG mosaic not supported");
            int clipY0 = sClipY0, clipY1 = sClipY1;
            bool lines = sNavBand && !(CentredLayers(&sCentredFills[sCentredScreen]) & (1u << bg));
            if (lines)
            {
                if (sClipY0 < sNavBand->top) sClipY0 = sNavBand->top;
                if (sClipY1 > sNavBand->bottom) sClipY1 = sNavBand->bottom;
                if (sClipY0 >= sClipY1)
                {
                    sClipY0 = clipY0;
                    sClipY1 = clipY1;
                    continue;
                }
                NavScissor(sNavBand->top + sNavBand->shift, sNavBand->bottom + sNavBand->shift);
            }
            Blend(bg, mask & 32, false);
            /* Which stage a slow frame is in: the layer walk, the sprites or
             * the GPU. Guessing that from fps alone costs a hardware run. */
            uint64_t start = svcGetSystemTick();
            float shift = sLayerShift;
            if (sFieldUi && bg == 0) sLayerShift += CTR_FIELD_UI_SHIFT / sShiftZoom;
            if (((mode == 1 && bg == 2) || mode == 2) && sNavBand && !lines) DrawNavBackmostAffine(bg);
            else if ((mode == 1 && bg == 2) || mode == 2) DrawAffineBg(bg);
            else if (sBattle && bg == 0) DrawBattleTextLayer(bg);
            else if (!DrawLineBg(bg) && !DrawFieldBgTex(bg) && !DrawStageBgTex(bg) && !DrawBandBgTex(bg))
                DrawTextBg(bg);
            sLayerShift = shift;
            if (lines)
            {
                sClipY0 = clipY0;
                sClipY1 = clipY1;
                NavScissor(sNavBand->screenTop, sNavBand->screenBottom);
            }
            sBgTicks += svcGetSystemTick() - start;
        }
        if ((mask & 16) && (display & 0x1000))
        {
            uint64_t start = svcGetSystemTick();
            DrawObjects(priority, mask & 32);
            sObjTicks += svcGetSystemTick() - start;
        }
    }
}

/*
 * A window edge is an 8-bit field, so 255 is the largest value the game can
 * write. On a 240x160 screen that already meant "past the edge"; on this
 * viewport it has to keep meaning the edge of the screen, or everything beyond
 * x=255 falls outside every window. The overworld sets WIN0H=0x00FF with
 * WININ showing all layers and WINOUT showing only the empty text layer, which
 * is exactly the black band on the right of the map.
 *
 * A window whose first edge is also saturated is empty on hardware (WIN1H is
 * set to 0xFFFF for that purpose) and stays empty here.
 */
static int WindowEdge(unsigned limits, bool last, unsigned extent)
{
    unsigned first = limits >> 8, end = limits & 255;

    if (last)
        return (end >= 255 && first < 255) ? (int)extent : (int)Min(end, extent);
    return (int)Min(first, extent);
}

/*
 * One axis of a window, in GBA coordinates. A stage keeps the GBA's own
 * reading of the registers (an end past the screen, or before the start, means
 * the screen edge), and a window that reaches an edge of the GBA screen reaches
 * the edge of the stage: that is what the game meant by it, and the letterbox
 * of the intro would otherwise leave its margins outside every window.
 */
/*
 * The battle intro opens its scene through window 0: a band across the whole
 * screen, with nothing shown outside it, growing from the middle line to the
 * full height. That is a curtain over the whole picture, so on the battle
 * screen it keeps the picture's proportions like the stage's letterbox does;
 * any other battle window (a spotlight, a move's band around a battler) marks
 * a place in the scene and stays 1:1.
 */
static bool BattleCurtain(void)
{
    unsigned across = Reg(0x40);

    return sBattle && sZoom == 1.0f && (Reg(0) & 0x2000) && !(Reg(0x4a) & 63)
        && (across >> 8) == 0 && (across & 255) >= 240;
}

static void WindowSpan(unsigned limits, bool vertical, int *first, int *last)
{
    bool gba = sStage || sBattle;
    unsigned extent = gba ? (vertical ? 160u : 240u)
                          : (vertical ? (unsigned)CTR_GAME_HEIGHT : (unsigned)CTR_GAME_WIDTH);

    *first = WindowEdge(limits, false, extent);
    *last = WindowEdge(limits, true, extent);
    if (!gba) return;
    if ((limits & 255) > extent || (limits >> 8) > (limits & 255)) *last = (int)extent;
    if (*first >= *last) return;
    if (vertical && sBattle && !BattleCurtain())
    {
        if (*first == 0) *first = VIEW_TOP;
        if (*last >= (int)extent) *last = VIEW_BOTTOM;
        return;
    }
    if (vertical)
    {
        /*
         * Vertically the stage keeps the picture's proportions: the intro's
         * cinematic bars close in to 32 lines of 160, and on a 240-line screen
         * that has to be 48 lines of 240, or the scene between them is a thin
         * strip in the middle of a black screen.
         */
        *first = VIEW_TOP + *first * (VIEW_BOTTOM - VIEW_TOP) / (int)extent;
        *last = VIEW_TOP + *last * (VIEW_BOTTOM - VIEW_TOP) / (int)extent;
        return;
    }
    if (*first == 0) *first = VIEW_LEFT;
    if (*last >= (int)extent) *last = VIEW_RIGHT;
}

/*
 * The window edges an HBlank DMA writes line by line (CtrVideo_SetLineWindow):
 * WIN0H, or WIN0H and WIN1H, for each of the picture's 160 lines. The PokéNav
 * lights its chosen option with one, and its condition graph is a polygon
 * drawn as two of them. Only the PokéNav's screens use them here (LineWindows):
 * the frame is composed in bands of lines whose windows are the same, each
 * band partitioned as a frame with fixed windows is.
 */
static struct
{
    unsigned windows;
    uint16_t across[2][160];
} sLineWindows;

/* The band being composed: its lines, and the line-driven windows' edges there. */
static struct
{
    bool on;
    int top, bottom;
    uint16_t across[2];
} sLineBand;

/*
 * A text background in a band of line windows (Compose): the picture's part
 * of the cell from the layer texture, and the margins as a centred screen
 * fills them. A band is a line or two tall, so walking its tiles would cost
 * far more than the quad.
 */
static bool DrawBandBgTex(unsigned bg)
{
    const CentredFill *fill = &sCentredFills[sCentredScreen];

    if (!sLineBand.on || !LayerDrawable(bg)) return false;
    ViewBase();
    if (sNavBand && (CentredLayers(fill) & (1u << bg)))
        DrawNavBackmost(bg, true);
    else
        DrawLayerRect(bg, 0, 240, 0, 160, 0, 0, 1, 1, false, 0, 0);
    if (!sNavBand && (CentredLayers(fill) & (1u << bg))) DrawCentredMargins(bg, fill, -64, 64);
    return true;
}

void CtrVideo_SetLineWindow(const uint16_t *values, unsigned lines, bool both)
{
    sLineWindows.windows = values ? (both ? 3u : 1u) : 0u;
    if (!values) return;
    for (unsigned y = 0; y < 160; ++y)
    {
        sLineWindows.across[0][y] = y < lines ? values[both ? y * 2 : y] : 0;
        sLineWindows.across[1][y] = y < lines && both ? values[y * 2 + 1] : 0;
    }
}

static bool LineWindows(void)
{
    return sLineWindows.windows && sCentredScreen == CTR_CENTRED_POKENAV && (Reg(0) & 0x6000);
}

/* Window w's rectangle in this band, empty when x0 >= x1 or y0 >= y1. */
static void WindowRect(unsigned w, int *x0, int *x1, int *y0, int *y1)
{
    unsigned across = Reg(0x40 + 2 * w);

    if (sLineBand.on && (sLineWindows.windows & (1u << w))) across = sLineBand.across[w];
    WindowSpan(across, false, x0, x1);
    WindowSpan(Reg(0x44 + 2 * w), true, y0, y1);
    if (sLineBand.on)
    {
        if (*y0 < sLineBand.top) *y0 = sLineBand.top;
        if (*y1 > sLineBand.bottom) *y1 = sLineBand.bottom;
    }
}

static bool Inside(int px, int py, unsigned w)
{
    int x0, x1, y0, y1;

    WindowRect(w, &x0, &x1, &y0, &y1);
    /* Same reading of an empty window as the partition uses. */
    return x0 < x1 && y0 < y1 && px >= x0 && px < x1 && py >= y0 && py < y1;
}

static void ComposeBand(int top, int bottom)
{
    unsigned display = Reg(0);
    /* Partition by window edges; each nonoverlapping rectangle has a uniform
     * WIN0 > WIN1 > outside mask. GPU scissor clips transformed primitives. */
    /* Window rectangles are GBA coordinates; the partition covers the whole
     * viewport so the margins keep the "outside" mask. */
    int xs[6] = {VIEW_LEFT, VIEW_RIGHT};
    int ys[6] = {top, bottom};
    unsigned nx = 2, ny = 2;
    for (unsigned w = 0; w < 2; ++w)
        if (display & (0x2000u << w))
        {
            int x0, x1, y0, y1;

            WindowRect(w, &x0, &x1, &y0, &y1);
            if (y0 < top) y0 = top;
            if (y1 > bottom) y1 = bottom;

            /*
             * A window left enabled with both edges past the screen is how the
             * game turns one off: the overworld runs with WIN1H = 0xFFFF all
             * the time. It covers nothing, so its edges must not cut the
             * frame. Cutting there split every overworld frame in two at
             * x=255 - the end of the GBA's addressable width, which is exactly
             * where the seam showed - and walked every layer twice to do it.
             */
            if (x0 >= x1 || y0 >= y1) continue;
            xs[nx++] = x0;
            xs[nx++] = x1;
            ys[ny++] = y0;
            ys[ny++] = y1;
        }
    for (unsigned i = 0; i < nx; ++i) for (unsigned j = i + 1; j < nx; ++j)
        if (xs[j] < xs[i]) { int t=xs[j]; xs[j]=xs[i]; xs[i]=t; }
    for (unsigned i = 0; i < ny; ++i) for (unsigned j = i + 1; j < ny; ++j)
        if (ys[j] < ys[i]) { int t=ys[j]; ys[j]=ys[i]; ys[i]=t; }
    for (unsigned y = 0; y + 1 < ny; ++y) for (unsigned x = 0; x + 1 < nx; ++x)
    {
        if (xs[x] == xs[x+1] || ys[y] == ys[y+1]) continue;
        unsigned mask = Reg(0x4a) & 63;
        for (int w = 1; w >= 0; --w)
            if ((display & (0x2000u << w)) && Inside(xs[x], ys[y], (unsigned)w))
                mask = (Reg(0x48) >> (8*w)) & 63;
        C2D_Flush();
        Scissor(xs[x], ys[y], xs[x+1], ys[y+1]);
        sScissored = true;
        sClipX0 = xs[x]; sClipX1 = xs[x+1];
        sClipY0 = ys[y]; sClipY1 = ys[y+1];
        Layers(mask);
    }
}

static void Compose(void)
{
    unsigned display = Reg(0);
    /* A band of the PokéNav composes only its part of the screen. */
    int top = sNavBand ? sClipY0 : VIEW_TOP, bottom = sNavBand ? sClipY1 : VIEW_BOTTOM;
    int first = top > 0 ? top : 0, end = bottom < 160 ? bottom : 160;

    if (display & 0x8000) Error(7, "OBJ windows not supported");
    if (!(display & 0x6000)) { Layers(63); return; }
    if (!LineWindows())
        ComposeBand(top, bottom);
    else
    {
        /* Above and below the picture no line opens a window. */
        sLineBand.on = true;
        sLineBand.across[0] = sLineBand.across[1] = 0;
        sLineBand.top = top;
        sLineBand.bottom = bottom < 0 ? bottom : 0;
        if (top < 0) ComposeBand(top, sLineBand.bottom);
        for (int y = first, next; y < end; y = next)
        {
            sLineBand.across[0] = sLineWindows.across[0][y];
            sLineBand.across[1] = sLineWindows.across[1][y];
            for (next = y + 1; next < end; ++next)
                if (sLineWindows.across[0][next] != sLineBand.across[0]
                 || sLineWindows.across[1][next] != sLineBand.across[1]) break;
            sLineBand.top = y;
            sLineBand.bottom = next;
            ComposeBand(y, next);
        }
        sLineBand.across[0] = sLineBand.across[1] = 0;
        sLineBand.top = top > 160 ? top : 160;
        sLineBand.bottom = bottom;
        if (bottom > 160) ComposeBand(sLineBand.top, bottom);
        sLineBand.on = false;
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    ClipToView();
}

bool CtrVideo_Init(void)
{
    const char *step = "C3D_Init command buffer";
    sC3d = C3D_Init(0x100000);
    if (!sC3d) goto fail;
    step = "C2D_Init geometry buffers";
    sC2d = C2D_Init(MAX_DRAWS);
    if (!sC2d) goto fail;
    step = "tile atlas in linear memory";
    if (!C3D_TexInit(&sAtlas, ATLAS_SIZE, ATLAS_SIZE, GPU_RGBA5551)) goto fail;
    /* Citro3D rejects render-to-texture targets outside VRAM. The atlas is
     * CPU-updated linear memory, but the GPU-written surface MUST be VRAM. */
    step = "logical surface in VRAM";
    if (!C3D_TexInitVRAM(&sSurface, 512, 256, GPU_RGBA8)) goto fail;
    C3D_TexSetFilter(&sAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&sSurface, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetWrap(&sSurface, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    step = "logical render target";
    sLogical = C3D_RenderTargetCreateFromTex(&sSurface, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16);
    if (!sLogical) goto fail;
    step = "top screen render target";
    sTop = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    if (!sTop) goto fail;
    /* The right eye is created once and only rendered while the 3D slider is
     * up; a console without it simply never composes a second pass. */
    sTopRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    if (!sTopRight) CtrLog_Write(CTR_LOG_VIDEO, "no right eye target: 3D disabled");
    for (unsigned i = 0; i < 64; ++i)
        sMorton[i] = CtrVideo_Texel(i & 7, i / 8, 8);
    for (unsigned i = 0; i < 512; ++i)
        sTexturePalette[i] = CtrVideo_RGBA5551(sPalette[i]);
    C2D_Prepare();
#if CTR_VOXEL_ENABLED
    /* Citro3D and Citro2D are up; the voxel module only adds its own shader,
     * textures and buffers on top of them. */
    step = "voxel overworld renderer";
    if (!CtrVoxel_Init()) goto fail;
#endif
    sFpsStart = CtrPlatform_Milliseconds();
    CtrLog_Write(CTR_LOG_VIDEO, "GPU: RGBA5551 atlas, 8 MiB linear heap, native 400x240 viewport");
    return true;
fail:
    CtrLog_Write(CTR_LOG_ERROR, "VIDEO init failed: %s (linear free=%lu, VRAM free=%lu)",
                 step, (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());
    CtrVideo_Shutdown();
    return false;
}

void CtrVideo_Bind(CtrVideoMemory memory)
{
    sMemory = memory;
    memset(sHash, 0, sizeof(sHash));
    sUsed = 0;
}

/*
 * Composes the logical frame with this eye's parallax and blits it to one
 * screen buffer. Render-to-texture followed by sampling that texture needs a
 * GPU command split, and so does composing the surface again for the other
 * eye, so each pass ends with one.
 */
/*
 * Outside the frame: the battle scene's surface while the battle is up, given
 * back a few seconds after. Without VRAM for it the scene is composed at 1.5
 * directly, and the allocation is tried again later.
 */
static void SceneRelease(void)
{
    if (sScene) C3D_RenderTargetDelete(sScene);
    if (sSceneTex.data) C3D_TexDelete(&sSceneTex);
    sScene = NULL;
    memset(&sSceneTex, 0, sizeof(sSceneTex));
}

static void ScenePrepare(void)
{
    if (!sBattle)
    {
        if (sScene && sStats.frames - sSceneUsedFrame > LAYER_IDLE_FRAMES) SceneRelease();
        return;
    }
    sSceneUsedFrame = sStats.frames;
    if (sScene || (sSceneFailed && sStats.frames - sSceneFailFrame < LAYER_RETRY_FRAMES)) return;
    for (unsigned attempt = 0; attempt < 2 && !sScene; ++attempt)
    {
        if (C3D_TexInitVRAM(&sSceneTex, SCENE_W, SCENE_H, GPU_RGBA5551)
            && (sScene = C3D_RenderTargetCreateFromTex(&sSceneTex, GPU_TEXFACE_2D, 0, -1)))
            break;
        SceneRelease();
        /* The overworld's atlases of other maps are what a 2D screen gets
         * back first, as for the depth planes (BandsReady). */
#if CTR_VOXEL_ENABLED
        if (attempt == 0 && CtrVoxel_ReleaseIdleVram() == 0) break;
#else
        break;
#endif
    }
    if (!sScene)
    {
        if (!sSceneFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for the battle scene surface (free=%lu); "
                         "unfiltered zoom", (unsigned long)vramSpaceFree());
        sSceneFailed = true;
        sSceneFailFrame = sStats.frames;
        return;
    }
    CtrLog_Write(CTR_LOG_VIDEO, "battle scene surface ready: filtered zoom (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    sSceneFailed = false;
    C3D_TexSetFilter(&sSceneTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sSceneTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

/*
 * The battle scene - everything but the text box - at SCENE_ZOOM into its own
 * surface, then that surface on the logical one at CTR_BATTLE_ZOOM. The scene
 * surface starts at the GBA pixel on the screen's top-left corner, so its
 * origin lands on screen (ox, oy), at most a screen pixel off the edge.
 */
static void RenderBattleScene(uint32_t clear)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;
    /*
     * The surface holds the lowest SCENE_H / SCENE_ZOOM lines of the scene.
     * What the screen shows above them is sky carried up from the terrain's
     * top row (DrawBattleBg), so it is filled from the surface's own top tile
     * rows, repeated, rather than doubling the surface for a dozen lines.
     */
    int left = VIEW_LEFT, top = VIEW_TOP, lowest = 112 - (int)(SCENE_H / SCENE_ZOOM);
    float ox, oy;

    if (top < lowest) top = lowest;
    ox = left * zoom + offX;
    oy = top * zoom + offY;
    float scale = zoom / SCENE_ZOOM;
    int width = (int)ceilf((CTR_GAME_WIDTH - ox) / scale);
    int height = (int)ceilf((CTR_GAME_HEIGHT - 48 - oy) / scale);
    Tex3DS_SubTexture cut;

    if (width > SCENE_W) width = SCENE_W;
    if (height > SCENE_H) height = SCENE_H;
    cut = (Tex3DS_SubTexture){(u16)width, (u16)height, 0, 1,
        width / (float)SCENE_W, 1 - height / (float)SCENE_H};
    sZoom = SCENE_ZOOM;
    sOffX = -left * SCENE_ZOOM;
    sOffY = -top * SCENE_ZOOM;
    sTargetW = width;
    sTargetH = height;
    sSurfaceH = SCENE_H;
    ClipToView();
    BlendForget();
    /* Cleared transparent: the backdrop is the logical surface's own clear,
     * which is RGBA8. A colour cleared into this 5551 surface comes out
     * wrong - black was a bright blue. */
    (void)clear;
    C2D_TargetClear(sScene, 0);
    C2D_SceneBegin(sScene);
    Blend(5, false, false);
    sLayerExclude = 1;
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FrameSplit(0);

    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sTargetW = CTR_GAME_WIDTH;
    sTargetH = CTR_GAME_HEIGHT;
    sSurfaceH = 256;
    ClipToView();
    BlendForget();
    C2D_SceneBegin(sLogical);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSceneTex, &cut}, ox, oy, 0, NULL, scale, scale);
    {
        /* Two tile rows, the period of the stripes the terrain repeats. */
        const float rows = 16 * SCENE_ZOOM;
        const Tex3DS_SubTexture strip = {(u16)width, (u16)rows, 0, 1,
            width / (float)SCENE_W, 1 - rows / SCENE_H};

        for (float y = oy; y > 0; )
        {
            y -= rows * scale;
            C2D_DrawImageAt((C2D_Image){&sSceneTex, &strip}, ox, y, 0, NULL, scale, scale);
        }
    }
    C2D_Flush();
}

/*
 * The PokéNav's surface on the bottom screen: 240x240, the area left of the
 * button column. It is not linked to the screen, since a linked target is
 * copied over the whole of it and would cover the column the bottom screen
 * draws itself; after the frame it is copied into the first 240 columns of
 * the framebuffer, which are exactly that area (the framebuffer runs column
 * by column from the left edge). Made when the PokéNav opens, given back a
 * few seconds after it closes.
 */
#define BOTTOM_SIZE 240
static C3D_RenderTarget *sBottom;
static uint32_t sBottomUsed, sBottomFailFrame;
static bool sBottomFailed;

static void BottomRelease(void)
{
    if (sBottom) C3D_RenderTargetDelete(sBottom);
    sBottom = NULL;
}

/* Outside the frame, where deleting a render target may wait for the GPU. */
static bool BottomReady(bool wanted)
{
    if (!wanted)
    {
        if (sBottom && sStats.frames - sBottomUsed > LAYER_IDLE_FRAMES)
        {
            BottomRelease();
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen surface released (VRAM free=%lu)",
                         (unsigned long)vramSpaceFree());
        }
        return false;
    }
    sBottomUsed = sStats.frames;
    if (sBottom) return true;
    if (sBottomFailed && sStats.frames - sBottomFailFrame < LAYER_RETRY_FRAMES) return false;
    sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_SIZE, GPU_RB_RGB565, -1);
#if CTR_VOXEL_ENABLED
    if (!sBottom && CtrVoxel_ReleaseIdleVram() > 0)
        sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_SIZE, GPU_RB_RGB565, -1);
#endif
    if (!sBottom)
    {
        if (!sBottomFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for the bottom screen surface (free=%lu)",
                         (unsigned long)vramSpaceFree());
        sBottomFailed = true;
        sBottomFailFrame = sStats.frames;
        return false;
    }
    sBottomFailed = false;
    CtrLog_Write(CTR_LOG_VIDEO, "bottom screen surface ready (VRAM free=%lu)", (unsigned long)vramSpaceFree());
    return true;
}

static bool sBottomInUse;

bool CtrVideo_BottomInUse(void) { return sBottomInUse; }

/* After the frame: the composed picture into the bottom screen's framebuffer. */
static void BottomTransfer(void)
{
    u32 *fb = (u32 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

    if (!sBottom || !fb || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return;
    C3D_SyncDisplayTransfer((u32 *)sBottom->frameBuf.colorBuf, GX_BUFFER_DIM(BOTTOM_SIZE, BOTTOM_SIZE),
                            fb, GX_BUFFER_DIM(BOTTOM_SIZE, BOTTOM_SIZE),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
                            | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565)
                            | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565)
                            | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

/* The PokéNav's frame on the bottom screen, band by band (sNavMain). */
static void NavCompose(void)
{
    unsigned short first, end;
    int viewY = sViewY;

    /* The bar is on top in the main menu; slid down past half, a help bar. */
    sNavBands = (Reg(0x12) & 511) >= 16 ? sNavSubmenu : sNavMain;
    sNavBandCount = sNavBands == sNavSubmenu ? 3 : 2;
    sNavHeaderTiles[0] = sNavHeaderTiles[1] = 0;
    if (CtrPokenav_HeaderTiles(&first, &end))
    {
        sNavHeaderTiles[0] = first;
        sNavHeaderTiles[1] = end;
    }
    for (unsigned b = 0; b < sNavBandCount; ++b)
    {
        sNavBand = &sNavBands[b];
        sViewY = sNavBand->shift;
        ClipToView();
        sClipY0 = sNavBand->screenTop - sViewY;
        sClipY1 = sNavBand->screenBottom - sViewY;
        sNavClip0 = sNavBand->screenTop;
        sNavClip1 = sNavBand->screenBottom;
        C2D_Flush();
        sScissored = true;
        RestoreScissor();
        Compose();
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        sScissored = false;
    }
    sNavBand = NULL;
    sViewY = viewY;
    ClipToView();
}

int CtrVideo_BottomPictureY(int y)
{
    for (unsigned b = 0; b < sNavBandCount; ++b)
    {
        const NavBand *band = &sNavBands[b];
        int line = y - band->shift;

        if (y < band->screenTop || y >= band->screenBottom) continue;
        /* The header tab lies where it does on the GBA. */
        if (band->top == band->bottom || (line >= band->top && line < band->bottom)) return line;
        return -1;
    }
    return -1;
}

static void RenderEye(C3D_RenderTarget *target, uint32_t clear, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    bool scene = sBattle && sScene;

    sParallax = parallax;
    sLayerShift = 0;
    BlendForget();
    C2D_TargetClear(sLogical, clear);
    if (scene) RenderBattleScene(clear);
    C2D_SceneBegin(sLogical);
    Blend(5, false, false);
    StageUnderlay();
    /* After a scene composed on its own, only the text box is left. */
    if (scene) sLayerExclude = 63 & ~(1u | 32u);
    if (!(Reg(0) & 128))
    {
        if (target == sBottom) NavCompose();
        else Compose();
    }
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FrameSplit(0);

    BlendForget();
    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(target);
    if (target == sBottom)
    {
        /* The middle 240 columns of the logical frame: the picture and the
         * margins above and below it. Tilted as a screen target is. */
        const Tex3DS_SubTexture middle = {BOTTOM_SIZE, BOTTOM_SIZE,
            (CTR_GAME_WIDTH - BOTTOM_SIZE) / 2 / 512.0f, 1,
            (CTR_GAME_WIDTH + BOTTOM_SIZE) / 2 / 512.0f, 1 - BOTTOM_SIZE / 256.0f};

        C2D_SceneSize(BOTTOM_SIZE, BOTTOM_SIZE, true);
        C2D_ViewReset();
        Blend(5, false, false);
        C2D_DrawImageAt((C2D_Image){&sSurface, &middle}, 0, 0, 0, NULL, 1, 1);
    }
    else
    {
        C2D_ViewReset();
        Blend(5, false, false);
        C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    }
    C2D_Flush();
    C3D_FrameSplit(0);
}

#if CTR_VOXEL_ENABLED
/*
 * The game's own UI and weather over the voxel world. BG0 carries text; only
 * OAM entries tagged by the sprite sorter as weather are composed. Drawing all
 * OBJ here would duplicate the player and every NPC over their billboards.
 */
/* Fixed north-facing camera: the upper 88 pixels are the distant background.
 * Four one-pixel taps, fading to zero towards the focus plane, reuse the world
 * surface. No extra target, depth readback or full-screen blur on Old 3DS. */
static void VoxelBackgroundBlur(void)
{
    static const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, 0.0f);
    C2D_TopImageTint(&tint, C2D_Color32(255, 255, 255, 56), 0.0f);
    for (unsigned i = 0; i < 4; ++i)
    {
        float x = 1.0f + offsets[i][0], y = 1.0f + offsets[i][1];
        const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, 87,
            x / 512.0f, 1.0f - y / 256.0f,
            (x + CTR_GAME_WIDTH - 2) / 512.0f, 1.0f - (y + 87) / 256.0f};
        C2D_DrawImageAt((C2D_Image){&sSurface, &region}, 1, 1, 0, &tint, 1, 1);
    }
}

static void ComposeVoxelOverlay(void)
{
    sPriorityMask = 15;
    sParallax = 0;
    sLayerOrigin = 0.0f;
    sLayerShift = 0.0f;
    sClipX0 = sClipY0 = 0;
    sClipX1 = CTR_GAME_WIDTH;
    sClipY1 = CTR_GAME_HEIGHT;
    sVoxelWeatherOnly = true;
    Layers(1u << 4);
    sVoxelWeatherOnly = false;
    /* Text windows and prompts must stay above the precipitation. */
    sLayerOrigin = CTR_FIELD_UI_SHIFT;
    Layers(1u << 0);
    sLayerOrigin = 0.0f;
}

/*
 * The voxel path of the single Citro3D frame opened by CtrVideo_Present.
 * Same shape as RenderEye: compose the logical surface, split, blit it to the
 * screen. What changes is who composes it.
 */
static void RenderVoxel(uint32_t clear)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};

    /* C2D_TargetClear clears colour and depth, which the 3D pass needs. */
    C2D_TargetClear(sLogical, clear);
    CtrVoxel_Draw(sLogical, 0.0f);

    /* Finish the world before sampling it. UI is drawn on top after blur. */
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FrameSplit(0);

    /* Back to the 2D compositor, which assumes its own program and no depth. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    C2D_TargetClear(sTop, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(sTop);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    VoxelBackgroundBlur();
    if (!(Reg(0) & 128)) ComposeVoxelOverlay();
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FrameSplit(0);
}
#endif

/*
 * How many depth planes this frame can be split into.
 *
 * A layer that blends with whatever lies beneath it has to keep that beneath
 * in the same surface, so everything from its priority down stays together.
 * That is GBA alpha blending, which needs a target-1 layer, a target-2 layer
 * and a nonzero second coefficient (the overworld leaves the effect enabled
 * with no target-1 layer at all, which blends nothing), and a sprite in the
 * semi-transparent OBJ mode, which blends whatever its own priority allows.
 *
 * The result is planes 0..count-2 holding one priority each and the last
 * holding the rest, so the planes never change what a frame looks like.
 */
static unsigned DepthPlanes(void)
{
    unsigned display = Reg(0), control = Reg(0x50);
    unsigned target1 = control & 63, target2 = (control >> 8) & 63;
    unsigned merge = 4;

    if (((control >> 6) & 3) == 1 && target1 && target2 && ((Reg(0x52) >> 8) & 31))
    {
        if (target1 & 16) merge = 0;
        for (unsigned bg = 0; bg < 4 && merge; ++bg)
            if ((target1 & (1u << bg)) && (display & (0x100u << bg)))
            {
                unsigned priority = Reg(8 + bg * 2) & 3;
                if (priority < merge) merge = priority;
            }
    }
    /*
     * Backgrounds scrolled together are one image cut into layers, like the
     * three metatile layers of the field, and giving them different depths
     * pulls their tiles a pixel apart. Only a shared nonzero scroll means
     * that: on a still screen every layer sits at zero and they are separate
     * pictures stacked on each other, which is exactly where depth belongs.
     */
    for (unsigned bg = 0; bg + 1 < 4 && merge; ++bg)
        for (unsigned other = bg + 1; other < 4 && merge; ++other)
        {
            unsigned scroll = Reg(0x10 + bg * 4) | (Reg(0x12 + bg * 4) << 16);
            unsigned priority, otherPriority;

            if (!(display & (0x100u << bg)) || !(display & (0x100u << other))) continue;
            if (!scroll || scroll != (Reg(0x10 + other * 4) | (Reg(0x12 + other * 4) << 16)))
                continue;
            priority = Reg(8 + bg * 2) & 3;
            otherPriority = Reg(8 + other * 2) & 3;
            if (otherPriority < priority) priority = otherPriority;
            if (priority < merge) merge = priority;
        }
    if (display & 0x1000)
        for (unsigned i = 0; i < 128 && merge; ++i)
        {
            unsigned attr0 = sMemory.oam[i * 4];
            unsigned priority;

            if (!(attr0 & 0x100) && (attr0 & 0x200)) continue;
            if (((attr0 >> 10) & 3) != 1) continue;
            priority = (sMemory.oam[i * 4 + 2] >> 10) & 3;
            if (priority < merge) merge = priority;
        }
    return merge + 1 > CTR_BANDS ? CTR_BANDS : merge + 1;
}

/*
 * Released again once unused for a few seconds - the slider lowered, or the
 * voxel overworld on screen, which never composes planes. They are 1.5 MiB of
 * VRAM, and kept for good after the title screen was shown in 3D they left
 * the overworld's atlases, pages and chunks starving for it.
 */
#define CTR_BANDS_IDLE_FRAMES 180
static uint32_t sBandsUsedFrame;
/* Asked by the overworld when an atlas found no VRAM; honoured before the next
 * frame opens, since deleting a render target may wait for the GPU. */
static bool sPlaneReleaseAsked;

void CtrVideo_RequestPlaneRelease(void)
{
    sPlaneReleaseAsked = true;
}

/*
 * Set inside a frame that wanted the planes and found none. They are made
 * before the next frame opens: a failed attempt frees the planes it did get,
 * and C3D_RenderTargetDelete inside an open frame is svcBreak(USERBREAK_PANIC)
 * in citro3d - on an Old 3DS, whose VRAM often holds only two of the three,
 * that was opening a menu with the slider up. Until then, per eye.
 */
static bool sBandsWanted;

static void BandsRelease(void)
{
    for (unsigned i = 0; i < CTR_BANDS; ++i)
    {
        if (sBand[i]) C3D_RenderTargetDelete(sBand[i]);
        if (sBandTex[i].data) C3D_TexDelete(&sBandTex[i]);
        sBand[i] = NULL;
        memset(&sBandTex[i], 0, sizeof(sBandTex[i]));
    }
    sBandsReady = false;
}

/* Allocated on first use: a console that never opens the 3D slider never pays
 * the VRAM. A failure here is not fatal, it just keeps the direct path. */
static bool BandsCreate(void);

/* In the frame: whether the planes are there, asking for them if not. */
static bool BandsUsable(void)
{
    sBandsUsedFrame = sStats.frames;
    if (!sBandsReady) sBandsWanted = true;
    return sBandsReady;
}

/* Outside the frame only; see sBandsWanted. */
static bool BandsReady(void)
{
    sBandsUsedFrame = sStats.frames;
    if (sBandsReady) return true;
    if (sBandsFailed && sStats.frames < sBandsRetryFrame) return false;
    for (unsigned attempt = 0; attempt < 2 && !sBandsReady; ++attempt)
    {
        if (BandsCreate())
            sBandsReady = true;
        /*
         * The overworld keeps an atlas per tileset pair it has met, and a
         * 2D screen - a menu, a battle - is where that VRAM is wanted back:
         * the atlases of maps not on screen go, the current map's stay.
         */
#if CTR_VOXEL_ENABLED
        else if (attempt == 0 && CtrVoxel_ReleaseIdleVram() == 0)
            break;
#else
        else
            break;
#endif
    }
    if (!sBandsReady)
    {
        if (!sBandsFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for 3D depth planes (free=%lu); composing "
                         "per eye, retrying later", (unsigned long)vramSpaceFree());
        sBandsFailed = true;
        sBandsRetryFrame = sStats.frames + CTR_BANDS_RETRY_FRAMES;
        return false;
    }
    sBandsFailed = false;
    CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes ready (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    return true;
}

/* The three planes and their targets, or nothing at all. */
static bool BandsCreate(void)
{
    for (unsigned i = 0; i < CTR_BANDS; ++i)
    {
        /* 5551: the GBA's own 15 bits and the one bit of cover a plane
         * needs, at half the VRAM of RGBA8 - 768 KiB for all three, which an
         * Old 3DS has beside the overworld's arenas; 1.5 MiB it did not. */
        if (!C3D_TexInitVRAM(&sBandTex[i], 512, 256, GPU_RGBA5551)) goto fail;
        C3D_TexSetFilter(&sBandTex[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sBandTex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        /* No depth buffer: Citro2D draws in submission order, and clearing one
         * per plane per frame is memory traffic this path exists to avoid. */
        sBand[i] = C3D_RenderTargetCreateFromTex(&sBandTex[i], GPU_TEXFACE_2D, 0, -1);
        if (!sBand[i]) goto fail;
    }
    return true;
fail:
    BandsRelease();
    return false;
}

/*
 * Plane i holds priority i on its own, except the last, which holds every
 * remaining priority so that anything blending with what is beneath it keeps
 * that beneath in the same surface. The last plane therefore always reaches
 * priority 3, sits at the screen plane and carries the backdrop; the others
 * start transparent and are stacked on top of it.
 */
static unsigned BandMask(unsigned band, unsigned count)
{
    if (band + 1 < count) return 1u << band;
    return 15u & ~((1u << band) - 1);
}

static float BandDepth(unsigned band, unsigned count)
{
    return band + 1 < count ? (float)(CTR_PRIORITIES - 1 - band) : 0.0f;
}

/* Composes every depth plane into its own surface, with no displacement. */
static void RenderBands(unsigned count, uint32_t backdrop)
{
    sParallax = 0;
    for (int band = (int)count - 1; band >= 0; --band)
    {
        uint32_t before = sStats.tiles;

        sPriorityMask = BandMask(band, count);
        sLayerShift = 0;
        BlendForget();
        C2D_TargetClear(sBand[band], band + 1 == (int)count ? backdrop : 0);
        C2D_SceneBegin(sBand[band]);
        if (band + 1 == (int)count)
        {
            Blend(5, false, false);
            StageUnderlay();
        }
        if (!(Reg(0) & 128)) Compose();
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        /* The furthest plane carries the backdrop, so it is never empty. */
        sBandUsed[band] = sStats.tiles != before || band + 1 == (int)count;
    }
    sPriorityMask = 15;
    /* Rendering to a texture and then sampling it needs a command split. */
    C3D_FrameSplit(0);
}

/* Stacks the depth planes on one screen buffer, each displaced by its depth. */
static void BlitBands(C3D_RenderTarget *target, unsigned count, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};

    BlendForget();
    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(target);
    C2D_ViewReset();
    Blend(5, false, false);
    for (int band = (int)count - 1; band >= 0; --band)
        if (sBandUsed[band])
            C2D_DrawImageAt((C2D_Image){&sBandTex[band], &logical},
                            parallax * BandDepth(band, count), 0, 0, NULL, 1, 1);
    C2D_Flush();
}

#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
#if CTR_SHOW_FPS
/*
 * The FPS counter (SHOW_FPS=1): a 3x5 pixel font, each pixel a 1x1 solid
 * rectangle, over a translucent box in the top-left corner. Drawn last, over
 * whatever the frame composed, and at zero parallax in both eyes.
 */
#define FPS_PIXEL 1.0f
#define FPS_ADVANCE (4 * FPS_PIXEL)

static const char *const sFpsGlyphs[] =
{
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "111100111100100", /* F */
    "111101111100100", /* P */
};

static void DrawFps(C3D_RenderTarget *target)
{
    unsigned fps = (unsigned)(sStats.fps + 0.5f);
    unsigned glyphs[8], count = 0;
    char digits[4];
    int length = snprintf(digits, sizeof(digits), "%u", fps > 999 ? 999 : fps);

    glyphs[count++] = 10;   /* F */
    glyphs[count++] = 11;   /* P */
    glyphs[count++] = 5;    /* S, which is a 5 */
    glyphs[count++] = ~0u;  /* space */
    for (int i = 0; i < length; ++i)
        glyphs[count++] = (unsigned)(digits[i] - '0');

    /* Whatever path composed the frame, the counter draws with the 2D
     * program and no depth test, as RenderVoxel's UI pass does. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    C2D_SceneBegin(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(2, 2, 0, count * FPS_ADVANCE + FPS_PIXEL * 3, 5 * FPS_PIXEL + FPS_PIXEL * 4,
                      C2D_Color32(0, 0, 0, 160));
    for (unsigned i = 0; i < count; ++i)
    {
        if (glyphs[i] == ~0u) continue;
        const char *bits = sFpsGlyphs[glyphs[i]];
        float x = 2 + FPS_PIXEL * 2 + i * FPS_ADVANCE, y = 2 + FPS_PIXEL * 2;
        for (unsigned p = 0; p < 15; ++p)
            if (bits[p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * FPS_PIXEL, y + (p / 3) * FPS_PIXEL, 0,
                                  FPS_PIXEL, FPS_PIXEL, C2D_Color32(255, 255, 255, 255));
    }
    C2D_Flush();
}
#endif

/*
 * The bottom screen runs some of the game's menus without showing them (the
 * party menu, the bag): while it does, the top screen keeps the last frame it
 * presented, the world as it was when the menu opened. Nothing is rendered;
 * the frame is only paced to the display.
 */
static bool sHoldTop;

void CtrVideo_HoldTop(bool hold)
{
    if (hold != sHoldTop)
        CtrLog_Write(CTR_LOG_VIDEO, "top screen %s at frame %lu", hold ? "held" : "released",
                     (unsigned long)sStats.frames);
    sHoldTop = hold;
}

void CtrVideo_Present(void)
{
    if (!sMemory.regs) CtrPlatform_Fatal("VIDEO has no logical memory bound");
    /* The PokéNav is drawn on the bottom screen, whether or not the top is held. */
    bool bottom = BottomReady(sCentredRequested == CTR_CENTRED_POKENAV && !sStageRequested);
    sBottomInUse = bottom;
    if (sHoldTop && !bottom)
    {
        gspWaitForVBlank();
        ++sStats.frames;
        return;
    }
    /* Wait for previous GPU work before editing the atlas. C3D owns VBlank
     * pacing and swap: no gfxSwapBuffers/gspWaitForVBlank in this path. */
    /* Outside the frame, where deleting a render target may wait for the GPU. */
    if (sBandsReady && (sPlaneReleaseAsked || sStats.frames - sBandsUsedFrame > CTR_BANDS_IDLE_FRAMES))
    {
        BandsRelease();
        CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes released (VRAM free=%lu)",
                     (unsigned long)vramSpaceFree());
    }
    sPlaneReleaseAsked = false;
    if (sLeavesTex[0].data && sStats.frames - sLeavesUsed > 120) LeavesRelease();
    if (sBandsWanted)
    {
        sBandsWanted = false;
        BandsReady();
    }
    /*
     * The view of this frame. The voxel overworld is never a stage, so a stage
     * only waits on the voxel decision in the unlikely case both are asked.
     */
    sStage = sStageRequested;
    sCentred = sCentredRequested != CTR_CENTRED_NONE && !sStage;
    sCentredScreen = sCentred ? sCentredRequested : CTR_CENTRED_NONE;
    sBattle = sBattleRequested && !sStage && !sCentred;
    sZoom = sBattle ? CTR_BATTLE_ZOOM : 1.0f;
    /* GBA (120, 112) - the middle of the scene's bottom edge - on screen (200, 192). */
    sOffX = sBattle ? CTR_GAME_WIDTH / 2 - 120 * sZoom : 0.0f;
    sOffY = sBattle ? CTR_GAME_HEIGHT - 48 - 112 * sZoom : 0.0f;
    sShiftZoom = sZoom;
    if (sStage || sCentred)
    {
        sViewX = CTR_STAGE_X;
        /* The PokeNav is on the bottom screen, laid out band by band from
         * its top edge (NavCompose). */
        sViewY = sCentredScreen == CTR_CENTRED_POKENAV ? 0 : CTR_STAGE_Y;
    }
    else if (sBattle)
    {
        sViewX = sViewY = 0;
    }
    else
    {
        sViewX = sViewY = 0;
    }
    ClipToView();
    /* The voxel overworld draws the field itself when it is switched on. */
    sFieldLayers = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld() && !CtrSettings_Voxel();
    LayersPrepare();
    ScenePrepare();
    uint64_t waitStart = svcGetSystemTick();
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    uint64_t start = svcGetSystemTick();
    sStats.waitMs = (start - waitStart) * 1000.0 / SYSCLOCK_ARM11;
#if CTR_VOXEL_ENABLED
    /*
     * FrameBegin returns on a VBlank, so the time between two returns is a
     * whole number of display frames: two of them is a frame the screen
     * showed twice. Logged with what the frame before it spent - the present
     * (and the voxel builds in it), then the game and its VBlank handler - so
     * that a slow renderer can be told from a slow game on hardware.
     */
    {
        static uint64_t sLastBegin;
        static unsigned sDropsLogged;
        /* A screen that runs at 30 for a while is one finding, not a line per
         * frame: a steady run is logged once a second with how many frames it
         * dropped, and only a hitch of three frames or more is always logged. */
        static uint32_t sLastDropLogged;
        static unsigned sDropsQuiet;
        float gap = sLastBegin ? (start - sLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;

        if (gap > 24.0f && sStats.frames > 120 && sDropsLogged < 3000)
        {
            if (gap < 45.0f && sStats.frames - sLastDropLogged < 60)
                ++sDropsQuiet;
            else
            {
                const CtrTiming *timing = CtrPlatform_GetTiming();
                const CtrVoxelStats *voxel = CtrVoxel_GetStats();

                ++sDropsLogged;
                sLastDropLogged = sStats.frames;
                /* The voxel figures are the last voxel frame's: on a 2D frame they
                 * are stale, and present= alone is the 2D compositor. */
                CtrLog_Write(CTR_LOG_VIDEO, "DROP frame=%lu gap=%.1fms (+%u quiet): present=%.1f "
                             "(voxel=%.1f: world=%.1f atlas=%.1f chunks=%.1f sprites=%.1f) "
                             "game=%.1f audio+vblank=%.1f",
                             (unsigned long)sStats.frames, gap, sDropsQuiet, sStats.cpuMs,
                             voxel->updateMs, voxel->worldMs, voxel->atlasMs,
                             voxel->meshMs - voxel->atlasMs, voxel->spritesMs,
                             timing->gameMs, timing->vblankMs);
                sDropsQuiet = 0;
            }
        }
        sLastBegin = start;
    }
#endif
    sStats.tiles = sStats.uploads = sStats.sprites = 0;
    sBgTicks = sObjTicks = 0;
    sStats.display = Reg(0);
    if (sUsed > CACHE_COUNT - 4096) { memset(sHash, 0, sizeof(sHash)); sUsed = 0; }
    UpdatePalette();
    if (sStage || sBattle) RecordScroll();
    uint16_t backdrop = (Reg(0) & 128) ? 0x7fff : sPalette[0];
    uint32_t rgb = CtrVideo_RGBA8(backdrop, true);
    uint32_t clear = C2D_Color32(rgb >> 24, rgb >> 16, rgb >> 8, 255);
    /*
     * The 3D slider decides the separation, and at zero the right eye is not
     * composed at all: with 3D off this is the same single pass as before.
     * Whole pixels only, so every layer stays on the pixel grid in both eyes.
     */
    bool field = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld();
#if CTR_VOXEL_ENABLED
    /*
     * Preparing the voxel frame is part of the decision. If the atlas or the
     * mesh could not be built this frame, the 2D compositor draws it: leaving
     * the previous map's geometry on screen would show the wrong place.
     */
    /* The overworld, drawn in 3D or - while its first atlas or mesh is still
     * being made - by the 2D compositor. Either way it never takes the depth
     * planes: holding them there is what kept the overworld's atlas out of
     * VRAM for good, the 2D picture standing in for it frame after frame. */
    /* Opt-in from the bottom screen's options (CtrSettings_Voxel). */
    bool overworld = field && CtrSettings_Voxel() && CtrVoxel_IsAvailable();
    bool voxel = overworld && CtrVoxel_Update();
    /* A new map still being made - a frame or two, behind the fade - is
     * black rather than the 2D picture flashing up before the 3D one. */
    bool blank = overworld && !voxel && CtrVoxel_IsWarmingUp();
#else
    const bool voxel = false, overworld = false, blank = false;
#endif
    /* The 2D field centres its text windows as the voxel overlay does. */
    sFieldUi = field && !voxel;
    float slider = osGet3DSliderState();
    /* Real stereoscopy for the voxel world is V8; the layer parallax of the
     * 2D path means nothing for a 3D scene, so it stays off there. */
    bool stereo = !voxel && !blank && sTopRight && slider > 0.0f
                  && roundf(slider * CTR_STEREO_PIXELS) > 0.0f;
    /* A 2D screen composed per eye walks every layer twice, which on an Old
     * 3DS is 30 fps in a menu. Without its planes it stays flat until they
     * can be made (before the next frame, see sBandsWanted). */
    /* The battle scene is two passes of its own (RenderBattleScene), and cheap
     * enough to be composed per eye. */
    bool planes = stereo && !overworld && !sStage && !sBattle && BandsUsable();
    if (stereo && !overworld && !sStage && !sBattle && !planes) stereo = false;
    if (bottom) stereo = planes = false;
    if (stereo != sStereo) { gfxSet3D(stereo); sStereo = stereo; }
    sStats.stereo = stereo ? roundf(slider * CTR_STEREO_PIXELS) : 0;
    if (!voxel && !blank) LayersRender();

    if (bottom)
    {
        /* The top screen is not drawn: it keeps the frame it showed last. */
        sPlanes = 0;
        RenderEye(sBottom, clear, 0.0f);
    }
    else if (voxel)
    {
#if CTR_VOXEL_ENABLED
        sPlanes = 0;
        RenderVoxel(clear);
#endif
    }
    else if (blank)
    {
        sPlanes = 0;
        C2D_TargetClear(sTop, C2D_Color32(0, 0, 0, 255));
    }
    else if (!stereo)
    {
        sPlanes = 0;
        RenderEye(sTop, clear, 0.0f);
    }
    else if (planes)
    {
        sPlanes = DepthPlanes();
        RenderBands(sPlanes, clear);
        BlitBands(sTop, sPlanes, sStats.stereo);
        BlitBands(sTopRight, sPlanes, -(float)sStats.stereo);
    }
    else
    {
        /* A stage, drawn from its layer textures, costs little enough to be
         * composed per eye; otherwise only without memory for the planes. */
        sPlanes = 0;
        RenderEye(sTop, clear, sStats.stereo);
        RenderEye(sTopRight, clear, -(float)sStats.stereo);
    }
#if CTR_SHOW_FPS
    if (!bottom) DrawFps(sTop);
    if (stereo) DrawFps(sTopRight);
#endif
    C3D_FrameEnd(0);
    if (bottom) BottomTransfer();
    ++sStats.frames;
    ++sFpsFrames;
    sStats.cpuMs = (svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11;
    sStats.gpuMs = C3D_GetDrawingTime();
    uint64_t now = CtrPlatform_Milliseconds();
    if (now - sFpsStart >= 1000)
    {
        sStats.fps = sFpsFrames * 1000.0f / (now - sFpsStart);
        sFpsStart = now;
        sFpsFrames = 0;
    }
    if (sStats.frames % 600 == 0)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "frames=%lu fps=%.1f cpu=%.2fms bg=%.2fms obj=%.2fms gpu=%.2fms "
                     "3d=%lupx%s x%lu quads=%lu sprites=%lu errors=%lu cache=%u linear=%lu vram=%lu",
                     (unsigned long)sStats.frames, sStats.fps, sStats.cpuMs,
                     sBgTicks * 1000.0 / SYSCLOCK_ARM11, sObjTicks * 1000.0 / SYSCLOCK_ARM11,
                     sStats.gpuMs, (unsigned long)sStats.stereo,
                     sStats.stereo ? (sPlanes ? "/planes" : "/eyes") : "", (unsigned long)sPlanes,
                     (unsigned long)sStats.tiles, (unsigned long)sStats.sprites,
                     (unsigned long)sStats.errors, sUsed,
                     (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());
#if CTR_VOXEL_ENABLED
        if (voxel)
        {
            const CtrVoxelStats *stats = CtrVoxel_GetStats();
            CtrLog_Write(CTR_LOG_VIDEO,
                         "VOXEL chunks=%u/%u missing=%u pending=%u builds=%u atlas=%u verts=%u "
                         "mesh=%.2fms peak=%.2fms update=%.2fms peak=%.2fms dropped=%u "
                         "anim=%u/%u refl=%u",
                         stats->visibleChunks, stats->chunks, stats->chunksMissing,
                         stats->pendingBuilds, stats->meshRebuilds, stats->atlasRebuilds,
                         stats->vertices, stats->meshMs, stats->meshPeakMs,
                         stats->updateMs, stats->updatePeakMs, stats->dropped,
                         stats->animationUploads, stats->animatedMetatiles,
                         stats->reflections);
        }
#endif
    }
}

const CtrVideoStats *CtrVideo_GetStats(void) { return &sStats; }

void CtrVideo_SetStage(bool stage) { sStageRequested = stage; }
void CtrVideo_SetCentred(unsigned screen) { sCentredRequested = screen < CTR_CENTRED_SCREENS ? screen : CTR_CENTRED_NONE; }
void CtrVideo_SetBattle(bool battle) { sBattleRequested = battle; }

void CtrVideo_SetLineScroll(unsigned reg, bool wide, const void *values, unsigned lines)
{
    const uint16_t *source = values;
    unsigned regs = wide ? 2 : 1;

    sLineMask = 0;
    sLineCount = 0;
    if (!values || reg + regs * 2 > 16) return;
    if (lines > LINE_MAX) lines = LINE_MAX;
    /*
     * The first line gets the register the VBlank handler wrote from entry 0.
     * The DMA then fires at the end of every line, so line y shows entry y-1.
     */
    for (unsigned r = 0; r < regs; ++r)
        for (unsigned y = 0; y < lines; ++y)
            sLineScroll[reg / 2 + r][y] = source[(y ? y - 1 : 0) * regs + r];
    sLineMask = ((1u << regs) - 1) << (reg / 2);
    sLineCount = lines;
}

void CtrVideo_Shutdown(void)
{
    BottomRelease();
#if CTR_VOXEL_ENABLED
    CtrVoxel_Shutdown();
#endif
    LayersRelease();
    SceneRelease();
    /* Target deletion waits for queued GPU work before buffers are freed. */
    if (sLogical) C3D_RenderTargetDelete(sLogical);
    if (sTop) C3D_RenderTargetDelete(sTop);
    if (sTopRight) C3D_RenderTargetDelete(sTopRight);
    BandsRelease();
    sBandsFailed = false;
    if (sC2d) C2D_Fini();
    if (sSurface.data) C3D_TexDelete(&sSurface);
    LeavesRelease();
    if (sAtlas.data) C3D_TexDelete(&sAtlas);
    if (sC3d) C3D_Fini();
    sLogical = sTop = sTopRight = NULL;
    sC2d = sC3d = false;
    memset(&sSurface, 0, sizeof(sSurface));
    memset(&sAtlas, 0, sizeof(sAtlas));
}
