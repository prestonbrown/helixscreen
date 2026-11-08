# SPDX-License-Identifier: GPL-3.0-or-later
"""Creality Filament System: rising side cheeks, a recessed black front and a clear domed lid."""
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D, H = 1.0, 0.76, 0.70
ZR = 0.29
CHEEK_TOP = ZR + 0.19
CHEEK_KNEE_Y = 0.07
BODY, PANEL, TRIM, CLEAR, ROOF = "#55585e", "#0a0b0d", "#c4c7cc", "#b4bac2", "#33363b"
LED = "#7fb6ff"
LEDS = [0.28, 0.42, 0.56, 0.70]
ROOF_Y0, ROOF_Y1 = 0.34, 0.54
CENTERS = [0.15, 0.39, 0.63, 0.865]
SPOOL_Y, SPOOL_Z, SPOOL_R = 0.39, 0.34, 0.26
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    """Up a nearly upright front, over a large round shoulder, to the top of the rear cheek."""
    return kit.fillet([(0.03, ZR), (0.055, H), (D - 0.05, H), (D - 0.035, CHEEK_TOP)], [0, 0.22, 0.17, 0], steps=10)


def _front_panel(sc, detail, thick):
    y = -0.002
    sc.poly(kit.rrect_front(0.045, 0.955, 0.042, 0.236, 0.05, y), PANEL, 1.0, stroke=TRIM, sw=0.004 * thick)
    if detail:
        # The recess's upper lip throws a soft band across the black glass.
        sc.poly(kit.rrect_front(0.06, 0.94, 0.19, 0.228, 0.02, y - 0.001), "#23252a")
        sc.poly(kit.rrect_front(0.415, 0.585, 0.09, 0.172, 0.008, y - 0.001), "#030405", 1.0, stroke="#3d4047", sw=0.002)
        sc.poly(kit.rrect_front(0.43, 0.442, 0.137, 0.155, 0.005, y - 0.002), "#47c76d")
        sc.poly(kit.rrect_front(0.43, 0.442, 0.106, 0.124, 0.005, y - 0.002), "#47c76d")
        for z in (0.141, 0.112):
            sc.poly(kit.rrect_front(0.452, 0.512, z, z + 0.018, 0.003, y - 0.002), "#eef1f4")
        sc.line([(0.525, y - 0.002, 0.1), (0.525, y - 0.002, 0.162)], "#55585e", 0.002)
        sc.poly(kit.rrect_front(0.54, 0.566, 0.112, 0.15, 0.004, y - 0.002), "#d8dbe0")
    else:
        sc.poly(kit.rrect_front(0.42, 0.58, 0.09, 0.17, 0.01, y - 0.001), "#6b7079")


def _handle_bar(sc, detail):
    y = -0.004
    sc.poly(kit.rrect_front(0.17, 0.83, 0.238, 0.286, 0.014, y), "#3a3d43", 1.0, stroke="#74777e", sw=0.0025)
    if detail:
        for xc in LEDS:
            sc.poly(kit.rrect_front(xc - 0.007, xc + 0.007, 0.257, 0.271, 0.006, y - 0.001), LED)


def _guide_posts(sc):
    """The dark feed post standing in front of each spool."""
    for xc in CENTERS:
        y = 0.075
        sc.line([(xc, y, ZR), (xc, y, ZR + 0.08)], "#cfd2d6", 0.012)
        sc.line([(xc, y, ZR + 0.075), (xc, y, ZR + 0.115)], "#17181b", 0.03)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    lidx0, lidx1 = 0.028, W - 0.028
    foot = kit.fillet([(0, 0), (0, D), (W, D), (W, 0)], [0.06] * 4, closed=True, steps=8)[::-1]
    cheek = [(CHEEK_KNEE_Y, ZR), (D - 0.02, CHEEK_TOP), (D, CHEEK_TOP - 0.02), (D, ZR)]

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    kit.lid_back(sc, prof, lidx0, lidx1, CLEAR, 0.22, 0.2)
    sc.poly([(0.03, 0.02, ZR), (W - 0.03, 0.02, ZR), (W - 0.03, D - 0.03, ZR), (0.03, D - 0.03, ZR)], "#111215")
    if detail:
        for xc in [(CENTERS[i] + CENTERS[i + 1]) / 2 for i in range(3)]:
            sc.poly([(xc, y, z) for y, z in prof], "#ccd2da", 0.22)
    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    for fx in (0.14, 0.76):
        sc.prism([(fx, 0.04), (fx, 0.12), (fx + 0.1, 0.12), (fx + 0.1, 0.04)], 0.0, 0.02, "#17181b", amb=0.9, dif=0.3, spec=0)
    sc.prism(foot, 0.016, ZR, BODY, amb=0.68, dif=0.46, spec=0.14, power=20)
    sc.poly([(W - 0.003, y, z) for y, z in cheek], kit.shade(cam, BODY, (1, 0, 0), amb=0.68, dif=0.46, spec=0))
    sc.line([(W - 0.003, y, z) for y, z in cheek[:3]], TRIM, 0.003 * thick, 0.7)
    _front_panel(sc, detail, thick)
    sc.line([(0.05, -0.003, 0.018), (0.95, -0.003, 0.018)], TRIM, 0.0035 * thick)
    if detail:
        _guide_posts(sc)
    _handle_bar(sc, detail)
    sc.line([(0.03, -0.003, ZR), (0.97, -0.003, ZR)], TRIM, 0.004 * thick)

    kit.lid_front(sc, prof, lidx0, lidx1, CLEAR, 0.26 if detail else 0.2, 0.2 if detail else 0.14)
    # The lid's opaque roof strip runs side to side along the crown.
    sc.poly([(lidx0, ROOF_Y0, H), (lidx1, ROOF_Y0, H), (lidx1, ROOF_Y1, H), (lidx0, ROOF_Y1, H)], ROOF, 0.9)
    sc.line([(lidx1, y, z) for y, z in prof], "#8f959d", 0.004 * thick, 0.85)
    sc.line([(lidx0, y, z) for y, z in prof[: len(prof) // 2]], "#8f959d", 0.003 * thick, 0.6)
    sc.poly(kit.rrect_front(-0.014, 0.006, ZR - 0.05, ZR - 0.012, 0.006, 0.04), "#8d9096")
    sc.poly([(W + 0.002, 0.08, ZR - 0.05), (W + 0.002, 0.15, ZR - 0.05), (W + 0.002, 0.15, ZR - 0.012), (W + 0.002, 0.08, ZR - 0.012)], "#8d9096")
    sc.poly([(W + 0.002, 0.3, ZR + 0.01), (W + 0.002, 0.35, ZR + 0.02), (W + 0.002, 0.35, ZR + 0.062), (W + 0.002, 0.3, ZR + 0.05)], "#7f8389")


def _tray_shell():
    profile = _lid_profile()
    dome = kit.tray_profile(profile, ZR, TRAY_PX_PER_UNIT)
    back_extra = dome[-1][1]
    knee = (CHEEK_KNEE_Y - profile[0][0]) / (profile[-1][0] - profile[0][0])
    return TrayShellDef([(0.0, 0.0), (knee, 0.0), (1.0, back_extra)], dome, back_extra, 38.0, 11.0, 7.0, True, False, 0x55585e, 0x0a0b0d, 0xc4c7cc, 0x7fb6ff, 8)


UNIT = UnitDef("cfs", 4, draw_art, _tray_shell())
