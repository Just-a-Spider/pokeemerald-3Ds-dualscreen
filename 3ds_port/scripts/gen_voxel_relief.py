#!/usr/bin/env python3
"""Terrain relief read off the drawing: cliffs, their stairs, rocks at sea.

The premise is the one voxel_building.py builds on: the GBA draws the world
in a 45-degree oblique projection, a point (X, Y, Z) of it landing on the art
at u = X, v = Z - Y. Turned round, a height h(u, v) for every point of the
art places that point at

    (u, h, v + h)

and for ANY choice of h the GBA view of the result is the drawing itself,
pixel for pixel. So terrain is not built of boxes: the art is laid as a sheet
and lifted, and the only question is how high each point of it is.

How high
--------
The cartridge does not say - a plateau and the beach below it share an
elevation - but the drawing does, because in an oblique view a cliff is only
ever seen from the front or from the side:

  * land splits into regions at the rock bands (roles `cliff` and `shelf`)
    and at the stairs through them;
  * a band with land to its north and land to its south, of different
    regions, is a south-facing face: the north region stands one level above
    the south one. Contradictions (a fence line between one wood) are simply
    not constraints;
  * water stands at the lowest land it touches, and a level below any land
    it only reaches across a rock band (a sea cliff facing west or east).

One level is one tile - the rock band is drawn one cell tall. Heights live on
a lattice every 4 pixels. Land and water fix it; the rock and stair cells in
between are solved harmonically, which makes a south face exactly vertical (h
falls one pixel per row, so v + h is constant down it), a west or east face a
45-degree slope (the diagonal striations the artist drew) and every corner of
the staircase a smooth turn - never a square block. A rock mass with no higher
land beside it (a rock at sea) is raised as a mound by its distance from the
water.

Ledges
------
Every outdoor map's ledges are lifted the same way, on maps whose cliffs
are not solved as well: the brown lip, told from the ground round it by its
colours, rises
from its foot on the jump side to LIP pixels at its top edge, and the ground
behind rises to meet it, a low berm facing where the jump goes. Corner cells
jump two ways and take the nearer foot; the junction pieces where a ledge
steps are ledge too; a lip comes down to the ground where the ledge ends.

    python gen_voxel_relief.py [--preview DIR] [--layouts A,B] [--output FILE]
"""

import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_building as vb  # noqa: E402
import voxel_cells as vc  # noqa: E402

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

LEVEL = 16          # pixels per level: the rock band is drawn one cell tall
STEP = 4            # lattice spacing in pixels
PER_CELL = 16 // STEP
MOUND = 8           # mound rise per lattice step inland from the water
MOUND_MAX = 24

# Maps the relief is solved for. Others keep the region extrusion until each
# has been looked at: a height model is a claim about a whole map.
ENABLED = ["LAYOUT_ROUTE104", "LAYOUT_RUSTBORO_CITY"]

RELIEF_ROLES = {"cliff", "shelf", "stair"}

# Ledges. The cartridge names each ledge cell's jump by its behaviour; the
# drawing gives its lip, the half of the cell on the jump side painted in the
# colours no ground round it has.
JUMPS = {0x38: ((1, 0),), 0x39: ((-1, 0),), 0x3A: ((0, -1),), 0x3B: ((0, 1),),
         0x3C: ((1, 0), (0, -1)), 0x3D: ((-1, 0), (0, -1)),
         0x3E: ((1, 0), (0, 1)), 0x3F: ((-1, 0), (0, 1))}
LIP = 6             # height of the lip's top edge, pixels
LIP_WIDTH = 8       # the lip is drawn half a cell deep: its foot to its top
LIP_BACK = 8        # the ground behind the lip rises to it over this many pixels


