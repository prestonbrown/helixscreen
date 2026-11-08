# SPDX-License-Identifier: GPL-3.0-or-later
"""The unit art kit: camera, spools tied to the app's values, baked layers."""
import json
import math
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from unit_art import build, kit, spool  # noqa: E402
from unit_art.units import ace_pro, box_turtle  # noqa: E402


def test_camera_is_the_agreed_three_quarter():
    cam = kit.ART_CAMERA
    yaw, pitch = math.radians(27), math.radians(20)
    x, y = cam.project((1.0, 0.0, 0.0))
    assert x == pytest.approx(math.cos(yaw))
    assert y == pytest.approx(math.sin(yaw) * math.sin(pitch))
    assert cam.project((0.0, 0.0, 1.0))[1] == pytest.approx(-math.cos(pitch))


def test_spool_constants_match_cpp():
    c = spool.spool_constants(REPO)
    assert c["flange_lift"] == 60
    assert c["flange_drop"] == 40
    assert c["hub_radius"] == pytest.approx(0.10)
    assert c["slot_inner"] == pytest.approx(0.165)
    assert c["slot_outer"] == pytest.approx(0.335)
    assert c["slot_half_width"] == pytest.approx(0.05)
    assert c["dot_pitch"] == pytest.approx(0.03)
    assert c["flange_radius"] == pytest.approx(0.42)


def test_spool_constants_fail_when_a_value_is_gone(tmp_path):
    with pytest.raises(KeyError):
        spool.spool_constants(tmp_path)


def _scene(tier, colors=None):
    scene = kit.Scene(kit.ART_CAMERA)
    ace_pro.UNIT.draw_art(scene, colors or ["#ff0000", "#00ff00", "#0000ff", "#ffffff"], tier)
    return scene


def test_thumb_has_no_air_holes():
    full = sum(1 for i in _scene("full").items if i.tag == "air_hole")
    thumb = sum(1 for i in _scene("thumb").items if i.tag == "air_hole")
    assert full > 100
    assert thumb == 0


def test_svg_has_no_text_elements():
    assert "<text" not in _scene("full").svg(100)


def test_lane_items_are_kept_apart_from_the_shell():
    scene = _scene("full")
    assert {i.lane for i in scene.items if i.lane is not None} == {0, 1, 2, 3}
    assert not [i for i in scene.items if i.lane is not None and i.layer == "front"]
    assert any(i.layer == "front" for i in scene.items)


def test_an_empty_lane_draws_no_filament():
    scene = _scene("full", ["#ff0000", "#00ff00", "#0000ff", None])
    assert not [i for i in scene.items if i.lane == 3]


@pytest.mark.parametrize("unit", build.UNITS, ids=lambda u: u.id)
def test_every_unit_keeps_its_filament_tintable(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.draw_art(scene, ["#ffffff"] * unit.lanes, "full")
    assert {i.lane for i in scene.items if i.lane is not None} == set(range(unit.lanes))
    assert "<text" not in scene.svg(100)


def test_every_unit_is_baked_through_the_one_camera(tmp_path):
    seen = []
    real = build.kit.Scene

    def spy(camera):
        seen.append(camera)
        return real(camera)

    build.kit.Scene = spy
    try:
        for unit in build.UNITS:
            build.bake_unit(unit, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    finally:
        build.kit.Scene = real
    assert seen and all(c is kit.ART_CAMERA for c in seen)


def test_the_open_box_turtle_has_no_lid_and_the_enclosed_one_does():
    assert box_turtle.UNIT.tray.dome == []
    assert box_turtle.ENCLOSED.tray.dome


def test_generated_shell_table_is_current():
    generated = REPO / "src/generated/ams_unit_shells_gen.cpp"
    assert generated.read_text() == build.generate_shell_table(build.UNITS)


def test_manifest_lists_every_lane_mask(tmp_path):
    def fake_render(svg_text, width, png_path):
        assert "<svg" in svg_text
        png_path.write_bytes(b"png")

    build.bake_unit(ace_pro.UNIT, tmp_path, render=fake_render)
    manifest = json.loads((tmp_path / ace_pro.UNIT.id / "manifest.json").read_text())
    assert manifest["lanes"] == 4
    for tier in ("full", "thumb"):
        t = manifest["tiers"][tier]
        assert len(t["lanes"]) == 4
        for name in [t["back"], t["front"]] + [p for lane in t["lanes"] for p in lane.values()]:
            assert (tmp_path / ace_pro.UNIT.id / name).is_file()
