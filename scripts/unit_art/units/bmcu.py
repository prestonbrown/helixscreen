# SPDX-License-Identifier: GPL-3.0-or-later
"""BMCU: four narrow feeder modules side by side, neutral but for a dark blue lever on each.

The real unit holds no spools. They stand on the floor behind it, spread wider than the
narrow body, so each lane still shows its filament. Body lengths below are in units of the
body width; SCALE turns them into the shared scale where a 200 mm spool has radius 0.25.
"""
import math
from pathlib import Path

from .. import feeder, kit, spool
from ..kit import UnitDef

W, D, H = 1.0, 0.44, 0.82
FOOT_Z = 0.1
TOP_D = 0.28
STEP_Z0, STEP_Z1 = 0.36, 0.5
BODY, SHADOW, SEAM = "#c9cbcf", "#7b7d81", "#55575a"
LEVER = "#173a6e"
CENTERS = [0.125, 0.375, 0.625, 0.875]
SPOOL_R = 0.25
# About 120 mm wide, read off the photos against its push-fit fittings.
SCALE = 0.3
SPOOL_PITCH = 0.2
SPOOL_CENTERS = [SCALE / 2 + (i - 1.5) * SPOOL_PITCH for i in range(4)]
SPOOL_Y = SCALE * D + SPOOL_R + 0.02
SPOOL_Z = SPOOL_R
LEVER_HALF, LEVER_Z0, LEVER_Z1 = 0.075, 0.2, 0.56
_REPO = Path(__file__).resolve().parents[3]

# The side profile: a shallow upper section, chamfering out to a deeper lower one.
PROFILE = [(0.0, FOOT_Z), (0.0, H), (TOP_D, H), (TOP_D, STEP_Z1), (D, STEP_Z0), (D, FOOT_Z)]


def _foot(sc, xc):
    outline = kit.fillet(
        [(xc - 0.105, -0.05), (xc - 0.105, D), (xc + 0.105, D), (xc + 0.105, -0.05)], [0.02, 0, 0, 0.02], closed=True, steps=4
    )[::-1]
    sc.prism(outline, 0.0, FOOT_Z, BODY, amb=0.68, dif=0.5, spec=0.1, power=16)
    sc.poly([(a, b, FOOT_Z) for a, b in outline], kit.shift(BODY, 18))


def _lever(sc, xc, detail):
    cam = sc.camera
    half = LEVER_HALF
    sc.poly(kit.rrect_front(xc - half + 0.008, xc + half + 0.012, LEVER_Z0 - 0.012, LEVER_Z1 - 0.004, 0.02, -0.003), SHADOW)
    face = kit.shade(cam, LEVER, (0, -1, 0), amb=0.72, dif=0.5, spec=0)
    sc.poly(kit.rrect_front(xc - half, xc + half, LEVER_Z0, LEVER_Z1, 0.018, -0.02), face, tag="lever")
    # The grip narrows toward its pivot at the top.
    sc.poly(kit.rrect_front(xc - half * 0.72, xc + half * 0.72, LEVER_Z1 - 0.01, LEVER_Z1 + 0.035, 0.012, -0.012), face, tag="lever")
    if detail:
        dimple = [(xc + 0.042 * math.cos(2 * math.pi * i / 28), -0.022, 0.43 + 0.042 * math.sin(2 * math.pi * i / 28)) for i in range(28)]
        sc.poly(dimple, kit.shift(face, 22), tag="lever")


def _fitting(sc, xc):
    y0, y1 = TOP_D / 2 - 0.022, TOP_D / 2 + 0.022
    x0, x1 = xc - 0.022, xc + 0.022
    sc.prism([(x0, y0), (x0, y1), (x1, y1), (x1, y0)][::-1], H, H + 0.045, "#b9bcc1", amb=0.75, dif=0.5, spec=0.35, power=16)
    x0, x1, y0, y1 = xc - 0.014, xc + 0.014, y0 + 0.008, y1 - 0.008
    sc.prism([(x0, y0), (x0, y1), (x1, y1), (x1, y0)][::-1], H + 0.045, H + 0.07, "#202122", amb=0.9, dif=0.5, spec=0.2, power=16)
    sc.poly([(x0, y0, H + 0.07), (x1, y0, H + 0.07), (x1, y1, H + 0.07), (x0, y1, H + 0.07)], "#3a3b3d")


