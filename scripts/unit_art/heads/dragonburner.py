# SPDX-License-Identifier: GPL-3.0-or-later
"""Dragon Burner v8 with a Galileo 2 standalone extruder, measured from the v8 and G2SA STEP assemblies.

A red cowl: a faceted cap, a front plate around the 3010 hotend fan and two duct feet. Black 4010 blowers fill
the cowl's sides, and the extruder sits on a dark mount behind the cap. Sizes are in millimetres, scaled by MM.
"""
import math

from ..head import Drawing, HeadDef, rounded

BODY, MOUNT, EXTRUDER, BLOWER, KNOB, DARK = "#b8242a", "#2f3034", "#3c3d42", "#232427", "#1a1b1e", "#101113"
SCREW = "#c6c8cc"
MM = 0.43 / 55.0

HALF_WIDTH = 27.5 * MM
PLATE_HALF = 16.5 * MM
DUCT_INNER = 15.0 * MM
DUCT_TOP = 12.0 * MM
DUCT_DEPTH = 40.0 * MM
PLATE_BOTTOM = 8.0 * MM
BODY_DEPTH = 35.0 * MM
CAP_BOTTOM = 52.0 * MM
CAP_SHOULDER = 61.0 * MM
CAP_TOP = 67.0 * MM
CAP_DEPTH = 14.0 * MM
CHAMFER = 6.0 * MM
FAN_Z = 33.0 * MM
FAN_R = 13.6 * MM
# The mounts behind the cap: the hotend's, then the extruder's on top of it.
MOUNT_FRONT, MOUNT_BACK = 14.0 * MM, 37.0 * MM
HOTEND_MOUNT_BOTTOM = 44.0 * MM
EXTRUDER_MOUNT_BOTTOM = 61.0 * MM
DECK = 67.4 * MM  # the extruder's foot
# The Galileo 2 standalone, placed as its mount puts it: everything coaxial with the feed knob, the pancake
# motor last, standing behind the housing with its round face to the back.
G2_AXIS = (-8.3 * MM, 87.9 * MM)
KNOB_FRONT, KNOB_R = 1.4 * MM, 10.9 * MM
FRONT_PLATE_Y = (12.0 * MM, 18.9 * MM)
MAIN_BODY_Y = (14.0 * MM, 34.0 * MM)
REAR_PLATE_Y = (34.0 * MM, 38.9 * MM)
RING_GEAR_Y, RING_GEAR_R = (37.9 * MM, 46.0 * MM), 20.0 * MM
MOTOR_Y, MOTOR_R = (46.0 * MM, 65.0 * MM), 18.9 * MM
G2_TOP = 108.4 * MM
COLLET_Y = 24.9 * MM
# Only the brass tip shows, poking below the duct feet's underside by about 15% of a foot's height as in the v8
# render. The hotend really sits about 25 mm back, where the front plate would hide it from this camera.
TIP = (0.0, 4.0 * MM, -3.1 * MM)


def _circle(cy, cz, r, n=24):
    return [(cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def _cylinder_x(d, cy, cz, r, x0, x1, color, cap, n=16):
    """A cylinder running across the head, its right end capped."""
    ring = _circle(cy, cz, r, n)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], color)
    d.facet([(x1, y, z) for y, z in ring], cap)


def _cylinder_y(d, cx, cz, r, y0, y1, color, cap, n=16):
    """A cylinder running front to back, its near end capped."""
    ring = _circle(cx, cz, r, n)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])], color)
    d.facet([(x, y0, z) for x, z in ring], cap)


def _hexagon(cx, cz, r, turn=0.0):
    return [(cx + r * math.cos(turn + math.pi * i / 3), cz + r * math.sin(turn + math.pi * i / 3)) for i in range(6)]


def _voron_mark(d, cx, cz, size, y, color):
    for dx, z0, z1 in ((-0.8, 0.0, 1.0), (0.0, -1.0, 1.0), (0.8, -1.0, 0.0)):
        half = 0.27 * size
        pts = [(cx + dx * size + z * size * 0.5 + w, cz + z * size) for z, w in ((z0, -half), (z0, half), (z1, half), (z1, -half))]
        d.face(pts, y, color)


