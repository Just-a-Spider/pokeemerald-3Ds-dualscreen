/*
 * Follow camera for the voxel overworld. See voxel_camera.h and NOTICE.md.
 */

#include <stdlib.h>
#include <math.h>

#include "3ds_platform.h"
#include "voxel_camera.h"
#include "voxel_world.h"
#include "voxel_sprite_dir.h"

void CtrVoxel_InvalidateTreeQuadrant(void);
void CtrChords_NotifyYUsed(void);

#define VOXEL_DEG_TO_RAD (3.14159265358979323846f / 180.0f)
#define VOXEL_FOLLOW 0.15f

void VoxelCamera_Init(VoxelCamera *cam)
{
    cam->x = 0.0f;
    cam->y = 8.0f;
    cam->z = 0.0f;
    cam->targetX = 0.0f;
    cam->targetY = 0.0f;
    cam->targetZ = 0.0f;
    cam->pitch = 40.0f;
    cam->yaw = 0.0f;
    cam->distance = 9.0f;
    cam->fov = 35.0f;
    cam->ground = 0.0f;
}

void VoxelCamera_SetGround(VoxelCamera *cam, float ground, int snap)
{
    cam->ground = snap ? ground : cam->ground + (ground - cam->ground) * VOXEL_FOLLOW;
}

/* Places the eye for the current target, pitch, yaw and distance. */
static void Place(VoxelCamera *cam)
{
    float pitchRad = cam->pitch * VOXEL_DEG_TO_RAD;
    float yawRad = cam->yaw * VOXEL_DEG_TO_RAD;

    /* The eye sits south of the player (+Z) and above it, looking north. */
    cam->x = cam->targetX + sinf(yawRad) * cam->distance;
    cam->y = cam->ground + tanf(pitchRad) * cam->distance;
    cam->z = cam->targetZ + cosf(yawRad) * cam->distance;
    cam->targetY = cam->ground;
}

/* Invariant sprite pixel scale across all pitches: calibrated against pitch 40 deg */
static void AdaptDistance(VoxelCamera *cam)
{
    float pitch = cam->pitch;
    float pitchRad = pitch * VOXEL_DEG_TO_RAD;
    float cosPitch = cosf(pitchRad);
    /* Projected sprite size on screen is proportional to cos^1.5(pitch) / distance.
     * cos^1.5(40 deg) = 0.67005f. Keeping ratio constant preserves 1:1 pixel grid at any pitch. */
    float pitchScale = powf(cosPitch, 1.5f) / 0.67005f;

    cam->distance = 9.0f * pitchScale * 100.0f / (float)CtrSettings_VoxelZoom();
}

void VoxelCamera_Snap(VoxelCamera *cam, float playerWorldX, float playerWorldZ)
{
    cam->targetX = playerWorldX;
    cam->targetZ = playerWorldZ;
    cam->pitch = (float)CtrSettings_VoxelPitch();
    AdaptDistance(cam);
    Place(cam);
}

static bool sRotatedWithStick = false;
static int sLastPitchSetting = -1;
static float sTargetYaw = 0.0f;
static bool sSnapping = false;

void VoxelCamera_ResetNorth(void)
{
    sTargetYaw = 0.0f;
    sSnapping = true;
    sRotatedWithStick = false;
    CtrPlatform_ShowToast("CAMERA: NORTH (0 deg)");
}

