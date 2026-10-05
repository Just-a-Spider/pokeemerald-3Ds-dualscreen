/*
 * Player and NPC billboards. See voxel_entities.h and NOTICE.md.
 */

/* Before global.h, which redefines abs() as a macro over stdlib's prototype. */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "global.h"
#include "fieldmap.h"
#include "sprite.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "constants/field_effects.h"
#include "constants/event_objects.h"
#include "gba/io_reg.h"
#include "port_platform.h"
#include "field_door.h"
#include "field_effect.h"
#include "field_weather.h"
#include "constants/weather.h"

#include "3ds_video.h"
#include "3ds_platform.h"
#include "voxel_entities.h"
#include "voxel_grade.h"
#include "voxel_relief.h"
#include "voxel_world.h"
#include "voxel_lighting.h"

/* 16 pixels of sprite is one world tile, as in the reference renderer. */
#define VOXEL_PIXELS_PER_TILE 16.0f

/* Largest sprite this atlas can hold, 8bpp: 64*64 bytes of source tiles. */
#define VOXEL_SPRITE_MAX_BYTES (VOXEL_SPRITE_SLOT_DIM * VOXEL_SPRITE_SLOT_DIM)

/* Step durations, matching sStepTimes[] in event_object_movement.c. */
static const int sStepFrames[] = { 16, 8, 6, 4, 2 };

/*
 * What a slot was last decoded from. The animation of a walking object event
 * rewrites the tiles *behind* a fixed tileNum, so the descriptor alone cannot
 * detect a frame change: the source bytes and the palette are kept and
 * compared. That is ~2 KiB of memcmp per object per frame, which is far
 * cheaper than re-decoding one.
 */
typedef struct
{
    bool valid;
    u16 tileNum, paletteNum;
    u8 shape, size;
    bool flipX, flipY, color256;
    int width, height;
    u32 sourceBytes;
    u8 source[VOXEL_SPRITE_MAX_BYTES];
    u16 palette[256];
    u32 paletteBytes;
    /* Extent last written into the atlas cell, so a shrinking sprite clears
     * exactly what it used to cover instead of the whole 64x64 slot. */
    int drawnWidth, drawnHeight;
    /* Empty rows under the feet of the standing pose: the art's feet sit this
     * far up the cell, and the card leaves them out to put them on the
     * ground (VisibleRows). Measured once per picture table (StandingFootPad). */
    int footPad;
    const struct SpriteFrameImage *footPadImages;
    /* Measured on the standing pose itself; else, until that picture is in
     * memory, on the frame the object first showed (StandingFootPad). */
    bool footPadExact;
} VoxelSpriteSlot;

static VoxelSpriteSlot sSlots[VOXEL_TOTAL_SLOTS];
/* The field effect sprite a slot of an unused object event shows, plus one;
 * 0 for none (VoxelEntities_Emit). */
static u8 sSlotEffect[VOXEL_SPRITE_SLOTS];
static int sPlayerVertexFirst = -1;

int VoxelEntities_PlayerVertexFirst(void) { return sPlayerVertexFirst; }

static struct {
    bool active;
    int x, y;
    int size;
    const u8 *tiles;
    const u8 *paletteNums;
    unsigned frameOffset;
    int partnerX, partnerY;
} sDoorAnimState;

void VoxelEntities_Reset(void)
{
    for (unsigned i = 0; i < VOXEL_TOTAL_SLOTS; ++i)
    {
        sSlots[i].valid = false;
        if (i < VOXEL_SPRITE_SLOTS)
            sSlotEffect[i] = 0;
    }
    sDoorAnimState.active = false;
}

/* ── OAM geometry ───────────────────────────────────────────────────────── */

static void GetSpriteDimensions(u8 shape, u8 size, int *w, int *h)
{
    static const u8 square[4][2] = {{8,8},{16,16},{32,32},{64,64}};
    static const u8 wide[4][2]   = {{16,8},{32,8},{32,16},{64,32}};
    static const u8 tall[4][2]   = {{8,16},{8,32},{16,32},{32,64}};
    const u8 (*table)[2];

    if (shape == ST_OAM_SQUARE) table = square;
    else if (shape == ST_OAM_H_RECTANGLE) table = wide;
    else if (shape == ST_OAM_V_RECTANGLE) table = tall;
    else { *w = 16; *h = 32; return; }
    *w = table[size & 3][0];
    *h = table[size & 3][1];
}

/* ── Movement interpolation ─────────────────────────────────────────────── */

static bool IsJumpMovement(u8 actionId)
{
    return (actionId >= MOVEMENT_ACTION_JUMP_2_DOWN && actionId <= MOVEMENT_ACTION_JUMP_2_RIGHT)
        || (actionId >= MOVEMENT_ACTION_JUMP_SPECIAL_DOWN && actionId <= MOVEMENT_ACTION_JUMP_SPECIAL_RIGHT)
        || (actionId >= MOVEMENT_ACTION_JUMP_DOWN && actionId <= MOVEMENT_ACTION_JUMP_IN_PLACE_RIGHT_LEFT)
        || (actionId >= MOVEMENT_ACTION_ACRO_WHEELIE_JUMP_DOWN && actionId <= MOVEMENT_ACTION_ACRO_WHEELIE_JUMP_RIGHT);
}

/*
 * Sub-tile progress in [0,1]: 0 at previousCoords, 1 at currentCoords.
 *
 * sprite->x/y are screen-space and already carry the camera offset, so they
 * cannot be used for a world position. The step timer can.
 */
static float GetMovementProgress(const struct ObjectEvent *obj, const struct Sprite *sprite)
{
    int speed, timer, stepLen;
    float t;

    /* Idle: ShiftStillObjectEventCoords has made previous == current. */
    if (!obj->singleMovementActive && !obj->heldMovementActive)
        return 1.0f;

    /* Ledge jumps and hop animations use data[6] as step timer */
    if (IsJumpMovement(obj->movementActionId))
    {
        int timer = (int)(u16)sprite->data[6];
        int distance = (int)(u16)sprite->data[4];
        int halfLen = (obj->movementActionId >= MOVEMENT_ACTION_JUMP_SPECIAL_DOWN &&
                       obj->movementActionId <= MOVEMENT_ACTION_JUMP_SPECIAL_RIGHT) ? 32 : 16;
        if (distance == 2 /* JUMP_DISTANCE_FAR */)
            t = (float)(timer % halfLen) / (float)halfLen;
        else
            t = (float)timer / (float)halfLen;

        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return t;
    }

    /* Slow walk actions (data[4] is timer, 32 frames) */
    if (obj->movementActionId >= 0x18 && obj->movementActionId <= 0x23)
    {
        timer = (int)(u16)sprite->data[4];
        t = (float)timer / 32.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return t;
    }

    speed = (int)(u16)sprite->data[4];
    timer = (int)(u16)sprite->data[5];
    if (speed < 0 || speed >= (int)ARRAY_COUNT(sStepFrames))
        return 1.0f;
    stepLen = sStepFrames[speed];
    if (stepLen <= 0)
        return 1.0f;

    t = (float)timer / (float)stepLen;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

static bool GetObjectWorldPos(const struct ObjectEvent *obj, const struct Sprite *sprite,
                              float *outX, float *outZ)
{
    float prevX = (float)(obj->previousCoords.x - MAP_OFFSET);
    float prevZ = (float)(obj->previousCoords.y - MAP_OFFSET);
    float curX = (float)(obj->currentCoords.x - MAP_OFFSET);
    float curZ = (float)(obj->currentCoords.y - MAP_OFFSET);
    float t = GetMovementProgress(obj, sprite);

    *outX = prevX + (curX - prevX) * t;
    *outZ = prevZ + (curZ - prevZ) * t;
    return true;
}

void VoxelEntities_GetPlayerWorldPos(float *worldX, float *worldZ)
{
    const struct ObjectEvent *obj = &gObjectEvents[gPlayerAvatar.objectEventId];
    float x = 0.0f, z = 0.0f;

    if (!obj->active || obj->spriteId >= MAX_SPRITES)
    {
        /* That helper already resolves to world space on its own. */
        VoxelWorld_GetPlayerWorldCoords(worldX, worldZ);
        return;
    }
    GetObjectWorldPos(obj, &gSprites[obj->spriteId], &x, &z);

    if (VoxelWorld_InstanceCount() > 0)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(0);

        x += (float)inst->originX;
        z += (float)inst->originY;
    }
    /* The camera follows the player where the relief puts it. */
    z += VoxelRelief_ShiftAt(x + 0.5f, z + 0.5f);
    if (worldX != NULL) *worldX = x;
    if (worldZ != NULL) *worldZ = z;
}

