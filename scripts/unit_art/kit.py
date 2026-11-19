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
# A selector rail on the path screen: straight on, a little from above so the gate tops read.
FACE_CAMERA = Camera(0, 7)
# Every unit is framed so its spools draw at one size: the frame, in the units of a
# drawing whose spool radius is ART_SPOOL_R, has room for any unit of up to four lanes.
# A wider unit widens its own frame, so its spools draw a little smaller.
ART_SPOOL_R = 0.25
ART_FRAME = (1.43, 1.21)
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


# A front part: (anchor, x0, x1, y0, y1, colour, radius). x is in pixels from its anchor at
# the 110 px reference spool, y in pixels below the front wall's top (negative rises above it).
def span(inset_left, inset_right, y0, y1, color, radius=0):
    """Across the row, inset from each end."""
    return ("Span", inset_left, inset_right, y0, y1, color, radius)


def lane(x0, x1, y0, y1, color, radius=0):
    """Repeated at every lane, offset from its centre."""
    return ("Lane", x0, x1, y0, y1, color, radius)


def center(x0, x1, y0, y1, color, radius=0):
    """Once, offset from the middle of the row."""
    return ("Center", x0, x1, y0, y1, color, radius)


def left(x0, x1, y0, y1, color, radius=0):
    """Once, measured in from the left end."""
    return ("Left", x0, x1, y0, y1, color, radius)


def right(x0, x1, y0, y1, color, radius=0):
    """Once, measured in from the right end."""
    return ("Right", x0, x1, y0, y1, color, radius)


def classic_front(t):
    """A panel under a rail, with a tab and tick per lane: the default until a unit sets its own."""
    wall, rail = t.front_wall_px, t.rail_px
    parts = [
        span(3, 3, rail + 2, wall - 3, t.panel, 3),
        span(4, 4, 0, rail, t.rail, 2),
        lane(-7, 7, -3, 0, t.accent),
        lane(-1, 1, rail * 0.35, rail * 0.85, 0xf7f7f2),
    ]
    if t.has_latch:
        parts.append(center(-15, 15, rail - 3, rail + 5, 0xd8dade, 4))
    if t.corner:
        parts += [left(0, 8, -1, wall, t.corner), right(0, 8, -1, wall, t.corner)]
    return parts


CAP_POINTS = 17  # kCapPoints in ui_ams_detail.cpp


def fit_cap_points(dome):
    """Pad a lid profile to the runtime's point count, repeating points so corners survive."""
    if not dome:
        return []
    if len(dome) < 2:
        raise ValueError("lid profile needs a front and a back point")
    if len(dome) > CAP_POINTS:
        last = len(dome) - 1
        return [dome[round(i * last / (CAP_POINTS - 1))] for i in range(CAP_POINTS)]
    points = list(dome)
    i = 0
    while len(points) < CAP_POINTS:
        points.insert(i + 1, points[i])
        i = (i + 2) % (len(points) - 1)
    return points


def check_cheek(cheek):
    """The body's top edge: at least two points, strictly increasing in depth."""
    if len(cheek) < 2 or any(b[0] <= a[0] for a, b in zip(cheek, cheek[1:])):
        raise ValueError(f"cheek depths must strictly increase: {cheek}")
    return cheek


def rrect_front(x0, x1, z0, z1, r, y):
    """A rounded rectangle on the front plane at depth y."""
    pts = fillet([(x0, z0), (x1, z0), (x1, z1), (x0, z1)], [r] * 4, closed=True, steps=6)
    return [(a, y, b) for a, b in pts]


def lid_back(scene, profile, x0, x1, color, cap_op, strip_op):
    """A lid's far end cap and far-side strips, drawn before the spools."""
    scene.poly([(x0, y, z) for y, z in profile], color, cap_op)
    scene.extrude_x(profile, x0, x1, color, strip_op, front=False)


def lid_front(scene, profile, x0, x1, color, cap_op, strip_op):
    """A lid's camera-facing strips and near end cap, drawn over the spools."""
    scene.extrude_x(profile, x0, x1, color, strip_op, front=True)
    scene.poly([(x1, y, z) for y, z in profile], color, cap_op)


