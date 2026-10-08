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
#include "3ds_bottom.h"
#include "../compat/port_prof.h"

/* Public queue passed by libctru, never a cast of Citro3D's private context.
 * Uploads reserve worst-case split+copy entries and leave the compositor's
 * clears, postprocessing, battle layers and final transfers room to finish. */
static gxCmdQueue_s *sFrameQueue;
static unsigned sUploadCommands, sRenderReserve;
void __real_GX_BindQueue(gxCmdQueue_s *queue);
void __wrap_GX_BindQueue(gxCmdQueue_s *queue)
{
    sFrameQueue = queue;
    __real_GX_BindQueue(queue);
}

extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
static bool sGpuEarly;
static uint64_t sGpuEarlyStart;

static uint64_t sFlushTicks;
static unsigned sFlushes;

/*
 * Every data cache flush - ours, Citro3D's at FrameEnd, the voxel uploads' -
 * goes through GSPGPU_FlushDataCache, a request to the GSP service, which
 * runs on the system core and makes the same kernel call for this process.
 * The request is a round trip to the other core per flush (0.6 ms each for
 * the whole heap on an Old 3DS, three a frame with the GPU started early).
 * The kernel call made here does the same work, clean and invalidate, with
 * no service in between. Should the kernel refuse it, the service is used
 * from then on. For the whole heap the two are timed against each other over
 * the first FLUSH_TRIAL flushes of each and the faster kept, logged once.
 */
Result __real_GSPGPU_FlushDataCache(const void *adr, u32 size);
static int sFlushDirect = 1;        /* 0: the kernel refused the call */
static int sFlushChoice = -1;       /* whole heap: -1 still timing, 0 service, 1 direct */
static uint64_t sFlushTrial[2];
static unsigned sFlushTrials[2];
#define FLUSH_TRIAL 120u
#define FLUSH_LARGE (1u << 20)

static Result FlushDirect(const void *adr, u32 size)
{
    Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)adr, size);

    if (R_FAILED(rc))
    {
        sFlushDirect = 0;
        CtrLog_Write(CTR_LOG_VIDEO, "cache flush: kernel call refused (%08lx), GSP service used",
                     (unsigned long)rc);
        return __real_GSPGPU_FlushDataCache(adr, size);
    }
    return rc;
}

Result __wrap_GSPGPU_FlushDataCache(const void *adr, u32 size)
{
    uint64_t start;
    Result rc;
    int way;

    if (sFlushDirect == 0)
        return __real_GSPGPU_FlushDataCache(adr, size);
    if (size < FLUSH_LARGE)
        return FlushDirect(adr, size);
    way = sFlushChoice >= 0 ? sFlushChoice : (int)((sFlushTrials[0] + sFlushTrials[1]) & 1);
    start = svcGetSystemTick();
    rc = way ? FlushDirect(adr, size) : __real_GSPGPU_FlushDataCache(adr, size);
    if (sFlushChoice < 0 && sFlushDirect != 0)
    {
        sFlushTrial[way] += svcGetSystemTick() - start;
        if (++sFlushTrials[way] >= FLUSH_TRIAL && sFlushTrials[!way] >= FLUSH_TRIAL)
        {
            float service = sFlushTrial[0] * 1000.0f / SYSCLOCK_ARM11 / sFlushTrials[0];
            float direct = sFlushTrial[1] * 1000.0f / SYSCLOCK_ARM11 / sFlushTrials[1];

            sFlushChoice = direct <= service;
            CtrLog_Write(CTR_LOG_VIDEO, "cache flush of the heap: GSP service %.3f ms, kernel call %.3f ms; "
                         "%s kept", service, direct, sFlushChoice ? "kernel call" : "GSP service");
        }
    }
    return rc;
}

static void FlushLinear(void)
{
    uint64_t start = svcGetSystemTick();

    GSPGPU_FlushDataCache((void *)__ctru_linear_heap, __ctru_linear_heap_size);
    sFlushTicks += svcGetSystemTick() - start;
    ++sFlushes;
}

Result __real_GX_ProcessCommandList(u32 *buf0a, u32 buf0s, u8 flags);
Result __wrap_GX_ProcessCommandList(u32 *buf0a, u32 buf0s, u8 flags)
{
    if (sGpuEarly) FlushLinear();
    return __real_GX_ProcessCommandList(buf0a, buf0s, flags);
}

Result __real_GX_TextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags);
Result __wrap_GX_TextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags)
{
    if (sGpuEarly) FlushLinear();
    return __real_GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
}

Result __real_GX_DisplayTransfer(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 flags);
Result __wrap_GX_DisplayTransfer(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 flags)
{
    /* Screens and render targets: VRAM the GPU wrote, nothing to flush. */
    return __real_GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
}

Result __real_GX_RequestDma(u32 *src, u32 *dst, u32 length);
Result __wrap_GX_RequestDma(u32 *src, u32 *dst, u32 length)
{
    if (sGpuEarly) FlushLinear();
    return __real_GX_RequestDma(src, dst, length);
}

/* In the frame: queue what is recorded so far and start the GPU on it. */
#ifndef CTR_GPU_EARLY
#define CTR_GPU_EARLY 1
#endif
/* Measured on an Old 3DS by turning it off every other 600 frames: the
 * voxel battle 59.8 fps with it, 48-52 without; the 2D battle no worse. */
static bool GpuEarlyThisFrame(void)
{
    return CTR_GPU_EARLY;
}

static void GpuStartEarly(void)
{
    if (!GpuEarlyThisFrame() || sGpuEarly || sFrameQueue == NULL) return;
    C2D_Flush();
    C3D_FrameSplit(0);
    FlushLinear();
    sGpuEarly = true;
    sGpuEarlyStart = svcGetSystemTick();
    gxCmdQueueRun(sFrameQueue);
}

static bool GpuFinishEarly(void)
{
    if (!sGpuEarly) return false;
    (void)sGpuEarlyStart;
    if (sFrameQueue->lastEntry == sFrameQueue->numEntries)
    {
        gxCmdQueueStop(sFrameQueue);
        sGpuEarly = false;
        return false;
    }
    return true;
}

bool CtrVideo_TryVoxelUpload(void)
{
    if (sFrameQueue == NULL)
        return false;
    unsigned capacity = sFrameQueue->maxEntries;
    unsigned used = sFrameQueue->numEntries;

    if (used + 2u + sRenderReserve > capacity
     || sUploadCommands + 2u + sRenderReserve > capacity)
        return false;
    sUploadCommands += 2u;
    return true;
}

unsigned CtrVideo_VoxelUploadsLeft(void)
{
    unsigned capacity, used, room;

    if (sFrameQueue == NULL)
        return 0;
    capacity = sFrameQueue->maxEntries;
    used = sFrameQueue->numEntries;
    if (used < sUploadCommands)
        used = sUploadCommands;
    room = capacity > used + sRenderReserve ? capacity - used - sRenderReserve : 0;
    return room / 2u;
}

/* The voxel overworld replaces this compositor's output while the player is
 * walking around; every other game state keeps the 2D path below. */
#ifndef CTR_VOXEL_ENABLED
#define CTR_VOXEL_ENABLED 0
#endif
#if CTR_VOXEL_ENABLED
#include "voxel/ctr_voxel.h"
#include "voxel/voxel_battle.h"
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
/*
 * A battle in front of the voxel world (RenderBattleWorld): this frame's
 * scenery is the world, and the layers it stands in for are left out of the
 * battle's own picture - BG3, its scenery, and while the intro slides that
 * in, the entry picture on BG1 and BG2.
 */
static bool sBattleWorld;
static unsigned sWorldLayers;
/* battle_bg.c: BG3 holds a move's background rather than the scenery.
 * battle_intro.c: the intro is sliding the scenery in. */
unsigned char CtrBattleBg_MoveBgShown(void);
unsigned char CtrBattleIntro_Sliding(void);
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
/* Citro2D quads a frame. Layer cells, the bulk of them once, have their own
 * buffer (FastCells), which takes the linear memory this used to. */
#define MAX_DRAWS 12288

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
/*
 * Which sprites the voxel view draws over itself: the weather, before its
 * text layer, and then the sprites placed on the screen rather than on the
 * map (sScreenOam), which on the GBA sit over the text layer: the mon shown
 * for a field move over its banner, Fly's bird and its rider. The map's own
 * sprites are cards in the 3D scene (voxel_entities.c).
 */
enum { VOXEL_OBJ_NONE, VOXEL_OBJ_WEATHER, VOXEL_OBJ_SCREEN };
static unsigned sVoxelObjPass = VOXEL_OBJ_NONE;

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

/* OAM entries of sprites placed on the screen rather than on the map. */
static uint32_t sScreenOam[4];
static uint32_t sMachineOam[4];

void CtrVideo_ClearScreenOam(void)
{
    memset(sScreenOam, 0, sizeof(sScreenOam));
    memset(sMachineOam, 0, sizeof(sMachineOam));
}

void CtrVideo_MarkScreenOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sScreenOam[i >> 5] |= 1u << (i & 31);
}

/* Screen sprites that belong to the map's picture around the player. */
void CtrVideo_MarkMachineOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sMachineOam[i >> 5] |= 1u << (i & 31);
}

/* OAM entries of the fog's sprites (CtrVideo_MarkFogOam). */
static uint32_t sFogOam[4];

void CtrVideo_ClearFogOam(void)
{
    memset(sFogOam, 0, sizeof(sFogOam));
}

void CtrVideo_MarkFogOam(unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        sFogOam[i >> 5] |= 1u << (i & 31);
}

/* A battle transition's sprite: on the screen, and not the weather's. */
static bool TransitionOam(unsigned i)
{
    bool screen = (sScreenOam[i >> 5] & (1u << (i & 31))) != 0;
#if CTR_VOXEL_ENABLED
    if (sVoxelWeatherOam[i >> 5] & (1u << (i & 31))) return false;
#endif
    return screen;
}

/* Which sprites a pass draws: all, only a transition's, or all but those. */
enum { OBJ_ALL, OBJ_TRANSITION, OBJ_FIELD };
static unsigned sObjFilter = OBJ_ALL;

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
/* The voxel bloom's quarter-size target (VoxelBloomPrepare); absent, no bloom. */
static C3D_Tex sBloomTex;
static C3D_RenderTarget *sBloom;
static C3D_RenderTarget *sLogical, *sTop, *sTopRight;
static bool sStereo;
static Tile sTiles[CACHE_COUNT];
static uint16_t sHash[HASH_COUNT];
static uint16_t sPalette[512];
static uint16_t sTexturePalette[512];
/* Background colours shown transparent, a bit per colour of each BG
 * palette (KeySectionColours). */
