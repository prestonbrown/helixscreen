# SPDX-License-Identifier: GPL-3.0-or-later
"""Bambu Lab AMS HT: one spool, narrow body, a tall arched lid and a display on the front."""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D = 0.27, 0.55
PLINTH_Z, BODY_Z, H = 0.03, 0.36, 0.73
LID_X0, LID_X1, LID_Y0, LID_Y1 = 0.012, W - 0.012, 0.02, D - 0.02
BODY, PLINTH, FACE, SCREEN = "#3a3b40", "#26272b", "#2b2c31", "#0c0d10"
SMOKE, EDGE, RIB, READOUT = "#6a6f78", "#c4c8ce", "#53565e", "#d8dbdf"
SPOOL_X, SPOOL_Y, SPOOL_Z, SPOOL_R = W / 2, D / 2, 0.465, 0.25
RIB_YS = [0.14, 0.22, 0.30, 0.38, 0.46]
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    return kit.arch_profile(LID_Y0, LID_Y1, BODY_Z, H, flat=0.40, riser=0.05)


def _disc(xc, yc, zc, r, axis):
    """A circle facing +x (axis "x") or +z (axis "z")."""
    pts = []
    for i in range(32):
        a = 2 * math.pi * i / 32
        u, v = r * math.cos(a), r * math.sin(a)
        pts.append((xc, yc + u, zc + v) if axis == "x" else (xc + u, yc + v, zc))
    return pts


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    foot = kit.fillet([(0, 0), (0, D), (W, D), (W, 0)], [0.02] * 4, closed=True, steps=6)[::-1]
    plinth = kit.fillet([(0.02, 0.01), (0.02, D - 0.02), (W - 0.02, D - 0.02), (W - 0.02, 0.01)], [0.015] * 4, closed=True, steps=6)[::-1]

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    sc.poly([(LID_X0, y, z) for y, z in prof], SMOKE, 0.5)
    sc.extrude_x(prof, LID_X0, LID_X1, SMOKE, 0.45, front=False)
    sc.poly([(0.0, 0.0, BODY_Z), (W, 0.0, BODY_Z), (W, D, BODY_Z), (0.0, D, BODY_Z)], "#121316")

    spool.draw_spool(sc, (SPOOL_X, SPOOL_Y, SPOOL_Z), SPOOL_R, colors[0], c, 0, detail=detail)

    sc.layer = "front"
    sc.prism(plinth, 0.0, PLINTH_Z, PLINTH, amb=0.7, dif=0.45, spec=0)
    sc.prism(foot, PLINTH_Z, BODY_Z, BODY, amb=0.66, dif=0.5, spec=0.12, power=20)
    y = -0.002
    sc.poly(kit.rrect_front(0.012, W - 0.012, PLINTH_Z + 0.008, BODY_Z - 0.008, 0.012, y), kit.shade(cam, FACE, (0, -1, 0), amb=0.7, dif=0.45, spec=0), 1.0, stroke="#3b3d43", sw=0.002 * thick)
    sc.poly(kit.rrect_front(0.03, W - 0.03, 0.05, BODY_Z - 0.022, 0.008, y - 0.001), SCREEN)
    for x0, x1 in ((W * 0.13, W * 0.245), (W * 0.35, W * 0.465), (W * 0.57, W * 0.8625)):
        sc.poly(kit.rrect_front(x0, x1, BODY_Z - 0.07, BODY_Z - 0.052, 0.002, y - 0.002), READOUT, 0.85)
    if detail:
        for ry in RIB_YS:
            sc.line([(W + 0.001, ry, PLINTH_Z + 0.02), (W + 0.001, ry, BODY_Z - 0.02)], RIB, 0.002)
        sc.line([(W + 0.001, 0.09, PLINTH_Z), (W + 0.001, 0.09, BODY_Z)], "#17181b", 0.003)
    sc.line([(0.0, 0.0, BODY_Z), (W, 0.0, BODY_Z), (W, D, BODY_Z)], "#55585f", 0.003 * thick)

    # The low partition and the filament knob stand under the lid, ahead of the spool.
    sc.poly([(LID_X0 + 0.01, 0.11, BODY_Z), (LID_X1 - 0.01, 0.11, BODY_Z), (LID_X1 - 0.01, 0.11, BODY_Z + 0.115), (LID_X0 + 0.01, 0.11, BODY_Z + 0.115)], "#a9adb4", 0.3)
    knob = _disc(W / 2, 0.06, BODY_Z + 0.022, 0.048, "z")
    sc.prism([(a, b) for a, b, _ in knob], BODY_Z, BODY_Z + 0.022, "#5b5e65", amb=0.8, dif=0.5, spec=0.15, power=16)
    sc.poly(knob, "#7a7d84")

    sc.extrude_x(prof, LID_X0, LID_X1, SMOKE, 0.3 if detail else 0.2, front=True)
    cap = [(LID_X1, y1, z) for y1, z in prof]
    sc.poly(cap, SMOKE, 0.34 if detail else 0.26)
    sc.line(cap, EDGE, 0.004 * thick, 0.75)
    sc.line([(LID_X0, y1, z) for y1, z in prof[: len(prof) // 2]], EDGE, 0.003 * thick, 0.45)
    sc.poly(_disc(LID_X1 + 0.001, D * 0.85, BODY_Z + 0.06, 0.024, "x"), "#101113", 1.0, stroke="#4a4c52", sw=0.002 * thick)


def _tray_shell():
    profile = _lid_profile()
    return TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=kit.tray_profile(profile, BODY_Z, TRAY_PX_PER_UNIT),
        back_extra_px=0.0,
        front_wall_px=BODY_Z * TRAY_PX_PER_UNIT,
        rail_px=8.0,
        cap_clearance_px=7.0,
        lid_spans_row=True,
        has_latch=False,
        body=int(BODY[1:], 16),
        panel=int(FACE[1:], 16),
        rail=0x55585f,
        accent=int(READOUT[1:], 16),
    )


UNIT = UnitDef("ams_ht", 1, draw_art, _tray_shell())

_WALL = UNIT.tray.front_wall_px
UNIT.tray.front = [
    kit.span(3, 3, 3, _WALL - 6, 0x0c0d10, 2),
    kit.span(8, 8, 8, 11, 0xd8dbdf),
]

UNIT.tray.built_in_selector = True
