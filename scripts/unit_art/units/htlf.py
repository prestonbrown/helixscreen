# SPDX-License-Identifier: GPL-3.0-or-later
"""HTLF: a green body under a tan lid, a motor out of each end, four inlets across the front.

Each lane leaves through its own outlet: AFC runs them direct or to a hub outside the unit, so
it has no built-in selector.
"""
from .. import feeder, kit
from ..kit import UnitDef

W, D, H, MOTOR = 0.46, 0.27, 0.2, 0.13
BODY, LID, GOLD = "#3f9a6e", "#cdb27c", "#b9a05a"
INLET_Z = 0.09
CENTERS = [0.12, 0.195, 0.27, 0.345]
PROFILE = [(0.0, 0.0), (0.0, 0.13), (0.04, 0.16), (D - 0.05, 0.16), (D, 0.12), (D, 0.0)]
LID_PROFILE = [(0.04, 0.14), (0.1, H), (D - 0.02, H), (D - 0.02, 0.13)]
GLOW = "#54e04a"


def _body(sc, detail):
    feeder.box(sc, -MOTOR, 0.0, 0.05, 0.2, 0.02, 0.16, "#141517")
    feeder.solid(sc, PROFILE, 0.0, W, BODY)
    for x0 in (0.0, W - 0.03):
        feeder.box(sc, x0, x0 + 0.03, 0.04, D - 0.02, 0.16, H - 0.012, kit.shift(BODY, -14))
    feeder.solid(sc, LID_PROFILE, 0.05, W - 0.1, LID)
    feeder.box(sc, W - 0.1, W - 0.035, 0.06, D - 0.04, 0.12, H - 0.01, "#16171a")
    if detail:
        for x in (W - 0.094, W - 0.06):
            sc.line([(x, 0.06, H - 0.01), (x, D - 0.04, H - 0.01)], GOLD, 0.008)
        for x in CENTERS:
            sc.poly([(x - 0.016, 0.06, 0.161), (x + 0.016, 0.06, 0.161), (x + 0.012, 0.085, 0.186), (x - 0.012, 0.085, 0.186)], GLOW)
    feeder.box(sc, W, W + MOTOR, 0.05, 0.2, 0.02, 0.16, "#141517")
    for x in CENTERS:
        feeder.disc(sc, x, -0.002, INLET_Z, 0.019, "#1e2f73")
        feeder.disc(sc, x, -0.004, INLET_Z, 0.009, "#c4c6ca")


def _tubes(scene, colors, width):
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        feeder.tube(scene, feeder.bend((x, -0.004, INLET_Z), (x, -0.13, INLET_Z), (x - 0.03, -0.26, INLET_Z - 0.15)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, D, _tubes)
UNIT = UnitDef("htlf", 4, SPOOLS, feeder.tray_strip(0x3f9a6e, 0xcdb27c), stub_art=STUBS)
