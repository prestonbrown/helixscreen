# SPDX-License-Identifier: GPL-3.0-or-later

"""The bed mesh intro's material chips ("Heat the bed first") show their whole
labels: Cold, PLA, PETG and ABS / ASA each sit in a chip wide enough for the
label and its temperature, without dots, on every landscape size."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

_CHIPS = ("mesh_heat_cold", "mesh_heat_pla", "mesh_heat_petg", "mesh_heat_abs")


@pytest.mark.parametrize("size", ["480x320", "480x400", "800x480", "1024x600"])
def test_material_chip_labels_fit(size, tmp_path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built")
    before = os.environ.get("HELIX_SCREEN_SIZE")
    os.environ["HELIX_SCREEN_SIZE"] = size
    try:
        with HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                      log_path=tmp_path / "app.log") as app:
            app.wait_for("nav_buttons_enabled", 1, timeout=30)
            app.navigate("controls")
            app.wait_idle()
            app.click("btn_bed_mesh")
            app.wait_idle()
            for chip in _CHIPS:
                path = app.geom(chip)["resolved_path"]
                # A label too narrow for its text ends in dots in place.
                shown = app.ctl("text", f"{path}/chip_label")["text"]
                assert "..." not in shown, (
                    f"{size}: {chip} label draws as {shown!r}, so the chip is too narrow")
    finally:
        if before is None:
            os.environ.pop("HELIX_SCREEN_SIZE", None)
        else:
            os.environ["HELIX_SCREEN_SIZE"] = before