/* ── Slot decoding ──────────────────────────────────────────────────────── */

/*
 * Gathers the source tiles of a sprite into `dest`, in reading order, using
 * the same address arithmetic the 2D compositor uses for OBJ: 1D or 2D
 * character mapping per DISPCNT bit 6, two VRAM units per tile at 8bpp.
 */
static u32 GatherSource(const struct Sprite *sprite, int w, int h, bool color256, u8 *dest)
{
    const u8 *vram = VRAM_ + 0x10000;
    bool mapping1d = (REG_DISPCNT & DISPCNT_OBJ_1D_MAP) != 0;
    unsigned bytesPerTile = color256 ? 64u : 32u;
    u32 written = 0;

    for (int ty = 0; ty < h / 8; ++ty)
    {
        for (int tx = 0; tx < w / 8; ++tx)
        {
            unsigned tile = CtrVideo_ObjTile(sprite->oam.tileNum, (unsigned)tx, (unsigned)ty,
                                             (unsigned)w, color256, mapping1d);
            unsigned offset = tile * 32;

            /* OBJ character data is the last 32 KiB of the 96 KiB VRAM bank. */
            if (offset + bytesPerTile > 0x8000)
                offset = 0;
            memcpy(dest + written, vram + offset, bytesPerTile);
            written += bytesPerTile;
        }
    }
    return written;
}

/* Writes the gathered tiles into the slot's 64x64 cell of the atlas. */
static void DecodeSlot(VoxelSpriteSlot *slot, unsigned index, uint16_t *atlas)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned bytesPerTile = slot->color256 ? 64u : 32u;
    int tilesX = slot->width / 8;
    /* Clearing only what was drawn before, union the new extent, keeps a
     * walking sprite's per-frame cost at its own size rather than 64x64. */
    int clearW = slot->drawnWidth > slot->width ? slot->drawnWidth : slot->width;
    int clearH = slot->drawnHeight > slot->height ? slot->drawnHeight : slot->height;

    for (int y = 0; y < clearH; ++y)
        for (int x = 0; x < clearW; ++x)
            atlas[CtrVideo_Texel(baseX + (unsigned)x, baseY + (unsigned)y,
                                 VOXEL_SPRITE_ATLAS_DIM)] = 0;
    slot->drawnWidth = slot->width;
    slot->drawnHeight = slot->height;

    for (int ty = 0; ty < slot->height / 8; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            const u8 *tile = slot->source + (unsigned)(ty * tilesX + tx) * bytesPerTile;

            for (unsigned py = 0; py < 8; ++py)
            {
                for (unsigned px = 0; px < 8; ++px)
                {
                    unsigned index8 = py * 8 + px;
                    unsigned colorIdx = slot->color256
                        ? tile[index8]
                        : ((tile[index8 / 2] >> ((px & 1) * 4)) & 0xF);
                    int outX = tx * 8 + (int)px;
                    int outY = ty * 8 + (int)py;

                    /* Colour index 0 is transparent; the alpha bit carries it. */
                    if (colorIdx == 0)
                        continue;
                    if (slot->flipX) outX = slot->width - 1 - outX;
                    if (slot->flipY) outY = slot->height - 1 - outY;
                    atlas[CtrVideo_Texel(baseX + (unsigned)outX, baseY + (unsigned)outY,
                                         VOXEL_SPRITE_ATLAS_DIM)] =
                        VoxelGrade_RGBA5551(slot->palette[colorIdx]);
                }
            }
        }
    }
}

/*
 * Empty rows under the feet of an object's standing pose: frame 0 of its
 * picture table, facing south. Not of the frame showing: the art moves the
 * whole body between frames, a walk's steps a row lower than the standing
 * pose and a run's standing pose a row higher than its strides, and that
 * row is the bob of the walk. Standing on the ground, the pose seen most
 * meets its shadow; every other frame keeps its offset from it, as on the
 * GBA (VisibleRows).
 */
/* Object event pictures are 4bpp, tiles in rows (1D mapping). Bottom up,
 * the first row with an opaque pixel is the feet. */
static int FootPadOf(const u8 *tiles, int width, int height)
{
    int tilesX = width / 8;

    for (int y = height - 1; y >= 0; --y)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            const u8 *row = tiles + (u32)((y / 8) * tilesX + tx) * TILE_SIZE_4BPP + (y % 8) * 4;

            if (row[0] | row[1] | row[2] | row[3])
                return height - 1 - y;
        }
    }
    return 0;
}

/*
 * The standing pose is read only if it is in memory already. Its picture is
 * an asset of its own, and resolving one that is not loaded reads it off the
 * card - on the render thread, in the middle of the frame: every object that
 * came into view with a picture the game had not shown yet cost 10-20 ms on
 * hardware. Until it is, the pad is measured once on the frame the object
 * first showed (in VRAM already, the slot's own copy): the standing pose,
 * nearly always, and at worst a row off for as long as it takes the game to
 * load the picture - it is looked for again every frame.
 */
static int StandingFootPad(const struct ObjectEventGraphicsInfo *info,
                           const VoxelSpriteSlot *slot, bool *exact)
{
    const struct SpriteFrameImage *frame = &info->images[0];
    int tilesX = info->width / 8, tilesY = info->height / 8;
    u32 size = Port_GetSpriteFrameSize(frame->data, frame->size);
    const u8 *tiles = Port_PeekSpriteFramePointer(frame->data, size, frame->offset);

    *exact = tiles != NULL;
    if (tilesX <= 0 || tilesY <= 0)
        return 0;
    if (tiles == NULL)
    {
        if (slot->color256 || slot->width != info->width || slot->height != info->height
         || slot->sourceBytes < (u32)(tilesX * tilesY) * TILE_SIZE_4BPP)
            return 0;
        return FootPadOf(slot->source, info->width, info->height);
    }
    if (size < (u32)(tilesX * tilesY) * TILE_SIZE_4BPP)
        return 0;
    return FootPadOf(tiles, info->width, info->height);
}

/*
 * Brings a slot up to date with its sprite. Returns true if it was re-decoded,
 * which only happens when something the picture depends on actually changed.
 */
