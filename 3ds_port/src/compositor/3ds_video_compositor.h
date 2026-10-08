#ifndef CTR_VIDEO_COMPOSITOR_H
#define CTR_VIDEO_COMPOSITOR_H

#include <citro2d.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>
#include "3ds_video.h"

/* Forward declarations for functions shared across compositor subfiles */
static void BattleWorldCurtain(uint32_t backdrop);
static bool BattleShadowTexture(void);
static void BattleWorldShadows(void);
static float BattleScenerySway(unsigned reg);
static void RenderBattleWorld(uint32_t clear);
static void RenderTransition(bool voxel, uint32_t clear);
static void TransitionField(void);
static void TransitionPicture(void);

static unsigned UsedSlots(void);
static unsigned BandsFromSlots(unsigned used, unsigned merge);
static void RenderBands(unsigned count, uint32_t backdrop);
static void RenderEye(C3D_RenderTarget *target, uint32_t clear, float parallax);

#endif /* CTR_VIDEO_COMPOSITOR_H */
