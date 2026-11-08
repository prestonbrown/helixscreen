# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/check_required_names.py: extends= expansion."""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from check_required_names import XmlTree  # noqa: E402


def test_view_extends_picks_up_the_wrapper_names(tmp_path):
    ui = tmp_path / "ui_xml"
    ui.mkdir()
    (ui / "wrapper.xml").write_text(
        '<component><view name="wrapper"><lv_obj name="overlay_header"/></view></component>'
    )
    (ui / "page.xml").write_text(
        '<component><view name="page" extends="wrapper"><lv_obj name="body"/></view></component>'
    )
    names = XmlTree(tmp_path, {}).names("page", [])
    assert {"body", "overlay_header"} <= names
