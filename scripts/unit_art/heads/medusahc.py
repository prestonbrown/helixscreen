# SPDX-License-Identifier: GPL-3.0-or-later
"""MedusaHC printhead, measured from the MedusaHC V0.2 STEP assembly.

The carriage carries a Dragon Burner cowl with two 4010 blowers. The docked hotend tool slots into its middle
from the front: a grey plate round its 3010 fan, with a tall fin that carries the filament in at its top. The
shared feeder sits on the cowl: two dark plates over the drive gears, a round NEMA14 behind them in a grey
bracket, and the servo that opens the feeder on top. Sizes are in millimetres; z counts up from the duct feet's
underside, y back from the tool's front.
"""
import math

from ..head import Drawing, HeadDef, rounded
from . import dragonburner as db

MM = db.MM
TOOL, FEEDER_FRONT, FEEDER_BACK, COVER, SERVO, MOTOR, METAL = "#8f9095", "#3c3d42", "#2f3034", "#6a6b70", "#1a1b1e", "#3a3b40", "#b9bbc0"
# The STEP's z of the duct feet's underside; every height below is quoted from the STEP and moved by it.
FEET = 1.1
# The cowl sits this far behind the tool's front plate.
COWL_BACK = 5.2
MOTOR_AXIS, MOTOR_R, MOTOR_Y = (0.15, 97.7), 18.2, (44.4, 67.9)
FIN_X = (-3.2, 2.3)
# The fin in side view, (y, z): straight up from the plate's top, sloping back to the filament block.
FIN_PROFILE = [(0.0, 66.0), (31.2, 66.0), (31.2, 112.0), (22.2, 112.0), (12.0, 109.0), (0.0, 104.0)]
FILAMENT_BLOCK = ((-6.0, 5.5), (22.2, 33.5), (112.0, 125.3))
COLLET_Y = 27.8
TIP = db.TIP


def _pt(x, z):
    return (x * MM, (z - FEET) * MM)


def _outline(points):
    return [_pt(x, z) for x, z in points]


def _ys(y0, y1):
    return y0 * MM, y1 * MM


class _Behind(Drawing):
    """Draws everything a fixed distance further back, so the Dragon Burner cowl sits behind the tool."""

    def __init__(self, scene, tier, dy, dz):
        super().__init__(scene, tier)
        self.dy, self.dz = dy, dz

    def facet(self, pts, color, **kw):
        super().facet([(x, y + self.dy, z + self.dz) for x, y, z in pts], color, **kw)


def _slab_x(d, profile, x0, x1, color):
    """A plate standing across the head's depth: profile is its (y, z) outline, x0..x1 its thickness."""
    n = len(profile)
    area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(profile, profile[1:] + profile[:1]))
    strips = []
    for i in range(n):
        (ya, za), (yb, zb) = profile[i], profile[(i + 1) % n]
        # Outward normal of the edge in the (y, z) plane.
        ny, nz = (zb - za, -(yb - ya)) if area > 0 else (-(zb - za), yb - ya)
        if ny * d.camera.view[1] + nz * d.camera.view[2] > 0:
            strips.append([(x0, ya, za), (x1, ya, za), (x1, yb, zb), (x0, yb, zb)])
    strips.sort(key=lambda q: sum(p[0] * d.camera.view[0] + p[1] * d.camera.view[1] + p[2] * d.camera.view[2] for p in q))
    for q in strips:
        d.facet(q, color)
    d.facet([(x1, y, z) for y, z in profile], color)


def _cowl_top(d):
    """The cowl runs its full depth above the blowers, behind the cap."""
    d.solid(_outline([(-db.HALF_WIDTH / MM, 45.1), (db.HALF_WIDTH / MM, 45.1), (db.HALF_WIDTH / MM, 68.5), (-db.HALF_WIDTH / MM, 68.5)]), *_ys(19.2, 43.4), db.BODY)


