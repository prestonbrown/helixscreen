# SPDX-License-Identifier: GPL-3.0-or-later
"""Solids, faces and fans shared by every toolhead drawing.

A head's shell starts near z = 0 and is about 1 tall, with the nozzle tip just below it. x runs across the face and y
into the part, so y = 0 is the front plane.
"""
import math
from dataclasses import dataclass
from typing import Callable, Tuple

from . import kit


# The tip hangs this far below z = 0, so it clears the shell's front lip at the shared camera.
NOZZLE_DROP = -0.07


@dataclass
class HeadDef:
    id: str
    style: str
    draw_art: Callable
    tip: Tuple[float, float, float]


@dataclass
class HeadFrame:
    aspect: float
    tip_x: float
    tip_y: float


def head_frame(scene, tip):
    """The art's width over height, and where the nozzle tip falls in it as shares of each."""
    vx, vy, vw, vh = scene._viewbox()
    x, y = scene.camera.project(tip)
    return HeadFrame(vw / vh, (x - vx) / vw, (y - vy) / vh)


def mirror(right):
    """A symmetric outline from its right half, listed bottom to top."""
    return right + [(-x, z) for x, z in reversed(right) if x != 0]


def _ccw(outline):
    area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(outline, outline[1:] + outline[:1]))
    return outline if area > 0 else outline[::-1]


def _inset(outline, d):
    """Each corner moved inward along its bisector."""
    out = []
    n = len(outline)
    for i, p in enumerate(outline):
        a, b = outline[i - 1], outline[(i + 1) % n]
        e0, e1 = kit.norm((p[0] - a[0], p[1] - a[1])), kit.norm((b[0] - p[0], b[1] - p[1]))
        # Inward normal of a counter-clockwise outline.
        n0, n1 = (-e0[1], e0[0]), (-e1[1], e1[0])
        mx, my = kit.norm((n0[0] + n1[0] + 1e-9, n0[1] + n1[1]))
        k = 1.0 / max(0.5, mx * n0[0] + my * n0[1])
        out.append((p[0] + mx * d * k, p[1] + my * d * k))
    return out


class Drawing:
    """One head's drawing calls; detail is False on the small tier."""

    def __init__(self, scene, tier):
        self.scene, self.detail, self.camera = scene, tier == "full", scene.camera
        # The frame hugs the drawing so its size and bounds are the same thing.
        scene.margin = 0.004

    def facet(self, pts, color, lane=None, amb=0.62, dif=0.55, spec=0.18):
        a, b, c = pts[0], pts[1], pts[2]
        u = tuple(q - p for p, q in zip(a, b))
        v = tuple(q - p for p, q in zip(a, c))
        n = kit.norm((u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]))
        if kit.dot(n, self.camera.view) < 0:
            n = tuple(-x for x in n)
        self.scene.poly(pts, kit.shade(self.camera, color, n, amb=amb, dif=dif, spec=spec), lane=lane)

    def face(self, outline, y, color, **kw):
        self.facet([(x, y, z) for x, z in outline], color, **kw)

    def solid(self, outline, y0, y1, color, bevel=0.0, **kw):
        """An outline extruded from y0 back to y1, its front edge chamfered by bevel."""
        outline = _ccw(outline)
        quads = []
        for a, b in zip(outline, outline[1:] + outline[:1]):
            quads.append([(a[0], y0 + bevel, a[1]), (b[0], y0 + bevel, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])])
        if bevel:
            inner = _inset(outline, bevel)
            for a, b, ia, ib in zip(outline, outline[1:] + outline[:1], inner, inner[1:] + inner[:1]):
                quads.append([(ia[0], y0, ia[1]), (ib[0], y0, ib[1]), (b[0], y0 + bevel, b[1]), (a[0], y0 + bevel, a[1])])
        else:
            inner = outline
        visible = []
        for q in quads:
            u = tuple(b - a for a, b in zip(q[0], q[1]))
            v = tuple(b - a for a, b in zip(q[0], q[3]))
            n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            if kit.dot(n, self.camera.view) < 0:
                visible.append(q)
        visible.sort(key=lambda q: sum(kit.dot(p, self.camera.view) for p in q))
        for q in visible:
            self.facet(q, color, **kw)
        self.face(inner, y0, color, **kw)

    def disc(self, cx, cz, r, y, color, sides=32, **kw):
        self.face(circle(cx, cz, r, sides), y, color, **kw)

    def fan(self, cx, cz, r, y, ring="#17181a", blade="#2c2d30", hub="#1f2022"):
        """A fan seen head on: a dark well, blades on the large tier, and a hub."""
        self.disc(cx, cz, r, y, ring, amb=0.9, dif=0.2, spec=0.0)
        if self.detail:
            for i in range(9):
                t = 2 * math.pi * i / 9
                pts = []
                for k, s in ((0.32, 0.0), (0.9, 0.34), (0.9, 0.62), (0.32, 0.3)):
                    pts.append((cx + r * k * math.cos(t + s), y - 0.001, cz + r * k * math.sin(t + s)))
                self.facet(pts, blade, amb=0.9, dif=0.2, spec=0.0)
        self.disc(cx, cz, r * 0.36, y - 0.002, hub, amb=0.9, dif=0.4)

    def tube(self, cx, cy, z0, z1, r, color, sides=14):
        """An upright cylinder: its camera-facing strips, then its top."""
        foot = circle(cx, cy, r, sides)[::-1]
        self.scene.prism(foot, z0, z1, color, amb=0.6, dif=0.55, spec=0.3)
        self.scene.poly([(x, y, z1) for x, y in foot], kit.shift(color, 26))

    def nozzle(self, x, y, z_top, r_top=0.05, r_tip=0.016, block=None, z_tip=NOZZLE_DROP):
        """The nozzle: a metal shoulder over a tip the runtime tints with the loaded filament."""
        if block:
            self.tube(x, y, z_top, z_top + 0.05, r_top * 1.25, block)
        n = 12
        for i in range(n):
            t0, t1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
            pts = [(x + r_top * math.cos(t), y + r_top * math.sin(t), z_top) for t in (t0, t1)]
            pts += [(x + r_tip * math.cos(t), y + r_tip * math.sin(t), z_tip) for t in (t1, t0)]
            mid = (t0 + t1) / 2
            nrm = kit.norm((math.cos(mid), math.sin(mid), 0.25))
            if kit.dot(nrm, self.camera.view) > 0:
                self.scene.poly(pts, kit.shade(self.camera, "#ffffff", nrm, amb=0.62, dif=0.45, spec=0.3), lane=0)


def circle(cx, cz, r, sides=32):
    return [(cx + r * math.cos(2 * math.pi * i / sides), cz + r * math.sin(2 * math.pi * i / sides)) for i in range(sides)]


def rounded(outline, r, steps=5):
    """Round every corner of a closed outline, keeping each arc within its share of the shorter edge."""
    n = len(outline)
    radii = []
    for i, p in enumerate(outline):
        shorter = min(math.dist(p, outline[i - 1]), math.dist(p, outline[(i + 1) % n]))
        radii.append(min(r, shorter * 0.45))
    return kit.fillet(outline, radii, closed=True, steps=steps)