def _duct(d, s):
    """One duct foot: a quarter round on the outside, running the full depth under its blower."""
    r, cx, cz = 10.0 * MM, 17.5 * MM, 10.0 * MM
    arc = [(cx + r * math.cos(-math.pi / 2 * i / 8), cz + r * math.sin(-math.pi / 2 * i / 8)) for i in range(9)]
    foot = [(14.6 * MM, DUCT_TOP), (HALF_WIDTH, DUCT_TOP)] + arc + [(15.7 * MM, 0.0), (14.4 * MM, 4.0 * MM)]
    foot = [(s * x, z) for x, z in foot]
    d.solid(foot, 0.0, DUCT_DEPTH, BODY, bevel=1.0 * MM)


def _blower(d, s):
    """A 4010 blower standing in the cowl's side, its outlet down into the duct."""
    x0, x1 = sorted((s * PLATE_HALF, s * HALF_WIDTH))
    d.solid([(x0, DUCT_TOP), (x1, DUCT_TOP), (x1, CAP_BOTTOM), (x0, CAP_BOTTOM)], 0.6 * MM, BODY_DEPTH, BLOWER)
    if d.detail:
        # The clips that hold the blower read as notches across its front edge.
        for z in (24.0 * MM, 40.0 * MM):
            d.face([(x0 + 1.5 * MM, z), (x1 - 1.0 * MM, z), (x1 - 1.0 * MM, z + 1.4 * MM), (x0 + 1.5 * MM, z + 1.4 * MM)], 0.5 * MM, DARK)
    if s > 0:
        _blower_intake(d, HALF_WIDTH, 18.0 * MM, 31.0 * MM, 14.0 * MM)


