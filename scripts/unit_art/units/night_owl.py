# SPDX-License-Identifier: GPL-3.0-or-later
"""Fysetc Night Owl: two spools riding on rollers above an orange and black base."""
import math
from pathlib import Path

from .. import feeder, kit, spool
from ..kit import TrayShellDef, UnitDef

# Lengths are in units of the base width; SCALE turns them into the shared scale where a
# 200 mm spool has radius 0.25.
SPOOL_R = 0.25
LOCAL_R = 0.46
SCALE = SPOOL_R / LOCAL_R
W, D = 1.0, 0.92
FOOT_Z, TOP = 0.045, 0.36
FRAME, PANEL, DECK, PLATE, RUBBER = "#1d2f55", "#14171f", "#a9afba", "#17191e", "#2e2f33"
DRUM, SHEEN = "#1b1c20", "#50545e"
CENTERS = [0.26, 0.74]
SPOOL_Y = 0.52
ROLLER_Y, ROLLER_R = 0.2, 0.115
ROLLER_Z = TOP + 0.08
SPOOL_Z = ROLLER_Z + math.sqrt((LOCAL_R + ROLLER_R) ** 2 - (SPOOL_Y - ROLLER_Y) ** 2)
REAR_Y = 0.82
TRAY_PX_PER_UNIT = 46.2 / LOCAL_R
_REPO = Path(__file__).resolve().parents[3]


def _plate_profile():
    """The side plates: low at the front, arching up over the spool, level at the rear."""
    points = [(0.18, TOP), (0.3, TOP + 0.16), (0.6, TOP + 0.24), (D - 0.02, TOP + 0.14), (D - 0.02, TOP)]
    return kit.fillet(points, [0, 0.08, 0.14, 0.05, 0], steps=6)


def _pod_profile():
    """A motor pod's side: straight up from the deck, round over the roller axle."""
    r = ROLLER_R * 1.3
    arc = [(ROLLER_Y - r * math.cos(math.pi * i / 16), ROLLER_Z + r * math.sin(math.pi * i / 16)) for i in range(17)]
    return [(ROLLER_Y - r, TOP)] + arc + [(ROLLER_Y + r, TOP)]


def _side_plate(sc, x0, x1, plate, cam, near, thick):
    """A black side arc with its thickness showing along the top edge."""
    sc.extrude_x(plate, x0, x1, PLATE, 1.0, front=True)
    sc.poly([(x1, y, z) for y, z in plate], kit.shade(cam, PLATE, (1, 0, 0), amb=1.0, dif=0.5, spec=0))
    sc.line([(x1, y, z) for y, z in plate], SHEEN, 0.004 * thick, 0.8)
    if near:
        inner = [(y, TOP + (z - TOP) * 0.62) for y, z in plate if 0.26 < y < D - 0.08]
        sc.line([(x1 + 0.001, y, z) for y, z in inner], "#30343c", 0.006 * thick)


def _drive(sc, xc, detail):
    """One lane's drive: a black roller either side of a rounded motor pod."""
    half = 0.036
    rim = {"flange_k": 1.1, "flange_w": 0.012, "sheen": SHEEN}
    kit.roller(sc, xc - 0.21, xc - half - 0.012, ROLLER_Y, ROLLER_Z, ROLLER_R, DRUM, FRAME, **rim)
    feeder.solid(sc, _pod_profile(), xc - half, xc + half, FRAME)
    face = xc + half + 0.001
    axle = [(face, y, z) for y, z in kit.circle_yz(ROLLER_Y, ROLLER_Z, ROLLER_R * 0.62)]
    sc.poly(axle, "#c6c9cf")
    sc.poly([(face, y, z) for y, z in kit.circle_yz(ROLLER_Y, ROLLER_Z, ROLLER_R * 0.34)], "#3a3d44")
    if detail:
        sc.line(axle, "#7f838c", 0.004)
    # The gap and the missing left flange leave the pod's bearing in view.
    kit.roller(sc, xc + half + 0.04, xc + 0.21, ROLLER_Y, ROLLER_Z, ROLLER_R, DRUM, FRAME, left_flange=False, **rim)


def draw_art(scene, colors, tier):
    sc = kit.ScaledScene(scene, SCALE)
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    plate = _plate_profile()
    foot = kit.fillet([(0, 0), (0, D), (W, D), (W, 0)], [0.05] * 4, closed=True, steps=6)[::-1]
    sc.poly([(0.02, 0.02, 0), (W + 0.04, 0.02, 0), (W + 0.04, D + 0.03, 0), (0.02, D + 0.03, 0)], "#000", 0.5, blur=True)
    sc.poly([(0.0, 0.0, TOP), (W, 0.0, TOP), (W, D, TOP), (0.0, D, TOP)], DECK)
    _side_plate(sc, 0.0, 0.03, plate, cam, False, thick)
    kit.roller(sc, 0.1, 0.9, REAR_Y, TOP + 0.09, 0.05, FRAME, FRAME)
    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(scene, (xc * SCALE, SPOOL_Y * SCALE, SPOOL_Z * SCALE), SPOOL_R, col, c, lane, detail=detail)

    scene.layer = "front"
    for cx, cy in ((0.06, 0.06), (W - 0.06, 0.06)):
        pad = [(cx - 0.05, cy - 0.05), (cx - 0.05, cy + 0.05), (cx + 0.05, cy + 0.05), (cx + 0.05, cy - 0.05)][::-1]
        sc.prism(pad, 0.0, FOOT_Z, "#101112", amb=0.9, dif=0.4, spec=0)
    sc.prism(foot, FOOT_Z, TOP, FRAME, amb=0.74, dif=0.5, spec=0.15, power=18)
    sc.poly(kit.rrect_front(0.09, 0.91, 0.1, TOP - 0.05, 0.02, -0.002), kit.shade(cam, PANEL, (0, -1, 0), amb=0.8, dif=0.4, spec=0))
    sc.poly([(W + 0.002, 0.12, 0.1), (W + 0.002, D - 0.12, 0.1), (W + 0.002, D - 0.12, TOP - 0.05), (W + 0.002, 0.12, TOP - 0.05)], kit.shade(cam, PANEL, (1, 0, 0), amb=0.8, dif=0.4, spec=0))
    sc.line([(0.0, 0.0, TOP), (W, 0.0, TOP), (W, D, TOP)], kit.shift(DECK, 16), 0.008 * thick)
    for xc in CENTERS:
        _drive(sc, xc, detail)
        if detail:
            tube = feeder.bend((xc, ROLLER_Y - ROLLER_R * 1.3, ROLLER_Z + 0.03), (xc, -0.05, ROLLER_Z), (xc, -0.004, TOP - 0.16))
            sc.line(tube, "#f2f3f4", 0.011)
    _side_plate(sc, W - 0.03, W, plate, cam, True, thick)


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