static uint16_t sKeyed[16];
static uint8_t sMorton[64];
static C2D_ImageTint sTint;
static uint32_t sPaletteVersion[34];
static uint32_t sPaletteChanges[34][8];
static unsigned sUsed;

/*
 * What changed in the logical VRAM, a kilobyte at a time. The game writes VRAM
 * in many ways (copies, DMA, decompression, plain pointers), so instead of
 * hooking them all the frame's VRAM is compared with a copy of the last one
 * before anything reads it: 96 KiB, against walking every cell of every layer
 * and comparing every tile drawn with its bytes, frame after frame, when
 * nothing changed. sVramStamp[b] is the frame token (sStats.frames + 1, as
 * Tile.checked) at which block b last changed; something checked at token t
 * still holds while the stamps of its blocks are <= t.
 */
#define VRAM_BYTES 0x18000
#define VRAM_BLOCK 1024
static uint32_t sVramShadow[VRAM_BYTES / 4];
static uint32_t sVramStamp[VRAM_BYTES / VRAM_BLOCK];
/* The token at which a background palette last changed. */
static uint32_t sBgPaletteStamp;
/* Bumped when the tile cache is emptied: every slot changes meaning. */
static uint32_t sCacheGeneration;

/* Eight words at a time, one branch for all eight: memcmp's byte loop cost
 * a millisecond a frame here. */
static bool BlockDiffers(const uint32_t *a, const uint32_t *b)
{
    for (unsigned i = 0; i < VRAM_BLOCK / 4; i += 8)
        if ((a[i] ^ b[i]) | (a[i + 1] ^ b[i + 1]) | (a[i + 2] ^ b[i + 2]) | (a[i + 3] ^ b[i + 3])
            | (a[i + 4] ^ b[i + 4]) | (a[i + 5] ^ b[i + 5]) | (a[i + 6] ^ b[i + 6]) | (a[i + 7] ^ b[i + 7]))
            return true;
    return false;
}

static void TrackVram(void)
{
    uint32_t now = sStats.frames + 1;
    const uint32_t *vram = (const uint32_t *)sMemory.vram;

    for (unsigned b = 0; b < VRAM_BYTES / VRAM_BLOCK; ++b)
    {
        const uint32_t *block = vram + b * (VRAM_BLOCK / 4);
        uint32_t *shadow = sVramShadow + b * (VRAM_BLOCK / 4);

        if (BlockDiffers(block, shadow))
        {
            memcpy(shadow, block, VRAM_BLOCK);
            sVramStamp[b] = now;
        }
    }
}

/* Whether VRAM [address, address + bytes) changed after token t. */
static bool VramChangedAfter(unsigned address, unsigned bytes, uint32_t t)
{
    unsigned last;

    if (bytes == 0) return false;
    if (address >= VRAM_BYTES) return true;
    if (bytes > VRAM_BYTES - address) bytes = VRAM_BYTES - address;
    last = (address + bytes - 1) / VRAM_BLOCK;
    for (unsigned b = address / VRAM_BLOCK; b <= last; ++b)
        if (sVramStamp[b] > t) return true;
    return false;
}
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
/*
 * The banner of a field move (src/field_effect.c, the mon shown for Surf, Cut,
 * Fly...) is BG0 too, but a pattern of streaks meant to wrap across the whole
 * screen as on the GBA, not a window: while it is up BG0 is drawn over the
 * full width and not moved to the text band.
 */
static bool sFieldBanner;

void CtrVideo_SetFieldBanner(bool banner)
{
    sFieldBanner = banner;
}
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
 * A layer that blends with what is underneath it needs that underneath in its
 * own surface: its plane is given the picture behind it, colour only, before
 * its layers are drawn (RenderBands). The last plane holds every remaining
 * priority.
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
/* How many of them exist: two when the third gave its VRAM to a layer. */
static unsigned sBandCount;
static bool sBandsFailed;
/* A failed allocation is tried again this many frames later, not never: the
 * VRAM it needs may have been in use by the overworld only for a while. */
#define CTR_BANDS_RETRY_FRAMES 300u
static uint32_t sBandsRetryFrame;
/* Whether each plane drew anything; an empty one is not worth a quad. */
static bool sBandUsed[CTR_BANDS];
/* Depth planes the last frame used, 0 when it was composed per eye. */
static unsigned sPlanes;
/*
 * What the current composition pass may draw, by depth slot: bit 2p is the
 * sprites of priority p, bit 2p+1 its backgrounds - front to back in that
 * order, as the GBA stacks them.
 */
#define SLOTS_ALL 255u
#define SLOT_OBJ(p) (1u << ((p) * 2))
#define SLOT_BG(p) (2u << ((p) * 2))
static unsigned sPriorityMask = SLOTS_ALL;

/*
 * Last blend configuration submitted. Reset whenever something else may have
 * changed the GPU state, which is once per eye pass.
 */
static unsigned sBlendKey = ~0u;
static void BlendForget(void) { sBlendKey = ~0u; }

/*
 * A battle transition's frame, line by line (CtrVideo_SetLineRegisters): the
 * registers each of its 160 lines shows, and while a band of those lines is
 * composed, that band's values in place of the registers (Reg).
 */
static bool sTransitionRequested, sTransition;
/* Composing the transition's own picture: GBA geometry (RenderTransition). */
static bool sTransitionCompose;
static const uint16_t (*sLineRegs)[CTR_LINE_REGS];
static const uint16_t *sRegLine;

static unsigned Reg(unsigned offset)
{
    if (sRegLine && offset - CTR_LINE_REG_FIRST < CTR_LINE_REGS * 2)
        return sRegLine[(offset - CTR_LINE_REG_FIRST) / 2];
    return sMemory.regs[offset / 2];
}
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

/*
 * A palette fade as a tint. The game fades by rewriting every colour each
 * frame (BlendPalette, src/util.c), and every colour of a layer texture
 * changing is every cell of it redrawn: the intro's four 256x512 layers,
 * 8192 tiles decoded and drawn a frame, 30 ms on an Old 3DS for as long as a
 * fade lasts. When the backgrounds' palette is exactly the unfaded one faded
 * towards one colour - black, white, the grey a battle transition flashes
 * (BlendPalettes towards RGB(11, 11, 11)) - the tiles keep the unfaded
 * colours and the fade is
 * drawn as a tint on the layers (Blend, LayerBrightness), which costs
 * nothing. Only an exact match counts, so any other palette effect is drawn
 * as before. Sprites keep the faded colours: they are a few tiles.
 */
extern uint16_t gPlttBufferUnfaded[];
static float sPaletteFade;
/* The colour it fades towards, GBA BGR555. */
static uint16_t sPaletteFadeColor;
/* Set while a layer texture is drawn: it holds the unfaded colours. */
static bool sInLayerTexture;

static uint16_t FadeColor(unsigned color, unsigned to, unsigned y)
{
    int r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
    int tr = to & 31, tg = (to >> 5) & 31, tb = (to >> 10) & 31;

    r += ((tr - r) * (int)y) >> 4;
    g += ((tg - g) * (int)y) >> 4;
    b += ((tb - b) * (int)y) >> 4;
    return (uint16_t)(r | g << 5 | b << 10);
}

/*
 * The value (0-31) one channel of every background colour is faded towards
 * by y, or -1. Tried value by value: a wrong one fails on the first colours,
 * so this is cheap unless it matches.
 */
static int FadeChannelTarget(const uint16_t *now, const uint16_t *unfaded, unsigned shift, unsigned y)
{
    for (int to = 0; to < 32; ++to)
    {
        unsigned i = 0;

        for (; i < 256; ++i)
        {
            int u = (unfaded[i] >> shift) & 31, f = (now[i] >> shift) & 31;
            if (u + (((to - u) * (int)y) >> 4) != f) break;
        }
        if (i == 256) return to;
    }
    return -1;
}

/* The coefficient (1-16) the background colours are faded by, or 0. */
static unsigned DetectPaletteFade(uint16_t *color)
{
    const uint16_t *now = sMemory.palette, *unfaded = gPlttBufferUnfaded;

    if (!memcmp(now, unfaded, 512)) return 0;
    for (unsigned w = 0; w < 2; ++w)
        for (unsigned y = 1; y <= 16; ++y)
        {
            unsigned i = 0;

            while (i < 256 && FadeColor(unfaded[i], w ? 0x7fff : 0, y) == (now[i] & 0x7fff)) ++i;
            if (i == 256)
            {
                *color = w ? 0x7fff : 0;
                return y;
            }
        }
    /* Any other colour, channel by channel: the transitions' grey flash. */
    for (unsigned y = 1; y <= 16; ++y)
    {
        int r = FadeChannelTarget(now, unfaded, 0, y), g, b;

        if (r < 0 || (g = FadeChannelTarget(now, unfaded, 5, y)) < 0
            || (b = FadeChannelTarget(now, unfaded, 10, y)) < 0)
            continue;
        *color = (uint16_t)(r | g << 5 | b << 10);
        return y;
    }
    return 0;
}

static void UpdatePalette(void)
{
    bool bgChanged = false, objChanged = false;
    unsigned fade = DetectPaletteFade(&sPaletteFadeColor);
    const uint16_t *bgSource = fade ? gPlttBufferUnfaded : sMemory.palette;

    sPaletteFade = fade / 16.0f;
    for (unsigned bank = 0; bank < 32; ++bank)
    {
        const uint16_t *source = bank < 16 ? bgSource : sMemory.palette;

        if (memcmp(sPalette + bank * 16, source + bank * 16, 32))
        {
            unsigned group = bank < 16 ? 32 : 33;
            if (bank < 16 ? !bgChanged : !objChanged)
                memset(sPaletteChanges[group], 0, sizeof(sPaletteChanges[group]));
            memset(sPaletteChanges[bank], 0, sizeof(sPaletteChanges[bank]));
            for (unsigned index = 0; index < 16; ++index)
            {
                unsigned p = bank * 16 + index;
                if (sPalette[p] == source[p]) continue;
                sPalette[p] = source[p];
                sTexturePalette[p] = bank < 16 && ((sKeyed[bank] >> index) & 1) ? 0
                                                                                 : CtrVideo_RGBA5551(sPalette[p]);
                sPaletteChanges[bank][0] |= 1u << index;
                unsigned entry = p & 255;
                sPaletteChanges[group][entry / 32] |= 1u << (entry & 31);
            }
            ++sPaletteVersion[bank];
            if (bank < 16) bgChanged = true; else objChanged = true;
        }
    }
    if (bgChanged)
    {
        ++sPaletteVersion[32];
        sBgPaletteStamp = sStats.frames + 1;
    }
    if (objChanged) ++sPaletteVersion[33];
}

