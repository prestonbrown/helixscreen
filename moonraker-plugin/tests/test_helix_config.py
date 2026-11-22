# SPDX-License-Identifier: GPL-3.0-or-later
"""
Klipper config read and edit (helix_config.py): include resolution and source
attribution against the fixture tree the app's C++ suite also reads, checked
against Klipper's own parsing; edits, their refusals, backups and the atomic
write; the endpoints and the config_edit feature.

Run with: pytest tests/test_helix_config.py -v
"""

import configparser
import glob
import io
import json
import os
import shutil
from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_config
from helix_config import (
    ConfigEditRefused,
    atomic_write,
    plan_edits,
    prune_backups,
    read_config,
    write_plan,
)
from helix_print import HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockFileManager,
    MockWebRequest,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)

# Shared with tests/unit/test_klipper_config_editor.cpp
TREE = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "klipper_config_tree"
EXPECTED = json.loads((TREE / "expected.json").read_text())


@pytest.fixture
def tree_copy(tmp_path):
    """A writable copy of the fixture tree."""
    root = tmp_path / "config"
    shutil.copytree(str(TREE), str(root))
    (root / "expected.json").unlink()
    return root


def _set(section, option, value, expect):
    return {"section": section, "option": option, "action": "set", "value": value,
            "expect": expect}


# ============================================================================
# Klipper's own reading, as the oracle
# ============================================================================

AUTOSAVE_HEADER = "\n" + "\n".join(helix_config.AUTOSAVE_HEADER_LINES) + "\n"


def _klipper_parse(data, filename, fileconfig, visited):
    """klippy/configfile.py PrinterConfig._parse_config, minus the printer."""
    path = os.path.abspath(filename)
    assert path not in visited
    visited.add(path)
    buffer = []

    def flush():
        if buffer:
            fileconfig.read_file(io.StringIO("\n".join(buffer)), filename)
            del buffer[:]

    for line in data.split("\n"):
        pos = line.find("#")
        if pos >= 0:
            line = line[:pos]
        mo = configparser.RawConfigParser.SECTCRE.match(line)
        header = mo and mo.group("header")
        if header and header.startswith("include "):
            flush()
            include_glob = os.path.join(os.path.dirname(filename), header[8:].strip())
            for name in sorted(glob.glob(include_glob)):
                with open(name) as f:
                    _klipper_parse(f.read().replace("\r\n", "\n"), name, fileconfig, visited)
        else:
            buffer.append(line)
    flush()
    visited.remove(path)


def _klipper_config(data, filename):
    fileconfig = configparser.RawConfigParser(strict=False, inline_comment_prefixes=(";", "#"))
    _klipper_parse(data, filename, fileconfig, set())
    return fileconfig


def klipper_effective(root: Path, strip_duplicates: bool = True):
    """Every option's value as Klipper's read_main_config leaves it. Without
    `strip_duplicates`, as a build that skips PrinterConfig._strip_duplicates
    reads it (OpenCentauri's COSMOS Kalico): the SAVE_CONFIG value wins."""
    filename = str(root / "printer.cfg")
    data = Path(filename).read_text().replace("\r\n", "\n")
    regular, autosave = data, ""
    pos = data.find(AUTOSAVE_HEADER)
    if pos >= 0:
        regular = data[:pos]
        autosave = "\n" + "\n".join(
            line[4:] for line in data[pos + len(AUTOSAVE_HEADER):].strip().split("\n")
        ) + "\n"
    regular_config = _klipper_config(regular, filename)
    # PrinterConfig._strip_duplicates: the regular config wins
    lines, section = autosave.split("\n"), None
    for i, line in enumerate(lines):
        pruned = line.split("#")[0].split(";")[0].rstrip()
        if not pruned or pruned[0].isspace():
            continue
        if pruned[0] == "[":
            section = pruned[1:-1].strip()
            continue
        field = pruned.split("=")[0].split(":")[0].strip().lower()
        if strip_duplicates and regular_config.has_option(section, field):
            lines[i] = "#" + line
    config = _klipper_config(regular + "\n".join(lines), filename)
    return {s: dict(config.items(s)) for s in config.sections()}


