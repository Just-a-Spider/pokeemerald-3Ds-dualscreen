/*
 * Segmented domain: Camera-relative sprite animation alignment for voxel mode.
 *
 * Aligns character and NPC sprite animations (0..39) to the current camera quadrant
 * so that avatar steps and facing directions visually correspond to the 3D viewport.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "global.h"
#include "sprite.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "3ds_platform.h"
#include "voxel_sprite_dir.h"

extern int CtrVoxel_GetCameraQuadrant(void);

/*
 * Camera relative quadrant mapping for directional anims:
 * Direction index: 0 = SOUTH, 1 = NORTH, 2 = WEST, 3 = EAST
 * Quadrants:
 *   q = 0: North view (default, looking North)
 *   q = 1: East view (camera at East, looking West)
 *   q = 2: South view (camera at South, looking North-East/South)
 *   q = 3: West view (camera at West, looking East)
 */
static const uint8_t sAnimRel[4][4] = {
    [0] = {0, 1, 2, 3}, /* S, N, W, E */
    [1] = {2, 3, 1, 0}, /* W, E, N, S */
    [2] = {1, 0, 3, 2}, /* N, S, E, W */
    [3] = {3, 2, 0, 1}, /* E, W, S, N */
};

static inline bool VoxelSprite_IsObjectEventSprite(const struct Sprite *sprite)
{
    if (sprite == NULL)
        return false;
    ptrdiff_t diff = sprite - gSprites;
    if (diff < 0 || diff >= MAX_SPRITES)
        return false;
    unsigned objId = sprite->data[0];
    if (objId >= OBJECT_EVENTS_COUNT)
        return false;
    return (gObjectEvents[objId].active && gObjectEvents[objId].spriteId == (u32)diff);
}

uint8_t VoxelSprite_RotateAnimNum(uint8_t animNum)
{
    if (!CtrSettings_Voxel() || animNum >= 40)
        return animNum;

    int q = CtrVoxel_GetCameraQuadrant();
    if (q == 0)
        return animNum;

    uint8_t dir = animNum & 3;
    uint8_t base = animNum & ~3;
    return (uint8_t)(base | sAnimRel[q][dir]);
}

extern void __real_StartSpriteAnim(struct Sprite *sprite, u8 animNum);

void VoxelSprite_OnQuadrantChanged(int newQ)
{
    (void)newQ;
    if (!CtrSettings_Voxel())
        return;

    for (unsigned i = 0; i < OBJECT_EVENTS_COUNT; ++i)
    {
        struct ObjectEvent *obj = &gObjectEvents[i];
        if (!obj->active || obj->spriteId >= MAX_SPRITES)
            continue;
        if (obj->heldMovementActive)
            continue;

        struct Sprite *sprite = &gSprites[obj->spriteId];
        u8 dir = obj->facingDirection;
        if (dir >= 1 && dir <= 4)
        {
            u8 animNum = GetFaceDirectionAnimNum(dir);
            animNum = VoxelSprite_RotateAnimNum(animNum);
            __real_StartSpriteAnim(sprite, animNum);
            SeekSpriteAnim(sprite, 0);
        }
    }
}

extern void __real_SetStepAnim(struct ObjectEvent *objectEvent, struct Sprite *sprite, u8 animNum);
void __wrap_SetStepAnim(struct ObjectEvent *objectEvent, struct Sprite *sprite, u8 animNum)
{
    if (CtrSettings_Voxel())
        animNum = VoxelSprite_RotateAnimNum(animNum);
    __real_SetStepAnim(objectEvent, sprite, animNum);
}

extern void __real_SetStepAnimHandleAlternation(struct ObjectEvent *objectEvent, struct Sprite *sprite, u8 animNum);
void __wrap_SetStepAnimHandleAlternation(struct ObjectEvent *objectEvent, struct Sprite *sprite, u8 animNum)
{
    if (CtrSettings_Voxel())
        animNum = VoxelSprite_RotateAnimNum(animNum);
    __real_SetStepAnimHandleAlternation(objectEvent, sprite, animNum);
}

void __wrap_StartSpriteAnim(struct Sprite *sprite, u8 animNum)
{
    if (CtrSettings_Voxel() && VoxelSprite_IsObjectEventSprite(sprite))
        animNum = VoxelSprite_RotateAnimNum(animNum);
    __real_StartSpriteAnim(sprite, animNum);
}

extern void __real_StartSpriteAnimIfDifferent(struct Sprite *sprite, u8 animNum);
void __wrap_StartSpriteAnimIfDifferent(struct Sprite *sprite, u8 animNum)
{
    if (CtrSettings_Voxel() && VoxelSprite_IsObjectEventSprite(sprite))
        animNum = VoxelSprite_RotateAnimNum(animNum);
    __real_StartSpriteAnimIfDifferent(sprite, animNum);
}
