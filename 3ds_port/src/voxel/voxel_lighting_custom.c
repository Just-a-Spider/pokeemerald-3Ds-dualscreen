/*
 * Segmented lighting, day/night cycles, quadrant distance fog, and dynamic shadows.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "global.h"
#include "field_weather.h"
#include "constants/weather.h"
#include "3ds_platform.h"
#include "voxel_world.h"
#include "voxel_lighting.h"
#include "voxel_lighting_custom.h"

#define DEG_TO_RAD (3.14159265358979323846f / 180.0f)
#define VOXEL_CAST_SHADOW_ALPHA 0.36f

void VoxelCustom_CalculateQuadrantFog(float distance, float pitch, float yaw, float haze,
                                      float *outFogStart, float *outFogScale)
{
    float pitchRad = pitch * DEG_TO_RAD;
    float yawRad = yaw * DEG_TO_RAD;
    float eye = distance / cosf(pitchRad);
    float lateral = fabsf(sinf(yawRad));
    float latSq = lateral * lateral;

    float pitchT = (pitch - 20.0f) / 20.0f;
    if (pitchT < 0.0f) pitchT = 0.0f;
    if (pitchT > 1.0f) pitchT = 1.0f;

    /* Flatter pitch tilts view into horizon: pull fog closer and steepen ramp */
    float pitchFogMod = 0.80f + 0.20f * pitchT;

    *outFogStart = eye * (VOXEL_HAZE_START - 0.20f * latSq) * pitchFogMod;
    *outFogScale = (haze / (eye * (VOXEL_HAZE_RAMP - 0.15f * latSq))) * (1.0f + 0.40f * (1.0f - pitchT) + 0.25f * latSq);
}

bool VoxelCustom_ShouldCullTrees(float chunkWorldX, float chunkWorldZ,
                                 float camX, float camZ, float pitch, float yaw)
{
    float dx = (chunkWorldX + 4.0f) - camX;
    float dz = (chunkWorldZ + 4.0f) - camZ;
    float distSq = dx * dx + dz * dz;

    float pitchT = (pitch - 20.0f) / 20.0f;
    if (pitchT < 0.0f) pitchT = 0.0f;
    if (pitchT > 1.0f) pitchT = 1.0f;

    float lateral = fabsf(sinf(yaw * DEG_TO_RAD));
    float latMod = 1.0f - 0.15f * (lateral * lateral);

    /* Max tree distance: 20 tiles at 20 deg tilt, 26 tiles at 40+ deg */
    float maxDist = (20.0f + 6.0f * pitchT) * latMod;
    return distSq > (maxDist * maxDist);
}

