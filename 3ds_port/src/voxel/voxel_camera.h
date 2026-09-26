/*
 * Follow camera for the voxel overworld: pure state and pure math.
 *
 * Ported from the MIT-licensed src/platform/voxel/voxel_camera.c of
 * pokeemerald-multiplatform (see NOTICE.md), minus gluPerspective/gluLookAt
 * and the SDL free-fly mode. Building the projection and view matrices is the
 * caller's job, so nothing in this module touches the GPU.
 */
#ifndef CTR_VOXEL_CAMERA_H
#define CTR_VOXEL_CAMERA_H

typedef struct
{
    float x, y, z;
    float targetX, targetY, targetZ;
    float pitch;    /* degrees below the horizon */
    float yaw;      /* degrees around the vertical axis */
    float distance; /* distance from the target, adapted to the map size */
    float fov;      /* vertical field of view, degrees */
} VoxelCamera;

void VoxelCamera_Init(VoxelCamera *cam);
/* Places the camera on the player with no interpolation (map change, re-entry). */
void VoxelCamera_Snap(VoxelCamera *cam, float playerWorldX, float playerWorldZ);
void VoxelCamera_Update(VoxelCamera *cam, float playerWorldX, float playerWorldZ);
/* Shifts the whole camera by a map-instance offset, keeping the framing. */
void VoxelCamera_Shift(VoxelCamera *cam, float dx, float dz);

#endif
