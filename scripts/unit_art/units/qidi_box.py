# SPDX-License-Identifier: GPL-3.0-or-later
"""QIDI Box: a wide silver base whose sides climb steeply to the rear, under a smoked lid.

Read off QIDI's product renders (front and three-quarter). Four spools stand side by side,
axles left to right, each in a bay between tall black dividers, sunk into the base so only its
upper three fifths shows above the black floor at the base's front top edge. The lid's front
pane is shallow; its roof climbs gently to the rear and clears the spools, and its sides slope
down to the base, whose sides climb toward the rear. A white slider latch sits on the base's
top edge, over a darker band across the top of the front.
"""
from pathlib import Path

from .. import kit, spool
from ..kit import TrayShellDef, UnitDef

SPOOL_R = 0.25
# The spools sink into the base: only their upper three fifths show above its front top edge,
# and the whole spool stands under the lid, whose front pane is shallower than the base. The
# renders' base is about a third of the height; the shared frame holds no taller unit, so the
# lid is lower than that and the base keeps the height the sunken spools need.
W, D = 1.0, 0.66
ZF = 0.32  # the base's front top edge, and the floor of the bays
ZB = 0.57  # the base's side top at the rear
LID_FRONT, LID_REAR = 0.57, 0.73
BODY, SEAM, BAND = "#b8bbc1", "#8f9298", "#9c9fa5"
SMOKE, FRAME = "#2f3239", "#1b1c20"
FLOOR, DIVIDER = "#141518", "#0f1013"
LATCH = "#eef0f3"
CENTERS = [W * f for f in (0.16, 0.385, 0.615, 0.84)]
SPOOL_Y = D * 0.48
# Three fifths of the spool above the base's top edge.
SPOOL_Z = ZF + 0.6 * 2 * SPOOL_R - SPOOL_R
TRAY_PX_PER_UNIT = 46.2 / SPOOL_R
_REPO = Path(__file__).resolve().parents[3]


def _side_z(y):
    """The height of the base's sloped side top at depth y: the line the lid sits on."""
    return ZF + (ZB - ZF) * y / D


def _lid_profile():
    """A vertical front pane flush with the base's front, a roof climbing to the rear, and a short
    rear pane down onto the base's high rear corner."""
    pts = [(0.0, ZF), (0.0, LID_FRONT), (D, LID_REAR), (D, ZB)]
    return kit.fillet(pts, [0, 0.01, 0.01, 0], steps=4)


