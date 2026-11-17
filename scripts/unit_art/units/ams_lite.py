# SPDX-License-Identifier: GPL-3.0-or-later
"""Bambu Lab AMS lite: an open stand. A grey housing slab stands on edge, tilted so its front
end sits low, with a rotary spool holder out of each face at each end; four spools hang there,
two a side. Two feeder units straddle the slab's top edge and the stand's two toes splay forward.

Lengths are millimetres from Bambu's dimension drawing. The slab runs along y, its faces at
x = +-SLAB_T, so the spools' axles run across the unit as the shared spool expects.
"""
import math
from pathlib import Path

from .. import feeder, kit, spool
from ..kit import TrayShellDef, UnitDef

SPOOL_R = 0.25
SPOOL_MM = 100.0
SCALE = SPOOL_R / SPOOL_MM
_REPO = Path(__file__).resolve().parents[3]

# The housing: two axles 266 mm apart on a line rising 28 degrees to the back.
SLAB_ZC = 196.0
TILT = math.atan2(126.0, 235.0)
AXLE_S = 133.3
END_R, WAIST = 81.0, 62.0
SLAB_T = 19.0
# Spools sit on the holders just clear of the slab; the holders reach past their outer flanges.
SPOOL_X = 60.0
HOLDER_R, HOLDER_TIP = 22.0, 101.0
# Feeder units: across the unit from the slab side out, along and above the slab's top edge.
FEED_X0, FEED_X1 = 14.0, 104.0
FEED_S, FEED_N0, FEED_SPLIT, FEED_N1 = 37.0, 40.0, 85.0, 146.0
HOUSING, HOUSING_EDGE = "#c8cacd", "#a9acb1"
STAND, FEEDER, FUNNEL, CAP, KNOB = "#3a3b3f", "#53565b", "#3b3c40", "#6a6d72", "#f2b31a"
HOLDER, HOLDER_END = "#d9dbde", "#2e2f33"
# Lane per holder (front/back, face): 1|4 at the high back end, 2|3 at the low front end.
LANES = {("back", -1): 0, ("front", -1): 1, ("front", 1): 2, ("back", 1): 3}


def _world(x, s, n):
    """A point in the slab's frame (s along it, n up off its top edge) in world millimetres."""
    c, k = math.cos(TILT), math.sin(TILT)
    return (x, s * c - n * k, SLAB_ZC + s * k + n * c)


def _axle(end):
    return _world(0.0, AXLE_S if end == "back" else -AXLE_S, 0.0)


def _slab_outline():
    """The dogbone side outline in solid() order: front foot, up, over the top, down the back."""

    def half(s):
        t = min(1.0, max(0.0, (abs(s) - 55.0) / 60.0))
        return WAIST + (END_R - WAIST) * t * t * (3 - 2 * t)

    top = [(s, half(s)) for s in (-AXLE_S + 266.6 * i / 40 for i in range(41))]
    back = [(AXLE_S + END_R * math.cos(a), END_R * math.sin(a)) for a in (math.pi / 2 - math.pi * i / 16 for i in range(1, 16))]
    bottom = [(s, -n) for s, n in reversed(top)]
    front = [(-AXLE_S + END_R * math.cos(a), END_R * math.sin(a)) for a in (-math.pi / 2 - math.pi * i / 16 for i in range(1, 16))]
    pts = top + back + bottom + front
    rot = pts.index(min(pts, key=lambda p: (_world(0, *p)[1], _world(0, *p)[2])))
    pts = pts[rot:] + pts[:rot]
    return [_world(0, s, n)[1:] for s, n in pts]


def _solid(sc, bottom, top, color, amb=0.72, dif=0.5, spec=0.1, opacity=1.0):
    """A convex body between two matching outlines, its camera-facing faces lit."""
    cam = sc.camera
    every = bottom + top
    mid = tuple(sum(p[i] for p in every) / len(every) for i in range(3))

    def face(pts):
        a, b, c = pts[0], pts[1], pts[2]
        u = tuple(b[i] - a[i] for i in range(3))
        v = tuple(c[i] - a[i] for i in range(3))
        nrm = kit.norm((u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]))
        centre = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
        if kit.dot(nrm, tuple(centre[i] - mid[i] for i in range(3))) < 0:
            nrm = tuple(-q for q in nrm)
        if kit.dot(nrm, cam.view) > 0:
            sc.poly(pts, kit.shade(cam, color, nrm, amb=amb, dif=dif, spec=spec), opacity)

    m = len(bottom)
    for i in range(m):
        j = (i + 1) % m
        face([bottom[i], bottom[j], top[j], top[i]])
    face(bottom[::-1])
    face(top)