/*
 * The party menu's and the bag's own backgrounds - the party menu's olive and
 * striped panel (BG1, palette 1, colours 3-5), the bag's stripes (BG2,
 * palettes 0 and 1, colours 12 and 13) - give way on the bottom screen to its
 * light green, drawn behind them (DrawSectionBackdrop): while one of those
 * screens is there, those colours are transparent. Only those layers use
 * those palettes; their panels, slots and buttons keep every other colour.
 */
static void KeySectionColours(void)
{
    uint16_t want[16] = {0};

    if (sCentredScreen == CTR_CENTRED_PARTY || sCentredScreen == CTR_CENTRED_PARTY_WHOLE)
        want[1] = (1u << 3) | (1u << 4) | (1u << 5);
    else if (sCentredScreen == CTR_CENTRED_BAG || sCentredScreen == CTR_CENTRED_BAG_WHOLE)
        want[0] = want[1] = (1u << 12) | (1u << 13);
    for (unsigned bank = 0; bank < 16; ++bank)
    {
        uint16_t changed = want[bank] ^ sKeyed[bank];

        if (!changed)
            continue;
        sKeyed[bank] = want[bank];
        for (unsigned index = 0; index < 16; ++index)
        {
            unsigned p = bank * 16 + index;

            if ((changed >> index) & 1)
                sTexturePalette[p] = ((want[bank] >> index) & 1) ? 0 : CtrVideo_RGBA5551(sPalette[p]);
        }
        /* A version on from whatever this frame's palette update made of
         * it: its tiles are rebuilt, and the layers that hold them walked. */
        memset(sPaletteChanges[bank], 0, sizeof(sPaletteChanges[bank]));
        sPaletteChanges[bank][0] = changed;
        memset(sPaletteChanges[32], 0xff, sizeof(sPaletteChanges[32]));
        ++sPaletteVersion[bank];
        ++sPaletteVersion[32];
        sBgPaletteStamp = sStats.frames + 1;
    }
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
        /* A tile's bytes sit in one block: unchanged since it was last
         * checked, they need no comparing. */
        if (!tile->valid || TilePaletteChanged(tile, paletteId)
            || (sVramStamp[address / VRAM_BLOCK] > tile->checked
                && memcmp(tile->bytes, sMemory.vram + address, bytes)))
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
    /* The palette fade (UpdatePalette) is on a background whatever the
     * windows say; the brightness effect of BLDY replaces it below. */
    bool paletteTint = sPaletteFade > 0 && !sInLayerTexture && (layer < 4 || layer == 5);
    uint32_t fadeTo = CtrVideo_RGBA8(sPaletteFadeColor, true);

    C2D_PlainImageTint(&sTint, C2D_Color32(fadeTo >> 24, fadeTo >> 16, fadeTo >> 8, 255),
                       paletteTint ? sPaletteFade : 0);
    /* A semi-transparent sprite blends even where the window turns colour
     * effects off: the overworld keeps WIN0 over the whole screen without
     * them, and its fog and clouds are still see-through on the GBA. */
    if (!effects && !semiTransparent) return;
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
/* How far the map's info window slides down when the map zooms out. */
#define NAV_INFO_SLIDE 96

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
static bool StageUnfaded(void);

static float EdgeFade(int x, int y, int top, int bottom)
{
    float across = x < 0 ? -x / FADE_ACROSS : x > 240 ? (x - 240) / FADE_ACROSS : 0.0f;
    float down = y < top ? (top - y) / FADE_DOWN : y > bottom ? (y - bottom) / FADE_DOWN : 0.0f;

    if (StageUnfaded()) return 0.0f;
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

/*
 * Rayquaza on the title screen stands on the bottom edge of the screen, as it
 * does on the GBA's, rather than 40 lines above it: its layer is drawn that
 * much lower, so nothing has to be made up under its coils, and the room it
 * leaves above is its sky, carried up from its top row. Nothing on this
 * screen fades: its margins are its edge tiles repeated at full light, and
 * no black is laid under them.
 */
#define TITLE_RAYQUAZA_DROP (CTR_GAME_HEIGHT - 160 - CTR_STAGE_Y)
int CtrTitleScreen_RayquazaBg(void);

static int StageLayerDrop(unsigned bg)
{
    return sStage && CtrTitleScreen_RayquazaBg() == (int)bg ? TITLE_RAYQUAZA_DROP : 0;
}

static bool StageUnfaded(void)
{
    return sStage && CtrTitleScreen_RayquazaBg() >= 0;
}

static void StageUnderlay(void)
{
    unsigned display = Reg(0), mode = display & 7;
    bool still = false;
    u32 black = C2D_Color32(0, 0, 0, 255);

    sUnderlaid = false;
    /* Under windows the margins may be the backdrop on purpose (the
     * letterbox, which flashes with it), so nothing is assumed there. */
    if (!sStage || StageUnfaded() || (display & 128) || (display & 0x6000)) return;
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
 * colour it moves towards and how far; else a background's palette fade drawn
 * as a tint (UpdatePalette); 0 when there is neither.
 */
static float LayerBrightness(unsigned layer, bool *white)
{
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;

    *white = effect == 2;
    if (!(control & (1u << layer)) || effect < 2)
    {
        /* The stages' own edge fades know black and white only. */
        *white = sPaletteFadeColor == 0x7fff;
        return sInLayerTexture || layer > 5 || layer == 4 ? 0.0f : sPaletteFade;
    }
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
 * and the objects stay in the picture. The entry that fills at least 40% of
 * the row, or ~0u when none does: Rayquaza's coils take half of that row, its
 * backdrop tile only the other half, and anything spread over less than 40%
 * of a row is the picture itself, not a band behind it.
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
    return bestCount * 10 >= 30 * 4 ? best : ~0u;
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
 *   were wider: the clock's teal, the starter's meadow, the naming screen's
 *   stripes and title bar, the sky of the professor's speech.
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
    /* The layers' margins read on through the tilemap, as the GBA wraps it,
     * instead of repeating the picture's edge row and column. */
    bool wrap;
    /* Instead: one tile of the layer, at tileColumn, tileRow, over all the
     * margins - a backdrop of stripes the picture's edges only cut. */
    bool tile;
    uint8_t tileColumn, tileRow;
    /* Or that tilemap entry itself, whatever the tilemap holds now. */
    bool entry;
    uint16_t tileEntry;
    uint8_t skyRows, bandRow, bandChars;
    int8_t bandX, bandY;
    /* Instead of any layer: the bottom screen's light green behind the
     * whole of it (DrawSectionBackdrop), the screen's own background colours
     * transparent (KeySectionColours). */
    bool section;
} CentredFill;

static const CentredFill sCentredFills[CTR_CENTRED_SCREENS] =
{
    [CTR_CENTRED_MAIN_MENU] = {.layers = 1u << 1, .across = true, .skyRows = 4, .bandRow = 14, .bandChars = 3,
                               .bandX = -4, .bandY = 32},
    /* Inside the yellow border; the title bar is its top row. */
    [CTR_CENTRED_NAMING] = {.layers = 1u << 3, .inset = {2, 0, 2, 1}},
    [CTR_CENTRED_CLOCK] = {.layers = 1u << 3},
    /* The meadow round the bag: one plain green tile at every edge of it. */
    [CTR_CENTRED_STARTER] = {.layers = 1u << 2},
    /* Whatever background is at the back of the PokéNav screen on show
     * (CentredLayers): the dots, the Hoenn sea, the ribbons' wood. */
    [CTR_CENTRED_POKENAV] = {.backmost = true},
    /* The PC's scrolling pattern, which the GBA wraps round its 32x32 map. */
    [CTR_CENTRED_STORAGE] = {.layers = 1u << 3, .wrap = true},
    [CTR_CENTRED_SUMMARY] = {0},
    /* The bag and the party menu on the bottom screen's light green. */
    [CTR_CENTRED_BAG] = {.section = true},
    [CTR_CENTRED_BAG_WHOLE] = {.section = true},
    /* The Pokédex: a tile its screen on show names (CentredFillOf), or else
     * whatever is at the back of it. */
    [CTR_CENTRED_POKEDEX] = {.backmost = true},
    [CTR_CENTRED_PARTY] = {.section = true},
    [CTR_CENTRED_PARTY_WHOLE] = {.section = true},
};

int CtrPokenavList_Bg(void);
/* pokedex.c: the tile of a layer the Pokédex's screen on show carries out to
 * the edges, as bg << 16 | column << 8 | row, or with 1 << 20 as
 * bg << 16 | the tilemap entry; -1 when it names none. */
int CtrPokedex_Backdrop(void);

/* How a centred screen's margins are filled. The Pokédex's depend on which of
 * its screens is up: its stripes, the search's green, the area map's sea. */
static const CentredFill *CentredFillOf(unsigned screen)
{
    static CentredFill dex;
    int backdrop;

    if (screen != CTR_CENTRED_POKEDEX || (backdrop = CtrPokedex_Backdrop()) < 0) return &sCentredFills[screen];
    dex = (CentredFill){.layers = (uint8_t)(1u << ((backdrop >> 16) & 3)), .tile = true,
                        .tileColumn = (uint8_t)(backdrop >> 8), .tileRow = (uint8_t)backdrop,
                        .entry = (backdrop & (1 << 20)) != 0, .tileEntry = (uint16_t)backdrop};
    return &dex;
}

/* The backgrounds a centred screen carries out to its edges. */
static unsigned CentredLayers(const CentredFill *fill)
{
    unsigned display = Reg(0), best = 4, priority = 0;
    int list;

    if (!fill->backmost) return fill->layers;
    if ((display & 7) > 2) return 0;
    /* A PokéNav list scrolls behind the screen's frame: the frame is what
     * goes on past the picture, not the list's next entries. */
    list = CtrPokenavList_Bg();
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && (Reg(8 + bg * 2) & 3) >= priority && (int)bg != list
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
    unsigned mask = (size & 1) ? 63u : 31u, rows = (size & 2) ? 63u : 31u;
    int ox = (int)(scrollX & 7), oy = (int)(scrollY & 7);

    ViewBase();
    if (from < FloorDiv8(sClipY0 - sSpanShiftY + oy)) from = FloorDiv8(sClipY0 - sSpanShiftY + oy);
    for (int w = from; w < to && w * 8 - oy + sSpanShiftY < sClipY1; ++w)
    {
        bool beside = w >= 0 && w < 20;
        unsigned row = fill->tile ? fill->tileRow
                     : fill->wrap ? (unsigned)(w + (int)(scrollY >> 3)) & rows
                     : w < 0 ? (unsigned)fill->inset[1] : w >= 20 ? 19u - (unsigned)fill->inset[3]
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
            column = fill->tile ? fill->tileColumn
                   : fill->wrap ? ((unsigned)v + (scrollX >> 3)) & mask
                   : v < 0 ? (unsigned)fill->inset[0] : v >= 30 ? 29u - (unsigned)fill->inset[2]
                   : ((unsigned)v + (scrollX >> 3)) & mask;
            entry = fill->entry ? fill->tileEntry : Read16(rowBase + (column & 31) * 2 + (column >> 5) * 2048);
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

/* The PokéNav keeps its own band by band path (NavCompose, DrawBandBgTex). */
static bool CentredTexture(unsigned bg)
{
    return sCentredScreen != CTR_CENTRED_POKENAV && LayerDrawable(bg);
}

/*
 * Lines [top, bottom) of a centred screen's picture: from the layer's texture
 * when it has one (LayersPrepare), one quad where the tiles are 30 per line.
 * Walked tile by tile, the PC's four layers were ~3000 quads a frame and 30
 * fps on an Old 3DS; the bag's three, 1800 and a frame dropped every second.
 */
static void DrawCentredPicture(unsigned bg, int top, int bottom)
{
    if (!CentredTexture(bg) || sSpanShiftX || sSpanShiftY)
    {
        DrawCentredSpan(bg, 0, 240, top, bottom);
        return;
    }
    ViewBase();
    DrawLayerRect(bg, 0, 240, top, bottom, 0, (float)top, 1, 1, false, 0, 0);
}

/*
 * The margins of a layer that reads on through its tilemap (fill->wrap): the
 * texture repeats as the tilemap wraps, so they are the four bands around the
 * picture cut from it at their own place.
 */
static bool DrawWrapMarginsTex(unsigned bg, const CentredFill *fill)
{
    if (!fill->wrap || fill->across || !CentredTexture(bg)) return false;
    ViewBase();
    DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, VIEW_TOP, 0, VIEW_LEFT, VIEW_TOP, 1, 1, false, 0, 0);
    DrawLayerRect(bg, VIEW_LEFT, VIEW_RIGHT, 160, VIEW_BOTTOM, VIEW_LEFT, 160, 1, 1, false, 0, 0);
    DrawLayerRect(bg, VIEW_LEFT, 0, 0, 160, VIEW_LEFT, 0, 1, 1, false, 0, 0);
    DrawLayerRect(bg, 240, VIEW_RIGHT, 0, 160, 240, 0, 1, 1, false, 0, 0);
    return true;
}

/* True when the centred screen draws this layer itself. */
static bool DrawCentredBg(unsigned bg)
{
    const CentredFill *fill = CentredFillOf(sCentredScreen);

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
        DrawCentredPicture(bg, sky * 8, 160);
        if (sky || !DrawWrapMarginsTex(bg, fill))
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
        if (CentredTexture(bg))
        {
            DrawCentredPicture(bg, 0, 160);
            return;
        }
        if (right > 240) right = 240;
        if (bottom > 160) bottom = 160;
    }
    if (bg == 0 && sFieldBanner) right = CTR_GAME_WIDTH;
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
 * there, only the cells whose tile, entry or palette changed drawn again
 * (LayerRenderCells). Drawing it is then a handful of quads cut from that texture
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
    uint32_t usedFrame;
    /* The token and tile cache generation of the last walk of its cells. */
    uint32_t walked, walkedGeneration;
    bool valid;
} LayerTexture;
static LayerTexture sLayers[4];
/* Whether each background is drawn from its texture this frame. */
static bool sLayerReady[4];
static uint32_t sLayerFail[4];
static bool sLayerFailLogged;
/*
 * A stage whose layer textures found no VRAM because the depth planes hold it.
 * Its layers drawn tile by tile cost 30 ms a frame on an Old 3DS, so the
 * planes give way to them: first the nearest (sPlaneShrinkAsked), which is
 * what the intro needs, and only with two left all of them, the stage then
 * composed per eye - twice the work, 30 fps - until it is over.
 */
static bool sStageWithoutPlanes;
static bool sPlaneShrinkAsked;
static bool sBandsReady;
void CtrVideo_RequestPlaneRelease(void);

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

/* A texture and render target for background bg at its tilemap's size. */
static bool LayerCreate(unsigned bg, unsigned width, unsigned height)
{
    LayerTexture *layer = &sLayers[bg];

    if (C3D_TexInitVRAM(&layer->tex, width, height, GPU_RGBA5551)
        && (layer->target = C3D_RenderTargetCreateFromTex(&layer->tex, GPU_TEXFACE_2D, 0, -1)))
        return true;
    LayerRelease(bg);
    return false;
}

static void LayersPrepare(void)
{
    /* A centred screen too (DrawCentredPicture), the PokéNav only when its
     * line windows need them. */
    bool centred = sCentred && sCentredScreen != CTR_CENTRED_POKENAV;
    unsigned want = LineBackgrounds()
                  | (sStage || sFieldLayers || centred || LineWindows() ? TextBackgrounds() : 0);

    /* The wanted ones of the wrong size go first, so what they held is
     * free for any of this frame's new ones. */
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned size = Reg(8 + bg * 2) >> 14;

        if ((want & (1u << bg)) && sLayers[bg].tex.data
            && (sLayers[bg].tex.width != ((size & 1) ? 512 : 256)
                || sLayers[bg].tex.height != ((size & 2) ? 512 : 256)))
            LayerRelease(bg);
    }
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
        if (!layer->tex.data)
        {
            if (sLayerFail[bg] && sStats.frames - sLayerFail[bg] < LAYER_RETRY_FRAMES) continue;
            if (!LayerCreate(bg, width, height))
            {
                /*
                 * VRAM is taken and given back by every screen (the bottom
                 * surface, the voxel atlases, the other layers), so a block
                 * this size may be missing with enough free in pieces. The
                 * layers kept for a screen that is not up, and the voxel
                 * world's idle memory, go before this one gives up.
                 */
                unsigned freed = 0;

                for (unsigned other = 0; other < 4; ++other)
                    if (!(want & (1u << other)) && sLayers[other].tex.data)
                    {
                        LayerRelease(other);
                        ++freed;
                    }
#if CTR_VOXEL_ENABLED
                if (!sStage) freed += CtrVoxel_ReleaseIdleVram() > 0;
#endif
                if (!freed || !LayerCreate(bg, width, height))
                {
                    if (sStage && sBandsReady)
                    {
                        /* Tile by tile for this one frame; a plane goes, or with
                         * only two left, all of them (sPlaneShrinkAsked). */
                        if (sBandCount > 2)
                            sPlaneShrinkAsked = true;
                        else
                        {
                            sStageWithoutPlanes = true;
                            CtrVideo_RequestPlaneRelease();
                        }
                        continue;
                    }
                    if (!sLayerFailLogged)
                        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for a %ux%u layer texture (free=%lu); "
                                     "tile walk", width, height, (unsigned long)vramSpaceFree());
                    sLayerFailLogged = true;
                    /* Only this layer waits; the others still try. */
                    sLayerFail[bg] = sStats.frames | 1;
                    continue;
                }
            }
            sLayerFail[bg] = 0;
            C3D_TexSetFilter(&layer->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&layer->tex, GPU_REPEAT, GPU_REPEAT);
        }
        layer->usedFrame = sStats.frames;
        sLayerReady[bg] = true;
    }
}

