# SPDX-License-Identifier: GPL-3.0-or-later
"""DW-TAS EMU: a row of separate sealed spool housings, one per lane, on a shared black base.

Each housing is a "D" from the side: a flat front, a half-round top and back. Its red walls
frame a clear window that runs up the front and over the top; under the window a red band
carries the rotary latch, over a short black lower body. The base is wider than the row,
its front face leaning back as it rises, with a status light under each housing.
"""
import math
from pathlib import Path

from .. import feeder, kit, spool
from ..kit import TrayShellDef, UnitDef

# 1.0 is 400 mm, so a 0.25 spool radius is a 200 mm spool. Read off the product photos.
PITCH = 0.215  # one housing's width, seam included
SEAM = 0.008  # the gap between neighbouring housings
WALL = 0.03  # a housing's red side wall, framing its window
D = 0.58  # housing depth, front to back
BASE_Z = 0.17  # the dock's height
BASE_FRONT, BASE_BACK, BASE_LEAN = -0.07, D + 0.03, 0.035
BASE_SIDE = 0.03  # how far the dock reaches past the end housings
BLACK_TOP = BASE_Z + 0.1  # a box's black base at its front, under the latch
WINDOW_Z = BLACK_TOP + 0.06  # the window's bottom edge, over the red band with the latch
ARCH_R = D / 2
ARCH_Z = 0.58  # where the half-round top springs from the walls
BLACK_REAR = ARCH_Z  # the same black base covers the back wall: its side rises toward the rear
SPOOL_Y, SPOOL_Z, SPOOL_R = D / 2, ARCH_Z, 0.25
RED, BLACK = "#b3201c", "#4a4b51"  # the bases and dock are black; grey reads on a dark screen
GLASS = "#c9b9b4"
INTERIOR = "#24100f"
LED, LED_CORE = "#dff3ec", "#3a4a46"
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def width(lanes):
    return lanes * PITCH


def centers(lanes):
    return [PITCH / 2 + i * PITCH for i in range(lanes)]


def _arch(z0, steps=24):
    """The housing's side outline from the front at height z0, over the top, down the back to z0."""
    pts = [(0.0, z0), (0.0, ARCH_Z)]
    pts += [(ARCH_R - ARCH_R * math.cos(math.pi * i / steps), ARCH_Z + ARCH_R * math.sin(math.pi * i / steps)) for i in range(1, steps)]
    return pts + [(D, ARCH_Z), (D, z0)]


# A box from the side: its black base, low at the front and rising toward the back, and the
# red shell above it, meeting the base along that slope.
BOX_BASE = [(0.0, BASE_Z), (0.0, BLACK_TOP), (D, BLACK_REAR), (D, BASE_Z)]
UPPER = _arch(BLACK_TOP)[:-1] + [(D, BLACK_REAR)]
WINDOW = _arch(WINDOW_Z)
# The dock under the whole row, its front lip leaning back as it rises.
DOCK = [(BASE_FRONT, 0.0), (BASE_FRONT + BASE_LEAN, BASE_Z), (BASE_BACK, BASE_Z), (BASE_BACK, 0.0)]


def _side(sc, x, profile, color, cam):
    sc.poly([(x, y, z) for y, z in profile], kit.shade(cam, color, (1, 0, 0), amb=0.72, dif=0.5, spec=0.1))


def _box_right_side(sc, x1, cam):
    """A box's right side: red shell over its black base, meeting along the slope."""
    _side(sc, x1, UPPER, RED, cam)
    _side(sc, x1, BOX_BASE, BLACK, cam)


