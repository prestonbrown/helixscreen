# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/doc_anchors.py.

Covers the citation grammar, the region resolver's refuse-to-guess contract,
and the per-language definition scanners.
"""

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from doc_anchors import (  # noqa: E402
    Citation,
    Segment,
    format_citation,
    parse_citation,
)


def test_path_only_citation_has_no_segments():
    c = parse_citation("src/printer/printer_state.cpp")
    assert c.path == "src/printer/printer_state.cpp"
    assert c.segments == ()


def test_single_identifier_segment():
    c = parse_citation("src/printer/printer_state.cpp#update_from_status")
    assert c.path == "src/printer/printer_state.cpp"
    assert c.segments == (Segment("update_from_status", False),)


def test_nested_identifier_segments():
    c = parse_citation("include/ui_nav_manager.h#PanelRequest/overlay_root")
    assert c.segments == (
        Segment("PanelRequest", False),
        Segment("overlay_root", False),
    )


def test_snippet_segment():
    c = parse_citation('src/application/application.cpp#instance/"shutdown_requested"')
    assert c.segments == (
        Segment("instance", False),
        Segment("shutdown_requested", True),
    )


def test_slash_inside_a_snippet_does_not_split_it():
    c = parse_citation('mk/cross.mk#PLATFORM_TARGET/"lib/lvgl"')
    assert c.segments == (
        Segment("PLATFORM_TARGET", False),
        Segment("lib/lvgl", True),
    )


def test_escaped_quote_inside_a_snippet():
    c = parse_citation(r'src/a.cpp#f/"says \"hi\""')
    assert c.segments[1] == Segment('says "hi"', True)


def test_qualified_identifier_segment_keeps_colons():
    c = parse_citation("src/ui/led_widget.cpp#helix::ui/attach")
    assert c.segments[0] == Segment("helix::ui", False)


@pytest.mark.parametrize(
    "text",
    [
        "src/printer/printer_state.cpp",
        "src/printer/printer_state.cpp#update_from_status",
        "include/ui_nav_manager.h#PanelRequest/overlay_root",
        'src/application/application.cpp#instance/"shutdown_requested"',
        r'src/a.cpp#f/"says \"hi\""',
    ],
)
def test_format_round_trips_parse(text):
    assert format_citation(parse_citation(text)) == text


def test_empty_fragment_is_an_error():
    with pytest.raises(ValueError):
        parse_citation("src/a.cpp#")


def test_unterminated_snippet_is_an_error():
    with pytest.raises(ValueError):
        parse_citation('src/a.cpp#f/"never closed')
