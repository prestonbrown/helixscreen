# SPDX-License-Identifier: GPL-3.0-or-later
"""HTLF (Happy Turtle Lettuce Feeder): a lane selector that stands in front of the spools.

Spools sit elsewhere and feed in at the back. Inside, one drive motor (the left end) turns a
shaft along the lanes, and a selector motor (the right end) turns a cam drum whose lobes press
one lane at a time against it, so the selector rotates in place rather than sliding along a rail.
Each lane leaves through its own collet on the front, under a tan lid with a lamp per lane.
"""
from .. import feeder, kit
from ..kit import TrayShellDef, UnitDef
from . import htlf_face

W, D, H = 0.46, 0.27, 0.29
# Front face, bottom up: a plain panel, the row of outlet collets, then the lid.
PANEL_TOP = 0.087
LID_Z0 = 0.157
OUTLET_Z = (PANEL_TOP + LID_Z0) / 2
WALL = 0.025
LID_X1 = 0.396
SEGMENTS = [0.132, 0.198, 0.264, 0.33, 0.396, W - WALL]
CENTERS = [0.165, 0.231, 0.297, 0.363]
BODY, LID, GOLD, MOTOR = "#3f9a6e", "#d3b97f", "#b9a05a", "#141517"
LAMP = "#54e04a"
PROFILE = [(0.0, 0.0), (0.0, LID_Z0), (D, LID_Z0), (D, 0.0)]
WALL_PROFILE = [(0.0, 0.0), (0.0, H - 0.03), (0.03, H), (D, H), (D, 0.0)]
LID_PROFILE = [(0.0, LID_Z0), (0.0, H - 0.035), (0.035, H), (D - 0.01, H), (D - 0.01, LID_Z0)]
DIAL_Y, DIAL_Z, DIAL_R = 0.12, H - 0.075, 0.07


def _dial(sc, detail):
    """The cam drum's geared ring, showing in the slot between the lid and the right wall."""
    x0, x1 = LID_X1 + 0.004, W - WALL - 0.004
    kit.roller(sc, x0 + 0.008, x1 - 0.008, DIAL_Y, DIAL_Z, DIAL_R, "#1b1c1f", GOLD, flange_k=1.08, flange_w=0.008, solid=True)
    if detail:
        for z in (DIAL_Z + DIAL_R * 0.35, DIAL_Z + DIAL_R * 0.7):
            sc.line([(x0, DIAL_Y - DIAL_R * 0.72, z), (x1, DIAL_Y - DIAL_R * 0.72, z)], GOLD, 0.003, 0.8)


def _body(sc, detail):
    front = kit.shade(sc.camera, BODY, (0, -1, 0), amb=0.72, dif=0.5, spec=0.1, power=16)
    feeder.box(sc, -0.09, 0.0, 0.06, 0.16, 0.11, 0.21, MOTOR)
    feeder.solid(sc, WALL_PROFILE, 0.0, WALL, BODY)
    feeder.solid(sc, PROFILE, WALL, W - WALL, BODY)
    # The drive gear shows through a slot left of the outlets.
    sc.poly(kit.rrect_front(0.07, 0.115, PANEL_TOP + 0.008, LID_Z0 - 0.004, 0.004, -0.002), "#16171a")
    if detail:
        for x in (0.08, 0.092, 0.104):
            sc.line([(x, -0.003, PANEL_TOP + 0.012), (x, -0.003, LID_Z0 - 0.008)], GOLD, 0.005)
    sc.line([(WALL, -0.002, PANEL_TOP), (W - WALL, -0.002, PANEL_TOP)], kit.scale(front, 0.7), 0.003)
    for x in SEGMENTS:
        sc.line([(x, -0.002, PANEL_TOP), (x, -0.002, LID_Z0)], kit.scale(front, 0.7), 0.0025)
    for x in CENTERS:
        feeder.disc(sc, x, -0.002, OUTLET_Z, 0.017, "#1e2f73")
        feeder.disc(sc, x, -0.004, OUTLET_Z, 0.008, "#c4c6ca")
    feeder.solid(sc, LID_PROFILE, WALL, LID_X1, LID)
    lid_front = kit.shade(sc.camera, LID, (0, -1, 0), amb=0.72, dif=0.5, spec=0.1, power=16)
    sc.line([(WALL, -0.002, LID_Z0 + 0.002), (LID_X1, -0.002, LID_Z0 + 0.002)], kit.scale(lid_front, 0.75), 0.003)
    for x in CENTERS:
        sc.poly(kit.rrect_front(x - 0.012, x + 0.012, 0.2, 0.226, 0.004, -0.002), LAMP)
    _dial(sc, detail)
    feeder.solid(sc, WALL_PROFILE, W - WALL, W, BODY)
    feeder.box(sc, W, W + 0.07, DIAL_Y - 0.05, DIAL_Y + 0.05, DIAL_Z - 0.05, DIAL_Z + 0.05, MOTOR)


def draw_art(scene, colors, tier):
    """The body with a short filament stub leaving each outlet; no spools."""
    _body(scene, tier == "full")
    width = feeder.TUBE_FULL if tier == "full" else feeder.TUBE_THUMB
    for lane, (x, col) in enumerate(zip(CENTERS, colors)):
        feeder.tube(scene, feeder.bend((x, -0.004, OUTLET_Z), (x, -0.08, OUTLET_Z), (x, -0.13, OUTLET_Z - 0.08)), col, lane, width)


def _tray_shell() -> TrayShellDef:
    return feeder.tray_strip(0x3F9A6E, 0xD3B97F)


UNIT = UnitDef("htlf", 4, draw_art, _tray_shell(), face_art=htlf_face.draw)