# ============================================================================
# Reading: the shared fixture tree
# ============================================================================

class TestSharedFixture:
    def test_files_in_read_order(self):
        assert read_config(TREE).files == EXPECTED["files"]

    def test_every_option_attributed(self):
        tree = read_config(TREE).to_json()
        got = {
            name: {o: {k: v for k, v in d.items() if k != "end_line"}
                   for o, d in s["options"].items()}
            for name, s in tree["sections"].items()
        }
        assert got == EXPECTED["options"]

    def test_section_owner_is_its_last_regular_definition(self):
        tree = read_config(TREE)
        owners = {}
        for sec in tree.sections:
            if not sec.autosave:
                owners[sec.section] = sec.file
        assert owners == EXPECTED["sections"]

    def test_values_match_klipper(self):
        tree = read_config(TREE)
        got = {s: {o: d.value for o, d in opts.items()} for s, opts in tree.effective().items()}
        assert got == klipper_effective(TREE)

    def test_wildcard_with_no_match_is_not_an_error(self):
        assert read_config(TREE).errors == []

    def test_regular_definition_beats_the_autosave_block(self):
        rotation = read_config(TREE).option("extruder", "rotation_distance")
        assert (rotation.value, rotation.autosave, rotation.line) == ("22.6789511", False, 17)


class TestReading:
    def write(self, root, files):
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def test_missing_plain_include_is_reported(self, tmp_path):
        self.write(tmp_path, {"printer.cfg": "[include nope.cfg]\n[printer]\nkinematics: none\n"})
        tree = read_config(tmp_path)
        assert any("nope.cfg" in e for e in tree.errors)
        assert tree.option("printer", "kinematics").value == "none"

    def test_recursive_include_is_reported_and_stops(self, tmp_path):
        self.write(tmp_path, {
            "printer.cfg": "[include a.cfg]\n",
            "a.cfg": "[include printer.cfg]\n[a]\nk: 1\n",
        })
        tree = read_config(tmp_path)
        assert any("recursive" in e for e in tree.errors)
        assert tree.option("a", "k").value == "1"

    def test_a_section_does_not_continue_across_an_include(self, tmp_path):
        self.write(tmp_path, {
            "printer.cfg": "[printer]\nkinematics: none\n[include a.cfg]\nmax_velocity: 5\n",
            "a.cfg": "[a]\nk: 1\n",
        })
        tree = read_config(tmp_path)
        assert tree.option("printer", "max_velocity") is None
        assert tree.option("a", "max_velocity") is None
        assert any("outside any section" in e for e in tree.errors)

    def test_glob_includes_read_sorted_and_relative_to_the_includer(self, tmp_path):
        self.write(tmp_path, {
            "printer.cfg": "[include sub/main.cfg]\n",
            "sub/main.cfg": "[include parts/*.cfg]\n",
            "sub/parts/b.cfg": "[s]\nk: b\n",
            "sub/parts/a.cfg": "[s]\nk: a\n",
        })
        tree = read_config(tmp_path)
        assert tree.files == ["printer.cfg", "sub/main.cfg", "sub/parts/a.cfg", "sub/parts/b.cfg"]
        assert tree.option("s", "k").file == "sub/parts/b.cfg"

    def test_a_double_star_matches_one_directory_as_klipper_reads_it(self, tmp_path):
        """Klipper globs without recursive=True, where ** is one path segment."""
        self.write(tmp_path, {
            "printer.cfg": "[include conf.d/**/*.cfg]\n",
            "conf.d/top.cfg": "[s]\nk: top\n",
            "conf.d/a/one.cfg": "[s]\nk: one\n",
            "conf.d/a/b/two.cfg": "[s]\nk: two\n",
        })
        tree = read_config(tmp_path)
        assert tree.files == ["printer.cfg", "conf.d/a/one.cfg"]
        assert tree.option("s", "k").value == "one"
        assert klipper_effective(tmp_path)["s"]["k"] == "one"

    def test_corrupted_autosave_block_reads_as_comments(self, tmp_path):
        text = (TREE / "printer.cfg").read_text() + "stray: line\n"
        self.write(tmp_path, {"printer.cfg": text})
        tree = read_config(tmp_path)
        assert tree.option("input_shaper", "shaper_freq_x") is None
        assert not any(o.autosave for o in tree.options)

    def test_crlf_lines_are_numbered_like_lf(self, tmp_path):
        (tmp_path / "printer.cfg").write_bytes(b"[printer]\r\nkinematics: none\r\nmax_accel: 9\r\n")
        opt = read_config(tmp_path).option("printer", "max_accel")
        assert (opt.value, opt.line) == ("9", 3)

    def test_unreadable_root_is_an_error_not_an_exception(self, tmp_path):
        tree = read_config(tmp_path)
        assert tree.options == [] and tree.errors

    def test_section_filter(self):
        out = read_config(TREE).to_json("probe")
        assert list(out["sections"]) == ["probe"]
        assert read_config(TREE).to_json("no_such")["sections"] == {}


