# SPDX-License-Identifier: GPL-3.0-or-later
"""Jabberwocky: an orange fan box under a grey core, with a motor and cable plate behind."""
import math

from ..head import Drawing, HeadDef, rounded

ORANGE, GREY, BLUE, DARK, GLOW = "#e8892b", "#a9acb0", "#2f8fd0", "#17181b", "#a9dfae"
HALF_WIDTH = 0.19
DEPTH = 0.3
BOX_TOP = 0.5
TIP = (0.0, 0.05, 0.004)


def _ring(cx, cz, r, n=24):
    return [(cx + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def _rear(d):
    """Behind the core: the orange plate that carries the cable bundle, then the motor in front of it."""
    plate = [(-0.07, 0.5), (0.1, 0.5), (0.19, 0.6), (0.19, 0.72), (0.11, 0.86), (0.05, 0.89), (0.05, 1.03), (-0.02, 1.03), (-0.02, 0.89), (-0.09, 0.85), (-0.16, 0.72), (-0.16, 0.6)]
    d.solid(rounded(plate, 0.012), 0.4, 0.44, ORANGE)
    d.solid([(-0.05, 1.0), (0.08, 1.0), (0.08, 1.13), (-0.05, 1.13)], 0.38, 0.46, "#2b2c30")
    cx, cz, r, y0, y1 = 0.02, 0.68, 0.15, 0.28, 0.4
    ring = _ring(cx, cz, r, 28)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        d.facet([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])], "#2b2c30")


def _core(d):
    """The grey core: wide at its base, stepping in to a narrow top, with its lit window."""
    outline = [(-0.17, BOX_TOP), (0.15, BOX_TOP), (0.15, 0.575), (0.098, 0.655), (0.098, 0.785), (0.06, 0.825), (-0.07, 0.825), (-0.11, 0.785), (-0.11, 0.655), (-0.205, 0.59)]
    d.solid(outline, 0.0, 0.3, GREY, bevel=0.012)
    window = rounded([(-0.04, 0.53), (0.055, 0.53), (0.055, 0.745), (0.0, 0.775), (-0.04, 0.745)], 0.012)
    d.face(window, -0.001, GLOW)
    # The maker mark: three prongs rising from a shallow V.
    for x0, x1, top in ((-0.024, -0.012, 0.65), (0.002, 0.014, 0.695), (0.028, 0.04, 0.65)):
        d.face([(x0, 0.615), (x1, 0.615), (x1, top), (x0, top)], -0.002, BLUE)
    d.face([(-0.032, 0.625), (0.048, 0.625), (0.048, 0.61), (0.008, 0.575), (-0.032, 0.61)], -0.002, BLUE)
    # The PTFE tube rising from a dark fitting, and the idler's blue lever down the right.
    d.tube(0.0, 0.15, 0.825, 0.875, 0.022, DARK)
    d.tube(0.0, 0.15, 0.875, 1.03, 0.007, "#1f8f88", sides=8)
    # The idler arm up the right flank, its spring and black knob standing out to the side.
    d.solid([(0.098, 0.6), (0.135, 0.6), (0.135, 0.8), (0.098, 0.83)], 0.06, 0.13, BLUE, bevel=0.004)
    for x0, x1, r, color in ((0.135, 0.175, 0.024, "#b9bbc0"), (0.175, 0.205, 0.04, DARK)):
        ring = _ring(0.095, 0.73, r, 14)
        for a, b in zip(ring, ring[1:] + ring[:1]):
            d.facet([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], color)
        d.facet([(x1, y, z) for y, z in ring], color)
    if d.detail:
        for x, z in ((-0.145, 0.535), (0.125, 0.535), (-0.145, 0.6)):
            d.disc(x, z, 0.018, -0.001, "#55575c", sides=12)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    _rear(d)
    # A blue tab behind the box's left edge.
    d.solid([(-0.23, 0.28), (-0.19, 0.28), (-0.19, 0.5), (-0.215, 0.5)], 0.05, 0.2, BLUE)
    d.face([(-0.012, 0.03), (0.012, 0.03), (0.003, TIP[2]), (-0.003, TIP[2])], 0.02, "#ffffff", lane=0)
    box = [(0.0, 0.05), (0.05, 0.05), (0.08, 0.02), (0.16, 0.02), (HALF_WIDTH, 0.07), (HALF_WIDTH, BOX_TOP), (-HALF_WIDTH, BOX_TOP), (-HALF_WIDTH, 0.07), (-0.16, 0.02), (-0.08, 0.02), (-0.05, 0.05)]
    d.solid(rounded(box, 0.012), 0.0, DEPTH, ORANGE, bevel=0.014)
    # The side blower's dark mouth, and the front fan's blue ring.
    d.facet([(HALF_WIDTH + 0.001, DEPTH * 0.52 + 0.075 * math.cos(2 * math.pi * i / 24), 0.25 + 0.115 * math.sin(2 * math.pi * i / 24)) for i in range(24)], DARK, amb=0.9, dif=0.2, spec=0.0)
    d.disc(-0.02, 0.255, 0.112, -0.001, BLUE, amb=0.8, dif=0.4)
    d.fan(-0.02, 0.255, 0.092, -0.002, ring="#141c2a", blade=BLUE, hub="#1b2a3f")
    if d.detail:
        for x in (-0.1, 0.1):
            d.disc(x, 0.43, 0.018, -0.001, "#c25e14", sides=12)
    _core(d)


HEAD = HeadDef("jabberwocky", "JABBERWOCKY", draw_art, TIP)
