# SPDX-License-Identifier: GPL-3.0-or-later
"""_HELIX_MATERIAL_PA, rendered the way Klipper renders a gcode_macro.

The C++ side (include/material_pa.h, tests/unit/test_material_pa.cpp) writes
helix_pa_<key>_* into [save_variables]; this renders the shipped macro against
those names with Klipper's template delimiters and checks the command it sends
at print start: the material's entry, else the extruder's config values, every
parameter named, nothing at all while the switch is off.
"""

import textwrap
from pathlib import Path

import jinja2
import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
MACROS = REPO_ROOT / "assets" / "config" / "helix_macros.cfg"


def macro_body(name):
    """The gcode template of [gcode_macro <name>], as Klipper's reader keeps it."""
    lines = MACROS.read_text().splitlines()
    start = lines.index(f"[gcode_macro {name}]")
    body = []
    in_gcode = False
    for line in lines[start + 1:]:
        if line.startswith("["):
            break
        if line.startswith("gcode:"):
            in_gcode = True
            continue
        if in_gcode:
            if line and not line[0].isspace():
                break
            body.append(line)
    return textwrap.dedent("\n".join(body))


def render(name, printer, params):
    # Klipper's gcode_macro environment: '{' '}' for expressions.
    env = jinja2.Environment("{%", "%}", "{", "}", extensions=["jinja2.ext.do"])
    out = env.from_string(macro_body(name)).render(printer=printer, params=params)
    return [line.strip() for line in out.splitlines() if line.strip()]


KALICO_TANH_CFG = {
    "pressure_advance_model": "tanh",
    "linear_advance": 0.02,
    "nonlinear_offset": 0.1,
    "linearization_velocity": 1.0,
    "pressure_advance_time_offset": 0.001,
    "pressure_advance_smooth_time": 0.02,
}
KALICO_TANH_LIVE = {
    "pressure_advance_model": "tanh",
    "linear_advance": 0.05,  # a slicer's ADVANCE= from the last print
    "nonlinear_offset": 0.1,
    "linearization_velocity": 1.0,
}
KLIPPER_CFG = {"pressure_advance": 0.04, "pressure_advance_smooth_time": 0.04}
KLIPPER_LIVE = {"pressure_advance": 0.07, "smooth_time": 0.04}

PLA_TANH = {
    "helix_pa_pla_model": "tanh",
    "helix_pa_pla_advance": 0.012,
    "helix_pa_pla_offset": 0.16,
    "helix_pa_pla_velocity": 1.0,
    "helix_pa_pla_time_offset": 0.0028,
    "helix_pa_pla_smooth_time": 0.02,
}


def printer(variables, cfg, live):
    return {
        "save_variables": {"variables": variables},
        "toolhead": {"extruder": "extruder"},
        "configfile": {"settings": {"extruder": cfg}},
        "extruder": live,
    }


def pa_lines(lines):
    return [line for line in lines if line.startswith("SET_PRESSURE_ADVANCE")]


def test_off_by_default_sends_nothing():
    lines = render("_HELIX_MATERIAL_PA",
                   printer(PLA_TANH, KALICO_TANH_CFG, KALICO_TANH_LIVE), {"KEY": "pla"})
    assert lines == []


def test_a_nonlinear_entry_sets_the_model_and_every_parameter():
    vars_ = dict(PLA_TANH, helix_material_pa_enabled=1)
    lines = render("_HELIX_MATERIAL_PA",
                   printer(vars_, KALICO_TANH_CFG, KALICO_TANH_LIVE), {"KEY": "pla"})
    assert pa_lines(lines) == [
        "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=tanh ADVANCE=0.012 OFFSET=0.16 "
        "VELOCITY=1.0 TIME_OFFSET=0.0028 SMOOTH_TIME=0.02"
    ]
    assert "SET_GCODE_VARIABLE MACRO=_HELIX_MATERIAL_PA VARIABLE=applied VALUE='\"PLA\"'" in lines


def test_a_material_with_no_entry_falls_back_to_the_config():
    vars_ = dict(PLA_TANH, helix_material_pa_enabled=1)
    lines = render("_HELIX_MATERIAL_PA",
                   printer(vars_, KALICO_TANH_CFG, KALICO_TANH_LIVE), {"KEY": "petg"})
    # The config's values, not the live ones a slicer left behind.
    assert pa_lines(lines) == [
        "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=tanh ADVANCE=0.02 OFFSET=0.1 "
        "VELOCITY=1.0 TIME_OFFSET=0.001 SMOOTH_TIME=0.02"
    ]
    assert "SET_GCODE_VARIABLE MACRO=_HELIX_MATERIAL_PA VARIABLE=applied VALUE='\"CONFIG\"'" in lines