static bool RefreshSlot(unsigned index, const struct Sprite *sprite, uint16_t *atlas)
{
    VoxelSpriteSlot *slot = &sSlots[index];
    /* Static, not automatic: this runs in the VBlank handler and 4.5 KiB of
     * stack frame there is not worth the convenience. */
    static u8 source[VOXEL_SPRITE_MAX_BYTES];
    static u16 palette[256];
    int w, h;
    bool color256 = sprite->oam.bpp != 0;
    bool flipX = sprite->oam.affineMode == ST_OAM_AFFINE_OFF
              && (sprite->oam.matrixNum & 0x08) != 0;
    bool flipY = sprite->oam.affineMode == ST_OAM_AFFINE_OFF
              && (sprite->oam.matrixNum & 0x10) != 0;
    u32 sourceBytes, paletteBytes;
    bool changed;

    GetSpriteDimensions(sprite->oam.shape, sprite->oam.size, &w, &h);
    if (w > (int)VOXEL_SPRITE_SLOT_DIM || h > (int)VOXEL_SPRITE_SLOT_DIM)
        return false;

    sourceBytes = GatherSource(sprite, w, h, color256, source);
    paletteBytes = color256 ? 256 * 2 : 16 * 2;
    /* OBJ palettes are the second half of PLTT; 8bpp uses it as one bank. */
    memcpy(palette, PLTT + 0x200 + (color256 ? 0 : sprite->oam.paletteNum * 32), paletteBytes);

    changed = !slot->valid
           || slot->width != w || slot->height != h
           || slot->color256 != color256
           || slot->flipX != flipX || slot->flipY != flipY
           || slot->tileNum != sprite->oam.tileNum
           || slot->paletteNum != sprite->oam.paletteNum
           || slot->sourceBytes != sourceBytes
           || memcmp(slot->source, source, sourceBytes) != 0
           || memcmp(slot->palette, palette, paletteBytes) != 0;
    if (!changed)
        return false;

    slot->valid = true;
    slot->width = w;
    slot->height = h;
    slot->color256 = color256;
    slot->flipX = flipX;
    slot->flipY = flipY;
    slot->tileNum = sprite->oam.tileNum;
    slot->paletteNum = sprite->oam.paletteNum;
    slot->shape = sprite->oam.shape;
    slot->size = sprite->oam.size;
    slot->sourceBytes = sourceBytes;
    slot->paletteBytes = paletteBytes;
    memcpy(slot->source, source, sourceBytes);
    memcpy(slot->palette, palette, paletteBytes);
    DecodeSlot(slot, index, atlas);
    return true;
}

/* ── Billboards ─────────────────────────────────────────────────────────── */

/*
 * Rows of the sprite the card shows: all but the standing pose's empty rows
 * under the feet, so the card ends at the feet and they are on the ground.
 * Cut off rather than sunk under the ground: a step's feet, a row lower, would
 * be hidden by the terrain there, and the player's x-ray pass (ctr_voxel.c)
 * draws exactly what terrain hides.
 */
static int VisibleRows(const VoxelSpriteSlot *slot)
{
    int pad = slot->footPad < slot->height ? slot->footPad : slot->height - 1;

    return slot->height - pad;
}

/*
 * A card standing on the ground at (cx, cz), moved (offX, offZ) from there
 * and raised by `rise`: an object stands on the centre of its tile, feet on
 * the ground; a field effect that belongs to an object is drawn in that
 * object's place, moved from it as it is on the GBA screen.
 */
static void EmitBillboard(VoxelBuilder *builder, const VoxelSpriteSlot *slot, unsigned index,
                          float cx, float cz, float offX, float offZ, float rise,
                          float rightX, float rightZ, float stretch, float shade)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_DIM;
    int rows = VisibleRows(slot);
    /* Row 0 in memory is v=1, the convention the whole port uses. */
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v1 = 1.0f - (baseY + rows) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float height = rows / VOXEL_PIXELS_PER_TILE * stretch;
    /* On relief the sprite stands where its cell was lifted to, and rides
     * the lattice between cells, so a flight of stairs is climbed. */
    float lift = VoxelRelief_LiftAt(cx, cz) + rise, shift = VoxelRelief_ShiftAt(cx, cz);
    float px = cx + offX, pz = cz + offZ + shift;
    float ax = px - rightX * halfW, az = pz - rightZ * halfW;
    float bx = px + rightX * halfW, bz = pz + rightZ * halfW;

    VoxelBuilder_Quad(builder,
        &(VoxelVertex){ax, lift,          az, u0, v1, shade},
        &(VoxelVertex){bx, lift,          bz, u1, v1, shade},
        &(VoxelVertex){bx, lift + height, bz, u1, v0, shade},
        &(VoxelVertex){ax, lift + height, az, u0, v0, shade});
}

/*
 * A mark on the ground (a ripple, footprints, tyre tracks) lies on it: a
 * card standing up would be a wall of footprints. The top of the picture is
 * north, as on the GBA screen.
 */
#define VOXEL_DECAL_LIFT 0.025f   /* over the ground, under cast shadows (0.03) */

static void EmitDecal(VoxelBuilder *builder, const VoxelSpriteSlot *slot, unsigned index,
                      float cx, float cz, float shade)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v1 = 1.0f - (baseY + slot->height) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float halfH = slot->height / VOXEL_PIXELS_PER_TILE * 0.5f;
    float y = VoxelRelief_LiftAt(cx, cz) + VOXEL_DECAL_LIFT;
    float z = cz + VoxelRelief_ShiftAt(cx, cz);

    VoxelBuilder_Quad(builder,
        &(VoxelVertex){cx - halfW, y, z + halfH, u0, v1, shade},
        &(VoxelVertex){cx + halfW, y, z + halfH, u1, v1, shade},
        &(VoxelVertex){cx + halfW, y, z - halfH, u1, v0, shade},
        &(VoxelVertex){cx - halfW, y, z - halfH, u0, v0, shade});
}

#if CTR_VOXEL_LIGHTING
/*
 * The billboard's own picture laid on the ground along the sun: the feet stay
 * where they are and every row of the sprite slides away from the sun by its
 * height, which is exactly where the fixed sun would put the shadow of a flat
 * card standing there. Same texture and texels as the sprite, so it walks,
 * turns and animates with it; one quad per object and no clipping work.
 *
 * The strength goes out in `shade`, which the shadow pass reads as alpha
 * (ctr_voxel.c). An object already in the scenery's shadow has no sun to
 * cast with, so its shadow fades out with the light it receives.
 */
#define VOXEL_CAST_SHADOW_ALPHA 0.36f
#define VOXEL_CAST_SHADOW_LIFT 0.03f    /* over decals (0.02) */
#define VOXEL_CAST_SHADOW_LIT 0.70f     /* sample at or below: no sun */

static void GetDynamicShadowVector(float *outSx, float *outSz, float *outAlpha, float height)
{
    if (!CtrSettings_DayNight())
    {
        *outSx = VOXEL_SUN_DX * height;
        *outSz = VOXEL_SUN_DZ * height;
        *outAlpha = VOXEL_CAST_SHADOW_ALPHA;
    }
    else
    {
        float t = CtrPlatform_GetDayTime();

        if (t >= 5.5f && t <= 19.5f)
        {
            /* Daytime: Sun progresses from East (+X) at dawn to West (-X) at dusk.
             * 5:30 (Dawn) -> 12:30 (Noon) -> 19:30 (Dusk) */
            float s = (t - 5.5f) / 14.0f;
            float angle = s * 3.1415926535f;
            float sinA = sinf(angle); /* 0 at dawn/dusk, 1.0 at midday */
            float cosA = cosf(angle); /* +1 at sunrise (East), 0 at midday, -1 at sunset (West) */

            /* Shadow projects opposite to sun position:
             * Morning (Sun in East): shadow points West (-X)
             * Evening (Sun in West): shadow points East (+X)
             * Midday: shadow is short under feet, slight North bias */
            float stretch = 0.45f + 0.80f * (1.0f - sinA);
            *outSx = -cosA * stretch * height;
            *outSz = (0.20f + 0.35f * (1.0f - sinA)) * height;
            *outAlpha = 0.26f + 0.12f * sinA;
        }
        else
        {
            /* Night: Soft moon cast */
            float nt = (t < 5.5f) ? (t + 4.5f) : (t - 19.5f);
            float ns = nt / 10.0f;
            float nAngle = ns * 3.1415926535f;
            float sinM = sinf(nAngle);
            float cosM = cosf(nAngle);

            *outSx = -cosM * 0.60f * height;
            *outSz = 0.30f * height;
            *outAlpha = 0.14f * sinM;
        }
    }

    /* Weather shadow attenuation: rain, storms, fog, sandstorm diffuse the direct sun cast */
    u8 weather = GetCurrentWeather();
    switch (weather)
    {
    case WEATHER_RAIN:
        *outAlpha *= 0.50f;
        break;
    case WEATHER_RAIN_THUNDERSTORM:
    case WEATHER_DOWNPOUR:
        *outAlpha *= 0.30f;
        break;
    case WEATHER_FOG_HORIZONTAL:
    case WEATHER_FOG_DIAGONAL:
        *outAlpha *= 0.35f;
        break;
    case WEATHER_SANDSTORM:
        *outAlpha *= 0.40f;
        break;
    case WEATHER_SHADE:
        *outAlpha *= 0.55f;
        break;
    case WEATHER_DROUGHT:
        *outAlpha = fminf(*outAlpha * 1.25f, 0.45f);
        break;
    default:
        break;
    }
}