/*
 * Every layer texture - the field's, the stages', the line windows' - is kept
 * cell by cell.
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
 *
 * The stages had a hash of the whole layer instead, and the title screen
 * cycles the colour of Rayquaza's markings every fourth frame: the whole
 * 1024-tile layer was drawn again each time, a dropped frame on an Old 3DS
 * for a dozen tiles that changed.
 */
#define LAYER_CELLS 4096
static uint32_t sCellSig[4][LAYER_CELLS];
static unsigned sCellControl[4];

static bool LayerDrawable(unsigned bg);

/*
 * Layer cells without Citro2D's per-quad work.
 *
 * Drawn through C2D_DrawImageAt a cell cost ~4.6 us of CPU on an Old 3DS: a
 * scene that opens on four 256x512 layers drew 8192 of them, 37 ms, and every
 * new screen paid it for each of its layers. A cell is always one whole atlas
 * tile (or nothing) over one 8x8 square of the texture, so it needs no tint,
 * no blending and no transform: four vertices of two shorts and two floats,
 * written straight into a buffer, and one indexed draw per layer. The tile
 * replaces what the square held, transparent texels included, so a changed
 * cell needs no clearing first; an empty one is drawn in the constant 0.
 *
 * It runs on Citro2D's own program, set up for the layer by an empty Citro2D
 * draw, and only borrows the vertex layout, the buffer and the first texture
 * stage, which it puts back: rebinding Citro2D (C2D_Prepare) mid-frame left
 * its tinted draws unfaded for the rest of the frame.
 *
 * The buffers are refilled from the start each frame: the GPU has finished
 * the last frame's draws once C3D_FrameBegin returns, and C3D_FrameEnd flushes
 * the linear heap before the GPU reads any of it. A frame that would need more
 * than FAST_QUADS cells keeps the Citro2D path.
 */
#define FAST_QUADS 8192
typedef struct { int16_t x, y; float s, t; } FastVertex;
static FastVertex *sFastVertices;
static uint16_t *sFastIndices;
static unsigned sFastUsed;
static bool sFastReady;

static void FastInit(void)
{
    sFastVertices = linearAlloc(FAST_QUADS * 4 * sizeof(FastVertex));
    sFastIndices = linearAlloc(FAST_QUADS * 6 * sizeof(uint16_t));
    if (sFastVertices == NULL || sFastIndices == NULL)
    {
        linearFree(sFastVertices);
        linearFree(sFastIndices);
        sFastVertices = NULL;
        sFastIndices = NULL;
        CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no linear memory for layer cells; through Citro2D");
        return;
    }
    for (unsigned q = 0; q < FAST_QUADS; ++q)
    {
        uint16_t *index = sFastIndices + q * 6, base = (uint16_t)(q * 4);

        index[0] = base;
        index[1] = base + 1;
        index[2] = base + 2;
        index[3] = base + 2;
        index[4] = base + 1;
        index[5] = base + 3;
    }
    sFastReady = true;
}

/* One cell's square: corners top-left, top-right, bottom-left, bottom-right,
 * with texcoords as Citro2D gives an atlas slot (DrawSlotTinted). */
