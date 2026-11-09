# SPDX-License-Identifier: GPL-3.0-or-later
"""Elegoo Canvas: a clear grey shell over a black core, four inlets along its lower front.

No backend reports one, so it is drawn but has no row in kShellRows. The photo does not show
how the lanes leave the unit, so it claims no built-in selector.
"""
from .. import feeder, kit
from ..kit import UnitDef

W, D, H = 0.4, 0.16, 0.2
SHELL, CORE, TRAY = "#a9acb1", "#111216", "#3a3b3e"
CENTERS = [W * f for f in (0.21, 0.4, 0.59, 0.78)]
INLET_Z = 0.06
PROFILE = feeder.rounded_profile(0.0, D, 0.012, H, 0.022)


def _body(sc, detail):
    feeder.box(sc, 0.03, W - 0.03, 0.01, D - 0.01, 0.0, 0.016, TRAY)
    feeder.solid(sc, PROFILE, 0.0, W, SHELL)
    sc.poly(kit.rrect_front(0.05, W - 0.05, 0.12, 0.185, 0.01, -0.001), CORE)
    sc.poly(kit.rrect_front(0.02, W - 0.02, 0.078, 0.112, 0.004, -0.001), "#2b2c30")
    sc.poly(kit.rrect_front(0.02, W - 0.02, 0.02, 0.078, 0.006, -0.001), "#17181b")
    for x in CENTERS:
        sc.line([(x, -0.002, 0.116), (x + 0.002, -0.002, 0.116)], "#5ab0ff", 0.008)
        sc.poly(kit.rrect_front(x - 0.024, x + 0.024, INLET_Z - 0.012, INLET_Z + 0.014, 0.004, -0.002), "#55575c")
        feeder.disc(sc, x, -0.004, INLET_Z, 0.009, "#d7d9dd")
    if detail:
        for i in range(1, 16):
            x = W * i / 16
            sc.line([(x, -0.0015, 0.08), (x, -0.0015, 0.11)], "#3c3d42", 0.002)


def _tubes(scene, colors, width):
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        feeder.tube(scene, feeder.bend((x, -0.004, INLET_Z), (x, -0.05, INLET_Z - 0.04), (x, -0.06, INLET_Z - 0.27)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, D, _tubes)
UNIT = UnitDef("elegoo_canvas", 4, SPOOLS, feeder.tray_strip(0xa9acb1, 0x5ab0ff), stub_art=STUBS)