# ============================================================================
# Planning edits
# ============================================================================

class TestPlanEdits:
    def test_set_lands_in_the_defining_file_and_line(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("probe", "z_offset", "1.75", "1.5")])
        assert list(plan.changed) == ["conf.d/20-overrides.cfg"]
        assert plan.changed["conf.d/20-overrides.cfg"].split("\n")[1] == "z_offset: 1.75"

    def test_set_keeps_the_inline_comment_and_delimiter(self, tree_copy):
        plan = plan_edits(tree_copy, [
            _set("printer", "max_velocity", "350", "300"),
            _set("printer", "square_corner_velocity", "8", "5.0"),
        ])
        lines = plan.changed["printer.cfg"].split("\n")
        assert lines[5] == "max_velocity: 350  # mm/s"
        assert lines[7] == "square_corner_velocity = 8"

    def test_set_reaches_the_last_definition(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("printer", "max_accel", "6000", "5000")])
        lines = plan.changed["printer.cfg"].split("\n")
        assert (lines[6], lines[26]) == ("max_accel: 3000", "max_accel: 6000")

    def test_multi_line_value_is_replaced_whole(self, tree_copy):
        plan = plan_edits(tree_copy, [
            _set("gcode_macro START_PRINT", "gcode", "\nG28\nG1 Z5", "\nG28\n\nG1 Z10 F600"),
        ])
        lines = plan.changed["printer.cfg"].split("\n")
        assert lines[20:24] == ["gcode:", "    G28", "    G1 Z5", ""]
        after = read_config(tree_copy, lambda p: plan.changed["printer.cfg"]
                            if p.endswith("printer.cfg") else Path(p).read_text())
        assert after.option("gcode_macro START_PRINT", "gcode").value == "\nG28\nG1 Z5"

    def test_add_goes_to_the_sections_last_regular_block(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("probe", "speed", "5", None)])
        assert plan.changed["conf.d/20-overrides.cfg"].split("\n")[:3] == [
            "[probe]", "z_offset: 1.5", "speed: 5"]

    def test_add_new_section_goes_above_the_autosave_block(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("firmware_retraction", "retract_length", "0.5", None)])
        text = plan.changed["printer.cfg"]
        assert text.index("[firmware_retraction]\nretract_length: 0.5") < text.index("#*# <---")
        assert text.endswith((TREE / "printer.cfg").read_text().split("max_accel: 5000")[1])

    def test_add_to_a_section_only_the_autosave_block_has(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("input_shaper", "shaper_type_y", "ei", None)])
        text = plan.changed["printer.cfg"]
        assert text.index("[input_shaper]\nshaper_type_y: ei") < text.index("#*# <---")

    def test_remove_comments_the_definition_out(self, tree_copy):
        plan = plan_edits(tree_copy, [{"section": "gcode_macro PARK", "option": "gcode",
                                       "action": "remove", "expect": "\nG90\n\nG1 X0 Y0"}])
        assert plan.changed["conf.d/10-macros.cfg"].split("\n")[2:6] == [
            "#gcode:", "#  G90", "", "#  G1 X0 Y0"]

    def test_edits_apply_in_order(self, tree_copy):
        plan = plan_edits(tree_copy, [
            _set("probe", "z_offset", "1.6", "1.5"),
            _set("probe", "z_offset", "1.7", "1.6"),
        ])
        assert "z_offset: 1.7" in plan.changed["conf.d/20-overrides.cfg"]

    def test_diff_names_the_file_and_both_lines(self, tree_copy):
        diff = plan_edits(tree_copy, [_set("probe", "z_offset", "1.75", "1.5")]).diff()
        assert "--- a/conf.d/20-overrides.cfg" in diff
        assert "-z_offset: 1.5" in diff and "+z_offset: 1.75" in diff

    def test_crlf_file_stays_crlf(self, tmp_path):
        (tmp_path / "printer.cfg").write_bytes(b"[printer]\r\nmax_accel: 9\r\n")
        plan = plan_edits(tmp_path, [_set("printer", "max_accel", "10", "9")])
        assert plan.changed["printer.cfg"] == "[printer]\r\nmax_accel: 10\r\n"

    def test_unchanged_value_changes_no_file(self, tree_copy):
        assert plan_edits(tree_copy, [_set("probe", "z_offset", "1.5", "1.5")]).changed == {}


