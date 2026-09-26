/*
 * Reader and emitter for voxel/buildings.bin (game data). See voxel_building.h;
 * scripts/gen_voxel_buildings.py writes the file and documents the models.
 *
 * Layout (little endian):
 *   "VXB5", u16 pages, models, pageModels, placements, heightBytes, 0,
 *   u32 vertices
 *   pages       x 8:  u16 w, h; u32 file offset of its RGBA5551 texels
 *   models      x 16: u8 w, h; u16 ground; u32 firstVertex, vertexCount, heights
 *   pageModels  x 8:  u16 model, page; i16 ox, oy (pixels)
 *   placements  x 16: u16 layout, pageModel, x, y, ground, extraCount;
 *                     u32 extraFirst (sorted by layout)
 *   heightBytes       one byte per model cell, pixels; 255 = not the model's
 *   padding to 4, vertices x 24: float x, y, z, u, v, shade (tiles, relative
 *                     to the top-left cell; u, v in pixels of the model's own
 *                     drawing, or of its page for a placement's ground patches)
 *   the pages' texels, read only when a map on screen needs them
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Host tests define PORT_LOG away rather than link the console's logger. */
#ifndef PORT_LOG
#include "port_log.h"
#endif

#include "voxel_building.h"
#include "voxel_file.h"
#include "voxel_relief.h"

#ifndef VOXEL_BUILDINGS_PATH
#define VOXEL_BUILDINGS_PATH "voxel/buildings.bin"
#endif

typedef struct
{
    uint16_t w, h;
    uint32_t offset;
} BuildingPage;

typedef struct
{
    uint8_t w, h;
    uint16_t ground;
    uint32_t firstVertex, vertexCount, heights;
} BuildingModel;

typedef struct
{
    uint16_t model, page;
    int16_t ox, oy;
} BuildingPageModel;

typedef struct
{
    uint16_t layout, pageModel, x, y, ground, extraCount;
    uint32_t extraFirst;
} BuildingPlacement;

static BuildingPage *sPages;
static unsigned sPageCount;
static BuildingModel *sModels;
static unsigned sModelCount;
static BuildingPageModel *sPageModels;
static unsigned sPageModelCount;
static BuildingPlacement *sPlacements;
static unsigned sPlacementCount;
static uint8_t *sHeights;
static VoxelVertex *sVertices;
static unsigned sVertexCount;
static float sMaxTop;
/* Kept open for the page reads, which come one slice at a time: opening the
 * file again for every slice cost a RomFS path lookup per slice. */
static FILE *sPageFile;
/* The placements last looked up: a build or a shadow ray asks about every
 * cell it touches, nearly always of the layout it asked about last. */
static int sLastLayout = -1;
static unsigned sLastFirst, sLastCount;

static unsigned U16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t U32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static bool ReadRows(FILE *file, unsigned count, unsigned size, uint8_t *row,
                     bool (*take)(unsigned, const uint8_t *))
{
    for (unsigned i = 0; i < count; ++i)
        if (fread(row, 1, size, file) != size || !take(i, row))
            return false;
    return true;
}

static unsigned sHeightBytes;

static bool TakePage(unsigned i, const uint8_t *r)
{
    sPages[i].w = (uint16_t)U16(r);
    sPages[i].h = (uint16_t)U16(r + 2);
    sPages[i].offset = U32(r + 4);
    return true;
}

static bool TakeModel(unsigned i, const uint8_t *r)
{
    BuildingModel *m = &sModels[i];
    m->w = r[0];
    m->h = r[1];
    m->ground = (uint16_t)U16(r + 2);
    m->firstVertex = U32(r + 4);
    m->vertexCount = U32(r + 8);
    m->heights = U32(r + 12);
    return m->firstVertex + m->vertexCount <= sVertexCount
        && m->heights + (unsigned)m->w * m->h <= sHeightBytes;
}

static bool TakePageModel(unsigned i, const uint8_t *r)
{
    BuildingPageModel *pm = &sPageModels[i];
    pm->model = (uint16_t)U16(r);
    pm->page = (uint16_t)U16(r + 2);
    pm->ox = (int16_t)U16(r + 4);
    pm->oy = (int16_t)U16(r + 6);
    return pm->model < sModelCount && pm->page < sPageCount;
}

static bool TakePlacement(unsigned i, const uint8_t *r)
{
    BuildingPlacement *p = &sPlacements[i];
    p->layout = (uint16_t)U16(r);
    p->pageModel = (uint16_t)U16(r + 2);
    p->x = (uint16_t)U16(r + 4);
    p->y = (uint16_t)U16(r + 6);
    p->ground = (uint16_t)U16(r + 8);
    p->extraCount = (uint16_t)U16(r + 10);
    p->extraFirst = U32(r + 12);
    return p->pageModel < sPageModelCount
        && p->extraFirst + p->extraCount <= sVertexCount;
}

