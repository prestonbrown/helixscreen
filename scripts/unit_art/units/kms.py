# SPDX-License-Identifier: GPL-3.0-or-later
"""KMS: a grey printed box with an arched lid between solid end caps."""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D = 1.0, 0.72
BASE_Z, H = 0.36, 0.67
LID_X0, LID_X1, LID_Y0, LID_Y1 = 0.02, 0.98, 0.08, 0.70
CAP_BAND = 0.075
BODY, CAP, GLASS = "#55575b", "#4b4d51", "#8f959e"
WINDOW = "#c4c7cb"
CENTERS = [0.155, 0.385, 0.615, 0.845]
SPOOL_Y, SPOOL_Z, SPOOL_R = 0.39, 0.37, 0.27
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    return kit.arch_profile(LID_Y0, LID_Y1, BASE_Z, H, riser=0.02)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    foot = kit.fillet([(0, 0), (0, D), (W, D), (W, 0)], [0.025] * 4, closed=True, steps=6)[::-1]

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    sc.poly([(0.0, 0.0, BASE_Z), (W, 0.0, BASE_Z), (W, D, BASE_Z), (0.0, D, BASE_Z)], kit.shift(BODY, 14))
    sc.poly([(LID_X0 + CAP_BAND, y, z) for y, z in prof], kit.shade(cam, CAP, (1, 0, 0), amb=0.6, dif=0.5, spec=0))
    sc.extrude_x(prof, LID_X0, LID_X1, kit.scale(CAP, 0.55), 1.0, front=False)
    sc.poly([(0.02, LID_Y0, BASE_Z), (W - 0.02, LID_Y0, BASE_Z), (W - 0.02, LID_Y1, BASE_Z), (0.02, LID_Y1, BASE_Z)], "#17181a")

    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    sc.prism(foot, 0.0, BASE_Z, BODY, amb=0.72, dif=0.5, spec=0.12, power=20)
    y = -0.002
    # The base's top nearest the camera stands in front of the spools.
    for x0, x1, y0, y1 in ((0.0, W, 0.0, LID_Y0), (LID_X1, W, 0.0, D)):
        sc.poly([(x0, y0, BASE_Z), (x1, y0, BASE_Z), (x1, y1, BASE_Z), (x0, y1, BASE_Z)], kit.shift(BODY, 14))
    sc.line([(0.0, 0.0, BASE_Z), (W, 0.0, BASE_Z)], "#7c7f85", 0.003 * thick)
    for x0, x1 in ((0.075, 0.485), (0.515, 0.925)):
        sc.poly(kit.rrect_front(x0, x1, 0.07, 0.29, 0.02, y), WINDOW, 0.6)
    sc.poly(
        [(0.5 + 0.045 * math.cos(2 * math.pi * i / 24), 0.04 + 0.022 * math.sin(2 * math.pi * i / 24), BASE_Z + 0.001) for i in range(24)],
        "#08090a",
    )

    sc.extrude_x(prof, LID_X0 + CAP_BAND, LID_X1 - CAP_BAND, GLASS, 0.14 if detail else 0.1, front=True)
    for x0, x1 in ((LID_X0, LID_X0 + CAP_BAND), (LID_X1 - CAP_BAND, LID_X1)):
        sc.extrude_x(prof, x0, x1, CAP, 1.0, front=True)
    cap = [(LID_X1, y1, z) for y1, z in prof]
    sc.poly(cap, kit.shade(cam, CAP, (1, 0, 0), amb=0.72, dif=0.5, spec=0))
    sc.line(cap, "#7c7f85", 0.003 * thick, 0.8)
    for x, op in ((LID_X0 + CAP_BAND, 0.7), (LID_X1 - CAP_BAND, 0.7)):
        sc.line([(x, y1, z) for y1, z in prof[: len(prof) * 2 // 3]], "#26272a", 0.004 * thick, op)


def _tray_shell():
    profile = _lid_profile()
    return TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=kit.tray_profile(profile, BASE_Z, TRAY_PX_PER_UNIT),
        back_extra_px=0.0,
        front_wall_px=BASE_Z * TRAY_PX_PER_UNIT,
        rail_px=8.0,
        cap_clearance_px=12.0,
        lid_spans_row=True,
        has_latch=False,
        body=int(BODY[1:], 16),
        panel=int(CAP[1:], 16),
        rail=0x7c7f85,
        accent=int(WINDOW[1:], 16),
    )


UNIT = UnitDef("kms", 4, draw_art, _tray_shell())
