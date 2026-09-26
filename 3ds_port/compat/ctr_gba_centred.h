/*
 * Force-included into the translation units whose screens are shown centred
 * as a GBA screen (full.mk, CTR_GBA_CENTRED_SRCS): the fly map and the Town Map.
 *
 * Unlike a stage (ctr_gba_stage.h) nothing is invented around the picture:
 * it sits 1:1 in the middle of the top screen, the layers that wrap on the
 * GBA (the region map's affine sea) carry on into the margins as they would,
 * and the rest stops at the edge of the 240x160 screen. The units build with
 * the GBA geometry (CTR_GBA_STAGE) and their VBlank callbacks go through the
 * bridge so the compositor knows when one of them is up. main.h declares the
 * renamed function, so the prototype still matches.
 */
#ifndef CTR_GBA_CENTRED_H
#define CTR_GBA_CENTRED_H

#define SetVBlankCallback CtrCentred_SetVBlankCallback

#endif
