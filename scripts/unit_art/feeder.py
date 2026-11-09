# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared parts of the feeder units: they hold no spools, so each bakes two looks.

The spool look stands 200 mm spools behind the body in the shared frame. The stub look draws
the body alone, large and framed to itself, with a tube per lane leaving its inlet.
"""
import math
from pathlib import Path

from . import kit, spool
from .kit import TrayShellDef

_REPO = Path(__file__).resolve().parents[2]
SPOOL_R = 0.25
SPOOL_PITCH = 0.2
TRAY_STRIP_PX = 14.0
TUBE_FULL, TUBE_THUMB = 0.02, 0.036


def box(sc, x0, x1, y0, y1, z0, z1, color):
    """A lit box; call boxes in rising x so nearer ones overdraw."""
    sc.prism([(x1, y0), (x1, y1), (x0, y1), (x0, y0)], z0, z1, color, amb=0.72, dif=0.5, spec=0.1, power=16)
    sc.poly([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], kit.shift(color, 16))


def solid(sc, profile, x0, x1, color):
    """Extrude a closed (y, z) side profile, listed front foot, up, over, down the back."""
    cam = sc.camera
    for a, b in zip(profile, profile[1:] + profile[:1]):
        d = (b[0] - a[0], b[1] - a[1])
        if d == (0, 0):
            continue
        n = kit.norm((0, -d[1], d[0]))
        if kit.dot(n, cam.view) > 0:
            fill = kit.shade(cam, color, n, amb=0.72, dif=0.5, spec=0.1, power=16)
            sc.poly([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], fill)
    sc.poly([(x1, y, z) for y, z in profile], kit.shade(cam, color, (1, 0, 0), amb=0.72, dif=0.5, spec=0))


def glass(sc, profile, x0, x1, color, opacity, edge):
    """A clear or smoked shell of a closed side profile, its camera-facing panes only."""
    cam = sc.camera
    for a, b in zip(profile, profile[1:] + profile[:1]):
        d = (b[0] - a[0], b[1] - a[1])
        if d == (0, 0):
            continue
        n = kit.norm((0, -d[1], d[0]))
        if kit.dot(n, cam.view) > 0:
            sc.poly([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], color, opacity)
    sc.poly([(x1, y, z) for y, z in profile], color, opacity * 0.8)
    sc.line([(x1, y, z) for y, z in profile + profile[:1]], edge, 0.0025, 0.75)
    y_front = min(y for y, _ in profile)
    z_top = max(z for _, z in profile)
    z_bottom = min(z for _, z in profile)
    sc.line([(x0, y_front, z_bottom), (x0, y_front, z_top), (x1, y_front, z_top)], edge, 0.0025, 0.55)


def rounded_profile(y0, y1, z0, z1, r):
    """A rounded (y, z) side profile in solid() order."""
    return kit.fillet([(y0, z0), (y0, z1), (y1, z1), (y1, z0)], [r] * 4, closed=True, steps=5)


def disc(sc, xc, y, zc, r, color, n=20):
    """A circle standing on a front plane."""
    sc.poly([(xc + r * math.cos(2 * math.pi * i / n), y, zc + r * math.sin(2 * math.pi * i / n)) for i in range(n)], color)


def bend(p0, ctrl, p1, n=14):
    """A quadratic bend through 3D points."""
    return [
        tuple((1 - t) ** 2 * a + 2 * (1 - t) * t * c + t * t * b for a, c, b in zip(p0, ctrl, p1))
        for t in (i / n for i in range(n + 1))
    ]


def tube(scene, pts, color, lane, width=0.02):
    """A filament tube: its lit core over a darker edge, both tintable."""
    if color:
        scene.line(pts, kit.scale(color, 0.62), width, lane=lane)
        scene.line(pts, color, width * 0.6, lane=lane)


def spool_row(scene, colors, cx, depth, detail):
    """Spools on the floor behind a body of the given depth, centred on cx."""
    c = spool.spool_constants(_REPO)
    n = len(colors)
    y = depth + SPOOL_R + 0.02
    x0, x1 = cx - SPOOL_PITCH * n / 2 - 0.02, cx + SPOOL_PITCH * n / 2 + 0.02
    scene.poly([(x0, 0.0, 0), (x1, 0.0, 0), (x1, y + 0.2, 0), (x0, y + 0.2, 0)], "#000", 0.5, blur=True)
    for lane, col in enumerate(colors):
        spool.draw_spool(scene, (cx + (lane - (n - 1) / 2) * SPOOL_PITCH, y, SPOOL_R), SPOOL_R, col, c, lane, detail=detail)


def looks(body, cx, depth, tubes):
    """The two draw functions of a feeder.

    body(sc, detail) draws the unit; tubes(scene, colors, width) draws its inlet tubes.
    """

    def spools(scene, colors, tier):
        spool_row(scene, colors, cx, depth, tier == "full")
        scene.layer = "front"
        body(scene, tier == "full")

    def stubs(scene, colors, tier):
        body(scene, tier == "full")
        tubes(scene, colors, TUBE_FULL if tier == "full" else TUBE_THUMB)

    return spools, stubs


def tray_strip(body_rgb, lane_rgb, **kw):
    """The detail screen shape of a feeder: a slim strip in the unit's colours."""
    return TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=[],
        back_extra_px=0.0,
        front_wall_px=TRAY_STRIP_PX,
        rail_px=2.0,
        cap_clearance_px=8.0,
        lid_spans_row=False,
        has_latch=False,
        body=body_rgb,
        panel=body_rgb,
        rail=body_rgb,
        accent=lane_rgb,
        front=[kit.span(1, 1, 1, TRAY_STRIP_PX - 1, body_rgb, 3), kit.lane(-14, 14, 4, TRAY_STRIP_PX - 4, lane_rgb, 2)],
        **kw,
    )