def _box(sc, x0, x1, xc, color, lane, c, cam, detail):
    """One box, back to front: its dark inside and left wall, its spool, then its right side and
    front, which the spool stands behind. Drawn in order across the row, each box hides the
    spool of the box to its left, and its own spool hides this box's left wall."""
    a, b = x0 + WALL, x1 - WALL
    sc.poly([(a, 0.0, BLACK_TOP), (b, 0.0, BLACK_TOP), (b, D, BLACK_REAR), (a, D, BLACK_REAR)], INTERIOR)
    kit.lid_back(sc, UPPER, a, b, INTERIOR, 1.0, 1.0)
    _side(sc, a, UPPER, RED, cam)
    spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, color, c, lane, detail=detail)
    _box_right_side(sc, x1, cam)
    sc.poly(kit.rrect_front(x0, x1, BASE_Z, BLACK_TOP, 0.0, 0.0), kit.shade(cam, BLACK, (0, -1, 0), amb=0.8, dif=0.4, spec=0))
    front = kit.shade(cam, RED, (0, -1, 0), amb=0.72, dif=0.5, spec=0.1)
    sc.poly(kit.rrect_front(x0, x1, BLACK_TOP, WINDOW_Z, 0.0, 0.0), front)
    for a, b in ((x0, x0 + WALL), (x1 - WALL, x1)):
        sc.poly(kit.rrect_front(a, b, WINDOW_Z, ARCH_Z, 0.0, 0.0), front)
        sc.extrude_x(_arch(ARCH_Z), a, b, RED, 1.0, front=True)
    # The rotary latch, a black teardrop on the line where the red band meets the black base.
    feeder.disc(sc, xc, -0.004, BLACK_TOP + 0.012, 0.034, "#0f0f11")
    sc.poly([(xc + 0.012, -0.005, BLACK_TOP + 0.037), (xc + 0.034, -0.005, BLACK_TOP + 0.012), (xc + 0.012, -0.005, BLACK_TOP - 0.013)], "#0f0f11")
    if detail:
        feeder.disc(sc, xc - 0.004, -0.006, BLACK_TOP + 0.005, 0.005, "#c8c8c8")


def draw_art(sc, colors, tier):
    detail = tier == "full"
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    lanes = len(colors)
    w = width(lanes)
    spans = [(i * PITCH + SEAM / 2, (i + 1) * PITCH - SEAM / 2) for i in range(lanes)]

    sc.poly([(-BASE_SIDE, BASE_FRONT + 0.03, 0), (w + BASE_SIDE + 0.04, BASE_FRONT + 0.03, 0), (w + BASE_SIDE + 0.04, BASE_BACK + 0.03, 0), (-BASE_SIDE, BASE_BACK + 0.03, 0)], "#000", 0.55, blur=True)
    feeder.solid(sc, DOCK, -BASE_SIDE, w + BASE_SIDE, BLACK)
    lean = BASE_LEAN * 0.45
    for xc in centers(lanes):
        sc.poly(kit.rrect_front(xc - 0.028, xc + 0.028, 0.065, 0.105, 0.008, BASE_FRONT + lean - 0.002), LED)
        sc.poly(kit.rrect_front(xc - 0.016, xc + 0.016, 0.075, 0.095, 0.004, BASE_FRONT + lean - 0.003), LED_CORE)
    for lane, ((x0, x1), xc, col) in enumerate(zip(spans, centers(lanes), colors)):
        _box(sc, x0, x1, xc, col, lane, c, cam, detail)

    # The windows, over everything they are in front of.
    sc.layer = "front"
    for x0, x1 in spans:
        sc.extrude_x(WINDOW, x0 + WALL, x1 - WALL, GLASS, 0.16 if detail else 0.1, front=True)


# On the tray: the dock is the front wall, each box's black base rises toward the back as
# the body's contour, and each box's red top stands on it around its glass.
_WALL = 26.0
_BAND = 10.0  # each box's base along the top of the wall; the dock below it runs the whole row
DOCK_COLOR = "#36373c"
_RISE = (BLACK_REAR - BLACK_TOP) * TRAY_PX_PER_UNIT
_RED, _BLACK = int(RED[1:], 16), int(BLACK[1:], 16)
UNIT = UnitDef(
    "emu",
    4,
    draw_art,
    TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, _RISE)],
        # Measured from the spool's bottom, which the tray stands a front wall below the lid.
        dome=kit.tray_profile(_arch(BLACK_TOP), SPOOL_Z - SPOOL_R + _WALL / TRAY_PX_PER_UNIT, TRAY_PX_PER_UNIT),
        back_extra_px=_RISE,
        front_wall_px=_WALL,
        rail_px=6.0,
        cap_clearance_px=6.0,
        lid_spans_row=False,
        has_latch=False,
        body=_BLACK,
        panel=_BLACK,
        rail=_BLACK,
        accent=_RED,
        # The lane's number badge stands where its status light is on the real dock.
        front=[kit.span(0, 0, 0, _WALL, int(DOCK_COLOR[1:], 16), 2), kit.lane(-28, 28, 0, _BAND, _BLACK, 1)],
        # Lanes leave each box through its own sensors and meet in a separate printed combiner.
        lane_join="junction",
        lid_color=_RED,
    ),
    lane_variants=(1, 2, 3, 4, 5, 6, 8),
)
