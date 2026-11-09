# SPDX-License-Identifier: GPL-3.0-or-later
"""Quattro Box: a faceted DIY enclosure, its width set by how many lanes it was built with."""
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

# Fractions of a four-lane body, read off the front photo.
CAP, PITCH = 0.151, 0.2325
D, H = 0.75, 0.81
SPOOL_Y, SPOOL_Z, SPOOL_R = 0.375, 0.47, 0.27
BODY, BODY_DARK, ACCENT, GLOW, GLASS = "#3b3836", "#292725", "#7b2fd0", "#62e6a0", "#6f7379"
ROLLER = "#2a2b2e"
WINDOW_Z0, WINDOW_Z1, WALL_TOP = 0.197, 0.665, 0.68
FEED_TOP = 0.295
# Front foot to rear foot, chamfered at all four corners.
PROFILE = [(0.09, 0.0), (0.0, 0.085), (0.0, WALL_TOP), (0.13, H), (0.62, H), (D, 0.62), (D, 0.17), (0.64, 0.0)]
SIDE_WINDOW = [(0.12, 0.36), (0.12, 0.66), (0.22, 0.77), (0.55, 0.77), (0.68, 0.62), (0.68, 0.52)]
LID = [(0.0, FEED_TOP), (0.0, WALL_TOP), (0.13, H), (0.62, H), (D, 0.62), (D, FEED_TOP)]
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def width(lanes):
    return 2 * CAP + (lanes - 1) * PITCH


def centers(lanes):
    return [CAP + i * PITCH for i in range(lanes)]


def _with_hole(outer, inner):
    """One polygon with a hole: the inner loop runs the other way, bridged from the outer."""
    return outer + [outer[0]] + inner[::-1] + [inner[-1]]


def _window(w):
    x0, x1, z0, z1, cut = 0.045, w - 0.045, WINDOW_Z0, WINDOW_Z1, 0.035
    return [(x0, z0), (x1, z0), (x1, z1 - cut), (x1 - cut, z1), (x0 + cut, z1), (x0, z1 - cut)]


