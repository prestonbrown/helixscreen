# SPDX-License-Identifier: GPL-3.0-or-later
"""Armored Turtle Box Turtle: an open extrusion base with spools resting on top, no lid."""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D = 1.0, 0.62
TOP = 0.19
# Centred in the frame's depth, with its diameter about 40% of the frame's width.
SPOOL_Y = 0.31
POST = 0.07
PANEL_Z0, PANEL_Z1 = 0.04, 0.15
FRAME, PANEL, CORNER, HUB = "#8c6f2e", "#1f5a3e", "#1f6a4a", "#b7a06e"
RAIL_LINE = "#d9bc6a"
ROLLER = "#101113"
STALK = "#e9e9e4"
STALK_TIP = "#b49a6a"
HOLE = "#0e2c1f"
CENTERS = [0.15, 0.39, 0.63, 0.865]
SPOOL_R = 0.20
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _front_rect(x0, x1, z0, z1, y=-0.001):
    return [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)]


def _side_rect(y0, y1, z0, z1, x=W + 0.001):
    return [(x, y0, z0), (x, y1, z0), (x, y1, z1), (x, y0, z1)]


def _honeycomb(sc, x0, x1, z0, z1, to_plane, cell=0.03):
    row, z = 0, z0 + cell
    while z < z1 - cell / 2:
        x = x0 + cell + (cell if row % 2 else 0)
        while x < x1 - cell / 2:
            pts = [(x + cell * 0.72 * math.cos(math.pi / 6 + k * math.pi / 3), z + cell * 0.72 * math.sin(math.pi / 6 + k * math.pi / 3)) for k in range(6)]
            sc.poly([to_plane(a, b) for a, b in pts], HOLE, tag="honeycomb")
            x += 2 * cell
        z += 1.75 * cell
        row += 1


def _corner(sc, cx, cy):
    """A corner block, flush with the frame's top rail."""
    z = TOP + 0.006
    post = [(cx, cy), (cx, cy + POST), (cx + POST, cy + POST), (cx + POST, cy)][::-1]
    sc.prism(post, 0.0, z, CORNER, amb=0.75, dif=0.55, spec=0.2, power=16)
    sc.poly([(cx, cy, z), (cx + POST, cy, z), (cx + POST, cy + POST, z), (cx, cy + POST, z)], kit.shift(CORNER, 30))


def _posts(sc, corners, z0, z1):
    for cx, cy in corners:
        post = [(cx, cy), (cx, cy + 0.05), (cx + 0.05, cy + 0.05), (cx + 0.05, cy)][::-1]
        sc.prism(post, z0, z1, FRAME, amb=0.9, dif=0.5, spec=0.2, power=16)


