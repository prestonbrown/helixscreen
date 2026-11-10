# SPDX-License-Identifier: GPL-3.0-or-later
"""Voron Stealthburner: a faceted red shell with two fans, over a deep black rear cowl."""
from ..head import Drawing, HeadDef

BODY, REAR, SEAM, HOLE = "#c92a2a", "#303134", "#b9babd", "#141517"
FLOOR = "#101113"
BACK_OF_SHELL = 0.24

# Right half of the shell, bottom centre to top centre: each pair is a point on the flat front
# plate and the point it slopes back to on the silhouette.
PLATE_TO_EDGE = [
    ((0.0, 0.13), (0.0, 0.13)),
    ((0.065, 0.13), (0.065, 0.13)),
    ((0.1, 0.045), (0.095, 0.0)),
    ((0.19, 0.045), (0.235, 0.0)),
    ((0.215, 0.2), (0.295, 0.2)),
    ((0.15, 0.47), (0.225, 0.49)),
    ((0.205, 0.64), (0.275, 0.72)),
    ((0.205, 0.83), (0.265, 0.86)),
    ((0.12, 0.945), (0.17, 0.985)),
    ((0.0, 0.955), (0.0, 1.0)),
]
EDGE_DEPTH = 0.15
HEX = [(0.135, 0.75), (0.068, 0.87), (-0.068, 0.87), (-0.135, 0.75), (-0.068, 0.63), (0.068, 0.63)]
HEX_FLOOR_SCALE = 0.8
HEX_DEPTH = 0.09
HEX_Z = 0.75
TIP = (0.0, 0.1, 0.022)


def _mirrored(pairs):
    return [(p, e) for p, e in pairs] + [((-p[0], p[1]), (-e[0], e[1])) for p, e in reversed(pairs) if p[0] != 0]


def _shell_front(d):
    ring = _mirrored(PLATE_TO_EDGE)
    for (p0, e0), (p1, e1) in zip(ring, ring[1:] + ring[:1]):
        if p0 == e0 and p1 == e1:
            continue
        d.facet([(p0[0], 0.0, p0[1]), (p1[0], 0.0, p1[1]), (e1[0], EDGE_DEPTH, e1[1]), (e0[0], EDGE_DEPTH, e0[1])], BODY)
    d.face([p for p, _ in ring], 0.0, BODY)


def _hex_well(d):
    """The upper fan sits at the bottom of a tapered hexagonal well."""
    floor = [(x * HEX_FLOOR_SCALE, HEX_Z + (z - HEX_Z) * HEX_FLOOR_SCALE) for x, z in HEX]
    for (a, b), (fa, fb) in zip(zip(HEX, HEX[1:] + HEX[:1]), zip(floor, floor[1:] + floor[:1])):
        d.facet([(a[0], 0.0, a[1]), (b[0], 0.0, b[1]), (fb[0], HEX_DEPTH, fb[1]), (fa[0], HEX_DEPTH, fa[1])], BODY)
    d.face(floor, HEX_DEPTH, FLOOR, amb=0.9, dif=0.2, spec=0.0)
    d.disc(0.0, HEX_Z, 0.098, HEX_DEPTH - 0.002, "#232427", amb=0.9, dif=0.35, spec=0.05)


def _rear(d):
    """The black cowl seen past the shell: deep behind the extruder, stepping in below it."""
    upper = [(-0.21, 0.5), (0.21, 0.5), (0.26, 0.72), (0.25, 0.88), (0.15, 0.99), (-0.15, 0.99), (-0.25, 0.88), (-0.26, 0.72)]
    lower = [(-0.2, 0.06), (0.22, 0.06), (0.26, 0.2), (0.21, 0.5), (-0.21, 0.5), (-0.26, 0.2)]
    d.solid(lower, BACK_OF_SHELL, 0.46, REAR)
    d.solid(upper, BACK_OF_SHELL, 0.66, REAR)
    # Facets that break up the cowl's flank.
    d.facet([(0.26, 0.3, 0.72), (0.25, 0.3, 0.88), (0.24, 0.5, 0.86), (0.25, 0.5, 0.7)], "#4a4b4f")
    d.facet([(0.26, 0.28, 0.2), (0.215, 0.28, 0.48), (0.2, 0.42, 0.46), (0.24, 0.42, 0.22)], "#454649")


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    _rear(d)
    # The tensioner lever behind the shell's top left.
    d.solid([(-0.2, 0.9), (-0.27, 0.965), (-0.15, 0.96)], 0.12, 0.2, REAR)
    outline = [e for _, e in _mirrored(PLATE_TO_EDGE)]
    # The pale seam between the red shell and the black cowl.
    d.solid(outline, BACK_OF_SHELL - 0.004, BACK_OF_SHELL, SEAM)
    d.solid(outline, EDGE_DEPTH, BACK_OF_SHELL, BODY)
    # The black silicone sock, with only the very tip below it, level with the feet.
    d.tube(TIP[0], TIP[1], 0.06, 0.14, 0.052, "#17181a")
    d.nozzle(TIP[0], TIP[1], 0.06, r_top=0.022, r_tip=0.008, z_tip=TIP[2])
    _shell_front(d)
    _hex_well(d)
    d.fan(0.0, 0.3, 0.148, -0.002, blade="#232427")
    d.collet(0.0, BACK_OF_SHELL + 0.07, 0.99)
    # The Voron mark: three slanted bars, the middle one longest.
    for dx, z0, z1 in ((-0.032, 0.525, 0.565), (0.0, 0.48, 0.565), (0.032, 0.48, 0.52)):
        slant, half = 0.42, 0.011
        d.face([(dx + (z - 0.52) * slant + w, z) for z, w in ((z0, -half), (z0, half), (z1, half), (z1, -half))], -0.001, HOLE)
    if d.detail:
        for x, z in ((-0.15, 0.53), (0.15, 0.53), (-0.165, 0.1), (0.165, 0.1)):
            d.disc(x, z, 0.018, -0.001, HOLE, sides=12)


HEAD = HeadDef("stealthburner", "STEALTHBURNER", draw_art, TIP, entry=(0.0, BACK_OF_SHELL + 0.07, 1.025))
