# SPDX-License-Identifier: GPL-3.0-or-later
"""Elegoo Canvas: a clear smoked shell around a black frame, four tubes leaving the bottom.

Proportions read off the front photo, 1000 px across the shell. No backend reports one, so it
is drawn but has no row in kShellRows. The photo does not show how the lanes leave the unit
toward the printer, so it claims no built-in selector.
"""
from .. import feeder, kit
from ..kit import UnitDef

W, D = 0.4, 0.16
FLOOR, H = 0.024, 0.234
SMOKE, EDGE, FRAME, BASE = "#b4b8bf", "#e4e7ec", "#0e0f13", "#303136"
# The lanes sit left of centre, as on the real unit.
CENTERS = [W * f for f in (0.148, 0.358, 0.565, 0.775)]
BAND_Z0, BAND_Z1 = 0.088, 0.118
COLLAR_Z = 0.084
SLEEVE_END_Z = 0.038
SHELL = feeder.rounded_profile(0.0, D, FLOOR, H, 0.026)
CORE = feeder.rounded_profile(0.02, D - 0.02, FLOOR + 0.006, H - 0.024, 0.012)
LID = feeder.rounded_profile(0.02, D - 0.03, H - 0.004, H + 0.01, 0.004)
CORE_FRONT = 0.018


def _body(sc, detail):
    feeder.box(sc, 0.02, W - 0.02, 0.012, D - 0.012, 0.0, FLOOR, BASE)
    sc.poly([(0.01, D, FLOOR), (W - 0.01, D, FLOOR), (W - 0.01, D, H - 0.01), (0.01, D, H - 0.01)], SMOKE, 0.14)
    feeder.solid(sc, CORE, 0.03, W - 0.03, FRAME)

    y = CORE_FRONT
    sc.poly(kit.rrect_front(0.05, W - 0.05, 0.128, 0.212, 0.008, y), "#05060a")
    for x in [W * f for f in (0.09, 0.252, 0.462, 0.67, 0.884)]:
        sc.poly(kit.rrect_front(x - 0.03, x + 0.03, FLOOR + 0.012, BAND_Z0 - 0.006, 0.01, y), "#20222a")
    if detail:
        for x, zs in ((0.06, (0.205, 0.195)), (W - 0.06, (0.205, 0.195))):
            sc.line([(x - 0.012, y - 0.001, zs[0]), (x, y - 0.001, zs[0]), (x, y - 0.001, zs[1])], "#d5d8dd", 0.002)

    feeder.glass(sc, SHELL, 0.0, W, SMOKE, 0.2, EDGE)
    feeder.glass(sc, LID, 0.03, W - 0.03, SMOKE, 0.22, EDGE)
    sc.poly(kit.rrect_front(0.01, W - 0.01, BAND_Z0, BAND_Z1, 0.004, -0.001), "#55585f", 0.72)
    for x in CENTERS:
        feeder.disc(sc, x, -0.002, BAND_Z1 + 0.012, 0.004, "#5ab0ff")
        sc.poly(kit.rrect_front(x - 0.022, x + 0.022, COLLAR_Z - 0.006, COLLAR_Z + 0.006, 0.003, -0.003), "#6a6d73")
        sc.line([(x, -0.004, COLLAR_Z), (x, -0.004, SLEEVE_END_Z)], "#d7d9dd", 0.012)
    if detail:
        for i in range(1, 32):
            x = W * i / 32
            sc.line([(x, -0.002, BAND_Z0 + 0.002), (x, -0.002, BAND_Z1 - 0.002)], "#30333a", 0.0016, 0.8)


def _tubes(scene, colors, width):
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        feeder.tube(scene, feeder.bend((x, -0.004, SLEEVE_END_Z), (x, -0.008, -0.06), (x, -0.012, -0.22)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, D, _tubes)
UNIT = UnitDef("elegoo_canvas", 4, SPOOLS, feeder.tray_strip(0x1a1b1f, 0x5ab0ff), stub_art=STUBS)
