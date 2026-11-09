# SPDX-License-Identifier: GPL-3.0-or-later
"""Ender 3: the K1's cover in matte black, folded back sharply at mid-height, with vents instead of a fan."""
from ..head import COLLET_HEIGHT, Drawing, HeadDef, wordmark
from .creality_k1 import COVER_TOP, HOLE, TIP, draw_body, draw_cover, lean as k1_lean

SHELL, MARK = "#3c3d42", "#e9eaec"
FOLD_Z, FOLD_SETBACK = 0.31, 0.2

def lean(z):
    """Flat from the top curve down to the fold, then one plane angled back to the bottom edge."""
    if z < FOLD_Z:
        return FOLD_SETBACK * (FOLD_Z - z) / FOLD_Z
    return k1_lean(z)


def _on_face(d, outline, color):
    d.facet([(x, lean(z) - 0.002, z) for x, z in outline], color, amb=0.9, dif=0.2, spec=0.0)


def _vent_row(d, z0, z1, count, half):
    for i in range(count):
        x = -0.26 + 0.52 * i / (count - 1)
        _on_face(d, [(x - half, z0), (x + half, z0), (x + half, z1), (x - half, z1)], HOLE)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    draw_body(d)
    d.collet(0.0, 0.46, COVER_TOP)
    draw_cover(d, SHELL, face_lean=lean)
    count, half = (11, 0.012) if d.detail else (6, 0.02)
    _vent_row(d, 0.05, 0.25, count, half)
    _vent_row(d, 0.84, 0.9, count, half)
    if d.detail:
        wordmark(d, "CREALITY", 0.0, 0.6, 0.032, 0.05, 0.012, MARK)


HEAD = HeadDef("ender3", "ENDER3", draw_art, TIP, entry=(0.0, 0.46, COVER_TOP + COLLET_HEIGHT), size=0.85)
