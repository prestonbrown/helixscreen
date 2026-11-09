# SPDX-License-Identifier: GPL-3.0-or-later
"""QIDI Box: a pale grey body whose sides slant up to the rear, under a boxy smoked lid."""
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D = 1.0, 0.68
ZR = 0.30
LID_FRONT, LID_REAR = 0.575, 0.60
SIDE_TOP = 0.56
BODY, SEAM, SMOKE = "#d6d8dc", "#9ea1a7", "#3b3f46"
FLOOR, PARTITION = "#17181b", "#1f2024"
LATCH = "#eceef0"
CENTERS = [0.15, 0.39, 0.63, 0.865]
SPOOL_Y, SPOOL_Z, SPOOL_R = 0.37, 0.325, 0.235
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    """A vertical front pane and a nearly flat roof, with barely softened corners."""
    return kit.fillet([(0.02, ZR), (0.02, LID_FRONT), (D - 0.035, LID_REAR), (D - 0.035, SIDE_TOP - 0.01)], [0, 0.012, 0.012, 0], steps=4)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    lidx0, lidx1 = 0.012, W - 0.012
    side = [(0.0, ZR), (D, SIDE_TOP), (D, 0.0), (0.0, 0.0)]

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.5, blur=True)
    kit.lid_back(sc, prof, lidx0, lidx1, SMOKE, 0.34, 0.36)
    sc.poly([(0.01, D - 0.03, ZR), (W - 0.01, D - 0.03, ZR), (W - 0.01, D - 0.03, SIDE_TOP), (0.01, D - 0.03, SIDE_TOP)], "#101114")
    sc.poly([(0.01, 0.01, ZR), (W - 0.01, 0.01, ZR), (W - 0.01, D - 0.03, ZR), (0.01, D - 0.03, ZR)], FLOOR)
    for gx in [(CENTERS[i] + CENTERS[i + 1]) / 2 for i in range(3)]:
        sc.poly([(gx, 0.08, ZR), (gx, D - 0.05, ZR), (gx, D - 0.05, LID_REAR - 0.02), (gx, 0.08, LID_FRONT - 0.02)], PARTITION)
    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    for fx in (0.05, 0.87):
        sc.prism([(fx, 0.03), (fx, 0.1), (fx + 0.08, 0.1), (fx + 0.08, 0.03)], 0.0, 0.012, "#2a2b2f", amb=0.9, dif=0.3, spec=0)
    z0 = 0.012
    sc.poly([(0, 0, z0), (W, 0, z0), (W, 0, ZR), (0, 0, ZR)], kit.shade(cam, BODY, (0, -1, 0), amb=0.74, dif=0.32, spec=0))
    sc.poly([(W, y, max(z, z0)) for y, z in side], kit.shade(cam, BODY, (1, 0, 0), amb=0.68, dif=0.34, spec=0))
    sc.poly([(0.01, 0.0, ZR), (W - 0.01, 0.0, ZR), (W - 0.01, 0.07, ZR), (0.01, 0.07, ZR)], FLOOR)
    sc.line([(W, 0, ZR), (W, D, SIDE_TOP)], "#f2f3f5", 0.003 * thick, 0.9)
    sc.line([(0, -0.002, ZR), (W, -0.002, ZR)], "#f2f3f5", 0.003 * thick)
    sc.line([(W, 0, z0), (W, 0, ZR)], "#f4f5f7", 0.003 * thick, 0.9)
    if detail:
        sc.line([(0, -0.002, ZR - 0.042), (W, -0.002, ZR - 0.042)], SEAM, 0.002)
    sc.poly(kit.rrect_front(0.435, 0.565, ZR - 0.038, ZR + 0.012, 0.008, -0.004), LATCH, 1.0, stroke=SEAM, sw=0.002)
    if detail:
        sc.line([(0.5, -0.005, ZR - 0.03), (0.5, -0.005, ZR + 0.004)], SEAM, 0.002)

    kit.lid_front(sc, prof, lidx0, lidx1, SMOKE, 0.34 if detail else 0.28, 0.16 if detail else 0.12)
    roof = [p for p in prof if p[1] >= LID_FRONT - 0.002]
    sc.extrude_x(roof, lidx0, lidx1, SMOKE, 0.5, front=True)
    edge = "#8f949c"
    sc.line([(lidx1, y, z) for y, z in prof], edge, 0.003 * thick, 0.85)
    sc.line([(lidx0, y, z) for y, z in prof[:7]], edge, 0.0025 * thick, 0.6)
    sc.line([(lidx0, 0.02, LID_FRONT), (lidx1, 0.02, LID_FRONT)], edge, 0.003 * thick, 0.75)


def _tray_shell():
    profile = _lid_profile()
    dome = kit.tray_profile(profile, ZR, TRAY_PX_PER_UNIT)
    back_extra = dome[-1][1]
    return TrayShellDef([(0.0, 0.0), (1.0, back_extra)], dome, back_extra, 40.0, 8.0, 6.0, True, True, 0xd6d8dc, 0xc4c7cc, 0xeceef0, 0x3b3f46)


UNIT = UnitDef("qidi_box", 4, draw_art, _tray_shell())

UNIT.tray.front = [
    kit.span(2, 2, 7, 8.5, 0xa9acb2),
    kit.center(-13, 13, 1, 7, 0xeef0f3, 2),
    kit.center(-2, 2, 2.5, 5.5, 0x8d9096),
]

UNIT.tray.built_in_selector = True
