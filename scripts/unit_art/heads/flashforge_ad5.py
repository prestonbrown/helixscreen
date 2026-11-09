# SPDX-License-Identifier: GPL-3.0-or-later
"""FlashForge AD5X and AD5M: a grey upright cover with a swirl-vented fan, a seam and an arched nozzle opening."""
import math

from ..head import Drawing, HeadDef, lofted_cover

SHELL, LOWER, DARK, BODY, HUB = "#84878c", "#75787d", "#101113", "#3a3b40", "#26272a"
HALF_WIDTH, COVER_DEPTH, BOTTOM, TOP = 0.27, 0.27, 0.05, 1.0
REAR_BOTTOM, REAR_BACK = 0.24, 0.46
FAN_Z, SEAM_Z = 0.63, 0.36
TOP_BEVEL = 0.075
HOTEND_Y = 0.09
TIP = (0.0, HOTEND_Y, -0.005)
ARCH_HALF_WIDTH, ARCH_SPRING = 0.105, 0.17


def _lean(z):
    """The face is flat, rolling back over a round bevel into the top."""
    t = max(0.0, (z - (TOP - TOP_BEVEL)) / TOP_BEVEL)
    return TOP_BEVEL * (1 - math.sqrt(max(0.0, 1 - t * t)))


def _half_width(z):
    """Straight sides, the top corners rounding in over a tall radius."""
    top = max(0.0, 1 - (TOP - z) / 0.09)
    foot = max(0.0, 1 - (z - BOTTOM) / 0.06)
    return HALF_WIDTH - 0.05 * top * top - 0.03 * foot * foot


def _arch(r):
    return [(-r, BOTTOM), (r, BOTTOM)] + [(r * math.cos(math.pi * i / 16), ARCH_SPRING + r * math.sin(math.pi * i / 16)) for i in range(17)]


def _seam_bow(d):
    """The seam dips at the centre of the face: the upper shell's colour fills down to its curve."""
    w = _half_width(SEAM_Z) - 0.06
    edge = [(w * (1 - 2 * i / 20), SEAM_Z - 0.03 * (1 - (1 - 2 * i / 20) ** 2)) for i in range(21)]
    d.face([(-w, SEAM_Z), (w, SEAM_Z)] + edge[1:-1], -0.001, SHELL)


def _fan(d):
    if d.detail:
        for i in range(44):
            t = 2 * math.pi * i / 44
            d.scene.line([((0.135 + 0.07 * k) * math.cos(t + 0.7 * k), -0.002, FAN_Z + (0.135 + 0.07 * k) * math.sin(t + 0.7 * k)) for k in (0.0, 0.25, 0.5, 0.75, 1.0)], "#4a4c50", 0.005)
    d.disc(0.0, FAN_Z, 0.125, -0.003, DARK, amb=0.9, dif=0.2, spec=0.0)
    d.disc(0.0, FAN_Z, 0.078, -0.004, HUB)


def _rear_body(d):
    """The toolhead behind the cover: it stops short of the cover's foot, leaving a cutaway under its back."""
    zs = [REAR_BOTTOM + (TOP - REAR_BOTTOM) * i / 40 for i in range(41)]
    right = [(_half_width(z), z) for z in zs]
    d.solid([(-right[0][0], right[0][1])] + right + [(-x, z) for x, z in reversed(right[1:])], COVER_DEPTH - 0.01, REAR_BACK, SHELL)
    d.solid([(-0.14, 0.05), (0.14, 0.05), (0.14, REAR_BOTTOM), (-0.14, REAR_BOTTOM)], COVER_DEPTH - 0.01, REAR_BACK - 0.08, BODY)


def _side_slots(d):
    x = HALF_WIDTH + 0.001
    for i in range(4):
        z = 0.56 - i * 0.075
        d.facet([(x, 0.34, z), (x, 0.4, z + 0.012), (x, 0.4, z + 0.05), (x, 0.34, z + 0.038)], DARK, amb=0.9, dif=0.2, spec=0.0)


def _through_arch(d, outline, y):
    """Where an outline on the plane at depth y appears through the arch: the cover's edge hides what passes behind it."""
    view = d.camera.view
    seen = []
    for (x0, z0), (x1, z1) in zip(outline, outline[1:] + outline[:1]):
        for i in range(12):
            t = i / 12
            x = x0 + (x1 - x0) * t - view[0] * y / view[1]
            z = z0 + (z1 - z0) * t - view[2] * y / view[1]
            lift = max(0.0, z - ARCH_SPRING)
            half = math.sqrt(max(0.0, ARCH_HALF_WIDTH ** 2 - lift ** 2))
            seen.append((max(-half, min(half, x)), min(ARCH_SPRING + ARCH_HALF_WIDTH, z)))
    return seen


def _hotend(d):
    """Deep in the arch: a black tapered nozzle body with only the tip below the cover."""
    d.nozzle(TIP[0], TIP[1], 0.032, r_top=0.018, r_tip=0.007, z_tip=TIP[2])
    body = [(-0.022, 0.03), (0.022, 0.03), (0.06, 0.09), (0.06, 0.245), (-0.06, 0.245), (-0.06, 0.09)]
    d.face(_through_arch(d, body, HOTEND_Y), -0.004, "#26272b")


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    _rear_body(d)
    lofted_cover(d, _lean, _half_width, 0.06, BOTTOM, SEAM_Z, COVER_DEPTH, LOWER, stations=32)
    lofted_cover(d, _lean, _half_width, 0.06, SEAM_Z, TOP, COVER_DEPTH, SHELL, stations=90)
    _seam_bow(d)
    d.face(_arch(ARCH_HALF_WIDTH + 0.018), -0.0015, "#65686d")
    d.face(_arch(ARCH_HALF_WIDTH), -0.002, DARK, amb=0.9, dif=0.2, spec=0.0)
    _hotend(d)
    _fan(d)
    _side_slots(d)
    d.collet(0.0, 0.3, TOP)


HEAD = HeadDef("flashforge_ad5", "FLASHFORGE_AD5", draw_art, TIP, entry=(0.0, 0.3, TOP + 0.035))