VoxelLight VoxelCustom_LightFor(bool indoor, float yaw, float pitch)
{
    VoxelLight light = {
        {1.00f, 0.99f, 0.95f}, {0.93f, 0.97f, 1.05f}, VOXEL_HAZE_MAX,
        0.96f, 1.04f, 0.11f, 0.07f, 0.85f,
        {(VOXEL_HAZE_COLOUR & 255) / 255.0f,
         ((VOXEL_HAZE_COLOUR >> 8) & 255) / 255.0f,
         ((VOXEL_HAZE_COLOUR >> 16) & 255) / 255.0f},
        VOXEL_HAZE_START, VOXEL_HAZE_RAMP
    };

    if (indoor)
        return light;

    if (CtrSettings_DayNight())
    {
        float t = CtrPlatform_GetDayTime();
        VoxelLight day = light;
        VoxelLight dawn = {
            {1.12f, 0.92f, 0.78f}, {0.84f, 0.86f, 1.02f}, 0.20f,
            0.94f, 1.05f, 0.15f, 0.10f, 0.80f,
            {0.92f, 0.80f, 0.70f},
            VOXEL_HAZE_START, VOXEL_HAZE_RAMP
        };
        VoxelLight dusk = {
            {1.18f, 0.76f, 0.52f}, {0.70f, 0.65f, 0.88f}, 0.24f,
            0.92f, 1.06f, 0.18f, 0.14f, 0.95f,
            {0.88f, 0.68f, 0.60f},
            VOXEL_HAZE_START, VOXEL_HAZE_RAMP
        };
        VoxelLight night = {
            {0.50f, 0.55f, 0.75f}, {0.38f, 0.40f, 0.55f}, 0.24f,
            0.98f, 1.02f, 0.02f, 0.08f, 0.20f,
            {0.18f, 0.22f, 0.38f},
            VOXEL_HAZE_START, VOXEL_HAZE_RAMP
        };

        float duskStart = CtrSettings_DuskStart();
        float nightStart = CtrSettings_NightStart();
        float duskPeak = (duskStart + nightStart) * 0.5f;

        VoxelLight from, to;
        float factor = 0.0f;

        if (t < 4.5f || t >= nightStart + 1.0f)
        {
            light = night;
        }
        else if (t < 6.5f)
        {
            from = night; to = dawn;
            factor = (t - 4.5f) / 2.0f;
            goto interpolate;
        }
        else if (t < 8.5f)
        {
            from = dawn; to = day;
            factor = (t - 6.5f) / 2.0f;
            goto interpolate;
        }
        else if (t < duskStart)
        {
            light = day;
        }
        else if (t < duskPeak)
        {
            from = day; to = dusk;
            factor = (t - duskStart) / (duskPeak - duskStart);
            goto interpolate;
        }
        else if (t < nightStart + 1.0f)
        {
            from = dusk; to = night;
            factor = (t - duskPeak) / (nightStart + 1.0f - duskPeak);
            goto interpolate;
        }
        else
        {
            light = night;
        }
        goto apply_weather;

    interpolate:
        factor = 0.5f - 0.5f * cosf(factor * 3.14159265f);
        for (int i = 0; i < 3; ++i)
        {
            light.sun[i] = from.sun[i] + (to.sun[i] - from.sun[i]) * factor;
            light.shade[i] = from.shade[i] + (to.shade[i] - from.shade[i]) * factor;
            light.hazeRgb[i] = from.hazeRgb[i] + (to.hazeRgb[i] - from.hazeRgb[i]) * factor;
        }
        light.haze = from.haze + (to.haze - from.haze) * factor;
        light.dappleLow = from.dappleLow + (to.dappleLow - from.dappleLow) * factor;
        light.dappleHigh = from.dappleHigh + (to.dappleHigh - from.dappleHigh) * factor;
        light.rays = from.rays + (to.rays - from.rays) * factor;
        light.bloom = from.bloom + (to.bloom - from.bloom) * factor;
        light.motes = from.motes + (to.motes - from.motes) * factor;
    }

apply_weather:
    {
        float lateral = fabsf(sinf(yaw * DEG_TO_RAD));
        float pitchT = (pitch - 20.0f) / 20.0f;
        if (pitchT < 0.0f) pitchT = 0.0f;
        if (pitchT > 1.0f) pitchT = 1.0f;
        float tiltHaze = 0.12f * (1.0f - pitchT);

        if (lateral > 0.05f || tiltHaze > 0.0f)
            light.haze = fminf(light.haze + 0.22f * (lateral * lateral) + tiltHaze, 0.45f);
    }
    switch (VoxelWorld_Weather())
    {
    case VOXEL_WEATHER_SUN:
        light.rays = fminf(light.rays + 0.05f, 0.20f);
        light.bloom = fminf(light.bloom + 0.03f, 0.18f);
        break;
    case VOXEL_WEATHER_RAIN:
        for (int i = 0; i < 3; ++i)
        {
            light.sun[i] *= 0.80f;
            light.shade[i] *= 0.85f;
        }
        light.haze = fmaxf(light.haze, 0.30f);
        light.rays = 0.0f;
        light.motes = 0.0f;
        break;
    case VOXEL_WEATHER_FOG:
        for (int i = 0; i < 3; ++i)
        {
            light.sun[i] *= 0.90f;
            light.shade[i] *= 0.94f;
        }
        light.haze = fmaxf(light.haze, 0.48f);
        light.bloom = fmaxf(light.bloom, 0.16f);
        light.rays = 0.0f;
        light.motes = 0.0f;
        break;
    case VOXEL_WEATHER_PARTICLES:
        for (int i = 0; i < 3; ++i)
        {
            light.sun[i] *= 0.88f;
            light.shade[i] *= 0.90f;
        }
        light.haze = fmaxf(light.haze, 0.36f);
        light.rays = 0.0f;
        light.motes = 0.0f;
        break;
    case VOXEL_WEATHER_SHADE:
        for (int i = 0; i < 3; ++i)
        {
            light.sun[i] *= 0.92f;
            light.shade[i] *= 0.95f;
        }
        light.haze = fmaxf(light.haze, 0.25f);
        light.rays = 0.0f;
        light.motes *= 0.5f;
        break;
    default:
        break;
    }
    return light;
}

void VoxelCustom_GetDynamicShadowVector(float *outSx, float *outSz, float *outAlpha, float height)
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
            /* Daytime: Sun progresses from East (+X) at dawn to West (-X) at dusk */
            float s = (t - 5.5f) / 14.0f;
            float angle = s * 3.1415926535f;
            float sinA = sinf(angle);
            float cosA = cosf(angle);

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

    /* Weather shadow diffusion */
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

uint32_t VoxelCustom_GetShadowColor(bool indoor)
{
    if (indoor)
        return 0xCC181414u; /* soft neutral indoor ambient contact shadow */

    if (CtrSettings_DayNight())
    {
        float t = CtrPlatform_GetDayTime();
        if (t < 5.0f || t >= 21.0f)
            return 0xCC301808u; /* midnight cool blue */
        else if (t < 7.0f || t >= 18.5f)
            return 0xFF181028u; /* dusk / twilight violet-indigo */
        else
            return 0xFF281408u; /* crisp day shadow */
    }
    return 0xFF281408u;
}
