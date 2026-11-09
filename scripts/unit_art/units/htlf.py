# SPDX-License-Identifier: GPL-3.0-or-later
"""HTLF: a green body under a tan lid, a motor out of each end, four inlets across the front.

Each lane leaves through its own outlet: AFC runs them direct or to a hub outside the unit, so
it has no built-in selector.
"""
from .. import feeder, kit
from ..kit import UnitDef

W, D, MOTOR = 0.46, 0.27, 0.13
# The height of the lettered panel below the row of inlets.
PANEL_TOP = 0.09
H = PANEL_TOP + 0.2
BODY, LID, GOLD = "#3f9a6e", "#cdb27c", "#b9a05a"
INLET_Z = PANEL_TOP + 0.09
CENTERS = [0.12, 0.195, 0.27, 0.345]
SHOULDER = PANEL_TOP + 0.16
PROFILE = [(0.0, 0.0), (0.0, PANEL_TOP + 0.13), (0.04, SHOULDER), (D - 0.05, SHOULDER), (D, PANEL_TOP + 0.12), (D, 0.0)]
LID_PROFILE = [(0.04, PANEL_TOP + 0.14), (0.1, H), (D - 0.02, H), (D - 0.02, PANEL_TOP + 0.13)]
GLOW = "#54e04a"


def _body(sc, detail):
    b = PANEL_TOP
    feeder.box(sc, -MOTOR, 0.0, 0.05, 0.2, b - 0.02, b + 0.12, "#141517")
    feeder.solid(sc, PROFILE, 0.0, W, BODY)
    front = kit.shade(sc.camera, BODY, (0, -1, 0), amb=0.72, dif=0.5, spec=0.1, power=16)
    sc.poly(kit.rrect_front(0.012, W - 0.012, 0.014, b + 0.022, 0.006, -0.002), kit.shift(front, 10))
    sc.line([(0.0, -0.003, b + 0.03), (W, -0.003, b + 0.03)], kit.scale(front, 0.7), 0.003)
    for x0 in (0.0, W - 0.03):
        feeder.box(sc, x0, x0 + 0.03, 0.04, D - 0.02, SHOULDER, H - 0.012, kit.shift(BODY, -14))
    feeder.solid(sc, LID_PROFILE, 0.05, W - 0.1, LID)
    feeder.box(sc, W - 0.1, W - 0.035, 0.06, D - 0.04, b + 0.12, H - 0.01, "#16171a")
    sc.poly(kit.rrect_front(0.028, 0.072, b + 0.045, b + 0.125, 0.004, -0.002), "#16171a")
    if detail:
        for x in (W - 0.094, W - 0.06):
            sc.line([(x, 0.06, H - 0.01), (x, D - 0.04, H - 0.01)], GOLD, 0.008)
        sc.line([(0.04, -0.003, b + 0.05), (0.04, -0.003, b + 0.12)], GOLD, 0.006)
        sc.line([(0.06, -0.003, b + 0.05), (0.06, -0.003, b + 0.12)], GOLD, 0.006)
        for x in CENTERS:
            sc.poly([(x - 0.016, 0.06, b + 0.161), (x + 0.016, 0.06, b + 0.161), (x + 0.012, 0.085, b + 0.186), (x - 0.012, 0.085, b + 0.186)], GLOW)
        for x in (0.0825, 0.1575, 0.2325, 0.3075, 0.3825):
            sc.line([(x, -0.003, b + 0.03), (x, -0.003, b + 0.13)], kit.scale(front, 0.7), 0.002)
    feeder.box(sc, W, W + MOTOR, 0.05, 0.2, b - 0.02, b + 0.12, "#141517")
    for x in CENTERS:
        feeder.disc(sc, x, -0.002, INLET_Z, 0.019, "#1e2f73")
        feeder.disc(sc, x, -0.004, INLET_Z, 0.009, "#c4c6ca")


def _tubes(scene, colors, width):
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        feeder.tube(scene, feeder.bend((x, -0.004, INLET_Z), (x, -0.13, INLET_Z), (x - 0.03, -0.26, INLET_Z - 0.15)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, D, _tubes)
UNIT = UnitDef("htlf", 4, SPOOLS, feeder.tray_strip(0x3f9a6e, 0xcdb27c), stub_art=STUBS)
