# SPDX-License-Identifier: GPL-3.0-or-later
"""Mini Stealthburner: a blocky shell with a front fan and a side blower, the motor behind its top."""
import math

from ..head import Drawing, HeadDef

BODY, REAR, DARK = "#cf2b2b", "#2e2f32", "#141517"
COIL = "#2b2c30"
DEPTH = 0.33
SHOULDER_DEPTH = 0.06
TIP = (0.0, 0.1, 0.022)

# Right half, bottom centre to top centre: a point on the front plate and where it slopes back to.
PLATE_TO_EDGE = [
    ((0.0, 0.12), (0.0, 0.12)),
    ((0.07, 0.12), (0.07, 0.12)),
    ((0.105, 0.03), (0.1, 0.0)),
    ((0.25, 0.03), (0.275, 0.0)),
    ((0.29, 0.13), (0.315, 0.13)),
    ((0.28, 0.45), (0.31, 0.47)),
    ((0.18, 0.62), (0.225, 0.66)),
    ((0.165, 0.83), (0.205, 0.88)),
    ((0.09, 0.885), (0.12, 0.945)),
    ((0.0, 0.885), (0.0, 0.945)),
]


def _mirrored(pairs):
    return pairs + [((-p[0], p[1]), (-e[0], e[1])) for p, e in reversed(pairs) if p[0] != 0]


def _motor(d):
    """The stepper runs back from the top of the head: a dark wound cylinder."""
    cx, cz, r, y0, y1, n = 0.02, 0.75, 0.21, DEPTH - 0.02, 0.62, 20
    ring = [(cx + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])], COIL)
    if d.detail:
        for t in range(1, 9):
            y = y0 + (y1 - y0) * t / 9
            arc = [p for p in ring if p[1] >= cz - 0.02]
            d.scene.line([(x, y, z) for x, z in arc], "#4b4d52", 0.004)


def _side_blower(d, x):
    n, r, cy, cz = 24, 0.125, DEPTH / 2, 0.235
    ring = [(x + 0.001, cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    d.facet(ring, DARK, amb=0.9, dif=0.2, spec=0.0)
    hub = [(x + 0.002, cy + 0.045 * math.cos(2 * math.pi * i / n), cz + 0.045 * math.sin(2 * math.pi * i / n)) for i in range(n)]
    d.facet(hub, "#232427", amb=0.9, dif=0.35, spec=0.05)


def _pocket(d):
    """The open pocket on the right shoulder, the extruder knob standing out of it."""
    y0, y1 = 0.1, 0.3
    rim = [(0.297, y0 - 0.012, 0.47), (0.297, y1 + 0.012, 0.47), (0.232, y1 + 0.012, 0.735), (0.232, y0 - 0.012, 0.735)]
    d.facet(rim, "#a82020")
    d.facet([(0.296, y0, 0.49), (0.296, y1, 0.49), (0.233, y1, 0.72), (0.233, y0, 0.72)], DARK, amb=0.9, dif=0.2, spec=0.0)
    cy, cz, r, x0, x1, n = 0.2, 0.6, 0.058, 0.22, 0.282, 16
    ring = [(cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], "#2a2b2f")
    d.facet([(x1, y, z) for y, z in ring], "#1b1c1f")
    hexagon = [(x1 + 0.001, cy + 0.03 * math.cos(math.pi * i / 3), cz + 0.03 * math.sin(math.pi * i / 3)) for i in range(6)]
    d.facet(hexagon, "#6e7075")


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    _motor(d)
    ring = _mirrored(PLATE_TO_EDGE)
    outline = [e for _, e in ring]
    d.solid(outline, SHOULDER_DEPTH, DEPTH, BODY)
    d.tube(TIP[0], TIP[1], 0.06, 0.13, 0.05, "#17181a")
    d.nozzle(TIP[0], TIP[1], 0.06, r_top=0.022, r_tip=0.008, z_tip=TIP[2])
    for (p0, e0), (p1, e1) in zip(ring, ring[1:] + ring[:1]):
        if p0 != e0 or p1 != e1:
            d.facet([(p0[0], 0.0, p0[1]), (p1[0], 0.0, p1[1]), (e1[0], SHOULDER_DEPTH, e1[1]), (e0[0], SHOULDER_DEPTH, e0[1])], BODY)
    d.face([p for p, _ in ring], 0.0, BODY)
    _side_blower(d, 0.315)
    _pocket(d)
    d.fan(0.0, 0.3, 0.125, -0.002, blade="#232427")
    # The Voron mark: three slanted bars, the middle one longest.
    for dx, z0, z1 in ((-0.07, 0.66, 0.77), (0.0, 0.55, 0.77), (0.07, 0.55, 0.66)):
        slant, half = 0.46, 0.026
        d.face([(dx + (z - 0.65) * slant + w, z) for z, w in ((z0, -half), (z0, half), (z1, half), (z1, -half))], -0.001, DARK)
    if d.detail:
        for x, z in ((-0.105, 0.475), (0.105, 0.475)):
            d.disc(x, z, 0.024, -0.001, DARK, sides=12)


HEAD = HeadDef("mini_stealthburner", "MINI_STEALTHBURNER", draw_art, TIP)
