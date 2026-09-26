"""Signposts: the sign event rule and the sign silhouette, on synthetic cells
and on Littleroot Town's real signs."""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import voxel_cells  # noqa: E402
from voxel_sign_mask import cutout_mask, ground_colours, metatile_mask  # noqa: E402


class FakeLayout:
    is_signpost = voxel_cells.Layout.is_signpost

    def __init__(self, blocked, signs, outdoor=True):
        self.w = len(blocked[0])
        self.h = len(blocked)
        self.outdoor = outdoor
        self.signs = signs
        self._blocked = blocked

    def off_map(self, x, y):
        return x < 0 or y < 0 or x >= self.w or y >= self.h

    def blocked(self, x, y):
        return False if self.off_map(x, y) else self._blocked[y][x]


def run():
    # An enclosed transparent pocket survives the outside flood.
    rows = cutout_mask({(x, y) for x in range(3) for y in range(3) if (x, y) != (1, 1)}, 3, 3)
    assert rows[1] & (1 << 1)
    # A board that fills its cell is all sign.
    assert cutout_mask({(x, y): 1 for x in range(16) for y in range(16)}) == [65535] * 16

    blocked = [[False] * 3 for _ in range(3)]
    blocked[1][1] = True
    blocked[0][1] = True  # a building behind the sign is allowed
    assert FakeLayout(blocked, {(1, 1)}).is_signpost(1, 1)
    assert not FakeLayout(blocked, set()).is_signpost(1, 1)            # no sign event
    assert not FakeLayout(blocked, {(1, 1)}, outdoor=False).is_signpost(1, 1)
    blocked[1][0] = True
    assert not FakeLayout(blocked, {(1, 1)}).is_signpost(1, 1)         # a side is closed

    entry = next(e for e in voxel_cells.load_layouts() if e.get("id") == "LAYOUT_LITTLEROOT_TOWN")
    layout = voxel_cells.Layout(entry, voxel_cells.MapEvents())
    pair = voxel_cells.pair_for(layout.primary, layout.secondary)
    signs = [(x, y) for y in range(layout.h) for x in range(layout.w)
             if layout.role_at(x, y) == "signpost"]
    assert signs, "Littleroot Town has signs"
    for x, y in signs:
        rows = metatile_mask(pair, layout, x, y)
        assert any(rows)
        ground = ground_colours(pair, layout, x, y)
        for (px, py), colour in pair.layer_pixels(layout.metatile(x, y), 0).items():
            if colour not in ground:
                assert rows[py] & (1 << px)
        for px, py in pair.layer_pixels(layout.metatile(x, y), 1):
            assert rows[py] & (1 << px)
    print("PASS voxel signpost: sign events, open sides, silhouettes (%d real signs)" % len(signs))


if __name__ == "__main__":
    run()
