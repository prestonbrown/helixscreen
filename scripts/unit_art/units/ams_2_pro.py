# SPDX-License-Identifier: GPL-3.0-or-later
"""Bambu Lab AMS 2 Pro: a dark see-through base under a very round smoked lid."""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D, H = 1.0, 0.76, 0.665
ZR = 0.35
PLINTH = 0.03
BODY, EDGE, SMOKE = "#2b2c30", "#6d7076", "#6f7379"
RIB = "#c7cacf"
CAN, CAP, GEAR = "#8b8e93", "#26272b", "#d9831f"
CENTERS = [0.15, 0.39, 0.63, 0.865]
SPOOL_Y, SPOOL_Z, SPOOL_R = D / 2, 0.335, 0.27
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    """A half superellipse from the front rim, over the spools, to the rear rim."""
    cy, a, b, n = D / 2, D / 2 - 0.012, H - ZR, 2.3
    pts = []
    for i in range(41):
        t = math.pi * (1 - i / 40)
        c, s = math.cos(t), math.sin(t)
        pts.append((cy + a * math.copysign(abs(c) ** (2 / n), c), ZR + b * abs(s) ** (2 / n)))
    return pts


def _feed(sc, xc, y, detail):
    """One lane's feed mechanism behind the see-through front."""
    half = 0.052
    sc.poly(kit.rrect_front(xc - half, xc + half, PLINTH + 0.03, 0.2, 0.018, y), kit.shade(sc.camera, CAN, (0, -1, 0), amb=0.72, dif=0.42, spec=0))
    sc.poly(kit.rrect_front(xc - half - 0.006, xc + half + 0.006, 0.19, 0.315, 0.016, y - 0.001), CAP, 0.92)
    if detail:
        sc.poly(kit.rrect_front(xc - half + 0.008, xc + half - 0.008, 0.215, 0.262, 0.006, y - 0.002), GEAR)
        for dx in (-0.024, 0.0, 0.024):
            sc.line([(xc + dx, y - 0.003, 0.218), (xc + dx, y - 0.003, 0.26)], "#7a4510", 0.004)
        sc.poly(kit.rrect_front(xc - half + 0.012, xc + half - 0.012, 0.272, 0.302, 0.006, y - 0.002), "#55575c")
    else:
        sc.poly(kit.rrect_front(xc - half + 0.006, xc + half - 0.006, 0.215, 0.28, 0.006, y - 0.002), GEAR)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    lidx0, lidx1 = 0.018, W - 0.018

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    kit.lid_back(sc, prof, lidx0, lidx1, SMOKE, 0.5, 0.42)
    sc.poly([(0.01, 0.01, PLINTH), (W - 0.01, 0.01, PLINTH), (W - 0.01, D - 0.01, PLINTH), (0.01, D - 0.01, PLINTH)], "#0d0e10")
    sc.poly([(0.01, D - 0.012, PLINTH), (W - 0.01, D - 0.012, PLINTH), (W - 0.01, D - 0.012, ZR), (0.01, D - 0.012, ZR)], "#121316")

    sc.poly([(0.006, 0.0, PLINTH), (0.006, D, PLINTH), (0.006, D, ZR), (0.006, 0.0, ZR)], "#121316")

    gaps = [(CENTERS[i] + CENTERS[i + 1]) / 2 for i in range(3)]
    if detail:
        for gx in gaps:
            sc.poly([(gx, y, z) for y, z in prof], RIB, 0.34)

    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    plinth = [(0.03, 0.02), (0.03, D - 0.02), (W - 0.03, D - 0.02), (W - 0.03, 0.02)]
    sc.prism(plinth, 0.0, PLINTH, "#151619", amb=0.8, dif=0.4, spec=0)
    sc.poly([(W, 0, PLINTH), (W, D, PLINTH), (W, D, ZR), (W, 0, ZR)], kit.shade(cam, BODY, (1, 0, 0), amb=0.8, dif=0.5, spec=0), 0.97)
    sc.poly([(0, 0, PLINTH), (W, 0, PLINTH), (W, 0, ZR), (0, 0, ZR)], kit.shade(cam, BODY, (0, -1, 0), amb=0.5, dif=0.28, spec=0), 0.76)
    for xc in CENTERS:
        _feed(sc, xc, -0.002, detail)
    if detail:
        for gx in gaps:
            sc.poly([(gx - 0.012, -0.002, PLINTH + 0.03), (gx + 0.012, -0.002, PLINTH + 0.03), (gx + 0.005, -0.002, 0.3), (gx - 0.005, -0.002, 0.3)], "#08090a")

    sc.line([(0, -0.003, ZR), (W, -0.003, ZR), (W, D, ZR)], EDGE, 0.005 * thick)
    sc.line([(W, 0, PLINTH), (W, 0, ZR)], EDGE, 0.004 * thick, 0.8)
    sc.line([(0, -0.003, PLINTH), (W, -0.003, PLINTH), (W, D, PLINTH)], EDGE, 0.003 * thick, 0.55)
    sc.poly([(0.0, -0.003, ZR - 0.014), (W, -0.003, ZR - 0.014), (W, -0.003, ZR), (0.0, -0.003, ZR)], "#3c3e43")

    kit.lid_front(sc, prof, lidx0, lidx1, SMOKE, 0.5 if detail else 0.42, 0.26 if detail else 0.18)
    band = [p for p in prof if p[0] < D / 2 and p[1] < ZR + 0.14]
    sc.extrude_x(band, lidx0, lidx1, "#25272b", 0.3, front=True)
    sc.line([(lidx1, y, z) for y, z in prof], "#b6bac0", 0.0035 * thick, 0.65)
    sc.line([(lidx0, y, z) for y, z in prof[:21]], "#b6bac0", 0.0025 * thick, 0.4)
    if detail:
        for gx in gaps:
            sc.line([(gx, y, z) for y, z in prof[:22]], RIB, 0.002, 0.32)
    pill = [(0.455, 0.0), (0.455, 0.05), (0.545, 0.05), (0.545, 0.0)]
    sc.prism(pill[::-1], ZR, ZR + 0.016, "#a9acb1", amb=0.82, dif=0.4, spec=0)
    sc.poly([(a, b, ZR + 0.016) for a, b in pill], "#c9ccd1")


def _tray_shell():
    dome = kit.tray_profile(_lid_profile(), ZR, TRAY_PX_PER_UNIT)
    return TrayShellDef([(0.0, 0.0), (1.0, 0.0)], dome, 0.0, 42.0, 10.0, 7.0, True, True, 0x2b2c30, 0x1b1c1f, 0x6d7076, 0xd9831f)


UNIT = UnitDef("ams_2_pro", 4, draw_art, _tray_shell())

UNIT.tray.front = [
    kit.span(2, 2, 2, UNIT.tray.front_wall_px - 2, 0x17181b, 2),
    kit.lane(-5.5, 5.5, 6, 15, 0xe0862a, 1),
    kit.lane(-7, 7, 15, UNIT.tray.front_wall_px - 5, 0x8a8d92, 2),
    kit.center(-8, 8, -2, 2, 0xb9bcc2, 2),
]
