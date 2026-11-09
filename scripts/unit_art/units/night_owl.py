# SPDX-License-Identifier: GPL-3.0-or-later
"""Fysetc Night Owl: two spools riding on rollers above an orange and black base."""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D = 1.0, 0.92
FOOT_Z, TOP = 0.045, 0.36
FRAME, PANEL, DECK, PLATE, RUBBER = "#e9761c", "#26272a", "#d9dbdf", "#232427", "#2e2f33"
CENTERS = [0.26, 0.74]
SPOOL_Y, SPOOL_R = 0.52, 0.46
ROLLER_Y, ROLLER_R = 0.2, 0.085
ROLLER_Z = TOP + 0.06
SPOOL_Z = ROLLER_Z + math.sqrt((SPOOL_R + ROLLER_R) ** 2 - (SPOOL_Y - ROLLER_Y) ** 2)
REAR_Y = 0.82
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _plate_profile():
    """The side plates: low at the front, arching up over the spool, level at the rear."""
    points = [(0.18, TOP), (0.3, TOP + 0.16), (0.6, TOP + 0.24), (D - 0.02, TOP + 0.14), (D - 0.02, TOP)]
    return kit.fillet(points, [0, 0.08, 0.14, 0.05, 0], steps=6)


def _circle(y, z, r, n=24):
    return [(y + r * math.cos(2 * math.pi * i / n), z + r * math.sin(2 * math.pi * i / n)) for i in range(n + 1)]


def _roller(sc, x0, x1, y, z, r, color, flange):
    ring = _circle(y, z, r)
    sc.extrude_x(ring, x0, x1, color, 1.0, front=True)
    sc.poly([(x1, a, b) for a, b in ring], kit.shift(color, 16))
    edge = _circle(y, z, r * 1.18)
    for xa, xb in ((x0 - 0.016, x0), (x1, x1 + 0.016)):
        sc.extrude_x(edge, xa, xb, flange, 1.0, front=True)
        sc.poly([(xb, a, b) for a, b in edge], kit.shift(flange, 18))


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    plate = _plate_profile()
    foot = kit.fillet([(0, 0), (0, D), (W, D), (W, 0)], [0.05] * 4, closed=True, steps=6)[::-1]
    sc.poly([(0.02, 0.02, 0), (W + 0.04, 0.02, 0), (W + 0.04, D + 0.03, 0), (0.02, D + 0.03, 0)], "#000", 0.5, blur=True)
    sc.poly([(0.0, 0.0, TOP), (W, 0.0, TOP), (W, D, TOP), (0.0, D, TOP)], DECK)
    sc.poly([(0.015, y, z) for y, z in plate], kit.shade(cam, PLATE, (1, 0, 0), amb=0.8, dif=0.4, spec=0))
    _roller(sc, 0.1, 0.9, REAR_Y, TOP + 0.09, 0.05, FRAME, FRAME)
    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    for cx, cy in ((0.06, 0.06), (W - 0.06, 0.06)):
        pad = [(cx - 0.05, cy - 0.05), (cx - 0.05, cy + 0.05), (cx + 0.05, cy + 0.05), (cx + 0.05, cy - 0.05)][::-1]
        sc.prism(pad, 0.0, FOOT_Z, "#101112", amb=0.9, dif=0.4, spec=0)
    sc.prism(foot, FOOT_Z, TOP, FRAME, amb=0.74, dif=0.5, spec=0.15, power=18)
    sc.poly(kit.rrect_front(0.09, 0.91, 0.1, TOP - 0.05, 0.02, -0.002), kit.shade(cam, PANEL, (0, -1, 0), amb=0.8, dif=0.4, spec=0))
    sc.poly([(W + 0.002, 0.12, 0.1), (W + 0.002, D - 0.12, 0.1), (W + 0.002, D - 0.12, TOP - 0.05), (W + 0.002, 0.12, TOP - 0.05)], kit.shade(cam, PANEL, (1, 0, 0), amb=0.8, dif=0.4, spec=0))
    sc.line([(0.0, 0.0, TOP), (W, 0.0, TOP), (W, D, TOP)], kit.shift(DECK, 16), 0.008 * thick)
    for xc in CENTERS:
        _roller(sc, xc - 0.19, xc + 0.19, ROLLER_Y, ROLLER_Z, ROLLER_R, RUBBER, FRAME)
        pod = [(xc - 0.03, ROLLER_Y - 0.13), (xc - 0.03, ROLLER_Y + 0.02), (xc + 0.03, ROLLER_Y + 0.02), (xc + 0.03, ROLLER_Y - 0.13)][::-1]
        sc.prism(pod, TOP, ROLLER_Z + ROLLER_R + 0.02, FRAME, amb=0.74, dif=0.5, spec=0.15, power=18)
        sc.poly([(a, b, ROLLER_Z + ROLLER_R + 0.02) for a, b in pod], kit.shift(FRAME, 24))
        if detail:
            tube = [(xc, 0.06, ROLLER_Z + 0.1), (xc, -0.012, ROLLER_Z), (xc, -0.012, TOP - 0.1), (xc, -0.004, TOP - 0.16)]
            sc.line(tube, "#f2f3f4", 0.011)
    sc.poly([(W - 0.015, y, z) for y, z in plate], kit.shade(cam, PLATE, (1, 0, 0), amb=0.9, dif=0.5, spec=0))
    sc.line([(W - 0.015, y, z) for y, z in plate], "#46484c", 0.003 * thick, 0.8)


_WALL = TOP * TRAY_PX_PER_UNIT
_FRAME = int(FRAME[1:], 16)
_RISE = 18.0


def _tray_shell():
    return TrayShellDef(
        cheek=[(0.0, 0.0), (0.25, 0.0), (0.62, _RISE), (1.0, _RISE)],
        dome=[],
        back_extra_px=_RISE,
        front_wall_px=_WALL,
        rail_px=6.0,
        cap_clearance_px=9.0,
        lid_spans_row=False,
        has_latch=False,
        body=_FRAME,
        panel=int(PANEL[1:], 16),
        rail=_FRAME,
        accent=_FRAME,
        front=[
            kit.span(0, 0, 0, _WALL, _FRAME, 3),
            kit.span(7, 7, 7, _WALL - 5, int(PANEL[1:], 16), 2),
            kit.lane(-17, 17, -8, 0, int(RUBBER[1:], 16), 4),
            kit.lane(-3, 3, -11, 2, _FRAME, 1),
        ],
        built_in_selector=True,
    )


UNIT = UnitDef("night_owl", 2, draw_art, _tray_shell())