class TestRefusals:
    @pytest.mark.parametrize("section,option,expect", [
        ("input_shaper", "shaper_freq_x", "48.2"),
        ("extruder", "control", "pid"),
    ])
    def test_option_in_the_autosave_block(self, tree_copy, section, option, expect):
        with pytest.raises(ConfigEditRefused) as e:
            plan_edits(tree_copy, [_set(section, option, "1", expect)])
        assert e.value.code == "autosave"

    def test_option_in_both_edits_the_regular_one(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("extruder", "rotation_distance", "22.7", "22.6789511")])
        assert plan.changed["printer.cfg"].split("\n")[16] == "rotation_distance: 22.7"

    def test_target_moved_since_the_read(self, tree_copy):
        with pytest.raises(ConfigEditRefused) as e:
            plan_edits(tree_copy, [_set("probe", "z_offset", "1.75", "1.2")])
        assert e.value.code == "moved"

    def test_option_that_appeared_since_the_read(self, tree_copy):
        with pytest.raises(ConfigEditRefused) as e:
            plan_edits(tree_copy, [_set("probe", "z_offset", "1.75", None)])
        assert e.value.code == "moved"

    def test_a_section_name_with_spaces_is_fine(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("gcode_macro MY_START", "description", "x", None)])
        assert plan.changed

    def test_one_refusal_refuses_the_whole_list(self, tree_copy):
        before = (tree_copy / "conf.d/20-overrides.cfg").read_text()
        with pytest.raises(ConfigEditRefused):
            plan_edits(tree_copy, [
                _set("probe", "z_offset", "1.75", "1.5"),
                _set("input_shaper", "shaper_freq_x", "50", "48.2"),
            ])
        assert (tree_copy / "conf.d/20-overrides.cfg").read_text() == before

    @pytest.mark.parametrize("edit", [
        {"section": "probe", "option": "z_offset", "action": "set", "value": "1"},
        {"section": "probe", "option": "z_offset", "action": "set", "expect": "1.5"},
        {"section": "", "option": "z_offset", "action": "set", "value": "1", "expect": None},
        {"section": "probe", "option": "z_offset", "action": "drop", "expect": "1.5"},
        "not an object",
        _set("probe]\n[extruder", "z_offset", "1", None),
        _set("probe\nz_offset: 9", "x", "1", None),
        _set("pro[be", "z_offset", "1", None),
        _set("probe # c", "z_offset", "1", None),
        _set("probe", "z_offset: 9\nx", "1", None),
        _set("probe", "z_offset = 9", "1", None),
        _set("probe", "z_offset:9", "1", None),
        _set("probe", "z_[offset", "1", None),
        _set("probe", "z offset", "1", None),
        _set("probe", "z_offset;", "1", None),
    ])
    def test_malformed_edit(self, tree_copy, edit):
        with pytest.raises(ConfigEditRefused) as e:
            plan_edits(tree_copy, [edit])
        assert e.value.code == "invalid"

    def test_option_defined_outside_the_config_root(self, tmp_path):
        outside = tmp_path / "outside.cfg"
        outside.write_text("[printer]\nmax_accel: 9\n")
        root = tmp_path / "config"
        root.mkdir()
        (root / "printer.cfg").write_text("[include ../outside.cfg]\n")
        with pytest.raises(ConfigEditRefused) as e:
            plan_edits(root, [_set("printer", "max_accel", "10", "9")])
        assert e.value.code == "outside_root"


