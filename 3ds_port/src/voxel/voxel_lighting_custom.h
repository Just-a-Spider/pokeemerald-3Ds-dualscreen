#ifndef VOXEL_LIGHTING_CUSTOM_H
#define VOXEL_LIGHTING_CUSTOM_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#define VOXEL_HAZE_START 1.05f
#define VOXEL_HAZE_RAMP 0.60f
#define VOXEL_HAZE_MAX 0.14f
#define VOXEL_HAZE_COLOUR 0xFFFAE6D2u

typedef struct
{
    float sun[3], shade[3];      /* the grade at full sun and at full shadow */
    float haze;                  /* the most the distance haze takes */
    float dappleLow, dappleHigh; /* brightness under a dapple's shade, in its light */
    float rays;                  /* the sun rays' strength at their brightest */
    float bloom;                 /* glow around the brightest parts (3ds_video.c) */
    float motes;                 /* the sunlit dust in the air, at its brightest */
    float hazeRgb[3];            /* what the distance fades to */
    float hazeStart, hazeRamp;   /* x the eye-to-player distance, see SetGrade */
} VoxelLight;

/* Computes outdoor/indoor daylight, dawn, dusk, night grading with smooth interpolation */
VoxelLight VoxelCustom_LightFor(bool indoor, float yaw, float pitch);

/* Computes quadrant- and tilt-optimized distance fog parameters */
void VoxelCustom_CalculateQuadrantFog(float distance, float pitch, float yaw, float haze,
                                      float *outFogStart, float *outFogScale);

/* Returns true if chunk trees should be culled beyond distance fog perimeter */
bool VoxelCustom_ShouldCullTrees(float chunkWorldX, float chunkWorldZ,
                                 float camX, float camZ, float pitch, float yaw);

/* Computes sun/moon cast shadow vector, stretch, and alpha with weather attenuation */
void VoxelCustom_GetDynamicShadowVector(float *outSx, float *outSz, float *outAlpha, float height);

/* Returns dynamic shadow color tint based on time of day and indoor state */
uint32_t VoxelCustom_GetShadowColor(bool indoor);

#endif
