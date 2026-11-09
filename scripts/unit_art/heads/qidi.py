# SPDX-License-Identifier: GPL-3.0-or-later
"""Qidi Q2 and its size variants: a tall white cover with a slatted vent and chamfered nozzle opening, cut through behind for the rail."""
import math

from ..head import Drawing, HeadDef, lofted_cover, rounded, wordmark

WHITE, SEAM, DARK, HOTEND, YELLOW, INK = "#e8e8e6", "#c4c4c1", "#101113", "#1d1e21", "#c9b52c", "#2b2c30"
TOP_HALF, BOTTOM_HALF, DEPTH, BOTTOM, TOP = 0.205, 0.226, 0.34, 0.05, 1.0
BACK, RAIL_BOTTOM, RAIL_TOP = 0.6, 0.45, 0.76
LID_Z, FOOT_Z = 0.858, 0.132
TOP_RADIUS = 0.09
TIP = (0.0, 0.05, 0.02)
OPENING = [(-0.08, BOTTOM), (0.08, BOTTOM), (0.08, 0.21), (0.045, 0.263), (-0.045, 0.263), (-0.08, 0.21)]


def _lean(z):
    """The face is flat, rolling back over a soft radius into the top."""
    t = max(0.0, (z - (TOP - TOP_RADIUS)) / TOP_RADIUS)
    return TOP_RADIUS * (1 - math.sqrt(max(0.0, 1 - t * t)))


def _half_width(z):
    """Flaring slightly toward the foot, eased at the very top and bottom."""
    t = (TOP - z) / (TOP - BOTTOM)
    edge = min(z - BOTTOM, TOP - z)
    return TOP_HALF + (BOTTOM_HALF - TOP_HALF) * t - 0.02 * max(0.0, 1 - edge / 0.05) ** 2


def _slats(d):
    count, half = (9, 0.0085) if d.detail else (5, 0.016)
    for i in range(count):
        x = -0.127 + 0.254 * i / (count - 1)
        d.face([(x - half, 0.3), (x + half, 0.3), (x + half, 0.487), (x - half, 0.487)], -0.002, DARK, amb=0.9, dif=0.2, spec=0.0)


def _logo(d):
    """A cube mark, its outline broken at the right around a heavy S-shaped inner stroke, then the name."""
    cx, cz, r = -0.105, 0.77, 0.036

    def vertex(i):
        return (cx + r * math.sin(math.pi * i / 3), cz + r * math.cos(math.pi * i / 3))

    v1, v2, v3 = vertex(1), vertex(2), vertex(3)
    arc_end = (v3[0] + (v2[0] - v3[0]) * 0.2, v3[1] + (v2[1] - v3[1]) * 0.2)
    d.scene.line([(x, -0.002, z) for x, z in (v3, vertex(4), vertex(5), vertex(0), v1)], INK, 0.0055)
    d.scene.line([(x, -0.002, z) for x, z in (v1, (cx, cz + 0.002), (cx, cz - 0.02), v3)], INK, 0.01)
    d.scene.line([(x, -0.002, z) for x, z in (v2, arc_end)], INK, 0.0055)
    wordmark(d, "QiDi", 0.04, 0.747, 0.036, 0.046, 0.013, INK, 0.0075)


def _seams(d):
    w = _half_width(LID_Z) - 0.04
    d.scene.line([(-w, -0.001, LID_Z), (w, -0.001, LID_Z)], SEAM, 0.005)
    w = _half_width(FOOT_Z) - 0.04
    for x0, x1 in ((-w, -0.08), (0.08, w)):
        d.scene.line([(x0, -0.001, FOOT_Z), (x1, -0.001, FOOT_Z)], SEAM, 0.005)


def _hotend(d):
    d.nozzle(TIP[0], TIP[1], 0.06, r_top=0.024, r_tip=0.008, z_tip=TIP[2])
    d.face([(-0.05, 0.05), (0.058, 0.05), (0.058, 0.215), (-0.05, 0.215)], -0.003, HOTEND)
    if d.detail:
        for x in (-0.018, 0.004, 0.026):
            d.scene.line([(x + 0.006 * math.sin(i * 1.3), -0.004, 0.115 + i * 0.012) for i in range(6)], YELLOW, 0.004)
        d.scene.line([(-0.024, -0.004, 0.095), (0.032, -0.004, 0.095)], YELLOW, 0.004)


def _section(d, z0, z1, color):
    """A rear block flush with the cover's sides between two heights."""
    zs = [z0 + (z1 - z0) * i / 16 for i in range(17)]
    right = [(_half_width(z), z) for z in zs]
    d.solid([(-right[0][0], right[0][1])] + right + [(-x, z) for x, z in reversed(right[1:])], DEPTH - 0.01, BACK, color)


def _top_opening(d):
    """The opening in the top the filament tube rises through."""
    z = TOP + 0.001
    d.scene.poly([(x, y, z) for x, y in rounded([(-0.085, 0.2), (0.085, 0.2), (0.085, 0.34), (-0.085, 0.34)], 0.025)], DARK)
    d.tube(0.0, 0.27, TOP, TOP + 0.17, 0.024, "#cfe2f0")


def _side_fan(d):
    """The small fan on the flank of the lower rear block."""
    n, cy, cz = 24, 0.24, 0.3

    def ring(r, lift):
        return [(_half_width(cz + r * math.sin(2 * math.pi * i / n)) + lift, cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]

    d.facet(ring(0.095, 0.002), DARK, amb=0.9, dif=0.2, spec=0.0)
    d.facet(ring(0.04, 0.003), "#3a3b40", amb=0.9, dif=0.3, spec=0.05)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid([(-0.17, RAIL_BOTTOM), (0.17, RAIL_BOTTOM), (0.17, RAIL_TOP), (-0.17, RAIL_TOP)], DEPTH - 0.01, BACK - 0.14, "#2a2b2f")
    _section(d, BOTTOM, RAIL_BOTTOM, WHITE)
    _section(d, RAIL_TOP, TOP, WHITE)
    lofted_cover(d, _lean, _half_width, 0.06, BOTTOM, TOP, DEPTH, WHITE, stations=90)
    _top_opening(d)
    d.face(OPENING, -0.002, DARK, amb=0.9, dif=0.2, spec=0.0)
    _hotend(d)
    _slats(d)
    _seams(d)
    _side_fan(d)
    if d.detail:
        _logo(d)


HEAD = HeadDef("qidi", "QIDI", draw_art, TIP)
