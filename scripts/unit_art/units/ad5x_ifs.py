# SPDX-License-Identifier: GPL-3.0-or-later
"""AD5X IFS: a flat taupe box, four inlets on its underside, four outlets on top.

Four separate tubes leave its top for the printer's combiner in the photo, so it has no
built-in selector.
"""
from .. import feeder, kit
from ..kit import UnitDef

W, D, H, Z0 = 0.35, 0.08, 0.22, 0.09
Z1 = Z0 + H
BODY = "#8a7f76"
CENTERS = [0.11, 0.17, 0.23, 0.29]
PORT_Y = D / 2
PROFILE = feeder.rounded_profile(0.0, D, Z0, Z1, 0.018)


def _body(sc, detail):
    for x in CENTERS:
        feeder.box(sc, x - 0.012, x + 0.012, PORT_Y - 0.012, PORT_Y + 0.012, Z1, Z1 + 0.03, "#e7e4dc")
    for x in CENTERS:
        feeder.box(sc, x - 0.012, x + 0.012, PORT_Y - 0.012, PORT_Y + 0.012, Z0 - 0.045, Z0, "#e7e4dc")
    feeder.solid(sc, PROFILE, 0.0, W, BODY)
    if detail:
        light = kit.shift(BODY, 22)
        sc.line([(0.055, -0.001, Z0 + 0.03), (0.055, -0.001, Z0 + 0.075)], light, 0.005)
        sc.line([(0.043, -0.001, Z0 + 0.06), (0.055, -0.001, Z0 + 0.075), (0.067, -0.001, Z0 + 0.06)], light, 0.005)
        for x in CENTERS:
            sc.line([(x, -0.001, Z0 + 0.035), (x, -0.001, Z0 + 0.055)], light, 0.005)


def _tubes(scene, colors, width):
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        feeder.tube(scene, feeder.bend((x, PORT_Y, Z0 - 0.045), (x, PORT_Y, Z0 - 0.17), (x + (lane - 1.5) * 0.04, -0.06, Z0 - 0.26)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, D, _tubes)
UNIT = UnitDef("ad5x_ifs", 4, SPOOLS, feeder.tray_strip(0x8a7f76, 0xe7e4dc), stub_art=STUBS)
