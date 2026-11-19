# SPDX-License-Identifier: GPL-3.0-or-later
"""YAMMU: a tall 2020-extrusion cabinet with a glass door, two stacked drawers of four spools.

Happy Hare reports each drawer as a unit of four gates, so each unit draws as its drawer's
slice of the cabinet, and views of the whole device draw the cabinet.

Each drawer holds two coaxial spools across by two deep. The cabinet stands on a skirt with
hex grills that houses the electronics. Proportions from the project's manual (extrusions
A 420 deep, B 500 tall, C 200 wide) and its renders.
"""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

S = 1 / 400  # world units per millimetre: a 200 mm spool draws at the shared 0.25 radius
EX = 20 * S  # 2020 extrusion
W, D = 240 * S, 460 * S
SKIRT = 55 * S
TOP = SKIRT + 500 * S  # the uprights run the full height, the rails sit between them
SHELF = SKIRT + 223 * S  # the upper drawer's support rails, centred 233 mm up the uprights
SPOOL_R = 100 * S
CENTERS_X = [80 * S, 160 * S]
FRONT_Y, BACK_Y = 132 * S, 330 * S
LOWER_Z = SKIRT + EX + 101 * S
UPPER_Z = SHELF + EX + 105 * S
FRAME, FRAME_EDGE = "#1c1d20", "#3a3c41"
PANEL, GLASS = "#8d939b", "#a9b2bc"
SKIRT_C, GRILL = "#202124", "#4b4e54"
ACCENT = "#d8432f"
CARRIER = "#2b2d31"
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _bar(sc, x0, x1, y0, y1, z0, z1, color=FRAME):
    """A box along an axis, its camera-facing faces shaded."""
    cam = sc.camera
    faces = [
        ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
        ((1, 0, 0), [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)]),
        ((-1, 0, 0), [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)]),
        ((0, 1, 0), [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)]),
        ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
    ]
    for n, pts in faces:
        if kit.dot(n, cam.view) > 0:
            sc.poly(pts, kit.shade(cam, color, n, amb=0.85, dif=0.6, spec=0.25, power=18))


def _hex_grill(sc, u0, u1, v0, v1, to_plane, cell):
    """Rows of hex openings on a flat face, (u, v) mapped through to_plane."""
    row, v = 0, v0 + cell * 1.1
    while v < v1 - cell * 0.8:
        u = u0 + cell * 1.2 + (cell if row % 2 else 0)
        while u < u1 - cell * 0.9:
            pts = [(u + cell * 0.8 * math.cos(math.pi / 6 + k * math.pi / 3), v + cell * 0.8 * math.sin(math.pi / 6 + k * math.pi / 3)) for k in range(6)]
            sc.poly([to_plane(a, b) for a, b in pts], GRILL, tag="grill")
            u += 2 * cell
        v += 1.72 * cell
        row += 1


def _carrier(sc, z):
    """A drawer's slide rails and cross members, under its spools."""
    _bar(sc, EX, EX + 14 * S, EX, D - EX, z, z + 10 * S, CARRIER)
    _bar(sc, W - EX - 14 * S, W - EX, EX, D - EX, z, z + 10 * S, CARRIER)
    for y in (FRONT_Y, BACK_Y):
        _bar(sc, EX, W - EX, y - 45 * S, y - 35 * S, z, z + 10 * S, CARRIER)
        _bar(sc, EX, W - EX, y + 35 * S, y + 45 * S, z, z + 10 * S, CARRIER)


def _drawer(sc, z_floor, zc, colors, first_lane, c, detail):
    """One drawer of four: back row first, then front, left to right."""
    _carrier(sc, z_floor)
    for y, row in ((BACK_Y, 2), (FRONT_Y, 0)):
        for i, xc in enumerate(CENTERS_X):
            lane = first_lane + row + i
            spool.draw_spool(sc, (xc, y, zc), SPOOL_R, colors[lane], c, lane, detail=detail)
            # A lane leaves its drawer through the floor under its spool, so the back pair's
            # columns stand clear of the front pair's.
            kit.outlet(sc, lane, (xc, y, z_floor))


