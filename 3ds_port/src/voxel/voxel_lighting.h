/* Fixed sun, baked into chunk colours; no extra terrain GPU pass. */
#ifndef CTR_VOXEL_LIGHTING_H
#define CTR_VOXEL_LIGHTING_H

#include "voxel_mesh_builder.h"

/* Standalone host consumers retain the original mesh unless explicitly enabled. */
#ifndef CTR_VOXEL_LIGHTING
#define CTR_VOXEL_LIGHTING 0
#endif

#if CTR_VOXEL_LIGHTING
/* Sun in the northwest; shadows travel southeast. Y is height. */
#define VOXEL_SUN_DX 0.85f
#define VOXEL_SUN_DZ 0.55f
#define VOXEL_LIGHT_REACH 8
/* Four clipped octagons, at most twelve vertices / ten triangles each. */
#define VOXEL_CONTACT_VERTICES 120u

/* Forgets every cached caster and sample. Required whenever the world they
 * were read from changes - a live tile, the set of maps or their origins -
 * and needed at no other time: the caches outlive builds and frames. */
void VoxelLighting_Reset(void);
uint32_t VoxelLighting_Hash(int x0, int z0, int x1, int z1);
float VoxelLighting_Sample(float x, float y, float z);
void VoxelLighting_Quad(VoxelBuilder *builder, const VoxelVertex *a,
                         const VoxelVertex *b, const VoxelVertex *c,
                         const VoxelVertex *d);
void VoxelLighting_Contact(VoxelBuilder *builder, float x, float z);
#endif

#endif
