# SPDX-License-Identifier: GPL-3.0-or-later
"""Dragon Burner v8 with a Galileo 2 standalone extruder, measured from the v8 STEP assembly.

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
DECK = 67.5 * MM  # the extruder's foot
EXTRUDER_FRONT, EXTRUDER_BACK = 9.0 * MM, 37.0 * MM
COLLET_Y = 22.0 * MM
TIP = (0.0, 18.0 * MM, -4.0 * MM)


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


def _extruder(d):
    """A Galileo 2 standalone: a housing with the feed knob on its face, the tension screw on top and a pancake motor on its right."""
    z = DECK
    d.solid(rounded([(-23.0 * MM, z), (22.0 * MM, z), (22.0 * MM, z + 8.0 * MM), (-23.0 * MM, z + 8.0 * MM)], 2.0 * MM), EXTRUDER_FRONT - 1.5 * MM, EXTRUDER_BACK, EXTRUDER, bevel=1.0 * MM)
    housing = [(-21.0 * MM, z + 8.0 * MM), (12.0 * MM, z + 8.0 * MM), (12.0 * MM, z + 37.0 * MM), (8.5 * MM, z + 40.0 * MM), (-5.0 * MM, z + 40.0 * MM), (-21.0 * MM, z + 29.0 * MM)]
    d.solid(rounded(housing, 2.5 * MM), EXTRUDER_FRONT, EXTRUDER_BACK - 4.0 * MM, EXTRUDER, bevel=1.2 * MM)
    # The pancake motor on the right, its face toward the side.
    _cylinder_x(d, 23.0 * MM, z + 22.0 * MM, 17.0 * MM, 12.0 * MM, 22.0 * MM, "#26272b", "#202124", n=28)
    if d.detail:
        _cylinder_x(d, 23.0 * MM, z + 22.0 * MM, 12.0 * MM, 22.0 * MM, 22.6 * MM, "#2e2f33", "#2e2f33", n=20)
    # The tension screw and spring, across the top right, with its thumb knob.
    _cylinder_x(d, 15.0 * MM, z + 34.0 * MM, 2.6 * MM, 4.0 * MM, 13.0 * MM, "#b9bbc0", "#b9bbc0", n=10)
    _cylinder_x(d, 15.0 * MM, z + 34.0 * MM, 5.2 * MM, 13.0 * MM, 16.0 * MM, KNOB, "#25262a", n=16)
    d.collet(0.0, COLLET_Y, z + 40.0 * MM, color="#1c2452")
    # The feed knob on the gear shaft, at the face's lower left.
    _cylinder_y(d, -12.0 * MM, z + 19.0 * MM, 9.0 * MM, EXTRUDER_FRONT - 7.0 * MM, EXTRUDER_FRONT, KNOB, KNOB, n=20)
    d.face(_hexagon(-12.0 * MM, z + 19.0 * MM, 5.2 * MM, math.pi / 6), EXTRUDER_FRONT - 7.0 * MM - 0.0005, "#2b2c30")
    if d.detail:
        for x, zz in ((-15.0 * MM, z + 10.5 * MM), (-3.0 * MM, z + 10.5 * MM), (7.0 * MM, z + 10.5 * MM), (-6.0 * MM, z + 33.0 * MM)):
            d.disc(x, zz, 1.8 * MM, EXTRUDER_FRONT - 0.0005, DARK, sides=12)
        d.disc(3.0 * MM, z + 24.0 * MM, 1.6 * MM, EXTRUDER_FRONT - 0.0005, "#55575c", sides=12)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid(rounded([(-8.0 * MM, 1.0 * MM), (8.0 * MM, 1.0 * MM), (8.0 * MM, 9.0 * MM), (-8.0 * MM, 9.0 * MM)], 1.5 * MM), TIP[1] - 7.0 * MM, TIP[1] + 7.0 * MM, "#7a1f1c")
    d.nozzle(TIP[0], TIP[1], 1.0 * MM, r_top=3.0 * MM, r_tip=1.0 * MM, z_tip=TIP[2])
    _mounts(d)
    _duct(d, -1)
    _blower(d, -1)
    _front_plate(d)
    _duct(d, 1)
    _blower(d, 1)
    _cap(d)
    _extruder(d)


HEAD = HeadDef("dragonburner", "DRAGONBURNER", draw_art, TIP, entry=(0.0, COLLET_Y, DECK + 40.0 * MM + 0.035), size=1.29)
