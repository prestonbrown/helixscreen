# SPDX-License-Identifier: GPL-3.0-or-later
"""Projection, shading and SVG layers shared by every unit drawing."""
import math
from dataclasses import dataclass
from typing import Callable, List, Optional, Tuple


def norm(v):
    m = math.sqrt(sum(a * a for a in v))
    return tuple(a / m for a in v)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i : i + 2], 16) for i in (0, 2, 4))


def hexc(c):
    return "#%02x%02x%02x" % tuple(max(0, min(255, int(a))) for a in c)


def shift(color, amt):
    return hexc(tuple(c + amt for c in rgb(color)))


def scale(color, k):
    return hexc(tuple(c * k for c in rgb(color)))


class Camera:
    def __init__(self, yaw_deg, pitch_deg):
        self.yaw = math.radians(yaw_deg)
        self.pitch = math.radians(pitch_deg)
        self.view = norm(
            (
                math.sin(self.yaw) * math.cos(self.pitch),
                -math.cos(self.yaw) * math.cos(self.pitch),
                math.sin(self.pitch),
            )
        )

    def project(self, p):
        x, y, z = p
        return (
            x * math.cos(self.yaw) + y * math.sin(self.yaw),
            (x * math.sin(self.yaw) - y * math.cos(self.yaw)) * math.sin(self.pitch) - z * math.cos(self.pitch),
        )


ART_CAMERA = Camera(27, 20)
LIGHT = norm((-0.35, -0.75, 0.62))


def shade(camera, color, n, amb=0.5, dif=0.72, spec=0.25, power=30):
    half = norm(tuple(a + b for a, b in zip(LIGHT, camera.view)))
    k = amb + dif * max(0, dot(n, LIGHT))
    s = spec * max(0, dot(n, half)) ** power * 255
    return hexc(tuple(a * k + s for a in rgb(color)))


def fillet(pts, radii, closed=False, steps=10):
    """Round polyline corners with quadratic arcs."""
    out = []
    n = len(pts)
    for i, p in enumerate(pts):
        r = radii[i]
        if r == 0 or (not closed and i in (0, n - 1)):
            out.append(p)
            continue
        a, b = pts[(i - 1) % n], pts[(i + 1) % n]
        la, lb = math.dist(p, a), math.dist(p, b)
        p0 = (p[0] + (a[0] - p[0]) * r / la, p[1] + (a[1] - p[1]) * r / la)
        p1 = (p[0] + (b[0] - p[0]) * r / lb, p[1] + (b[1] - p[1]) * r / lb)
        for k in range(steps + 1):
            t = k / steps
            out.append(tuple((1 - t) ** 2 * p0[j] + 2 * (1 - t) * t * p[j] + t * t * p1[j] for j in (0, 1)))
    return out


def tray_profile(profile, base_z, px_per_unit):
    """A (y, z) side profile as tray points: (depth fraction, pixels above the front wall)."""
    y0, y1 = profile[0][0], profile[-1][0]
    return [((y - y0) / (y1 - y0), (z - base_z) * px_per_unit) for y, z in profile]


@dataclass
class TrayShellDef:
    cheek: List[Tuple[float, float]]
    dome: List[Tuple[float, float]]
    back_extra_px: float
    front_wall_px: float
    rail_px: float
    cap_clearance_px: float
    lid_spans_row: bool
    has_latch: bool
    body: int
    panel: int
    rail: int
    accent: int
    corner: int = 0


@dataclass
class UnitDef:
    id: str
    lanes: int
    draw_art: Callable
    tray: Optional[TrayShellDef] = None


@dataclass
class Item:
    points: list
    fill: str
    opacity: float
    stroke: str
    stroke_width: float
    closed: bool
    layer: str
    lane: object
    tag: str
    blur: bool = False

    def svg(self, fill=None, opacity=None):
        d = " ".join(f"{x:.4f},{y:.4f}" for x, y in self.points)
        f = self.fill if fill is None else fill
        op = self.opacity if opacity is None else opacity
        if not self.closed:
            stroke = self.stroke if fill is None else fill
            return (
                f'<polyline points="{d}" fill="none" stroke="{stroke}" stroke-width="{self.stroke_width}" '
                f'stroke-opacity="{op}" stroke-linecap="round" stroke-linejoin="round"/>'
            )
        attrs = ""
        if self.stroke:
            stroke = self.stroke if fill is None else fill
            attrs = f' stroke="{stroke}" stroke-width="{self.stroke_width}" stroke-linejoin="round"'
        elif op >= 1.0:
            attrs = f' stroke="{f}" stroke-width="0.0015"'
        blur = ' filter="url(#soft)"' if self.blur else ""
        return f'<polygon points="{d}" fill="{f}" fill-opacity="{op}"{attrs}{blur}/>'


