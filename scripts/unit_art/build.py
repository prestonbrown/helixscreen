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

from unit_art import head as head_kit, kit  # noqa: E402
from unit_art.heads import a4t, anthead, bambu, creality_k1, creality_k2, dragonburner, artillery_m1, qidi, sovol_sv08, elegoo_centauri, ender3, flashforge_ad5, flashforge_creator5, snapmaker_u1, xol, jabberwocky, mini_stealthburner, stealthburner  # noqa: E402
from unit_art.units import (  # noqa: E402
    ace_pro,
    ad5x_ifs,
    ams_2_pro,
    ams_ht,
    bmcu,
    box_turtle,
    btt_vivid,
    cfs,
    elegoo_canvas,
    htlf,
    kms,
    mmx,
    night_owl,
    qidi_box,
    quattro_box,
    rail,
    three_ms,
)

UNITS = [
    ace_pro.UNIT,
    box_turtle.UNIT,
    box_turtle.ENCLOSED,
    ams_2_pro.UNIT,
    cfs.UNIT,
    qidi_box.UNIT,
    btt_vivid.UNIT,
    kms.UNIT,
    ams_ht.UNIT,
    bmcu.UNIT,
    night_owl.UNIT,
    quattro_box.UNIT,
    mmx.UNIT,
    htlf.UNIT,
    three_ms.UNIT,
    ad5x_ifs.UNIT,
    elegoo_canvas.UNIT,
    rail.ERCF,
    rail.TRADRACK,
]
HEADS = [
    stealthburner.HEAD,
    mini_stealthburner.HEAD,
    dragonburner.HEAD,
    a4t.HEAD,
    anthead.HEAD,
    jabberwocky.HEAD,
    bambu.HEAD,
    creality_k1.HEAD,
    creality_k2.HEAD,
    ender3.HEAD,
    xol.HEAD,
    snapmaker_u1.HEAD,
    flashforge_ad5.HEAD,
    flashforge_creator5.HEAD,
    elegoo_centauri.HEAD,
    artillery_m1.HEAD,
    qidi.HEAD,
    sovol_sv08.HEAD,
]
HEAD_TIER_HEIGHT = {"full": 160, "path": 72}
TIER_WIDTH = {"full": 320, "thumb": 40}
SAMPLE = ["#e2452f", "#ef7a2b", "#2e7fd0", "#d8dadd", "#63a83c", "#6a5597", "#3f9fb5", "#c9b458"]


def rsvg_render(svg_text, width, png_path):
    svg_path = png_path.with_suffix(".svg")
    svg_path.write_text(svg_text)
    try:
        subprocess.run(["rsvg-convert", "-w", str(width), "-o", str(png_path), str(svg_path)], check=True)
    finally:
        svg_path.unlink()


def art_dirs(unit, out_dir):
    """Each set of baked layers a unit has, with its lane count."""
    root = Path(out_dir) / unit.id
    if unit.lane_variants:
        return [(root / str(lanes), lanes) for lanes in unit.lane_variants]
    return [(root, unit.lanes)]


def stub_dirs(unit, out_dir):
    """Each set of baked layers of a unit's stub look; empty when it has none."""
    if unit.stub_art is None:
        return []
    root = Path(out_dir) / unit.id / "stubs"
    if unit.lane_variants:
        return [(root / str(lanes), lanes) for lanes in unit.lane_variants]
    return [(root, unit.lanes)]


def bake_unit(unit, out_dir, render=rsvg_render):
    for target, lanes in art_dirs(unit, out_dir):
        _bake_layers(unit.id, unit.draw_art, target, lanes, render, unit.carriage_step is not None)
    for target, lanes in stub_dirs(unit, out_dir):
        _bake_layers(unit.id, unit.stub_art, target, lanes, render, False)


