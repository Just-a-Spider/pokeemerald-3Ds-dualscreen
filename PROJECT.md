# Project: pokeemerald-3Ds-dualscreen

## Architecture
- **Dual Screen System**: Nintendo 3DS port featuring 3D top screen rendering (GBA viewport + voxel world projection) and interactive bottom screen touch UI (menus, Pokénav, battle controller, enhancements, cheats).
- **Compositor**: Splits video pipeline (`3ds_video.c`) into modular battle scenery (`3ds_video_battle.c`), depth presentation and stereo compositing (`3ds_video_present.c`), coordinated by compositor header and driver (`3ds_video_compositor.c`/`.h`).
- **Bottom UI**: Modularized subsystem in `3ds_port/src/bottom_ui/`:
  - `bottom_ui_draw.c`: Raster primitives, fonts, BTA art loader, icon caching.
  - `bottom_ui_screens.c`: Hoenn map, trainer card, save screen, battle menus, dirty rect updates.
  - `bottom_ui_state.c`: Session states, snapshotting, key injection, subsystem lifecycle.
  - `bottom_ui_touch.c`: Hit testing, touch state machine, D-pad button focus.
- **Input Subsystem**:
  - `voxel_camera.c`: 3D voxel camera orientation. Cardinal yaw snaps (0°, 90°, 180°, -90°) mapped to `Y + D-Pad`.
  - `3ds_platform.c` & `3ds_input.c`: RTC clock offset scrubbing mapped to `X + D-Pad` (Left/Right hours scrub, SELECT reset). D-pad input masked from GBA player movement during chords.
- **Voxel Engine (`ctr_voxel.c`)**:
  - Omnidirectional distance haze and mist boost (+0.22, cap 0.45) across all 360° camera yaw orientations (including North 0° and South 180°).
  - Constrained reach bounds (reachMax = 28.0 tiles) across all yaw angles to protect Old 3DS (ARM11 @ 268 MHz) frame rate.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Bottom UI Modularization | Split `3ds_bottom_ui.c` into `bottom_ui_draw.c`, `bottom_ui_screens.c`, `bottom_ui_state.c`, `bottom_ui_touch.c` | M1 | R1 |
| 2 | Compositor Modularization | Split `3ds_video.c` into `3ds_video_battle.c`, `3ds_video_present.c`, `3ds_video_compositor.h`/`.c` | M1 | R1 |
| 3 | Upstream Feature Preservation | Retain Quick Ball, updated Battle UI, Fast-Forward, Visible Wild, Cheats/Enhancements, smooth port art | M1 | R1 |
| 4 | Build Configuration Update | Update `3ds_port/Makefile` and `3ds_port/full.mk` to build modular files cleanly | M1 | R1 |
| 5 | Camera Control Remapping | Remap cardinal camera angle snaps (0°, 90°, 180°, -90°) to `Y + D-Pad` | M2 | R2 |
| 6 | RTC Scrub Remapping | Map RTC time adjustments to `X + D-Pad` (Left/Right) without camera interference | M2 | R2 |
| 7 | Input Collision Suppression | Suppress voxel toggle and GBA SELECT injection on Y chord; mask D-pad during chords | M2 | R2 |
| 8 | Omnidirectional Mist Haze | Apply mist haze occlusion across all 360° yaw angles (including North 0° and South 180°) | M3 | R3 |
| 9 | Old 3DS Chunk Bounds | Constrain view reachMax to 28.0 tiles in North/South views to lock 60 fps | M3 | R3 |
| 10 | Clean Build & Verification | Clean build with `make -C 3ds_port` and passing test suites | M4 | Acceptance Criteria |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | M1: Modularize Bottom UI & Compositor | Decompose `3ds_bottom_ui.c` and `3ds_video.c` preserving all upstream features and updating makefiles | none | IN_PROGRESS (69aacf4b-82e1-4b51-98be-cf89c72e0658) |
| 2 | M2: Input Disambiguation | Disambiguate camera controls (Y + D-Pad) from RTC scrubbing (X + D-Pad) | none | PLANNED |
| 3 | M3: Omnidirectional Voxel Mist & Bounds | Uniform 360° mist occlusion (including 0°/180°) and chunk reachMax bound in `ctr_voxel.c` | none | PLANNED |
| 4 | M4: E2E Integration & Verification | Clean compilation of `3ds_port`, E2E test suite execution, coverage verification | M1, M2, M3 | PLANNED |

