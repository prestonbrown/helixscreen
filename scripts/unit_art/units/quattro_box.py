# SPDX-License-Identifier: GPL-3.0-or-later
"""Quattro Box: four spools on rollers under a faceted, hinged smoked-glass lid.

Proportions come from the open-source v2 design (Batalhoti/QuattroBox): its base, panel
cut files and renders. 1.0 is 400 mm.
"""
import math
from pathlib import Path

from .. import feeder, kit, spool
from ..kit import TrayShellDef, UnitDef

MM = 1 / 400
LANES = 4
CAP, PITCH = 0.151, 0.2325
W = 2 * CAP + (LANES - 1) * PITCH
FOOT = 8 * MM
# The corner facets between the sides and every face around them.
BEVEL = 22 * MM


def _mm(pts):
    return [(y * MM, z * MM + FOOT) for y, z in pts]


# Side outline, front foot up over the ridge and down the back. The base holds the lower
# lip and the rear wall below the hinge; the lid is everything above its sloped top edge.
PROFILE = _mm([(25, 0), (0, 25), (0, 56), (16, 282), (50, 304), (160, 322), (240, 285), (268, 160), (262, 60), (246, 0)])
D = max(y for y, _ in PROFILE)
H = max(z for _, z in PROFILE)
FRONT_PANE = (PROFILE[2], PROFILE[3])
TOP_FACET = (PROFILE[4], PROFILE[5])
# Where the lid meets the base: level at the front lip, then rising to the rear hinge.
SEAM = _mm([(0, 56), (16, 56), (42, 95), (266, 170)])
SIDE_SEAM = _mm([(22, 66), (42, 95), (244, 163)])
SIDE_WINDOW = _mm([(36, 112), (50, 250), (78, 276), (160, 296), (222, 266), (242, 180)])
WINDOW_Z0, WINDOW_Z1 = 77 * MM + FOOT, 258 * MM + FOOT
WINDOW_W, WINDOW_CUT_X, WINDOW_CUT_Z = 335 * MM, 22 * MM, 32 * MM
TOP_GLASS_W = 338 * MM

SPOOL_R = 0.25
SPOOL_Y, SPOOL_Z = 156 * MM, 199 * MM + FOOT
ROLLER_R = 20 * MM
ROLLER_Z = 95 * MM + FOOT
ROLLER_REAR_Y = SPOOL_Y + 60 * MM
# The front drum is the drive roller: as tall as the feeder block that straddles it, and
# placed so the spool rests on it.
FRONT_R = 30 * MM
FRONT_Z = 118 * MM + FOOT
FRONT_Y = SPOOL_Y - ((SPOOL_R + FRONT_R) ** 2 - (SPOOL_Z - FRONT_Z) ** 2) ** 0.5
# The feeder's block stands before the drum, a thin black cap on its top.
FEEDER_Y0, FEEDER_Y1 = 22 * MM, 46 * MM
FEEDER_Z0, FEEDER_Z1 = 92 * MM + FOOT, 152 * MM + FOOT
# The tray's lid stands on the feeder row.
FEED_TOP = 118 * MM + FOOT

BODY, INSIDE, ACCENT, ACCENT_DARK, GLOW, GLASS = "#3b3836", "#1a1918", "#7b2fd0", "#4a1d84", "#62e6a0", "#2c2f34"
ROLLER = "#1e1d1c"
RUBBER = "#3d3733"
EDGE = "#5d5a56"
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def width(lanes=LANES):
    return 2 * CAP + (lanes - 1) * PITCH


def centers(lanes=LANES):
    return [CAP + i * PITCH for i in range(lanes)]


def _with_hole(outer, inner):
    """One polygon with a hole: the inner loop runs the other way, bridged from the outer."""
    return outer + [outer[0]] + inner[::-1] + [inner[-1]]


def _inset(poly, d):
    """A convex outline moved in by d along every edge's normal."""
    n = len(poly)
    area = sum(poly[i][0] * poly[(i + 1) % n][1] - poly[(i + 1) % n][0] * poly[i][1] for i in range(n))
    sign = 1 if area > 0 else -1
    lines = []
    for i in range(n):
        (ay, az), (by, bz) = poly[i], poly[(i + 1) % n]
        dy, dz = by - ay, bz - az
        length = (dy * dy + dz * dz) ** 0.5
        ny, nz = -dz / length * sign, dy / length * sign
        lines.append(((ay + ny * d, az + nz * d), (dy, dz)))
    out = []
    for i in range(n):
        (p1, d1), (p2, d2) = lines[i - 1], lines[i]
        den = d1[0] * d2[1] - d1[1] * d2[0]
        t = ((p2[0] - p1[0]) * d2[1] - (p2[1] - p1[1]) * d2[0]) / den
        out.append((p1[0] + d1[0] * t, p1[1] + d1[1] * t))
    return out