_INVERT = (
    '<filter id="shade" x="0" y="0" width="1" height="1" color-interpolation-filters="sRGB">'
    '<feColorMatrix type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  -1 0 0 0 1"/></filter>'
)
_SOFT = '<filter id="soft" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="0.025"/></filter>'


class Scene:
    """Ordered drawing, tagged so one drawing bakes into separate layers."""

    def __init__(self, camera):
        self.camera = camera
        self.items = []
        self.grads = []
        self.layer = "back"

    def _add(self, pts3, fill, opacity, stroke, sw, closed, lane, tag, blur=False):
        self.items.append(
            Item([self.camera.project(p) for p in pts3], fill, opacity, stroke, sw, closed, self.layer, lane, tag, blur)
        )

    def poly(self, pts3, fill, opacity=1.0, stroke=None, sw=0.0, lane=None, tag="", blur=False):
        self._add(pts3, fill, opacity, stroke, sw, True, lane, tag, blur)

    def line(self, pts3, stroke, sw, opacity=1.0, lane=None, tag=""):
        self._add(pts3, stroke, opacity, stroke, sw, False, lane, tag)

    def grad(self, top, bottom, z_top, z_bottom, x, y):
        ax, ay = self.camera.project((x, y, z_top))
        bx, by = self.camera.project((x, y, z_bottom))
        gid = f"g{len(self.grads)}"
        self.grads.append(
            f'<linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="{ax:.4f}" y1="{ay:.4f}" '
            f'x2="{bx:.4f}" y2="{by:.4f}"><stop offset="0" stop-color="{top}"/>'
            f'<stop offset="1" stop-color="{bottom}"/></linearGradient>'
        )
        return f"url(#{gid})"

    def prism(self, foot, z0, z1, color, **kw):
        for a, b in zip(foot, foot[1:] + foot[:1]):
            nrm = norm((b[1] - a[1], -(b[0] - a[0]), 0))
            if dot(nrm, self.camera.view) > 0:
                self.poly(
                    [(a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], z1), (a[0], a[1], z1)],
                    shade(self.camera, color, nrm, **kw),
                )

    def extrude_x(self, profile, x0, x1, color, opacity, front):
        """Extrude a (y, z) profile along x; front selects the camera-facing strips."""
        half = norm(tuple(a + b for a, b in zip(LIGHT, self.camera.view)))
        for a, b in zip(profile, profile[1:]):
            d = (b[0] - a[0], b[1] - a[1])
            if d == (0, 0):
                continue
            nrm = norm((0, -d[1], d[0]))
            if (dot(nrm, self.camera.view) > 0) != front:
                continue
            k = max(0, dot(nrm, half)) ** 24
            self.poly(
                [(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])],
                hexc(tuple(c + 90 * k for c in rgb(color))),
                min(0.9, opacity + 0.16 * k),
                stroke=None,
            )

    def _viewbox(self):
        xs = [p[0] for i in self.items for p in i.points]
        ys = [p[1] for i in self.items for p in i.points]
        m = 0.05
        return min(xs) - m, min(ys) - m, max(xs) - min(xs) + 2 * m, max(ys) - min(ys) + 2 * m

    def _wrap(self, width, body, extra_defs=""):
        vx, vy, vw, vh = self._viewbox()
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vx} {vy} {vw} {vh}" width="{width}">'
            f"<defs>{_SOFT}{_INVERT}{''.join(self.grads)}{extra_defs}</defs>{body}</svg>"
        )

    def svg(self, width):
        return self._wrap(width, "".join(i.svg() for i in self.items))

    def layer_svg(self, layer, width):
        """A shell layer, without any tintable filament."""
        return self._wrap(width, "".join(i.svg() for i in self.items if i.layer == layer and i.lane is None))

    def _coverage_mask(self, lane):
        body = "".join(
            i.svg(fill="#fff" if i.lane == lane else "#000", opacity=1.0)
            for i in self.items
            if i.layer == "back" and not i.blur
        )
        return f'<mask id="cover" maskUnits="userSpaceOnUse" {self._frame()}>{body}</mask>'

    def _frame(self):
        vx, vy, vw, vh = self._viewbox()
        return f'x="{vx}" y="{vy}" width="{vw}" height="{vh}"'

    def lane_color_svg(self, lane, width):
        """White where lane's filament shows; the runtime recolours it."""
        return self._wrap(width, f'<rect {self._frame()} fill="#fff" mask="url(#cover)"/>', self._coverage_mask(lane))

    def lane_shade_svg(self, lane, width):
        """Black at the filament's darkness, laid over its tinted colour."""
        lit = "".join(i.svg() for i in self.items if i.lane == lane)
        body = f'<g mask="url(#cover)"><g filter="url(#shade)"><rect {self._frame()} fill="#fff"/>{lit}</g></g>'
        return self._wrap(width, body, self._coverage_mask(lane))