def _bake_layers(unit_id, draw, target, lane_count, render, has_carriage):
    target.mkdir(parents=True, exist_ok=True)
    manifest = {"id": unit_id, "lanes": lane_count, "tiers": {}}
    for tier, width in TIER_WIDTH.items():
        scene = kit.Scene(kit.ART_CAMERA)
        # White filament: its shading alone reaches the shade masks.
        draw(scene, ["#ffffff"] * lane_count, tier)
        back, front = f"{tier}_back.png", f"{tier}_front.png"
        render(scene.layer_svg("back", width), width, target / back)
        render(scene.layer_svg("front", width), width, target / front)
        lanes = []
        for lane in range(lane_count):
            names = {"color": f"{tier}_lane{lane}.png", "shade": f"{tier}_lane{lane}_shade.png"}
            render(scene.lane_color_svg(lane, width), width, target / names["color"])
            render(scene.lane_shade_svg(lane, width), width, target / names["shade"])
            lanes.append(names)
        manifest["tiers"][tier] = {"width": width, "back": back, "front": front, "lanes": lanes}
        if has_carriage:
            carriage = f"{tier}_carriage.png"
            render(scene.layer_svg("carriage", width), width, target / carriage)
            manifest["tiers"][tier]["carriage"] = carriage
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def generate_shell_table(units):
    """The C++ table of every unit's tray shape."""

    def fronts(parts):
        return ", ".join(
            f"{{FrontAnchor::{a}, {x0:.2f}f, {x1:.2f}f, {y0:.2f}f, {y1:.2f}f, 0x{c:06x}, {r}}}"
            for a, x0, x1, y0, y1, c, r in parts
        )

    def points(pts):
        return ", ".join(f"{{{d:.4f}f, {h:.2f}f}}" for d, h in pts)

    def carriage_rows(unit):
        rows = []
        for gates in unit.lane_variants:
            scene = kit.Scene(kit.ART_CAMERA)
            unit.draw_art(scene, ["#ffffff"] * gates, "full")
            step_x, step_y = kit.carriage_step(scene, unit.carriage_step(gates))
            _, _, vw, vh = scene._viewbox()
            rows.append(f"{{{gates}, {step_x:.5f}f, {step_y:.5f}f, {vw / vh:.5f}f}}")
        return rows

    out = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// GENERATED by scripts/unit_art/build.py (make unit-art). Do not edit.",
        "// clang-format off",
        "",
        '#include "ams_unit_shell.h"',
        "",
        "#include <cstring>",
        "",
        "namespace helix::ui::tray {",
        "",
        "namespace {",
    ]
    rows = []
    for unit in units:
        t = unit.tray
        if t is None:
            continue
        kit.check_cheek(t.cheek)
        dome_points = kit.fit_cap_points(t.dome)
        parts = t.front if t.front is not None else kit.classic_front(t)
        out.append(f"constexpr FrontPart k_{unit.id}_front[] = {{{fronts(parts)}}};")
        out.append(f"constexpr ProfilePoint k_{unit.id}_cheek[] = {{{points(t.cheek)}}};")
        if dome_points:
            out.append(f"constexpr ProfilePoint k_{unit.id}_dome[] = {{{points(dome_points)}}};")
        dome = f"k_{unit.id}_dome" if dome_points else "nullptr"
        lid = "true" if t.lid_spans_row else "false"
        variants = sum(1 << lanes for lanes in unit.lane_variants)
        track = ""
        if unit.carriage_step is not None:
            carriage = carriage_rows(unit)
            out.append(f"constexpr CarriageTrack k_{unit.id}_carriage[] = {{{', '.join(carriage)}}};")
            track = f", k_{unit.id}_carriage, {len(carriage)}"
            if unit.rail_face is not None:
                colors = ", ".join(f"0x{c:06x}" for c in unit.rail_face)
                out.append(f"constexpr RailFace k_{unit.id}_face = {{{colors}}};")
                track += f", &k_{unit.id}_face"
        rows.append(
            f'    {{"{unit.id}", k_{unit.id}_cheek, {len(t.cheek)}, {dome}, {len(dome_points)}, {unit.lanes}, '
            f"{t.back_extra_px:.2f}f, {t.front_wall_px:.1f}f, {t.cap_clearance_px:.1f}f, "
            f"{lid}, LaneJoin::{LANE_JOINS[t.lane_join]}, 0x{t.body:06x}, 0x{t.corner:06x}, k_{unit.id}_front, {len(parts)}, {'true' if unit.stub_art else 'false'}, 0x{variants:x}{track}}},"
        )
    out += ["", "const UnitShell kShells[] = {"] + rows + ["};", "} // namespace", ""]
    out += [
        "const UnitShell* unit_shell_by_id(const char* id) {",
        "    for (const UnitShell& shell : kShells) {",
        "        if (std::strcmp(shell.id, id) == 0)",
        "            return &shell;",
        "    }",
        "    return nullptr;",
        "}",
        "",
        "} // namespace helix::ui::tray",
        "",
    ]
    return "\n".join(out)


# World height, at size 1, of a head drawn at the app's nominal height.
LANE_JOINS = {"hub": "Hub", "junction": "Junction", "inside": "Inside", "block": "FixedBlock"}
HEAD_REFERENCE_HEIGHT = 1.18


def _head_frame(head):
    scene = kit.Scene(kit.ART_CAMERA)
    head.draw_art(scene, "full")
    return head_kit.head_frame(scene, head.tip, head.entry)


