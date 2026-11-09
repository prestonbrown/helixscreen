#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portrait splash canvases, as the packager sees them.

scripts/platform_manifest.py decides which splash classes a package carries.
A portrait panel takes the largest portrait canvas that fits; every embedded
panel is landscape once rotated, so no embedded package may gain a portrait
canvas. tests/unit/test_prerender_size_class.cpp holds the compiled selector to
the same manifest rule.
"""

import re
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import platform_manifest as pm  # noqa: E402

MANIFEST = pm.load()


@pytest.mark.parametrize(
    "width,height,expected",
    [
        (480, 800, "medium-portrait"),
        (600, 1024, "large-portrait"),
        (720, 1280, "xlarge-portrait"),
        (1080, 1920, "xlarge-portrait"),
        (600, 800, "medium-portrait"),
        (720, 1024, "large-portrait"),
    ],
)
def test_portrait_panel_takes_the_largest_canvas_that_fits(width, height, expected):
    assert pm.splash_portrait_class(MANIFEST, width, height) == expected
    assert pm.splash_3d_class(MANIFEST, width, height) == expected


@pytest.mark.parametrize(
    "width,height",
    [(800, 480), (1280, 720), (720, 720), (479, 800), (320, 480), (272, 480)],
)
def test_no_portrait_canvas_keeps_the_tier(width, height):
    assert pm.splash_portrait_class(MANIFEST, width, height) is None
    assert pm.splash_3d_class(MANIFEST, width, height) == pm.splash_class(MANIFEST, width, height)


def _fixed_panel_ids():
    return [pid for pid, entry in MANIFEST["platforms"].items() if pm.has_fixed_panel(entry)]


@pytest.mark.parametrize("platform_id", sorted(MANIFEST["platforms"]))
def test_no_restricted_package_carries_a_portrait_canvas(platform_id):
    classes = pm.splash_3d_classes_for(MANIFEST, platform_id)
    assert not [c for c in classes if c in MANIFEST["size_classes"]["splash_portrait"]]


def test_the_embedded_panels_are_all_fixed():
    # The test above proves nothing about a platform that restricts nothing.
    assert {"k1", "k2", "ad5m", "ad5x", "cc1", "snapmaker-u1"} <= set(_fixed_panel_ids())


def test_a_portrait_fixed_panel_ships_its_portrait_canvas(tmp_path):
    manifest = pm.load()
    manifest["platforms"]["k2"]["panel"]["rotate"] = 0  # 480x800 as the app sees it
    assert pm.splash_3d_classes_for(manifest, "k2") == ["medium-portrait"]


def test_every_portrait_class_has_a_canvas_size_and_a_generator_entry():
    classes = MANIFEST["size_classes"]
    src = (REPO_ROOT / "scripts" / "gen_splash_3d.py").read_text(encoding="utf-8")
    block = re.search(r"SCREEN_SIZES = \[(.*?)\]", src, re.S).group(1)
    gen = {m[0]: (int(m[1]), int(m[2]))
           for m in re.findall(r'\("([a-z_-]+)",\s*(\d+),\s*(\d+),', block)}
    for name in classes["splash_portrait"]:
        w = classes["splash_composite_width"][name]
        h = classes["splash_composite_height"][name]
        assert h > w, name
        assert gen.get(name) == (w, h), name


def _splash_generator():
    import gen_splash_3d
    return gen_splash_3d


def _content_width_fraction(gen, canvas):
    left, right = gen.find_content_columns(canvas, gen.sample_edge_color(canvas))
    return (right - left) / canvas.size[0]


@pytest.mark.parametrize("size", [(480, 800), (600, 1024), (720, 1280)])
def test_portrait_composition_leaves_side_margins(size):
    gen = _splash_generator()
    from PIL import Image
    img = Image.open(REPO_ROOT / gen.SOURCE_IMAGES["dark"]).convert("RGBA")
    canvas = gen.composite_canvas(img, *size)
    assert canvas.size == size
    assert _content_width_fraction(gen, canvas) == pytest.approx(gen.PORTRAIT_CONTENT_WIDTH, abs=0.01)
    # Centred: the margins either side differ by no more than a couple of pixels.
    left, right = gen.find_content_columns(canvas, gen.sample_edge_color(canvas))
    assert abs(left - (size[0] - right)) <= 3


def test_landscape_composition_ignores_the_portrait_width(monkeypatch):
    gen = _splash_generator()
    from PIL import Image
    img = Image.open(REPO_ROOT / gen.SOURCE_IMAGES["dark"]).convert("RGBA")
    before = gen.composite_canvas(img, 800, 480).tobytes()
    monkeypatch.setattr(gen, "PORTRAIT_CONTENT_WIDTH", 0.3)
    assert gen.composite_canvas(img, 800, 480).tobytes() == before
    assert gen.composite_canvas(img, 480, 800).size == (480, 800)
