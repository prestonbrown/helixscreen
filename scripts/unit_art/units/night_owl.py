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
DRUM, SHEEN = "#2b2d33", "#5c606b"
CENTERS = [0.26, 0.74]
# Each lane is a drum, a pod and a drum; a spool's flanges ride on the two drums.
LANE_HALF, POD_HALF = 0.21, 0.062
FLANGE_K, FLANGE_W = 1.08, 0.01
BASE_TOP = TOP + 0.045
ROLLER_Y, ROLLER_R = 0.25, 0.155
ROLLER_Z = BASE_TOP + ROLLER_R * FLANGE_K + 0.006
SPOOL_Y = 0.535
REAR_Y, REAR_R = 0.82, 0.06
SPOOL_Z = ROLLER_Z + math.sqrt((LOCAL_R + ROLLER_R) ** 2 - (SPOOL_Y - ROLLER_Y) ** 2)
REAR_Z = SPOOL_Z - math.sqrt((LOCAL_R + REAR_R) ** 2 - (REAR_Y - SPOOL_Y) ** 2)
TRAY_PX_PER_UNIT = 46.2 / LOCAL_R
_REPO = Path(__file__).resolve().parents[3]


def _plate_profile():
    """The side plates: low at the front, arching up over the spool, level at the rear."""
    points = [(0.18, TOP), (0.3, TOP + 0.16), (0.6, TOP + 0.24), (D - 0.02, TOP + 0.14), (D - 0.02, TOP)]
    return kit.fillet(points, [0, 0.08, 0.14, 0.05, 0], steps=6)


def _pod_profile():
    """A pod's side: a yoke standing on the base plate, open above the axle for the bearing."""
    reach = ROLLER_R * 0.72
    pts = [(ROLLER_Y - reach, BASE_TOP), (ROLLER_Y - reach, ROLLER_Z + 0.03), (ROLLER_Y + reach, ROLLER_Z + 0.03), (ROLLER_Y + reach, BASE_TOP)]
    return kit.fillet(pts, [0, 0.05, 0.05, 0], closed=True, steps=6)


def _side_plate(sc, x0, x1, plate, cam, near, thick):
    """A black side arc with its thickness showing along the top edge."""
    sc.extrude_x(plate, x0, x1, PLATE, 1.0, front=True)
    sc.poly([(x1, y, z) for y, z in plate], kit.shade(cam, PLATE, (1, 0, 0), amb=1.0, dif=0.5, spec=0))
    sc.line([(x1, y, z) for y, z in plate], SHEEN, 0.004 * thick, 0.8)
    if near:
        inner = [(y, TOP + (z - TOP) * 0.62) for y, z in plate if 0.26 < y < D - 0.08]
        sc.line([(x1 + 0.001, y, z) for y, z in inner], "#30343c", 0.006 * thick)


def _bearing(sc, xc, detail):
    """The steel bearing clamped in the pod's yoke, its outer race showing above the pod."""
    half, r = POD_HALF * 0.62, ROLLER_R * 0.88
    sc.extrude_x(kit.circle_yz(ROLLER_Y, ROLLER_Z, r), xc - half, xc + half, "#b9bdc4", 1.0, front=False)
    sc.poly([(xc + half, y, z) for y, z in kit.circle_yz(ROLLER_Y, ROLLER_Z, r)], "#d7dade")
    sc.poly([(xc + half, y, z) for y, z in kit.circle_yz(ROLLER_Y, ROLLER_Z, r * 0.74)], "#34373d")
    if detail:
        for i in range(9):
            a = 2 * math.pi * i / 9
            sc.poly([(xc + half, y, z) for y, z in kit.circle_yz(ROLLER_Y + r * 0.62 * math.cos(a), ROLLER_Z + r * 0.62 * math.sin(a), r * 0.09, n=8)], "#c6c9cf")


def _drive(sc, xc, detail):
    """One lane's drive: two black drums butted against a pod between them."""
    rim = {"flange_k": FLANGE_K, "flange_w": FLANGE_W, "sheen": SHEEN, "end_face": "#101116", "end_hub": FRAME, "solid": True}
    inner = POD_HALF + FLANGE_W + 0.004
    feeder.box(sc, xc - LANE_HALF - 0.012, xc + LANE_HALF + 0.012, ROLLER_Y - ROLLER_R - 0.02, ROLLER_Y + ROLLER_R + 0.02, TOP, BASE_TOP, FRAME)
    kit.roller(sc, xc - LANE_HALF, xc - inner, ROLLER_Y, ROLLER_Z, ROLLER_R, DRUM, FRAME, **rim)
    _bearing(sc, xc, detail)
    feeder.solid(sc, _pod_profile(), xc - POD_HALF, xc + POD_HALF, FRAME)
    if detail:
        collet = (xc, ROLLER_Y - ROLLER_R * 0.72 - 0.002)
        feeder.disc(sc, collet[0], collet[1], ROLLER_Z - 0.03, 0.026, "#c6c9cf")
        feeder.disc(sc, collet[0], collet[1] - 0.001, ROLLER_Z - 0.03, 0.016, "#6f9fdc")
    kit.roller(sc, xc + inner, xc + LANE_HALF, ROLLER_Y, ROLLER_Z, ROLLER_R, DRUM, FRAME, **rim)


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
    for xc in CENTERS:
        feeder.box(sc, xc - 0.022, xc + 0.022, REAR_Y - 0.03, REAR_Y + 0.03, TOP, REAR_Z, kit.shift(PLATE, 22))
        kit.roller(sc, xc - LANE_HALF, xc + LANE_HALF, REAR_Y, REAR_Z, REAR_R, FRAME, FRAME, flange_k=1.7, solid=True)
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
            tube = feeder.bend((xc, ROLLER_Y - ROLLER_R * 0.72 - 0.004, ROLLER_Z - 0.03), (xc, -0.06, ROLLER_Z - 0.04), (xc, -0.004, TOP - 0.16))
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
        lane_join="junction",
    )


UNIT = UnitDef("night_owl", 2, draw_art, _tray_shell())
