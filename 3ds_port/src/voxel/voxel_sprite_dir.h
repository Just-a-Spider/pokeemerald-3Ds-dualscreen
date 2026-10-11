#ifndef VOXEL_SPRITE_DIR_H
#define VOXEL_SPRITE_DIR_H

#include <stdint.h>
#include <stdbool.h>

/* Refreshes standing facing direction for all active object events when quadrant changes */
void VoxelSprite_OnQuadrantChanged(int newQ);

/* Adjusts character sprite OAM flip bits and VRAM tiles for camera perspective after animation tick */
void VoxelSprite_AfterAnimateSprites(void);

#endif