def solve(layout):
    """Per-cell level (None for relief cells) and the lattice heights."""
    W, H = layout.w, layout.h
    role = [[layout.role_at(x, y) for x in range(W)] for y in range(H)]
    kind = [["relief" if role[y][x] in RELIEF_ROLES else
             "water" if role[y][x] == "water" else "land" for x in range(W)] for y in range(H)]

    # land regions
    region = [[-1] * W for _ in range(H)]
    count = 0
    for y in range(H):
        for x in range(W):
            if kind[y][x] != "land" or region[y][x] >= 0:
                continue
            stack = [(x, y)]
            region[y][x] = count
            while stack:
                cx, cy = stack.pop()
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < W and 0 <= ny < H and kind[ny][nx] == "land" and region[ny][nx] < 0:
                        region[ny][nx] = count
                        stack.append((nx, ny))
            count += 1

    # south faces: land above a rock run and land below it
    edges = {}
    for x in range(W):
        y = 0
        while y < H:
            if kind[y][x] != "relief" or role[y][x] == "stair":
                y += 1
                continue
            y0 = y
            while y < H and kind[y][x] == "relief" and role[y][x] != "stair":
                y += 1
            if y0 > 0 and y < H and kind[y0 - 1][x] == "land" and kind[y][x] == "land":
                up, down = region[y0 - 1][x], region[y][x]
                if up != down:
                    edges[(up, down)] = edges.get((up, down), 0) + 1

    # levels: breadth first over "up is down + 1", first answer kept
    level = [None] * count
    adj = [[] for _ in range(count)]
    for (up, down), n in edges.items():
        adj[up].append((down, -1))
        adj[down].append((up, +1))
    for start in range(count):
        if level[start] is not None:
            continue
        level[start] = 0
        comp, queue = [start], [start]
        while queue:
            r = queue.pop()
            for (o, d) in adj[r]:
                if level[o] is None:
                    level[o] = level[r] + d
                    comp.append(o)
                    queue.append(o)
        low = min(level[r] for r in comp)
        for r in comp:
            level[r] -= low

    cell = [[None] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if kind[y][x] == "land":
                cell[y][x] = level[region[y][x]]

    # Level 0 is the land the map meets its neighbours with: a town beside
    # this route is drawn flat, so the seam must be too. Whatever lies lower
    # (a beach under its cliff) goes below the ground plane.
    edge = []
    for side in getattr(layout, "connected_sides", ()):
        cells = {"up": [(x, 0) for x in range(W)], "down": [(x, H - 1) for x in range(W)],
                 "left": [(0, y) for y in range(H)], "right": [(W - 1, y) for y in range(H)]}[side]
        edge += [cell[y][x] for (x, y) in cells if kind[y][x] == "land"]
    if edge:
        ref = max(set(edge), key=edge.count)
        for y in range(H):
            for x in range(W):
                if cell[y][x] is not None:
                    cell[y][x] -= ref

    # water: the lowest land it touches, body by body
    seen = set()
    for y in range(H):
        for x in range(W):
            if kind[y][x] != "water" or (x, y) in seen:
                continue
            body, stack, touch = [], [(x, y)], []
            seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                body.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if not (0 <= nx < W and 0 <= ny < H):
                        continue
                    if kind[ny][nx] == "water" and (nx, ny) not in seen:
                        seen.add((nx, ny))
                        stack.append((nx, ny))
                    elif kind[ny][nx] == "land":
                        touch.append(cell[ny][nx])
                    elif kind[ny][nx] == "relief" and role[ny][nx] != "stair":
                        # across a rock band the land stands above the water
                        dx, dy = nx - cx, ny - cy
                        ax, ay = nx, ny
                        for _ in range(3):
                            ax, ay = ax + dx, ay + dy
                            if not (0 <= ax < W and 0 <= ay < H) or kind[ay][ax] == "water":
                                break
                            if kind[ay][ax] == "land":
                                touch.append(cell[ay][ax] - 1)
                                break
            lv = min(touch) if touch else 0
            for (cx, cy) in body:
                cell[cy][cx] = lv

    # Rock masses with one level all round are not terrain steps. Touching
    # water they are rocks at sea, raised as mounds below; on land they are a
    # fence line or a patch of soil the role table calls rock, and they stay
    # level with the ground round them for the structure pass to stand up.
    mounds = []
    mass_seen = set()
    for y in range(H):
        for x in range(W):
            if kind[y][x] != "relief" or role[y][x] == "stair" or (x, y) in mass_seen:
                continue
            mass, stack, around, wet = [], [(x, y)], set(), False
            mass_seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                mass.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if not (0 <= nx < W and 0 <= ny < H):
                        continue
                    if kind[ny][nx] == "relief" and role[ny][nx] != "stair":
                        if (nx, ny) not in mass_seen:
                            mass_seen.add((nx, ny))
                            stack.append((nx, ny))
                    elif cell[ny][nx] is not None:
                        around.add(cell[ny][nx])
                        wet = wet or kind[ny][nx] == "water"
            if len(around) > 1:
                continue
            if wet and not awash(layout, mass, kind):
                mounds.append(mass)
            else:
                lv = around.pop() if around else 0
                for (cx, cy) in mass:
                    cell[cy][cx] = lv

    # lattice: fixed where any non-relief cell touches, the highest of them
    LW, LH = W * PER_CELL + 1, H * PER_CELL + 1
    h = [[0.0] * LW for _ in range(LH)]
    fixed = [[False] * LW for _ in range(LH)]
    for j in range(LH):
        for i in range(LW):
            touching = []
            xs = (i // PER_CELL - 1, i // PER_CELL) if i % PER_CELL == 0 else (i // PER_CELL,)
            ys = (j // PER_CELL - 1, j // PER_CELL) if j % PER_CELL == 0 else (j // PER_CELL,)
            for cx in xs:
                for cy in ys:
                    if 0 <= cx < W and 0 <= cy < H and cell[cy][cx] is not None:
                        touching.append(cell[cy][cx])
            if touching:
                h[j][i] = max(touching) * LEVEL
                fixed[j][i] = True
    # harmonic fill of the relief (Gauss-Seidel, warm-started at the mean)
    free = [(i, j) for j in range(LH) for i in range(LW) if not fixed[j][i]]
    for _ in range(400):
        delta = 0.0
        for (i, j) in free:
            s, n = 0.0, 0
            for (a, b) in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)):
                if 0 <= a < LW and 0 <= b < LH:
                    s += h[b][a]
                    n += 1
            v = s / n
            delta = max(delta, abs(v - h[j][i]))
            h[j][i] = v
        if delta < 0.01:
            break

    # mounds: rocks at sea, raised by their distance from the water
    for mass in mounds:
        inside = set()
        for (cx, cy) in mass:
            for j in range(cy * PER_CELL, cy * PER_CELL + PER_CELL + 1):
                for i in range(cx * PER_CELL, cx * PER_CELL + PER_CELL + 1):
                    if not fixed[j][i]:
                        inside.add((i, j))
        # distance (in lattice steps) to the nearest fixed point
        dist = {}
        frontier = [p for p in inside if any(
            0 <= a < LW and 0 <= b < LH and fixed[b][a]
            for (a, b) in ((p[0] + 1, p[1]), (p[0] - 1, p[1]), (p[0], p[1] + 1), (p[0], p[1] - 1)))]
        for p in frontier:
            dist[p] = 1
        k = 0
        while k < len(frontier):
            p = frontier[k]
            k += 1
            for q in ((p[0] + 1, p[1]), (p[0] - 1, p[1]), (p[0], p[1] + 1), (p[0], p[1] - 1)):
                if q in inside and q not in dist:
                    dist[q] = dist[p] + 1
                    frontier.append(q)
        for p, d in dist.items():
            h[p[1]][p[0]] += min(MOUND_MAX, d * MOUND)
    return role, cell, h


_ART = {}


# ── mountains read off the drawing ──────────────────────────────────────────
#
# The mountains of the General tileset (Route 116, Verdanturf, ...) are drawn
# terrace upon terrace, and their tops, faces and rims are told apart by their
# colours, not by the cell they are in: most of their metatiles are part top,
# part face. So these maps are read pixel by pixel.
#
#   * a pixel is top, face, rim (the pale edge a terrace is drawn with where it
#     rises from the one north of it: its own face, turned north, is hidden),
#     ground (anything else: grass, trees, houses), or free (stairs);
#   * tops and grounds are regions, 4-connected, each of its own kind;
#   * down a column, a face between two regions is a south face: seen straight
#     on at 45 degrees it is drawn as tall as it is, so the region above stands
#     exactly as many pixels higher as the face is long. A rim is a step of
#     RIM_RISE up to the south;
#   * the levels are the least-squares answer to all of that, weighed by how
#     many columns say it, the ground at the maps' open edges held at 0. Runs
#     that disagree with the answer (a west or east face read down its length)
#     are dropped and it is solved again;
#   * the lattice takes a region's level at every point inside it; the rest -
#     faces, rims, stairs and the foot of every south face - is a straight
#     ramp between the levels either side of it, down its column for a south
#     face and along its row for a west or east one: a plane that leans, never
#     a right angle.
#
# Maps drawn across a seam are solved on one canvas, placed as they connect,
# so the mountain is one mountain on both sides of it.

DRAWN = {
    "route116": {"LAYOUT_ROUTE116": (0, 0), "LAYOUT_VERDANTURF_TOWN": (80, 20)},
}
ROCK_TOP = {(0xde, 0xb4, 0xa4), (0xbd, 0x94, 0x8b)}
ROCK_RIM = {(0xee, 0xd5, 0xcd)}
ROCK_FACE = {(0x83, 0x5a, 0x5a), (0x62, 0x41, 0x52), (0x41, 0x31, 0x41)}
ROCK_FLECK = {(0x9c, 0x73, 0x73)}   # in both: the tops' speckle, the faces' shading
RIM_RISE = 16
# West and east faces are drawn as bands down the side of the higher terrace,
# one band a level: the face on the left of the cell and the top on its right
# faces west, and the other way round east. Nothing in a 45-degree drawing
# says how tall they are, so each is a level, as its south faces are.
SIDE_WEST = {0x070, 0x073}
SIDE_EAST = {0x072, 0x075, 0x0a2}
SIDE_RISE = 16
# No face is a wall: a south face's drop runs on past its foot for FOOT
# pixels of the land below, a west or east one's along its row the same way,
# so each leans back (about 45 degrees for a level) and a flight of stairs is
# a ramp.
FOOT = 16
REGION_MIN = 200    # smaller patches of top (a boulder, a speck) are not terraces
THIN = 7            # nor is a strip of top thinner than this down its columns, on average
SMOOTH = 2          # lattice columns either side a spike in a face's edge is voted out over
STEP_BREAK = 6      # a face's edge only steps where the drawing steps this much
MEDIAN_PASSES = 2   # of a 3x3 median over the faces, against lone spikes
MAJORITY = 2        # half-width of the window a rock pixel's kind is voted in

GROUND, TOP, FACE, FLECK, RIM, VOID, FREE = range(7)

_DRAWN = {}
_SHIFT = {}     # layout -> the depth lattice of a drawn map (see solve_drawn)


def drawn_group(layout_id):
    for name, members in DRAWN.items():
        if layout_id in members:
            return name
    return None


def drawn_canvas(name):
    """The group's canvas: (layouts, width, height, per-pixel kind, per-cell
    hiding - a roof or a tree a face can go down behind -, per-cell side: +1
    a face turned west, -1 east)."""
    members = DRAWN[name]
    layouts = {lid: open_roles(lid) for lid in members}
    for lid in members:
        if lid not in _ART:
            _ART[lid] = vb.LayoutArt(lid)
    CW = max(ox + layouts[l].w for l, (ox, oy) in members.items())
    CH = max(oy + layouts[l].h for l, (ox, oy) in members.items())
    W, H = CW * 16, CH * 16
    kind = [[VOID] * W for _ in range(H)]
    blocked = [[False] * CW for _ in range(CH)]
    side = [[0] * CW for _ in range(CH)]
    for lid, (ox, oy) in members.items():
        L, A = layouts[lid], _ART[lid]
        for cy in range(L.h):
            for cx in range(L.w):
                # a face goes down behind a roof or a tree, not a signpost
                blocked[oy + cy][ox + cx] = L.blocked(cx, cy) and                     L.role_at(cx, cy) in ("tree", "wall", "prop")
                m = A.metatile(cx, cy)
                side[oy + cy][ox + cx] = 1 if m in SIDE_WEST else -1 if m in SIDE_EAST else 0
                img = A.cell_image(A.metatile(cx, cy)).load()
                stair = L.role_at(cx, cy) == "stair"
                for j in range(16):
                    row = kind[(oy + cy) * 16 + j]
                    for i in range(16):
                        c = img[i, j][:3]
                        row[(ox + cx) * 16 + i] = (
                            FREE if stair else TOP if c in ROCK_TOP else
                            FACE if c in ROCK_FACE else FLECK if c in ROCK_FLECK else
                            RIM if c in ROCK_RIM else GROUND)
    # a rock pixel is what most rock round it is: faces are speckled with the
    # tops' colours and the tops with the faces'
    voted = [row[:] for row in kind]
    R = MAJORITY
    for y in range(H):
        for x in range(W):
            if kind[y][x] not in (TOP, FACE, FLECK):
                continue
            t = f = 0
            for j in range(max(0, y - R), min(H, y + R + 1)):
                row = kind[j]
                for i in range(max(0, x - R), min(W, x + R + 1)):
                    k = row[i]
                    t += k == TOP or k == RIM
                    f += k == FACE
            voted[y][x] = TOP if t > f else FACE
    return layouts, CW, CH, voted, blocked, side


def solve_drawn(name):
    """{layout id: lattice} of a group of maps drawn as one."""
    if name in _DRAWN:
        return _DRAWN[name]
    members = DRAWN[name]
    layouts, CW, CH, kind, blocked, side = drawn_canvas(name)
    W, H = CW * 16, CH * 16

    # regions
    region = [[-1] * W for _ in range(H)]
    sizes = []
    for y in range(H):
        for x in range(W):
            k = kind[y][x]
            if k not in (GROUND, TOP) or region[y][x] >= 0:
                continue
            n = len(sizes)
            stack, count = [(x, y)], 0
            region[y][x] = n
            while stack:
                a, b = stack.pop()
                count += 1
                for (i, j) in ((a + 1, b), (a - 1, b), (a, b + 1), (a, b - 1)):
                    if 0 <= i < W and 0 <= j < H and region[j][i] < 0 and kind[j][i] == k:
                        region[j][i] = n
                        stack.append((i, j))
            sizes.append(count)
    # a strip of top a few pixels thick - the light lip between two faces
    # stacked one on the other - is part of the face, not a terrace
    columns = [set() for _ in sizes]
    for y in range(H):
        row = region[y]
        for x in range(W):
            if row[x] >= 0:
                columns[row[x]].add(x)
    big = [s >= REGION_MIN and s >= THIN * len(columns[n]) for n, s in enumerate(sizes)]

    # what the columns say: (upper, lower) -> drops
    runs = {}
    for x in range(W):
        y = 0
        while y < H:
            k = kind[y][x]
            if k not in (FACE, RIM):
                y += 1
                continue
            y0 = y
            while y < H and kind[y][x] == k:
                y += 1
            if y0 == 0 or y == H:
                continue
            a, b = region[y0 - 1][x], region[y][x]
            if a < 0 or b < 0 or a == b or not (big[a] and big[b]):
                continue
            if kind[y][x] == GROUND and blocked[y // 16][x // 16]:
                continue  # its foot is behind a roof or a tree: not all of it shows
            if k == FACE and sum(side[j // 16][x // 16] != 0 for j in range(y0, y)) * 2 > y - y0:
                continue  # down a west or east face, not across a south one
            drop = y - y0 if k == FACE else -RIM_RISE
            runs.setdefault((a, b), []).append(drop)

    # across the rows: the west and east faces
    for y in range(H):
        x = 0
        row = kind[y]
        while x < W:
            if row[x] != FACE:
                x += 1
                continue
            x0 = x
            while x < W and row[x] == FACE:
                x += 1
            if x0 == 0 or x == W:
                continue
            turn = sum(side[y // 16][i // 16] for i in range(x0, x))
            if abs(turn) * 2 <= x - x0:
                continue
            left, right = region[y][x0 - 1], region[y][x]
            if left < 0 or right < 0 or left == right or not (big[left] and big[right]):
                continue
            if turn > 0:
                runs.setdefault((right, left), []).append(SIDE_RISE)
            else:
                runs.setdefault((left, right), []).append(SIDE_RISE)

    # held at 0: the ground at the canvas's open edges
    held = set()
    for lid, (ox, oy) in members.items():
        L = layouts[lid]
        for edge in L.connected_sides:
            if edge == "up":
                px = [((ox + cx) * 16 + i, oy * 16) for cx in range(L.w) for i in range(16)]
            elif edge == "down":
                px = [((ox + cx) * 16 + i, (oy + L.h) * 16 - 1)
                      for cx in range(L.w) for i in range(16)]
            elif edge == "left":
                px = [(ox * 16, (oy + cy) * 16 + j) for cy in range(L.h) for j in range(16)]
            else:
                px = [((ox + L.w) * 16 - 1, (oy + cy) * 16 + j)
                      for cy in range(L.h) for j in range(16)]
            dx = {"left": -1, "right": 1}.get(edge, 0)
            dy = {"up": -1, "down": 1}.get(edge, 0)
            for (x, y) in px:
                # an edge that meets another member is a seam, not open
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and kind[ny][nx] != VOID:
                    continue
                r = region[y][x]
                if r >= 0 and big[r] and kind[y][x] == GROUND:
                    held.add(r)

    def levels(constraints):
        h = [0.0] * len(sizes)
        adj = [[] for _ in sizes]
        for (a, b), (drop, w) in constraints.items():
            adj[a].append((b, drop, w))    # h[a] = h[b] + drop
            adj[b].append((a, -drop, w))
        for _ in range(5000):
            delta = 0.0
            for r in range(len(sizes)):
                if r in held or not adj[r]:
                    continue
                s = sum(w * (h[o] + d) for (o, d, w) in adj[r])
                v = s / sum(w for (_, _, w) in adj[r])
                delta = max(delta, abs(v - h[r]))
                h[r] = v
            if delta < 0.001:
                break
        return h

    def median(v):
        v = sorted(v)
        return v[len(v) // 2]

    level = levels({k: (median(v), len(v)) for k, v in runs.items()})
    # drop the runs that disagree - a side face read down its length, a face
    # whose foot is hidden behind something - and solve again
    kept = {}
    for (a, b), v in runs.items():
        good = [d for d in v if abs(level[a] - level[b] - d) <= 8]
        if good:
            kept[(a, b)] = (median(good), len(good))
    level = [round(v) for v in levels(kept)]

    def hidden_foot(x, y):
        """A face whose foot is a roof or a tree it goes down behind: the
        cell FOOT pixels on is blocked ground."""
        y = min(y + FOOT - 1, H - 1)
        return kind[y][x] == GROUND and blocked[y // 16][x // 16]

    def straight(edge):
        """An edge of a face along the face, made straight: constant in runs,
        broken only where the drawing steps (STEP_BREAK pixels or more, a
        cell's corner), never following the drawn rim's wobble."""
        med = [sorted(edge[max(0, k - SMOOTH):k + SMOOTH + 1])[
            len(edge[max(0, k - SMOOTH):k + SMOOTH + 1]) // 2] for k in range(len(edge))]
        out, start = [], 0
        for k in range(1, len(med) + 1):
            if k == len(med) or abs(med[k] - sorted(med[start:k])[(k - start) // 2]) >= STEP_BREAK:
                seg = sorted(med[start:k])
                out += [seg[len(seg) // 2]] * (k - start)
                start = k
        return out

    # the lattice: a point inside one terrace or ground is at its level; the
    # rest - faces, rims, stairs and the foot of every south face - is solved
    # harmonically between them
    LW, LH = CW * PER_CELL + 1, CH * PER_CELL + 1
    h = [[0.0] * LW for _ in range(LH)]
    state = [[0] * LW for _ in range(LH)]   # 0 off the canvas, 1 fixed, 2 free
    for j in range(LH):
        for i in range(LW):
            px, py = i * STEP, j * STEP
            around = [(x, y) for x in (px - 1, px) for y in (py - 1, py)
                      if 0 <= x < W and 0 <= y < H and kind[y][x] != VOID]
            if not around:
                continue
            regs = {region[y][x] for (x, y) in around}
            r = min(regs)
            cx, cy = min(px, W - 1), min(py, H - 1)
            foot = not hidden_foot(cx, cy) and any(
                0 <= py - d < H and kind[py - d][cx] in (FACE, FREE)
                and not side[(py - d) // 16][cx // 16] for d in range(1, FOOT + 1))
            if len(regs) == 1 and r >= 0 and big[r] and not foot:
                h[j][i] = float(level[r])
                state[j][i] = 1
            else:
                state[j][i] = 2
    interior = [[state[j][i] == 1 for i in range(LW)] for j in range(LH)]
    # South faces are planes: down each lattice column a face (with stairs,
    # with the strips of lip in it) runs straight from the level above it to
    # the level below FOOT pixels past its foot, and its top edge and its foot
    # are evened out along the face, so the drawing's ragged rim stays in the
    # texture and not in the shape.
    faces = []
    for i in range(LW):
        px = min(i * STEP, W - 1)
        y = 0
        while y < H:
            r = region[y][px]
            facey = kind[y][px] in (FACE, FREE) or (kind[y][px] in (GROUND, TOP) and r >= 0
                                                    and not big[r])
            if not facey or side[y // 16][px // 16]:
                y += 1
                continue
            y0 = y
            while y < H and not side[y // 16][px // 16] and (
                    kind[y][px] in (FACE, FREE) or (kind[y][px] in (GROUND, TOP)
                                                    and region[y][px] >= 0
                                                    and not big[region[y][px]])):
                y += 1
            if y0 == 0 or y == H:
                continue
            a, b = region[y0 - 1][px], region[y][px]
            if a < 0 or b < 0 or a == b or not (big[a] and big[b]) or level[a] <= level[b]:
                continue
            foot = 0 if hidden_foot(px, y) else FOOT
            faces.append([i, y0, y, a, b, foot])
    # faces: runs in neighbouring columns between the same two regions
    by_pair = {}
    for f in faces:
        by_pair.setdefault((f[3], f[4]), []).append(f)
    for runs_ in by_pair.values():
        runs_.sort()
        groups, cur = [], [runs_[0]]
        for f in runs_[1:]:
            g = cur[-1]
            if f[0] == g[0] + 1 and f[1] < g[2] and g[1] < f[2]:
                cur.append(f)
            else:
                groups.append(cur)
                cur = [f]
        groups.append(cur)
        for g in groups:
            y0s = straight([f[1] for f in g])
            y1s = straight([f[2] for f in g])
            for k, f in enumerate(g):
                t0, t1 = y0s[k], y1s[k] + f[5]
                la, lb = level[f[3]], level[f[4]]
                for j in range(LH):
                    py = j * STEP
                    if t0 <= py <= t1 and state[j][f[0]]:
                        h[j][f[0]] = la + (lb - la) * (py - t0) / float(t1 - t0)
                        state[j][f[0]] = 1
    # West and east faces are planes too, along each lattice row from the
    # level on one side to the level on the other, their edges evened out
    # down the band
    bands = []
    for j in range(LH):
        py = min(j * STEP, H - 1)
        row, reg = kind[py], region[py]
        x = 0
        while x < W:
            if row[x] != FACE or not side[py // 16][x // 16]:
                x += 1
                continue
            x0 = x
            while x < W and (row[x] == FACE or (row[x] in (GROUND, TOP) and reg[x] >= 0
                                                 and not big[reg[x]])):
                x += 1
            if x0 == 0 or x == W:
                continue
            a, b = reg[x0 - 1], reg[x]
            if a < 0 or b < 0 or a == b or not (big[a] and big[b]):
                continue
            bands.append([j, x0, x, a, b])
    by_pair = {}
    for f in bands:
        by_pair.setdefault((f[3], f[4]), []).append(f)
    for runs_ in by_pair.values():
        runs_.sort()
        groups, cur = [], [runs_[0]]
        for f in runs_[1:]:
            g = cur[-1]
            if f[0] == g[0] + 1 and f[1] < g[2] + STEP and g[1] < f[2] + STEP:
                cur.append(f)
            else:
                groups.append(cur)
                cur = [f]
        groups.append(cur)
        for g in groups:
            x0s = straight([f[1] for f in g])
            x1s = straight([f[2] for f in g])
            for k, f in enumerate(g):
                t0, t1 = x0s[k], x1s[k]
                la, lb = level[f[3]], level[f[4]]
                j = f[0]
                for i in range(LW):
                    px = i * STEP
                    if t0 <= px <= t1 and state[j][i] == 2:
                        h[j][i] = la + (lb - la) * (px - t0) / float(max(1, t1 - t0))
                        state[j][i] = 1
    # What is left are the corners where a south face turns into a west or
    # an east one. Each is ramped straight both ways, down its column and
    # along its row between the nearest set points, and takes the lower of the
    # two: the hip of a corner, as a hipped roof turns one, never a spike.
    def corner_ramps(lines):
        found = {}
        for line in lines:
            k = 0
            while k < len(line):
                i, j = line[k]
                if state[j][i] != 2:
                    k += 1
                    continue
                k0 = k
                while k < len(line) and state[line[k][1]][line[k][0]] == 2:
                    k += 1
                if k0 == 0 or k == len(line):
                    continue
                (ia, ja), (ib, jb) = line[k0 - 1], line[k]
                if state[ja][ia] != 1 or state[jb][ib] != 1:
                    continue
                va, vb = h[ja][ia], h[jb][ib]
                for m in range(k0, k):
                    found[line[m]] = va + (vb - va) * (m - k0 + 1) / float(k - k0 + 1)
        return found
    down = corner_ramps([[(i, j) for j in range(LH)] for i in range(LW)])
    along = corner_ramps([[(i, j) for i in range(LW)] for j in range(LH)])
    for p in set(down) | set(along):
        v = min(v for v in (down.get(p), along.get(p)) if v is not None)
        h[p[1]][p[0]] = v
        state[p[1]][p[0]] = 1
    frontier = [(i, j) for j in range(LH) for i in range(LW) if state[j][i] == 1]
    known = set(frontier)
    while frontier:
        nxt = []
        for (i, j) in frontier:
            for (a, b) in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)):
                if 0 <= a < LW and 0 <= b < LH and state[b][a] == 2 and (a, b) not in known:
                    known.add((a, b))
                    h[b][a] = h[j][i]
                    nxt.append((a, b))
        frontier = nxt
    free = [(i, j) for j in range(LH) for i in range(LW) if state[j][i] == 2]
    nbrs = [[(a, b) for (a, b) in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1))
             if 0 <= a < LW and 0 <= b < LH and state[b][a]] for (i, j) in free]
    for _ in range(3000):
        delta = 0.0
        for (i, j), nb in zip(free, nbrs):
            d = sum(h[b][a] for (a, b) in nb) / len(nb) - h[j][i]
            h[j][i] += 1.8 * d
            delta = max(delta, abs(d))
        if delta < 0.01:
            break

    # Each row of a band, each column of a face, ends its ramp at its own
    # pixel of the drawn rim: a column or a row out of step with its
    # neighbours is a spike or a groove. A median over the faces' points
    # (never a terrace's) takes it out and leaves a straight ramp as it is.
    for _ in range(MEDIAN_PASSES):
        out = [row[:] for row in h]
        for j in range(LH):
            for i in range(LW):
                if interior[j][i] or not state[j][i]:
                    continue
                near = sorted(h[b][a] for b in (j - 1, j, j + 1) for a in (i - 1, i, i + 1)
                              if 0 <= a < LW and 0 <= b < LH and state[b][a])
                out[j][i] = near[len(near) // 2]
        h = out

    # every point of the drawing as far south as it is high: (u, h, v + h)
    shift = [row[:] for row in h]

    out = {}
    for lid, (ox, oy) in members.items():
        L = layouts[lid]
        out[lid] = [row[ox * PER_CELL:(ox + L.w) * PER_CELL + 1]
                    for row in h[oy * PER_CELL:(oy + L.h) * PER_CELL + 1]]
        _SHIFT[lid] = [row[ox * PER_CELL:(ox + L.w) * PER_CELL + 1]
                       for row in shift[oy * PER_CELL:(oy + L.h) * PER_CELL + 1]]
    print("drawn %-10s %d regions (%d terraces), %d held at 0, highest %.0f px"
          % (name, len(sizes), sum(big), len(held), max(max(r) for r in h)))
    _DRAWN[name] = out
    return out





def ledge_cells(layout, junctions=True):
    """{(x, y): jump directions} of every ledge cell of a layout.

    Where a ledge turns a corner by a step, the artist joins the two runs
    with a piece the cartridge does not let anyone jump from: a blocked cell
    with ledges on two sides at right angles. It belongs to the ledge, and
    takes the directions of the ledge cells beside it. Not in a map whose cliffs are solved: there the rock
    round it is relief already.
    """
    out = {}
    for y in range(layout.h):
        for x in range(layout.w):
            d = JUMPS.get(layout.behaviour(x, y))
            if d:
                out[(x, y)] = d
    if not junctions or getattr(layout, "layout_id", None) in ENABLED:
        return out
    joins = {}
    for (x, y) in out:
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if (nx, ny) in out or layout.off_map(nx, ny) or not layout.blocked(nx, ny):
                continue
            if not layout.is_ledge_junction(nx, ny):
                continue
            dirs = joins.setdefault((nx, ny), [])
            dirs.extend(d for d in out[(x, y)] if d not in dirs)
    out.update((c, tuple(d)) for c, d in joins.items())
    return out


def ledge_berms(layout, art, h):
    """Raise every ledge's lip off the drawing, added to the lattice `h`.

    The lip's top edge - where the ground behind it meets the brown - stands
    LIP pixels up, its foot on the jump side at the ground: lifted as all
    relief is, (u, h, v + h), the brown is a face turned to where the jump
    goes and the GBA view is still the drawing. The ground behind rises to the
    top edge over LIP_BACK pixels, so the ledge is a low berm, not a step in
    the land.

    A point on the lip is as high as it is far from the lip's foot, walking
    the way the jump goes: LIP at LIP_WIDTH pixels and more. Where the lip
    turns (a corner cell jumps two ways) the nearer foot counts, so an outer
    corner rounds off like the drawing and neither arm is read along its
    length. Where it ends - a point shared with a cell that is no ledge - it
    comes down to the ground, and the drawn end taper does the rest.
    """
    cells = ledge_cells(layout)
    if not cells:
        return 0
    lip = {}
    for (x, y), dirs in cells.items():
        # ground colours: the cells the jump leaves and lands on, else round it
        ground = set()
        for ring in ([(-dx, -dy) for dx, dy in dirs] + list(dirs),
                     [(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]):
            for dx, dy in ring:
                nx, ny = x + dx, y + dy
                if (nx, ny) in cells or layout.off_map(nx, ny) or layout.blocked(nx, ny):
                    continue
                img = art.cell_image(art.metatile(nx, ny))
                ground.update(img.getpixel((i, j))[:3] for j in range(16) for i in range(16))
            if ground:
                break
        if not ground:
            continue
        img = art.cell_image(art.metatile(x, y)).load()
        for j in range(16):
            for i in range(16):
                if img[i, j][:3] not in ground:
                    lip[(x * 16 + i, y * 16 + j)] = True

    def run(px, py, dx, dy, limit=64):
        n = 0
        while n < limit and lip.get((px + n * dx, py + n * dy)):
            n += 1
        return n

    def first(px, py, dx, dy, limit):
        for k in range(limit):
            if lip.get((px + k * dx, py + k * dy)):
                return k
        return None

    def lip_height(f):
        return LIP * min(f, LIP_WIDTH) / float(LIP_WIDTH)

    raised = {}
    for (x, y), dirs in cells.items():
        for j in range(PER_CELL + 1):
            for i in range(PER_CELL + 1):
                px, py = x * 16 + i * STEP, y * 16 + j * STEP
                reads = []
                for dx, dy in dirs:
                    # the pixel line through the point, inside the cell across
                    # the jump, entered on the jump side of the point along it
                    if dx == 0:
                        sx = min(max(px, x * 16), x * 16 + 15)
                        sy = py - (dy < 0)
                    else:
                        sx = px - (dx < 0)
                        sy = min(max(py, y * 16), y * 16 + 15)
                    reads.append((sx, sy, dx, dy, run(sx, sy, dx, dy),
                                  run(sx - dx, sy - dy, -dx, -dy)))
                if all(f for (_, _, _, _, f, _) in reads):
                    best = min(lip_height(f) for (_, _, _, _, f, _) in reads)
                elif any(b and not f for (_, _, _, _, f, b) in reads):
                    best = 0.0  # in front of the lip: the ground jumped down to
                else:
                    # behind the lip: rising to where the line meets it
                    best = 0.0
                    for (sx, sy, dx, dy, f, b) in reads:
                        k = first(sx, sy, dx, dy, LIP_BACK)
                        if k is not None:
                            top = lip_height(run(sx + k * dx, sy + k * dy, dx, dy))
                            best = max(best, top * (1.0 - k / float(LIP_BACK)))
                key = (x * PER_CELL + i, y * PER_CELL + j)
                raised[key] = max(raised.get(key, 0.0), best)
    for (gx, gy), v in raised.items():
        # a point shared with a cell that is no ledge stays on the ground
        touching = [(cx, cy) for cx in {(gx - 1) // PER_CELL, gx // PER_CELL}
                    for cy in {(gy - 1) // PER_CELL, gy // PER_CELL}
                    if 0 <= cx < layout.w and 0 <= cy < layout.h]
        if all(c in cells for c in touching):
            h[gy][gx] += v
    return len(cells)


def flat_lattice(layout):
    return [[0.0] * (layout.w * PER_CELL + 1) for _ in range(layout.h * PER_CELL + 1)]


def layout_heights(layout_id):
    """The lattice of a layout: its relief where it is solved, its ledges."""
    roles_layout = open_roles(layout_id)
    group = drawn_group(layout_id)
    if group:
        h = [row[:] for row in solve_drawn(group)[layout_id]]
    elif layout_id in ENABLED:
        role, cell, h = solve(roles_layout)
    else:
        h = flat_lattice(roles_layout)
    art = _ART.get(layout_id)
    if art is None:
        art = _ART[layout_id] = vb.LayoutArt(layout_id)
    ledges = ledge_berms(roles_layout, art, h)
    return roles_layout, h, ledges


def ledge_layouts():
    """Every outdoor layout that has a ledge."""
    layouts = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    out = []
    for e in layouts:
        path = os.path.join(vb.ROOT, e["blockdata_filepath"])
        if not os.path.exists(path):
            continue
        layout = open_roles(e["id"], connections=False)
        if layout.outdoor and ledge_cells(layout, junctions=False):
            out.append(e["id"])
    return out


def awash(layout, mass, kind):
    """A rock drawn mostly in the colours of the water round it - foam over a
    submerged reef - lies at the water line; only a rock drawn as rock is
    raised out of the sea."""
    art = _ART.get(layout.layout_id)
    if art is None:
        art = _ART[layout.layout_id] = vb.LayoutArt(layout.layout_id)
    water = set()
    for (x, y) in mass:
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < layout.w and 0 <= ny < layout.h and kind[ny][nx] == "water":
                water.update(art.cell_image(art.metatile(nx, ny)).getdata())
    same = total = 0
    for (x, y) in mass:
        for p in art.cell_image(art.metatile(x, y)).getdata():
            total += 1
            same += p in water
    return total and same > total // 2


def cell_grid(h, x, y):
    return [[h[y * PER_CELL + j][x * PER_CELL + i] for i in range(PER_CELL + 1)]
            for j in range(PER_CELL + 1)]


def relief_cells(layout, h):
    """Cells whose lattice is not all zero: (x, y, grid)."""
    out = []
    for y in range(layout.h):
        for x in range(layout.w):
            g = cell_grid(h, x, y)
            if any(abs(v) > 0.25 for row in g for v in row):
                out.append((x, y, g))
    return out


# ── preview ─────────────────────────────────────────────────────────────────

def preview(layout_id, out_dir, cams):
    art = vb.LayoutArt(layout_id)
    roles_layout, h, _ = layout_heights(layout_id)
    grids = {(x, y): g for (x, y, g) in relief_cells(art, h)}
    depth = _SHIFT.get(layout_id)
    ids = {}
    for y in range(art.h):
        for x in range(art.w):
            ids.setdefault(art.metatile(x, y), len(ids))
    cols = 32
    from PIL import Image
    tex = Image.new("RGBA", (cols * 16, ((len(ids) + cols - 1) // cols) * 16), (0, 0, 0, 255))
    for m, i in ids.items():
        tex.paste(art.cell_image(m), ((i % cols) * 16, (i // cols) * 16))
    tris = []
    for y in range(art.h):
        for x in range(art.w):
            i = ids[art.metatile(x, y)]
            u0, v0 = (i % cols) * 16, (i // cols) * 16
            g = grids.get((x, y))
            if g is None:
                g = [[0.0] * (PER_CELL + 1) for _ in range(PER_CELL + 1)]
            for j in range(PER_CELL):
                for k in range(PER_CELL):
                    def P(a, b):
                        hh = g[b][a] / 16.0
                        zz = depth[y * PER_CELL + b][x * PER_CELL + a] / 16.0 if depth else hh
                        return (x + a * STEP / 16.0, hh, y + b * STEP / 16.0 + zz,
                                u0 + a * STEP, v0 + b * STEP)
                    A, B, C, D = P(k, j), P(k + 1, j), P(k + 1, j + 1), P(k, j + 1)
                    tris.append(((A, B, C), 1.0))
                    tris.append(((A, C, D), 1.0))
    paths = []
    for name, (tx, tz, pitch, yaw, dist) in cams:
        cam = vb.Camera((tx, 0.0, tz), pitch=pitch, yaw=yaw, distance=dist)
        img = vb.render_scene(cam, [(tris, tex)], scale=2)
        path = os.path.join(out_dir, "relief_%s_%s.png" % (layout_id.lower(), name))
        img.save(path)
        paths.append(path)
    return paths


_ROLES = []


def open_roles(layout_id, connections=True):
    layouts = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    entry = next(e for e in layouts if e["id"] == layout_id)
    if not _ROLES:
        _ROLES.append(vc.MapEvents())
    layout = vc.Layout(entry, _ROLES[0])
    layout.layout_id = layout_id
    if not connections:
        return layout
    # the sides some map using this layout is connected to another on
    sides = set()
    maps = os.path.join(vb.ROOT, "data", "maps")
    for name in os.listdir(maps):
        path = os.path.join(maps, name, "map.json")
        if not os.path.exists(path):
            continue
        m = json.load(open(path, encoding="utf-8"))
        if m.get("layout") == layout_id:
            for c in m.get("connections") or []:
                if c.get("direction") in ("up", "down", "left", "right"):
                    sides.add(c["direction"])
    layout.connected_sides = sorted(sides)
    layout.layout_id = layout_id
    return layout


# ── export ──────────────────────────────────────────────────────────────────

MAGIC = b"VXL1"


def export(layout_ids, path):
    """"VXL1", u16 layouts, u16 per-cell lattice side (5), then per layout
    u16 layout id, u16 cells, u16 width, u16 height, u32 offset; cells are u8 x,
    u8 y and 25 signed bytes of height in pixels, row major. The top bit of the
    height marks a map read off its drawing (DRAWN): its cells carry 25 more
    bytes, the depth each point lies at (see solve_drawn), and the relief is the
    whole of the terrain, and every blocked cell of rock (a patch of soil the
    role table calls rock too) is written, level or not, so no structure
    stands a box on it."""
    layouts = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    index = {e["id"]: i + 1 for i, e in enumerate(layouts)}
    tables = []
    for lid in layout_ids:
        roles_layout, h, ledges = layout_heights(lid)
        cells = relief_cells(roles_layout, h)
        drawn = drawn_group(lid) is not None
        if drawn:
            have = {(x, y) for (x, y, _) in cells}
            cells += [(x, y, cell_grid(h, x, y)) for y in range(roles_layout.h)
                      for x in range(roles_layout.w)
                      if (x, y) not in have and (
                          (roles_layout.blocked(x, y)
                           and roles_layout.role_at(x, y) in RELIEF_ROLES)
                          or any(abs(v) > 0.25 for row in cell_grid(_SHIFT[lid], x, y)
                                 for v in row))]
            cells.sort(key=lambda c: (c[1], c[0]))
        if not cells:
            continue
        if drawn:
            depth = _SHIFT[lid]
            cells = [(x, y, g + cell_grid(depth, x, y)) for (x, y, g) in cells]
        tables.append((index[lid], cells, roles_layout.w,
                       roles_layout.h | (0x8000 if drawn else 0)))
        print("relief %-34s %4d cells lifted, %3d ledge cells" % (lid, len(cells), ledges))
    side = PER_CELL + 1
    head = MAGIC + struct.pack("<HH", len(tables), side)
    offset = len(head) + 12 * len(tables)
    body = bytearray()
    idx = bytearray()
    for lid, cells, w, hh in sorted(tables):
        idx += struct.pack("<HHHHI", lid, len(cells), w, hh, offset + len(body))
        for (x, y, g) in cells:
            body += struct.pack("<BB", x, y)
            body += struct.pack("<%db" % (len(g) * len(g[0])),
                                *(max(-128, min(127, int(round(v)))) for row in g for v in row))
    blob = head + idx + body
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(blob)
    print("voxel relief: %d layouts, %.1f KiB -> %s" % (len(tables), len(blob) / 1024.0, path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", default=None)
    ap.add_argument("--layouts", default=None,
                    help="default: the solved maps and every map with a ledge")
    ap.add_argument("--output", default=None)
    ap.add_argument("--at", default=None, help="x,z of the preview target")
    args = ap.parse_args()
    if args.layouts:
        lids = args.layouts.split(",")
    else:
        drawn = [l for members in DRAWN.values() for l in members]
        lids = list(ENABLED) + [l for l in drawn if l not in ENABLED]
        lids += [l for l in ledge_layouts() if l not in lids]
    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        tx, tz = (float(v) for v in args.at.split(",")) if args.at else (20.0, 60.0)
        cams = [("game", (tx, tz, 40.0, 0.0, 12.0)), ("yaw40", (tx, tz, 35.0, 40.0, 11.0)),
                ("yaw-45", (tx, tz, 30.0, -45.0, 10.0)), ("high", (tx, tz, 60.0, 15.0, 12.0))]
        for lid in lids:
            for p in preview(lid, args.preview, cams):
                print("preview:", p)
    if args.output:
        export(lids, args.output)


if __name__ == "__main__":
    main()
