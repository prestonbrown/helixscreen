# SPDX-License-Identifier: GPL-3.0-or-later
"""Creality K2: a flat narrow cover creased back below its vent, over a dark body that wraps the lower flank."""
import math

from ..head import Drawing, HeadDef, lofted_cover

SHELL, DARK, BODY, SOCK, MARK = "#7f8286", "#17181a", "#26272b", "#303136", "#e4e5e7"
HALF_WIDTH = 0.21
DEPTH = 0.3
BODY_BACK = 0.56
BOTTOM = 0.05
INLET_Y = 0.21
INLET_CUT_HALF_WIDTH, INLET_CUT_FRONT = 0.075, 0.13
TOP = 1.0
CREASE = 0.3
TIP = (0.0, 0.07, 0.022)
ARCH_HALF_WIDTH, ARCH_SPRING = 0.088, 0.175
SOCK_HALF_WIDTH = 0.062
SOCK_OUTLINE = [(-0.034, 0.058), (0.034, 0.058), (0.05, 0.075), (SOCK_HALF_WIDTH, 0.105)] + [
    (SOCK_HALF_WIDTH * math.cos(math.pi * i / 16), ARCH_SPRING + SOCK_HALF_WIDTH * math.sin(math.pi * i / 16)) for i in range(17)
] + [(-SOCK_HALF_WIDTH, 0.105), (-0.05, 0.075)]
FLANK_FRONT, FLANK_TOP, FLANK_CORNER = 0.14, 0.18, 0.04


def _lean(z):
    """How far the face sits back at z: flat down to the crease, then one plane angled back."""
    return 0.07 * max(0.0, CREASE - z) / (CREASE - BOTTOM)


def _half_width(z):
    edge = min(z - BOTTOM, TOP - z)
    return HALF_WIDTH - 0.025 * max(0.0, 1 - edge / 0.06) ** 2


def _on_face(d, outline, color, **kw):
    d.facet([(x, _lean(z) - 0.002, z) for x, z in outline], color, **kw)


def _arch():
    """The nozzle opening: straight sides from the bottom edge up to a round top."""
    r = ARCH_HALF_WIDTH
    top = [(r * math.cos(math.pi * i / 16), ARCH_SPRING + r * math.sin(math.pi * i / 16)) for i in range(17)]
    return [(-r, BOTTOM), (r, BOTTOM)] + top + [(-r, BOTTOM)]


def _mark(d):
    """The arrowhead: pale, its foot notched, hollowed by a smaller arrowhead on the large tier."""
    _on_face(d, [(0.0, 0.866), (0.068, 0.755), (0.0, 0.785), (-0.068, 0.755)], MARK)
    if d.detail:
        _on_face(d, [(0.0, 0.836), (0.032, 0.782), (0.0, 0.8), (-0.032, 0.782)], SHELL)


def _inlet(d):
    """The PTFE tube rises through a slot cut into the back of the cover's top."""
    w, y, z = INLET_CUT_HALF_WIDTH, INLET_CUT_FRONT, TOP + 0.001
    d.facet([(-w, y, z), (w, y, z), (w, DEPTH, z), (-w, DEPTH, z)], DARK, amb=0.9, dif=0.2, spec=0.0)
    d.tube(0.0, INLET_Y, TOP, TOP + 0.03, 0.04, "#2b2c30")


def _flank_block(d):
    """The dark body comes forward over the side of the cover, flush with its bottom edge."""
    x, y, z, c = HALF_WIDTH + 0.001, FLANK_FRONT, FLANK_TOP, FLANK_CORNER
    foot = _half_width(BOTTOM) + 0.001
    corner = [(y + c - c * math.cos(math.pi / 2 * i / 6), z - c + c * math.sin(math.pi / 2 * i / 6)) for i in range(7)]
    d.facet([(x, DEPTH, BOTTOM), (foot, y, BOTTOM)] + [(x, a, b) for a, b in corner] + [(x, DEPTH, z)], BODY)


def _side_holes(d):
    """A staggered field of round holes in the flank, above the dark block."""
    x = HALF_WIDTH + 0.001
    rows, cols, r = (14, 5, 0.0085) if d.detail else (6, 3, 0.016)
    for row in range(rows):
        z = 0.25 + 0.36 * row / (rows - 1)
        step = 0.15 / (cols - 1)
        for col in range(cols - (row % 2)):
            y = 0.095 + step * (col + 0.5 * (row % 2))
            ring = [(x, y + r * math.cos(math.pi * i / 4), z + r * math.sin(math.pi * i / 4)) for i in range(8)]
            d.facet(ring, DARK, amb=0.9, dif=0.2, spec=0.0)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid([(-HALF_WIDTH, BOTTOM), (HALF_WIDTH, BOTTOM), (HALF_WIDTH, TOP), (-HALF_WIDTH, TOP)], DEPTH, BODY_BACK, BODY)
    lofted_cover(d, _lean, _half_width, 0.05, BOTTOM, TOP, DEPTH, SHELL)
    _inlet(d)
    _on_face(d, _arch(), DARK, amb=0.9, dif=0.2, spec=0.0)
    d.nozzle(TIP[0], TIP[1], 0.07, r_top=0.022, r_tip=0.008, z_tip=TIP[2])
    _on_face(d, SOCK_OUTLINE, SOCK, amb=0.8, dif=0.4, spec=0.1)
    slats = 9 if d.detail else 5
    half = 0.008 if d.detail else 0.014
    for i in range(slats):
        x = -0.14 + 0.28 * i / (slats - 1)
        _on_face(d, [(x - half, 0.32), (x + half, 0.32), (x + half, 0.51), (x - half, 0.51)], DARK, amb=0.9, dif=0.2, spec=0.0)
    _mark(d)
    _flank_block(d)
    _side_holes(d)


HEAD = HeadDef("creality_k2", "CREALITY_K2", draw_art, TIP, entry=(0.0, INLET_Y, TOP + 0.03), size=1.2)
