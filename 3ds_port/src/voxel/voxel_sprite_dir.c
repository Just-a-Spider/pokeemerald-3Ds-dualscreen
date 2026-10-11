/*
 * Segmented domain: Camera-relative sprite animation alignment for voxel mode.
 *
 * Aligns character and NPC sprite visuals (OAM flip bits and VRAM tile copies)
 * to the current camera quadrant without modifying the GBA engine's internal
 * animNum or step animation state machine.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "global.h"
#include "gba/defines.h"
#include "sprite.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "3ds_platform.h"
#include "port_platform.h"
#include "voxel_sprite_dir.h"

extern int CtrVoxel_GetCameraQuadrant(void);
extern uint8_t CtrVoxel_ToCameraRelativeDirection(uint8_t direction);

static void VoxelSprite_RequestFrameCopy(u16 index, u16 tileNum, const struct SpriteFrameImage *images)
{
    const void *src = images[index].data;
    u32 size = images[index].size;
    size = Port_GetSpriteFrameSize(src, size);
    src = Port_ResolveSpriteFramePointer(src, size, images[index].offset);
    if (src != NULL && size != 0)
    {
        RequestSpriteCopy((const u8 *)src, (u8 *)(OBJ_VRAM0 + tileNum * TILE_SIZE_4BPP), (u16)size);
    }
}

void VoxelSprite_AfterAnimateSprites(void)
{
    if (!CtrSettings_Voxel() || !CtrGame_IsOverworld())
        return;

    int q = CtrVoxel_GetCameraQuadrant();
    if (q == 0)
        return;

    for (unsigned i = 0; i < OBJECT_EVENTS_COUNT; ++i)
    {
        struct ObjectEvent *obj = &gObjectEvents[i];
        if (!obj->active || obj->inanimate || obj->spriteId >= MAX_SPRITES)
            continue;

        struct Sprite *sprite = &gSprites[obj->spriteId];
        if (!sprite->inUse || sprite->anims == NULL)
            continue;

        uint8_t dir = obj->facingDirection;
        if (dir < 1 || dir > 4)
            continue;

        uint8_t relDir = CtrVoxel_ToCameraRelativeDirection(dir);
        if (relDir < 1 || relDir > 4)
            continue;

        if (sprite->animNum >= 40)
            continue;

        uint8_t base = sprite->animNum & ~3;
        uint8_t targetAnim = (uint8_t)(base | (relDir - 1));
        if (targetAnim == sprite->animNum || sprite->anims[targetAnim] == NULL)
            continue;

        u8 cmdIdx = sprite->animCmdIndex;
        const union AnimCmd *cmd = &sprite->anims[targetAnim][cmdIdx];
        if (cmd->type < 0)
            cmd = &sprite->anims[targetAnim][0];

        s16 imageValue = cmd->frame.imageValue;
        if (imageValue != -1)
        {
            u8 hFlip = cmd->frame.hFlip;
            u8 vFlip = cmd->frame.vFlip;

            if (!(sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK))
            {
                sprite->oam.matrixNum &= 0x7;
                sprite->oam.matrixNum |= (((hFlip ^ sprite->hFlip) & 1) << 3);
                sprite->oam.matrixNum |= (((vFlip ^ sprite->vFlip) & 1) << 4);
            }

            if (sprite->usingSheet)
                sprite->oam.tileNum = sprite->sheetTileStart + imageValue;
            else
                VoxelSprite_RequestFrameCopy(imageValue, sprite->oam.tileNum, sprite->images);
        }
    }
}

void VoxelSprite_OnQuadrantChanged(int newQ)
{
    (void)newQ;
    VoxelSprite_AfterAnimateSprites();
}