def arch_profile(y0, y1, z0, z1, flat=0.0, riser=0.0, steps=20):
    """An arched lid's (y, z) side profile, front foot to rear foot.

    flat is the crown's share of the width; riser is the straight wall below each shoulder.
    """
    mid, half_flat = (y0 + y1) / 2, (y1 - y0) * flat / 2
    a, b = (y1 - y0) / 2 - half_flat, z1 - z0 - riser
    base = z0 + riser
    angles = [math.pi / 2 * i / steps for i in range(steps + 1)]
    front = [(mid - half_flat - a * math.cos(t), base + b * math.sin(t)) for t in angles]
    rear = [(mid + half_flat + a * math.sin(t), base + b * math.cos(t)) for t in angles]
    pts = front + rear
    return [(y0, z0)] + pts + [(y1, z0)] if riser else pts


def circle_yz(y, z, r, n=24):
    """A closed circle in the (y, z) side plane."""
    return [(y + r * math.cos(2 * math.pi * i / n), z + r * math.sin(2 * math.pi * i / n)) for i in range(n + 1)]


def _roller_end(sc, x, y, z, r, face, hub):
    sc.poly([(x, a, b) for a, b in circle_yz(y, z, r * 0.92)], face)
    if hub:
        sc.poly([(x, a, b) for a, b in circle_yz(y, z, r * 0.42)], hub)


def roller(sc, x0, x1, y, z, r, color, flange, flange_k=1.18, flange_w=0.016, left_flange=True, sheen=None, end_face=None, end_hub=None, solid=False, right_flange=True):
    """A drum along x, flanged at either end; each flange can be left off.

    sheen is the colour of a highlight along the drum's lit shoulder, for a colour too dark
    to show its curve by shading alone. end_face fills each flange inside its edge,
    leaving a thin rim, and end_hub
    colours a boss at the middle of that face. solid draws the barrel and flange
    bands' camera-facing halves; without it their far halves show, like a hollow sleeve.
    """
    ring = circle_yz(y, z, r)
    edge = circle_yz(y, z, r * flange_k)
    if left_flange:
        sc.extrude_x(edge, x0 - flange_w, x0, flange, 1.0, front=not solid)
        sc.poly([(x0, a, b) for a, b in edge], shift(flange, 18))
        if end_face:
            _roller_end(sc, x0 + 0.0005, y, z, r, end_face, end_hub)
    sc.extrude_x(ring, x0, x1, color, 1.0, front=not solid)
    if sheen:
        sc.line([(x0 + r * 0.1, y - r * 0.6, z + r * 0.8), (x1 - r * 0.1, y - r * 0.6, z + r * 0.8)], sheen, r * 0.2, 0.75)
    if not right_flange:
        sc.poly([(x1, a, b) for a, b in ring], shift(color, 10))
        return
    sc.extrude_x(edge, x1, x1 + flange_w, flange, 1.0, front=not solid)
    sc.poly([(x1 + flange_w, a, b) for a, b in edge], shift(flange, 18))
    if end_face:
        _roller_end(sc, x1 + flange_w + 0.0005, y, z, r, end_face, end_hub)


def tray_profile(profile, base_z, px_per_unit):
    """A (y, z) side profile as tray points: (depth fraction, pixels above the front wall)."""
    y0, y1 = profile[0][0], profile[-1][0]
    return [((y - y0) / (y1 - y0), (z - base_z) * px_per_unit) for y, z in profile]


def outlet(scene, lane, point):
    """Where lane's filament leaves the unit, a world point; without one it leaves the front
    bottom edge under its spool."""
    scene.outlets[lane] = point


def sensor_region(scene, lane, kind, points):
    """The part of the drawing lane's sensor of kind ("prep") stands in, as world points: the
    app draws it red when that sensor reports a fault. Only "prep" is baked; the load sensor
    stands at the lane's outlet. A ScaledScene's points are in its own units."""
    if isinstance(scene, ScaledScene):
        points, scene = scene._pts(points), scene.scene
    scene.sensors.setdefault(lane, {})[kind] = list(points)


