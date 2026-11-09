# SPDX-License-Identifier: GPL-3.0-or-later
"""Artillery M1 Pro: a silver shell rolling over a big front radius, a black fan panel below, and a deeper upper rear."""
import math

from ..head import Drawing, HeadDef, lofted_cover, rounded, wordmark

SILVER, BLACK, HOLE, RIB = "#b4b6b9", "#1b1c1f", "#0e0f11", "#2c2d31"
HALF_WIDTH, BOTTOM, TOP = 0.3, 0.05, 1.0
PANEL_TOP, FRONT_RADIUS = 0.56, 0.17
LOWER_DEPTH, UPPER_DEPTH, STEP_Z = 0.55, 0.76, 0.6
FAN_Z = 0.3
TIP = (0.0, 0.1, -0.01)


def _lean(z):
    """Flat up the face, then rolling back over the large top radius."""
    t = max(0.0, (z - (TOP - FRONT_RADIUS)) / FRONT_RADIUS)
    return FRONT_RADIUS * (1 - math.sqrt(max(0.0, 1 - t * t)))


def _half_width(z):
    return HALF_WIDTH


def _flank(d, pts, color=HOLE):
    d.facet([(HALF_WIDTH + 0.001, y, z) for y, z in pts], color, amb=0.9, dif=0.2, spec=0.0)


def _rect(y0, y1, z0, z1):
    return [(y0, z0), (y1, z0), (y1, z1), (y0, z1)]


def _flank_details(d):
    _flank(d, _rect(0.28, 0.32, 0.72, 0.86))
    _flank(d, _rect(0.22, 0.38, 0.5, 0.58))
    if not d.detail:
        _flank(d, _rect(0.17, 0.4, 0.14, 0.44))
        _flank(d, _rect(0.56, 0.7, 0.78, 0.9))
        return
    for col, y in enumerate((0.13, 0.16, 0.19)):
        _flank(d, _rect(y, y + 0.014, 0.76 + 0.012 * (col % 2), 0.86 - 0.012 * (col % 2)))
    for row in range(4):
        z = 0.14 + row * 0.08
        for col in range(8):
            y = 0.17 + col * 0.03 + (0.015 if row % 2 else 0)
            _flank(d, _rect(y, y + 0.014, z, z + 0.065))
    for row in range(3):
        z = 0.78 + row * 0.042
        _flank(d, _rect(0.56, 0.7, z, z + 0.024))
    _flank(d, _rect(0.58, 0.67, 0.64, 0.74), "#3c3d42")


def _top_details(d):
    """The slatted vent toward the back left, the filament hole, and the pill-shaped opening near the right edge."""
    z = TOP + 0.001
    x0, x1, y0, y1 = -0.24, 0.07, 0.6, 0.74
    d.scene.poly([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], "#8e9093" if d.detail else HOLE)
    if d.detail:
        for i in range(8):
            x = x0 + 0.02 + i * (x1 - x0 - 0.04) / 7
            d.scene.line([(x, y0 + 0.012, z + 0.001), (x, y1 - 0.012, z + 0.001)], HOLE, 0.009)
    n = 18
    d.scene.poly([(-0.06 + 0.036 * math.cos(2 * math.pi * i / n), 0.447 + 0.036 * math.sin(2 * math.pi * i / n), z) for i in range(n)], HOLE)
    d.scene.poly([(x, y, z) for x, y in rounded([(0.08, 0.29), (0.15, 0.29), (0.15, 0.44), (0.08, 0.44)], 0.033)], HOLE)


def _fan(d):
    if d.detail:
        for i in range(9):
            x = -0.24 + i * 0.06
            d.scene.line([(x, -0.002, BOTTOM + 0.03), (x, -0.002, PANEL_TOP - 0.03)], RIB, 0.006)
    d.disc(0.0, FAN_Z, 0.19, -0.003, "#26272b")
    d.disc(0.0, FAN_Z, 0.15, -0.004, HOLE, amb=0.9, dif=0.2, spec=0.0)
    d.disc(0.0, FAN_Z, 0.07, -0.005, "#232427")


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid(rounded([(-0.12, 0.06), (0.12, 0.06), (0.12, 0.32), (-0.12, 0.32)], 0.02), LOWER_DEPTH - 0.01, 0.72, BLACK)
    d.nozzle(TIP[0], TIP[1], 0.06, r_top=0.022, r_tip=0.008, z_tip=TIP[2])
    d.solid(_rect_outline(), LOWER_DEPTH - 0.01, UPPER_DEPTH, SILVER)
    lofted_cover(d, _lean, _half_width, 0.05, BOTTOM, PANEL_TOP, 0.07, BLACK, stations=12)
    d.facet([(HALF_WIDTH, 0.07, BOTTOM), (HALF_WIDTH, LOWER_DEPTH, BOTTOM), (HALF_WIDTH, LOWER_DEPTH, PANEL_TOP), (HALF_WIDTH, 0.07, PANEL_TOP)], SILVER)
    lofted_cover(d, _lean, _half_width, 0.05, PANEL_TOP, TOP, LOWER_DEPTH, SILVER, stations=70)
    _fan(d)
    _flank_details(d)
    _top_details(d)
    if d.detail:
        wordmark(d, "ARTILLERY", 0.0, 0.69, 0.03, 0.046, 0.01, "#f1f2f3", 0.006)


def _rect_outline():
    return [(-HALF_WIDTH, STEP_Z), (HALF_WIDTH, STEP_Z), (HALF_WIDTH, TOP), (-HALF_WIDTH, TOP)]


HEAD = HeadDef("artillery_m1", "ARTILLERY_M1", draw_art, TIP, entry=(-0.06, 0.447, TOP))
