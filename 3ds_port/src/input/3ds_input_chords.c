/*
 * Segmented chord arbitration and hotkey dispatching for Old 3DS / New 3DS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "3ds_platform.h"
#include "3ds_input.h"
#include "3ds_input_chords.h"

void VoxelCamera_ResetNorth(void);

static bool sYUsed = false;
static int sInjectSelectFrames = 0;
static bool sStartToggle = false;

void CtrChords_NotifyYUsed(void)
{
    sYUsed = true;
}

void CtrChords_Update(void)
{
    const CtrInput *input = CtrInput_Get();
    if (input == NULL)
        return;

    /* 1. Y Hold: Camera rotation modifier */
    if (input->physicalHeld & CTR_KEY_Y)
    {
        /* L + Y Chord: Toggle Top Screen HUD (FPS + Cheatsheet) */
        if (input->physicalHeld & CTR_KEY_L)
        {
            if ((input->physicalDown & CTR_KEY_Y) || (input->physicalDown & CTR_KEY_L))
            {
                CtrSettings_SetShowFps(!CtrSettings_ShowFps());
                CtrPlatform_ShowToast(CtrSettings_ShowFps() ? "HUD: ON" : "HUD: OFF");
                sYUsed = true;
            }
        }
#if CTR_VOXEL_ENABLED
        /* R + Y Chord: Toggle Voxel Stereoscopic 3D */
        else if (input->physicalHeld & CTR_KEY_R)
        {
            if ((input->physicalDown & CTR_KEY_Y) || (input->physicalDown & CTR_KEY_R))
            {
                CtrSettings_SetVoxelStereo(!CtrSettings_VoxelStereo());
                CtrPlatform_ShowToast(CtrSettings_VoxelStereo() ? "3D STEREO: ON" : "3D STEREO: OFF");
                sYUsed = true;
            }
        }
#endif

        /* Check for stick deflection or D-Pad snap while Y is held */
        if (abs(input->circleX) > 18 || abs(input->circleY) > 18 || (input->physicalHeld & CTR_KEY_DPAD))
        {
            sYUsed = true;
        }
    }

    /* Y Release: If stick/chords never moved while Y was held, fire registered item (GBA Select) */
    if (input->physicalUp & CTR_KEY_Y)
    {
        if (!sYUsed)
        {
            sInjectSelectFrames = 2;
        }
        sYUsed = false;
    }

    /* 2. X Hold: RTC Time Offset Scrubbing */
    if (input->physicalHeld & CTR_KEY_X)
    {
        if (input->physicalDown & CTR_KEY_LEFT)
        {
            CtrPlatform_AddTimeOffset(-3600);
            char toast[32];
            snprintf(toast, sizeof(toast), "TIME: %.1fh (-1h)", CtrPlatform_GetDayTime());
            CtrPlatform_ShowToast(toast);
        }
        else if (input->physicalDown & CTR_KEY_RIGHT)
        {
            CtrPlatform_AddTimeOffset(3600);
            char toast[32];
            snprintf(toast, sizeof(toast), "TIME: %.1fh (+1h)", CtrPlatform_GetDayTime());
            CtrPlatform_ShowToast(toast);
        }
        else if (input->physicalDown & CTR_KEY_SELECT)
        {
            CtrPlatform_SetTimeOffset(0);
            CtrPlatform_ShowToast("RTC: REAL TIME");
        }
    }

    /* 3. START Tap: Bottom Screen Menu Focus Toggle (protect soft-reset chord) */
    if ((input->physicalDown & CTR_KEY_START) && !(input->physicalHeld & CTR_KEY_SELECT))
    {
        sStartToggle = true;
    }

    /* 4. SELECT Tap: Snap Camera to North (yaw 0°, pitch 40°) */
    if ((input->physicalDown & CTR_KEY_SELECT) && !(input->physicalHeld & (CTR_KEY_START | CTR_KEY_X | CTR_KEY_Y | CTR_KEY_A | CTR_KEY_B)))
    {
        VoxelCamera_ResetNorth();
    }

    /* 5. L + R Chord: Fast-Forward Speed Cycle (1x -> 2x -> 3x -> 4x -> 1x) */
    if (((input->physicalDown & CTR_KEY_R) && (input->physicalHeld & CTR_KEY_L)) ||
        ((input->physicalDown & CTR_KEY_L) && (input->physicalHeld & CTR_KEY_R)))
    {
        int speed = CtrSettings_Speed();
        int nextSpeed = (speed >= 4) ? 1 : speed + 1;
        CtrSettings_SetInt("speed", nextSpeed);
        char toast[16];
        snprintf(toast, sizeof(toast), "SPEED: %dx", nextSpeed);
        CtrPlatform_ShowToast(toast);
    }
}

uint16_t CtrChords_FilterGbaKeys(uint16_t held)
{
    const CtrInput *input = CtrInput_Get();

    /* Inject GBA Select on clean Y release */
    if (sInjectSelectFrames > 0)
    {
        held |= (1 << 2); /* SELECT_BUTTON */
        sInjectSelectFrames--;
    }

    /* Suppress D-pad movement to GBA while rotating camera (Y) or scrubbing RTC (X) */
    if (input != NULL && (input->physicalHeld & (CTR_KEY_Y | CTR_KEY_X)))
    {
        held &= ~0x00F0;
    }

    /* Remap cardinal direction to camera quadrant for natural navigation */
    if (CtrSettings_Voxel())
    {
        held = CtrVoxel_RotateDpadKeys(held, CtrVoxel_GetCameraQuadrant());
    }

    return held;
}

bool CtrChords_ConsumeStartToggle(void)
{
    if (sStartToggle)
    {
        sStartToggle = false;
        return true;
    }
    return false;
}