void VoxelCamera_Update(VoxelCamera *cam, float playerWorldX, float playerWorldZ)
{
    const CtrInput *in = CtrInput_Get();

    if (in != NULL)
    {
        if (in->physicalDown & CTR_KEY_Y)
        {
            sRotatedWithStick = false;
        }

        /* Hold Y: Free 360 camera rotation with Circle Pad, or cardinal snaps with D-Pad */
        if (in->physicalHeld & CTR_KEY_Y)
        {
            /* Direct cardinal snaps via D-Pad */
            if (in->physicalDown & CTR_KEY_UP)
            {
                sTargetYaw = 0.0f; /* North */
                sSnapping = true;
                sRotatedWithStick = true;
                CtrInput_Mask(CTR_KEY_DPAD | CTR_KEY_Y);
            }
            else if (in->physicalDown & CTR_KEY_RIGHT)
            {
                sTargetYaw = 90.0f; /* East */
                sSnapping = true;
                sRotatedWithStick = true;
                CtrInput_Mask(CTR_KEY_DPAD | CTR_KEY_Y);
            }
            else if (in->physicalDown & CTR_KEY_DOWN)
            {
                sTargetYaw = 180.0f; /* South */
                sSnapping = true;
                sRotatedWithStick = true;
                CtrInput_Mask(CTR_KEY_DPAD | CTR_KEY_Y);
            }
            else if (in->physicalDown & CTR_KEY_LEFT)
            {
                sTargetYaw = -90.0f; /* West */
                sSnapping = true;
                sRotatedWithStick = true;
                CtrInput_Mask(CTR_KEY_DPAD | CTR_KEY_Y);
            }

            /* Free camera rotation via Circle Pad */
            if (abs(in->circleX) > 20)
            {
                sSnapping = false;
                cam->yaw += (float)in->circleX * 0.015f;
                sRotatedWithStick = true;
                CtrChords_NotifyYUsed();
                while (cam->yaw > 180.0f) cam->yaw -= 360.0f;
                while (cam->yaw < -180.0f) cam->yaw += 360.0f;
                sTargetYaw = cam->yaw;
            }
            if (abs(in->circleY) > 25)
            {
                cam->pitch += (float)in->circleY * 0.008f;
                if (cam->pitch < 20.0f) cam->pitch = 20.0f;
                if (cam->pitch > 60.0f) cam->pitch = 60.0f;
                sRotatedWithStick = true;
                CtrChords_NotifyYUsed();
            }
        }
        else
        {
            /* Sync pitch with bottom screen settings when not actively rotating */
            int curPitchSetting = CtrSettings_VoxelPitch();
            if (curPitchSetting != sLastPitchSetting)
            {
                sLastPitchSetting = curPitchSetting;
                cam->pitch = (float)curPitchSetting;
            }
        }

        /* SELECT single tap (when not holding START/A/B): Instant camera reset to North */
        if ((in->physicalDown & CTR_KEY_SELECT) && !(in->physicalHeld & (CTR_KEY_START | CTR_KEY_A | CTR_KEY_B | CTR_KEY_Y | CTR_KEY_X)))
        {
            sTargetYaw = 0.0f;
            cam->pitch = (float)CtrSettings_VoxelPitch();
            sSnapping = true;
            sRotatedWithStick = false;
            CtrPlatform_ShowToast("CAMERA: NORTH (0 deg)");
            CtrInput_Mask(CTR_KEY_SELECT);
        }

        /* Smooth camera snap interpolation */
        if (sSnapping)
        {
            while (sTargetYaw > 180.0f) sTargetYaw -= 360.0f;
            while (sTargetYaw <= -180.0f) sTargetYaw -= 360.0f;
            float diff = sTargetYaw - cam->yaw;
            while (diff > 180.0f) diff -= 360.0f;
            while (diff < -180.0f) diff += 360.0f;
            if (fabsf(diff) > 0.5f)
                cam->yaw += diff * 0.30f;
            else
            {
                cam->yaw = sTargetYaw;
                sSnapping = false;
            }
        }
    }

    {
        static int sLastQuadrant = 0;
        int curQ = (int)floorf((cam->yaw + 45.0f) / 90.0f);
        curQ = ((curQ % 4) + 4) % 4;
        if (curQ != sLastQuadrant)
        {
            sLastQuadrant = curQ;
            CtrVoxel_InvalidateTreeQuadrant();
            VoxelSprite_OnQuadrantChanged(curQ);
        }
    }

    cam->targetX += (playerWorldX - cam->targetX) * VOXEL_FOLLOW;
    cam->targetZ += (playerWorldZ - cam->targetZ) * VOXEL_FOLLOW;
    AdaptDistance(cam);
    Place(cam);
}

void VoxelCamera_Shift(VoxelCamera *cam, float dx, float dz)
{
    cam->targetX += dx;
    cam->targetZ += dz;
    Place(cam);
}

void VoxelCamera_Frame(VoxelCamera *cam, float targetX, float targetZ, float ground,
                       float pitch, float distance)
{
    cam->targetX = targetX;
    cam->targetZ = targetZ;
    cam->ground = ground;
    cam->pitch = pitch;
    cam->distance = distance;
    Place(cam);
}
