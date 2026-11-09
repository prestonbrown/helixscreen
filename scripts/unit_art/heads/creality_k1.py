# SPDX-License-Identifier: GPL-3.0-or-later
"""Creality K1: a curved satin cover, flat around its fan, curving back above it and folding back below it."""
import math

from ..head import Drawing, HeadDef, lofted_cover

SHELL, RIM, HOLE = "#94969a", "#b9bbbf", "#111214"
DEPTH = 0.46
CORNER = 0.07
BODY_BACK = 0.72
BODY = "#26272b"
HALF_WIDTH = 0.33
CORNER_BEVEL = 0.035
BEVEL_HEIGHT = 0.07
FLAT_BOTTOM, FLAT_TOP, COVER_TOP = 0.21, 0.67, 0.95
FAN_Z = 0.44
TIP = (0.0, 0.24, 0.004)


def lean(z):
    """How far the face has fallen back at height z: nothing on the flat, a sharp fold below it, an easing curve above."""
    if z < FLAT_BOTTOM:
        return 0.14 * (FLAT_BOTTOM - z) / FLAT_BOTTOM
    if z > FLAT_TOP:
        t = (z - FLAT_TOP) / (COVER_TOP - FLAT_TOP)
        return 0.24 * t * t
    return 0.0


def half_width(z):
    """One width from bottom to top, easing in only at the gently bevelled corners."""
    edge = min(z - 0.02, COVER_TOP - z)
    return HALF_WIDTH - CORNER_BEVEL * max(0.0, 1 - edge / BEVEL_HEIGHT) ** 2


def _vent(d, pts):
    d.facet([(half_width(z) + 0.001, y, z) for y, z in pts], HOLE, amb=0.9, dif=0.2, spec=0.0)


def draw_body(d):
    """The hidden tip and the dark toolhead behind the cover, a little inside its outline."""
    d.face([(-0.016, 0.03), (0.016, 0.03), (0.004, TIP[2]), (-0.004, TIP[2])], TIP[1], "#ffffff", lane=0)
    d.solid([(-0.31, 0.25), (0.31, 0.25), (0.31, COVER_TOP), (-0.31, COVER_TOP)], DEPTH - 0.02, BODY_BACK, BODY)


def draw_cover(d, color, face_lean=lean):
    """The curved cover with its clean angular windows toward the back of the flank."""
    lofted_cover(d, face_lean, half_width, CORNER, 0.02, COVER_TOP, DEPTH, color)
    _vent(d, [(0.2, 0.78), (0.28, 0.7), (0.28, 0.42), (0.2, 0.5)])
    _vent(d, [(0.33, 0.8), (0.4, 0.8), (0.4, 0.16), (0.33, 0.16)])
    _vent(d, [(0.17, 0.33), (0.28, 0.33), (0.28, 0.14), (0.17, 0.14)])


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    draw_body(d)
    # The hole the PTFE tube enters, in the top of the body.
    for r, color, lift in ((0.055, "#3d3e43", 0.001), (0.034, HOLE, 0.002)):
        d.scene.poly([(r * math.cos(2 * math.pi * i / 20), 0.59 + r * math.sin(2 * math.pi * i / 20), COVER_TOP + lift) for i in range(20)], color)
    if d.detail:
        x = 0.311
        d.facet([(x, 0.53, 0.5), (x, 0.61, 0.5), (x, 0.61, 0.68), (x, 0.53, 0.68)], "#b08a3a")
        for y, z in ((0.52, 0.31), (0.64, 0.31), (0.64, 0.8)):
            d.facet([(x, y + 0.016 * math.cos(i * math.pi / 5), z + 0.016 * math.sin(i * math.pi / 5)) for i in range(10)], "#6b6d72")
    draw_cover(d, SHELL)
    # The blower, wholly on the flat of the face, in a slim turned ring.
    d.disc(0.0, FAN_Z, 0.215, -0.002, RIM)
    d.disc(0.0, FAN_Z, 0.186, -0.003, HOLE, amb=0.9, dif=0.2, spec=0.0)
    d.disc(0.0, FAN_Z, 0.11, -0.004, "#2b2c30", amb=0.8, dif=0.45, spec=0.2)


HEAD = HeadDef("creality_k1", "CREALITY_K1", draw_art, TIP)