def _blower_intake(d, x, cy, cz, r):
    def on_side(pts, dx, color, **kw):
        d.facet([(x + dx, y, z) for y, z in pts], color, **kw)

    on_side(_circle(cy, cz, r * 1.08, 28), 0.0006, "#2e2f34", amb=0.8, dif=0.4, spec=0.1)
    on_side(_circle(cy, cz, r, 28), 0.0012, DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        for i in range(18):
            t = 2 * math.pi * i / 18
            blade = [(cy + r * k * math.cos(t + a), cz + r * k * math.sin(t + a)) for k, a in ((0.62, 0.0), (0.94, 0.12), (0.94, 0.24), (0.62, 0.1))]
            on_side(blade, 0.0018, "#34353a", amb=0.9, dif=0.3, spec=0.0)
    on_side(_circle(cy, cz, r * 0.55, 20), 0.0024, "#1f2023", amb=0.9, dif=0.35, spec=0.05)


# The cap's right half, bottom centre to top centre: each pair is a corner of its flat front and the corner
# of its outline that the chamfer slopes back to.
CAP_FRONT_TO_EDGE = [
    ((0.0, CAP_BOTTOM), (0.0, CAP_BOTTOM)),
    ((25.0 * MM, CAP_BOTTOM), (HALF_WIDTH, CAP_BOTTOM)),
    ((25.0 * MM, CAP_SHOULDER - 2.0 * MM), (HALF_WIDTH, CAP_SHOULDER)),
    ((17.0 * MM, CAP_SHOULDER), (21.0 * MM, CAP_TOP)),
    ((0.0, CAP_SHOULDER), (0.0, CAP_TOP)),
]


def _cap(d):
    """The cap over the blowers: a flat front, chamfered back along its top and sides, then straight to its back."""
    pairs = CAP_FRONT_TO_EDGE + [((-f[0], f[1]), (-e[0], e[1])) for f, e in reversed(CAP_FRONT_TO_EDGE) if f[0] != 0]
    d.solid([e for _, e in pairs], CHAMFER, CAP_DEPTH, BODY)
    for (f0, e0), (f1, e1) in zip(pairs, pairs[1:] + pairs[:1]):
        if f0 != e0 or f1 != e1:
            d.facet([(f0[0], 0.0, f0[1]), (f1[0], 0.0, f1[1]), (e1[0], CHAMFER, e1[1]), (e0[0], CHAMFER, e0[1])], BODY)
    d.face([f for f, _ in pairs], 0.0, BODY)
    for x in (-12.3 * MM, 12.3 * MM):
        d.disc(x, 54.5 * MM, 2.2 * MM, -0.001, SCREW, sides=14)
        if d.detail:
            d.face(_hexagon(x, 54.5 * MM, 1.0 * MM), -0.002, "#55575c")
    _voron_mark(d, 0.0, 56.5 * MM, 3.3 * MM, -0.001, "#f0f0f2")


def _front_plate(d):
    plate = [(-PLATE_HALF, PLATE_BOTTOM), (PLATE_HALF, PLATE_BOTTOM), (PLATE_HALF, CAP_BOTTOM + 0.5 * MM), (-PLATE_HALF, CAP_BOTTOM + 0.5 * MM)]
    d.solid(plate, 0.0, 3.0 * MM, BODY)
    # The fan sits a little back in a round bore.
    d.disc(0.0, FAN_Z, FAN_R, -0.0005, "#7d1a1e", sides=36, amb=0.9, dif=0.2, spec=0.0)
    d.fan(1.0 * MM, FAN_Z + 0.8 * MM, FAN_R * 0.9, -0.001, blade="#2a2b2f")


def _mounts(d):
    """The hotend mount and the extruder mount, showing above the blower behind the cap."""
    d.solid([(-HALF_WIDTH, HOTEND_MOUNT_BOTTOM), (HALF_WIDTH, HOTEND_MOUNT_BOTTOM), (HALF_WIDTH, EXTRUDER_MOUNT_BOTTOM), (-HALF_WIDTH, EXTRUDER_MOUNT_BOTTOM)], MOUNT_FRONT, MOUNT_BACK - 1.0 * MM, BODY)
    d.solid(rounded([(-26.0 * MM, EXTRUDER_MOUNT_BOTTOM), (26.0 * MM, EXTRUDER_MOUNT_BOTTOM), (26.0 * MM, DECK), (-26.0 * MM, DECK)], 1.5 * MM), MOUNT_FRONT, MOUNT_BACK, MOUNT)


def _axial_cylinder(d, cx, cz, r, y0, y1, color, n=28):
    """A cylinder on a front-to-back axis: only the strips that face the camera, its back hidden."""
    for i in range(n):
        t0, t1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
        mid = (t0 + t1) / 2
        if math.cos(mid) * d.camera.view[0] + math.sin(mid) * d.camera.view[2] <= 0:
            continue
        pts = [(cx + r * math.cos(t), cz + r * math.sin(t)) for t in (t0, t1)]
        d.facet([(pts[0][0], y0, pts[0][1]), (pts[1][0], y0, pts[1][1]), (pts[1][0], y1, pts[1][1]), (pts[0][0], y1, pts[0][1])], color)


def _mm(points):
    return [(x * MM, z * MM) for x, z in points]


# Front views of the Galileo 2's parts, in millimetres, read off its STEP on the Dragon Burner mount.
G2_LEFT_SIDE = [(-15.5, 108.4), (-21.1, 105.2), (-24.3, 101.4), (-27.0, 96.7), (-28.5, 90.9), (-30.2, 84.2), (-31.2, 80.0), (-31.2, 74.0), (-29.5, 70.0)]
MAIN_BODY = [(-28.0, 67.4), (26.0, 67.4), (26.0, 74.6), (15.0, 76.0), (15.0, 102.0), (13.3, 104.5), (13.3, 108.4)] + G2_LEFT_SIDE
REAR_PLATE = [(-29.5, 67.4), (14.8, 67.4), (14.8, 108.4)] + G2_LEFT_SIDE[:-1]
FRONT_PLATE = [(-27.0, 67.4), (23.6, 67.4), (23.6, 72.8), (21.5, 74.2), (13.5, 74.5), (13.3, 108.2), (-6.6, 108.2), (-14.6, 100.5), (-18.0, 92.0), (-18.0, 84.0), (-15.5, 77.5), (-16.5, 75.0), (-25.0, 74.5), (-27.0, 71.0)]
TENSIONER_ARM = [(7.8, 98.0), (13.3, 98.0), (13.3, 113.8), (11.5, 114.8), (9.5, 114.4), (7.8, 112.0)]


def _extruder(d):
    """A Galileo 2 standalone: knob, front plate, body, rear plate and motor stacked front to back on one axis."""
    ax, az = G2_AXIS
    # Back to front: the motor and its ring gear, then the printed stack.
    _axial_cylinder(d, ax, az, MOTOR_R, *MOTOR_Y, "#2e2f33")
    if d.detail:
        # The motor's laminations read as a lighter band round its middle.
        _axial_cylinder(d, ax, az, MOTOR_R + 0.3 * MM, 50.0 * MM, 58.0 * MM, "#3a3b40")
    _axial_cylinder(d, ax, az, RING_GEAR_R, *RING_GEAR_Y, "#232427")
    d.solid(rounded(_mm(REAR_PLATE), 1.0 * MM), *REAR_PLATE_Y, BODY)
    d.solid(rounded(_mm(MAIN_BODY), 1.0 * MM), *MAIN_BODY_Y, EXTRUDER)
    d.collet(0.0, COLLET_Y, G2_TOP, color="#1c2452")
    d.solid(rounded(_mm(FRONT_PLATE), 1.2 * MM), *FRONT_PLATE_Y, BODY, bevel=0.8 * MM)
    d.solid(rounded(_mm(TENSIONER_ARM), 0.8 * MM), 11.3 * MM, 28.8 * MM, BODY, bevel=0.6 * MM)
    # The tension spring and its thumb screw, lying across the top right.
    _cylinder_x(d, 18.3 * MM, 105.0 * MM, 3.4 * MM, 13.3 * MM, 23.0 * MM, "#b9bbc0", "#b9bbc0", n=10)
    _cylinder_x(d, 18.3 * MM, 105.0 * MM, 6.0 * MM, 23.0 * MM, 25.8 * MM, KNOB, "#25262a", n=18)
    # The feed knob, on the gearbox's output at the front.
    _cylinder_y(d, ax, az, KNOB_R, KNOB_FRONT, FRONT_PLATE_Y[0], KNOB, KNOB, n=24)
    d.face(_hexagon(ax, az, 5.6 * MM, math.pi / 6), KNOB_FRONT - 0.0005, "#2b2c30")
    if d.detail:
        for x, z in ((-9.6, 72.0), (-0.5, 104.5), (4.8, 77.5)):
            d.disc(x * MM, z * MM, 2.0 * MM, FRONT_PLATE_Y[0] - 0.0005, DARK, sides=12)
        d.disc(5.0 * MM, 90.0 * MM, 1.4 * MM, FRONT_PLATE_Y[0] - 0.0005, "#55575c", sides=12)


def draw_nozzle_tip(d, tip):
    """Only the brass tip, below the duct feet's underside."""
    d.nozzle(tip[0], tip[1], 0.0, r_top=2.6 * MM, r_tip=0.8 * MM, z_tip=tip[2])


def draw_cowl(d, front_plate=True):
    """Duct feet, blowers, the front plate round the hotend fan, and the cap; a head that covers the middle skips the plate."""
    _duct(d, -1)
    _blower(d, -1)
    if front_plate:
        _front_plate(d)
    _duct(d, 1)
    _blower(d, 1)
    _cap(d)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    draw_nozzle_tip(d, TIP)
    _mounts(d)
    draw_cowl(d)
    _extruder(d)


HEAD = HeadDef("dragonburner", "DRAGONBURNER", draw_art, TIP, entry=(0.0, COLLET_Y, G2_TOP + 0.035), size=1.29)
