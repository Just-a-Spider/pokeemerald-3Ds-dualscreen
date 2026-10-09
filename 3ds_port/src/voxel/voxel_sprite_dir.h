#ifndef VOXEL_SPRITE_DIR_H
#define VOXEL_SPRITE_DIR_H

#include <stdint.h>
#include <stdbool.h>

/* Rotates directional animation IDs (0..39) according to camera quadrant */
uint8_t VoxelSprite_RotateAnimNum(uint8_t animNum);

/* Refreshes standing facing direction for all active object events when quadrant changes */
void VoxelSprite_OnQuadrantChanged(int newQ);

#endif
