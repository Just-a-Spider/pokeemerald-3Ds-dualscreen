# Buildings modelled from their own drawing

`scripts/gen_voxel_buildings.py` turns every building described in
`scripts/voxel_building_specs.py` into a 3D model and writes them to
`voxel/buildings.bin` (an RGBA5551 texture in PICA order plus triangles in the
exact `VoxelVertex` format). On the console, `src/voxel/voxel_building.c`
copies the triangles into the chunk of the model's top-left cell and draws
flat ground under its cells.

## The principle

Emerald's art is a fixed oblique projection: a point (X, Y, Z) in pixels lands
at `u = X, v = Z − Y`, which is what an orthographic camera sees at 45°. So:

1. **Every texel is one pixel** seen at 45° square to its face: 1:1 on walls
   and floors, `cos b + sin b` along a roof slope of pitch `b`.
   `density_check` verifies it triangle by triangle. Nothing is stretched.
2. **The front is the drawing**: the generator renders the model at 45° and
   requires a pixel-exact match in the spec's `exact` zones (facade, eave,
   upper floor…). If it does not match, the build fails.
3. **Real depth**: the drawing is flatter than the building (a house with 4
   rows of collision drawn in 5 rows of art would be 2 cells deep). The model
   takes the real footprint and fills the missing depth with **more courses
   of the same tiles** or more of the same plaster column — never stretching.
4. **Sides and backs from the art itself**: hip roofs with courses laid
   parallel to each eave; side walls with the clean plaster column and the
   facade's corner posts.
5. **No ground**: a pixel belongs to the building if its upper layer is opaque
   or its lower-layer subtile is not one of the ground subtiles around it.
   Everything else is transparent and the cell gets the ground metatile.

## Modelling a new building

1. Find one copy in a layout and its rectangle of cells.
2. Read the art's bands row by row (see the docstring of
   `voxel_building_specs.py` for the Littleroot house): facade, eave (fascia),
   roof courses and their period, ridge teeth and cap, clean columns
   (`ROOF_COLUMNS`) without edge boards.
3. Describe the parts: `Prism` with `Proj` for walls at their drawn depth,
   `HipRoof` for roofs, `Strip` for coursed faces, `Tile` for plaster and posts.
4. Declare the `exact` zones and run:

   ```
   python scripts/gen_voxel_buildings.py --preview build/buildings
   ```

   It writes `*_ortho.png` (art | 45° render | differences) and perspective
   views from several cameras (`game`, `left`, `right`, `yaw35`, `yaw-50`,
   `high`) framed like the voxel renderer.
5. The generator finds every appearance of the same drawing in every layout
   with the same tilesets, so a building is modelled once.

## Reuse across Hoenn

- `match_rows` says which rows of the drawing must repeat cell for cell. If
  they all come from the primary tileset, the building matches in **any**
  town sharing that primary, whatever its secondary. That is how the Pokémon
  Center and the Mart (two specs) cover their 20 appearances.
- The spec points at a **canonical** copy (Petalburg's Center, Mauville's
  Mart): other towns paint things of their own in the top row (the path in
  Oldale, a tree crown, a cliff in Lilycove and Verdanturf, a sign in
  Lavaridge).
- Those cells are not lost: where the model has no geometry, the generator
  creates a **ground patch** from what the map paints there minus the
  building's pixels and lays it flat on the ground with the model. If the
  model does occupy the cell, the log says so.
- The ground under each placement is the most common walkable metatile around
  it in that map, which its atlas always contains.
- `--town LAYOUT_X` renders the whole layout with every model and the same
  rules as the console.

## Free-form objects (hedges, walls)

A spec with `components` does not describe a building but a set of metatiles
(Petalburg's hedges). Every connected run of those metatiles, in every layout
of that tileset, becomes its own model:

- The art comes only from the run's cells, and the ground is separated
  **pixel by pixel** (foliage and grass share a subtile on the hedge's flank).
- `Relief` reads it column by column: in each column the drawing shows the
  object's top and, at its south end, its front of `height` rows. The top goes
  at that height and the front is vertical, both projected, so the drawing is
  reproduced exactly. Flanks and back are dressed with a piece of the drawn
  front.
- Cells of its rectangle that are not part of the run (the surrounding house)
  are marked with height 255 and the console does not claim them.
- A run is only placed where it was found: a short one also appears inside
  every long one.

## Memory

The shared texture lives in VRAM (uploaded through the atlas staging buffer,
so at most 512×256). The generator fails if it is exceeded.

## Cost on Old 3DS

Littleroot house: 614 triangles (1,842 vertices, 43 KiB). Shared texture
256×128 (64 KiB of linear memory) for every model.
