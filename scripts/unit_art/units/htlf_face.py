# SPDX-License-Identifier: GPL-3.0-or-later
"""HTLF from the front, from the project's render: 90 photo px to a lane, four lanes.

The lanes come in at the back, under the lid, and each leaves through its own collet on the
front, so every lane has its own outlet straight below its inlet and nothing slides.
"""
from .rail_face import FaceAnchors, Photo

PITCH = 90
GATE0 = 395  # left edge of lane 0
FLOOR = 640
TOP = 170
# The HTLF palette, shared with its three-quarter drawing.
BODY, BODY_D, LID, LID_D = "#3f9a6e", "#2f7a56", "#d3b97f", "#b89c62"
MOTOR, MOTOR_D = "#1d1e21", "#141517"
GOLD, BLUE, LAMP = "#b9a05a", "#1e2f73", "#54e04a"
STEEL = "#c4c6ca"


def gate_x(i):
    return GATE0 + PITCH * i + PITCH / 2


def _motors(a):
    """The drive motor out of the left end, the selector motor high on the right."""
    a.box(65, 290, 200, 470, 0.04, 0.3, MOTOR)
    a.front([(72, 298), (196, 298), (196, 462), (72, 462)], 0.04, MOTOR_D)
    a.box(830, 195, 915, 360, 0.04, 0.3, MOTOR)
    a.front([(834, 202), (910, 202), (910, 352), (834, 352)], 0.04, MOTOR_D)


def _dial(a, detail):
    """The cam drum's geared ring in its slot beside the lid."""
    a.box(762, 190, 800, 384, 0.02, 0.2, "#1b1c1f")
    for x in (764, 794):
        a.box(x, 192, x + 6, 382, 0.015, 0.2, GOLD)
    if detail:
        for y in range(204, 380, 14):
            a.front([(772, y), (790, y), (790, y + 5), (772, y + 5)], 0.014, "#3a3b40")


def draw(sc, gates, tier="full"):
    detail = tier == "full"
    a = Photo(sc, gates, GATE0, PITCH, 4, FLOOR)
    _motors(a)
    # Green walls and front, then the lid over the top and the upper front.
    a.box(200, TOP, 830, FLOOR, 0.0, 0.5, BODY)
    a.box(235, TOP - 4, 760, 385, -0.02, 0.48, LID)
    a.front([(235, TOP + 26), (760, TOP + 26), (760, TOP + 30), (235, TOP + 30)], -0.02, LID_D)
    _dial(a, detail)
    # The outlet row: five panels, a collet in each of the first four.
    a.front([(380, 385), (822, 385), (822, 500), (380, 500)], -0.001, BODY_D)
    for x in (380, 470, 560, 650, 740, 822):
        a.front([(x - 2, 387), (x + 2, 387), (x + 2, 498), (x - 2, 498)], -0.002, BODY)
    # The drive gear shows through a slot left of the outlets.
    a.front([(300, 440), (350, 440), (350, 520), (300, 520)], -0.001, "#16171a")
    if detail:
        for x in (310, 322, 334):
            a.front([(x, 446), (x + 5, 446), (x + 5, 514), (x, 514)], -0.002, GOLD)
    a.front([(205, 505), (825, 505), (825, 509), (205, 509)], -0.001, BODY_D)
    a.by_gate = True
    for i in range(gates):
        x = gate_x(i)
        a.front([(x - 14, 290), (x + 14, 290), (x + 14, 318), (x - 14, 318)], -0.022, LAMP)
        a.disc(x, 442, 20, -0.003, BLUE)
        a.disc(x, 442, 9, -0.004, STEEL)
        a.ptfe(x, 442, FLOOR + 30, -0.006)
    anchors = FaceAnchors([a.point(gate_x(i), TOP, 0.2) for i in range(gates)], a.point(gate_x(0), FLOOR + 30, -0.006))
    a.by_gate = False
    return anchors
