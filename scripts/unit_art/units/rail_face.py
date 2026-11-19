# SPDX-License-Identifier: GPL-3.0-or-later
"""Selector rails seen head-on, as the path screen draws them: laid out in a product photo's pixels.

A face drawing places every part by its photo position. Parts left of the gates stay put, the
gates and carriage are placed per gate, and parts right of the gates move with the gate count.
"""
import math
from dataclasses import dataclass
from typing import List, Tuple

from .. import kit

PX = 1 / 300  # scene units per photo pixel
STEEL = "#c3c7cc"

Point = Tuple[float, float, float]


@dataclass(frozen=True)
class FaceAnchors:
    """Where the lanes meet a face drawing and where the selected filament leaves it, in world space."""

    inlets: List[Point]  # one per gate, where its lane goes in
    outlet: Point  # where the carriage's output tube leaves the art, with the carriage at gate 0


class Photo:
    """Draws into a scene from photo coordinates: x across, y down from the top, depth v in scene units."""

    def __init__(self, sc, gates, gate0, pitch, photo_gates, floor, split=None, left_end=0, left_pad=0):
        self.sc, self.floor = sc, floor
        self.split = gate0 + photo_gates * pitch - 8 if split is None else split
        self.shift = (gates - photo_gates) * pitch
        # Parts left of left_end move left_pad further out, for a carriage wider than the photo shows room for.
        self.left_end, self.left_pad = left_end, left_pad
        self.by_gate = False  # gates and the carriage are placed by gate, never shifted

    def X(self, x):
        if not self.by_gate:
            if x >= self.split:
                x += self.shift
            elif x < self.left_end:
                x -= self.left_pad
        return x * PX

    def Z(self, y):
        return (self.floor - y) * PX

    def point(self, x, y, v):
        return (self.X(x), v, self.Z(y))

    def box(self, x0, y0, x1, y1, v0, v1, color, **kw):
        """A block over the photo rect (x0, y0)-(x1, y1), v0..v1 deep, its lit top and left edges picked out."""
        u0, u1, z0, z1 = self.X(x0), self.X(x1), self.Z(y1), self.Z(y0)
        shading = dict(amb=0.72, dif=0.42, spec=0.05)
        shading.update(kw)
        self.sc.prism([(u1, v0), (u1, v1), (u0, v1), (u0, v0)], z0, z1, color, **shading)
        self.sc.line([(u0, v0 - 0.002, z1 - 0.003), (u1, v0 - 0.002, z1 - 0.003)], kit.shift(color, 38), 2.2 * PX)
        self.sc.line([(u0 + 0.003, v0 - 0.002, z0), (u0 + 0.003, v0 - 0.002, z1)], kit.shift(color, 18), 1.6 * PX)

    def front(self, pts, v, color, **kw):
        """A flat shape on a front face at depth v."""
        self.sc.poly([(self.X(x), v - 0.001, self.Z(y)) for x, y in pts], color, **kw)

    def disc(self, x, y, r, v, color, n=20):
        self.front([(x + r * math.cos(2 * math.pi * i / n), y + r * math.sin(2 * math.pi * i / n)) for i in range(n)], v, color)

    def wire(self, pts, v, color, width):
        self.sc.line([self.point(x, y, v) for x, y in pts], color, width * PX)

    def ptfe(self, x, y0, y1, v):
        """A short PTFE tube hanging straight down, pale and a little translucent."""
        self.wire([(x, y0), (x, y1)], v, "#8d969e", 15)
        self.wire([(x, y0), (x, y1)], v - 0.001, "#d9e0e5", 10)
        self.wire([(x - 2, y0), (x - 2, y1)], v - 0.002, "#f4f7f9", 3)

    def chain(self, pts, v):
        """A cable chain along pts, its link plates marked."""
        self.wire(pts, v, "#111214", 26)
        self.wire(pts, v, "#2c2d32", 17)
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            n = max(1, int(math.hypot(x1 - x0, y1 - y0) / 18))
            for k in range(n):
                x, y = x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n
                self.sc.line([(self.X(x) - 0.004, v - 0.005, self.Z(y)), (self.X(x) + 0.004, v - 0.005, self.Z(y))], "#46484f", 3 * PX)

    def rod(self, x0, x1, y, v, r=7):
        u0, u1, z = self.X(x0), self.X(x1), self.Z(y)
        self.sc.line([(u0, v, z), (u1, v, z)], kit.scale(STEEL, 0.5), r * 2 * PX)
        self.sc.line([(u0, v - 0.01, z + r * 0.35 * PX), (u1, v - 0.01, z + r * 0.35 * PX)], STEEL, r * 1.1 * PX)
        self.sc.line([(u0, v - 0.012, z + r * 0.55 * PX), (u1, v - 0.012, z + r * 0.55 * PX)], "#eef0f2", r * 0.35 * PX)

    def u_chain(self, start, bend, top, low, v, toward):
        """A chain's moving run: from start along top to bend, then a U down to low. toward is -1 or +1."""
        r = (low - top) / 2
        run = [start, (bend, top)] + [(bend + toward * r * math.sin(math.pi * k / 14), top + r - r * math.cos(math.pi * k / 14)) for k in range(1, 15)]
        self.chain(run + [(bend - toward * 10, low)], v)
        return r


def shadow(sc, x0, x1):
    sc.poly([(x0, -0.1, 0), (x1, -0.1, 0), (x1, 0.5, 0), (x0, 0.5, 0)], "#000", 0.45, blur=True)