# ============================================================================
# An option the SAVE_CONFIG block also defines (#2157)
# ============================================================================

ISSUE_2157_ROOT = """[include machine.cfg]

[printer]
kinematics: corexy

#*# <---------------------- SAVE_CONFIG ---------------------->
#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are auto-generated.
#*#
#*# [extruder]
#*# pid_version = 1
#*# pid_target = 255.00
#*# pid_tolerance = 0.0200
#*# control = pid
#*# pid_kp = 27.945
#*# pid_ki = 2.451
#*# pid_kd = 79.647
#*#
#*# [input_shaper]
#*# shaper_type_x = mzv
#*# shaper_freq_x = 48.2
"""

ISSUE_2157_INCLUDE = """[extruder]
control: pid
pid_Kp: 24.860427
pid_Ki: 2.702253
pid_Kd: 57.178292
"""

MPC_MIGRATION = [
    _set("extruder", "control", "mpc", "pid"),
    _set("extruder", "heater_power", "50", None),
    _set("extruder", "cooling_fan", "fan", None),
]


@pytest.fixture
def shadowed_tree(tmp_path):
    """[extruder] in an include, with PID_CALIBRATE + SAVE_CONFIG results in
    printer.cfg's block, as #2157 reports it."""
    root = tmp_path / "config"
    root.mkdir()
    (root / "printer.cfg").write_text(ISSUE_2157_ROOT)
    (root / "machine.cfg").write_text(ISSUE_2157_INCLUDE)
    return root


def _autosave_body(text: str):
    lines = text.split("\n")
    start = helix_config.split_autosave(lines)
    assert start is not None, "Klipper would read the SAVE_CONFIG block as corrupted"
    return lines[start + len(helix_config.AUTOSAVE_HEADER_LINES):]