class ScaledScene:
    """Draws into a scene with every length multiplied by k, so a body keeps its own units."""

    def __init__(self, scene, k):
        self.scene, self.k, self.camera = scene, k, scene.camera

    def _pts(self, pts):
        return [tuple(a * self.k for a in p) for p in pts]

    def poly(self, pts3, fill, opacity=1.0, stroke=None, sw=0.0, **kw):
        self.scene.poly(self._pts(pts3), fill, opacity, stroke, sw * self.k, **kw)

    def line(self, pts3, stroke, sw, opacity=1.0, **kw):
        self.scene.line(self._pts(pts3), stroke, sw * self.k, opacity, **kw)

    def prism(self, foot, z0, z1, color, **kw):
        self.scene.prism(self._pts(foot), z0 * self.k, z1 * self.k, color, **kw)

    def extrude_x(self, profile, x0, x1, color, opacity, front):
        self.scene.extrude_x(self._pts(profile), x0 * self.k, x1 * self.k, color, opacity, front)


def carriage_step(scene, step):
    """One gate of world-space travel as a fraction of the art's width on screen."""
    a = scene.camera.project((0.0, 0.0, 0.0))
    b = scene.camera.project(step)
    _, _, vw, _ = scene._viewbox()
    return (b[0] - a[0]) / vw, (b[1] - a[1]) / vw


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
    front: Optional[list] = None
    # Where the unit's lanes merge into one output, from Preston's table in docs/devel/UNIT_ART.md
    # § "Combiner": "internal", "external" or "none". A unit missing from that table: ask him.
    combiner: Optional[str] = None
    port_block: bool = False  # its selector is a fixed block with one port per lane
    lid_color: int = 0  # a box per lane: each box's solid top around its glass; 0 for all glass
    spool_lift_px: float = 0.0  # how far the unit's base reaches below its spools
    plain_tray: bool = False  # in front of loose spools: the tray under them is the plain one