def draw_art(sc, colors, tier):
    detail = tier == "full"
    thick = 1 if detail else 3
    c = spool.spool_constants(_REPO)
    cam = sc.camera
    prof = _lid_profile()
    side = [(0.0, ZF), (D, ZB), (D, 0.0), (0.0, 0.0)]

    sc.poly([(0.0, 0.02, 0), (W + 0.03, 0.02, 0), (W + 0.03, D + 0.03, 0), (0, D + 0.03, 0)], "#000", 0.5, blur=True)
    kit.lid_back(sc, prof, 0.0, W, SMOKE, 0.4, 0.42)
    # The bays: a black floor at the base's front top edge, a dark back wall, tall dividers.
    sc.poly([(0.01, D - 0.02, ZF), (W - 0.01, D - 0.02, ZF), (W - 0.01, D - 0.02, ZB), (0.01, D - 0.02, ZB)], "#0d0e10")
    sc.poly([(0.01, 0.01, ZF), (W - 0.01, 0.01, ZF), (W - 0.01, D - 0.02, ZF), (0.01, D - 0.02, ZF)], FLOOR)
    for gx in [(CENTERS[i] + CENTERS[i + 1]) / 2 for i in range(3)]:
        top_front, top_rear = LID_FRONT - 0.03, LID_REAR - 0.04
        sc.poly([(gx, 0.06, ZF), (gx, D - 0.03, ZF), (gx, D - 0.03, top_rear), (gx, 0.06, top_front)], DIVIDER)
    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        spool.draw_spool(sc, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)

    sc.layer = "front"
    # The bays' floor in front of the spools hides their sunken lower parts.
    sc.poly([(0.01, 0.0, ZF), (W - 0.01, 0.0, ZF), (W - 0.01, SPOOL_Y - 0.17, ZF), (0.01, SPOOL_Y - 0.17, ZF)], FLOOR)
    for fx in (0.06, W - 0.14):
        sc.prism([(fx, 0.03), (fx, 0.1), (fx + 0.08, 0.1), (fx + 0.08, 0.03)], 0.0, 0.012, "#2a2b2f", amb=0.9, dif=0.3, spec=0)
    z0 = 0.012
    front = kit.shade(cam, BODY, (0, -1, 0), amb=0.76, dif=0.3, spec=0)
    sc.poly([(0, 0, z0), (W, 0, z0), (W, 0, ZF), (0, 0, ZF)], front)
    sc.poly([(W, y, max(z, z0)) for y, z in side], kit.shade(cam, BODY, (1, 0, 0), amb=0.64, dif=0.34, spec=0))
    # The darker band across the top of the front, under the latch.
    sc.poly([(0, -0.001, ZF - 0.07), (W, -0.001, ZF - 0.07), (W, -0.001, ZF - 0.035), (0, -0.001, ZF - 0.035)], kit.shade(cam, BAND, (0, -1, 0), amb=0.8, dif=0.3, spec=0))
    sc.line([(W, 0, ZF), (W, D, ZB)], "#eef0f2", 0.003 * thick, 0.9)
    sc.line([(0, -0.002, ZF), (W, -0.002, ZF)], "#eef0f2", 0.003 * thick)
    sc.line([(W, 0, z0), (W, 0, ZF)], "#f1f2f4", 0.003 * thick, 0.9)
    if detail:
        sc.line([(0, -0.002, ZF - 0.07), (W, -0.002, ZF - 0.07)], SEAM, 0.002)

    # The lid: its black foot along the base's top, the smoked glass, a dark frame on its edges.
    sc.poly([(0, -0.003, ZF), (W, -0.003, ZF), (W, -0.003, ZF + 0.035), (0, -0.003, ZF + 0.035)], FRAME, 0.95)
    kit.lid_front(sc, prof, 0.0, W, SMOKE, 0.42 if detail else 0.34, 0.2 if detail else 0.15)
    roof = [p for p in prof if p[1] >= LID_FRONT - 0.002]
    sc.extrude_x(roof, 0.0, W, SMOKE, 0.55, front=True)
    sc.line([(W, y, z) for y, z in prof], FRAME, 0.004 * thick, 0.9)
    sc.line([(0.0, y, z) for y, z in prof[:7]], FRAME, 0.003 * thick, 0.7)
    rim = [p for p in prof if p[0] < 0.04 and p[1] > LID_FRONT - 0.02]
    rim_y, rim_z = rim[len(rim) // 2]
    sc.line([(0.0, rim_y - 0.001, rim_z), (W, rim_y - 0.001, rim_z)], "#9ea2a9", 0.004 * thick, 0.9)

    # The slider latch on the base's top edge, at the middle.
    xm = W / 2
    sc.poly(kit.rrect_front(xm - 0.07, xm + 0.07, ZF - 0.03, ZF + 0.03, 0.01, -0.006), LATCH, 1.0, stroke=SEAM, sw=0.002)
    if detail:
        for dx in (0.02, 0.035, 0.05):
            sc.line([(xm + dx, -0.008, ZF - 0.02), (xm + dx, -0.008, ZF + 0.02)], SEAM, 0.0018)


def _tray_shell():
    dome = kit.tray_profile(_lid_profile(), ZF, TRAY_PX_PER_UNIT)
    back_extra = (ZB - ZF) * TRAY_PX_PER_UNIT
    return TrayShellDef([(0.0, 0.0), (1.0, back_extra)], dome, back_extra, 40.0, 8.0, 6.0, True, True, 0xb8bbc1, 0xa9acb2, 0xeef0f3, 0x2f3239)


UNIT = UnitDef("qidi_box", 4, draw_art, _tray_shell())

UNIT.tray.front = [
    kit.span(2, 2, 7, 11, 0x9c9fa5),
    kit.center(-13, 13, 1, 7, 0xeef0f3, 2),
    kit.center(-2, 2, 2.5, 5.5, 0x8f9298),
]

UNIT.tray.combiner = "external"
