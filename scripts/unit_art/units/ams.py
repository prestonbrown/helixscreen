# SPDX-License-Identifier: GPL-3.0-or-later
"""Bambu Lab AMS: a smoked box with the spools in a cradle on top, under a clear half-cylinder lid."""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

# 368 x 283 x 224 mm closed, from Bambu Lab's dimension drawing.
W, D, H = 1.0, 0.77, 0.61
ZR = 0.34
BODY, END, EDGE, LID = "#36383c", "#55585d", "#7a7d83", "#7d8187"
FEEDER, FEEDER_CAP, FUNNEL = "#b9bbbe", "#2a2b2e", "#eef0f2"
CENTERS = [0.165, 0.39, 0.61, 0.835]
SPOOL_Y, SPOOL_Z, SPOOL_R = D / 2, ZR - 0.02, 0.27
END_W = 0.035
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    """A half ellipse from the front rim, over the spools, to the hinge at the rear rim."""
    cy, a, b = D / 2, D / 2 - 0.01, H - ZR
    pts = []
    for i in range(41):
        t = math.pi * (1 - i / 40)
        pts.append((cy + a * math.cos(t), ZR + b * math.sin(t)))
    return pts


def _feeder(sc, xc, y, detail):
    """One lane's first-stage feeder, seen through the smoked front."""
    half = 0.05
    sc.poly(kit.rrect_front(xc - half, xc + half, 0.04, 0.185, 0.02, y), kit.shade(sc.camera, FEEDER, (0, -1, 0), amb=0.7, dif=0.4, spec=0), 0.85)
    sc.poly(kit.rrect_front(xc - half + 0.004, xc + half - 0.004, 0.185, 0.27, 0.012, y - 0.001), FEEDER_CAP, 0.9)
    if detail:
        sc.poly(kit.rrect_front(xc - half + 0.016, xc + half - 0.016, 0.2, 0.255, 0.006, y - 0.002), "#5d5f63", 0.8)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    lidx0, lidx1 = 0.008, W - 0.008

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.5, blur=True)
    kit.lid_back(sc, prof, lidx0, lidx1, LID, 0.42, 0.34)
    # The cradle the spools sit down in, and the rear wall behind them.
    sc.poly([(0.01, 0.01, ZR - 0.07), (W - 0.01, 0.01, ZR - 0.07), (W - 0.01, D - 0.01, ZR - 0.07), (0.01, D - 0.01, ZR - 0.07)], "#101113")
    sc.poly([(0.01, D - 0.012, ZR - 0.07), (W - 0.01, D - 0.012, ZR - 0.07), (W - 0.01, D - 0.012, ZR), (0.01, D - 0.012, ZR)], "#17181b")
    sc.poly([(0.008, 0.0, ZR - 0.07), (0.008, D, ZR - 0.07), (0.008, D, ZR), (0.008, 0.0, ZR)], "#17181b")
    if detail:
        # Support shafts the spools roll on.
        for y in (SPOOL_Y - 0.19, SPOOL_Y + 0.19):
            sc.line([(0.03, y, ZR - 0.045), (W - 0.03, y, ZR - 0.045)], "#3a3c40", 0.012)

    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    # The base: a smoked front and right side, lighter end strips at the corners.
    sc.poly([(W, 0, 0.012), (W, D, 0.012), (W, D, ZR), (W, 0, ZR)], kit.shade(cam, END, (1, 0, 0), amb=0.8, dif=0.5, spec=0), 0.97)
    sc.poly([(0, 0, 0.012), (W, 0, 0.012), (W, 0, ZR), (0, 0, ZR)], kit.shade(cam, BODY, (0, -1, 0), amb=0.55, dif=0.3, spec=0), 0.8)
    for xc in CENTERS:
        _feeder(sc, xc, -0.002, detail)
    for x0, x1 in ((0.0, END_W), (W - END_W, W)):
        sc.poly(kit.rrect_front(x0, x1, 0.012, ZR, 0.012, -0.003), kit.shade(cam, END, (0, -1, 0), amb=0.7, dif=0.35, spec=0), 0.95)
    sc.poly([(0.01, -0.003, 0.0), (W - 0.01, -0.003, 0.0), (W - 0.01, -0.003, 0.014), (0.01, -0.003, 0.014)], "#1c1d20")

    sc.line([(0, -0.003, ZR), (W, -0.003, ZR), (W, D, ZR)], EDGE, 0.005 * thick)
    sc.line([(W, 0, 0.012), (W, 0, ZR)], EDGE, 0.004 * thick, 0.7)
    sc.poly([(0.0, -0.003, ZR - 0.012), (W, -0.003, ZR - 0.012), (W, -0.003, ZR), (0.0, -0.003, ZR)], "#46484d")

    if detail:
        # Feeder funnels along the front edge of the cradle.
        for xc in CENTERS:
            sc.poly(kit.rrect_front(xc - 0.015, xc + 0.015, ZR - 0.004, ZR + 0.014, 0.006, 0.03), FUNNEL, 0.85)

    kit.lid_front(sc, prof, lidx0, lidx1, LID, 0.42 if detail else 0.36, 0.2 if detail else 0.14)
    sc.line([(lidx1, y, z) for y, z in prof], "#c3c6cb", 0.0035 * thick, 0.65)
    sc.line([(lidx0, y, z) for y, z in prof[:21]], "#c3c6cb", 0.0025 * thick, 0.4)
    sc.line([(lidx0, 0.004, ZR + 0.002), (lidx1, 0.004, ZR + 0.002)], "#c3c6cb", 0.003 * thick, 0.5)
    # The front button and the two grip pads at the front corners.
    button = kit.circle_yz(0, 0, 1, 20)
    sc.poly([(0.5 + 0.03 * yy, 0.02 + 0.025 * zz, ZR + 0.006) for yy, zz in button], "#d5d7da")
    for x0 in (0.02, W - 0.075):
        sc.poly([(x0, 0.008, ZR + 0.004), (x0 + 0.055, 0.008, ZR + 0.004), (x0 + 0.055, 0.04, ZR + 0.004), (x0, 0.04, ZR + 0.004)], "#1a1b1e")


def _tray_shell():
    dome = kit.tray_profile(_lid_profile(), ZR, TRAY_PX_PER_UNIT)
    return TrayShellDef([(0.0, 0.0), (1.0, 0.0)], dome, 0.0, 40.0, 10.0, 7.0, True, True, 0x36383c, 0x1e1f22, 0x7a7d83, 0xb9bbbe)


UNIT = UnitDef("ams", 4, draw_art, _tray_shell())

UNIT.tray.front = [
    kit.span(2, 2, 2, UNIT.tray.front_wall_px - 2, 0x2a2b2f, 2),
    kit.lane(-6, 6, 12, UNIT.tray.front_wall_px - 5, 0xb9bbbe, 3),
    kit.lane(-6, 6, 6, 12, 0x2a2b2e, 2),
    kit.left(0, 7, 2, UNIT.tray.front_wall_px - 2, 0x55585d, 2),
    kit.right(-7, 0, 2, UNIT.tray.front_wall_px - 2, 0x55585d, 2),
    kit.center(-6, 6, -2, 2, 0xd5d7da, 2),
]

UNIT.tray.combiner = "internal"