def _skirt(sc, detail):
    """Hex grills front and side over the electronics bay, rubber feet under it."""
    for fx, fy in ((6 * S, 6 * S), (W - 30 * S, 6 * S), (W - 30 * S, D - 30 * S)):
        _bar(sc, fx, fx + 24 * S, fy, fy + 24 * S, 0, 8 * S, "#111214")
    _bar(sc, 0, W, 0, D, 8 * S, SKIRT, SKIRT_C)
    cell = (11 if detail else 18) * S
    _hex_grill(sc, 0, W, 8 * S, SKIRT, lambda u, v: (u, -0.001, v), cell)
    _hex_grill(sc, 0, D, 8 * S, SKIRT, lambda u, v: (W + 0.001, u, v), cell)


def _draw(sc, colors, tier, upper, lower):
    """The cabinet, or one drawer's slice of it cut at the shelf between the drawers.

    The upper slice runs from the shelf to the roof; the lower one from the floor through the
    skirt, open at the shelf. Stacked, the two make the whole cabinet. Lanes count from the
    top drawer down.
    """
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    zb = SKIRT if lower else SHELF
    zt = TOP if upper else SHELF
    ground = 0 if lower else zb
    sc.poly([(0, 0.02, ground), (W + 0.04, 0.02, ground), (W + 0.04, D + 0.04, ground), (0, D + 0.04, ground)], "#000", 0.5, blur=True)
    if lower:
        _skirt(sc, detail)

    # Inside: the far panels, the floor and the far frame members.
    sc.poly([(0, D, zb), (W, D, zb), (W, D, zt), (0, D, zt)], kit.shift(PANEL, -45))
    sc.poly([(0, 0, zb), (0, D, zb), (0, D, zt), (0, 0, zt)], kit.shift(PANEL, -55))
    sc.poly([(0, 0, zb), (W, 0, zb), (W, D, zb), (0, D, zb)], "#16171a")
    _bar(sc, 0, EX, D - EX, D, zb, zt)
    _bar(sc, 0, W, D - EX, D, zb, zb + EX)
    _bar(sc, 0, EX, 0, D, zb, zb + EX)
    if lower:
        _drawer(sc, SKIRT + EX, LOWER_Z, colors, 4 if upper else 0, c, detail)
    if upper:
        if lower:
            _bar(sc, 0, EX, 0, D, SHELF, SHELF + EX)
        _drawer(sc, SHELF + EX, UPPER_Z, colors, 0, c, detail)

    sc.layer = "front"
    # The near frame members: the right side's rails, the front sills and the uprights, which
    # run to a cut; the top also carries the roof frame.
    for z in [zb] + ([SHELF] if upper and lower else []):
        _bar(sc, W - EX, W, 0, D, z, z + EX)
        _bar(sc, EX, W - EX, 0, EX, z, z + EX)
    _bar(sc, W - EX, W, D - EX, D, zb, zt)
    _bar(sc, 0, EX, 0, EX, zb, zt)
    _bar(sc, W - EX, W, 0, EX, zb, zt)
    if upper:
        _bar(sc, 0, EX, 0, D, TOP - EX, TOP)
        _bar(sc, W - EX, W, 0, D, TOP - EX, TOP)
        _bar(sc, EX, W - EX, 0, EX, TOP - EX, TOP)
        _bar(sc, EX, W - EX, D - EX, D, TOP - EX, TOP)

    # Panels: polycarbonate sides and roof, a glass door on the front.
    panel_top = zt - EX if upper else zt
    side = [(W + 0.0015, EX, zb + EX), (W + 0.0015, D - EX, zb + EX), (W + 0.0015, D - EX, panel_top), (W + 0.0015, EX, panel_top)]
    sc.poly(side, kit.shade(cam, PANEL, (1, 0, 0), amb=0.8, dif=0.3, spec=0), 0.42 if detail else 0.5)
    if upper:
        roof = [(EX, EX, TOP + 0.001), (W - EX, EX, TOP + 0.001), (W - EX, D - EX, TOP + 0.001), (EX, D - EX, TOP + 0.001)]
        sc.poly(roof, kit.shade(cam, PANEL, (0, 0, 1), amb=0.8, dif=0.3, spec=0), 0.55)
    door = [(EX, -0.002, zb + EX), (W - EX, -0.002, zb + EX), (W - EX, -0.002, panel_top), (EX, -0.002, panel_top)]
    sc.poly(door, GLASS, 0.16 if detail else 0.12)
    if detail:
        sc.line([(EX + 30 * S, -0.003, panel_top - 30 * S), (EX + 90 * S, -0.003, panel_top - 150 * S)], "#ffffff", 0.004, 0.18)
        sc.line([(EX + 60 * S, -0.003, panel_top - 25 * S), (EX + 150 * S, -0.003, panel_top - 205 * S)], "#ffffff", 0.0025, 0.12)
    if upper:
        sc.line([(W + 0.002, 0, TOP), (W + 0.002, D, TOP)], FRAME_EDGE, 0.003 * thick, 0.9)
        sc.line([(0, -0.002, TOP), (W, -0.002, TOP)], FRAME_EDGE, 0.003 * thick, 0.9)
    sc.line([(W + 0.002, -0.002, zb), (W + 0.002, -0.002, zt)], FRAME_EDGE, 0.003 * thick, 0.9)

    # Door hardware in the accent colour: a latch at each outer end, and a handle on each
    # upright rising from the shelf, as on the real door.
    if upper:
        for x0 in (2 * S, W - 18 * S):
            _bar(sc, x0, x0 + 16 * S, -9 * S, -1 * S, SHELF + 8 * S, SHELF + 98 * S, ACCENT)
        _bar(sc, W / 2 - 14 * S, W / 2 + 14 * S, -7 * S, -1 * S, TOP - 16 * S, TOP - 4 * S, ACCENT)
    if lower:
        _bar(sc, W / 2 - 14 * S, W / 2 + 14 * S, -7 * S, -1 * S, SKIRT + 4 * S, SKIRT + 16 * S, ACCENT)


