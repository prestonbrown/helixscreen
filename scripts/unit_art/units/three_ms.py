# SPDX-License-Identifier: GPL-3.0-or-later
"""3MS: four grey drive modules on teal brackets, a red back plate and a green board beneath.

Every module has its own outlet tube and the merge is a hub outside the row, so it has no
built-in selector. A DIY unit: these are the photo's colours.
"""
from .. import feeder, kit
from ..kit import UnitDef

PITCH, COUNT = 0.185, 4
W = PITCH * COUNT
D = 0.17
TEAL, GREY, RED, BOARD = "#35c4c4", "#3e4044", "#c52428", "#3fb24a"
CENTERS = [PITCH * (i + 0.5) for i in range(COUNT)]
INLET_Z = 0.27


def _module(sc, x, detail):
    h = 0.082
    cut = 0.02
    foot = [(x + h, cut), (x + h, D - 0.012), (x - h, D - 0.012), (x - h, cut), (x - h + cut, 0.0), (x + h - cut, 0.0)]
    sc.prism(foot, 0.07, 0.2, TEAL, amb=0.75, dif=0.5, spec=0.1, power=16)
    sc.poly([(a, b, 0.2) for a, b in foot], kit.shift(TEAL, 16))
    feeder.box(sc, x - 0.074, x + 0.074, 0.01, 0.15, 0.2, 0.335, GREY)
    sc.line([(x - 0.074, 0.008, 0.268), (x + 0.074, 0.008, 0.268)], "#26282b", 0.004)
    if detail:
        sc.poly([(x - 0.04, 0.05, 0.3351), (x + 0.04, 0.05, 0.3351), (x + 0.04, 0.09, 0.3351), (x - 0.04, 0.09, 0.3351)], "#e8e9ea")
        for bx, bz in ((-0.058, 0.215), (0.058, 0.215), (-0.058, 0.318), (0.058, 0.318)):
            feeder.disc(sc, x + bx, 0.008, bz, 0.006, "#7a7c80")
    feeder.disc(sc, x + 0.03, 0.007, INLET_Z, 0.028, "#c8cacd")
    feeder.disc(sc, x + 0.03, 0.002, INLET_Z, 0.016, "#17181a")
    sc.line([(x, 0.15, 0.3), (x, 0.27, 0.35)], "#ecedee", 0.016)


def _body(sc, detail):
    feeder.box(sc, 0.02, W - 0.02, 0.17, 0.2, 0.15, 0.29, RED)
    feeder.box(sc, W * 0.18, W * 0.82, 0.015, D - 0.01, 0.0, 0.07, BOARD)
    for x in CENTERS:
        _module(sc, x, detail)


def _tubes(scene, colors, width):
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        x_in = x + 0.03
        feeder.tube(scene, feeder.bend((x_in, 0.0, INLET_Z), (x_in, -0.14, INLET_Z), (x_in - 0.03, -0.28, INLET_Z - 0.18)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, D + 0.03, _tubes)
UNIT = UnitDef("3ms", 4, SPOOLS, feeder.tray_strip(0x35c4c4, 0x3e4044), stub_art=STUBS)