def _body(scene, detail):
    sc = kit.ScaledScene(scene, SCALE)
    thick = 1 if detail else 3
    cam = sc.camera
    for xc in CENTERS:
        _foot(sc, xc)
    side = kit.shade(cam, BODY, (1, 0, 0), amb=0.68, dif=0.5, spec=0)
    sc.poly([(W, y, z) for y, z in PROFILE], side)
    sc.poly([(0.0, 0.0, H), (W, 0.0, H), (W, TOP_D, H), (0.0, TOP_D, H)], kit.shift(BODY, 16))
    front = kit.shade(cam, BODY, (0, -1, 0), amb=0.72, dif=0.5, spec=0)
    sc.poly([(0.0, 0.0, FOOT_Z), (W, 0.0, FOOT_Z), (W, 0.0, H), (0.0, 0.0, H)], front)
    for xc in CENTERS:
        sc.poly(kit.rrect_front(xc - LEVER_HALF - 0.016, xc + LEVER_HALF + 0.016, LEVER_Z0 - 0.03, LEVER_Z1 + 0.05, 0.012, -0.001), kit.scale(front, 0.82))
    for x in (0.25, 0.5, 0.75):
        sc.line([(x, -0.002, FOOT_Z), (x, -0.002, H)], SEAM, 0.005 * thick)
        sc.line([(x, 0.0, H + 0.001), (x, TOP_D, H + 0.001)], SEAM, 0.004 * thick)
    if detail:
        for xc in CENTERS:
            sc.poly(kit.rrect_front(xc - 0.085, xc - 0.03, H - 0.05, H - 0.032, 0.006, -0.002), SHADOW)
            sc.poly(kit.rrect_front(xc - 0.085, xc - 0.03, LEVER_Z1 + 0.085, LEVER_Z1 + 0.1, 0.006, -0.002), SHADOW)
        sc.poly([(W + 0.001, 0.08, 0.62), (W + 0.001, 0.2, 0.62), (W + 0.001, 0.2, 0.76), (W + 0.001, 0.08, 0.76)], SHADOW)
        for y in (0.1, 0.17, 0.24, 0.31):
            for z in (0.18, 0.26):
                sc.poly([(W + 0.001, y + 0.011 * math.cos(2 * math.pi * i / 10), z + 0.011 * math.sin(2 * math.pi * i / 10)) for i in range(10)], SHADOW)
    for xc in CENTERS:
        _lever(sc, xc, detail)
    for xc in CENTERS:
        _fitting(sc, xc)


def draw_art(scene, colors, tier):
    detail = tier == "full"
    c = spool.spool_constants(_REPO)
    x0, x1 = SPOOL_CENTERS[0] - 0.12, SPOOL_CENTERS[-1] + 0.12
    scene.poly([(x0, 0.0, 0), (x1, 0.0, 0), (x1, SPOOL_Y + 0.2, 0), (x0, SPOOL_Y + 0.2, 0)], "#000", 0.5, blur=True)
    for lane, (xc, col) in enumerate(zip(SPOOL_CENTERS, colors)):
        spool.draw_spool(scene, (xc, SPOOL_Y, SPOOL_Z), SPOOL_R, col, c, lane, detail=detail)
    scene.layer = "front"
    _body(scene, detail)


def draw_stubs(scene, colors, tier):
    _body(scene, tier == "full")
    y = TOP_D / 2 * SCALE
    z = (H + 0.07) * SCALE
    for lane, (xc, col) in enumerate(zip(CENTERS, colors)):
        x = xc * SCALE
        feeder.tube(scene, feeder.bend((x, y, z), (x, y, z + 0.13), (x, y + 0.1, z + 0.2)), col, lane, 0.016 if tier == "full" else feeder.TUBE_THUMB)


_LEVER_RGB = int(LEVER[1:], 16)
UNIT = UnitDef("bmcu", 4, draw_art, feeder.tray_strip(int(BODY[1:], 16), _LEVER_RGB), stub_art=draw_stubs)
