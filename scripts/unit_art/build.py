#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bake every unit's layers into assets/images/units and draw a contact sheet."""
import base64
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from unit_art import kit  # noqa: E402
from unit_art.units import ace_pro  # noqa: E402

UNITS = [ace_pro.UNIT]
TIER_WIDTH = {"full": 320, "thumb": 40}
SAMPLE = ["#e2452f", "#ef7a2b", "#2e7fd0", "#d8dadd", "#63a83c", "#6a5597", "#3f9fb5", "#c9b458"]


def rsvg_render(svg_text, width, png_path):
    svg_path = png_path.with_suffix(".svg")
    svg_path.write_text(svg_text)
    try:
        subprocess.run(["rsvg-convert", "-w", str(width), "-o", str(png_path), str(svg_path)], check=True)
    finally:
        svg_path.unlink()


def bake_unit(unit, out_dir, render=rsvg_render):
    target = Path(out_dir) / unit.id
    target.mkdir(parents=True, exist_ok=True)
    manifest = {"id": unit.id, "lanes": unit.lanes, "tiers": {}}
    for tier, width in TIER_WIDTH.items():
        scene = kit.Scene(kit.ART_CAMERA)
        # White filament: its shading alone reaches the shade masks.
        unit.draw_art(scene, ["#ffffff"] * unit.lanes, tier)
        back, front = f"{tier}_back.png", f"{tier}_front.png"
        render(scene.layer_svg("back", width), width, target / back)
        render(scene.layer_svg("front", width), width, target / front)
        lanes = []
        for lane in range(unit.lanes):
            names = {"color": f"{tier}_lane{lane}.png", "shade": f"{tier}_lane{lane}_shade.png"}
            render(scene.lane_color_svg(lane, width), width, target / names["color"])
            render(scene.lane_shade_svg(lane, width), width, target / names["shade"])
            lanes.append(names)
        manifest["tiers"][tier] = {"width": width, "back": back, "front": front, "lanes": lanes}
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def _png_uri(path):
    return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()


def contact_sheet(out_dir, sheet_path):
    """Every unit composed from its real layers, on dark and light cards."""
    # An SVG mask recolours each lane the way the runtime will.
    cells, x_step, y = [], 360, 0
    for bg in ("#1a1b1e", "#e8e8ea"):
        x = 0
        cells.append(f'<rect x="0" y="{y}" width="{x_step * max(1, len(UNITS))}" height="290" fill="{bg}"/>')
        for unit in UNITS:
            root = Path(out_dir) / unit.id
            m = json.loads((root / "manifest.json").read_text())
            for tier, ox in (("full", 10), ("thumb", 335 - 40)):
                t = m["tiers"][tier]
                w = t["width"]
                h = w
                cells.append(f'<image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / t["back"])}"/>')
                for lane, names in enumerate(t["lanes"]):
                    mask_id = f"m{bg[1:]}{unit.id}{tier}{lane}"
                    cells.append(
                        f'<mask id="{mask_id}"><image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / names["color"])}"/></mask>'
                        f'<rect x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" fill="{SAMPLE[lane]}" mask="url(#{mask_id})"/>'
                        f'<image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / names["shade"])}"/>'
                    )
                cells.append(f'<image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / t["front"])}"/>')
            x += x_step
        y += 290
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{x_step * max(1, len(UNITS))}" height="{y}">{"".join(cells)}</svg>'
    sheet_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = sheet_path.with_suffix(".svg")
    tmp.write_text(svg)
    subprocess.run(["rsvg-convert", "-o", str(sheet_path), str(tmp)], check=True)


def main():
    out = REPO / "assets/images/units"
    for unit in UNITS:
        bake_unit(unit, out)
    contact_sheet(out, REPO / "build/unit-art/contact_sheet.png")


if __name__ == "__main__":
    main()