def bake_head(head, out_dir, render=rsvg_render):
    """One head at each tier: its body, and the nozzle's tint and shade masks."""
    target = Path(out_dir) / head.id
    target.mkdir(parents=True, exist_ok=True)
    frame = _head_frame(head)
    manifest = {"id": head.id, "aspect": round(frame.aspect, 4), "tiers": {}}
    for tier, height in HEAD_TIER_HEIGHT.items():
        scene = kit.Scene(kit.ART_CAMERA)
        head.draw_art(scene, tier)
        width = round(height * manifest["aspect"])
        names = {"back": f"{tier}.png", "tint": f"{tier}_tint.png", "shade": f"{tier}_shade.png"}
        render(scene.layer_svg("back", width), width, target / names["back"])
        render(scene.lane_color_svg(0, width), width, target / names["tint"])
        render(scene.lane_shade_svg(0, width), width, target / names["shade"])
        manifest["tiers"][tier] = {"width": width, "height": height, **names}
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def generate_head_table(heads):
    """The C++ table of every baked head's frame."""
    out = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// GENERATED by scripts/unit_art/build.py (make unit-art). Do not edit.",
        "// clang-format off",
        "",
        '#include "toolhead_art.h"',
        "",
        "namespace helix::ui {",
        "",
        "const ToolheadArt kToolheadArt[] = {",
    ]
    for head in heads:
        frame = _head_frame(head)
        height = frame.world_height * head.size / HEAD_REFERENCE_HEIGHT
        out.append(
            f'    {{ToolheadStyle::{head.style}, "{head.id}", {frame.aspect:.4f}f, {frame.tip_x:.4f}f, {frame.tip_y:.4f}f, '
            f"{height:.4f}f, {frame.entry_x:.4f}f, {frame.entry_y:.4f}f}},"
        )
    out += ["};", f"const int kToolheadArtCount = {len(heads)};", "", "} // namespace helix::ui", ""]
    return "\n".join(out)


def _png_uri(path):
    return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()


def contact_sheet(out_dir, sheet_path):
    """Every unit composed from its real layers, on dark and light cards."""
    # An SVG mask recolours each lane the way the runtime will.
    # A row clears the 320 px box a tall unit's art fills.
    cells, x_step, row_h, y = [], 360, 340, 0
    roots = [root for unit in UNITS for root, _ in art_dirs(unit, out_dir) + stub_dirs(unit, out_dir)]
    for bg in ("#1a1b1e", "#e8e8ea"):
        x = 0
        cells.append(f'<rect x="0" y="{y}" width="{x_step * max(1, len(roots))}" height="{row_h}" fill="{bg}"/>')
        for root in roots:
            m = json.loads((root / "manifest.json").read_text())
            for tier, ox in (("full", 10), ("thumb", 335 - 40)):
                t = m["tiers"][tier]
                w = t["width"]
                h = w
                cells.append(f'<image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / t["back"])}"/>')
                for lane, names in enumerate(t["lanes"]):
                    mask_id = f"m{bg[1:]}{m['id']}{m['lanes']}{tier}{lane}"
                    cells.append(
                        f'<mask id="{mask_id}"><image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / names["color"])}"/></mask>'
                        f'<rect x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" fill="{SAMPLE[lane % len(SAMPLE)]}" mask="url(#{mask_id})"/>'
                        f'<image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / names["shade"])}"/>'
                    )
                cells.append(f'<image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / t["front"])}"/>')
                if "carriage" in t:
                    cells.append(f'<image x="{x + ox}" y="{y + 10}" width="{w}" height="{h}" preserveAspectRatio="xMidYMin" href="{_png_uri(root / t["carriage"])}"/>')
            x += x_step
        y += row_h
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{x_step * max(1, len(roots))}" height="{y}">{"".join(cells)}</svg>'
    sheet_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = sheet_path.with_suffix(".svg")
    tmp.write_text(svg)
    subprocess.run(["rsvg-convert", "-o", str(sheet_path), str(tmp)], check=True)


def main():
    out = REPO / "assets/images/units"
    for unit in UNITS:
        bake_unit(unit, out)
    (REPO / "src/generated/ams_unit_shells_gen.cpp").write_text(generate_shell_table(UNITS))
    heads_out = REPO / "assets/images/toolheads"
    for head in HEADS:
        bake_head(head, heads_out)
    (REPO / "src/rendering/toolhead_art_gen.cpp").write_text(generate_head_table(HEADS))
    contact_sheet(out, REPO / "build/unit-art/contact_sheet.png")


if __name__ == "__main__":
    main()