static void EmitCastShadow(VoxelBuilder *shadows, const VoxelSpriteSlot *slot, unsigned index,
                           float worldX, float worldZ, float rightX, float rightZ, float stretch,
                           float light)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_DIM;
    /* The rows the card shows (VisibleRows), from the feet: they meet their
     * own shadow. */
    int rows = VisibleRows(slot);
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v1 = 1.0f - (baseY + rows) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float height = rows / VOXEL_PIXELS_PER_TILE * stretch;
    float cx = worldX + 0.5f, cz = worldZ + 0.5f;
    float ax = cx - rightX * halfW, az = cz - rightZ * halfW;
    float bx = cx + rightX * halfW, bz = cz + rightZ * halfW;
    float sx, sz, maxAlpha;
    GetDynamicShadowVector(&sx, &sz, &maxAlpha, height);
    /* Each corner on the ground under it: at a face's foot the ground under
     * the shadow's far end is not the ground under the feet. */
    float ya = VoxelRelief_LiftAt(ax, az) + VOXEL_CAST_SHADOW_LIFT;
    float yb = VoxelRelief_LiftAt(bx, bz) + VOXEL_CAST_SHADOW_LIFT;
    float yc = VoxelRelief_LiftAt(bx + sx, bz + sz) + VOXEL_CAST_SHADOW_LIFT;
    float yd = VoxelRelief_LiftAt(ax + sx, az + sz) + VOXEL_CAST_SHADOW_LIFT;
    float za = VoxelRelief_ShiftAt(ax, az), zb = VoxelRelief_ShiftAt(bx, bz);
    float zc = VoxelRelief_ShiftAt(bx + sx, bz + sz), zd = VoxelRelief_ShiftAt(ax + sx, az + sz);
    float strength = (light - VOXEL_CAST_SHADOW_LIT) / (1.0f - VOXEL_CAST_SHADOW_LIT);

    if (strength <= 0.0f)
        return;
    if (strength > 1.0f)
        strength = 1.0f;
    strength *= maxAlpha;
    VoxelBuilder_Quad(shadows,
        &(VoxelVertex){ax,      ya, az + za,      u0, v1, strength},
        &(VoxelVertex){bx,      yb, bz + zb,      u1, v1, strength},
        &(VoxelVertex){bx + sx, yc, bz + sz + zc, u1, v0, strength},
        &(VoxelVertex){ax + sx, yd, az + sz + zd, u0, v0, strength});
}

static void EmitIndoorContactShadow(VoxelBuilder *shadows, const VoxelSpriteSlot *slot, unsigned index,
                                    float worldX, float worldZ, float rightX, float rightZ)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_DIM;
    int rows = VisibleRows(slot);
    if (rows <= 0)
        return;
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v1 = 1.0f - (baseY + rows) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.40f;
    float cx = worldX + 0.5f, cz = worldZ + 0.5f;

    /* Soft ambient contact pool under feet, slightly elliptical */
    float forwardX = -rightZ;
    float forwardZ =  rightX;
    float halfD = 0.20f;

    float ax = cx - rightX * halfW - forwardX * halfD;
    float az = cz - rightZ * halfW - forwardZ * halfD;
    float bx = cx + rightX * halfW - forwardX * halfD;
    float bz = cz + rightZ * halfW - forwardZ * halfD;
    float cx2 = cx + rightX * halfW + forwardX * halfD;
    float cz2 = cz + rightZ * halfW + forwardZ * halfD;
    float dx = cx - rightX * halfW + forwardX * halfD;
    float dz = cz - rightZ * halfW + forwardZ * halfD;

    float ya = VoxelRelief_LiftAt(ax, az) + VOXEL_CAST_SHADOW_LIFT;
    float yb = VoxelRelief_LiftAt(bx, bz) + VOXEL_CAST_SHADOW_LIFT;
    float yc = VoxelRelief_LiftAt(cx2, cz2) + VOXEL_CAST_SHADOW_LIFT;
    float yd = VoxelRelief_LiftAt(dx, dz) + VOXEL_CAST_SHADOW_LIFT;
    float za = VoxelRelief_ShiftAt(ax, az), zb = VoxelRelief_ShiftAt(bx, bz);
    float zc = VoxelRelief_ShiftAt(cx2, cz2), zd = VoxelRelief_ShiftAt(dx, dz);

    float strength = 0.22f;
    VoxelBuilder_Quad(shadows,
        &(VoxelVertex){ax,  ya, az + za,  u0, v1, strength},
        &(VoxelVertex){bx,  yb, bz + zb,  u1, v1, strength},
        &(VoxelVertex){cx2, yc, cz2 + zc, u1, v0, strength},
        &(VoxelVertex){dx,  yd, dz + zd,  u0, v0, strength});
}
#endif

/* Require every map cell under the actual, relief-shifted quad to be
 * reflective. Checking only the first south cell let a long reflection spill
 * across shorelines into grass. The small AABB is conservative when yawed. */
static bool IsReflectiveFootprint(float ax, float az, float bx, float bz, float length)
{
    float minX = fminf(ax, bx), maxX = fmaxf(ax, bx);
    float minZ = fminf(az, bz), maxZ = fmaxf(az, bz) + length;
    int firstX = (int)floorf(minX + 0.0001f);
    int lastX = (int)floorf(maxX - 0.0001f);
    int firstZ = (int)floorf(minZ + 0.0001f);
    int lastZ = (int)floorf(maxZ - 0.0001f);

    for (int z = firstZ; z <= lastZ; ++z)
    {
        for (int x = firstX; x <= lastX; ++x)
        {
            if (!VoxelWorld_IsVisibleReflectiveSurface(x, z))
                return false;
        }
    }
    return true;
}

/* One quad per reflected object. Grow only through cells that the world marks
 * as water or ice; the original ground effect merely says water is nearby. */
static void EmitReflection(VoxelBuilder *reflections, const VoxelSpriteSlot *slot,
                           unsigned index, float worldX, float worldZ,
                           float rightX, float rightZ, float stretch)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v1 = 1.0f - (baseY + slot->height) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float cardLength = slot->height / VOXEL_PIXELS_PER_TILE * stretch;
    float fullLength = cardLength;
    float length = 0.0f;
    float cx = worldX + 0.5f, nearZ = worldZ + 1.0f;
    float shift = VoxelRelief_ShiftAt(cx, nearZ + 0.5f);
    float ax = cx - rightX * halfW, az = nearZ - rightZ * halfW + shift;
    float bx = cx + rightX * halfW, bz = nearZ + rightZ * halfW + shift;
    float y = VoxelRelief_LiftAt(cx, nearZ + 0.5f) + 0.035f;

    if (fullLength > 1.75f) fullLength = 1.75f;
    for (float next = 0.25f; next <= fullLength + 0.0001f; next += 0.25f)
    {
        if (!IsReflectiveFootprint(ax, az, bx, bz, next))
            break;
        length = next;
    }
    if (length < 0.25f)
        return;
    float vFar = v1 + (v0 - v1) * (length / cardLength);
    VoxelBuilder_Quad(reflections,
        &(VoxelVertex){ax, y, az, u0, v1, 1.0f},
        &(VoxelVertex){bx, y, bz, u1, v1, 1.0f},
        &(VoxelVertex){bx, y, bz + length, u1, vFar, 1.0f},
        &(VoxelVertex){ax, y, az + length, u0, vFar, 1.0f});
}

