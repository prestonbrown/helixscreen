# SPDX-License-Identifier: GPL-3.0-or-later
"""ERCF v2 from the front, from the product photo: 60 photo px to a gate, six gates in the photo."""
from .rail_face import FaceAnchors, Photo, shadow

PITCH = 60
GATE0 = 298  # left edge of gate 0
FLOOR = 625
BLK, BLK2, RIM = "#303136", "#3c3d43", "#5a5c64"
RED, RED_D, RED_LIT = "#cf2420", "#9e1a17", "#e8433d"
BELT = "#3a2a26"
BLUE, ORANGE, BRASS = "#3f8fd9", "#f08228", "#c9a04a"
STEEL = "#c3c7cc"


def gate_x(i):
    return GATE0 + PITCH * i + PITCH / 2


def _base(a, last):
    """The red bracket under the gate row, and a low flanged foot under each end."""
    a.by_gate = True
    a.box(GATE0 + 6, 596, last - 6, 612, -0.01, 0.32, RED)
    for x in (GATE0 + 6, last - 22):
        a.box(x, 610, x + 16, 618, 0.0, 0.3, RED_D)
    a.by_gate = False
    for pad, block, holes in (((78, 210), (112, 196), (92, 196)), ((700, 812), (704, 788), (712, 802))):
        a.box(pad[0], 606, pad[1], FLOOR, -0.03, 0.4, RED)
        a.box(block[0], 572, block[1], 606, -0.02, 0.38, RED)
        for x in holes:
            a.disc(x, 615, 4, -0.03, "#5e1210")


def _idler(a, detail):
    """The idler end: a block with a triangular truss and the belt pulley on top."""
    a.box(688, 340, 792, 597, 0.0, 0.34, BLK)
    a.front([(700, 352), (782, 352), (782, 392), (728, 470), (700, 470)], 0.0, BLK2)
    a.front([(712, 400), (770, 400), (722, 462)], 0.0, "#141518")
    if detail:
        for x, y in ((706, 356), (776, 356), (706, 464)):
            a.disc(x, y, 4, 0.0, "#55575e")
    a.box(735, 316, 770, 342, 0.06, 0.22, STEEL, spec=0.5)
    a.disc(752, 329, 6, 0.06, "#6d7076")


def _gates(a, gates, detail):
    """Black gate bodies, each with a slot cap, a stepped red latch and its brass screw."""
    a.by_gate = True
    for i in range(gates):
        x = gate_x(i)
        a.box(x - 29, 418, x + 29, 597, 0.02, 0.3, BLK)
        a.box(x - 25, 412, x + 25, 446, 0.04, 0.28, BLK2)
        a.front([(x - 27, 448), (x + 27, 448), (x + 27, 498), (x + 9, 498), (x + 9, 486), (x - 27, 486)], 0.02, RED)
        a.front([(x - 27, 448), (x + 27, 448), (x + 27, 452), (x - 27, 452)], 0.02, RED_LIT)
        a.disc(x + 17, 492, 3, 0.02, BRASS)
        if detail:
            a.front([(x - 14, 512), (x + 14, 512), (x + 14, 536), (x - 14, 536)], 0.02, "#16171a")
            a.disc(x + 18, 572, 3, 0.02, "#77797f")
    a.by_gate = False


def _drive(a, detail):
    """The drive end: stepper, rod clamp, the gearbox with its stripes, red cap and tension arm."""
    a.box(35, 332, 132, 432, 0.04, 0.34, "#26272b")
    for y in range(352, 420, 9):
        a.front([(40, y), (126, y), (126, y + 3), (40, y + 3)], 0.04, "#34353a")
    for y in (338, 420):
        a.front([(40, y), (126, y), (126, y + 6), (40, y + 6)], 0.04, "#45474d")
    if detail:
        for k, color in enumerate(("#3f7fd6", "#3fc0c0", "#d23fae", "#3f7fd6")):
            a.wire([(48, 392 + 7 * k), (36, 398 + 7 * k), (46, 412 + 7 * k)], 0.03, color, 3)
    a.box(128, 326, 285, 420, 0.02, 0.32, BLK2)
    a.box(150, 340, 270, 410, 0.0, 0.02, BLK)
    a.box(100, 418, 300, 446, -0.01, 0.34, RED)
    a.box(100, 446, 205, 597, 0.0, 0.34, BLK)
    for k in range(3):
        a.front([(128 + 14 * k, 535), (140 + 14 * k, 535), (168 + 14 * k, 490), (156 + 14 * k, 490)], 0.0, RED_D)
    a.box(200, 408, 242, 586, -0.03, 0.2, RED)
    a.front([(214, 440), (228, 440), (228, 470), (214, 470)], -0.03, RED_D)


def _carriage(a, detail):
    """The selector at gate 0: tall body, red cap, chain tower, the coupler with its clip and tube."""
    a.by_gate = True
    c = gate_x(0)
    a.box(c - 40, 336, c + 40, 600, -0.08, 0.26, BLK2)
    a.box(c - 40, 322, c + 40, 338, -0.06, 0.26, RED)
    a.box(c - 30, 258, c + 34, 324, 0.02, 0.22, BLK)
    a.front([(c - 18, 270), (c + 24, 270), (c + 3, 318)], 0.02, "#151619")
    a.front([(c - 36, 440), (c + 36, 440), (c + 36, 444), (c - 36, 444)], -0.08, RIM)
    if detail:
        a.front([(c - 20, 478), (c - 4, 462), (c + 4, 470), (c - 12, 486)], -0.08, "#4a4c53")
    a.ptfe(c, 566, FLOOR + 8, -0.079)
    a.disc(c, 566, 15, -0.081, BLUE)
    a.disc(c, 566, 6, -0.082, "#16304f")
    a.disc(c + 20, 550, 9, -0.083, ORANGE)
    # The chain's moving run leaves the tower toward the drive end and bends down behind the rods.
    bend = c - 104
    r = a.u_chain((c - 28, 286), bend, 286, 402, 0.32, -1)
    a.rod(bend - r - 14, bend + 12, 376, 0.12)
    a.rod(bend - r - 14, bend + 12, 348, 0.22)
    a.by_gate = False
    return c


def draw(sc, gates, tier="full"):
    detail = tier == "full"
    a = Photo(sc, gates, GATE0, PITCH, 6, FLOOR)
    last = GATE0 + PITCH * gates
    shadow(sc, a.X(60), a.X(790))
    a.box(240, 392, 700, 418, 0.3, 0.36, "#26272b")
    a.box(560, 388, 690, 412, 0.24, 0.3, RED)
    _base(a, last)
    _idler(a, detail)
    a.rod(150, 790, 376, 0.12)
    a.wire([(170, 362), (760, 362)], 0.2, BELT, 6)
    a.rod(150, 790, 348, 0.22)
    # The chain's fixed run lies along the gate tops to the idler end.
    a.chain([(150, 402), (700, 402)], 0.33)
    _gates(a, gates, detail)
    _drive(a, detail)
    sc.layer = "carriage"
    c = _carriage(a, detail)
    sc.layer = "front"
    a.by_gate = True
    anchors = FaceAnchors([a.point(gate_x(i), 412, 0.15) for i in range(gates)], a.point(c, FLOOR + 8, -0.08))
    a.by_gate = False
    return anchors
