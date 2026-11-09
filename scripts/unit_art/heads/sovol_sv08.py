# SPDX-License-Identifier: GPL-3.0-or-later
"""Sovol SV08: a faceted blue shell with a raised honeycomb vent over an exposed blower, a round vent in its flank, and a black rear slotted for the rail."""
import math

from ..head import Drawing, HeadDef, wordmark

BLUE, RECESS, DARK = "#7099c6", "#5f88b5", "#101113"
EDGE_DEPTH, DEPTH, BACK = 0.1, 0.38, 0.6
WIDEN = 1.18
RAIL_BOTTOM, RAIL_TOP = 0.4, 0.68
REAR = "#1c1d20"
TIP = (0.0, 0.1, 0.02)
FAN_Z, VENT_Z = 0.36, 0.76
# Right half, bottom centre to top centre: a point on the flat front plate and where it slopes back to.
PLATE_TO_EDGE = [
    ((0.0, 0.1), (0.0, 0.1)),
    ((0.09, 0.1), (0.09, 0.1)),
    ((0.12, 0.05), (0.12, 0.05)),
    ((0.2, 0.05), (0.235, 0.03)),
    ((0.215, 0.14), (0.285, 0.12)),
    ((0.19, 0.3), (0.275, 0.3)),
    ((0.19, 0.6), (0.275, 0.62)),
    ((0.215, 0.8), (0.285, 0.84)),
    ((0.15, 0.97), (0.21, 1.0)),
    ((0.0, 0.97), (0.0, 1.0)),
]


def _mirrored():
    right = [((p[0] * WIDEN, p[1]), (e[0] * WIDEN, e[1])) for p, e in PLATE_TO_EDGE]
    return right + [((-p[0], p[1]), (-e[0], e[1])) for p, e in reversed(right) if p[0] != 0]


def _shell(d):
    ring = _mirrored()
    outline = [e for _, e in ring]
    d.solid(outline, EDGE_DEPTH, DEPTH, BLUE)
    for (p0, e0), (p1, e1) in zip(ring, ring[1:] + ring[:1]):
        if p0 != e0 or p1 != e1:
            d.facet([(p0[0], 0.0, p0[1]), (p1[0], 0.0, p1[1]), (e1[0], EDGE_DEPTH, e1[1]), (e0[0], EDGE_DEPTH, e0[1])], BLUE)
    d.face([p for p, _ in ring], 0.0, BLUE)


def _edge_x(z):
    """The shell's silhouette half-width at height z."""
    points = [(e[1], e[0] * WIDEN) for _, e in PLATE_TO_EDGE[3:-1]] + [(1.0, PLATE_TO_EDGE[-2][1][0] * WIDEN)]
    for (z0, x0), (z1, x1) in zip(points, points[1:]):
        if z <= z1 and z1 > z0:
            return x0 + (x1 - x0) * (z - z0) / (z1 - z0)
    return points[-1][1]


def _rear_block(d, z0, z1):
    """A black block behind the shell, following the shell's outline between two heights."""
    breaks = [e[1] for _, e in PLATE_TO_EDGE[3:-1] if z0 < e[1] < z1]
    zs = [z0] + breaks + [z1]
    right = [(_edge_x(z), z) for z in zs]
    d.solid([(-right[0][0], right[0][1])] + right + [(-x, z) for x, z in reversed(right[1:])], DEPTH - 0.01, BACK, REAR)


def _hex(cx, cz, r):
    return [(cx + r * math.sin(math.pi * i / 3), cz + r * math.cos(math.pi * i / 3)) for i in range(6)]


def _vent(d):
    """A raised pentagon carrying the hexagonal honeycomb, its lower edge a chevron facing down."""
    y = -0.03
    d.facet([(-0.15, y, 0.62), (0.15, y, 0.62), (0.1, 0.0, 0.53), (-0.1, 0.0, 0.53)], RECESS)
    d.face([(-0.15, 0.62), (0.15, 0.62), (0.16, 0.86), (0.1, 0.93), (-0.1, 0.93), (-0.16, 0.86)], y, BLUE)
    d.face(_hex(0.0, VENT_Z, 0.125), y - 0.001, RECESS)
    d.face(_hex(0.0, VENT_Z, 0.1), y - 0.002, "#3a587a" if d.detail else DARK)
    if d.detail:
        for row in range(-3, 4):
            count = 7 - abs(row)
            for col in range(count):
                x = (col - (count - 1) / 2) * 0.029
                d.disc(x, VENT_Z + row * 0.025, 0.0095, y - 0.003, DARK, sides=8, amb=0.9, dif=0.2, spec=0.0)


def _flank(d):
    x = 0.275 * WIDEN + 0.001
    cy, cz, r, n = 0.24, 0.48, 0.075, 24
    d.facet([(x, cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)], DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        d.scene.line([(x + 0.001, cy - r, cz), (x + 0.001, cy + r, cz)], BLUE, 0.009)
        d.scene.line([(x + 0.001, cy, cz - r), (x + 0.001, cy, cz + r)], BLUE, 0.009)
        ring = [(x + 0.001, cy + r * 0.55 * math.cos(2 * math.pi * i / n), cz + r * 0.55 * math.sin(2 * math.pi * i / n)) for i in range(n + 1)]
        d.scene.line(ring, BLUE, 0.009)
    d.facet([(x, 0.115, 0.26), (x, 0.145, 0.26), (x, 0.145, 0.68), (x, 0.115, 0.68)], RECESS)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid([(-0.18, RAIL_BOTTOM), (0.18, RAIL_BOTTOM), (0.18, RAIL_TOP), (-0.18, RAIL_TOP)], DEPTH - 0.01, BACK - 0.14, "#0d0e10")
    _rear_block(d, 0.03, RAIL_BOTTOM)
    _rear_block(d, RAIL_TOP, 1.0)
    d.nozzle(TIP[0], TIP[1], 0.08, r_top=0.024, r_tip=0.008, z_tip=TIP[2])
    _shell(d)
    _vent(d)
    d.disc(0.0, FAN_Z, 0.165, -0.002, RECESS)
    d.fan(0.0, FAN_Z, 0.145, -0.003, blade="#34353a", hub="#4b4c50")
    _flank(d)
    if d.detail:
        wordmark(d, "SOVOL", 0.0, 0.135, 0.04, 0.055, 0.014, "#3a587a", 0.007)


HEAD = HeadDef("sovol_sv08", "SOVOL_SV08", draw_art, TIP)
