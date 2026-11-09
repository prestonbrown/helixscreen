# SPDX-License-Identifier: GPL-3.0-or-later
"""Baked toolhead art: every head drawn, framed on its nozzle, and tied to the C++ table."""
import json
import re
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from unit_art import build, head, kit  # noqa: E402

HEADS = build.HEADS
EXPECTED = {
    "stealthburner", "mini_stealthburner", "dragonburner", "a4t", "anthead", "jabberwocky",
    "bambu", "creality_k1", "creality_k2", "ender3_bowden", "ender3_integrated",
}


def _scene(h, tier="full"):
    scene = kit.Scene(kit.ART_CAMERA)
    h.draw_art(scene, tier)
    return scene


def test_all_eleven_heads_are_baked():
    assert {h.id for h in HEADS} == EXPECTED


@pytest.mark.parametrize("h", HEADS, ids=lambda h: h.id)
def test_style_is_an_enum_value(h):
    assert re.search(rf"\b{h.style}\b", (REPO / "include/toolhead_style.h").read_text())


@pytest.mark.parametrize("h", HEADS, ids=lambda h: h.id)
def test_drawn_with_the_shared_camera_and_one_tinted_nozzle(h):
    scene = _scene(h)
    assert scene.camera is kit.ART_CAMERA
    tinted = [i for i in scene.items if i.lane is not None]
    assert tinted and all(i.lane == 0 and i.layer == "back" for i in tinted)
    assert {i.layer for i in scene.items} == {"back"}


@pytest.mark.parametrize("h", HEADS, ids=lambda h: h.id)
def test_nozzle_tip_sits_near_the_bottom_of_the_frame(h):
    # The shell's front lip stands nearer the camera than the tip, so it can draw a little lower.
    frame = head.head_frame(_scene(h), h.tip)
    assert 0.82 < frame.tip_y <= 1.0


@pytest.mark.parametrize("h", HEADS, ids=lambda h: h.id)
def test_small_tier_drops_detail(h):
    assert len(_scene(h, "path").items) < len(_scene(h, "full").items)


def test_frame_reports_tip_column_inside_the_image():
    for h in HEADS:
        frame = head.head_frame(_scene(h), h.tip)
        assert 0.0 < frame.tip_x < 1.0
        assert 0.3 < frame.aspect < 2.2


def test_bake_writes_each_tier_at_its_height(tmp_path):
    rendered = []

    def render(svg, width, path):
        rendered.append((path.name, width))
        path.write_bytes(b"png")

    h = HEADS[0]
    build.bake_head(h, tmp_path, render=render)
    manifest = json.loads((tmp_path / h.id / "manifest.json").read_text())
    assert set(manifest["tiers"]) == set(build.HEAD_TIER_HEIGHT)
    for tier, height in build.HEAD_TIER_HEIGHT.items():
        t = manifest["tiers"][tier]
        assert t["height"] == height
        assert t["width"] == round(height * manifest["aspect"])
        for name in (t["back"], t["tint"], t["shade"]):
            assert (tmp_path / h.id / name).exists()


def test_generated_table_is_current():
    generated = build.generate_head_table(HEADS)
    assert generated == (REPO / "src/generated/toolhead_art_gen.cpp").read_text()