class TestSaveConfigShadow:
    @pytest.mark.parametrize("strip_duplicates", [True, False])
    def test_mpc_migration_takes_effect_under_either_reading(self, shadowed_tree,
                                                            strip_duplicates):
        write_plan(shadowed_tree, plan_edits(shadowed_tree, MPC_MIGRATION), 1700000000.0)
        extruder = klipper_effective(shadowed_tree, strip_duplicates)["extruder"]
        assert (extruder["control"], extruder["heater_power"], extruder["cooling_fan"]) == (
            "mpc", "50", "fan")

    def test_the_unpatched_reading_kept_pid_before_the_edit(self, shadowed_tree):
        (shadowed_tree / "machine.cfg").write_text(
            ISSUE_2157_INCLUDE.replace("control: pid", "control: mpc"))
        assert klipper_effective(shadowed_tree, False)["extruder"]["control"] == "pid"
        assert klipper_effective(shadowed_tree, True)["extruder"]["control"] == "mpc"

    def test_only_the_edited_options_leave_the_block(self, shadowed_tree):
        plan = plan_edits(shadowed_tree, MPC_MIGRATION)
        assert sorted(plan.changed) == ["machine.cfg", "printer.cfg"]
        assert _autosave_body(plan.changed["printer.cfg"]) == [
            "#*# [extruder]",
            "#*# pid_version = 1",
            "#*# pid_target = 255.00",
            "#*# pid_tolerance = 0.0200",
            "#*# pid_kp = 27.945",
            "#*# pid_ki = 2.451",
            "#*# pid_kd = 79.647",
            "#*#",
            "#*# [input_shaper]",
            "#*# shaper_type_x = mzv",
            "#*# shaper_freq_x = 48.2",
            "",
        ]
        assert "-#*# control = pid" in plan.diff()

    def test_both_files_are_backed_up(self, shadowed_tree):
        backups = write_plan(shadowed_tree, plan_edits(shadowed_tree, MPC_MIGRATION),
                             1700000000.0)
        assert sorted(b["file"] for b in backups) == ["machine.cfg", "printer.cfg"]
        for b in backups:
            text = (shadowed_tree / b["backup"]).read_text()
            assert text == {"machine.cfg": ISSUE_2157_INCLUDE,
                            "printer.cfg": ISSUE_2157_ROOT}[b["file"]]

    def test_a_last_block_section_left_empty_takes_the_separator_before_it(self, tmp_path):
        (tmp_path / "printer.cfg").write_text(
            "[input_shaper]\nshaper_type_x: ei\nshaper_freq_x: 40\n" + ISSUE_2157_ROOT)
        (tmp_path / "machine.cfg").write_text(ISSUE_2157_INCLUDE)
        plan = plan_edits(tmp_path, [
            _set("input_shaper", "shaper_type_x", "zv", "ei"),
            _set("input_shaper", "shaper_freq_x", "41", "40"),
        ])
        body = _autosave_body(plan.changed["printer.cfg"])
        assert body[-3:] == ["#*# pid_ki = 2.451", "#*# pid_kd = 79.647", ""]
        write_plan(tmp_path, plan, 1700000000.0)
        assert klipper_effective(tmp_path, False)["input_shaper"] == {
            "shaper_type_x": "zv", "shaper_freq_x": "41"}

    def test_a_first_block_section_left_empty_takes_the_separator_after_it(self, tmp_path):
        block = ISSUE_2157_ROOT.split("#*#\n", 1)[1].split("#*# [input_shaper]")[0]
        (tmp_path / "printer.cfg").write_text(
            "[input_shaper]\nshaper_type_x: ei\nshaper_freq_x: 40\n\n"
            + "\n".join(helix_config.AUTOSAVE_HEADER_LINES) + "\n"
            + "#*# [input_shaper]\n#*# shaper_type_x = mzv\n#*# shaper_freq_x = 48.2\n#*#\n"
            + block)
        plan = plan_edits(tmp_path, [
            _set("input_shaper", "shaper_type_x", "zv", "ei"),
            _set("input_shaper", "shaper_freq_x", "41", "40"),
        ])
        body = _autosave_body(plan.changed["printer.cfg"])
        assert body[:2] == ["#*# [extruder]", "#*# pid_version = 1"]
        assert not any("input_shaper" in line or "shaper_" in line for line in body)

    def test_remove_also_drops_the_blocks_copy(self, shadowed_tree):
        plan = plan_edits(shadowed_tree, [{"section": "extruder", "option": "control",
                                           "action": "remove", "expect": "pid"}])
        assert "#*# control = pid" not in plan.changed["printer.cfg"]
        assert "#control: pid" in plan.changed["machine.cfg"]

    def test_an_option_only_the_block_defines_is_still_refused(self, shadowed_tree):
        with pytest.raises(ConfigEditRefused) as e:
            plan_edits(shadowed_tree, [_set("extruder", "pid_target", "250", "255.00")])
        assert e.value.code == "autosave"