## Interface Contracts
### `bottom_ui` Subsystem ↔ `3ds_port` Core
- Headers: `3ds_port/src/bottom_ui/bottom_ui_internal.h`, `3ds_port/src/3ds_bottom_ui.h`
- Public Entry Points:
  - `void CtrBottom_Init(void)`
  - `void CtrBottom_Frame(u32 keysHeld, u32 keysDown, touchPosition touch)`
  - `u16 CtrBottom_InjectedKeys(void)`
  - `u16 CtrBottom_FilterKeys(u16 keys)`
  - `void CtrBattleMenu_SelectAction(u8 action)`
  - `void CtrExtras_ShowScreen(u8 screen)`

### `compositor` Subsystem ↔ `3ds_video` Core
- Headers: `3ds_port/src/compositor/3ds_video_compositor.h`, `3ds_port/src/3ds_video.h`
- Internal Prototypes:
  - `void CtrVideo_RenderBattleScenery(...)`
  - `void CtrVideo_PresentFrame(...)`
- Exported Video Interface:
  - `void CtrVideo_GetStats(CtrVideoStats *stats)`
  - `void CtrVideo_SetStage(int stage)`
  - `void CtrVideo_SetBattle(bool battle)`
  - `void CtrVideo_Shutdown(void)`

### Input System ↔ Camera & RTC
- `voxel_camera.c`:
  - `in->physicalHeld & CTR_KEY_Y`: D-Pad Up (yaw 0°), Right (yaw 90°), Down (yaw 180°), Left (yaw -90°).
  - Mask D-Pad inputs via `CtrInput_Mask(CTR_KEY_CPAD | CTR_KEY_DPAD)`.
- `3ds_platform.c`:
  - `input->physicalHeld & CTR_KEY_X`: D-Pad Right (+3600s), Left (-3600s), SELECT (0s reset).
  - Mask D-Pad inputs via `CtrInput_Mask()`.

### Voxel Engine Occlusion Contract
- `ctr_voxel.c`:
  - `UpdateFrustum`: `reachMax = (float)VOXEL_VIEW_REACH_MAX - 16.0f;` (28.0 tiles uniform).
  - `LightFor`: `light.haze = fminf(light.haze + 0.22f, 0.45f);` (omnidirectional boost).
  - `SetGrade`: `fogStart = eye * (VOXEL_HAZE_START - 0.20f);`, `fogScale = light->haze / (eye * (VOXEL_HAZE_RAMP - 0.15f));`.

## Code Layout
- `3ds_port/src/3ds_bottom_ui.c` (top-level coordinator or unity include)
- `3ds_port/src/bottom_ui/bottom_ui_internal.h`
- `3ds_port/src/bottom_ui/bottom_ui_draw.c`
- `3ds_port/src/bottom_ui/bottom_ui_screens.c`
- `3ds_port/src/bottom_ui/bottom_ui_state.c`
- `3ds_port/src/bottom_ui/bottom_ui_touch.c`
- `3ds_port/src/3ds_video.c` (top-level coordinator or unity include)
- `3ds_port/src/compositor/3ds_video_compositor.h`
- `3ds_port/src/compositor/3ds_video_compositor.c`
- `3ds_port/src/compositor/3ds_video_battle.c`
- `3ds_port/src/compositor/3ds_video_present.c`
- `3ds_port/src/voxel/voxel_camera.c`
- `3ds_port/src/voxel/ctr_voxel.c`
- `3ds_port/src/3ds_platform.c`
- `3ds_port/src/3ds_game_bridge.c`
- `3ds_port/Makefile`
- `3ds_port/full.mk`