/* ── Field effects ──────────────────────────────────────────────────────── */

/*
 * Every other sprite on the map: the Pokémon the player surfs on, grass
 * shaking under a step, a trainer's "!", splashes, sand piles. The GBA draws
 * them over the map at a screen position; here each one is a card like the
 * objects' own, in an atlas slot no object event is using.
 *
 * One drawn over an object belongs to it (the surf mon, the grass, the icon
 * over a head) and stands in that object's place, moved from it by as much as
 * on the GBA screen, just in front of it or just behind it as the GBA orders
 * the two. One drawn behind the object and below its feet is what the object
 * rides - the surf mon - and lifts it: the rider sits on it as on the GBA, and
 * the mon floats on the water instead of sinking under it. A sprite on its own
 * stands where its feet are drawn, placed from the player; a mark on the
 * ground lies on it (EmitDecal).
 *
 * Not here: object shadows and reflections, which this renderer draws its own
 * way; the weather, drawn over the view (ComposeVoxelOverlay); and sprites
 * placed on the screen rather than on the map (the mon shown for a field
 * move, the bird of Fly and its rider), drawn over the view too.
 */
bool8 CtrSprite_IsVoxelWeather(const struct Sprite *sprite);   /* sprite.c */

#define VOXEL_EFFECTS_MAX VOXEL_SPRITE_SLOTS
/* How far an effect card sits in front of or behind the object it belongs to. */
#define VOXEL_EFFECT_DEPTH 0.04f

typedef struct
{
    bool drawn;
    float worldX, worldZ;    /* the tile, world */
    int screenX;             /* GBA screen: centre, */
    int top, base, feet;     /* top edge, bottom edge, feet (no y2) */
    const struct Sprite *sprite;
    float rise;              /* onto what it rides */
    float shade;
    bool outdoor;            /* casts a shadow */
} VoxelObjectCard;

typedef struct
{
    const struct Sprite *sprite;
    unsigned slot;
    int owner;               /* object index, or -1 */
    bool front, decal, tile;
} VoxelEffectCard;

static bool IsTemplate(const struct SpriteTemplate *template, unsigned first, unsigned last)
{
    for (unsigned i = first; i <= last; ++i)
        if (template == gFieldEffectObjectTemplatePointers[i])
            return true;
    return false;
}

/* Grass that rustles where it was stepped on keeps that tile in its data
 * (field_effect_helpers.c: sX, sY) and stays there while its object walks on:
 * placed from the object, it floated in the object's plane, off its tile. */
static bool IsTileEffect(const struct SpriteTemplate *template)
{
    return template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_TALL_GRASS]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_LONG_GRASS];
}

static bool IsDecal(const struct SpriteTemplate *template)
{
    return template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_RIPPLE]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_SAND_FOOTPRINTS]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_DEEP_SAND_FOOTPRINTS]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_BIKE_TIRE_TRACKS];
}

/*
 * A slot for field effect sprite `id`: the one it had, if no object has taken
 * it back, or else one whose object event is not in use. -1 when every slot
 * is taken.
 */
static int EffectSlot(unsigned id, bool claimed[VOXEL_SPRITE_SLOTS])
{
    int found = -1;

    for (unsigned s = 0; s < VOXEL_SPRITE_SLOTS; ++s)
    {
        if (claimed[s]) continue;
        if (sSlotEffect[s] == id + 1) { found = (int)s; break; }
        if (found < 0 && sSlotEffect[s] == 0) found = (int)s;
    }
    if (found < 0)
        return -1;
    if (sSlotEffect[found] != id + 1)
    {
        sSlotEffect[found] = (u8)(id + 1);
        sSlots[found].valid = false;
        sSlots[found].footPad = 0;
        sSlots[found].footPadImages = NULL;
    }
    claimed[found] = true;
    return found;
}

/* Drawn over `object` on the GBA screen: sprites come first in OAM, and so
 * on top, by priority and then subpriority. */
static bool DrawnOver(const struct Sprite *sprite, const struct Sprite *object)
{
    if (sprite->oam.priority != object->oam.priority)
        return sprite->oam.priority < object->oam.priority;
    return sprite->subpriority <= object->subpriority;
}

static int SpriteHeight(const struct Sprite *sprite)
{
    int w, h;

    GetSpriteDimensions(sprite->oam.shape, sprite->oam.size, &w, &h);
    return h;
}

/* The object an effect is drawn over, or -1: its centre inside the object's
 * picture, the nearest to the object's middle. */
static int EffectOwner(const struct Sprite *sprite, const VoxelObjectCard *objects)
{
    int x = sprite->x + sprite->x2;
    int y = sprite->y + sprite->y2 + sprite->centerToCornerVecY + SpriteHeight(sprite) / 2;
    int best = -1, bestDistance = 0x7fff;

    for (unsigned i = 0; i < VOXEL_SPRITE_SLOTS; ++i)
    {
        const VoxelObjectCard *object = &objects[i];
        int y2, distance;

        if (!object->drawn || abs(x - object->screenX) > 8)
            continue;
        y2 = object->sprite->y2;
        if (y < object->top + y2 || y > object->base + y2)
            continue;
        distance = abs(y - (object->top + object->base) / 2 - y2);
        if (distance < bestDistance)
        {
            best = (int)i;
            bestDistance = distance;
        }
    }
    return best;
}

