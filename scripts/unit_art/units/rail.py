# SPDX-License-Identifier: GPL-3.0-or-later
"""Selector rails: a long linear unit along x, one gate block per lane, and a sliding carriage."""
from dataclasses import dataclass

from .. import kit
from ..kit import TrayShellDef, UnitDef
from . import ercf_face, tradrack_face

# Lengths are in scene units along the rail (u), across it (v), and up (z).
PITCH = 0.19
LEFT, RIGHT = 0.5, 0.24
GATE_V0, GATE_V1, GATE_Z0, GATE_Z = 0.08, 0.34, 0.07, 0.2
ROD_Z, ROD_R = 0.32, 0.016
FRONT_ROD, REAR_ROD, BELT_V = 0.12, 0.38, 0.25
BLOCK_HALF = 0.078
STUB_LEN = 0.21
STUB_R = 0.042
CARRIAGE_HALF = 0.085
GATE_COUNTS = (4, 6, 8, 9, 12)


@dataclass(frozen=True)
class RailStyle:
    id: str
    frame: str
    dark: str
    accent: str
    coupler: str
    motor: str
    rod: str
    chain: bool
    head: str


def gate_u(gate):
    return LEFT + gate * PITCH


def length(gates):
    return LEFT + (gates - 1) * PITCH + RIGHT


def _box(sc, u0, u1, v0, v1, z0, z1, color, **kw):
    sc.prism([(u1, v0), (u1, v1), (u0, v1), (u0, v0)], z0, z1, color, amb=0.72, dif=0.42, spec=0)
    sc.poly([(u0, v0, z1), (u1, v0, z1), (u1, v1, z1), (u0, v1, z1)], kit.shade(sc.camera, color, (0, 0, 1), amb=0.72, dif=0.42, spec=0), **kw)


def _rod(sc, u0, u1, v, z, color, width):
    sc.line([(u0, v, z), (u1, v, z)], kit.scale(color, 0.55), width * 1.5)
    sc.line([(u0, v, z + width * 0.2), (u1, v, z + width * 0.2)], color, width)


def _foot(sc, u, style):
    _box(sc, u - 0.05, u + 0.05, -0.14, 0.5, -0.12, -0.08, style.frame)
    _box(sc, u - 0.035, u + 0.035, 0.1, 0.34, -0.08, 0.07, style.frame)


def _gate(sc, gate, style, color, detail):
    u = gate_u(gate)
    _box(sc, u - BLOCK_HALF, u + BLOCK_HALF, GATE_V0, GATE_V1, GATE_Z0, GATE_Z, style.dark)
    _box(sc, u - BLOCK_HALF * 0.7, u + BLOCK_HALF * 0.7, GATE_V0 + 0.06, GATE_V1 - 0.03, GATE_Z, GATE_Z + 0.035, style.accent)
    # The filament leaves the block's front through a coupler, as a short tintable stub.
    r, z = STUB_R, 0.135
    v0, v1 = GATE_V0 - 0.02, GATE_V0 - 0.02 - STUB_LEN
    sc.poly([(u - r, v0, z), (u + r, v0, z), (u + r, v1, z - 0.05), (u - r, v1, z - 0.05)], color, lane=gate)
    if detail:
        sc.line([(u - r * 0.35, v0, z + 0.001), (u - r * 0.35, v1, z - 0.049)], "#ffffff", 0.012, 0.4)
    _box(sc, u - 0.05, u + 0.05, GATE_V0 - 0.024, GATE_V0, GATE_Z0 + 0.025, GATE_Z - 0.015, style.coupler)


def _chain(sc, length_, style, detail):
    """A cable chain arching over the rail's far side."""
    v = 0.46
    steps = 14 if detail else 6
    prev = None
    for i in range(steps + 1):
        t = i / steps
        u = 0.12 + (length_ - 0.24) * t
        z = 0.4 + 0.42 * (1 - (2 * t - 1) ** 2)
        if prev:
            sc.line([(prev[0], v, prev[1]), (u, v, z)], "#101012", 0.06)
            sc.line([(prev[0], v, prev[1]), (u, v, z)], "#2d2d32", 0.03)
        prev = (u, z)


