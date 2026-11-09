# SPDX-License-Identifier: GPL-3.0-or-later
"""3MS: four grey drive modules in echelon on teal brackets, a red back plate and a green board beneath.

Every module has its own outlet tube and the merge is a hub outside the row, so it has no
built-in selector. A DIY unit: these are the photo's colours.
"""
import math

from .. import feeder, kit
from ..kit import UnitDef

PITCH, COUNT = 0.185, 4
W = PITCH * COUNT
D = 0.17
# Each module sits this far behind the one on its left, so the row runs in echelon.
STAGGER = 0.05
TEAL, GREY, RED, BOARD = "#35c4c4", "#3e4044", "#c52428", "#3fb24a"
CENTERS = [PITCH * (i + 0.5) for i in range(COUNT)]
OFFSETS = [STAGGER * i for i in range(COUNT)]
INLET_Z = 0.27
INLET_X = 0.034
DEPTH = D + STAGGER * (COUNT - 1)


def _chamfered(x0, x1, y0, y1, cut):
    return [(x1, y0 + cut), (x1, y1 - cut), (x1 - cut, y1), (x0 + cut, y1), (x0, y1 - cut), (x0, y0 + cut), (x0 + cut, y0), (x1 - cut, y0)]


def _head(sc, x, y, detail):
    """The grey drive: chamfered corners, a two-part seam, a label and a push-fit inlet."""
    h, y0, y1, z0, z1 = 0.074, y + 0.01, y + 0.15, 0.2, 0.335
    foot = _chamfered(x - h, x + h, y0, y1, 0.012)
    sc.prism(foot, z0, z1, GREY, amb=0.72, dif=0.5, spec=0.1, power=16)
    sc.poly([(a, b, z1) for a, b in foot], kit.shift(GREY, 16))
    sc.line([(x - h, y0 - 0.001, 0.268), (x + h, y0 - 0.001, 0.268), (x + h, y1 - 0.01, 0.268)], "#26282b", 0.004)
    if detail:
        mid = (y0 + y1) / 2
        label = [(x - 0.052, mid), (x - 0.036, mid - 0.022), (x + 0.036, mid - 0.022), (x + 0.052, mid), (x + 0.036, mid + 0.022), (x - 0.036, mid + 0.022)]
        sc.poly([(a, b, z1 + 0.0005) for a, b in label], "#e8e9ea")
        for bx in (-0.058, 0.058):
            for by in (y0 + 0.016, y1 - 0.016):
                sc.poly([(x + bx + 0.007 * math.cos(t), by + 0.007 * math.sin(t), z1 + 0.0005) for t in (2 * math.pi * i / 12 for i in range(12))], "#7a7c80")
        feeder.disc(sc, x - 0.058, y0 - 0.002, 0.32, 0.006, "#7a7c80")
        feeder.disc(sc, x - 0.058, y0 - 0.002, 0.215, 0.006, "#7a7c80")
    feeder.disc(sc, x + INLET_X, y0 - 0.002, INLET_Z, 0.03, "#c8cacd")
    feeder.disc(sc, x + INLET_X, y0 - 0.008, INLET_Z, 0.021, "#8d9095")
    feeder.disc(sc, x + INLET_X, y0 - 0.012, INLET_Z, 0.012, "#17181a")


def _module(sc, x, y, detail):
    h = 0.082
    if detail:
        feeder.box(sc, x - 0.07, x - 0.03, y + D + 0.005, y + D + 0.02, 0.24, 0.31, RED)
        feeder.box(sc, x + 0.03, x + 0.07, y + D + 0.005, y + D + 0.02, 0.24, 0.31, RED)
    foot = _chamfered(x - h, x + h, y, y + D - 0.012, 0.02)
    sc.prism(foot, 0.07, 0.2, TEAL, amb=0.75, dif=0.5, spec=0.1, power=16)
    sc.poly([(a, b, 0.2) for a, b in foot], kit.shift(TEAL, 16))
    sc.line([(x, y + 0.15, 0.3), (x, y + 0.27, 0.35)], "#ecedee", 0.016)
    _head(sc, x, y, detail)


def _board(sc, detail):
    y0, y1 = STAGGER, D + STAGGER * 2 - 0.01
    feeder.box(sc, W * 0.18, W * 0.82, y0, y1, 0.0, 0.05, BOARD)
    feeder.box(sc, W * 0.2, W * 0.8, y0 + 0.01, y1 - 0.01, 0.05, 0.07, "#1f7a30")
    if detail:
        for i in range(5):
            x = W * 0.24 + i * W * 0.13
            feeder.box(sc, x, x + 0.05, y0 - 0.002, y0 + 0.018, 0.05, 0.078, "#e9e9e4")


def _body(sc, detail):
    feeder.box(sc, 0.02, W - 0.02, 0.17, 0.2 + DEPTH - D, 0.15, 0.29, RED)
    _board(sc, detail)
    for x, y in zip(CENTERS, OFFSETS):
        _module(sc, x, y, detail)


def _tubes(scene, colors, width):
    for lane, (x, y, col) in enumerate(zip(CENTERS, OFFSETS, colors)):
        x_in = x + INLET_X
        y0 = y - 0.004
        feeder.tube(scene, feeder.bend((x_in, y0, INLET_Z), (x_in, y0 - 0.14, INLET_Z), (x_in - 0.03, y0 - 0.28, INLET_Z - 0.18)), col, lane, width)


SPOOLS, STUBS = feeder.looks(_body, W / 2, DEPTH + 0.03, _tubes)
UNIT = UnitDef("3ms", 4, SPOOLS, feeder.tray_strip(0x35c4c4, 0x3e4044), stub_art=STUBS)