def _along(edge, t):
    """The point a fraction t of the way along a (y, z) edge."""
    (y0, z0), (y1, z1) = edge
    return (y0 + (y1 - y0) * t, z0 + (z1 - z0) * t)


def _on_pane(x, z):
    """A point on the front pane, which leans back as it rises."""
    (y0, z0), (y1, z1) = FRONT_PANE
    return (x, y0 + (y1 - y0) * (z - z0) / (z1 - z0), z)


def _front_window():
    x0 = (W - WINDOW_W) / 2
    x1 = x0 + WINDOW_W
    z0, z1, cx, cz = WINDOW_Z0, WINDOW_Z1, WINDOW_CUT_X, WINDOW_CUT_Z
    return [(x0, z0), (x1, z0), (x1, z1 - cz), (x1 - cx, z1), (x0 + cx, z1), (x0, z1 - cz)]


def _top_glass():
    """The top glass on the rising roof facet, as (x, t) along that facet."""
    x0 = (W - TOP_GLASS_W) / 2
    return [(x0, 0.08), (x0 + TOP_GLASS_W, 0.08), (x0 + TOP_GLASS_W, 0.92), (x0, 0.92)]


def _on_top(x, t):
    y, z = _along(TOP_FACET, t)
    return (x, y, z)


def _rollers(sc, xc, y):
    """One lane's pair of drums on one axle line, split around the lane's middle."""
    half, reach = 0.026, 0.1
    rim = {"flange_k": 1.22, "flange_w": 0.008, "sheen": "#4a4744", "end_face": ACCENT_DARK}
    kit.roller(sc, xc - reach, xc - half, y, ROLLER_Z, ROLLER_R, ROLLER, ACCENT, **rim)
    kit.roller(sc, xc + half, xc + reach, y, ROLLER_Z, ROLLER_R, ROLLER, ACCENT, **rim)


def _drum(sc, x0, x1):
    """A front drum's rubber barrel, shaded strip by strip so it reads round."""
    cam = sc.camera
    ring = kit.circle_yz(FRONT_Y, FRONT_Z, FRONT_R, 32)
    for a, b in zip(ring, ring[1:]):
        n = kit.norm((0, (a[0] + b[0]) / 2 - FRONT_Y, (a[1] + b[1]) / 2 - FRONT_Z))
        if kit.dot(n, cam.view) > 0:
            fill = kit.shade(cam, RUBBER, n, amb=0.55, dif=0.7, spec=0.12, power=10)
            sc.poly([(x0, a[0], a[1]), (x1, a[0], a[1]), (x1, b[0], b[1]), (x0, b[0], b[1])], fill)


def _front_rollers(sc, xc, detail):
    """One lane's drive drum: black rubber either side of its feeder, a purple rim at each outer end."""
    half, reach, flange_w = 0.04, PITCH / 2 - 0.024, 0.012
    rim = {"flange_k": 1.2, "flange_w": flange_w, "end_face": ACCENT, "solid": True}
    kit.roller(sc, xc - reach, xc - half, FRONT_Y, FRONT_Z, FRONT_R, RUBBER, ACCENT, right_flange=False, **rim)
    _drum(sc, xc - reach, xc - half)
    kit.roller(sc, xc + half, xc + reach, FRONT_Y, FRONT_Z, FRONT_R, RUBBER, ACCENT, left_flange=False, **rim)
    _drum(sc, xc + half, xc + reach)
    x = xc + reach + flange_w + 0.0005
    sc.poly([(x, a, b) for a, b in kit.circle_yz(FRONT_Y, FRONT_Z, FRONT_R * 1.2)], kit.shift(ACCENT, 12))
    sc.poly([(x + 0.0005, a, b) for a, b in kit.circle_yz(FRONT_Y, FRONT_Z, FRONT_R * 0.85)], kit.shift(ACCENT, -22))