def _feeder(sc, xc, detail):
    """One lane\x27s drive behind the glass: a purple carrier between two rings."""
    y = 0.03
    sc.poly(kit.rrect_front(xc - 0.056, xc + 0.056, WINDOW_Z0, FEED_TOP, 0.012, y), ACCENT)
    sc.poly(kit.rrect_front(xc - 0.038, xc + 0.038, WINDOW_Z0 + 0.012, FEED_TOP - 0.012, 0.008, y - 0.001), "#4a1d84")
    if detail:
        for dz in (0.0, 0.03):
            z = WINDOW_Z0 + 0.03 + dz
            sc.line([(xc - 0.024, y - 0.002, z), (xc, y - 0.002, z + 0.018), (xc + 0.024, y - 0.002, z)], GLOW, 0.007)
        for dx in (-0.09, 0.09):
            sc.poly(kit.rrect_front(xc + dx - 0.007, xc + dx + 0.007, WINDOW_Z0, FEED_TOP - 0.008, 0.005, y), ACCENT)
    else:
        sc.poly(kit.rrect_front(xc - 0.02, xc + 0.02, WINDOW_Z0 + 0.03, FEED_TOP - 0.03, 0.004, y - 0.002), GLOW)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    lanes = len(colors)
    w = width(lanes)

    sc.poly([(0.0, 0.03, 0), (w + 0.03, 0.03, 0), (w + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    sc.poly([(0.01, D - 0.01, 0.17), (w - 0.01, D - 0.01, 0.17), (w - 0.01, D - 0.01, 0.62), (0.01, D - 0.01, 0.62)], "#141312")
    sc.poly([(0.012, y, z) for y, z in PROFILE], "#1b1a19")
    sc.poly([(0.01, 0.02, WINDOW_Z0), (w - 0.01, 0.02, WINDOW_Z0), (w - 0.01, D - 0.02, WINDOW_Z0), (0.01, D - 0.02, WINDOW_Z0)], "#201f1e")
    for lane, (xc, col) in enumerate(zip(centers(lanes), colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    for fx in (0.09, w - 0.13):
        for fy in (0.12, D - 0.17):
            sc.prism([(fx + 0.04, fy), (fx + 0.04, fy + 0.04), (fx, fy + 0.04), (fx, fy)], -0.018, 0.0, "#101010")
    sc.poly(kit.rrect_front(0.02, w - 0.02, WINDOW_Z0, FEED_TOP - 0.018, 0.0, 0.05), ROLLER)
    for xc in centers(lanes):
        _feeder(sc, xc, detail)

    front = kit.shade(cam, BODY, (0, -1, 0), amb=0.72, dif=0.42, spec=0)
    outer = [(0.0, 0.085), (w, 0.085), (w, WALL_TOP), (0.0, WALL_TOP)]
    sc.poly([(x, 0.0, z) for x, z in _with_hole(outer, _window(w))], front)
    sc.poly([(x, -0.001, z) for x, z in _window(w)], GLASS, 0.14 if detail else 0.08)
    sc.line([(x, -0.002, z) for x, z in _window(w) + _window(w)[:1]], "#55524e", 0.0025 * thick)
    sc.poly([(0.0, -0.001, 0.085), (w, -0.001, 0.085), (w, -0.001, WINDOW_Z0 - 0.012), (0.0, -0.001, WINDOW_Z0 - 0.012)], kit.scale(front, 0.82))

    for a, b in zip(PROFILE + PROFILE[:1], PROFILE[1:] + PROFILE[:1]):
        if a[0] == 0.0 and b[0] == 0.0:
            continue
        n = kit.norm((0, -(b[1] - a[1]), b[0] - a[0]))
        if kit.dot(n, cam.view) > 0:
            sc.poly([(0.0, a[0], a[1]), (w, a[0], a[1]), (w, b[0], b[1]), (0.0, b[0], b[1])], kit.shade(cam, BODY, n, amb=0.72, dif=0.42, spec=0))

    lid = [(0.07, 0.17, H + 0.001), (w - 0.07, 0.17, H + 0.001), (w - 0.07, 0.585, H + 0.001), (0.07, 0.585, H + 0.001)]
    sc.poly(lid, kit.shift(GLASS, -28), 0.9)

    side = kit.shade(cam, BODY, (1, 0, 0), amb=0.72, dif=0.42, spec=0)
    sc.poly([(w, y, z) for y, z in _with_hole(PROFILE, SIDE_WINDOW)], side)
    sc.poly([(w + 0.001, y, z) for y, z in SIDE_WINDOW], GLASS, 0.16)
    sc.line([(w + 0.002, y, z) for y, z in SIDE_WINDOW + SIDE_WINDOW[:1]], "#55524e", 0.0025 * thick)
    sc.poly([(w + 0.003, 0.05, 0.13), (w + 0.003, 0.09, 0.13), (w + 0.003, 0.09, 0.25), (w + 0.003, 0.05, 0.25)], ACCENT)
    sc.line([(0.0, -0.002, WALL_TOP), (w, -0.002, WALL_TOP), (w, 0.0, 0.085)], "#5d5a56", 0.003 * thick, 0.8)


_TRAY_WALL = 36.0
UNIT = UnitDef(
    "quattro_box",
    4,
    draw_art,
    TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=kit.tray_profile(LID, FEED_TOP, TRAY_PX_PER_UNIT),
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
        # A type-B unit: every lane merges at the hub built into the enclosure, and one tube leaves.
        built_in_selector=True,
        front=[
            kit.span(2, 2, 3, _TRAY_WALL - 2, 0x2a2826, 3),
            kit.lane(-11, 11, 5, 24, 0x7b2fd0, 2),
            kit.lane(-5, 5, 9, 20, 0x62e6a0, 1),
        ],
    ),
    lane_variants=(2, 4, 5),
)