bool VoxelBuildings_Init(void)
{
    uint8_t header[20], row[16];
    long offset;
    FILE *file;
    bool ok = false;

    VoxelBuildings_Shutdown();
    file = VoxelFile_Open(VOXEL_BUILDINGS_PATH);
    if (file == NULL)
    {
        PORT_LOG("[VIDEO] VOXEL buildings: %s absent, houses stay extruded\n",
                VOXEL_BUILDINGS_PATH);
        return false;
    }
    if (fread(header, 1, sizeof(header), file) != sizeof(header)
     || memcmp(header, "VXB5", 4) != 0)
        goto done;
    sPageCount = U16(header + 4);
    sModelCount = U16(header + 6);
    sPageModelCount = U16(header + 8);
    sPlacementCount = U16(header + 10);
    sHeightBytes = U16(header + 12);
    sVertexCount = U32(header + 16);

    sPages = malloc(sPageCount * sizeof(*sPages) + 1);
    sModels = malloc(sModelCount * sizeof(*sModels) + 1);
    sPageModels = malloc(sPageModelCount * sizeof(*sPageModels) + 1);
    sPlacements = malloc(sPlacementCount * sizeof(*sPlacements) + 1);
    sHeights = malloc(sHeightBytes + 1);
    sVertices = malloc(sVertexCount * sizeof(VoxelVertex) + 1);
    if (!sPages || !sModels || !sPageModels || !sPlacements || !sHeights || !sVertices)
        goto done;
    if (!ReadRows(file, sPageCount, 8, row, TakePage)
     || !ReadRows(file, sModelCount, 16, row, TakeModel)
     || !ReadRows(file, sPageModelCount, 8, row, TakePageModel)
     || !ReadRows(file, sPlacementCount, 16, row, TakePlacement))
        goto done;
    if (fread(sHeights, 1, sHeightBytes, file) != sHeightBytes)
        goto done;
    offset = ftell(file);
    if (offset < 0 || fseek(file, (4 - (offset & 3)) & 3, SEEK_CUR) != 0)
        goto done;
    /* The file's vertex record is VoxelVertex exactly: six little-endian
     * floats, which is also the console's layout. */
    if (fread(sVertices, sizeof(VoxelVertex), sVertexCount, file) != sVertexCount)
        goto done;
    sMaxTop = 0.0f;
    for (unsigned i = 0; i < sHeightBytes; ++i)
        if (sHeights[i] != 0xFF && sHeights[i] / 16.0f > sMaxTop)
            sMaxTop = sHeights[i] / 16.0f;
    ok = true;

done:
    fclose(file);
    if (!ok)
    {
        PORT_LOG("[ERROR] VOXEL buildings: %s is truncated or malformed\n",
                VOXEL_BUILDINGS_PATH);
        VoxelBuildings_Shutdown();
        return false;
    }
    PORT_LOG("[VIDEO] VOXEL buildings: %u models on %u pages, %u placements, %u vertices\n",
            sModelCount, sPageCount, sPlacementCount, sVertexCount);
    return true;
}

void VoxelBuildings_Shutdown(void)
{
    free(sPages);
    free(sModels);
    free(sPageModels);
    free(sPlacements);
    free(sHeights);
    free(sVertices);
    sPages = NULL;
    sModels = NULL;
    sPageModels = NULL;
    sPlacements = NULL;
    sHeights = NULL;
    sVertices = NULL;
    sPageCount = sModelCount = sPageModelCount = sPlacementCount = sVertexCount = 0;
    sLastLayout = -1;
    sMaxTop = 0.0f;
    if (sPageFile != NULL)
    {
        fclose(sPageFile);
        sPageFile = NULL;
    }
}

float VoxelBuildings_MaxTop(void)
{
    return sMaxTop;
}

bool VoxelBuildings_PageSize(unsigned page, unsigned *width, unsigned *height)
{
    if (page >= sPageCount)
        return false;
    *width = sPages[page].w;
    *height = sPages[page].h;
    return true;
}

bool VoxelBuildings_ReadPage(unsigned page, unsigned first, unsigned count, uint16_t *dest)
{
    if (page >= sPageCount || first + count > (unsigned)sPages[page].w * sPages[page].h)
        return false;
    if (sPageFile == NULL)
    {
        sPageFile = VoxelFile_Open(VOXEL_BUILDINGS_PATH);
        if (sPageFile == NULL)
            return false;
        /* Slices go straight into the caller's buffer, never through stdio's. */
        setvbuf(sPageFile, NULL, _IONBF, 0);
    }
    return fseek(sPageFile, (long)(sPages[page].offset + first * sizeof(uint16_t)), SEEK_SET) == 0
        && fread(dest, sizeof(uint16_t), count, sPageFile) == count;
}