static void DecodeDoorFrame(uint16_t *atlas, const u8 *tiles, unsigned offset,
                            int size, const u8 *paletteNums)
{
    unsigned baseX = (VOXEL_DOOR_SLOT % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (VOXEL_DOOR_SLOT / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    const u8 *frameTiles = tiles + offset;
    int totalTiles = (size == 2) ? 16 : 8;

    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            atlas[CtrVideo_Texel(baseX + (unsigned)x, baseY + (unsigned)y,
                                 VOXEL_SPRITE_ATLAS_DIM)] = 0;

    for (int t = 0; t < totalTiles; ++t)
    {
        const u8 *tileData = frameTiles + t * 32;
        int tileX, tileY;
        u8 palIndex;

        if (size == 2)
        {
            int m = t / 4;
            int subtile = t % 4;
            int mx = (m >= 2) ? 1 : 0;
            int my = (m % 2);
            tileX = mx * 16 + (subtile % 2) * 8;
            tileY = my * 16 + (subtile / 2) * 8;
            palIndex = paletteNums[my * 4 + subtile];
        }
        else
        {
            int subtile = t % 4;
            int my = t / 4;
            tileX = (subtile % 2) * 8;
            tileY = my * 16 + (subtile / 2) * 8;
            palIndex = paletteNums[t];
        }

        const u16 *pal = (const u16 *)PLTT + (palIndex * 16);

        for (unsigned py = 0; py < 8; ++py)
        {
            for (unsigned px = 0; px < 8; ++px)
            {
                unsigned index8 = py * 8 + px;
                unsigned colorIdx = (tileData[index8 / 2] >> ((px & 1) * 4)) & 0xF;
                if (colorIdx == 0)
                    continue;

                int outX = tileX + (int)px;
                int outY = tileY + (int)py;
                uint16_t texel = VoxelGrade_RGBA5551(pal[colorIdx]);
                atlas[CtrVideo_Texel(baseX + (unsigned)outX, baseY + (unsigned)outY,
                                     VOXEL_SPRITE_ATLAS_DIM)] = texel;
            }
        }
    }
}

static void EmitDoorQuad(VoxelBuilder *builder, int mapX, int mapY, int size)
{
    float doorWorldX = (float)(mapX - MAP_OFFSET);
    float doorWorldZ = (float)(mapY - MAP_OFFSET);

    if (VoxelWorld_InstanceCount() > 0)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(0);
        doorWorldX += (float)inst->originX;
        doorWorldZ += (float)inst->originY;
    }

    float lift = VoxelRelief_LiftAt(doorWorldX + 0.5f, doorWorldZ + 0.5f);
    float shift = VoxelRelief_ShiftAt(doorWorldX + 0.5f, doorWorldZ + 0.5f);

    unsigned baseX = (VOXEL_DOOR_SLOT % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (VOXEL_DOOR_SLOT / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    int pixelW = size * 16;
    int pixelH = 32;

    float u0 = (float)baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
    float u1 = (float)(baseX + pixelW) / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v0 = 1.0f - (float)baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
    float v1 = 1.0f - (float)(baseY + pixelH) / (float)VOXEL_SPRITE_ATLAS_DIM;

    float x0 = doorWorldX;
    float x1 = doorWorldX + (float)size;
    float y0 = lift;
    float y1 = lift + 2.0f;
    float z = doorWorldZ + 1.0f + shift + 0.005f;

    VoxelBuilder_Quad(builder,
        &(VoxelVertex){x0, y0, z, u0, v1, 1.0f},
        &(VoxelVertex){x1, y0, z, u1, v1, 1.0f},
        &(VoxelVertex){x1, y1, z, u1, v0, 1.0f},
        &(VoxelVertex){x0, y1, z, u0, v0, 1.0f});
}

static bool GetPokecenterPositions(float *nurseX, float *nurseZ, float *machineX, float *machineZ)
{
    for (int i = 0; i < OBJECT_EVENTS_COUNT; ++i)
    {
        if (gObjectEvents[i].active && gObjectEvents[i].graphicsId == OBJ_EVENT_GFX_NURSE)
        {
            float nx = (float)(gObjectEvents[i].currentCoords.x - MAP_OFFSET);
            float nz = (float)(gObjectEvents[i].currentCoords.y - MAP_OFFSET);
            if (VoxelWorld_InstanceCount() > 0)
            {
                const VoxelMapInstance *inst = VoxelWorld_Instance(0);
                nx += (float)inst->originX;
                nz += (float)inst->originY;
            }
            if (nurseX) *nurseX = nx;
            if (nurseZ) *nurseZ = nz;
            if (machineX) *machineX = nx - 1.0f;
            if (machineZ) *machineZ = nz;
            return true;
        }
    }
    return false;
}

unsigned VoxelEntities_Emit(VoxelBuilder *builder, uint16_t *atlas, const VoxelCamera *camera,
                            VoxelBuilder *shadows, VoxelBuilder *reflections)
{
    /* The billboards turn about the vertical axis to face the camera. With the
     * default yaw of 0 this is (1,0,0), the same plane the reference uses.
     * The card stands upright and the camera looks down on it, so on screen
     * its height comes out cos(pitch) short. That is made up half on each
     * side: the width comes down by sqrt(cos(pitch)) and the height goes up
     * by as much, so on screen both are sqrt(cos(pitch)) of the sprite's
     * (0.875 at 40 degrees). The proportions are the sprite's, and so is the
     * area on screen of the plain card. All of it off the width left the
     * characters small, all of it on the height big. Still upright, so it
     * sorts against walls as before. Shadow and reflection are the card's
     * own, so they take the same size. */
    float yawRad = camera->yaw * (3.14159265358979323846f / 180.0f);
    float narrow = sqrtf(cosf(camera->pitch * (3.14159265358979323846f / 180.0f)));
    float stretch = 1.0f / narrow;
    float rightX = cosf(yawRad) * narrow, rightZ = -sinf(yawRad) * narrow;
    /* Towards the camera, along the ground. */
    float towardX = sinf(yawRad), towardZ = cosf(yawRad);
    float pixel = stretch / VOXEL_PIXELS_PER_TILE;
    /* Forward camera bias prevents character occlusion by adjacent wall corners */
    const float billboardBias = 0.25f;
    /* Static, not automatic: this runs in the VBlank handler. */
    static VoxelObjectCard objects[VOXEL_SPRITE_SLOTS];
    static VoxelEffectCard effects[VOXEL_EFFECTS_MAX];
    static bool objectSprite[MAX_SPRITES];
    static u16 objectTiles[OBJECT_EVENTS_COUNT];
    bool claimed[VOXEL_SPRITE_SLOTS] = {false};
    unsigned objectTileCount = 0, effectCount = 0;
    unsigned updates = 0;
    sPlayerVertexFirst = -1;

    /* The lighting caches are the chunks' own, valid until the world
     * changes (ctr_voxel.c resets them then), so they are not reset here. */
#if !CTR_VOXEL_LIGHTING
    (void)shadows;
#endif
    memset(objectSprite, 0, sizeof(objectSprite));
    for (unsigned i = 0; i < VOXEL_SPRITE_SLOTS && i < OBJECT_EVENTS_COUNT; ++i)
    {
        const struct ObjectEvent *obj = &gObjectEvents[i];
        const struct Sprite *sprite;
        VoxelObjectCard *card = &objects[i];
        float worldX = 0.0f, worldZ = 0.0f;
        float shade = 1.0f;
        bool outdoor = false;

        card->drawn = false;
        if (!obj->active)
            continue;
        /* An object in use takes its slot back from any field effect. */
        claimed[i] = true;
        if (sSlotEffect[i] != 0)
        {
            sSlotEffect[i] = 0;
            sSlots[i].valid = false;
            sSlots[i].footPadImages = NULL;
        }
        if (obj->spriteId >= MAX_SPRITES)
            continue;
        sprite = &gSprites[obj->spriteId];
        objectSprite[obj->spriteId] = true;
        objectTiles[objectTileCount++] = sprite->oam.tileNum;
        /*
         * Not sprite->invisible: the game also sets that for an object that
         * has left the 2D field of view (offScreen), and the 3D camera sees
         * twice as far north. Only the object's own flag hides it here.
         */
        if (obj->invisible)
            continue;
        /* Carried on the screen instead of the map - the rider of Fly's
         * bird - it is drawn with the screen's sprites, over the view. */
        if (!sprite->coordOffsetEnabled)
            continue;

        GetObjectWorldPos(obj, sprite, &worldX, &worldZ);
        /* Object coordinates are map-local; the mesh is in world space. */
        if (VoxelWorld_InstanceCount() > 0)
        {
            const VoxelMapInstance *inst = VoxelWorld_Instance(0);

            worldX += (float)inst->originX;
            worldZ += (float)inst->originY;
        }
        /* Active events may be dozens of tiles beyond the camera, including
         * across map connections. They cannot appear in this view, and packing
         * them relative to the camera's tile overflows the 16-bit format. */
        if (fabsf(worldX - camera->targetX) > 24.0f
         || fabsf(worldZ - camera->targetZ) > 24.0f)
            continue;
        if (RefreshSlot(i, sprite, atlas))
            ++updates;
        if (!sSlots[i].valid)
            continue;
        {
            const struct ObjectEventGraphicsInfo *info = GetObjectEventGraphicsInfo(obj->graphicsId);

            bool changed = sSlots[i].footPadImages != info->images;

            if (changed || !sSlots[i].footPadExact)
            {
                bool exact = true;
                int pad = info->images != NULL ? StandingFootPad(info, &sSlots[i], &exact) : 0;

                sSlots[i].footPadImages = info->images;
                /* A provisional pad is taken once, never from each frame of a
                 * walk: that would undo its bob. */
                if (changed || exact)
                    sSlots[i].footPad = pad;
                sSlots[i].footPadExact = exact;
            }
        }
#if CTR_VOXEL_LIGHTING
        {
            const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt((int)floorf(worldX + 0.5f),
                                                                   (int)floorf(worldZ + 0.5f));
            outdoor = inst != NULL && !inst->indoor;
            if (outdoor)
                shade = VoxelLighting_Sample(worldX + 0.5f, 0.75f, worldZ + 0.5f);
        }
#endif
        card->drawn = true;
        card->worldX = worldX;
        card->worldZ = worldZ;
        card->screenX = sprite->x + sprite->x2;
        card->top = sprite->y + sprite->centerToCornerVecY;
        card->base = card->top + sSlots[i].height;
        card->feet = card->base - (sSlots[i].height - VisibleRows(&sSlots[i]));
        card->sprite = sprite;
        card->rise = 0.0f;
        card->shade = shade;
        card->outdoor = outdoor;
    }

    for (unsigned id = 0; id < MAX_SPRITES && effectCount < VOXEL_EFFECTS_MAX; ++id)
    {
        const struct Sprite *sprite = &gSprites[id];
        VoxelEffectCard *effect = &effects[effectCount];
        unsigned t;
        int w, h, slot;

        if (!sprite->inUse || sprite->invisible || !sprite->coordOffsetEnabled || objectSprite[id])
            continue;
        if (IsTemplate(sprite->template, FLDEFFOBJ_SHADOW_S, FLDEFFOBJ_SHADOW_XL)
         || CtrSprite_IsVoxelWeather(sprite))
            continue;
        /* A reflection is a copy of its object's picture. */
        for (t = 0; t < objectTileCount && objectTiles[t] != sprite->oam.tileNum; ++t)
            ;
        if (t < objectTileCount)
            continue;
        GetSpriteDimensions(sprite->oam.shape, sprite->oam.size, &w, &h);
        if (w > (int)VOXEL_SPRITE_SLOT_DIM || h > (int)VOXEL_SPRITE_SLOT_DIM)
            continue;
        slot = EffectSlot(id, claimed);
        if (slot < 0)
            break;
        effect->sprite = sprite;
        effect->slot = (unsigned)slot;
        effect->decal = IsDecal(sprite->template);
        effect->tile = IsTileEffect(sprite->template);
        effect->owner = effect->decal || effect->tile ? -1 : EffectOwner(sprite, objects);
        effect->front = effect->owner < 0 || DrawnOver(sprite, objects[effect->owner].sprite);
        /* Behind its object and below its feet: what the object rides. Only
         * the surf mon is ridden; grass behind a walker's feet, as it steps
         * onto or off a tile of it, lifted the walker a moment. */
        if (effect->owner >= 0 && !effect->front
         && sprite->template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_SURF_BLOB])
        {
            VoxelObjectCard *object = &objects[effect->owner];
            int base = sprite->y + sprite->centerToCornerVecY + h;
            float need = (base - object->feet) * pixel;

            if (need > object->rise)
                object->rise = need;
        }
        ++effectCount;
    }
    /* Slots neither an object nor an effect wants this frame. */
    for (unsigned s = 0; s < VOXEL_SPRITE_SLOTS; ++s)
    {
        if (claimed[s]) continue;
        sSlotEffect[s] = 0;
        sSlots[s].valid = false;
    }

    for (unsigned i = 0; i < VOXEL_SPRITE_SLOTS; ++i)
    {
        VoxelObjectCard *card = &objects[i];
        float rise;

        if (!card->drawn)
            continue;
        /* Riding: up and down with what it rides, plus sprite y2 jump/hop elevation arc. */
        rise = (card->rise > 0.0f ? card->rise : 0.0f) - card->sprite->y2 * pixel;
#if CTR_VOXEL_LIGHTING
        /* Riding, it is off the ground: a shadow cast from its feet would be
         * left behind on the water. The GBA gives the surf mon none either. */
        if (shadows != NULL && card->rise <= 0.0f)
        {
            if (card->outdoor)
                EmitCastShadow(shadows, &sSlots[i], i, card->worldX, card->worldZ, rightX, rightZ,
                               stretch, card->shade);
            else
                EmitIndoorContactShadow(shadows, &sSlots[i], i, card->worldX, card->worldZ, rightX, rightZ);
        }
#endif
        if (reflections != NULL && gObjectEvents[i].hasReflection)
            EmitReflection(reflections, &sSlots[i], i, card->worldX, card->worldZ,
                           rightX, rightZ, stretch);
        unsigned first = builder->count;
        EmitBillboard(builder, &sSlots[i], i, card->worldX + 0.5f, card->worldZ + 0.5f,
                      towardX * billboardBias, towardZ * billboardBias, rise,
                      rightX, rightZ, stretch, card->shade);
        if (i == gPlayerAvatar.objectEventId && builder->count == first + 6)
            sPlayerVertexFirst = (int)first;
    }

    for (unsigned e = 0; e < effectCount; ++e)
    {
        const VoxelEffectCard *effect = &effects[e];
        const struct Sprite *sprite = effect->sprite;
        const VoxelObjectCard *reference;
        int h, x, base;

        if (RefreshSlot(effect->slot, sprite, atlas))
            ++updates;
        if (!sSlots[effect->slot].valid)
            continue;
        h = sSlots[effect->slot].height;
        x = sprite->x + sprite->x2;
        base = sprite->y + sprite->centerToCornerVecY + h;
        if (effect->tile)
        {
            /* Lying on its own tile over the ground's drawing of it, which it
             * animates: stood up as a card it read as a flat picture on top
             * of the tile instead of the tile itself. */
            float cx = (float)(sprite->data[1] - MAP_OFFSET) + 0.5f;
            float cz = (float)(sprite->data[2] - MAP_OFFSET) + 0.5f;
            float shade = 1.0f;

            if (gPlayerAvatar.objectEventId < VOXEL_SPRITE_SLOTS
             && objects[gPlayerAvatar.objectEventId].drawn)
                shade = objects[gPlayerAvatar.objectEventId].shade;

            if (VoxelWorld_InstanceCount() > 0)
            {
                cx += (float)VoxelWorld_Instance(0)->originX;
                cz += (float)VoxelWorld_Instance(0)->originY;
            }
            if (fabsf(cx - camera->targetX) > 24.0f || fabsf(cz - camera->targetZ) > 24.0f)
                continue;
            EmitDecal(builder, &sSlots[effect->slot], effect->slot, cx, cz, shade);
            continue;
        }
        if (effect->owner >= 0)
        {
            const VoxelObjectCard *object = &objects[effect->owner];
            float depth = (effect->front ? VOXEL_EFFECT_DEPTH : -VOXEL_EFFECT_DEPTH) + billboardBias;
            float along = (x - object->screenX) / VOXEL_PIXELS_PER_TILE;
            /* The owner's feet on the ground, lifted by what it rides. */
            float rise = object->rise + (object->feet - base - sprite->y2) * pixel;

            EmitBillboard(builder, &sSlots[effect->slot], effect->slot,
                          object->worldX + 0.5f, object->worldZ + 0.5f,
                          rightX * along + towardX * depth, rightZ * along + towardZ * depth,
                          rise, rightX, rightZ, stretch, object->shade);
            continue;
        }
        /* On its own: placed from the player, whose feet are on the centre of
         * its tile and whose picture ends at the tile's bottom edge. */
        reference = gPlayerAvatar.objectEventId < VOXEL_SPRITE_SLOTS
                  ? &objects[gPlayerAvatar.objectEventId] : NULL;
        if (reference == NULL || !reference->drawn)
            continue;
        {
            float cx = reference->worldX + 0.5f + (x - reference->screenX) / VOXEL_PIXELS_PER_TILE;
            float cz;

            if (effect->decal)
            {
                /* Its middle on the ground: a tile's middle is 8 px above
                 * its bottom edge. */
                cz = reference->worldZ + 0.5f
                   + (base - h / 2 - (reference->base - 8)) / VOXEL_PIXELS_PER_TILE;
                if (fabsf(cx - camera->targetX) > 24.0f || fabsf(cz - camera->targetZ) > 24.0f)
                    continue;
                EmitDecal(builder, &sSlots[effect->slot], effect->slot, cx, cz, reference->shade);
                continue;
            }
            cz = reference->worldZ + 0.5f + (base - reference->base) / VOXEL_PIXELS_PER_TILE;
            if (fabsf(cx - camera->targetX) > 24.0f || fabsf(cz - camera->targetZ) > 24.0f)
                continue;
            EmitBillboard(builder, &sSlots[effect->slot], effect->slot, cx, cz,
                          towardX * billboardBias, towardZ * billboardBias,
                          -sprite->y2 * pixel, rightX, rightZ, stretch, reference->shade);
        }
    }

    /* ── Door animation ─────────────────────────────────────────────────── */
    {
        int doorX = 0, doorY = 0, doorSize = 1;
        const u8 *doorTiles = NULL;
        const u8 *doorPalettes = NULL;
        unsigned doorFrameOffset = 0;
        int partnerX = -1, partnerY = -1;
        static int sLastMapGroup = -1, sLastMapNum = -1;
        static unsigned sLastDecodedOffset = 0xFFFFFFFF;
        static const u8 *sLastDecodedTiles = NULL;
        int curGroup = -1, curNum = -1;

        VoxelWorld_GetLocation(&curGroup, &curNum);
        if (curGroup != sLastMapGroup || curNum != sLastMapNum)
        {
            sLastMapGroup = curGroup;
            sLastMapNum = curNum;
            sDoorAnimState.active = false;
            sLastDecodedOffset = 0xFFFFFFFF;
            sLastDecodedTiles = NULL;
            ResetCurrentDoorAnimationInfo();
        }

        bool doorRunning = GetCurrentDoorAnimationInfo(&doorX, &doorY, &doorSize,
                                                       &doorTiles, &doorPalettes,
                                                       &doorFrameOffset, &partnerX, &partnerY);
        if (doorRunning)
        {
            sDoorAnimState.active = true;
            sDoorAnimState.x = doorX;
            sDoorAnimState.y = doorY;
            sDoorAnimState.size = doorSize;
            sDoorAnimState.tiles = doorTiles;
            sDoorAnimState.paletteNums = doorPalettes;
            sDoorAnimState.frameOffset = doorFrameOffset;
            sDoorAnimState.partnerX = partnerX;
            sDoorAnimState.partnerY = partnerY;
            if (doorFrameOffset != sLastDecodedOffset || doorTiles != sLastDecodedTiles)
            {
                sLastDecodedOffset = doorFrameOffset;
                sLastDecodedTiles = doorTiles;
                DecodeDoorFrame(atlas, doorTiles, doorFrameOffset, doorSize, doorPalettes);
                ++updates;
            }
        }
        else
        {
            sDoorAnimState.active = false;
            sLastDecodedOffset = 0xFFFFFFFF;
            sLastDecodedTiles = NULL;
        }

        if (sDoorAnimState.active)
        {
            float px = 0.0f, pz = 0.0f;
            VoxelEntities_GetPlayerWorldPos(&px, &pz);
            float dx = fabsf(px - ((float)(sDoorAnimState.x - MAP_OFFSET) + 0.5f));
            float dz = fabsf(pz - ((float)(sDoorAnimState.y - MAP_OFFSET) + 0.5f));
            if (dx > 4.0f || dz > 4.0f)
            {
                sDoorAnimState.active = false;
                sLastDecodedOffset = 0xFFFFFFFF;
                sLastDecodedTiles = NULL;
                ResetCurrentDoorAnimationInfo();
            }
            else
            {
                EmitDoorQuad(builder, sDoorAnimState.x, sDoorAnimState.y, sDoorAnimState.size);
                if (sDoorAnimState.partnerX >= 0 && sDoorAnimState.partnerY >= 0)
                    EmitDoorQuad(builder, sDoorAnimState.partnerX, sDoorAnimState.partnerY, sDoorAnimState.size);
            }
        }
    }

    /* ── Pokecenter healing animation ─────────────────────────────────────── */
    {
        float nurseX = 0.0f, nurseZ = 0.0f, machineX = 0.0f, machineZ = 0.0f;
        bool hasNurse = false;
        bool pokeballRefreshed = false;
        bool monitorRefreshed = false;

        for (unsigned id = 0; id < MAX_SPRITES; ++id)
        {
            const struct Sprite *sprite = &gSprites[id];
            if (!sprite->inUse)
                continue;

            if (CtrSprite_IsPokeballGlow(sprite))
            {
                if (!hasNurse)
                    hasNurse = GetPokecenterPositions(&nurseX, &nurseZ, &machineX, &machineZ);
                if (hasNurse)
                {
                    if (!pokeballRefreshed)
                    {
                        if (RefreshSlot(VOXEL_HEAL_BALL_SLOT, sprite, atlas))
                            ++updates;
                        pokeballRefreshed = true;
                    }

                    int dx = (sprite->x + sprite->x2) - 93;
                    int dy = (sprite->y + sprite->y2) - 36;
                    int col = (dx >= 3) ? 1 : 0;
                    int row = dy / 4;
                    if (row < 0) row = 0;
                    if (row > 2) row = 2;

                    float bx = machineX + (col == 0 ? 0.36f : 0.64f);
                    float bz = machineZ + (row == 0 ? 0.32f : (row == 1 ? 0.52f : 0.72f));
                    float by = 0.71f;

                    unsigned baseX = (VOXEL_HEAL_BALL_SLOT % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
                    unsigned baseY = (VOXEL_HEAL_BALL_SLOT / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
                    float u0 = (float)baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
                    float u1 = (float)(baseX + 8) / (float)VOXEL_SPRITE_ATLAS_DIM;
                    float v0 = 1.0f - (float)baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
                    float v1 = 1.0f - (float)(baseY + 8) / (float)VOXEL_SPRITE_ATLAS_DIM;
                    float halfW = 0.14f;

                    VoxelBuilder_Quad(builder,
                        &(VoxelVertex){bx - halfW, by, bz + halfW, u0, v1, 1.0f},
                        &(VoxelVertex){bx + halfW, by, bz + halfW, u1, v1, 1.0f},
                        &(VoxelVertex){bx + halfW, by, bz - halfW, u1, v0, 1.0f},
                        &(VoxelVertex){bx - halfW, by, bz - halfW, u0, v0, 1.0f});
                }
            }
            else if (CtrSprite_IsPokecenterMonitor(sprite))
            {
                if (!sprite->invisible)
                {
                    if (!hasNurse)
                        hasNurse = GetPokecenterPositions(&nurseX, &nurseZ, &machineX, &machineZ);
                    if (hasNurse)
                    {
                        if (!monitorRefreshed)
                        {
                            if (RefreshSlot(VOXEL_HEAL_MONITOR_SLOT, sprite, atlas))
                                ++updates;
                            monitorRefreshed = true;
                        }

                        float mx = nurseX + 0.5f;
                        float mz = nurseZ - 0.5f;
                        float my = 0.02f;

                        unsigned baseX = (VOXEL_HEAL_MONITOR_SLOT % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
                        unsigned baseY = (VOXEL_HEAL_MONITOR_SLOT / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
                        float u0 = (float)baseX / (float)VOXEL_SPRITE_ATLAS_DIM;
                        float u1 = (float)(baseX + 16) / (float)VOXEL_SPRITE_ATLAS_DIM;
                        float v0 = 1.0f - (float)baseY / (float)VOXEL_SPRITE_ATLAS_DIM;
                        float v1 = 1.0f - (float)(baseY + 16) / (float)VOXEL_SPRITE_ATLAS_DIM;
                        float halfW = 0.5f;
                        float halfH = 0.5f;

                        VoxelBuilder_Quad(builder,
                            &(VoxelVertex){mx - halfW, my, mz + halfH, u0, v1, 1.0f},
                            &(VoxelVertex){mx + halfW, my, mz + halfH, u1, v1, 1.0f},
                            &(VoxelVertex){mx + halfW, my, mz - halfH, u1, v0, 1.0f},
                            &(VoxelVertex){mx - halfW, my, mz - halfH, u0, v0, 1.0f});
                    }
                }
            }
        }
    }

    return updates;
}
