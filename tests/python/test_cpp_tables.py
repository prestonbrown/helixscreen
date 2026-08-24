#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Tests for the static-C++-table extractor (scripts/translations/cpp_tables.py).

These tables reach lv_tr() through a variable -- `lv_tr(def.display_name)` --
so the call-site patterns in extractor.py never saw the strings they hold. The
whole home-screen widget catalog and the AFC/Happy Hare/ACE device-settings
surface were untranslated in all nine languages as a result.

Each false-positive case below is one the parsers actually produced while being
written; they are pinned so a later loosening cannot bring them back.
"""

import sys
from pathlib import Path
from textwrap import dedent

scripts_dir = Path(__file__).parent.parent.parent / "scripts"
sys.path.insert(0, str(scripts_dir))

from translations.cpp_tables import extract_table_strings  # noqa: E402


# =============================================================================
# PanelWidgetDef
# =============================================================================


WIDGET_TABLE = dedent("""\
    static std::vector<PanelWidgetDef> s_widget_defs = {
        {"printer_image", "Printer Image", "rotate_3d", "3D printer visualization",
         nullptr, nullptr, true, 2, 2, 1, 1, 4, 3},
        {"power_device", "Power", "power_cycle", "Toggle Moonraker power devices",
         "power_device_count", "Requires Moonraker power device",
         false, 1, 1, 1, 1, 1, 1, true},
    };
""")


def test_widget_rows_yield_name_description_and_gate_hint():
    found = extract_table_strings(WIDGET_TABLE)
    assert "Printer Image" in found
    assert "3D printer visualization" in found
    assert "Toggle Moonraker power devices" in found
    assert "Requires Moonraker power device" in found


def test_widget_rows_do_not_yield_ids_icons_or_gate_subjects():
    """Only the three display fields are user-facing."""
    found = extract_table_strings(WIDGET_TABLE)
    for internal in ("printer_image", "rotate_3d", "power_cycle", "power_device_count"):
        assert internal not in found


def test_a_nullptr_gate_hint_is_not_a_string():
    found = extract_table_strings(WIDGET_TABLE)
    assert "nullptr" not in found
    assert "" not in found


# =============================================================================
# Toolchange phase templates
# =============================================================================


PHASE_TABLE = dedent("""\
    AmsBackendAfc::toolchange_phase_template(StepOperationType op) const {
        switch (op) {
        case StepOperationType::LOAD_SWAP:
            return {
                {"heat", "Heat nozzle", false},    {"cut", "Cut tip", true},
                {"kick", "Kick away", true},       {"load", "Load complete", false},
            };
        case StepOperationType::UNLOAD:
            return {
                {"unload", "Retract filament", false},
            };
        }
        return {};
    }
""")


def test_phase_rows_yield_the_label_not_the_token():
    found = extract_table_strings(PHASE_TABLE)
    assert {"Heat nozzle", "Cut tip", "Kick away", "Load complete", "Retract filament"} <= found
    for token in ("heat", "cut", "kick", "load", "unload"):
        assert token not in found


def test_phase_rows_survive_the_switch_nesting():
    """
    Regression: the parser used to count brace levels down to the rows. The
    `switch` around the `return {...}` arms added one, so a whole row was read
    as a single field and its two literals were joined -- "cut" + "Cut tip"
    extracted as the string `cutCut tip`.
    """
    found = extract_table_strings(PHASE_TABLE)
    assert not [s for s in found if s.startswith(("cut", "heat", "kick", "load"))]


# =============================================================================
# DeviceSection / DeviceAction
# =============================================================================


def test_device_section_rows_yield_label_and_description():
    found = extract_table_strings(dedent("""\
        std::vector<DeviceSection> afc_default_sections() {
            return {
                {"hub", "Hub & Cutter", 4, "Blade change and parking"},
            };
        }
    """))
    assert "Hub & Cutter" in found
    assert "Blade change and parking" in found
    assert "hub" not in found


def test_designated_device_action_yields_label_and_description():
    found = extract_table_strings(dedent("""\
        actions.push_back({
            .id = "hub_cut_enabled",
            .label = "Cutter Enabled",
            .icon = "content-cut",
            .section = "hub",
            .description = "Enable or disable the hub cutter",
            .disable_reason = "Cutter not installed",
        });
    """))
    assert "Cutter Enabled" in found
    assert "Enable or disable the hub cutter" in found
    # Only ever logged, never rendered.
    assert "Cutter not installed" not in found
    assert "content-cut" not in found


def test_positional_device_action_yields_label_and_description():
    found = extract_table_strings(dedent("""\
        DA{"ace_manual_retract",
           "Manual Retract",
           "",
           "filament_control",
           "Retract filament to current slot",
           AT::BUTTON,
           {}, {}, 0, 100, "", -1, true, ""},
    """))
    assert "Manual Retract" in found
    assert "Retract filament to current slot" in found
    assert "filament_control" not in found


# =============================================================================
# Builder-lambda helpers
# =============================================================================


def test_builder_lambda_calls_yield_the_label_argument():
    """hh_defaults.cpp builds its rows through local lambdas, not initializers."""
    found = extract_table_strings(dedent("""\
        std::vector<DeviceAction> hh_default_actions() {
            std::vector<DeviceAction> actions;
            auto add_button = [&](std::string id, std::string label, std::string section) {
                DeviceAction a;
                a.id = std::move(id);
                a.label = std::move(label);
                actions.push_back(std::move(a));
            };
            add_button("calibrate_bowden", "Calibrate Bowden", "setup");
            return actions;
        }
    """))
    assert "Calibrate Bowden" in found
    assert "calibrate_bowden" not in found
    assert "setup" not in found


def test_a_validation_helper_named_label_is_not_a_row_builder():
    """
    Regression: matching on the parameter name alone swept up validation
    helpers, whose `label` names the FIELD that failed. That produced keys like
    "sync state", "motor state" and "distance" -- none of them ever displayed.
    A row builder is distinguished by actually constructing a row.
    """
    found = extract_table_strings(dedent("""\
        auto require_bool = [&](const char* label) -> std::pair<bool, AmsError> {
            if (!value.has_value()) {
                return {false, AmsError(AmsResult::WRONG_STATE,
                                        fmt::format("{} value required", label),
                                        "Missing value", fmt::format("Provide {}", label))};
            }
            return {std::any_cast<bool>(value), AmsError()};
        };
        auto [enable, err] = require_bool("sync state");
        auto [motor, err2] = require_bool("motor state");
    """))
    assert found == set()


# =============================================================================
# Files with none of these shapes
# =============================================================================


def test_an_unrelated_file_yields_nothing():
    found = extract_table_strings(dedent("""\
        void PrinterState::set_temp(double t) {
            spdlog::info("[PrinterState] temp {}", t);
            lv_subject_set_int(&temp_, static_cast<int>(t));
        }
    """))
    assert found == set()


def test_a_brace_inside_a_literal_does_not_unbalance_the_walk():
    found = extract_table_strings(dedent("""\
        std::vector<DeviceSection> sections() {
            return {
                {"fmt", "Braces { and }", 0, "A description with } in it"},
            };
        }
    """))
    assert "Braces { and }" in found
    assert "A description with } in it" in found