@dataclass
class UnitDef:
    id: str
    lanes: int
    draw_art: Callable
    tray: Optional[TrayShellDef] = None
    # Lane counts this design is built in, each baked to its own set. Empty: only 'lanes'.
    lane_variants: Tuple[int, ...] = ()
    # A second drawing, baked under stubs/: the body alone, tight in its frame, a tube per lane.
    stub_art: Optional[Callable] = None
    # A unit with a moving selector: given a lane count, the world-space travel between gates.
    carriage_step: Optional[Callable] = None
    # A rail's head-on drawing for the path screen: draw(scene, gates, tier) -> rail_face.FaceAnchors,
    # the carriage in its own layer at gate 0. Baked per lane variant under face/.
    face_art: Optional[Callable] = None


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
    spool: object = None  # the lane whose spool this item is part of, flanges and core included

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
        self.spool_radius = None
        self.margin = 0.05
        # Per lane, the spool spool.draw_spool drew there and where its filament leaves the unit.
        self.spools = {}
        self.outlets = {}
        # Per lane, the drawn parts its own sensors stand in: {"prep": [world points]}.
        self.sensors = {}
        # The lane whose spool is being drawn: spool.draw_spool sets it around its items.
        self.current_spool = None

    def _add(self, pts3, fill, opacity, stroke, sw, closed, lane, tag, blur=False):
        self.items.append(
            Item(
                [self.camera.project(p) for p in pts3], fill, opacity, stroke, sw, closed, self.layer, lane, tag, blur,
                self.current_spool,
            )
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
                min(max(0.9, opacity), opacity + 0.16 * k),
                stroke=None,
            )

    def _viewbox(self):
        xs = [p[0] for i in self.items for p in i.points]
        ys = [p[1] for i in self.items for p in i.points]
        m = self.margin
        if self.spool_radius is None:
            return min(xs) - m, min(ys) - m, max(xs) - min(xs) + 2 * m, max(ys) - min(ys) + 2 * m
        # Centred across the frame and standing on its floor, so units line up in a row.
        scale = self.spool_radius / ART_SPOOL_R
        vw, vh = ART_FRAME[0] * scale, ART_FRAME[1] * scale
        fit = max(1.0, (max(xs) - min(xs) + 2 * m * scale) / vw, (max(ys) - min(ys) + 2 * m * scale) / vh)
        vw, vh = vw * fit, vh * fit
        return (min(xs) + max(xs)) / 2 - vw / 2, max(ys) + m * scale - vh, vw, vh

    def _wrap(self, width, body, extra_defs=""):
        vx, vy, vw, vh = self._viewbox()
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vx} {vy} {vw} {vh}" width="{width}">'
            f"<defs>{_SOFT}{_INVERT}{''.join(self.grads)}{extra_defs}</defs>{body}</svg>"
        )

    def svg(self, width):
        return self._wrap(width, "".join(i.svg() for i in self.items))

    def layers_svg(self, layers, width):
        """Everything drawn in any of layers, filament included."""
        return self._wrap(width, "".join(i.svg() for i in self.items if i.layer in layers))

    # A drawing with spools composes back to front, left to right: the back layer, then each
    # lane's stack from the farthest (leftmost) to the nearest, then the front layer.

    def _spool_starts(self):
        """Where each lane's spool starts in the drawing order, by lane."""
        starts = {}
        for k, item in enumerate(self.items):
            if item.spool is not None and item.spool not in starts:
                starts[item.spool] = k
        return starts

    def lane_order(self):
        """The lanes with spools in the order the drawing paints their stacks, back to front.
        The runtime composes the lane stacks in this order, whatever the lanes' numbering."""
        starts = self._spool_starts()
        return sorted(starts, key=lambda lane: starts[lane])

    def back_items(self):
        """The back layer's items drawn before any spool: what stands behind every lane."""
        starts = self._spool_starts()
        first = min(starts.values()) if starts else len(self.items)
        return [i for k, i in enumerate(self.items) if i.layer == "back" and k < first]

    def lane_items(self, lane):
        """Lane's stack: its spool and every back-layer item drawn after it and before the next
        lane's spool, such as a box per lane."""
        starts = self._spool_starts()
        if lane not in starts:
            return []
        later = [k for k in starts.values() if k > starts[lane]]
        end = min(later) if later else len(self.items)
        return [i for i in self.items[starts[lane] : end] if i.layer == "back"]

    def layer_svg(self, layer, width):
        """A shell layer, without any tintable filament; with spools, the back holds only what
        stands behind them."""
        items = self.back_items() if layer == "back" and self.spools else [i for i in self.items if i.layer == layer]
        return self._wrap(width, "".join(i.svg() for i in items if i.lane is None))

    def lane_body_svg(self, lane, width):
        """Lane's stack without its tintable filament: flanges, core, and what is drawn with them."""
        return self._wrap(width, "".join(i.svg() for i in self.lane_items(lane) if i.lane is None))

    def stacks_hold_more_than_spools(self):
        """Whether any lane's stack carries parts besides its spool, such as a bay's walls."""
        return any(i.spool is None for lane in self.spools for i in self.lane_items(lane))

    def _silhouette(self, lane):
        return "".join(i.svg(fill="#fff", opacity=1.0) for i in self.items if i.spool == lane and not i.blur)

    def lane_silhouette_svg(self, lane, width):
        """White over all of lane's spool, as if nothing stood in front of it."""
        return self._wrap(width, self._silhouette(lane))

    def lane_glow_svg(self, lane, width):
        """Lane's silhouette blurred into a soft light spreading past its outline."""
        sd = (self.spool_radius or 0.25) * 0.16
        glow = (
            f'<filter id="glow" x="-50%" y="-50%" width="200%" height="200%">'
            f'<feGaussianBlur stdDeviation="{sd:.4f}"/>'
            '<feComponentTransfer><feFuncA type="linear" slope="1.8"/></feComponentTransfer></filter>'
        )
        return self._wrap(width, f'<g filter="url(#glow)">{self._silhouette(lane)}</g>', glow)

    def _coverage_mask(self, lane):
        # With spools, the tint is cut only by its own stack: the lanes in front cover it
        # by being composed after it.
        items = self.lane_items(lane) if lane in self.spools else [i for i in self.items if i.layer == "back"]
        body = "".join(i.svg(fill="#fff" if i.lane == lane else "#000", opacity=1.0) for i in items if not i.blur)
        return f'<mask id="cover" maskUnits="userSpaceOnUse" {self._frame()}>{body}</mask>'

    def _frame(self):
        vx, vy, vw, vh = self._viewbox()
        return f'x="{vx}" y="{vy}" width="{vw}" height="{vh}"'

    def lane_color_svg(self, lane, width):
        """White where lane's filament shows; the runtime recolours it."""
        return self._wrap(width, f'<rect {self._frame()} fill="#fff" mask="url(#cover)"/>', self._coverage_mask(lane))

    # A turning spool: the vents of both its flanges rotate at runtime, so they bake apart from
    # the flanges they turn on. Each lane's face layers are its flanges as they show, cut by the
    # stacks drawn over them: the near one by later lanes, the far one by its own core and near
    # flange too (its filament covers it by the fill the runtime draws). The vents are baked once
    # per flange, from one lane, whole and centered on it, since every spool of a drawing is the
    # same spool moved along the row.

    _VENT_TAGS = ("slot", "air_hole")

    def _vent_items(self, lane, far=False):
        tags = tuple(("far_" if far else "") + t for t in self._VENT_TAGS)
        return [i for i in self.items if i.spool == lane and i.tag in tags]

    def spools_turn(self):
        """Whether every lane's spool has vents on both flanges and all are one size, so one
        set of vent layers serves every lane."""
        radii = {round(sp["radius"], 9) for sp in self.spools.values()}
        return (bool(self.spools) and len(radii) == 1
                and all(self._vent_items(lane) and self._vent_items(lane, far=True) for lane in self.spools))

    def _over_face(self, lane, far=False):
        """Every lane stack item the runtime composes over lane's near (or far) flange: the rest
        of its own stack but its filament, then each later lane's stack. The front layer, often
        glass, is laid over the turning vents at runtime instead."""
        order = self.lane_order()
        tags = ("far_face",) + tuple("far_" + t for t in self._VENT_TAGS) if far else None
        mine = [k for k, i in enumerate(self.items) if i.spool == lane and (tags is None or i.tag in tags)]
        own_ids = {id(i) for i in self.lane_items(lane)}
        own = [i for i in self.items[max(mine) + 1 :] if id(i) in own_ids and i.lane is None]
        later = [i for m in order[order.index(lane) + 1 :] for i in self.lane_items(m)]
        return own + later

    def lane_face_svg(self, lane, width, far=False):
        """Lane's near flange and hub (or its far flange) without their vents, only where nothing
        but its own filament stands over them."""
        cover = "".join(i.svg(fill="#000") for i in self._over_face(lane, far))
        mask = f'<mask id="seen" maskUnits="userSpaceOnUse" {self._frame()}><rect {self._frame()} fill="#fff"/>{cover}</mask>'
        tags = ("far_face",) if far else ("face", "hub")
        face = "".join(i.svg() for i in self.items if i.spool == lane and i.tag in tags)
        return self._wrap(width, f'<g mask="url(#seen)">{face}</g>', mask)

    def vents_size(self, lane, art_width, far=False):
        """The vent layer's size in pixels, even both ways, for a drawing art_width wide: the
        flange's extent with a pixel to spare, centered on the flange's center."""
        _, _, vw, _ = self._viewbox()
        k = art_width / vw
        cx, cy = self.camera.project(self.spools[lane]["far" if far else "near"])
        tag = "far_face" if far else "face"
        pts = [p for i in self.items if i.spool == lane and i.tag == tag for p in i.points]
        half_x = max(abs(p[0] - cx) for p in pts)
        half_y = max(abs(p[1] - cy) for p in pts)
        return 2 * math.ceil(half_x * k + 2), 2 * math.ceil(half_y * k + 2)

    def vents_svg(self, lane, art_width, lit, far=False):
        """Lane's near (or far) flange vents alone, at the drawing's scale, centered on the
        flange: their dark hollows and lips, or with lit the filament seen through the near
        ones drawn white, for the runtime to multiply by the filament's color."""
        _, _, vw, _ = self._viewbox()
        k = art_width / vw
        w, h = self.vents_size(lane, art_width, far)
        cx, cy = self.camera.project(self.spools[lane]["far" if far else "near"])
        items = [i for i in self._vent_items(lane, far) if (i.lane == lane) == lit]
        view = f"{cx - w / 2 / k} {cy - h / 2 / k} {w / k} {h / k}"
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}" width="{w}" height="{h}">'
            f"<defs>{''.join(self.grads)}</defs>{''.join(i.svg() for i in items)}</svg>"
        )

    def lane_shade_svg(self, lane, width):
        """Black at the filament's darkness, laid over its tinted colour."""
        lit = "".join(i.svg() for i in self.items if i.lane == lane)
        body = f'<g mask="url(#cover)"><g filter="url(#shade)"><rect {self._frame()} fill="#fff"/>{lit}</g></g>'
        return self._wrap(width, body, self._coverage_mask(lane))