def draw_art(sc, colors, tier):
    _draw(sc, colors, tier, upper=True, lower=True)


def draw_upper_art(sc, colors, tier):
    _draw(sc, colors, tier, upper=True, lower=False)


def draw_lower_art(sc, colors, tier):
    _draw(sc, colors, tier, upper=False, lower=True)


def _tray_shell():
    # One drawer on the tray: its floor rail is the front wall, its glass rises to the shelf.
    rise = (SHELF - (SKIRT + EX)) * TRAY_PX_PER_UNIT
    dome = [(0.0, 0.0), (0.0, rise), (1.0, rise), (1.0, 0.0)]
    return TrayShellDef([(0.0, 0.0), (1.0, 0.0)], dome, 0.0, 30.0, 8.0, 6.0, True, True, 0x1c1d20, 0x202124, 0x3a3c41, 0xd8432f)


# The whole cabinet, for views of the device; each Happy Hare unit draws as its drawer's slice.
UNIT = UnitDef("yammu", 8, draw_art, _tray_shell())
UPPER = UnitDef("yammu_upper", 4, draw_upper_art, _tray_shell())
LOWER = UnitDef("yammu_lower", 4, draw_lower_art, _tray_shell())

UNIT.tray.front = UPPER.tray.front = LOWER.tray.front = [
    kit.span(2, 2, 2, UNIT.tray.front_wall_px - 2, 0x202124, 2),
    kit.span(4, 4, 0, 5, 0x3a3c41, 1),
    kit.center(-12, 12, 8, 13, 0xd8432f, 2),
]

# A drawer's four paths join in its splitter, and the two drawers join in a Y in the back grill:
# one tube leaves the cabinet.
UNIT.tray.combiner = UPPER.tray.combiner = LOWER.tray.combiner = "internal"
