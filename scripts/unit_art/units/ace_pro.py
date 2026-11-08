# SPDX-License-Identifier: GPL-3.0-or-later
"""Anycubic ACE Pro and Pro 2, which share one body."""
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D, H = 1.0, 0.72, 0.707
RAIL0, ZR = 0.293, 0.357
BODY, PANEL, RAIL = "#3a3b3f", "#47494e", "#a9acb1"
SMOKE = "#565d68"
ORANGE = "#ee6a1f"
EDGE = "#c9ced6"
CENTERS = [0.15, 0.39, 0.63, 0.865]
LOWER = ZR + 0.17
CHEEK_KNEE_Y = 0.42
# A 0.25-radius spool here is a 46.2 px flange on the tray's 110 px reference spool.
TRAY_PX_PER_UNIT = 46.2 / 0.25
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    """The lid's real side profile: up the front, a rounded corner, a flat top, a tighter rear corner."""
    return kit.fillet([(0.018, ZR), (0.018, H), (D - 0.03, H), (D - 0.03, LOWER)], [0, 0.15, 0.09, 0], steps=7)


def _rrect_x(x0, x1, z0, z1, r, y):
    pts = kit.fillet([(x0, z0), (x1, z0), (x1, z1), (x0, z1)], [r] * 4, closed=True, steps=6)
    return [(a, y, b) for a, b in pts]


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    foot = kit.fillet([(0, 0), (0, D), (W, D), (W, 0)], [0.05] * 4, closed=True, steps=8)[::-1]
    lower = LOWER
    lidx0, lidx1 = 0.012, W - 0.012
    prof = _lid_profile()
    cap = [(0.018, ZR)] + prof[1:] + [(0.42, ZR)]

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    sc.poly([(lidx0, y, z) for y, z in cap], SMOKE, 0.55)
    sc.extrude_x(prof, lidx0, lidx1, SMOKE, 0.5, front=False)
    sc.poly([(0.02, 0.02, ZR), (W - 0.02, 0.02, ZR), (W - 0.02, D - 0.03, ZR), (0.02, D - 0.03, ZR)], "#101113")
    sc.poly([(0.02, D - 0.04, ZR), (W - 0.02, D - 0.04, ZR), (W - 0.02, D - 0.04, lower), (0.02, D - 0.04, lower)], "#17181b")

    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, 0.40, 0.336), 0.25, col, c, lane, detail=detail)

    sc.layer = "front"
    sc.prism(foot, 0.0, 0.016, "#141518")
    sc.prism(foot, 0.016, ZR, BODY, amb=0.62, dif=0.5, spec=0.12, power=20)
    wedge = [(0.42, ZR), (D - 0.03, lower), (D - 0.012, lower), (D - 0.012, ZR)]
    sc.poly([(W - 0.004, y, z) for y, z in wedge], kit.shade(cam, BODY, (1, 0, 0), amb=0.62, dif=0.5, spec=0))
    y = -0.002
    if detail:
        sc.poly(_rrect_x(0.03, 0.97, 0.055, 0.275, 0.03, -0.001), kit.shade(cam, PANEL, (0, -1, 0), amb=0.6, dif=0.5, spec=0), 1.0, stroke="#17181b", sw=0.004)
    sc.poly([(0.04, y, RAIL0), (0.96, y, RAIL0), (0.96, y, ZR), (0.04, y, ZR)], kit.shade(cam, RAIL, (0, -1, 0), amb=0.72, dif=0.42, spec=0))
    sc.line([(0.04, y, ZR), (0.96, y, ZR)], "#e7e9ec", 0.003 * thick)
    if detail:
        for xc in CENTERS:
            sc.poly([(xc - 0.004, y, 0.318), (xc + 0.004, y, 0.318), (xc + 0.004, y, 0.342), (xc - 0.004, y, 0.342)], "#f5f5f0")
            tab = [(xc - 0.02, 0.012), (xc - 0.02, 0.05), (xc + 0.02, 0.05), (xc + 0.02, 0.012)]
            sc.prism(tab[::-1], ZR, ZR + 0.022, ORANGE, amb=0.75, dif=0.5, spec=0)
            sc.poly([(a, b, ZR + 0.022) for a, b in tab], "#ff8a44")
        for i in range(5):
            x = 0.03 + i * 0.235
            sc.line([(x, 0.2, H - 0.012), (x, 0.2, H - 0.13), (x, 0.42, H - 0.17), (x, 0.56, H - 0.012)], "#ffffff", 0.0016, 0.16)
        sc.poly(_rrect_x(0.43, 0.57, 0.297, 0.351, 0.025, -0.004), "#d3d5d9", 1.0, stroke="#6d7076", sw=0.002)
        sc.poly(_rrect_x(0.495, 0.505, 0.308, 0.340, 0.004, -0.005), "#70737a")

    sc.extrude_x(prof, lidx0, lidx1, SMOKE, 0.34 if detail else 0.2, front=True)
    sc.poly([(lidx1, y1, z) for y1, z in cap], SMOKE, 0.42 if detail else 0.3)
    sc.line([(lidx1, y1, z) for y1, z in prof], EDGE, 0.004 * thick, 0.7)
    sc.line([(lidx0, y1, z) for y1, z in prof[: len(prof) // 2]], EDGE, 0.003 * thick, 0.45)
    sc.line([(lidx0, D - 0.12, H), (lidx1, D - 0.12, H)], EDGE, 0.003 * thick, 0.4)
    for lx, op in ((lidx0, 0.5), (lidx1, 0.7)):
        sc.line([(lx, 0.018, ZR), (lx, 0.018, H - 0.15)], EDGE, 0.003 * thick, op)


def _tray_shell():
    profile = _lid_profile()
    dome = kit.tray_profile(profile, ZR, TRAY_PX_PER_UNIT)
    knee = (CHEEK_KNEE_Y - profile[0][0]) / (profile[-1][0] - profile[0][0])
    back_extra = dome[-1][1]
    cheek = [(0.0, 0.0), (knee, 0.0), (1.0, back_extra)]
    return TrayShellDef(cheek, dome, back_extra, 44.0, 13.0, 7.0, True, True, 0x3a3b3f, 0x4a4c52, 0xb4b7bc, 0xee6a1f)


UNIT = UnitDef("ace_pro", 4, draw_art, _tray_shell())
