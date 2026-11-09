# SPDX-License-Identifier: GPL-3.0-or-later
"""Dragonburner: a red fan body and duct lips, black fan blocks, and a compact dark extruder."""
import math

from ..head import Drawing, HeadDef, rounded

BODY, EXTRUDER, BLACK, DARK = "#b8242a", "#3a3b3f", "#2a2b2f", "#101113"
HALF_WIDTH = 0.215
DEPTH = 0.33
BODY_TOP = 0.5
BLOCK_TOP = 0.445
TIP = (0.0, 0.05, 0.012)


def _circle(cx, cz, r, n=24, start=0.0):
    return [(cx + r * math.cos(start + 2 * math.pi * i / n), cz + r * math.sin(start + 2 * math.pi * i / n)) for i in range(n)]


def _cylinder_y(d, cx, cz, r, y0, y1, color, cap, n=20):
    """A cylinder running front to back, its near end capped."""
    ring = _circle(cx, cz, r, n)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])], color)
    d.facet([(x, y0, z) for x, z in ring], cap)


def _cylinder_x(d, cy, cz, r, x0, x1, color, cap, n=14):
    """A cylinder running across the head, its right end capped."""
    ring = _circle(cy, cz, r, n)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], color)
    d.facet([(x1, y, z) for y, z in ring], cap)


def _side_fan(d, x, cy, cz, r):
    """The blower on the head's side: a raised ring, a dark well, blades and a hub."""
    def on_side(pts, dx, color, **kw):
        d.facet([(x + dx, y, z) for y, z in pts], color, **kw)

    on_side(_circle(cy, cz, r * 1.1), 0.001, "#3a3b40", amb=0.8, dif=0.4, spec=0.1)
    on_side(_circle(cy, cz, r), 0.002, DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        for i in range(14):
            t = 2 * math.pi * i / 14
            blade = [(cy + r * k * math.cos(t + s), cz + r * k * math.sin(t + s)) for k, s in ((0.5, 0.0), (0.92, 0.2), (0.92, 0.36), (0.5, 0.16))]
            on_side(blade, 0.003, "#3b3c41", amb=0.9, dif=0.3, spec=0.0)
    on_side(_circle(cy, cz, r * 0.42), 0.004, "#26272b", amb=0.9, dif=0.35, spec=0.05)


def _voron_mark(d, cx, cz, size, y, color):
    for dx, z0, z1 in ((-0.8, 0.0, 1.0), (0.0, -1.0, 1.0), (0.8, -1.0, 0.0)):
        half = 0.27 * size
        pts = [(cx + dx * size + z * size * 0.5 + w, cz + z * size) for z, w in ((z0, -half), (z0, half), (z1, half), (z1, -half))]
        d.face(pts, y, color)


def _extruder(d):
    z = BODY_TOP - 0.595
    # The motor behind.
    _cylinder_y(d, 0.05, 0.82 + z, 0.14, 0.2, 0.26, "#2b2c30", "#1f2023", n=24)
    d.solid(rounded([(-0.13, 0.665 + z), (0.03, 0.665 + z), (0.03, 0.94 + z), (-0.07, 0.925 + z), (-0.13, 0.84 + z)], 0.03), 0.05, 0.2, EXTRUDER)
    d.tube(0.03, 0.1, 0.94 + z, 0.985 + z, 0.03, "#d08a3a")
    # The tension spring and its knob at the top right of the tower.
    _cylinder_x(d, 0.07, 0.925 + z, 0.028, 0.185, 0.222, "#b9bbc0", "#b9bbc0", n=12)
    _cylinder_x(d, 0.07, 0.925 + z, 0.05, 0.222, 0.246, "#1c2452", "#232c63", n=16)
    body = [(-0.1, 0.665 + z), (0.185, 0.665 + z), (0.185, 0.965 + z), (0.15, 1.0 + z), (0.095, 1.0 + z), (0.085, 0.9 + z), (0.03, 0.86 + z), (-0.1, 0.86 + z)]
    d.solid(body, 0.0, 0.13, EXTRUDER, bevel=0.012)
    d.solid(rounded([(-0.135, BODY_TOP), (0.19, BODY_TOP), (0.19, 0.67 + z), (-0.135, 0.67 + z)], 0.012), 0.0, 0.17, EXTRUDER, bevel=0.01)
    # The drive gear boss on the left, a bright bore in its centre.
    _cylinder_y(d, -0.03, 0.8 + z, 0.078, -0.022, 0.01, "#3e3f44", "#2d2e32", n=14)
    d.disc(-0.03, 0.8 + z, 0.036, -0.024, "#b9bbc0", sides=16)
    d.disc(-0.03, 0.8 + z, 0.016, -0.026, DARK, sides=12)
    if d.detail:
        for x, zz in ((0.075, 0.71 + z), (-0.04, 0.915 + z), (0.15, 0.72 + z)):
            d.disc(x, zz, 0.019, -0.002, "#a9abb0", sides=12)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.tube(TIP[0], TIP[1], 0.05, 0.15, 0.04, "#a02a26")
    d.nozzle(TIP[0], TIP[1], 0.05, r_top=0.022, r_tip=0.008, z_tip=TIP[2])
    for s in (1, -1):
        lip = rounded([(s * 0.095, 0.11), (s * HALF_WIDTH, 0.11), (s * HALF_WIDTH, 0.06), (s * 0.165, 0.02), (s * 0.115, 0.02), (s * 0.095, 0.05)], 0.025)
        d.solid(lip, 0.0, DEPTH, BODY)
    # Black fan blocks either side of the strip; the side face is about as tall as it is deep.
    for x0, x1 in ((0.125, HALF_WIDTH), (-HALF_WIDTH, -0.125)):
        d.solid([(x0, 0.1), (x1, 0.1), (x1, BLOCK_TOP), (x0, BLOCK_TOP)], 0.004, DEPTH, BLACK)
    _side_fan(d, HALF_WIDTH, DEPTH / 2, 0.265, 0.105)
    cap = rounded([(-HALF_WIDTH, BODY_TOP - 0.095), (HALF_WIDTH, BODY_TOP - 0.095), (HALF_WIDTH, BODY_TOP - 0.045), (0.16, BODY_TOP), (-0.16, BODY_TOP), (-HALF_WIDTH, BODY_TOP - 0.045)], 0.012)
    d.solid(cap, 0.0, DEPTH, BODY, bevel=0.012)
    d.face([(-0.125, 0.075), (0.125, 0.075), (0.125, BODY_TOP - 0.075), (-0.125, BODY_TOP - 0.075)], 0.0, BODY)
    d.fan(0.0, 0.26, 0.105, -0.002, blade="#232427")
    _voron_mark(d, 0.0, BODY_TOP - 0.05, 0.022, -0.001, "#e8e8ea")
    if d.detail:
        for x in (-0.09, 0.09):
            d.disc(x, BODY_TOP - 0.05, 0.016, -0.001, "#c6c8cc", sides=12)
    _extruder(d)


HEAD = HeadDef("dragonburner", "DRAGONBURNER", draw_art, TIP, entry=(0.03, 0.1, BODY_TOP + 0.39), size=1.29)