static void FastQuad(FastVertex *v, int x, int y, int slot, bool flipX, bool flipY)
{
    unsigned tileX = ((unsigned)slot % (ATLAS_SIZE / 8)) * 8, tileY = ((unsigned)slot / (ATLAS_SIZE / 8)) * 8;
    float s0 = tileX / (float)ATLAS_SIZE, s1 = (tileX + 8) / (float)ATLAS_SIZE;
    float t0 = 1.0f - tileY / (float)ATLAS_SIZE, t1 = 1.0f - (tileY + 8) / (float)ATLAS_SIZE;

    if (flipX) { float swap = s0; s0 = s1; s1 = swap; }
    if (flipY) { float swap = t0; t0 = t1; t1 = swap; }
    v[0] = (FastVertex){(int16_t)x, (int16_t)y, s0, t0};
    v[1] = (FastVertex){(int16_t)(x + 8), (int16_t)y, s1, t0};
    v[2] = (FastVertex){(int16_t)x, (int16_t)(y + 8), s0, t1};
    v[3] = (FastVertex){(int16_t)(x + 8), (int16_t)(y + 8), s1, t1};
}

static void FastDraw(unsigned first, unsigned count)
{
    if (count)
        C3D_DrawElements(GPU_TRIANGLES, (int)count * 6, C3D_UNSIGNED_SHORT, sFastIndices + first * 6);
}

static bool FastCells(unsigned bg, const uint16_t *cells, unsigned count, const int16_t *cellSlot,
                      const uint16_t *cellEntry, unsigned columns)
{
    LayerTexture *layer = &sLayers[bg];
    unsigned tiles = 0, first = sFastUsed;
    C3D_AttrInfo savedAttr, *attr;
    C3D_BufInfo savedBuf, *buf;
    C3D_TexEnv savedEnv, *env;
    Tex3DS_SubTexture none = {0, 0, 0.0f, 1.0f, 0.0f, 1.0f};

    if (!sFastReady || count > FAST_QUADS - sFastUsed)
        return false;
    /* Tiles first, then the empty cells after them. */
    for (unsigned i = 0; i < count; ++i)
        if (cellSlot[cells[i]] >= 0) ++tiles;
    for (unsigned i = 0, t = 0, e = tiles; i < count; ++i)
    {
        unsigned cell = cells[i], entry = cellEntry[cell];
        int x = (int)(cell % columns) * 8, y = (int)(cell / columns) * 8;

        if (cellSlot[cell] >= 0)
            FastQuad(sFastVertices + (first + t++) * 4, x, y, cellSlot[cell], entry & 1024, entry & 2048);
        else
            FastQuad(sFastVertices + (first + e++) * 4, x, y, 0, false, false);
    }
    sFastUsed += count;

    /* Citro2D sets up the target, its projection and the atlas, with a quad
     * of no size, and sends it: its state is then the GPU's. */
    sInLayerTexture = true;
    BlendForget();
    C2D_SceneBegin(layer->target);
    C2D_ViewReset();
    Blend(bg, false, false);
    C2D_DrawImageAt((C2D_Image){&sAtlas, &none}, 0, 0, 0, &sTint, 1, 1);
    C2D_Flush();

    attr = C3D_GetAttrInfo();
    savedAttr = *attr;
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 2);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddFixed(attr, 2);
    AttrInfo_AddFixed(attr, 3);
    C3D_FixedAttribSet(2, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FixedAttribSet(3, 255.0f, 255.0f, 255.0f, 255.0f);
    buf = C3D_GetBufInfo();
    savedBuf = *buf;
    BufInfo_Init(buf);
    BufInfo_Add(buf, sFastVertices, sizeof(FastVertex), 2, 0x10);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    env = C3D_GetTexEnv(0);
    savedEnv = *env;
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    FastDraw(first, tiles);
    if (count > tiles)
    {
        env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, 0);
        FastDraw(first + tiles, count - tiles);
    }

    /* Citro2D's layout, buffer and stage back, as it left them. */
    *C3D_GetTexEnv(0) = savedEnv;
    C3D_SetAttrInfo(&savedAttr);
    C3D_SetBufInfo(&savedBuf);
    sInLayerTexture = false;
    BlendForget();
    sStats.cells += count;
    return true;
}

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

    /* Nothing it is made of changed since its cells were last walked: the
     * texture is what a walk would draw. */
    if (!full && layer->walkedGeneration == sCacheGeneration && sBgPaletteStamp <= layer->walked
        && !VramChangedAfter(map, rows * columns * 2, layer->walked)
        && !VramChangedAfter(chars, Min(1024u * (color256 ? 64 : 32), 0x10000 - chars), layer->walked))
        return false;
    layer->walked = sStats.frames + 1;
    layer->walkedGeneration = sCacheGeneration;
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

    {
        /* Mostly changed: the whole layer, which needs no clearing either. */
        bool whole = full || count > cells / 2;

        if (whole)
        {
            count = 0;
            for (unsigned cell = 0; cell < cells; ++cell)
                dirty[count++] = (uint16_t)cell;
        }
        if (FastCells(bg, dirty, count, cellSlot, cellEntry, columns))
            return true;
        full = whole;
    }
    BlendForget();
    sInLayerTexture = true;
    if (full)
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
    sInLayerTexture = false;
    BlendForget();
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
    /* The texture repeats as the tilemap wraps. */
    if (bg == 0 && sFieldBanner) x1 = sClipX1;
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
        if (sLayerReady[bg] && LayerRenderCells(bg)) any = true;
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

/*
 * The strips' art, read and laid out once at start-up: done on the first frame
 * of the leaves scene it was a 120 KiB read and a walk of every strip pixel in
 * the middle of the intro, the frame the scene fades in on.
 */
static void LeavesLoad(void)
{
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
}

