# SPDX-License-Identifier: GPL-3.0-or-later
"""Ender 3 bowden: a black hex-grille shroud with the bare hot end beside it on the left."""
from ..head import Drawing, HeadDef, circle, rounded

BLACK, GRILLE, SILVER = "#46474c", "#121315", "#b7b9bd"
SHROUD = rounded([(-0.08, 0.2), (0.5, 0.2), (0.56, 0.3), (0.56, 0.74), (0.46, 0.84), (-0.08, 0.84)], 0.09)
TIP = (-0.2, 0.2, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    x, y = TIP[0], TIP[1]
    # Bowden tube, coupler, heat break, heater block.
    d.tube(x, y, 0.9, 1.0, 0.02, "#e8e8e6")
    d.tube(x, y, 0.82, 0.9, 0.05, "#c5a35c")
    d.tube(x, y, 0.44, 0.82, 0.08, SILVER)
    d.nozzle(x, y, 0.1, block="#b78a46")
    d.solid([(x - 0.11, 0.16), (x + 0.11, 0.16), (x + 0.11, 0.44), (x - 0.11, 0.44)], 0.12, 0.3, "#cfd1d4")
    d.solid(SHROUD, 0.0, 0.3, BLACK, bevel=0.03)
    if d.detail:
        for row in range(6):
            for col in range(7):
                cx, cz = 0.0 + col * 0.075 + (0.037 if row % 2 else 0), 0.3 + row * 0.08
                d.face(circle(cx, cz, 0.03, 6), -0.002, GRILLE, amb=0.9, dif=0.2, spec=0.0)
    else:
        d.face(rounded([(0.02, 0.3), (0.46, 0.3), (0.46, 0.74), (0.02, 0.74)], 0.05), -0.002, GRILLE, amb=0.9, dif=0.2, spec=0.0)


HEAD = HeadDef("ender3_bowden", "ENDER3_BOWDEN", draw_art, TIP)
