# SPDX-License-Identifier: GPL-3.0-or-later
"""Bambu Lab: a near-cubic white shell with a soft inset panel, a fan, and a notched black skirt."""
import math

from .. import kit
from ..head import Drawing, HeadDef, mirror, rounded

WHITE, PANEL, DARK, HOLE = "#e6e6e3", "#d6d6d3", "#26272a", "#121316"
HALF_WIDTH = 0.283
DEPTH = 0.52
SKIRT_TOP = 0.234
TIP = (0.0, 0.0, 0.012)
SKIRT = mirror([(0.0, 0.115), (0.15, 0.115), (0.175, 0.02), (0.255, 0.02), (HALF_WIDTH, 0.06), (HALF_WIDTH, SKIRT_TOP), (0.0, SKIRT_TOP)])
TOP = 0.975
BLEND_HEIGHT = 0.125
SLOPE_DEPTH = 0.13
STEPS = 7


def _top_window(d):
    """The opening in the top of the shell where the filament enters."""
    z = 0.976
    outline = rounded([(-0.15, SLOPE_DEPTH + 0.02), (0.15, SLOPE_DEPTH + 0.02), (0.15, 0.38), (-0.15, 0.38)], 0.03)
    d.scene.poly([(x, y, z) for x, y in outline], HOLE)
    d.tube(0.02, 0.26, z, z + 0.05, 0.022, "#f2f2ef")


def _point(u, v, side):
    """A point on the blended corner: u sweeps front to flank, v sweeps up from the wall to the top."""
    k = math.sin(v)
    rx, ry = 0.02 + 0.05 * k, 0.02 + 0.11 * k
    return (
        side * (HALF_WIDTH - rx * (1 - math.sin(u) * math.cos(v))),
        ry * (1 - math.cos(u) * math.cos(v)),
        TOP - BLEND_HEIGHT + BLEND_HEIGHT * k,
    )


def _quad(d, q):
    """One outward patch of the shell, skipped when it faces away from the camera."""
    a, b, c = q[0], q[1], q[3]
    u, v = [y - x for x, y in zip(a, b)], [y - x for x, y in zip(a, c)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    if abs(kit.dot(n, n)) > 1e-12:
        d.facet(q, WHITE)


def _shell(d):
    """The white shell: flat walls whose front leans back and blends round into the flanks and top."""
    us = [math.pi / 2 * i / STEPS for i in range(STEPS + 1)]
    vs = us
    wall = TOP - BLEND_HEIGHT
    # Flats first, then the blended edges and corners over their seams.
    d.facet([(HALF_WIDTH, 0.02, SKIRT_TOP), (HALF_WIDTH, DEPTH, SKIRT_TOP), (HALF_WIDTH, DEPTH, wall), (HALF_WIDTH, 0.02, wall)], WHITE)
    d.facet([(-0.23, 0.13, TOP), (0.23, 0.13, TOP), (0.23, DEPTH, TOP), (-0.23, DEPTH, TOP)], WHITE)
    for v0, v1 in zip(vs, vs[1:]):
        a0, a1 = _point(math.pi / 2, v0, 1), _point(math.pi / 2, v1, 1)
        _quad(d, [a0, (a0[0], DEPTH, a0[2]), (a1[0], DEPTH, a1[2]), a1])
    d.face([(-HALF_WIDTH + 0.02, SKIRT_TOP), (HALF_WIDTH - 0.02, SKIRT_TOP), (HALF_WIDTH - 0.02, wall), (-HALF_WIDTH + 0.02, wall)], 0.0, WHITE)
    for v0, v1 in zip(vs, vs[1:]):
        _quad(d, [_point(0, v0, -1), _point(0, v0, 1), _point(0, v1, 1), _point(0, v1, -1)])
    for side in (1, -1):
        for u0, u1 in zip(us, us[1:]):
            p0, p1 = _point(u0, 0, side), _point(u1, 0, side)
            _quad(d, [(p0[0], p0[1], SKIRT_TOP), (p1[0], p1[1], SKIRT_TOP), p1, p0])
            for v0, v1 in zip(vs, vs[1:]):
                _quad(d, [_point(u0, v0, side), _point(u1, v0, side), _point(u1, v1, side), _point(u0, v1, side)])


def _side_slot(d):
    x = HALF_WIDTH + 0.001
    d.facet([(x, 0.1, 0.42), (x, 0.145, 0.42), (x, 0.145, 0.8), (x, 0.1, 0.8)], "#2a2b2e", amb=0.9, dif=0.3, spec=0.0)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid(SKIRT, 0.0, DEPTH, DARK, bevel=0.012)
    _shell(d)
    d.face(rounded([(-0.24, SKIRT_TOP + 0.02), (0.24, SKIRT_TOP + 0.02), (0.24, 0.84), (-0.24, 0.84)], 0.05), -0.001, PANEL)
    # A blower seen from its inlet: a light rim, a dark throat, and a large smooth hub.
    d.disc(0.0, 0.49, 0.195, -0.002, "#b9b9b6")
    d.disc(0.0, 0.49, 0.168, -0.003, HOLE, amb=0.9, dif=0.2, spec=0.0)
    d.disc(0.0, 0.49, 0.108, -0.004, "#2b2c30", amb=0.8, dif=0.45, spec=0.2)
    _top_window(d)
    if d.detail:
        _side_slot(d)
    # The hotend in the notch: a dark sock with a short brass collar and the tip just below the feet.
    d.face([(-0.062, 0.05), (0.062, 0.05), (0.062, 0.115), (-0.062, 0.115)], -0.001, "#17181b")
    d.face([(-0.02, 0.034), (0.02, 0.034), (0.02, 0.05), (-0.02, 0.05)], -0.001, "#a8842f")
    d.face([(-0.014, 0.034), (0.014, 0.034), (0.004, TIP[2]), (-0.004, TIP[2])], -0.001, "#ffffff", lane=0)


HEAD = HeadDef("bambu", "BAMBU", draw_art, TIP)
