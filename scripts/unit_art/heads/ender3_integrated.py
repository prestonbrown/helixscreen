# SPDX-License-Identifier: GPL-3.0-or-later
"""Ender 3 integrated: a plain black rounded box with its filament tube entering at the top."""
from ..head import Drawing, HeadDef, rounded

BLACK, TUBE = "#4a4b50", "#ececea"
BODY = rounded([(-0.26, 0.06), (0.26, 0.06), (0.3, 0.14), (0.3, 0.86), (-0.3, 0.86), (-0.3, 0.14)], 0.07)
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.tube(0.0, 0.16, 0.84, 1.0, 0.025, TUBE)
    d.nozzle(TIP[0], TIP[1], 0.07, block="#8e8f92")
    d.solid(BODY, 0.0, 0.32, BLACK, bevel=0.03)
    # The small status light low on the face.
    d.face([(-0.05, 0.12), (0.05, 0.12), (0.05, 0.145), (-0.05, 0.145)], -0.002, "#7a4bd0", amb=1.1, dif=0.1, spec=0.0)
    if d.detail:
        d.face(rounded([(-0.2, 0.2), (0.2, 0.2), (0.2, 0.78), (-0.2, 0.78)], 0.04), -0.001, "#515257")


HEAD = HeadDef("ender3_integrated", "ENDER3_INTEGRATED", draw_art, TIP)