/* The placements of one layout, which the generator sorted by layout. */
static const BuildingPlacement *LayoutPlacements(const VoxelMapInstance *inst, unsigned *count)
{
    unsigned lo = 0, hi = sPlacementCount, n = 0;

    *count = 0;
    if (inst == NULL || sPlacementCount == 0)
        return NULL;
    if (inst->layoutId != sLastLayout)
    {
        while (lo < hi)
        {
            unsigned mid = (lo + hi) / 2;

            if (sPlacements[mid].layout < (unsigned)inst->layoutId)
                lo = mid + 1;
            else
                hi = mid;
        }
        while (lo + n < sPlacementCount && sPlacements[lo + n].layout == (unsigned)inst->layoutId)
            ++n;
        sLastLayout = inst->layoutId;
        sLastFirst = lo;
        sLastCount = n;
    }
    *count = sLastCount;
    return sLastCount ? &sPlacements[sLastFirst] : NULL;
}

int VoxelBuildings_PageOf(const VoxelMapInstance *inst)
{
    unsigned count;
    const BuildingPlacement *p = LayoutPlacements(inst, &count);

    return p ? (int)sPageModels[p[0].pageModel].page : -1;
}

bool VoxelBuildings_CellAt(const VoxelMapInstance *inst, int x, int y,
                           int *groundMetatile, float *top)
{
    unsigned count;
    const BuildingPlacement *p = LayoutPlacements(inst, &count);
    int lx, ly;

    if (p == NULL)
        return false;
    lx = x - inst->originX;
    ly = y - inst->originY;
    for (unsigned i = 0; i < count; ++i)
    {
        const BuildingModel *m = &sModels[sPageModels[p[i].pageModel].model];
        int cx = lx - p[i].x, cy = ly - p[i].y;
        uint8_t h;

        if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h)
            continue;
        /* A hedge's rectangle holds the house it runs round. */
        h = sHeights[m->heights + (unsigned)cy * m->w + (unsigned)cx];
        if (h == 0xFF)
            continue;
        if (groundMetatile != NULL)
            *groundMetatile = p[i].ground;
        if (top != NULL)
            *top = h / 16.0f;
        return true;
    }
    return false;
}

bool VoxelBuildings_EmitSome(VoxelBuilder *builder, const VoxelMapInstance *inst,
                             int x0, int y0, int x1, int y1, VoxelBuildingCursor *cursor,
                             unsigned triangles)
{
    unsigned count;
    const BuildingPlacement *p = LayoutPlacements(inst, &count);

    for (; p != NULL && cursor->placement < count; ++cursor->placement, cursor->part = 0,
                                                     cursor->vertex = 0)
    {
        unsigned i = cursor->placement;
        const BuildingPageModel *pm = &sPageModels[p[i].pageModel];
        const BuildingModel *m = &sModels[pm->model];
        const BuildingPage *page = &sPages[pm->page];
        float wx = (float)(inst->originX + p[i].x);
        float wz = (float)(inst->originY + p[i].y);
        float su = 1.0f / page->w, sv = 1.0f / page->h;
        /* The model, then this placement's own ground patches: the model's
         * uv are pixels of its drawing, placed on the page at (ox, oy). */
        const uint32_t first[2] = { m->firstVertex, p[i].extraFirst };
        const uint32_t total[2] = { m->vertexCount, p[i].extraCount };
        const float ox[2] = { (float)pm->ox, 0.0f }, oy[2] = { (float)pm->oy, 0.0f };

        if (inst->originX + p[i].x < x0 || inst->originX + p[i].x >= x1
         || inst->originY + p[i].y < y0 || inst->originY + p[i].y >= y1)
            continue;
        /* On a lifted plateau the building stands on it: the lift of the
         * cell under its door, the bottom-left of its rectangle. */
        builder->lift = VoxelRelief_CellLift(inst, inst->originX + p[i].x,
                                             inst->originY + p[i].y + m->h - 1);
        builder->shift = VoxelRelief_CellShift(inst, inst->originX + p[i].x,
                                             inst->originY + p[i].y + m->h - 1);
        for (; cursor->part < 2; ++cursor->part, cursor->vertex = 0)
        {
            unsigned r = cursor->part;
            const VoxelVertex *v = &sVertices[first[r]];

            for (; cursor->vertex + 2 < total[r]; cursor->vertex += 3)
            {
                uint32_t k = cursor->vertex;
                VoxelVertex t[3] = { v[k], v[k + 1], v[k + 2] };

                if (triangles == 0)
                {
                    builder->lift = 0.0f;
                    builder->shift = 0.0f;
                    return false;
                }
                --triangles;

                for (int j = 0; j < 3; ++j)
                {
                    t[j].x += wx;
                    t[j].z += wz;
                    t[j].u = (ox[r] + t[j].u) * su;
                    t[j].v = 1.0f - (oy[r] + t[j].v) * sv;
                }
                VoxelBuilder_Tri(builder, &t[0], &t[1], &t[2]);
            }
        }
        builder->lift = 0.0f;
        builder->shift = 0.0f;
    }
    return true;
}

void VoxelBuildings_EmitInstance(VoxelBuilder *builder, const VoxelMapInstance *inst,
                                 int x0, int y0, int x1, int y1)
{
    VoxelBuildingCursor cursor = { 0, 0, 0 };

    VoxelBuildings_EmitSome(builder, inst, x0, y0, x1, y1, &cursor, UINT32_MAX);
}