# ============================================================================
# Writing
# ============================================================================

class TestWrite:
    def test_write_backs_up_then_replaces(self, tree_copy):
        target = tree_copy / "conf.d/20-overrides.cfg"
        before = target.read_text()
        plan = plan_edits(tree_copy, [_set("probe", "z_offset", "1.75", "1.5")])
        backups = write_plan(tree_copy, plan, 1700000000.0)
        assert target.read_text() == plan.changed["conf.d/20-overrides.cfg"]
        assert len(backups) == 1 and backups[0]["file"] == "conf.d/20-overrides.cfg"
        assert backups[0]["backup"].startswith(".helix_backups/conf.d/20-overrides.cfg.helixbak-")
        assert (tree_copy / backups[0]["backup"]).read_text() == before

    def test_backup_names_never_collide(self, tree_copy):
        for value, expect in (("1.6", "1.5"), ("1.7", "1.6")):
            plan = plan_edits(tree_copy, [_set("probe", "z_offset", value, expect)])
            write_plan(tree_copy, plan, 1700000000.0)
        assert len(list((tree_copy / ".helix_backups/conf.d").glob("20-overrides.cfg.helixbak-*"))) == 2

    def test_crash_between_temp_and_rename_leaves_the_file_whole(self, tmp_path):
        target = tmp_path / "printer.cfg"
        target.write_text("[printer]\nmax_accel: 9\n")

        def crash(src, dst):
            assert Path(src).read_text() == "new"
            raise KeyboardInterrupt

        with pytest.raises(KeyboardInterrupt):
            atomic_write(target, "new", crash)
        assert target.read_text() == "[printer]\nmax_accel: 9\n"
        assert sorted(p.name for p in tmp_path.iterdir()) == ["printer.cfg"]

    def test_a_failed_second_file_puts_the_first_back(self, tree_copy):
        plan = plan_edits(tree_copy, [
            _set("probe", "z_offset", "1.75", "1.5"),
            _set("printer", "max_accel", "6000", "5000"),
        ])
        originals = {n: (tree_copy / n).read_text() for n in plan.changed}
        calls = []

        def second_fails(src, dst):
            calls.append(dst)
            if len(calls) == 2:
                raise OSError("disk full")
            os.replace(src, dst)

        with pytest.raises(OSError):
            write_plan(tree_copy, plan, 1700000000.0, second_fails)
        assert {n: (tree_copy / n).read_text() for n in plan.changed} == originals

    def test_file_changed_after_planning_is_refused(self, tree_copy):
        plan = plan_edits(tree_copy, [_set("probe", "z_offset", "1.75", "1.5")])
        target = tree_copy / "conf.d/20-overrides.cfg"
        target.write_text(target.read_text() + "# edited elsewhere\n")
        with pytest.raises(ConfigEditRefused) as e:
            write_plan(tree_copy, plan, 1700000000.0)
        assert e.value.code == "moved"
        assert "edited elsewhere" in target.read_text()

    def test_each_write_keeps_only_the_newest_backups(self, tree_copy):
        value = "1.5"
        for n in range(helix_config.BACKUPS_KEPT + 2):
            new = f"1.{60 + n}"
            write_plan(tree_copy, plan_edits(tree_copy, [_set("probe", "z_offset", new, value)]),
                       1700000000.0 + n)
            value = new
        backups = list((tree_copy / ".helix_backups/conf.d").glob("20-overrides.cfg.helixbak-*"))
        assert len(backups) == helix_config.BACKUPS_KEPT

    def test_backups_are_out_of_reach_of_every_include_glob(self, tree_copy):
        (tree_copy / "printer.cfg").write_text(
            "[include *]\n[include conf.d/*]\n[include **/*]\n"
            + (tree_copy / "printer.cfg").read_text()
        )
        before = read_config(tree_copy).files
        plan = plan_edits(tree_copy, [_set("probe", "z_offset", "1.75", "1.5")])
        write_plan(tree_copy, plan, 1700000000.0)
        assert read_config(tree_copy).files == before
        assert not any(".helix" in f for f in before)

    def test_old_backups_are_pruned(self, tmp_path):
        backup_dir = tmp_path / ".helix_backups"
        backup_dir.mkdir()
        for i in range(8):
            (backup_dir / f"printer.cfg.helixbak-2026010{i}-000000").write_text(str(i))
        (backup_dir / "printer.cfg.other").write_text("kept")
        prune_backups(tmp_path, "printer.cfg")
        assert (backup_dir / "printer.cfg.other").exists()
        left = sorted(p.name for p in backup_dir.glob("printer.cfg.helixbak-*"))
        assert left == [f"printer.cfg.helixbak-2026010{i}-000000" for i in range(3, 8)]


