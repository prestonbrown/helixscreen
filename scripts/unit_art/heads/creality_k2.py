# SPDX-License-Identifier: GPL-3.0-or-later
"""Creality K2: a tall narrow shell, a block of vertical vents, and an arch over the nozzle."""
from ..head import Drawing, HeadDef, rounded

SHELL, DARK, RED = "#9ea1a5", "#17181a", "#c43a2e"
OUTER = rounded([(-0.22, 0.04), (0.22, 0.04), (0.26, 0.12), (0.26, 0.9), (-0.26, 0.9), (-0.26, 0.12)], 0.07)
ARCH = rounded([(-0.09, 0.02), (0.09, 0.02), (0.09, 0.2), (0.0, 0.27), (-0.09, 0.2)], 0.05)
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid([(-0.16, 0.88), (0.16, 0.88), (0.16, 1.0), (-0.16, 1.0)], 0.1, 0.42, "#1c1d20")
    d.nozzle(TIP[0], TIP[1], 0.06, block="#1c1d20")
    d.solid(OUTER, 0.0, 0.46, SHELL, bevel=0.03)
    d.face(ARCH, -0.002, DARK, amb=0.9, dif=0.2, spec=0.0)
    d.tube(TIP[0], TIP[1], 0.06, 0.17, 0.05, RED)
    for i in range(9 if d.detail else 5):
        n = 9 if d.detail else 5
        x = -0.15 + 0.3 * i / (n - 1)
        hw = 0.009 if d.detail else 0.016
        d.face([(x - hw, 0.36), (x + hw, 0.36), (x + hw, 0.6), (x - hw, 0.6)], -0.002, DARK, amb=0.9, dif=0.2, spec=0.0)
    # Mesh on the right flank.
    if d.detail:
        for r in range(6):
            d.facet([(0.26, 0.12, 0.3 + r * 0.06), (0.26, 0.34, 0.3 + r * 0.06), (0.26, 0.34, 0.33 + r * 0.06), (0.26, 0.12, 0.33 + r * 0.06)], DARK, amb=0.9, dif=0.2, spec=0.0)


HEAD = HeadDef("creality_k2", "CREALITY_K2", draw_art, TIP)
