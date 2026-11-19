# SPDX-License-Identifier: GPL-3.0-or-later
"""Tradrack from the front, from the product photo: 43.6 photo px to a gate, fourteen in the photo.

The gates' couplers are on the back of the beam, so from the front the lanes go in behind it.
"""
from .rail_face import FaceAnchors, Photo, shadow

PITCH = 43.6
GATE0 = 112  # left edge of gate 0
FLOOR = 610
RIGHT_END = 640  # photo x from which parts move with the gate count
# The carriage reaches past gate 0 by its filament motor's width; the drive end stands clear of it.
LEFT_END, LEFT_PAD = 165, 130
BLK, BLK2 = "#303136", "#3c3d43"
GREEN = "#25c275"
BLUE, BLUE_D = "#3fa6ea", "#123a5c"
STEEL, FLANGE = "#c3c7cc", "#a9adb3"
BELT, BELT_TEETH = "#4a3328", "#6b4a3a"


def gate_x(i):
    return GATE0 + PITCH * i + PITCH / 2


def _feet(a):
    """A splayed leg under each end, out to a flanged pad."""
    for x0, x1, out in ((50, 120, -1), (740, 810, 1)):
        a.front([(x0 + 26, 512), (x1 - 26, 512), (x1 if out > 0 else x1 - 30, 596), (x0 if out < 0 else x0 + 30, 596)], 0.12, BLK2)
        a.box(x0 - 6, 594, x1 + 6, FLOOR, 0.0, 0.3, BLK)
        a.disc(x0 + 6 if out < 0 else x1 - 6, 602, 4, 0.0, "#55575e")


def _selector_motor(a):
    """The selector motor at the left end, face-on, on its green bracket."""
    a.box(40, 330, 140, 430, -0.04, 0.3, "#26272b")
    a.front([(46, 336), (134, 336), (134, 424), (46, 424)], -0.04, "#2f3035")
    a.disc(90, 380, 18, -0.041, "#3d3e44")
    a.disc(90, 380, 6, -0.042, STEEL)
    for x, y in ((52, 342), (128, 342), (52, 418), (128, 418)):
        a.disc(x, y, 4, -0.041, "#55575e")
    a.box(100, 424, 160, 470, -0.03, 0.3, GREEN)
    a.box(60, 430, 120, 512, 0.0, 0.3, BLK)


def _carriage(a, detail):
    """The selector at gate 0: the filament motor riding behind, its drive, green bracket and coupler."""
    a.by_gate = True
    c = gate_x(0)
    a.box(c - 92, 344, c - 10, 400, 0.1, 0.36, "#26272b")
    a.box(c - 92, 346, c - 80, 398, 0.09, 0.36, FLANGE, spec=0.5)
    a.box(c - 22, 346, c - 10, 398, 0.09, 0.36, FLANGE, spec=0.5)
    a.box(c - 8, 360, c + 40, 398, 0.08, 0.3, BLK)
    a.box(c - 2, 366, c + 34, 380, 0.07, 0.08, "#1b1c1f")
    if detail:
        for k, color in enumerate(("#d94f3a", "#f0a030", "#3f7fd6", "#4cc070")):
            x = c + 4 + 8 * k
            a.wire([(x, 380), (x + 3, 392), (x - 6, 398)], 0.06, color, 2.4)
    a.box(c - 44, 396, c + 44, 440, -0.02, 0.3, BLK2)
    a.box(c - 36, 432, c + 34, 500, -0.06, 0.2, GREEN)
    a.box(c + 2, 440, c + 26, 456, -0.07, -0.06, "#e9eaec")
    a.box(c - 28, 474, c - 4, 498, -0.08, -0.06, BLK)
    a.ptfe(c - 16, 486, FLOOR + 8, -0.079)
    a.disc(c - 16, 486, 10, -0.081, BLUE)
    a.disc(c - 16, 486, 4, -0.082, BLUE_D)
    # The chain's moving run leaves the carriage toward the idler and bends down to the fixed run.
    a.u_chain((c + 40, 404), c + 120, 404, 530, -0.07, 1)
    a.by_gate = False
    return c


def draw(sc, gates, tier="full"):
    detail = tier == "full"
    a = Photo(sc, gates, GATE0, PITCH, 14, FLOOR, split=RIGHT_END, left_end=LEFT_END, left_pad=LEFT_PAD)
    shadow(sc, a.X(40), a.X(820))
    _feet(a)
    # The beam, a linear rail along its face, the idler end.
    a.box(96, 446, 790, 522, 0.04, 0.34, BLK)
    a.box(110, 470, 776, 486, 0.02, 0.04, "#45474e", spec=0.4)
    a.front([(110, 470), (776, 470), (776, 473), (110, 473)], 0.02, "#7d8088")
    a.box(770, 400, 806, 522, 0.0, 0.34, BLK2)
    a.disc(788, 420, 8, 0.0, STEEL)
    for y in (412, 428):
        a.wire([(130, y), (788, y)], 0.2, BELT, 6)
        if detail:
            a.wire([(130, y), (788, y)], 0.19, BELT_TEETH, 2)
    _selector_motor(a)
    # The chain's fixed run along the bottom front, anchored a third of the way along.
    a.by_gate = True
    a.chain([(GATE0 + PITCH * gates * 0.35, 530), (770 + a.shift - 10, 530)], -0.06)
    a.by_gate = False
    sc.layer = "carriage"
    c = _carriage(a, detail)
    sc.layer = "front"
    a.by_gate = True
    anchors = FaceAnchors([a.point(gate_x(i), 446, 0.2) for i in range(gates)], a.point(c - 16, FLOOR + 8, -0.08))
    a.by_gate = False
    return anchors