def _carriage(sc, style, detail):
    u = gate_u(0)
    half = CARRIAGE_HALF
    sc.layer = "carriage"
    # Rear rod clip, the bridge across the rods, then the head on the front rod.
    _box(sc, u - half * 0.7, u + half * 0.7, REAR_ROD - 0.05, REAR_ROD + 0.05, ROD_Z - 0.02, ROD_Z + 0.1, style.accent)
    _box(sc, u - half * 0.7, u + half * 0.7, FRONT_ROD, REAR_ROD, ROD_Z + 0.015, ROD_Z + 0.055, style.dark)
    if style.chain:
        # Tradrack's filament drive rides on the carriage.
        _box(sc, u - 0.1, u + 0.1, REAR_ROD - 0.12, REAR_ROD + 0.07, ROD_Z + 0.1, ROD_Z + 0.3, style.motor)
        _box(sc, u - 0.11, u + 0.11, REAR_ROD - 0.13, REAR_ROD + 0.08, ROD_Z + 0.09, ROD_Z + 0.125, style.accent)
    _box(sc, u - half, u + half, FRONT_ROD - 0.08, FRONT_ROD + 0.07, ROD_Z - 0.19, ROD_Z + 0.09, style.head)
    _box(sc, u - half * 0.8, u + half * 0.8, FRONT_ROD - 0.05, FRONT_ROD + 0.05, ROD_Z + 0.09, ROD_Z + 0.12, style.accent)
    sc.poly(kit.rrect_front(u - 0.028, u + 0.028, ROD_Z - 0.17, ROD_Z - 0.114, 0.012, FRONT_ROD - 0.082), style.coupler)
    if detail:
        sc.poly(kit.rrect_front(u - half * 0.75, u + half * 0.75, ROD_Z - 0.06, ROD_Z + 0.04, 0.008, FRONT_ROD - 0.082), kit.shift(style.head, 18))


def _draw(style, sc, colors, tier):
    detail = tier == "full"
    gates = len(colors)
    end = length(gates)
    rail0, rail1 = LEFT - 0.1, end - RIGHT + 0.1
    sc.poly([(0.02, -0.14, -0.12), (end, -0.14, -0.12), (end, 0.52, -0.12), (0.02, 0.52, -0.12)], "#000", 0.5, blur=True)

    if style.chain:
        _chain(sc, end, style, detail)
    _foot(sc, 0.14, style)
    # Far end: a block carrying the rods and the selector motor.
    _box(sc, 0.04, rail0, 0.02, REAR_ROD + 0.08, 0.04, ROD_Z + 0.06, style.dark)
    _box(sc, 0.06, rail0 - 0.03, 0.1, 0.34, ROD_Z + 0.06, ROD_Z + 0.26, style.motor)
    _box(sc, 0.05, rail0 - 0.02, 0.1, 0.34, ROD_Z + 0.06, ROD_Z + 0.085, style.accent)
    _box(sc, rail0, rail1, GATE_V0 + 0.02, GATE_V1 + 0.02, 0.02, GATE_Z0, style.dark)
    for gate, color in enumerate(colors):
        _gate(sc, gate, style, color, detail)
    _rod(sc, rail0, end - 0.05, REAR_ROD, ROD_Z, style.rod, ROD_R * 1.6)
    sc.line([(rail0, BELT_V, ROD_Z - 0.03), (end - 0.05, BELT_V, ROD_Z - 0.03)], "#1b1515", 0.028)
    if detail:
        sc.line([(rail0, BELT_V, ROD_Z - 0.018), (end - 0.05, BELT_V, ROD_Z - 0.018)], "#7a4436", 0.008)
    _rod(sc, rail0, end - 0.05, FRONT_ROD, ROD_Z, style.rod, ROD_R * 1.6)
    _foot(sc, end - 0.1, style)

    sc.layer = "front"
    _box(sc, end - 0.1, end, GATE_V0 - 0.02, REAR_ROD + 0.08, 0.04, ROD_Z + 0.06, style.dark)
    _box(sc, end - 0.09, end - 0.01, FRONT_ROD - 0.05, REAR_ROD + 0.05, ROD_Z + 0.06, ROD_Z + 0.09, style.accent)

    _carriage(sc, style, detail)


def _step(gates):
    return (PITCH, 0.0, 0.0)


def _unit(style, body, panel, rail_color, face_art):
    return UnitDef(
        style.id,
        6,
        lambda sc, colors, tier: _draw(style, sc, colors, tier),
        TrayShellDef(
            cheek=[(0.0, 0.0), (1.0, 0.0)],
            dome=[],
            back_extra_px=0.0,
            front_wall_px=22.0,
            rail_px=6.0,
            cap_clearance_px=6.0,
            lid_spans_row=False,
            has_latch=False,
            body=body,
            panel=panel,
            rail=rail_color,
            accent=rail_color,
        ),
        lane_variants=GATE_COUNTS,
        carriage_step=_step,
        face_art=face_art,
    )


# Colours read off the photos: printed red on black, and black with green latches and blue couplers.
ERCF = _unit(RailStyle("ercf_rail", "#c8211f", "#1d1d20", "#d42a26", "#3f8fd9", "#26262a", "#b9bdc2", False, "#222226"), 0x1d1d20, 0x2a2a2e, 0xc8211f, ercf_face.draw)
TRADRACK = _unit(RailStyle("tradrack_rail", "#1c1d20", "#18191b", "#25c275", "#3f9edd", "#2f3134", "#a7abb0", True, "#1f2024"), 0x18191b, 0x26272a, 0x25c275, tradrack_face.draw)