# ============================================================================
# Endpoints and the feature
# ============================================================================

class ConfigFileManager(MockFileManager):
    def __init__(self, gcodes_path, config_path):
        super().__init__(gcodes_path)
        self._config_path = config_path

    def get_directory(self, name):
        if name == "config":
            return self._config_path
        return super().get_directory(name)


@pytest.fixture
def component(helix_print_component, mock_server, tree_copy, temp_gcodes_dir):  # noqa: F811
    mock_server.components["file_manager"] = ConfigFileManager(temp_gcodes_dir, str(tree_copy))
    return helix_print_component


async def _init(component):
    await component.component_init()
    return component


class TestEndpoints:
    @pytest.mark.asyncio
    async def test_feature_is_on_whatever_plugin_features_says(self, component, mock_server):
        await mock_server.components["database"].insert_item(
            HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, {"config_edit": False}
        )
        await _init(component)
        status = await component._handle_status(MockWebRequest({}))
        assert status["features"]["config_edit"] == {
            "version": 1, "available": True, "enabled": True, "reason": None}

    @pytest.mark.asyncio
    async def test_config_read(self, component, mock_server):
        await _init(component)
        handler = mock_server.endpoints["/server/helix/config"]
        out = await handler(MockWebRequest({"section": "probe"}))
        assert out["sections"]["probe"]["options"]["z_offset"]["file"] == "conf.d/20-overrides.cfg"

    @pytest.mark.asyncio
    async def test_dry_run_writes_nothing(self, component, mock_server, tree_copy):
        await _init(component)
        before = {p: p.read_bytes() for p in tree_copy.rglob("*") if p.is_file()}
        handler = mock_server.endpoints["/server/helix/config_edit"]
        out = await handler(MockWebRequest({
            "edits": [_set("probe", "z_offset", "1.75", "1.5")], "dry_run": True}))
        assert out["dry_run"] is True and out["backups"] == []
        assert out["files"] == ["conf.d/20-overrides.cfg"]
        assert "+z_offset: 1.75" in out["diff"]
        assert {p: p.read_bytes() for p in tree_copy.rglob("*") if p.is_file()} == before

    @pytest.mark.asyncio
    async def test_edit_writes_and_returns_the_backup(self, component, mock_server, tree_copy):
        await _init(component)
        handler = mock_server.endpoints["/server/helix/config_edit"]
        out = await handler(MockWebRequest({"edits": [_set("probe", "z_offset", "1.75", "1.5")]}))
        assert "z_offset: 1.75" in (tree_copy / "conf.d/20-overrides.cfg").read_text()
        assert out["backups"][0]["backup"].startswith(".helix_backups/")
        assert (tree_copy / out["backups"][0]["backup"]).read_text().count("z_offset: 1.5") == 1

    @pytest.mark.asyncio
    async def test_refusal_is_a_409_naming_the_rule(self, component, mock_server):
        await _init(component)
        handler = mock_server.endpoints["/server/helix/config_edit"]
        with pytest.raises(Exception) as e:
            await handler(MockWebRequest({"edits": [_set("input_shaper", "shaper_freq_x",
                                                         "50", "48.2")]}))
        assert str(e.value).startswith("409: autosave")