def _feeder(d):
    ax, az = MOTOR_AXIS
    d.solid(rounded(_outline([(-17.5, 116.0), (17.5, 116.0), (17.5, 132.0), (-17.5, 132.0)]), 1.5 * MM), *_ys(49.0, 81.0), SERVO)
    db._axial_cylinder(d, ax * MM, (az - FEET) * MM, MOTOR_R * MM, *_ys(*MOTOR_Y), MOTOR)
    # The bracket that holds the servo arches over the motor.
    inner = [(ax + 18.6 * math.cos(math.pi * (12 - i) / 12), az + 18.6 * math.sin(math.pi * (12 - i) / 12)) for i in range(13)]
    outer = [(ax + 25.0 * math.cos(math.pi * i / 12), az + 25.0 * math.sin(math.pi * i / 12)) for i in range(13)]
    d.solid(_outline(outer + inner), *_ys(47.8, 63.6), COVER)
    d.face(_outline([(ax + MOTOR_R * math.cos(2 * math.pi * i / 28), az + MOTOR_R * math.sin(2 * math.pi * i / 28)) for i in range(28)]), MOTOR_Y[0] * MM, "#46474c")
    back = [(-25.1, 68.5), (28.9, 68.5), (28.9, 93.0), (36.1, 97.0), (36.1, 101.0), (33.5, 103.5), (-2.0, 103.5), (-20.4, 106.0), (-25.1, 101.0)]
    d.solid(rounded(_outline(back), 1.2 * MM), *_ys(40.5, 50.5), FEEDER_BACK)
    d.tube(32.0 * MM, 43.8 * MM, (99.3 - FEET) * MM, (122.4 - FEET) * MM, 2.4 * MM, METAL)
    lever = [(-20.1, 107.0), (5.2, 107.0), (5.2, 101.4), (16.2, 101.4), (18.4, 108.0), (35.7, 109.5), (35.7, 114.7), (-18.8, 114.7)]
    d.solid(rounded(_outline(lever), 1.0 * MM), *_ys(36.6, 40.0), COVER)
    d.solid(rounded(_outline([(-3.3, 113.5), (14.4, 113.5), (14.4, 118.0), (4.0, 131.2), (-3.3, 131.2)]), 1.0 * MM), *_ys(37.4, 41.0), COVER)
    front = [(-25.2, 68.6), (4.8, 68.6), (4.8, 79.0), (7.1, 88.0), (7.1, 97.0), (3.5, 106.0), (-20.9, 106.0), (-25.2, 101.0)]
    d.solid(rounded(_outline(front), 1.2 * MM), *_ys(19.3, 41.0), FEEDER_FRONT, bevel=0.6 * MM)
    d.solid(rounded(_outline([(2.3, 68.6), (28.6, 68.6), (28.6, 75.0), (10.0, 75.0), (7.3, 77.0), (4.6, 79.0), (2.3, 79.0)]), 0.8 * MM), *_ys(19.2, 25.3), COVER)
    # The idler's tension screw and spring, lying across the right.
    db._cylinder_x(d, 29.5 * MM, (92.7 - FEET) * MM, 3.7 * MM, 12.4 * MM, 26.4 * MM, METAL, METAL, n=10)
    db._cylinder_x(d, 29.5 * MM, (92.7 - FEET) * MM, 2.5 * MM, 26.4 * MM, 28.9 * MM, "#55575c", "#55575c", n=10)
    if d.detail:
        for x, z in ((-21.5, 72.0), (-21.5, 98.0), (1.0, 72.0)):
            d.disc(x * MM, (z - FEET) * MM, 1.8 * MM, 19.3 * MM - 0.0005, db.DARK, sides=12)


def _fin_hexes(d):
    """The hex windows through the fin, a staggered grid kept clear of its edges."""
    def top(y):
        return 104.0 + 5.0 * y / 12.0 if y < 12.0 else 109.0 + 3.0 * (y - 12.0) / 10.2

    for row, z in enumerate((72.5, 79.0, 85.5, 92.0, 98.5)):
        for y in ((4.6, 13.6, 22.6) if row % 2 == 0 else (9.1, 18.1, 27.1)):
            if y + 2.6 < 31.2 and z + 2.6 < top(y):
                hexagon = [(y + 2.4 * math.cos(math.pi * i / 3), z + 2.4 * math.sin(math.pi * i / 3)) for i in range(6)]
                d.facet([((FIN_X[1] + 0.05) * MM, hy * MM, (hz - FEET) * MM) for hy, hz in hexagon], db.DARK)


def _tool(d):
    plate = [(-13.0, 9.1), (13.0, 9.1), (16.5, 13.0), (16.5, 67.0), (-16.5, 67.0), (-16.5, 13.0)]
    d.solid(rounded(_outline(plate), 2.0 * MM), *_ys(0.0, COWL_BACK), TOOL, bevel=0.6 * MM)
    d.disc(0.0, (31.0 - FEET) * MM, 13.2 * MM, -0.0005, "#5f6065", sides=36, amb=0.9, dif=0.2, spec=0.0)
    d.fan(0.0, (31.5 - FEET) * MM, 12.2 * MM, -0.001, blade="#2a2b2f")
    for x in (-8.5, 8.5):
        for z in (15.3, 60.3):
            d.disc(x * MM, (z - FEET) * MM, 2.6 * MM, -0.001, db.SCREW, sides=12)
    _slab_x(d, [(y * MM, (z - FEET) * MM) for y, z in FIN_PROFILE], FIN_X[0] * MM, FIN_X[1] * MM, TOOL)
    if d.detail:
        _fin_hexes(d)
    (x0, x1), (y0, y1), (z0, z1) = FILAMENT_BLOCK
    d.solid(rounded(_outline([(x0, z0), (x1, z0), (x1, z1), (x0, z1)]), 1.0 * MM), *_ys(y0, y1), TOOL, bevel=0.5 * MM)
    d.collet(0.0, COLLET_Y * MM, (z1 - FEET) * MM, color="#26272b")


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    db.draw_nozzle_tip(d, TIP)
    _cowl_top(d)
    db.draw_cowl(_Behind(scene, tier, COWL_BACK * MM, 0.0), front_plate=False)
    _feeder(d)
    _tool(d)


HEAD = HeadDef("medusahc", "MEDUSAHC", draw_art, TIP, entry=(0.0, COLLET_Y * MM, (FILAMENT_BLOCK[2][1] - FEET) * MM + 0.035), size=1.29)
