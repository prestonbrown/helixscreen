# SPDX-License-Identifier: GPL-3.0-or-later
"""Triangle-Lab A4T: a square black fan body, a gold band, and a dark extruder with a gold lever."""
import math

from ..head import Drawing, HeadDef, rounded

BLACK, GOLD, DARK, HOLE = "#2d2e32", "#b8a23a", "#35363a", "#101113"
HALF_WIDTH = 0.272
DEPTH = 0.34
BODY_TOP = 0.57
TIP = (0.0, 0.05, 0.004)


def _hexagon(cx, cz, r):
    return [(cx + r * math.cos(math.pi * i / 3), cz + r * math.sin(math.pi * i / 3)) for i in range(6)]


def _side_fan(d, x, cy, cz, r):
    n = 24
    circle = lambda radius: [(x, cy + radius * math.cos(2 * math.pi * i / n), cz + radius * math.sin(2 * math.pi * i / n)) for i in range(n)]
    d.facet(circle(r), HOLE, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        for i in range(14):
            t = 2 * math.pi * i / 14
            d.facet([(x, cy + r * k * math.cos(t + s), cz + r * k * math.sin(t + s)) for k, s in ((0.5, 0.0), (0.92, 0.2), (0.92, 0.36), (0.5, 0.16))], "#34353a", amb=0.9, dif=0.3, spec=0.0)
    d.facet(circle(r * 0.42), "#26272b", amb=0.9, dif=0.35, spec=0.05)


def _honeycomb(d):
    """The gold hexes printed across the fan face."""
    for cx, cz in ((0.215, 0.33), (0.2, 0.2), (0.105, 0.16), (-0.165, 0.17)):
        d.face(_hexagon(cx, cz, 0.046), -0.002, GOLD)
    for cx, cz, r in ((0.185, 0.45, 0.055), (-0.215, 0.34, 0.05), (-0.15, 0.27, 0.05)):
        ring = _hexagon(cx, cz, r)
        for a, b in zip(ring, ring[1:] + ring[:1]):
            d.scene.line([(a[0], -0.002, a[1]), (b[0], -0.002, b[1])], GOLD, 0.006)


def _extruder(d):
    # The motor behind and the dark arm that carries the idler.
    n, cx, cz, r, y0, y1 = 24, 0.03, 0.8, 0.17, 0.2, 0.33
    ring = [(cx + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])], "#2b2c30")
    d.facet([(x, y0, z) for x, z in ring], "#232427")
    d.solid(rounded([(0.0, 0.69), (0.195, 0.69), (0.23, 0.8), (0.17, 0.92), (0.0, 0.92)], 0.03), 0.05, 0.18, BLACK)
    # The main housing, a faceted dome on the left.
    d.solid(rounded([(-0.155, 0.7), (0.03, 0.7), (0.05, 0.965), (-0.03, 1.0), (-0.095, 0.965), (-0.155, 0.87)], 0.02), 0.0, 0.2, DARK, bevel=0.02)
    # The gold idler lever, leaning right across the face.
    lever = [(0.05, 0.72), (0.12, 0.74), (0.135, 0.82), (0.21, 0.965), (0.17, 1.0), (0.11, 0.97), (0.03, 0.85), (0.02, 0.77)]
    d.solid(lever, -0.03, 0.04, GOLD, bevel=0.012)
    d.disc(-0.075, 0.875, 0.03, -0.002, HOLE, sides=16)
    if d.detail:
        d.disc(0.17, 0.955, 0.016, -0.031, HOLE, sides=10)
        d.disc(-0.07, 0.775, 0.024, -0.002, "#55575c", sides=14)
        for x, z in ((0.0, 0.675), (0.2, 0.76)):
            d.disc(x, z, 0.02, -0.002, "#c6c8cc", sides=12)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    body = [(0.0, 0.11), (0.07, 0.11), (0.1, 0.01), (0.232, 0.01), (HALF_WIDTH, 0.09), (HALF_WIDTH, 0.51), (0.232, BODY_TOP),
            (0.16, BODY_TOP), (0.1, 0.53), (-0.1, 0.53), (-0.16, BODY_TOP), (-0.232, BODY_TOP), (-HALF_WIDTH, 0.51), (-HALF_WIDTH, 0.09),
            (-0.232, 0.01), (-0.1, 0.01), (-0.07, 0.11)]
    d.solid(rounded(body, 0.012), 0.0, DEPTH, BLACK, bevel=0.018)
    # In the notch, drawn on top so the duct never cuts it off: copper block, flange and tip.
    d.face([(-0.06, 0.05), (0.06, 0.05), (0.06, 0.11), (-0.06, 0.11)], -0.001, "#b76a3f")
    d.face([(-0.04, 0.03), (0.04, 0.03), (0.04, 0.05), (-0.04, 0.05)], -0.001, "#9a5a35")
    d.face([(-0.022, 0.03), (0.022, 0.03), (0.006, TIP[2]), (-0.006, TIP[2])], -0.001, "#ffffff", lane=0)
    _side_fan(d, HALF_WIDTH, DEPTH / 2, 0.31, 0.115)
    d.fan(0.0, 0.35, 0.112, -0.002, blade="#232427")
    _honeycomb(d)
    # The cube mark and the four corner holes.
    # The cube mark: a white pointy-topped hexagon with its three faces drawn in black.
    cz, r = 0.176, 0.033
    corner = lambda deg: (r * math.cos(math.radians(deg)), cz + r * math.sin(math.radians(deg)))
    d.face([corner(30 + 60 * i) for i in range(6)], -0.003, "#f2f1e6")
    for deg in (270, 150, 30):
        x, z = corner(deg)
        d.scene.line([(0.0, -0.004, cz), (x, -0.004, z)], HOLE, 0.005)
    if d.detail:
        for x, z in ((-0.15, 0.1), (0.15, 0.1)):
            d.disc(x, z, 0.022, -0.002, "#c6c8cc", sides=12)
        for x in (-0.15, 0.15):
            d.disc(x, 0.51, 0.016, -0.002, HOLE, sides=10)
    # The gold band, its centre dipping into the top of the body.
    d.solid(rounded([(-0.23, 0.56), (0.23, 0.56), (0.23, 0.625), (-0.23, 0.625)], 0.008), 0.02, DEPTH - 0.05, GOLD, bevel=0.012)
    d.face([(-0.15, 0.625), (0.15, 0.625), (0.085, 0.53), (-0.085, 0.53)], 0.006, GOLD)
    d.solid(rounded([(-0.23, 0.625), (0.23, 0.625), (0.23, 0.7), (-0.23, 0.7)], 0.012), 0.015, DEPTH - 0.08, BLACK, bevel=0.01)
    _extruder(d)


HEAD = HeadDef("a4t", "A4T", draw_art, TIP, entry=(-0.03, 0.17, 1.0), size=1.06)
