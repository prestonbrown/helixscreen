# SPDX-License-Identifier: GPL-3.0-or-later
"""AntHead: printed side legs around a grey fan panel, under a compact extruder with a hex knob."""
import math

from ..head import Drawing, HeadDef, rounded

BODY, PANEL, DARK, HOLE = "#a92fbe", "#555a61", "#26272b", "#101113"
DEPTH = 0.34
BODY_TOP = 0.6
TIP = (0.0, 0.05, 0.022)


def _ring(cx, cz, r, n, start=0.0):
    return [(cx + r * math.cos(start + 2 * math.pi * i / n), cz + r * math.sin(start + 2 * math.pi * i / n)) for i in range(n)]


def _cylinder_y(d, cx, cz, r, y0, y1, color, cap, n=20, start=0.0):
    ring = _ring(cx, cz, r, n, start)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])], color)
    d.facet([(x, y0, z) for x, z in ring], cap)


def _cylinder_x(d, cy, cz, r, x0, x1, color, cap, n=14):
    ring = _ring(cy, cz, r, n)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], color)
    d.facet([(x1, y, z) for y, z in ring], cap)


def _side_fan(d, x, cy, cz, r):
    for radius, color in ((r, HOLE), (r * 0.42, DARK)):
        d.facet([(x, y, z) for y, z in _ring(cy, cz, radius, 24)], color, amb=0.9, dif=0.25, spec=0.0)
    if d.detail:
        for i in range(14):
            t = 2 * math.pi * i / 14
            d.facet([(x, cy + r * k * math.cos(t + s), cz + r * k * math.sin(t + s)) for k, s in ((0.5, 0.0), (0.92, 0.2), (0.92, 0.36), (0.5, 0.16))], "#34353a", amb=0.9, dif=0.3, spec=0.0)


def _legs(d):
    right = [(0.145, BODY_TOP - 0.04), (0.205, BODY_TOP - 0.04), (0.222, 0.5), (0.218, 0.1), (0.178, 0.0), (0.118, 0.0), (0.145, 0.1)]
    for s in (1, -1):
        d.solid(rounded([(s * x, z) for x, z in right], 0.012), 0.0, DEPTH, BODY, bevel=0.012)


def _fan_panel(d):
    panel = [(-0.145, BODY_TOP - 0.03), (0.145, BODY_TOP - 0.03), (0.145, 0.125), (0.0, 0.045), (-0.145, 0.125)]
    d.face(panel, 0.002, PANEL)
    octagon = [(-0.07, 0.105), (0.07, 0.105), (0.115, 0.15), (0.115, 0.37), (0.07, 0.415), (-0.07, 0.415), (-0.115, 0.37), (-0.115, 0.15)]
    d.face(octagon, 0.001, BODY)
    if d.detail:
        for i in range(10):
            t = 2 * math.pi * (i + 0.5) / 10
            hexagon = _ring(0.112 * math.cos(t) * 0.92, 0.26 + 0.135 * math.sin(t), 0.024, 6)
            for a, b in zip(hexagon, hexagon[1:] + hexagon[:1]):
                d.scene.line([(a[0], 0.0, a[1]), (b[0], 0.0, b[1])], PANEL, 0.004)
    d.fan(0.0, 0.26, 0.09, -0.001, blade="#232427")
    # The micron mark above the fan.
    mu = "#eeeef0"
    italic = lambda x, z: (x + (z - 0.48) * 0.32, z)
    width, left, right = 0.027, -0.046, 0.019
    d.face([italic(*q) for q in ((left, 0.438), (left + width, 0.438), (left + width, 0.532), (left, 0.532))], -0.002, mu)
    d.face([italic(*q) for q in ((right, 0.466), (right + width, 0.466), (right + width, 0.532), (right, 0.532))], -0.002, mu)
    d.face([italic(*q) for q in ((left + width, 0.466), (right, 0.466), (right, 0.487), (left + width, 0.487))], -0.002, mu)


def _extruder(d):
    _cylinder_y(d, 0.02, 0.8, 0.15, 0.2, 0.34, "#2b2c30", "#232427", n=24)
    body = [(-0.16, 0.6), (0.17, 0.6), (0.125, 0.66), (0.08, 0.66), (0.112, 0.8), (0.162, 0.93), (0.065, 1.0), (-0.02, 0.95), (-0.12, 0.93), (-0.19, 0.82)]
    d.solid(body, 0.0, 0.19, BODY, bevel=0.018)
    # The lever arm up the right, flush with the housing, its black tension screw at the top.
    d.solid([(0.08, 0.66), (0.125, 0.66), (0.19, 0.84), (0.2, 0.93), (0.162, 0.93), (0.112, 0.8)], 0.0, 0.13, PANEL, bevel=0.01)
    # The tension screw runs into the body from the right, its black knob on the end.
    _cylinder_x(d, 0.04, 0.905, 0.012, 0.195, 0.25, "#b9bbc0", "#b9bbc0", n=10)
    _cylinder_x(d, 0.04, 0.905, 0.042, 0.25, 0.292, HOLE, DARK, n=14)
    # The hex knob on the left.
    _cylinder_y(d, -0.074, 0.76, 0.085, -0.05, 0.01, PANEL, "#6a6f77", n=6, start=math.pi / 6)
    d.disc(-0.074, 0.76, 0.03, -0.052, "#3c3e43", sides=6)
    if d.detail:
        for x, z in ((0.03, 0.85), (0.04, 0.72), (0.1, 0.63), (-0.15, 0.625)):
            d.disc(x, z, 0.016, -0.001, DARK, sides=12)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    _legs(d)
    _fan_panel(d)
    _side_fan(d, 0.2205, DEPTH / 2, 0.3, 0.105)
    # In the gap between the legs, drawn on top so the legs never cut it off.
    d.face([(-0.012, 0.05), (0.012, 0.05), (0.004, TIP[2]), (-0.004, TIP[2])], -0.001, "#ffffff", lane=0)
    cap = rounded([(-0.205, BODY_TOP - 0.06), (0.205, BODY_TOP - 0.06), (0.205, BODY_TOP - 0.02), (0.155, BODY_TOP), (-0.155, BODY_TOP), (-0.205, BODY_TOP - 0.02)], 0.012)
    d.solid(cap, 0.0, DEPTH, BODY, bevel=0.01)
    _extruder(d)


HEAD = HeadDef("anthead", "ANTHEAD", draw_art, TIP)