def _tilted(sc, foot, n0, n1, color, **kw):
    """A prism standing up off the slab's top edge: foot is an (x, s) outline."""
    _solid(sc, [_world(x, s, n0) for x, s in foot], [_world(x, s, n1) for x, s in foot], color, **kw)


def _round_rect(x0, x1, s0, s1, r):
    return kit.fillet([(x0, s0), (x1, s0), (x1, s1), (x0, s1)], [r] * 4, closed=True, steps=4)


def _ring(x, s, r, n=20):
    return [(x + r * math.cos(2 * math.pi * i / n), s + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def _feeder(sc, side, detail, thick):
    """One feeder unit: a smoky funnel block at the slab, the two-lobed motor body above it."""
    x0, x1 = side * FEED_X0, side * FEED_X1
    lo, hi = min(x0, x1), max(x0, x1)
    _tilted(sc, _round_rect(lo, hi, -FEED_S, FEED_S, 10.0), FEED_N0, FEED_SPLIT, FUNNEL, amb=0.8, dif=0.4, spec=0.05)
    lobes = [(side * 42.0, 14.0), (side * 78.0, -16.0)]
    hull = []
    for i in range(24):
        a = 2 * math.pi * i / 24
        d = (math.cos(a), math.sin(a))
        cx, cs = max(lobes, key=lambda p: p[0] * d[0] + p[1] * d[1])
        hull.append((cx + 25.0 * d[0], cs + 25.0 * d[1]))
    _tilted(sc, hull, FEED_SPLIT, FEED_N1, FEEDER)
    for cx, cs in lobes:
        sc.poly([_world(x, s, FEED_N1 + 0.5) for x, s in _ring(cx, cs, 21.0)], CAP)
        sc.line([_world(x, s, FEED_N1 + 0.6) for x, s in _ring(cx, cs, 21.0) + _ring(cx, cs, 21.0)[:1]], kit.shift(CAP, 26), 1.2 * thick, 0.8)
    if detail and side > 0:
        for s in (-18.0, 18.0):
            knob = [_world(x1 + side * 0.6, s + 7.0 * math.cos(a), 62.0 + 7.0 * math.sin(a)) for a in (2 * math.pi * i / 14 for i in range(14))]
            sc.poly(knob, KNOB)
        for cx, cs in lobes:
            sc.poly([_world(x, s, FEED_N1 + 1.0) for x, s in _ring(cx, cs, 4.0, 10)], kit.shift(CAP, -24))


def _holder_cap(sc, x, end, color):
    """The rotary holder's end: a grey sleeve through the spool's hub, a dark cap, three claws."""
    y, z = _axle(end)[1:]
    sc.poly([(x, y + HOLDER_R * math.cos(a), z + HOLDER_R * math.sin(a)) for a in (2 * math.pi * i / 20 for i in range(20))], HOLDER)
    sc.poly([(x + 0.5, y + 15 * math.cos(a), z + 15 * math.sin(a)) for a in (2 * math.pi * i / 20 for i in range(20))], color)
    for k in range(3):
        a = math.pi / 2 + 2 * math.pi * k / 3
        sc.line([(x + 0.6, y + 15 * math.cos(a), z + 15 * math.sin(a)), (x + 0.6, y + HOLDER_R * math.cos(a), z + HOLDER_R * math.sin(a))], "#8d9096", 2.0)


def _stand(sc, detail):
    """Two toes splaying forward from a root at the back, and the column up to the slab."""
    root, root_w = 155.0, 18.0
    for side in (-1, 1):
        tip = (side * 102.0, -114.0)
        dx, dy = tip[0], tip[1] - root
        length = math.hypot(dx, dy)
        px, py = -dy / length, dx / length
        foot = [
            (side * 4.0 + px * root_w, root + py * root_w),
            (tip[0] + px * 9.0, tip[1] + py * 9.0),
            (tip[0] - px * 9.0, tip[1] - py * 9.0),
            (side * 4.0 - px * root_w, root - py * root_w),
        ]
        heights = [16.0, 5.0, 5.0, 16.0]
        _solid(sc, [(x, y, 0.0) for x, y in foot], [(x, y, h) for (x, y), h in zip(foot, heights)], STAND, amb=0.75, dif=0.45, spec=0.12)
    lo_front, lo_back = _world(0, -75.0, -WAIST), _world(0, 40.0, -WAIST)
    column = [(60.0, 8.0), (lo_front[1], lo_front[2] + 8.0), (lo_back[1], lo_back[2] + 8.0), (172.0, 8.0)]
    sc.extrude_x(column + column[:1], -15.0, 15.0, STAND, 1.0, front=True)
    sc.poly([(15.0, y, z) for y, z in column], kit.shade(sc.camera, STAND, (1, 0, 0), amb=0.78, dif=0.45, spec=0))
    if detail:
        for t0 in (0.1, 0.38):
            a = [column[1][i] + (column[2][i] - column[1][i]) * t0 for i in range(2)]
            b = [column[1][i] + (column[2][i] - column[1][i]) * (t0 + 0.26) for i in range(2)]
            c = [(a[i] + b[i]) / 2 + (column[0][i] - column[1][i]) * 0.3 for i in range(2)]
            sc.poly([(15.4, *a), (15.4, *b), (15.4, *c)], "#26272a")


def draw_art(scene, colors, tier):
    sc = kit.ScaledScene(scene, SCALE)
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)

    sc.poly([(-22, 172, 0), (22, 172, 0), (104, -106, 0), (92, -116, 0), (-92, -116, 0), (-104, -106, 0)], "#000", 0.45, blur=True)
    _stand(sc, detail)

    def hang(end, side):
        lane = LANES[(end, side)]
        col = colors[lane] if lane < len(colors) else None
        _, y, z = _axle(end)
        spool.draw_spool(scene, (side * SPOOL_X * SCALE, y * SCALE, z * SCALE), SPOOL_R, col, c, lane, detail=detail)

    # Left face, behind the slab: its holders show only where they pass through each hub.
    hang("back", -1)
    _holder_cap(sc, -SPOOL_X + 35.5, "back", HOLDER)
    hang("front", -1)
    _holder_cap(sc, -SPOOL_X + 35.5, "front", HOLDER)
    _feeder(sc, -1, detail, thick)

    outline = _slab_outline()
    feeder.solid(sc, outline, -SLAB_T, SLAB_T, HOUSING)
    sc.line([(SLAB_T + 0.3, y, z) for y, z in outline + outline[:1]], HOUSING_EDGE, 1.6 * thick, 0.9)
    for end in ("back", "front"):
        _, y, z = _axle(end)
        sc.poly([(SLAB_T + 0.4, y + 34 * math.cos(a), z + 34 * math.sin(a)) for a in (2 * math.pi * i / 28 for i in range(28))], kit.shift(HOUSING, -10))
    _tilted(sc, _ring(0.0, 0.0, 13.0, 16), WAIST - 6.0, 112.0, "#e9eaec")

    hang("back", 1)
    _feeder(sc, 1, detail, thick)
    hang("front", 1)

    scene.layer = "front"
    for end in ("back", "front"):
        x = SPOOL_X + 35.5
        _, y, z = _axle(end)
        ring = [(y + HOLDER_R * math.cos(a), z + HOLDER_R * math.sin(a)) for a in (2 * math.pi * i / 20 for i in range(21))]
        sc.extrude_x(ring, x, HOLDER_TIP, HOLDER, 1.0, front=True)
        _holder_cap(sc, HOLDER_TIP, end, HOLDER_END)


_STAND = int(STAND[1:], 16)
_HOUSING = int(HOUSING[1:], 16)
_WALL = 16.0


def _tray_shell():
    """No walls or lid: the spools hang free, over a slim bar for the stand."""
    return TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=[],
        back_extra_px=0.0,
        front_wall_px=_WALL,
        rail_px=5.0,
        cap_clearance_px=8.0,
        lid_spans_row=False,
        has_latch=False,
        body=_STAND,
        panel=_HOUSING,
        rail=_STAND,
        accent=int(KNOB[1:], 16),
        front=[
            kit.span(0, 0, 0, _WALL, _STAND, 3),
            kit.lane(-11, 11, -8, 0, int(HOLDER[1:], 16), 4),
        ],
        lane_join="hub",
        spool_lift_px=_WALL - 6.0,
    )


UNIT = UnitDef("ams_lite", 4, draw_art, _tray_shell())
