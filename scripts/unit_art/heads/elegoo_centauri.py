# SPDX-License-Identifier: GPL-3.0-or-later
"""Elegoo Centauri Carbon: a dark faceted cover with an inset panel, a round blower and a notched foot, over a matching rear body."""
import math

from ..head import Drawing, HeadDef, lofted_cover, rounded

SHELL, PANEL, DARK, MARK, FITTING = "#4b4d52", "#44464b", "#101113", "#e6e7e9", "#26272b"
HALF_WIDTH, COVER_DEPTH, BOTTOM, TOP = 0.3, 0.3, 0.05, 1.0
BACK = 0.56
SHOULDER_Z = 0.925
FOOT_BEVEL = 0.06
REAR_BOTTOM = 0.16
FAN_Z, LOGO_Z = 0.36, 0.64
TIP = (0.0, 0.06, 0.02)


def _lean(z):
    """Flat between a rounded bottom edge and the shoulder, then one plane sloping back into the top."""
    t = max(0.0, (BOTTOM + FOOT_BEVEL - z) / FOOT_BEVEL)
    return FOOT_BEVEL * (1 - math.sqrt(max(0.0, 1 - t * t))) + 0.1 * max(0.0, z - SHOULDER_Z) / (TOP - SHOULDER_Z)


def _half_width(z):
    """Straight sides whose top corners chamfer in above the shoulder."""
    return HALF_WIDTH - 0.045 * max(0.0, z - SHOULDER_Z) / (TOP - SHOULDER_Z)


def _on_face(d, outline, offset, color, **kw):
    d.facet([(x, _lean(z) - offset, z) for x, z in outline], color, **kw)


def _logo(d):
    """The looped mark, two rings side by side."""
    for cx in (-0.038, 0.038):
        ring = [(cx + 0.034 * math.cos(2 * math.pi * i / 20), -0.003, LOGO_Z + 0.034 * math.sin(2 * math.pi * i / 20)) for i in range(21)]
        d.scene.line(ring, MARK, 0.01)


def _fan(d):
    d.disc(0.0, FAN_Z, 0.13, -0.003, "#5a5c61")
    d.disc(0.0, FAN_Z, 0.11, -0.004, DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        for i in range(22):
            t = 2 * math.pi * i / 22
            d.scene.line([(r * math.cos(t), -0.005, FAN_Z + r * math.sin(t)) for r in (0.07, 0.105)], "#3a3b40", 0.005)
    d.disc(0.0, FAN_Z, 0.055, -0.006, "#3c3d42")
    d.disc(0.0, FAN_Z, 0.012, -0.007, "#8c8e92", sides=10)


def _rear_body(d):
    """The body behind the cover, the same dark grey, with the docking hardware on its flank."""
    zs = [REAR_BOTTOM + (TOP - REAR_BOTTOM) * i / 30 for i in range(31)]
    right = [(_half_width(z), z) for z in zs]
    d.solid([(-right[0][0], right[0][1])] + right + [(-x, z) for x, z in reversed(right[1:])], COVER_DEPTH - 0.01, BACK, SHELL)
    z = TOP + 0.001
    d.scene.poly([(x, y, z) for x, y in rounded([(-0.14, 0.34), (0.14, 0.34), (0.14, 0.5), (-0.14, 0.5)], 0.03)], DARK)
    d.tube(0.0, 0.42, z, z + 0.04, 0.05, FITTING)
    x = HALF_WIDTH + 0.001
    n = 16
    for cy, cz, r in ((0.345, 0.72, 0.045), (0.375, 0.31, 0.078)):
        d.facet([(x, cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)], DARK, amb=0.9, dif=0.2, spec=0.0)
    d.facet([(x, 0.375, 0.43), (x, 0.54, 0.43), (x, 0.54, 0.52), (x, 0.375, 0.52)], DARK, amb=0.9, dif=0.2, spec=0.0)
    # The filament cutter's lever stands out of the slot.
    d.solid(rounded([(x, 0.38), (x + 0.055, 0.38), (x + 0.055, 0.55), (x, 0.55)], 0.012), 0.295, 0.375, "#17181a")


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.nozzle(TIP[0], TIP[1], 0.06, r_top=0.022, r_tip=0.008, z_tip=TIP[2])
    _rear_body(d)
    lofted_cover(d, _lean, _half_width, 0.05, BOTTOM, TOP, COVER_DEPTH, SHELL, stations=140)
    _on_face(d, rounded([(-0.2, 0.2), (0.2, 0.2), (0.2, 0.82), (-0.2, 0.82)], 0.1), 0.001, PANEL)
    _on_face(d, rounded([(-0.09, BOTTOM), (0.09, BOTTOM), (0.075, 0.15), (-0.075, 0.15)], 0.015), 0.002, DARK, amb=0.9, dif=0.2, spec=0.0)
    _fan(d)
    if d.detail:
        _logo(d)
    else:
        d.face([(-0.06, LOGO_Z - 0.012), (0.06, LOGO_Z - 0.012), (0.06, LOGO_Z + 0.012), (-0.06, LOGO_Z + 0.012)], -0.002, MARK)


HEAD = HeadDef("elegoo_centauri", "ELEGOO_CENTAURI", draw_art, TIP, entry=(0.0, 0.42, TOP + 0.041))
