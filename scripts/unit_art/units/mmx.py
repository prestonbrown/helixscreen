# SPDX-License-Identifier: GPL-3.0-or-later
"""MMX: a black box with four coloured inlet collets across its narrow front face.

The photo shows the filaments running through the body to separate outlets at the rear, so it
has no built-in selector. Its collets keep the photo's colours.
"""
from .. import feeder, kit
from ..kit import UnitDef

W, D, H = 0.3, 0.42, 0.3
BODY, EDGE = "#1f2022", "#333438"
COLLET = ["#e6c62d", "#d6342d", "#2b53c2", "#2f9b45"]
CENTERS = [0.065, 0.122, 0.179, 0.236]
INLET_Z = 0.232
PROFILE = [(0.0, 0.0), (0.0, H - 0.03), (0.03, H), (D - 0.05, H), (D, H - 0.03), (D, 0.0)]


def _lever_profile():
    """A cam lever's side: a low nose at the front, a raised grip, a tail towards the hinge."""
    pts = [(0.06, H), (0.06, H + 0.012), (0.1, H + 0.038), (0.19, H + 0.038), (0.24, H + 0.016), (0.24, H)]
    return kit.fillet(pts, [0, 0.01, 0.022, 0.03, 0.012, 0], closed=False, steps=5)


def _lever(sc, x, detail):
    feeder.solid(sc, _lever_profile(), x - 0.022, x + 0.022, "#2c2d31")
    if detail:
        top = H + 0.0385
        slot = [(x - 0.01, 0.112), (x + 0.01, 0.112), (x + 0.01, 0.18), (x - 0.01, 0.18)]
        sc.poly([(a, b, top) for a, b in slot], "#0c0c0e")


def _tower(sc, x0):
    pts = [(0.24, H), (0.24, H + 0.085), (0.34, H + 0.085), (0.34, H)]
    feeder.solid(sc, kit.fillet(pts, [0, 0.045, 0.045, 0], closed=True, steps=6), x0, x0 + 0.04, EDGE)


def _end_plate(sc):
    """The plate standing proud of the body's left side, its corners rounded."""
    pts = [(0.03, 0.0), (0.03, H + 0.055), (D + 0.03, H + 0.055), (D + 0.03, 0.0)]
    feeder.solid(sc, kit.fillet(pts, [0, 0.05, 0.05, 0], closed=True, steps=6), -0.03, -0.006, "#26272a")


def _body(sc, detail):
    _end_plate(sc)
    feeder.solid(sc, PROFILE, 0.0, W, BODY)
    for x in CENTERS:
        _lever(sc, x, detail)
    _tower(sc, 0.0)
    sc.line([(0.04, 0.29, H + 0.06), (W - 0.04, 0.29, H + 0.06)], "#e6e6e8", 0.034)
    _tower(sc, W - 0.04)
    if detail:
        for z in (0.05, 0.25):
            for x in (0.02, W - 0.02):
                feeder.disc(sc, x, -0.002, z, 0.006, "#62646a")
        sc.poly([(W + 0.001, 0.28, 0.03), (W + 0.001, 0.36, 0.03), (W + 0.001, 0.36, 0.09), (W + 0.001, 0.28, 0.09)], "#101012")
    for x, col in zip(CENTERS, COLLET):
        feeder.disc(sc, x, -0.002, INLET_Z, 0.03, kit.scale(col, 0.7))
        feeder.disc(sc, x, -0.01, INLET_Z, 0.027, col)
        feeder.disc(sc, x, -0.012, INLET_Z, 0.012, "#d9dadc")


def _tubes(scene, colors, width):
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        feeder.tube(scene, feeder.bend((x, -0.012, INLET_Z), (x, -0.16, INLET_Z), (x - 0.04, -0.3, INLET_Z - 0.2)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, D, _tubes)
UNIT = UnitDef("mmx", 4, SPOOLS, feeder.tray_strip(0x1f2022, 0x2b53c2), stub_art=STUBS)
