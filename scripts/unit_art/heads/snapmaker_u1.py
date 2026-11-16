# SPDX-License-Identifier: GPL-3.0-or-later
"""Snapmaker U1: a tall white shell with broadly bevelled corners and a round fan, its top a black plate with an orange lever."""
import math

from ..head import Drawing, HeadDef, lofted_cover, rounded, wordmark

WHITE, TRIM, BLACK, HOLE, ORANGE = "#ececea", "#cfcfcc", "#1d1e21", "#0f1012", "#e0772c"
HALF_WIDTH, DEPTH, BOTTOM, TOP = 0.3, 0.5, 0.05, 1.0
FAN_Z, SEAM_Z = 0.47, 0.14
CORNER_BEVEL = 0.1
TIP = (0.0, 0.2, 0.02)


def _lean(z):
    return 0.0


def _half_width(z):
    """One width top to bottom, easing only at the very top and bottom edges."""
    edge = min(z - BOTTOM, TOP - z)
    return HALF_WIDTH - 0.02 * max(0.0, 1 - edge / 0.05) ** 2


def _top_plate(d):
    z = TOP + 0.001
    plate = rounded([(-0.22, 0.12), (0.22, 0.12), (0.26, 0.18), (0.26, DEPTH - 0.01), (-0.26, DEPTH - 0.01), (-0.26, 0.18)], 0.03)
    d.scene.poly([(x, y, z) for x, y in plate], BLACK)
    d.facet([(-0.2, 0.2, z + 0.001), (-0.08, 0.2, z + 0.001), (-0.08, 0.32, z + 0.001), (-0.2, 0.32, z + 0.001)], HOLE, amb=0.9, dif=0.2, spec=0.0)
    d.solid([(-0.24, TOP + 0.001), (-0.14, TOP + 0.001), (-0.14, TOP + 0.05), (-0.24, TOP + 0.05)], 0.22, 0.28, ORANGE)
    d.tube(0.0, 0.28, TOP, TOP + 0.045, 0.06, "#26272b")


def _fan(d):
    d.disc(0.0, FAN_Z, 0.185, -0.002, TRIM)
    d.disc(0.0, FAN_Z, 0.157, -0.003, HOLE, amb=0.9, dif=0.2, spec=0.0)
    d.disc(0.0, FAN_Z, 0.135, -0.004, ORANGE)
    if d.detail:
        for i in range(20):
            t = 2 * math.pi * i / 20
            d.scene.line([(0.135 * k * math.cos(t), -0.005, FAN_Z + 0.135 * k * math.sin(t)) for k in (0.72, 0.98)], "#a9531c", 0.005)
        d.disc(0.0, FAN_Z, 0.02, -0.005, "#c56624", sides=12)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.nozzle(TIP[0], TIP[1], 0.06, r_top=0.024, r_tip=0.008, z_tip=TIP[2])
    lofted_cover(d, _lean, _half_width, CORNER_BEVEL, BOTTOM, TOP, DEPTH, WHITE)
    _top_plate(d)
    _fan(d)
    if d.detail:
        wordmark(d, "snapmaker", 0.0, 0.79, 0.034, 0.05, 0.01, "#70737a")
        d.scene.line([(-0.26, -0.001, SEAM_Z), (0.26, -0.001, SEAM_Z)], TRIM, 0.005)


HEAD = HeadDef("snapmaker_u1", "SNAPMAKER_U1", draw_art, TIP, entry=(0.0, 0.28, TOP + 0.045))
