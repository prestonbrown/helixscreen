# SPDX-License-Identifier: GPL-3.0-or-later
"""XOL: a square black hot-end box with a yellow fan frame and angled ducts, under an open-framed extruder and its motor disc."""
import math

from ..head import Drawing, HeadDef, rounded

BODY, DARK, YELLOW, BOLT = "#2c2d31", "#121315", "#efbf1d", "#0d0e10"
HALF_WIDTH, BOX_BOTTOM, BOX_TOP, BOX_DEPTH = 0.215, 0.12, 0.55, 0.38
BACK_HALF_WIDTH = 0.287
TIP = (0.0, 0.08, 0.05)
FAN_Z = 0.31
FRAME = rounded([(-0.14, 0.145), (0.14, 0.145), (0.15, 0.17), (0.15, 0.45), (0.09, 0.525), (-0.09, 0.525), (-0.15, 0.45), (-0.15, 0.17)], 0.02)
NECK_TOP = 0.62
FRAME_Y0, FRAME_Y1 = 0.04, 0.16
MOTOR_X, MOTOR_Z, MOTOR_R = 0.02, 0.8, 0.17
LEVER_X, LEVER_BOTTOM, LEVER_SCALE = -0.07, 0.62, 0.00052
# Traced from the part's outline, in pixels of the reference photo with the bolt column at 430.
LEVER_TRACE = [
    (385, 870), (470, 880), (530, 825), (530, 745), (730, 745), (770, 670), (720, 595), (590, 585), (575, 530), (575, 400),
    (530, 250), (470, 210), (400, 225), (360, 290), (345, 345), (290, 430), (290, 480), (330, 560), (380, 600), (370, 830),
]


def _lever_point(px, py):
    return (LEVER_X + (px - 430) * LEVER_SCALE, LEVER_BOTTOM + (880 - py) * LEVER_SCALE)


def _motor(d):
    """The stepper behind the frame: a large disc facing forward with its can running back."""
    n, y0, y1 = 32, FRAME_Y1, 0.42
    ring = [(MOTOR_X + MOTOR_R * math.cos(2 * math.pi * i / n), MOTOR_Z + MOTOR_R * math.sin(2 * math.pi * i / n)) for i in range(n)]
    for a, b in zip(ring, ring[1:] + ring[:1]):
        out = (((a[0] + b[0]) / 2 - MOTOR_X), 0.0, ((a[1] + b[1]) / 2 - MOTOR_Z))
        if sum(p * q for p, q in zip(out, d.camera.view)) > 0:
            d.facet([(a[0], y0, a[1]), (a[0], y1, a[1]), (b[0], y1, b[1]), (b[0], y0, b[1])], "#232428")
    d.face(ring, y0, "#3a3b40")
    d.disc(MOTOR_X, MOTOR_Z, MOTOR_R * 0.62, y0 - 0.001, "#26272b", sides=24)


def _duct(d, side):
    """One duct: the box's own corner footprint at the top, angling in and forward toward the nozzle."""
    top = [(0.095, 0.0), (HALF_WIDTH, 0.0), (BACK_HALF_WIDTH, BOX_DEPTH), (0.095, BOX_DEPTH)]
    foot = [(0.05, 0.03), (0.135, 0.03), (0.135, 0.17), (0.05, 0.17)]
    zt, zf = BOX_BOTTOM + 0.001, 0.02
    corners = [((side * tx, ty), (side * fx, fy)) for (tx, ty), (fx, fy) in zip(top, foot)]
    # Back wall, the wall turned from the camera, the wall turned to it, then the front.
    for i in (2, 3 if side > 0 else 1, 1 if side > 0 else 3, 0):
        (a_top, a_foot), (b_top, b_foot) = corners[i], corners[(i + 1) % 4]
        d.facet([(a_top[0], a_top[1], zt), (b_top[0], b_top[1], zt), (b_foot[0], b_foot[1], zf), (a_foot[0], a_foot[1], zf)], DARK)


def _flank_x(y):
    """Half the box's width at depth y: its fan sides splay apart from front to back."""
    return HALF_WIDTH + (BACK_HALF_WIDTH - HALF_WIDTH) * y / BOX_DEPTH


def _box(d):
    foot = [(-HALF_WIDTH, 0.0), (HALF_WIDTH, 0.0), (BACK_HALF_WIDTH, BOX_DEPTH), (-BACK_HALF_WIDTH, BOX_DEPTH)]
    d.scene.prism(foot, BOX_BOTTOM, BOX_TOP, BODY, amb=0.62, dif=0.55, spec=0.18)
    d.facet([(x, y, BOX_TOP) for x, y in foot], BODY)