def draw_art(sc, colors, tier, rear_posts_to=None):
    detail = tier == "full"
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    foot = [(0, 0), (0, D), (W, D), (W, 0)][::-1]
    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    sc.poly([(0.02, 0.02, TOP), (W - 0.02, 0.02, TOP), (W - 0.02, D - 0.02, TOP), (0.02, D - 0.02, TOP)], "#1d5238")
    # The rear corners stand behind the spools.
    for cx in (0.0, W - POST):
        _corner(sc, cx, D - POST)
    if rear_posts_to:
        _posts(sc, ((0.0, D - 0.05), (W - 0.05, D - 0.05)), TOP + 0.02, rear_posts_to)
    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, TOP + 0.13), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    sc.prism(foot, 0.018, TOP, FRAME, amb=0.8, dif=0.5, spec=0.15, power=20)
    front = kit.shade(cam, PANEL, (0, -1, 0), amb=0.72, dif=0.42, spec=0)
    side = kit.shade(cam, PANEL, (1, 0, 0), amb=0.72, dif=0.42, spec=0)
    sc.poly(_front_rect(0.05, 0.95, PANEL_Z0, PANEL_Z1), front)
    sc.poly(_side_rect(0.05, D - 0.05, PANEL_Z0, PANEL_Z1), side)
    if detail:
        _honeycomb(sc, 0.05, 0.95, PANEL_Z0, PANEL_Z1, lambda a, b: (a, -0.002, b))
        _honeycomb(sc, 0.05, D - 0.05, PANEL_Z0, PANEL_Z1, lambda a, b: (W + 0.002, a, b))
    for z in (TOP - 0.004, PANEL_Z1 + 0.012, PANEL_Z0 - 0.012):
        sc.line([(0.06, -0.003, z), (0.94, -0.003, z)], RAIL_LINE, 0.003)
    for xc in CENTERS:
        roller = [(xc - 0.09, 0.05), (xc - 0.09, 0.1), (xc + 0.09, 0.1), (xc + 0.09, 0.05)][::-1]
        sc.prism(roller, TOP, TOP + 0.03, ROLLER, amb=0.9, dif=0.5, spec=0.3, power=14)
        hub = [(xc - 0.035, 0.035), (xc - 0.035, 0.115), (xc + 0.035, 0.115), (xc + 0.035, 0.035)]
        sc.prism(hub[::-1], TOP, TOP + 0.055, HUB, amb=0.75, dif=0.5, spec=0.1)
        sc.poly([(a, b, TOP + 0.055) for a, b in hub], kit.shift(HUB, 25))
        if detail:
            sc.line([(xc, 0.075, TOP + 0.055), (xc, 0.075, TOP + 0.17)], STALK, 0.008)
            sc.line([(xc, 0.075, TOP + 0.165), (xc, 0.075, TOP + 0.19)], STALK_TIP, 0.013)
    for cx, cy in ((0.0, 0.0), (W - POST, 0.0)):
        _corner(sc, cx, cy)


ENCLOSURE_TOP = 0.61
GLASS = "#8d8579"


def draw_enclosed_art(sc, colors, tier):
    """The open unit inside the optional extrusion-and-panel enclosure."""
    draw_art(sc, colors, tier, rear_posts_to=ENCLOSURE_TOP)
    thick = 1 if tier == "full" else 3
    sc.layer = "front"
    z0, z1 = TOP + 0.02, ENCLOSURE_TOP
    # Rear panels go in first; the near glass then lies over the spools.
    sc.poly([(W, 0.0, z0), (W, D, z0), (W, D, z1), (W, 0.0, z1)], GLASS, 0.22)
    sc.poly([(0.0, 0.0, z1), (W, 0.0, z1), (W, D, z1), (0.0, D, z1)], GLASS, 0.3)
    sc.poly([(0.0, 0.0, z0), (W, 0.0, z0), (W, 0.0, z1), (0.0, 0.0, z1)], GLASS, 0.14)
    _posts(sc, ((0.0, 0.0), (W - 0.05, 0.0)), z0, z1)
    for a, b in (((0.0, 0.0), (W, 0.0)), ((W, 0.0), (W, D)), ((W, D), (0.0, D)), ((0.0, D), (0.0, 0.0))):
        sc.line([(a[0], a[1], z1), (b[0], b[1], z1)], "#3a3c41", 0.006 * thick)


def _shell(**over):
    base = dict(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=[],
        back_extra_px=0.0,
        front_wall_px=TOP * TRAY_PX_PER_UNIT,
        rail_px=8.0,
        cap_clearance_px=9.0,
        lid_spans_row=False,
        has_latch=False,
        body=int(FRAME[1:], 16),
        panel=int(PANEL[1:], 16),
        rail=0xb99a4a,
        accent=int(HUB[1:], 16),
        corner=int(CORNER[1:], 16),
    )
    base.update(over)
    return TrayShellDef(**base)


UNIT = UnitDef("box_turtle", 4, draw_art, _shell())
_ENCLOSED_RISE = (ENCLOSURE_TOP - TOP) * TRAY_PX_PER_UNIT
ENCLOSED = UnitDef(
    "box_turtle_enclosed",
    4,
    draw_enclosed_art,
    _shell(dome=[(0.0, 0.0), (0.0, _ENCLOSED_RISE), (1.0, _ENCLOSED_RISE), (1.0, 0.0)], lid_spans_row=True),
)