def _feeder(sc, xc, detail):
    """One lane's drive in front of its rollers: a purple block, a black cap and its LED button."""
    half = 0.046
    cap = 0.008
    feeder.box(sc, xc - half, xc + half, FEEDER_Y0, FEEDER_Y1, FEEDER_Z0, FEEDER_Z1 - cap, ACCENT)
    feeder.box(sc, xc - half + 0.004, xc + half - 0.004, FEEDER_Y0 + 0.004, FEEDER_Y1, FEEDER_Z1 - cap, FEEDER_Z1, "#202020")
    y = FEEDER_Y0 - 0.001
    sc.poly(kit.rrect_front(xc - 0.03, xc + 0.03, FEEDER_Z0 + 0.062, FEEDER_Z1 - cap - 0.006, 0.006, y), ACCENT_DARK)
    if detail:
        for dz in (0.0, 0.026):
            z = FEEDER_Z0 + 0.076 + dz
            sc.line([(xc - 0.018, y - 0.001, z), (xc, y - 0.001, z + 0.015), (xc + 0.018, y - 0.001, z)], GLOW, 0.007)
    else:
        sc.poly(kit.rrect_front(xc - 0.018, xc + 0.018, FEEDER_Z0 + 0.075, FEEDER_Z1 - 0.02, 0.004, y - 0.001), GLOW)


def _strip(sc, cam, edge, x0, x1, hole=None, to3=None):
    """One face of the body across its width between the side bevels, with an optional hole."""
    (ay, az), (by, bz) = edge
    n = kit.norm((0, -(bz - az), by - ay))
    if kit.dot(n, cam.view) <= 0:
        return
    fill = kit.shade(cam, BODY, n, amb=0.72, dif=0.42, spec=0)
    if hole is None:
        sc.poly([(x0, ay, az), (x1, ay, az), (x1, by, bz), (x0, by, bz)], fill)
        return
    outer = [(x0, 0.0), (x1, 0.0), (x1, 1.0), (x0, 1.0)]
    sc.poly([to3(x, t) for x, t in _with_hole(outer, hole)], fill)


def _bevels(sc, cam, x_flat, x_edge):
    """One end's bevel band: from the full outline at x_edge in to the flat side at x_flat."""
    inner = _inset(PROFILE, BEVEL)
    out = 1 if x_flat > x_edge else -1
    n = len(PROFILE)
    for i in range(n):
        a, b = PROFILE[i], PROFILE[(i + 1) % n]
        ia, ib = inner[i], inner[(i + 1) % n]
        u = (0.0, b[0] - a[0], b[1] - a[1])
        v = (x_flat - x_edge, ia[0] - a[0], ia[1] - a[1])
        nrm = kit.norm((u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]))
        if nrm[0] * out < 0:
            nrm = tuple(-c for c in nrm)
        if kit.dot(nrm, cam.view) > 0:
            fill = kit.shade(cam, BODY, nrm, amb=0.72, dif=0.42, spec=0)
            sc.poly([(x_edge, a[0], a[1]), (x_edge, b[0], b[1]), (x_flat, ib[0], ib[1]), (x_flat, ia[0], ia[1])], fill)