def _side_blower(d):
    cy, cz, r, n = BOX_DEPTH / 2, FAN_Z, 0.14, 24

    def on_flank(y, z, lift=0.001):
        return (_flank_x(y) + lift, y, z)

    d.facet([on_flank(cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)], DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        for i in range(n):
            t = 2 * math.pi * i / n
            d.scene.line([on_flank(cy + r * k * math.cos(t), cz + r * k * math.sin(t)) for k in (0.55, 0.95)], "#4b4d52", 0.004)
    d.facet([on_flank(cy + 0.06 * math.cos(2 * math.pi * i / n), cz + 0.06 * math.sin(2 * math.pi * i / n), 0.002) for i in range(n)], "#232427", amb=0.9, dif=0.35, spec=0.05)


def _neck(d):
    """A straight column narrower than the box, flush with its front, leaving the box top flat on either side."""
    d.solid([(-0.15, BOX_TOP), (0.15, BOX_TOP), (0.15, NECK_TOP), (-0.15, NECK_TOP)], 0.0, BOX_DEPTH, BODY)
    d.face([(-0.09, BOX_TOP), (0.09, BOX_TOP), (0.09, NECK_TOP), (-0.09, NECK_TOP)], -0.001, DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        for x in (-0.05, 0.0, 0.05):
            d.face([(x - 0.012, BOX_TOP), (x + 0.012, BOX_TOP), (x + 0.012, NECK_TOP), (x - 0.012, NECK_TOP)], -0.002, "#34353a")


def _frame(d):
    """The open skeleton: a foot bar, a top bar, the gear housing between them and the knob arm at the right."""
    y0, y1 = FRAME_Y0, FRAME_Y1
    d.solid([(-0.17, NECK_TOP), (0.17, NECK_TOP), (0.17, 0.675), (-0.17, 0.675)], y0, y1, BODY)
    d.solid([(0.075, 0.675), (0.115, 0.675), (0.155, 0.87), (0.115, 0.87)], y0 + 0.02, y1, BODY)
    d.solid([(-0.03, 0.675), (0.065, 0.675), (0.065, 0.905), (-0.03, 0.905)], y0, y1, DARK)
    d.solid([(-0.165, 0.905), (0.14, 0.905), (0.14, 0.955), (-0.165, 0.955)], y0, y1, BODY)
    d.solid([(-0.04, 0.955), (0.04, 0.955), (0.04, 0.985), (-0.04, 0.985)], y0, y1, BODY)
    d.disc(-0.15, 0.93, 0.022, y0 - 0.001, BOLT, sides=12)
    d.disc(0.14, 0.648, 0.022, y0 - 0.001, BOLT, sides=12)
    _spring_and_knob(d)


def _spring_and_knob(d):
    """The tension screw runs across the head: coils around the x axis, ending in a knob whose face looks right."""
    cy, cz, n = (FRAME_Y0 + FRAME_Y1) / 2, 0.845, 16
    if d.detail:
        for coil in range(6):
            x = 0.07 + coil * 0.008
            d.scene.line([(x + 0.008 * i / n, cy + 0.028 * math.cos(math.pi * i / n), cz - 0.028 * math.sin(math.pi * i / n)) for i in range(n + 1)], "#b7b9bd", 0.005)
    x0, x1, r = 0.118, 0.16, 0.05
    ring = [(cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    for a, b in zip(ring, ring[1:] + ring[:1]):
        out = (0.0, (a[0] + b[0]) / 2 - cy, (a[1] + b[1]) / 2 - cz)
        if sum(p * q for p, q in zip(out, d.camera.view)) > 0:
            d.facet([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], "#1c1d20")
    d.facet([(x1, y, z) for y, z in ring], "#8d8f94")


def _lever(d):
    """The yellow tensioner: a bolted spine, a hub, and a slotted tail to the right."""
    y = FRAME_Y0 - 0.004
    d.face(rounded([_lever_point(*p) for p in LEVER_TRACE], 0.012), y, YELLOW)
    x, z = _lever_point(430, 475)
    d.disc(x, z, 0.034, y - 0.001, "#5a5b60", sides=16)
    d.disc(x, z, 0.022, y - 0.002, DARK, sides=14)
    for px, py in ((430, 305), (432, 645), (437, 808)):
        bx, bz = _lever_point(px, py)
        d.disc(bx, bz, 0.026, y - 0.001, BOLT, sides=12)
    if d.detail:
        hx, hz = _lever_point(687, 672)
        d.disc(hx, hz, 0.014, y - 0.001, BOLT, sides=10)
        d.face([_lever_point(535, 650), _lever_point(625, 650), _lever_point(625, 672), _lever_point(535, 672)], y - 0.001, BOLT)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    for side in (-1, 1):
        _duct(d, side)
    d.nozzle(TIP[0], TIP[1], 0.12, r_top=0.024, r_tip=0.008, z_tip=TIP[2])
    _box(d)
    _side_blower(d)
    d.face(FRAME, -0.002, YELLOW)
    d.fan(0.0, FAN_Z, 0.132, -0.003, blade="#232427")
    if d.detail:
        for x, z in ((-0.122, 0.17), (0.122, 0.17), (-0.122, 0.45), (0.122, 0.45)):
            d.disc(x, z, 0.019, -0.004, BOLT, sides=12)
    _motor(d)
    _neck(d)
    _frame(d)
    d.tube(0.0, 0.1, 0.985, 1.03, 0.042, "#7f8fae")
    _lever(d)


HEAD = HeadDef("xol", "XOL", draw_art, TIP, entry=(0.0, 0.1, 1.03), size=1.06)