static bool LeavesScene(void)
{
    if (!sStage) return false;
    for (unsigned bg = 0; bg < 4; ++bg)
        if (Reg(8 + bg * 2) != (bg | ((16 + 2 * bg) << 8) | 0x8000)) return false;
    LeavesLoad();
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
    /* The box of the map under the clip rectangle's four corners. */
    float minX = INFINITY, maxX = -INFINITY, minY = INFINITY, maxY = -INFINITY;
    for (unsigned corner = 0; corner < 4; ++corner)
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

/*
 * Where a sprite placed on the screen, not on the map, goes in the field view
 * (400x240, the 240x160 picture's own layout on it). Those of a window - the
 * item shown by the PC, the mon shown for a move - sit where their window
 * layer does: BG0 is drawn moved by CTR_FIELD_UI_SHIFT (the field banner's
 * streaks excepted), and they follow it. The healing machine's balls and
 * monitors were placed by hand on the picture around the player (the player's
 * tile centre at GBA (120, 80)), so they go where the map's machine is: in
 * the 2D field the picture's offset on the view, in the voxel one the
 * machine's tile seen by the camera.
 */
static void PlaceScreenObject(int i, unsigned boxW, unsigned boxH, bool voxel, int *x, int *y)
{
    if (sMachineOam[i >> 5] & (1u << (i & 31)))
    {
#if CTR_VOXEL_ENABLED
        if (voxel)
        {
            float sx, sy;

            if (CtrVoxel_ProjectPictureTile((*x + boxW / 2.0f - 120.0f) / 16.0f,
                                            (*y + boxH / 2.0f - 80.0f) / 16.0f, &sx, &sy))
            {
                *x = (int)(sx - boxW / 2.0f);
                *y = (int)(sy - boxH / 2.0f);
            }
            return;
        }
#endif
        if (!voxel)
        {
            *x += CTR_STAGE_X;
            *y += CTR_STAGE_Y - 16;
        }
        return;
    }
    if (!sFieldBanner) *x += (int)CTR_FIELD_UI_SHIFT;
}

/*
 * The fog's picture repeated over the whole clip area, on the lattice the
 * sprite at (x, y) sits on. The game lays twenty sprites for a 240x160
 * screen and lets OAM wrap them round its edges; on this view that left the
 * right of the screen bare, and the 8-bit y of a row past the bottom read as
 * a row further down instead of the one at the top. The lattice does not
 * care which sprite it starts from, so any wrap of x or y is harmless here.
 * The 64 tiles are looked up once and drawn at every place.
 */
static void DrawFogLattice(unsigned attr2, unsigned width, unsigned height, bool color256,
                           int x, int y, bool flipX, bool flipY)
{
    int slots[8 * 8];
    unsigned columns = width / 8, rows = height / 8;
    int x0 = x % (int)width, y0 = y % (int)height;

    if (columns * rows > sizeof(slots) / sizeof(slots[0])) return;
    for (unsigned ty = 0; ty < rows; ++ty)
        for (unsigned tx = 0; tx < columns; ++tx)
        {
            unsigned sx = flipX ? columns - 1 - tx : tx;
            unsigned sy = flipY ? rows - 1 - ty : ty;
            unsigned tile = CtrVideo_ObjTile(attr2 & 1023, sx, sy, width, color256, Reg(0) & 0x40);
            slots[ty * columns + tx] = GetTileSlot(0x10000 + tile * 32, 16 + (attr2 >> 12), color256);
        }
    if (x0 > sClipX0) x0 -= (int)width * ((x0 - sClipX0 + (int)width - 1) / (int)width);
    if (y0 > sClipY0) y0 -= (int)height * ((y0 - sClipY0 + (int)height - 1) / (int)height);
    for (int py = y0; py < sClipY1; py += (int)height)
        for (int px = x0; px < sClipX1; px += (int)width)
        {
            if (px + (int)width <= sClipX0 || py + (int)height <= sClipY0) continue;
            for (unsigned ty = 0; ty < rows; ++ty)
                for (unsigned tx = 0; tx < columns; ++tx)
                {
                    int slot = slots[ty * columns + tx];

                    if (slot >= 0)
                        DrawSlot(slot, px + CTR_VIEW_X + sLayerShift + (int)tx * 8,
                                 py + CTR_VIEW_Y + (int)ty * 8, flipX, flipY);
                }
        }
}

static void DrawObjects(unsigned priority, bool effects)
{
    static const uint8_t dimensions[3][4][2] = {
        {{8,8},{16,16},{32,32},{64,64}},
        {{16,8},{32,8},{32,16},{64,32}},
        {{8,16},{8,32},{16,32},{32,64}}
    };
    bool fogDrawn = false;

    for (int i = 127; i >= 0; --i)
    {
        bool fog = (sFogOam[i >> 5] & (1u << (i & 31))) != 0;

        /* The fog is one picture: drawn once, over the whole view. */
        if (fog && fogDrawn) continue;
#if CTR_VOXEL_ENABLED
        /* The voxel world draws a fog of its own, in the scene. */
        if (fog && sVoxelObjPass != VOXEL_OBJ_NONE && CtrVoxel_DrawsFog()) continue;
        if (sVoxelObjPass != VOXEL_OBJ_NONE)
        {
            bool weather = (sVoxelWeatherOam[i >> 5] & (1u << (i & 31))) != 0;
            bool screen = (sScreenOam[i >> 5] & (1u << (i & 31))) != 0;

            if (sVoxelObjPass == VOXEL_OBJ_WEATHER ? !weather : (weather || !screen))
                continue;
        }
#endif
        if (sObjFilter != OBJ_ALL && TransitionOam((unsigned)i) != (sObjFilter == OBJ_TRANSITION))
            continue;
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
        if (sStage || sCentred || sBattle || sTransitionCompose)
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
            if (sVoxelObjPass == VOXEL_OBJ_WEATHER)
            {
                x += CTR_STAGE_X;
                y += CTR_STAGE_Y;
            }
            else if (sVoxelObjPass == VOXEL_OBJ_SCREEN)
                PlaceScreenObject(i, boxW, boxH, true, &x, &y);
#endif
            if (sFieldUi && (sScreenOam[i >> 5] & (1u << (i & 31))))
                PlaceScreenObject(i, boxW, boxH, false, &x, &y);
        }
        /* Below the PokeNav's picture is its background, not the space the
         * GBA parks its unused sprites in. */
        if (sCentredScreen == CTR_CENTRED_POKENAV && y >= 160) continue;
        if (sNavBand && NavObjectBand(attr2 & 1023, y) != sNavBand) continue;
        /*
         * Any one of the fog's sprites gives the lattice its phase, on the
         * screen or not: as a map is entered the game can have all twenty
         * of them outside it until the camera first moves.
         */
        if (fog && !affine)
        {
            ++sStats.sprites;
            Blend(4, effects, mode == 1);
            ViewBase();
            DrawFogLattice(attr2, width, height, color256, x, y,
                           (attr1 & 0x1000) != 0, (attr1 & 0x2000) != 0);
            fogDrawn = true;
            continue;
        }
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
 * An affine backmost layer in a band of the PokéNav (the Hoenn map): the map
 * carries on around the body as it is, above it and below, at the body's
 * shift in every band - more of the sea, more of the zoomed map - drawn once.
 */
static void DrawNavBackmostAffine(unsigned bg)
{
    int clipY0 = sClipY0, clipY1 = sClipY1, viewY = sViewY, shift = sNavBands[1].shift;

    sViewY = shift;
    sClipY0 = clipY0 + viewY - shift;
    sClipY1 = clipY1 + viewY - shift;
    if (sClipY0 < sClipY1) DrawAffineBg(bg);
    sViewY = viewY;
    sClipY0 = clipY0;
    sClipY1 = clipY1;
}

/*
 * How much lower than its band a background of the PokéNav goes: the map's
 * info window, which the GBA slides down to the help bar while the map is
 * zoomed out, keeps to the help bar here too, and rises with the window as it
 * slides up for the zoomed map.
 */
int CtrPokenavRegionMap_InfoBg(void);

static int NavLayerShift(unsigned bg)
{
    int scroll;

    if (sNavBand != &sNavSubmenu[1] || CtrPokenavRegionMap_InfoBg() != (int)bg) return 0;
    scroll = (int)(Reg(0x12 + bg * 4) & 511);
    scroll = scroll >= 256 ? 512 - scroll : 0;
    if (scroll > NAV_INFO_SLIDE) scroll = NAV_INFO_SLIDE;
    return (240 - 160 - NAV_BODY_SHIFT) * scroll / NAV_INFO_SLIDE;
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
        if (!(sPriorityMask & (SLOT_OBJ(priority) | SLOT_BG(priority)))) continue;
        /* Priority is the depth scale: 3 stays at the screen plane. */
        sLayerShift = (sLayerOrigin + sParallax * (3 - priority)) / sShiftZoom;
        for (int bg = 3; bg >= 0 && (sPriorityMask & SLOT_BG(priority)); --bg)
        {
            if (!(mask & (1u << bg)) || !(display & (0x100u << bg)) || (Reg(8 + bg * 2) & 3) != (unsigned)priority) continue;
            if ((mode == 1 && bg == 3) || (mode == 2 && bg < 2)) continue;
            if (Reg(8 + bg * 2) & 0x40) Error(11, "BG mosaic not supported");
            int clipY0 = sClipY0, clipY1 = sClipY1, viewY = sViewY, drop = StageLayerDrop(bg);
            bool lines = sNavBand && !(CentredLayers(CentredFillOf(sCentredScreen)) & (1u << bg));

            sViewY += drop;
            sClipY0 -= drop;
            sClipY1 -= drop;
            if (lines)
            {
                int lower = NavLayerShift(bg);

                sViewY += lower;
                sClipY0 -= lower;
                sClipY1 -= lower;
                if (sClipY0 < sNavBand->top) sClipY0 = sNavBand->top;
                if (sClipY1 > sNavBand->bottom) sClipY1 = sNavBand->bottom;
                if (sClipY0 >= sClipY1)
                {
                    sViewY = viewY;
                    sClipY0 = clipY0;
                    sClipY1 = clipY1;
                    continue;
                }
                NavScissor(sNavBand->top + sViewY, sNavBand->bottom + sViewY);
            }
            Blend(bg, mask & 32, false);
            /* Which stage a slow frame is in: the layer walk, the sprites or
             * the GPU. Guessing that from fps alone costs a hardware run. */
            uint64_t start = svcGetSystemTick();
            float shift = sLayerShift;
            if (sFieldUi && bg == 0 && !sFieldBanner) sLayerShift += CTR_FIELD_UI_SHIFT / sShiftZoom;
            if (((mode == 1 && bg == 2) || mode == 2) && sNavBand && !lines) DrawNavBackmostAffine(bg);
            else if ((mode == 1 && bg == 2) || mode == 2) DrawAffineBg(bg);
            else if (sBattle && bg == 0) DrawBattleTextLayer(bg);
            else if (!DrawLineBg(bg) && !DrawFieldBgTex(bg) && !DrawStageBgTex(bg) && !DrawBandBgTex(bg))
                DrawTextBg(bg);
            sLayerShift = shift;
            sViewY = viewY;
            sClipY0 = clipY0;
            sClipY1 = clipY1;
            if (lines) NavScissor(sNavBand->screenTop, sNavBand->screenBottom);
            sBgTicks += svcGetSystemTick() - start;
        }
        if ((mask & 16) && (display & 0x1000) && (sPriorityMask & SLOT_OBJ(priority)))
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
    bool gba = sStage || sBattle || sTransitionCompose;
    unsigned extent = gba ? (vertical ? 160u : 240u)
                          : (vertical ? (unsigned)CTR_GAME_HEIGHT : (unsigned)CTR_GAME_WIDTH);

    *first = WindowEdge(limits, false, extent);
    *last = WindowEdge(limits, true, extent);
    /*
     * A left edge past the right one wraps round the line on the GBA: that is
     * how a window starts left of the screen. The label of the starter on the
     * left (starter_choose.c, its column 0 less 4 pixels) is WIN0H 252..108,
     * which on a centred screen is -4..108, just into the margin.
     */
    if (sCentred && !vertical && *first > *last) *first -= 256;
    if (!gba) return;
    if ((limits & 255) > extent || (limits >> 8) > (limits & 255)) *last = (int)extent;
    /* A transition is composed as the GBA picture, margins added after. */
    if (*first >= *last || sTransitionCompose) return;
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
    const CentredFill *fill = CentredFillOf(sCentredScreen);

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

/*
 * The rectangles lines [top, bottom) split into by the windows, each with the
 * layer mask it shows: WIN0 > WIN1 > outside. At most 5 x 5 of them.
 */
#define WINDOW_RECTS 25
static unsigned WindowPartition(int top, int bottom, int rects[WINDOW_RECTS][4],
                                unsigned masks[WINDOW_RECTS])
{
    unsigned display = Reg(0), count = 0;
    /* Window rectangles are GBA coordinates; the partition covers the whole
     * viewport so the margins keep the "outside" mask. */
    bool gba = sNavBand || sTransitionCompose;
    int xs[6] = {gba ? 0 : VIEW_LEFT, gba ? 240 : VIEW_RIGHT};
    int ys[6] = {top, bottom};
    unsigned nx = 2, ny = 2;
    for (unsigned w = 0; w < 2; ++w)
        if (display & (0x2000u << w))
        {
            int x0, x1, y0, y1;

            WindowRect(w, &x0, &x1, &y0, &y1);
            if (y0 < top) y0 = top;
            if (y1 > bottom) y1 = bottom;
            if (sTransitionCompose)
            {
                if (x0 < xs[0]) x0 = xs[0];
                if (x1 > xs[1]) x1 = xs[1];
            }

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
        unsigned mask = (display & 0x6000) ? Reg(0x4a) & 63 : 63;
        for (int w = 1; w >= 0; --w)
            if ((display & (0x2000u << w)) && Inside(xs[x], ys[y], (unsigned)w))
                mask = (Reg(0x48) >> (8*w)) & 63;
        rects[count][0] = xs[x];
        rects[count][1] = ys[y];
        rects[count][2] = xs[x+1];
        rects[count][3] = ys[y+1];
        masks[count++] = mask;
    }
    return count;
}

/* Layers a transition's band composes through its windows (TransitionCompose). */
static unsigned sTransitionLayers;

static void ComposeBand(int top, int bottom)
{
    int rects[WINDOW_RECTS][4];
    unsigned masks[WINDOW_RECTS];
    unsigned count = WindowPartition(top, bottom, rects, masks);

    /* Each rectangle has a uniform mask. GPU scissor clips transformed primitives. */
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned mask = sTransitionCompose ? masks[i] & sTransitionLayers : masks[i];

        if (sTransitionCompose && !(mask & 17u)) continue;
        C2D_Flush();
        Scissor(rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
        sScissored = true;
        sClipX0 = rects[i][0]; sClipX1 = rects[i][2];
        sClipY0 = rects[i][1]; sClipY1 = rects[i][3];
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
    /* Not fatal: without it the voxel picture has no bloom. */
    if (C3D_TexInitVRAM(&sBloomTex, 128, 64, GPU_RGB565))
    {
        C3D_TexSetFilter(&sBloomTex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&sBloomTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sBloom = C3D_RenderTargetCreateFromTex(&sBloomTex, GPU_TEXFACE_2D, 0, -1);
    }
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
    FastInit();
    LeavesLoad();
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
    if (!sBattle && !sTransition)
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

#if CTR_VOXEL_ENABLED
static void GpuSplit(void);
#else
#define GpuSplit() C3D_FrameSplit(0)
#endif

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
    sLayerExclude = 1 | sWorldLayers;
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* Seal the world list; FrameEnd owns queue submission and completion. */
    GpuSplit();

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
    /* The sky above the scenery, which the world has of its own. */
    if (!sBattleWorld)
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
 * The bottom screen's surface, the whole 320x240 screen. The PokéNav uses
 * the 240x240 area left of the button column; the PC's boxes all of it. It
 * is not linked to the screen, since a linked target is copied over the
 * whole of it and would cover the column the bottom screen draws itself;
 * after the frame the PokéNav's part is copied into the first 240 columns of
 * the framebuffer, which are exactly that area (the framebuffer runs column
 * by column from the left edge), and the boxes' into all of it. Made when
 * one of them opens, given back a few seconds after it closes.
 */
#define BOTTOM_SIZE 240
#define BOTTOM_WIDTH 320
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
    sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_WIDTH, GPU_RB_RGB565, -1);
#if CTR_VOXEL_ENABLED
    if (!sBottom && CtrVoxel_ReleaseIdleVram() > 0)
        sBottom = C3D_RenderTargetCreate(BOTTOM_SIZE, BOTTOM_WIDTH, GPU_RB_RGB565, -1);
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

/* The screens drawn over the whole bottom screen, rather than left of the column. */
static bool BottomWhole(unsigned screen)
{
    return screen == CTR_CENTRED_STORAGE || screen == CTR_CENTRED_SUMMARY || screen == CTR_CENTRED_BAG_WHOLE
        || screen == CTR_CENTRED_PARTY_WHOLE;
}

/* The screens drawn on the bottom screen at all. */
static bool BottomScreen(unsigned screen)
{
    return screen == CTR_CENTRED_POKENAV || screen == CTR_CENTRED_BAG || screen == CTR_CENTRED_POKEDEX
        || screen == CTR_CENTRED_PARTY || BottomWhole(screen);
}

bool CtrVideo_BottomWhole(void) { return sBottomInUse && BottomWhole(sCentredScreen); }

/*
 * The composed picture into the bottom screen's framebuffer, inside the
 * frame: there citro3d queues the transfer after the drawing (a split) instead
 * of waiting for it. Called after C3D_FrameEnd it waited for the GPU to finish
 * the whole frame and then for the copy, every frame a bottom screen is up:
 * the PC, the bag and the Pokédex spent ~5 ms of CPU a frame on an Old 3DS
 * beyond their layers and sprites, and dropped frames. The framebuffer is
 * single-buffered (3ds_log.c) and the CPU canvas only writes the column past
 * it.
 */
static void BottomTransfer(void)
{
    u32 *fb = (u32 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

    unsigned columns = BottomWhole(sCentredScreen) ? BOTTOM_WIDTH : BOTTOM_SIZE;

    if (!sBottom || !fb || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return;
    C3D_SyncDisplayTransfer((u32 *)sBottom->frameBuf.colorBuf, GX_BUFFER_DIM(BOTTOM_SIZE, columns),
                            fb, GX_BUFFER_DIM(BOTTOM_SIZE, columns),
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
        /* The bottom screen shows the picture's 240 columns. */
        sClipX0 = 0;
        sClipX1 = 240;
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

/*
 * The PC's boxes (and a summary, and the bag) on the bottom screen: the GBA
 * picture 1:1 in the middle of the whole screen, or of the area left of the
 * column. Around it only the layer at the back goes on - the boxes' scrolling
 * pattern, the bag's stripes, the summary's backdrop; everything else,
 * sprites included, is cut at the picture's edges, where the GBA screen ends
 * and parks what it hides.
 */
#define STORAGE_MARGIN_X ((BOTTOM_WIDTH - 240) / 2)

static void StorageComposePart(int left, int right, int top, int bottom, unsigned exclude)
{
    ClipToView();
    sClipX0 = left;
    sClipX1 = right;
    sClipY0 = top;
    sClipY1 = bottom;
    sLayerExclude = exclude;
    C2D_Flush();
    sScissored = true;
    RestoreScissor();
    Compose();
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissored = false;
    sLayerExclude = 0;
}

/*
 * The bottom screen's light green behind the bag or the party menu, over the
 * whole of its part of the screen: what 3ds_bottom_ui.c draws behind its own
 * screens (DrawSection), faded as their background layer is. The Poké Ball
 * is a texture of its own, made once; the rest are rectangles.
 */
static C3D_Tex sBallTex;
static bool sBallTried;

static uint32_t SectionColour(uint16_t bgr, float fade, bool white)
{
    uint32_t c = CtrVideo_RGBA8(bgr, true);
    float to = white ? 255.0f : 0.0f;
    unsigned r = (unsigned)((c >> 24) * (1.0f - fade) + to * fade + 0.5f);
    unsigned g = (unsigned)(((c >> 16) & 255) * (1.0f - fade) + to * fade + 0.5f);
    unsigned b = (unsigned)(((c >> 8) & 255) * (1.0f - fade) + to * fade + 0.5f);

    return C2D_Color32(r, g, b, 255);
}

static bool BallTexture(void)
{
    const uint8_t *mask = CtrBottom_BallMask();
    const unsigned size = 2 * CTR_SECTION_BALL_RADIUS;
    uint16_t *data;

    if (sBallTex.data) return true;
    if (sBallTried || !mask) return false;
    sBallTried = true;
    if (!C3D_TexInit(&sBallTex, size, size, GPU_RGBA5551))
    {
        memset(&sBallTex, 0, sizeof(sBallTex));
        CtrLog_Write(CTR_LOG_ERROR, "video: no memory for the section's Poke Ball");
        return false;
    }
    C3D_TexSetFilter(&sBallTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sBallTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    data = sBallTex.data;
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
        {
            uint8_t v = mask[y * size + x];
            data[CtrVideo_Texel(x, y, size)] =
                v ? CtrVideo_RGBA5551(v == 1 ? CTR_SECTION_BALL : CTR_SECTION_BALL_TOP) : 0;
        }
    C3D_TexFlush(&sBallTex);
    return true;
}

static void DrawSectionBackdrop(int marginX, unsigned layer)
{
    const int w = 240 + 2 * marginX, h = 160 + 2 * CTR_STAGE_Y;
    const float x0 = -marginX + CTR_VIEW_X + sLayerShift, y0 = -CTR_STAGE_Y + CTR_VIEW_Y;
    bool white;
    float fade = LayerBrightness(layer, &white);
    uint32_t base = SectionColour(CTR_SECTION_BASE, fade, white);
    uint32_t line = SectionColour(CTR_SECTION_LINE, fade, white);
    uint32_t light = SectionColour(CTR_SECTION_LIGHT, fade, white);

    ViewBase();
    Blend(layer, false, false);
    C2D_DrawRectSolid(x0, y0, 0, w, h, base);
    C2D_DrawRectSolid(x0 + 1, y0 + 1, 0, w - 2, 1, line);
    C2D_DrawRectSolid(x0 + 1, y0 + h - 2, 0, w - 2, 1, line);
    C2D_DrawRectSolid(x0 + 1, y0 + 2, 0, w - 2, 1, light);
    C2D_DrawRectSolid(x0 + 1, y0 + h - 1, 0, w - 2, 1, light);
    C2D_DrawRectSolid(x0 + 1, y0 + 1, 0, 1, h - 2, line);
    C2D_DrawRectSolid(x0 + w - 2, y0 + 1, 0, 1, h - 2, line);
    C2D_DrawRectSolid(x0 + 2, y0 + 1, 0, 1, h - 2, light);
    if (BallTexture())
    {
        const float size = 2 * CTR_SECTION_BALL_RADIUS;
        const Tex3DS_SubTexture all = {(u16)size, (u16)size, 0, 1, 1, 0};
        C2D_ImageTint tint;

        C2D_PlainImageTint(&tint, white ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(0, 0, 0, 255), fade);
        C2D_DrawImageAt((C2D_Image){&sBallTex, &all}, x0 + w / 2 - CTR_SECTION_BALL_RADIUS,
                        y0 + h / 2 - CTR_SECTION_BALL_RADIUS, 0, &tint, 1, 1);
    }
    sStats.tiles += 9;
}

static void StorageCompose(int marginX)
{
    unsigned back = CentredLayers(CentredFillOf(sCentredScreen)) ? CentredLayers(CentredFillOf(sCentredScreen))
                                                                   : 1u << 3;
    unsigned marginsOnly = 63u & ~back;

    if (CentredFillOf(sCentredScreen)->section)
    {
        bool party = sCentredScreen == CTR_CENTRED_PARTY || sCentredScreen == CTR_CENTRED_PARTY_WHOLE;

        /* Nothing goes on past the picture: the green is the margins. */
        DrawSectionBackdrop(marginX, party ? 1 : 2);
        StorageComposePart(0, 240, 0, 160, 0);
        ClipToView();
        return;
    }
    StorageComposePart(-marginX, 240 + marginX, -CTR_STAGE_Y, 160 + CTR_STAGE_Y, marginsOnly);
    StorageComposePart(0, 240, 0, 160, 0);
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
        if (target != sBottom) Compose();
        else if (BottomWhole(sCentredScreen)) StorageCompose(STORAGE_MARGIN_X);
        else if (sCentredScreen == CTR_CENTRED_BAG || sCentredScreen == CTR_CENTRED_POKEDEX
                 || sCentredScreen == CTR_CENTRED_PARTY) StorageCompose(0);
        else NavCompose();
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
        /* The middle 240 columns of the logical frame, or 320 for the boxes:
         * the picture and the margins around it. Tilted as a screen target is. */
        int width = BottomWhole(sCentredScreen) ? BOTTOM_WIDTH : BOTTOM_SIZE;
        const Tex3DS_SubTexture middle = {(u16)width, BOTTOM_SIZE,
            (CTR_GAME_WIDTH - width) / 2 / 512.0f, 1,
            (CTR_GAME_WIDTH + width) / 2 / 512.0f, 1 - BOTTOM_SIZE / 256.0f};

        /* The framebuffer's own measures, as for a screen target. */
        C2D_SceneSize(BOTTOM_SIZE, BOTTOM_WIDTH, true);
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
/* Submit only at FrameEnd: Citro3D owns transfer completion and buffer swaps.
 * Its single linear-heap flush covers all producers, including Citro2D. */
static void GpuSplit(void)
{
    C3D_FrameSplit(0);
}

/*
 * HD-2D diorama: the tilt-shift of a miniature. The camera always looks north,
 * so the top of the screen is the distance and the bottom the nearest ground;
 * both are blurred, most at the screen's edge and not at all by the focus band
 * around the player. Each band is two copies of the world surface drawn over
 * it with bilinear filtering half a texel off either way, so each copy is the
 * average of four texels and the two together are centred on the pixel;
 * fading in from the focus band to the edge. A wider
 * blur than the four one-pixel taps it replaces, for about the same fill.
 * The surface is sampled NEAREST again afterwards: the picture itself stays
 * pixel-sharp.
 */
#define DIORAMA_TOP 100    /* rows blurred at the top */
#define DIORAMA_BOTTOM 56  /* ... and at the bottom */

static void SurfaceFilter(GPU_TEXTURE_FILTER_PARAM filter)
{
    /* Batched draws sample with the state at the flush: flush first, and
     * rebind so the new filter reaches the GPU. */
    C2D_Flush();
    C3D_TexSetFilter(&sSurface, filter, filter);
    C3D_TexBind(0, &sSurface);
}

static void DioramaTap(int y0, int rows, bool top, float dx, float dy, float alpha)
{
    /* One column short at each side: the surface past the picture is not
     * part of it. */
    const Tex3DS_SubTexture region = {CTR_GAME_WIDTH - 2, (u16)rows,
        (1.0f + dx) / 512.0f, 1.0f - (y0 + dy) / 256.0f,
        (1.0f + dx + CTR_GAME_WIDTH - 2) / 512.0f, 1.0f - (y0 + dy + rows) / 256.0f};
    C2D_ImageTint tint;

    C2D_AlphaImageTint(&tint, 0.0f);
    if (top)
        C2D_TopImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 0.0f);
    else
        C2D_BottomImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 0.0f);
    C2D_DrawImageAt((C2D_Image){&sSurface, &region}, 1, y0, 0, &tint, 1, 1);
}

static void VoxelDiorama(void)
{
    SurfaceFilter(GPU_LINEAR);
    /* Half a texel either way: two 2x2 averages on either side of each
     * pixel, together centred on it - a blur that does not shift the picture. */
    DioramaTap(0, DIORAMA_TOP, true, 0.5f, 0.5f, 0.67f);
    DioramaTap(0, DIORAMA_TOP, true, -0.5f, -0.5f, 0.50f);
    DioramaTap(CTR_GAME_HEIGHT - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, 0.5f, 0.5f, 0.60f);
    DioramaTap(CTR_GAME_HEIGHT - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, -0.5f, -0.5f, 0.45f);
    SurfaceFilter(GPU_NEAREST);
}

/*
 * Bloom: the brightest parts of the picture - sunlit sand, white walls, water
 * catching the light - glow softly into their surroundings. The world surface
 * is drawn at a quarter of its size into a small target, keeping only what is
 * brighter than BLOOM_THRESHOLD (texture environment 4, which citro2d leaves
 * free), then stretched back over the screen with bilinear filtering and
 * added. Two passes: 6000 pixels, then one additive screen.
 */
#define BLOOM_W (CTR_GAME_WIDTH / 4)
#define BLOOM_H (CTR_GAME_HEIGHT / 4)
#define BLOOM_THRESHOLD 0.85f

static void VoxelBloomPrepare(void)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};
    unsigned t = (unsigned)(BLOOM_THRESHOLD * 255.0f + 0.5f);
    C3D_TexEnv *env;

    C2D_TargetClear(sBloom, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(sBloom);
    C2D_ViewReset();
    Blend(5, false, false);
    SurfaceFilter(GPU_LINEAR);
    /* Replace, not blend: the surface's alpha is not the picture's. */
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    /* (colour - threshold) x 4: black below it. */
    env = C3D_GetTexEnv(4);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_SUBTRACT);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_4);
    C3D_TexEnvColor(env, 0xFF000000u | t << 16 | t << 8 | t);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 0.25f, 0.25f);
    C2D_Flush();
    C3D_TexEnvInit(C3D_GetTexEnv(4));
    SurfaceFilter(GPU_NEAREST);
    BlendForget();
}

static void VoxelBloomCompose(float strength)
{
    const Tex3DS_SubTexture region = {BLOOM_W, BLOOM_H, 0, 1,
        BLOOM_W / 128.0f, 1 - BLOOM_H / 64.0f};
    C2D_ImageTint tint;

    C2D_Flush();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
    C2D_AlphaImageTint(&tint, strength);
    C2D_DrawImageAt((C2D_Image){&sBloomTex, &region}, 0, 0, 0, &tint, 4.0f, 4.0f);
    C2D_Flush();
    BlendForget();
}

/*
 * The game's own UI and weather over the voxel world. BG0 carries text; only
 * OAM entries tagged by the sprite sorter as weather are composed. Drawing all
 * OBJ here would duplicate the player and every NPC over their billboards.
 */

/*
 * The dark of a cave: a soft ring of shadow closing in round the player, over
 * the world and under the game's text. One textured quad (CtrVoxel_Gloom).
 */
static void VoxelGloom(void)
{
    float x, y, size, amount;
    const C3D_Tex *tex = CtrVoxel_Gloom(&x, &y, &size, &amount);
    C2D_ImageTint tint;

    if (tex == NULL || amount <= 0.0f) return;
    const Tex3DS_SubTexture whole = {tex->width, tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
    unsigned alpha = (unsigned)(amount * 255.0f + 0.5f);

    Blend(5, false, false);
    C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, alpha > 255 ? 255 : alpha), 1.0f);
    C2D_DrawImageAt((C2D_Image){(C3D_Tex *)tex, &whole}, x - size * 0.5f, y - size * 0.5f, 0,
                    &tint, size / tex->width, size / tex->height);
    C2D_Flush();
    BlendForget();
}

static void ComposeVoxelOverlay(void)
{
    sPriorityMask = SLOTS_ALL;
    sParallax = 0;
    sLayerOrigin = 0.0f;
    sLayerShift = 0.0f;
    sClipX0 = sClipY0 = 0;
    sClipX1 = CTR_GAME_WIDTH;
    sClipY1 = CTR_GAME_HEIGHT;
    sVoxelObjPass = VOXEL_OBJ_WEATHER;
    /* With the blending: clouds and fog are semi-transparent sprites, and
     * drawn without it they hid the world under them. */
    Layers(1u << 4 | 32u);
    BlendForget();
    /* Text windows and prompts must stay above the precipitation. */
    sLayerOrigin = sFieldBanner ? 0.0f : CTR_FIELD_UI_SHIFT;
    Layers(1u << 0);
    sLayerOrigin = 0.0f;
    sVoxelObjPass = VOXEL_OBJ_SCREEN;
    Layers(1u << 4);
    sVoxelObjPass = VOXEL_OBJ_NONE;
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

    /* The brightness effect (BLDY) on the field's backgrounds and sprites. */
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    float bright = effect >= 2 ? Min(Reg(0x54) & 31, 16) / 16.0f : 0.0f;
    float bloom;

    CtrVoxel_SetBrightness((control & 0x0e) ? bright : 0.0f, (control & 0x10) ? bright : 0.0f,
                           effect == 2);
    /* C2D_TargetClear clears colour and depth, which the 3D pass needs. */
    C2D_TargetClear(sLogical, clear);
    CtrVoxel_Draw(sLogical, 0.0f);

    /* Finish the world before sampling it. UI is drawn on top after blur.
     * The GPU draws it from here while the rest is recorded. */
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();

    /* Back to the 2D compositor, which assumes its own program and no depth. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    bloom = sBloom != NULL ? CtrVoxel_Bloom() : 0.0f;
    if (bloom > 0.005f)
        VoxelBloomPrepare();
    C2D_TargetClear(sTop, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(sTop);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawImageAt((C2D_Image){&sSurface, &logical}, 0, 0, 0, NULL, 1, 1);
    if (CtrSettings_VoxelBlur())
        VoxelDiorama();
    if (bloom > 0.005f)
        VoxelBloomCompose(bloom);
    VoxelGloom();
    if (!(Reg(0) & 128)) ComposeVoxelOverlay();
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    GpuSplit();
}
#endif

#include "compositor/3ds_video_compositor.h"
#include "compositor/3ds_video_compositor.c"

const CtrVideoStats *CtrVideo_GetStats(void) { return &sStats; }

void CtrVideo_SetStage(bool stage) { sStageRequested = stage; }
void CtrVideo_SetCentred(unsigned screen) { sCentredRequested = screen < CTR_CENTRED_SCREENS ? screen : CTR_CENTRED_NONE; }
void CtrVideo_SetBattle(bool battle) { sBattleRequested = battle; }
void CtrVideo_SetTransition(bool transition) { sTransitionRequested = transition; }

void CtrVideo_SetLineRegisters(const uint16_t *regs, unsigned lines)
{
    /* The bridge's table, filled for this frame just before it is presented. */
    sLineRegs = regs && lines >= CTR_GBA_LINES ? (const uint16_t (*)[CTR_LINE_REGS])regs : NULL;
}

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

static struct
{
    bool on;
    int x, y, radius;
    float shown, burst;
    uint32_t ticks;
} sFieldLight;

void CtrVideo_SetFieldLight(bool on, int x, int y, int radius)
{
    if (on && (!sFieldLight.on || sFieldLight.radius < 0))
        sFieldLight.shown = (float)radius;
    else if (on && radius > sFieldLight.radius && sFieldLight.shown < radius - 6.0f)
        sFieldLight.burst = 1.0f;
    sFieldLight.on = on;
    if (!on) { sFieldLight.burst = 0.0f; return; }
    sFieldLight.x = x;
    sFieldLight.y = y;
    sFieldLight.radius = radius;
    ++sFieldLight.ticks;
    sFieldLight.shown += ((float)radius - sFieldLight.shown) * 0.25f;
    if (radius > sFieldLight.shown - 0.5f && radius < sFieldLight.shown + 0.5f) sFieldLight.shown = (float)radius;
    sFieldLight.burst *= 0.955f;
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
    if (sBloom) C3D_RenderTargetDelete(sBloom);
    sBloom = NULL;
    if (sBloomTex.data) C3D_TexDelete(&sBloomTex);
    memset(&sBloomTex, 0, sizeof(sBloomTex));
    if (sSurface.data) C3D_TexDelete(&sSurface);
#if CTR_VOXEL_ENABLED
    if (sBattleShadowTex.data) C3D_TexDelete(&sBattleShadowTex);
    memset(&sBattleShadowTex, 0, sizeof(sBattleShadowTex));
#endif
    LeavesRelease();
    if (sAtlas.data) C3D_TexDelete(&sAtlas);
    if (sC3d) C3D_Fini();
    sLogical = sTop = sTopRight = NULL;
    sC2d = sC3d = false;
    memset(&sSurface, 0, sizeof(sSurface));
    memset(&sAtlas, 0, sizeof(sAtlas));
}
