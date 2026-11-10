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
    entry: Tuple[float, float, float] = (0.0, 0.0, 1.0)  # where the filament goes in
    size: float = 1.0  # relative real-world size, the motor-bearing heads matched to one motor


@dataclass
class HeadFrame:
    aspect: float
    tip_x: float
    tip_y: float
    world_height: float
    entry_x: float
    entry_y: float


def head_frame(scene, tip, entry=None):
    """The art's width over height, and where the nozzle tip and filament entry fall in it as shares of each."""
    vx, vy, vw, vh = scene._viewbox()
    x, y = scene.camera.project(tip)
    ex, ey = scene.camera.project(entry if entry is not None else tip)
    return HeadFrame(vw / vh, (x - vx) / vw, (y - vy) / vh, vh, (ex - vx) / vw, (ey - vy) / vh)


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


COLLET_HEIGHT = 0.035


class Drawing:
    """One head's drawing calls; detail is False on the small tier."""

    def __init__(self, scene, tier):
        self.scene, self.detail, self.camera = scene, tier == "full", scene.camera
        # The frame hugs the drawing so its size and bounds are the same thing.
        scene.margin = 0.004

    def facet(self, pts, color, lane=None, amb=0.62, dif=0.55, spec=0.18):
        # Densely sampled outlines can open with collinear points, so take the first corner that spans a plane.
        a, b = pts[0], pts[1]
        u = tuple(q - p for p, q in zip(a, b))
        cross = (0.0, 0.0, 0.0)
        for c in pts[2:]:
            v = tuple(q - p for p, q in zip(a, c))
            cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            if kit.dot(cross, cross) > 1e-14:
                break
        n = kit.norm(cross)
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
        foot = circle(cx, cy, r, sides)
        self.scene.prism(foot, z0, z1, color, amb=0.6, dif=0.55, spec=0.3)
        self.scene.poly([(x, y, z1) for x, y in foot], kit.shift(color, 26))

    def collet(self, x, y, z, color="#26272b"):
        """The fitting the filament tube enters, on a top surface at height z; the app draws the tube."""
        self.tube(x, y, z, z + COLLET_HEIGHT, 0.042, color)

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


def lofted_cover(d, lean, half_width, corner, z0, z1, depth, color, stations=48):
    """A cover whose face leans back by lean(z) and whose width is half_width(z), its corners rounded by corner."""

    def section(z):
        y, w = lean(z), half_width(z)
        pts = [(0.0, y)]
        for i in range(7):
            a = math.pi / 2 * i / 6
            pts.append((w - corner + corner * math.sin(a), y + corner * (1 - math.cos(a))))
        return pts

    zs = [z0 + (z1 - z0) * i / stations for i in range(stations + 1)]
    for za, zb in zip(zs, zs[1:]):
        a, b = section(za), section(zb)
        for side in (-1, 1):
            for j in range(len(a) - 1):
                turn = math.pi / 2 * (j - 0.5) / 6 if j else 0.0
                if side * math.sin(turn) * d.camera.view[0] - math.cos(turn) * d.camera.view[1] <= 0:
                    continue
                d.facet([(side * a[j][0], a[j][1], za), (side * a[j + 1][0], a[j + 1][1], za), (side * b[j + 1][0], b[j + 1][1], zb), (side * b[j][0], b[j][1], zb)], color)
        d.facet([(a[-1][0], a[-1][1], za), (a[-1][0], depth, za), (b[-1][0], depth, zb), (b[-1][0], b[-1][1], zb)], color)
    top = section(z1)
    w = top[-1][0]
    right = [(x, y, z1) for x, y in top]
    left = [(-x, y, z1) for x, y in reversed(top[1:])]
    d.facet(right + [(w, depth, z1), (-w, depth, z1)] + left, color)


LETTER_STROKES = {
    "C": [[(1, 1), (0, 1), (0, 0), (1, 0)]],
    "R": [[(0, 0), (0, 1), (1, 1), (1, 0.5), (0, 0.5)], [(0.4, 0.5), (1, 0)]],
    "E": [[(1, 1), (0, 1), (0, 0), (1, 0)], [(0, 0.5), (0.8, 0.5)]],
    "A": [[(0, 0), (0.5, 1), (1, 0)]],
    "L": [[(0, 1), (0, 0), (1, 0)]],
    "I": [[(0.5, 0), (0.5, 1)]],
    "T": [[(0, 1), (1, 1)], [(0.5, 1), (0.5, 0)]],
    "Y": [[(0, 1), (0.5, 0.5), (1, 1)], [(0.5, 0.5), (0.5, 0)]],
    "Q": [[(1, 0.55), (1, 0.8), (0.8, 1), (0.2, 1), (0, 0.8), (0, 0.2), (0.2, 0), (0.5, 0)], [(0.45, 0.5), (1, 0)]],
    "D": [[(0, 0), (0.75, 0), (1, 0.25), (1, 0.75), (0.75, 1), (0, 1), (0, 0)]],
    "V": [[(0, 1), (0.5, 0), (1, 1)]],
    "F": [[(0, 0), (0, 1), (1, 1)], [(0, 0.5), (0.8, 0.5)]],
    "S": [[(1, 1), (0, 1), (0, 0.5), (1, 0.5), (1, 0), (0, 0)]],
    "H": [[(0, 0), (0, 1)], [(1, 0), (1, 1)], [(0, 0.5), (1, 0.5)]],
    "O": [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)]],
    "G": [[(1, 1), (0, 1), (0, 0), (1, 0), (1, 0.45), (0.5, 0.45)]],
    "i": [[(0.5, 0), (0.5, 0.7)], [(0.5, 0.88), (0.5, 1)]],
    "s": [[(1, 0.7), (0, 0.7), (0, 0.35), (1, 0.35), (1, 0), (0, 0)]],
    "n": [[(0, 0), (0, 0.7), (1, 0.7), (1, 0)]],
    "a": [[(0, 0.7), (1, 0.7), (1, 0), (0, 0), (0, 0.35), (1, 0.35)]],
    "p": [[(0, -0.3), (0, 0.7), (1, 0.7), (1, 0), (0, 0)]],
    "m": [[(0, 0), (0, 0.7), (1, 0.7), (1, 0)], [(0.5, 0.7), (0.5, 0)]],
    "k": [[(0, 1), (0, 0)], [(1, 0.7), (0, 0.3), (1, 0)]],
    "e": [[(1, 0), (0, 0), (0, 0.7), (1, 0.7), (1, 0.35), (0, 0.35)]],
    "r": [[(0, 0), (0, 0.7), (1, 0.7)]],
}


def wordmark(d, text, cx, z, w, h, gap, color, stroke=0.006, y=-0.002):
    """Text drawn in straight strokes on the plane at y, centred on cx with its baseline at z."""
    x = cx - (len(text) * w + (len(text) - 1) * gap) / 2
    for letter in text:
        for line in LETTER_STROKES[letter]:
            d.scene.line([(x + u * w, y, z + v * h) for u, v in line], color, stroke)
        x += w + gap


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