def test_no_material_falls_back_to_the_config():
    vars_ = dict(PLA_TANH, helix_material_pa_enabled=1)
    lines = render("_HELIX_MATERIAL_PA",
                   printer(vars_, KLIPPER_CFG, KLIPPER_LIVE), {"KEY": ""})
    assert pa_lines(lines) == ["SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.04 SMOOTH_TIME=0.04"]


def test_a_linear_entry_under_a_model_firmware_takes_the_rest_from_the_config():
    # Leftovers of an earlier nonlinear entry for the same material never apply.
    vars_ = dict(PLA_TANH, helix_material_pa_enabled=1,
                 helix_pa_pla_model="linear", helix_pa_pla_advance=0.035)
    lines = render("_HELIX_MATERIAL_PA",
                   printer(vars_, KALICO_TANH_CFG, KALICO_TANH_LIVE), {"KEY": "pla"})
    assert pa_lines(lines) == [
        "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=linear ADVANCE=0.035 TIME_OFFSET=0.001 "
        "SMOOTH_TIME=0.02"
    ]


def test_a_linear_entry_on_classic_firmware_sends_no_model():
    vars_ = {"helix_material_pa_enabled": 1,
             "helix_pa_pla_model": "linear", "helix_pa_pla_advance": 0.035}
    lines = render("_HELIX_MATERIAL_PA",
                   printer(vars_, KLIPPER_CFG, KLIPPER_LIVE), {"KEY": "pla"})
    assert pa_lines(lines) == ["SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.035 SMOOTH_TIME=0.04"]


def test_a_nonlinear_entry_on_classic_firmware_uses_the_config():
    # Sending nothing would leave the previous print's material value live.
    vars_ = dict(PLA_TANH, helix_material_pa_enabled=1)
    lines = render("_HELIX_MATERIAL_PA",
                   printer(vars_, KLIPPER_CFG, KLIPPER_LIVE), {"KEY": "pla"})
    assert pa_lines(lines) == ["SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.04 SMOOTH_TIME=0.04"]
    assert any("not used" in line for line in lines)


@pytest.mark.parametrize("model", ["", "bogus"])
def test_a_half_written_or_unknown_entry_uses_the_config(model):
    vars_ = dict(PLA_TANH, helix_material_pa_enabled=1, helix_pa_pla_model=model)
    lines = render("_HELIX_MATERIAL_PA",
                   printer(vars_, KALICO_TANH_CFG, KALICO_TANH_LIVE), {"KEY": "pla"})
    assert pa_lines(lines) == [
        "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=tanh ADVANCE=0.02 OFFSET=0.1 "
        "VELOCITY=1.0 TIME_OFFSET=0.001 SMOOTH_TIME=0.02"
    ]


def test_several_extruders_get_nothing():
    vars_ = dict(PLA_TANH, helix_material_pa_enabled=1)
    p = printer(vars_, KALICO_TANH_CFG, KALICO_TANH_LIVE)
    p["configfile"]["settings"]["extruder1"] = KALICO_TANH_CFG
    lines = render("_HELIX_MATERIAL_PA", p, {"KEY": "pla"})
    assert pa_lines(lines) == []


def offset_macro_lines(material, variables):
    p = printer(variables, KLIPPER_CFG, KLIPPER_LIVE)
    p["gcode_macro _HELIX_MATERIAL"] = {"next": "", "next_file": "", "applied": "",
                                        "apply_count": 0}
    p["print_stats"] = {"filename": "x.gcode"}
    return render("HELIX_MATERIAL_OFFSET", p, {"MATERIAL": material})


@pytest.mark.parametrize("material,key", [("PLA", "pla"), ("PETG-CF", "petg_cf"),
                                          ("PLA Silk", "pla_silk"), ("", ""),
                                          # Klipper's parser ends the arguments at '*' and
                                          # splits a parameter at a tab.
                                          ("PA*CF", "pacf"), ("PLA\tCF", "pla_cf")])
def test_the_offset_macro_hands_over_its_key(material, key):
    # Called whether or not Z offset by material is on.
    lines = offset_macro_lines(material, {"helix_material_pa_enabled": 1})
    assert f'_HELIX_MATERIAL_PA KEY="{key}"' in lines


def test_the_offset_macro_leaves_pa_alone_while_it_is_off():
    lines = offset_macro_lines("PLA", {})
    assert not any(line.startswith("_HELIX_MATERIAL_PA") for line in lines)
