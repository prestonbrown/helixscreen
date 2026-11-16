# SPDX-License-Identifier: GPL-3.0-or-later
"""A spool in the app's style, its proportions read from the C++ that draws the live one."""
import math
import re
from pathlib import Path

from .kit import dot, scale, shade, shift

_SOURCES = {
    "flange_lift": ("src/ui/filament/ui_spool_canvas.cpp", "kFlangeLift"),
    "flange_drop": ("src/ui/filament/ui_spool_canvas.cpp", "kFlangeDrop"),
    "hub_radius": ("src/ui/filament/ui_spool_canvas.cpp", "kHubRadius"),
    "vent_shade_top": ("src/ui/filament/ui_spool_canvas.cpp", "kVentFilamentShadeTop"),
    "vent_shade_bottom": ("src/ui/filament/ui_spool_canvas.cpp", "kVentFilamentShadeBottom"),
    "hollow_shade_top": ("src/ui/filament/ui_spool_canvas.cpp", "kVentInsideShadeTop"),
    "hollow_shade_bottom": ("src/ui/filament/ui_spool_canvas.cpp", "kVentInsideShadeBottom"),
    "slot_inner": ("src/ui/filament/spool_vents.cpp", "kSlotInner"),
    "slot_outer": ("src/ui/filament/spool_vents.cpp", "kSlotOuter"),
    "slot_half_width": ("src/ui/filament/spool_vents.cpp", "kSlotHalfWidth"),
    "dot_pitch": ("src/ui/filament/spool_vents.cpp", "kDotPitch"),
    "dot_radius": ("src/ui/filament/spool_vents.cpp", "kDotRadius"),
    "dot_inner": ("src/ui/filament/spool_vents.cpp", "kDotBandInner"),
    "dot_outer": ("src/ui/filament/spool_vents.cpp", "kDotBandOuter"),
    "dot_clearance": ("src/ui/filament/spool_vents.cpp", "kDotSlotClearance"),
    "flange_radius": ("include/ams_tray_projection.h", "kSpoolFlangeRadius"),
    "spool_width": ("include/ams_tray_projection.h", "kSpoolWidth"),
}

_FLANGE, _FLANGE_BACK = "#666666", "#444444"


def spool_constants(repo_root):
    """Read the live spool's values, raising KeyError if one has moved."""
    out = {}
    for key, (rel, name) in _SOURCES.items():
        path = Path(repo_root) / rel
        text = path.read_text() if path.is_file() else ""
        m = re.search(rf"\b{name}\s*=\s*([0-9.]+)f?\b", text)
        if not m:
            raise KeyError(f"{name} not found in {rel}")
        out[key] = float(m.group(1))
    return out