def _side(sc, cam, detail):
    """The right end: its flat side cut for its window, the seam and the lid's latch."""
    x = W
    flat = kit.shade(cam, BODY, (1, 0, 0), amb=0.72, dif=0.42, spec=0)
    sc.poly([(x, y, z) for y, z in _with_hole(_inset(PROFILE, BEVEL), SIDE_WINDOW)], flat)
    sc.poly([(x + 0.001, y, z) for y, z in SIDE_WINDOW], GLASS, 0.5 if detail else 0.6)
    sc.line([(x + 0.002, y, z) for y, z in SIDE_WINDOW + SIDE_WINDOW[:1]], EDGE, 0.003 if detail else 0.008)
    if detail:
        sc.line([(x + 0.002, y, z) for y, z in SIDE_SEAM], "#121110", 0.003, 0.9)
    y0, z0 = SIDE_SEAM[0]
    sc.prism([(x + 0.012, y0), (x + 0.012, y0 + 0.045), (x, y0 + 0.045), (x, y0)], z0 - 0.01, z0 + 0.06, ACCENT, amb=0.8, dif=0.5, spec=0.1)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    x0, x1 = BEVEL, W - BEVEL

    sc.poly([(0.0, 0.03, 0), (W + 0.03, 0.03, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    for fx in (0.07, W - 0.11):
        for fy in (0.04, D - 0.08):
            sc.prism([(fx + 0.04, fy), (fx + 0.04, fy + 0.04), (fx, fy + 0.04), (fx, fy)], 0.0, FOOT, "#101010")
    # Inside the shell: its far walls and roof, the left wall and the floor under the rollers.
    wall = _inset(PROFILE, BEVEL + 0.004)
    sc.extrude_x(wall, BEVEL, W - BEVEL, INSIDE, 1.0, front=False)
    sc.poly([(BEVEL, y, z) for y, z in _inset(PROFILE, BEVEL)], "#141312")
    floor_z = ROLLER_Z - 0.02
    sc.poly([(BEVEL, 0.05, floor_z), (W - BEVEL, 0.05, floor_z), (W - BEVEL, D - 0.05, floor_z), (BEVEL, D - 0.05, floor_z)], "#211f1e")
    for xc in centers():
        _rollers(sc, xc, ROLLER_REAR_Y)
    for lane, (xc, col) in enumerate(zip(centers(), colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    for xc in centers():
        _front_rollers(sc, xc, detail)
    for xc in centers():
        _feeder(sc, xc, detail)
    # The base's slanted front under the feeder row, inside the lid.
    sc.poly([(0.02, 16 * MM, 56 * MM + FOOT), (W - 0.02, 16 * MM, 56 * MM + FOOT), (W - 0.02, 42 * MM, 95 * MM + FOOT), (0.02, 42 * MM, 95 * MM + FOOT)], "#242221")

    n = len(PROFILE)
    for i in range(n):
        edge = (PROFILE[i], PROFILE[(i + 1) % n])
        if edge == FRONT_PANE:
            _strip(sc, cam, edge, x0, x1, [(x, (z - edge[0][1]) / (edge[1][1] - edge[0][1])) for x, z in _front_window()],
                   lambda x, t: _on_pane(x, edge[0][1] + (edge[1][1] - edge[0][1]) * t))
        elif edge == TOP_FACET:
            _strip(sc, cam, edge, x0, x1, _top_glass(), _on_top)
        else:
            _strip(sc, cam, edge, x0, x1)
    window = [_on_pane(x, z) for x, z in _front_window()]
    sc.poly([(x, y - 0.001, z) for x, y, z in window], GLASS, 0.32 if detail else 0.2)
    sc.line([(x, y - 0.002, z) for x, y, z in window + window[:1]], EDGE, 0.0025 * thick)
    top = [_on_top(x, t) for x, t in _top_glass()]
    sc.poly([(x, y, z + 0.001) for x, y, z in top], GLASS, 0.88 if detail else 0.92)
    sc.line([(x, y, z + 0.002) for x, y, z in top + top[:1]], EDGE, 0.0025 * thick)
    if detail:
        seam_y, seam_z = SEAM[0]
        sc.line([(x0, seam_y - 0.002, seam_z), (x1, seam_y - 0.002, seam_z)], "#121110", 0.003)

    _bevels(sc, cam, 0.0, BEVEL)
    _bevels(sc, cam, W, W - BEVEL)
    _side(sc, cam, detail)


def _lid_above(z_base):
    """The lid's outline above a height, front to back, cut where it crosses that height."""
    out = []
    pts = PROFILE[2:9]
    for a, b in zip(pts, pts[1:]):
        if (a[1] - z_base) * (b[1] - z_base) < 0:
            t = (z_base - a[1]) / (b[1] - a[1])
            out.append(_along((a, b), t))
        if b[1] > z_base:
            out.append(b)
    return out


_TRAY_WALL = 36.0
UNIT = UnitDef(
    "quattro_box",
    LANES,
    draw_art,
    TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=kit.tray_profile(_lid_above(FEED_TOP), FEED_TOP, TRAY_PX_PER_UNIT),
        back_extra_px=0.0,
        front_wall_px=_TRAY_WALL,
        rail_px=10.0,
        cap_clearance_px=9.0,
        lid_spans_row=True,
        has_latch=False,
        body=0x3b3836,
        panel=0x292725,
        rail=0x55524e,
        accent=0x7b2fd0,
        # The lanes leave the box separately and merge in a hub mounted outside it.
        combiner="external",
        front=[
            kit.span(2, 2, 3, _TRAY_WALL - 2, 0x2a2826, 3),
            kit.lane(-11, 11, 5, 24, 0x7b2fd0, 2),
            kit.lane(-5, 5, 9, 20, 0x62e6a0, 1),
        ],
    ),
)
