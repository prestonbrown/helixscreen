# SPDX-License-Identifier: GPL-3.0-or-later
"""BIGTREETECH ViViD: a black base under a smoked lid with a faceted front."""
import math
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

W, D = 1.0, 0.70
BASE_Z, KNEE_Z, H = 0.21, 0.40, 0.66
LID_X0, LID_X1, LID_Y0, SLOPE_Y = 0.015, 0.985, 0.02, 0.17
BODY, LIP, SMOKE, EDGE = "#34353a", "#44474e", "#5a606b", "#c9ced6"
HEX = "#e4e6e8"
LEDS = ((0.085, 0.195, "#2f6fe0"), (0.215, 0.32, "#2fb54a"), (0.68, 0.785, "#ee5a24"), (0.805, 0.915, "#f4d21f"))
CENTERS = [0.155, 0.375, 0.625, 0.845]
SPOOL_Y, SPOOL_Z, SPOOL_R = 0.35, 0.335, 0.28
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _lid_profile():
    """Up the front wall, back along the sloped upper face, over a flat top, and down the mirrored rear."""
    points = [(LID_Y0, BASE_Z), (LID_Y0, KNEE_Z), (SLOPE_Y, H), (D - SLOPE_Y, H), (D - LID_Y0, KNEE_Z), (D - LID_Y0, BASE_Z)]
    return kit.fillet(points, [0, 0.03, 0.05, 0.05, 0.03, 0], steps=6)


def _on_slope(t):
    """A point on the lid's sloped front face, t from its knee to its top edge."""
    return LID_Y0 + (SLOPE_Y - LID_Y0) * t - 0.002, KNEE_Z + (H - KNEE_Z) * t


def _hexagon(xc, t, r):
    """A hexagon lying on the sloped face."""
    pts = []
    run = math.hypot(SLOPE_Y - LID_Y0, H - KNEE_Z)
    for k in range(6):
        a = math.pi / 2 + k * math.pi / 3
        y, z = _on_slope(t + r * math.sin(a) / run)
        pts.append((xc + r * math.cos(a), y, z))
    return pts


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    foot = kit.fillet([(0, 0), (0, D), (W, D), (W, 0)], [0.03] * 4, closed=True, steps=6)[::-1]

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.55, blur=True)
    sc.poly([(LID_X0, y, z) for y, z in prof], SMOKE, 0.55)
    sc.extrude_x(prof, LID_X0, LID_X1, SMOKE, 0.5, front=False)
    sc.poly([(0.0, 0.0, BASE_Z), (W, 0.0, BASE_Z), (W, D, BASE_Z), (0.0, D, BASE_Z)], "#101113")

    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    sc.prism(foot, 0.0, 0.018, "#121316")
    sc.prism(foot, 0.018, BASE_Z, BODY, amb=0.62, dif=0.5, spec=0.12, power=20)
    y = -0.002
    sc.poly([(0.015, y, 0.022), (0.985, y, 0.022), (0.985, y, 0.05), (0.015, y, 0.05)], kit.shade(cam, LIP, (0, -1, 0), amb=0.72, dif=0.42, spec=0))
    sc.line([(0.015, y, BASE_Z - 0.003), (0.985, y, BASE_Z - 0.003)], "#6a6e77", 0.003 * thick)
    for x0, x1, col in LEDS:
        sc.poly(kit.rrect_x(x0, x1, BASE_Z - 0.04, BASE_Z - 0.024, 0.005, y - 0.001), col)
    if detail:
        for x in (0.3, 0.7):
            sc.line([(x, y - 0.001, 0.022), (x, y - 0.001, 0.05)], "#17181b", 0.003)
        sc.line([(0.4, y - 0.001, BASE_Z - 0.03), (0.6, y - 0.001, BASE_Z - 0.03)], "#17181b", 0.004)

    sc.extrude_x(prof, LID_X0, LID_X1, SMOKE, 0.3 if detail else 0.2, front=True)
    cap = [(LID_X1, y1, z) for y1, z in prof]
    sc.poly(cap, SMOKE, 0.5 if detail else 0.36)
    sc.line(cap, EDGE, 0.004 * thick, 0.7)
    sc.line([(LID_X0, y1, z) for y1, z in prof[: len(prof) // 2]], EDGE, 0.003 * thick, 0.45)
    knee_y, knee_z = _on_slope(0.0)
    sc.line([(LID_X0, knee_y, knee_z), (LID_X1, knee_y, knee_z)], EDGE, 0.003 * thick, 0.35)
    top_y, top_z = _on_slope(1.0)
    sc.line([(LID_X0, top_y, top_z), (LID_X1, top_y, top_z)], EDGE, 0.003 * thick, 0.5)

    sc.poly(_hexagon(0.5, 0.6, 0.042), "#8d9096")
    sc.poly(_hexagon(0.5, 0.6, 0.034), HEX)


def _tray_shell():
    profile = _lid_profile()
    dome = kit.tray_profile(profile, BASE_Z, TRAY_PX_PER_UNIT)
    return TrayShellDef(
        cheek=[(0.0, 0.0), (1.0, 0.0)],
        dome=dome,
        back_extra_px=0.0,
        front_wall_px=BASE_Z * TRAY_PX_PER_UNIT,
        rail_px=8.0,
        cap_clearance_px=7.0,
        lid_spans_row=True,
        has_latch=True,
        body=int(BODY[1:], 16),
        panel=int(LIP[1:], 16),
        rail=0x6a6e77,
        accent=0xe4e6e8,
    )


UNIT = UnitDef("btt_vivid", 4, draw_art, _tray_shell())