def _circle(x, yc, zc, r, n=48):
    return [(x, yc + r * math.cos(2 * math.pi * i / n), zc + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def _stadium(x, yc, zc, a, inner, outer, half, n=10):
    pts = []
    for r0, start in ((outer, a - math.pi / 2), (inner, a + math.pi / 2)):
        cy, cz = yc + r0 * math.cos(a), zc + r0 * math.sin(a)
        for i in range(n + 1):
            t = start + math.pi * i / n
            pts.append((x, cy + half * math.cos(t), cz + half * math.sin(t)))
    return pts


def _dot_sites(c, size):
    """Air-hole centres on the face, by the rule in spool_vents.cpp#dot_site_open."""
    dirs = [(math.cos(math.pi / 2 + k * math.pi / 3), math.sin(math.pi / 2 + k * math.pi / 3)) for k in range(6)]
    pitch = c["dot_pitch"] * size
    keep_out = (c["slot_half_width"] + c["dot_radius"] + c["dot_clearance"]) * size
    n = int(c["dot_outer"] / c["dot_pitch"]) + 1
    for i in range(-n, n + 1):
        for j in range(-n, n + 1):
            u, v = i * pitch, j * pitch
            if not c["dot_inner"] * size < math.hypot(u, v) < c["dot_outer"] * size:
                continue
            d = max(dirs, key=lambda q: u * q[0] + v * q[1])
            t = max(c["slot_inner"] * size, min(c["slot_outer"] * size, u * d[0] + v * d[1]))
            if math.hypot(u - t * d[0], v - t * d[1]) >= keep_out:
                yield u, v


def draw_spool(scene, center, radius, color, c, lane, detail=True, stripes=11):
    """Draw one spool; color None leaves the bare core with hollow vents."""
    scene.spool_radius = radius
    xc, yc, zc = center
    cam = scene.camera
    R = radius
    size = R / c["flange_radius"]
    w = c["spool_width"] * size / 2
    hub = size * c["hub_radius"]
    r = R * 0.93
    x_near, x_far = xc + w, xc - w
    lift, drop = c["flange_lift"], c["flange_drop"]
    scene.poly(
        _circle(x_far, yc, zc, R),
        scene.grad(shift(_FLANGE_BACK, lift / 2), shift(_FLANGE_BACK, -drop), zc + R, zc - R, x_far, yc),
        1.0,
        stroke="#2a2a2a",
        sw=0.001,
    )
    # The bare core stands behind the filament, so it shows when a bay is empty.
    _cylinder(scene, x_far, x_near, yc, zc, hub, lambda n: shade(cam, _FLANGE, n, amb=0.8, dif=0.5, spec=0))
    if color:
        _cylinder(scene, x_far, x_near, yc, zc, r, lambda n: shade(cam, color, n, amb=0.6, dif=0.65, spec=0.18, power=12), lane=lane)
        if detail:
            for k in range(1, stripes + 1):
                x = x_far + (x_near - x_far) * k / (stripes + 1)
                arc = []
                for i in range(61):
                    a = 2 * math.pi * i / 60
                    if dot((0, math.cos(a), math.sin(a)), cam.view) > 0:
                        arc.append((x, yc + r * math.cos(a), zc + r * math.sin(a)))
                arc.sort(key=lambda p: math.atan2(p[2] - zc, p[1] - yc) % (2 * math.pi))
                scene.line(arc, "#000000", 0.0016, 0.22, lane=lane, tag="wrap")

    face = scene.grad(shift(_FLANGE, lift), shift(_FLANGE, -drop), zc + R, zc - R, x_near, yc)
    scene.poly(_circle(x_near, yc, zc, R), face, 1.0, stroke=shift(_FLANGE, 100), sw=0.002)
    if detail:
        hollow = scene.grad(
            shift(_FLANGE_BACK, -c["hollow_shade_top"]), shift(_FLANGE_BACK, -c["hollow_shade_bottom"]), zc + R, zc - R, x_near, yc
        )
        _vents(scene, x_near, yc, zc, size, c, hollow, scale(_FLANGE_BACK, 0.4), None)
        if color:
            lit = scene.grad(shift(color, -c["vent_shade_top"]), shift(color, -c["vent_shade_bottom"]), zc + R, zc - R, x_near, yc)
            lip = scene.grad(
                scale(shift(color, -c["vent_shade_top"]), 0.55), scale(shift(color, -c["vent_shade_bottom"]), 0.55), zc + R, zc - R, x_near, yc
            )
            _vents(scene, x_near, yc, zc, size, c, lit, lip, lane)
    scene.poly(_circle(x_near, yc, zc, hub, 32), scene.grad("#050505", "#404040", zc + hub, zc - hub, x_near, yc))


def _cylinder(scene, x0, x1, yc, zc, r, paint, lane=None, n=64):
    for i in range(n):
        a0, a1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
        mid = (a0 + a1) / 2
        nrm = (0, math.cos(mid), math.sin(mid))
        if dot(nrm, scene.camera.view) <= -0.05:
            continue
        ring = [(yc + r * math.cos(a), zc + r * math.sin(a)) for a in (a0, a1)]
        scene.poly(
            [(x0, *ring[0]), (x1, *ring[0]), (x1, *ring[1]), (x0, *ring[1])],
            paint(nrm),
            lane=lane,
        )


def _vents(scene, x, yc, zc, size, c, fill, lip_fill, lane):
    inner, outer, half = c["slot_inner"] * size, c["slot_outer"] * size, c["slot_half_width"] * size
    lip = size * 3.0 / 240.0
    for k in range(6):
        a = math.pi / 2 + k * math.pi / 3
        scene.poly(_stadium(x, yc, zc, a, inner, outer, half), lip_fill, lane=lane, tag="slot")
        scene.poly(_stadium(x, yc, zc - lip / 2, a, inner, outer, half - lip / 2), fill, lane=lane, tag="slot")
    for u, v in _dot_sites(c, size):
        scene.poly(_circle(x, yc + u, zc + v, c["dot_radius"] * size, 8), fill, lane=lane, tag="air_hole")
