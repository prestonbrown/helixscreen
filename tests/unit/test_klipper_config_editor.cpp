// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "http_executor.h"
#include "klipper_config_editor.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix::system;

TEST_CASE("KlipperConfigEditor - section parsing", "[config][parser]") {
    KlipperConfigEditor editor;

    SECTION("Finds simple section") {
        std::string content = "[printer]\nkinematics: corexy\n\n[probe]\npin: PA1\nz_offset: 1.5\n";
        auto result = editor.parse_structure(content);
        REQUIRE(result.sections.count("probe") == 1);
        REQUIRE(result.sections["probe"].line_start > 0);
    }

    SECTION("Handles section with space in name") {
        std::string content = "[bed_mesh default]\nversion: 1\n";
        auto result = editor.parse_structure(content);
        REQUIRE(result.sections.count("bed_mesh default") == 1);
    }

    SECTION("Finds key within section") {
        std::string content = "[probe]\npin: PA1\nz_offset: 1.5\nsamples: 3\n";
        auto result = editor.parse_structure(content);
        auto key = result.find_key("probe", "z_offset");
        REQUIRE(key.has_value());
        REQUIRE(key->value == "1.5");
    }

    SECTION("Handles both : and = delimiters") {
        std::string content = "[probe]\npin: PA1\nz_offset = 1.5\n";
        auto result = editor.parse_structure(content);
        auto key1 = result.find_key("probe", "pin");
        auto key2 = result.find_key("probe", "z_offset");
        REQUIRE(key1->delimiter == ":");
        REQUIRE(key2->delimiter == "=");
    }

    SECTION("Skips multi-line values correctly") {
        std::string content =
            "[gcode_macro START]\ngcode:\n    G28\n    G1 Z10\n\n[probe]\npin: PA1\n";
        auto result = editor.parse_structure(content);
        auto key = result.find_key("probe", "pin");
        REQUIRE(key.has_value());
        REQUIRE(key->value == "PA1");
    }

    SECTION("Identifies SAVE_CONFIG boundary") {
        std::string content = "[probe]\npin: PA1\n\n"
                              "#*# <---------------------- SAVE_CONFIG ---------------------->\n"
                              "#*# DO NOT EDIT THIS BLOCK OR BELOW.\n"
                              "#*#\n"
                              "#*# [probe]\n"
                              "#*# z_offset = 1.234\n";
        auto result = editor.parse_structure(content);
        REQUIRE(result.save_config_line > 0);
    }

    SECTION("Preserves comments - not treated as keys") {
        std::string content = "# My config\n[probe]\n# Z offset\nz_offset: 1.5\n";
        auto result = editor.parse_structure(content);
        auto key = result.find_key("probe", "z_offset");
        REQUIRE(key.has_value());
        // Should only have z_offset as a key, not comments
        REQUIRE(result.sections["probe"].keys.size() == 1);
    }

    SECTION("Detects include directives") {
        std::string content =
            "[include hardware/*.cfg]\n[include macros.cfg]\n[printer]\nkinematics: corexy\n";
        auto result = editor.parse_structure(content);
        REQUIRE(result.includes.size() == 2);
        REQUIRE(result.includes[0] == "hardware/*.cfg");
        REQUIRE(result.includes[1] == "macros.cfg");
    }

    SECTION("Option names are lowercased") {
        std::string content = "[probe]\nZ_Offset: 1.5\n";
        auto result = editor.parse_structure(content);
        auto key = result.find_key("probe", "z_offset");
        REQUIRE(key.has_value());
    }

    SECTION("Handles empty file") {
        auto result = editor.parse_structure("");
        REQUIRE(result.sections.empty());
        REQUIRE(result.includes.empty());
    }

    SECTION("Handles file with only comments") {
        auto result = editor.parse_structure("# Just a comment\n; Another\n");
        REQUIRE(result.sections.empty());
    }

    SECTION("Multi-line value with empty lines preserved") {
        std::string content =
            "[gcode_macro M]\ngcode:\n    G28\n\n    G1 Z10\n\n[probe]\npin: PA1\n";
        auto result = editor.parse_structure(content);
        // The gcode macro's multi-line value spans across the empty line
        auto gcode_key = result.find_key("gcode_macro M", "gcode");
        REQUIRE(gcode_key.has_value());
        REQUIRE(gcode_key->is_multiline);
        // probe section should still be found after the multi-line value
        REQUIRE(result.sections.count("probe") == 1);
    }

    SECTION("Section line ranges are correct") {
        std::string content =
            "[printer]\nkinematics: corexy\nmax_velocity: 300\n\n[probe]\npin: PA1\n";
        auto result = editor.parse_structure(content);
        auto& printer = result.sections["printer"];
        auto& probe = result.sections["probe"];
        REQUIRE(printer.line_start < probe.line_start);
        REQUIRE(printer.line_end < probe.line_start);
    }
}

TEST_CASE("KlipperConfigEditor - value editing", "[config][editor]") {
    KlipperConfigEditor editor;

    SECTION("set_value replaces existing value") {
        std::string content = "[probe]\npin: PA1\nz_offset: 1.5\nsamples: 3\n";
        auto result = editor.set_value(content, "probe", "samples", "5");
        REQUIRE(result.has_value());
        REQUIRE(result->find("samples: 5") != std::string::npos);
        // Other values unchanged
        REQUIRE(result->find("pin: PA1") != std::string::npos);
        REQUIRE(result->find("z_offset: 1.5") != std::string::npos);
    }

    SECTION("set_value preserves delimiter style") {
        std::string content = "[probe]\nz_offset = 1.5\n";
        auto result = editor.set_value(content, "probe", "z_offset", "2.0");
        REQUIRE(result.has_value());
        REQUIRE(result->find("z_offset = 2.0") != std::string::npos);
    }

    SECTION("set_value preserves comments") {
        std::string content = "[probe]\n# Important comment\nz_offset: 1.5\n";
        auto result = editor.set_value(content, "probe", "z_offset", "2.0");
        REQUIRE(result.has_value());
        REQUIRE(result->find("# Important comment") != std::string::npos);
    }

    SECTION("set_value returns nullopt for missing key") {
        std::string content = "[probe]\npin: PA1\n";
        auto result = editor.set_value(content, "probe", "samples", "5");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("set_value returns nullopt for missing section") {
        std::string content = "[printer]\nkinematics: corexy\n";
        auto result = editor.set_value(content, "probe", "pin", "PA1");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("add_key adds to end of section") {
        std::string content = "[probe]\npin: PA1\nz_offset: 1.5\n\n[printer]\nkinematics: corexy\n";
        auto result = editor.add_key(content, "probe", "samples", "3");
        REQUIRE(result.has_value());
        REQUIRE(result->find("samples: 3") != std::string::npos);
        // Should be in [probe] section, before [printer]
        auto samples_pos = result->find("samples: 3");
        auto printer_pos = result->find("[printer]");
        REQUIRE(samples_pos < printer_pos);
    }

    SECTION("add_key returns nullopt for missing section") {
        std::string content = "[printer]\nkinematics: corexy\n";
        auto result = editor.add_key(content, "probe", "pin", "PA1");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("add_key respects custom delimiter") {
        std::string content = "[probe]\npin = PA1\n";
        auto result = editor.add_key(content, "probe", "samples", "3", " = ");
        REQUIRE(result.has_value());
        REQUIRE(result->find("samples = 3") != std::string::npos);
    }

    SECTION("remove_key comments out the line") {
        std::string content = "[probe]\npin: PA1\nsamples: 3\nz_offset: 1.5\n";
        auto result = editor.remove_key(content, "probe", "samples");
        REQUIRE(result.has_value());
        REQUIRE(result->find("#samples: 3") != std::string::npos);
        // Other keys untouched
        REQUIRE(result->find("pin: PA1") != std::string::npos);
        REQUIRE(result->find("z_offset: 1.5") != std::string::npos);
    }

    SECTION("remove_key returns nullopt for missing key") {
        std::string content = "[probe]\npin: PA1\n";
        auto result = editor.remove_key(content, "probe", "nonexistent");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("set_value handles value with spaces") {
        std::string content = "[probe]\nsamples_result: median\n";
        auto result = editor.set_value(content, "probe", "samples_result", "average");
        REQUIRE(result.has_value());
        REQUIRE(result->find("samples_result: average") != std::string::npos);
    }
}

TEST_CASE("KlipperConfigEditor - include resolution", "[config][includes]") {
    KlipperConfigEditor editor;

    SECTION("Resolves simple include") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include hardware.cfg]\n[printer]\nkinematics: corexy\n";
        files["hardware.cfg"] = "[probe]\npin: PA1\nz_offset: 1.5\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        REQUIRE(result.count("probe") == 1);
        REQUIRE(result["probe"].file_path == "hardware.cfg");
        REQUIRE(result.count("printer") == 1);
        REQUIRE(result["printer"].file_path == "printer.cfg");
    }

    SECTION("Resolves nested includes") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include hardware/main.cfg]\n[printer]\nkinematics: corexy\n";
        files["hardware/main.cfg"] = "[include probe.cfg]\n[stepper_x]\nstep_pin: PA1\n";
        files["hardware/probe.cfg"] = "[probe]\npin: PB6\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        REQUIRE(result.count("probe") == 1);
        REQUIRE(result["probe"].file_path == "hardware/probe.cfg");
        REQUIRE(result.count("stepper_x") == 1);
        REQUIRE(result["stepper_x"].file_path == "hardware/main.cfg");
    }

    SECTION("Detects circular includes without infinite loop") {
        std::map<std::string, std::string> files;
        files["a.cfg"] = "[include b.cfg]\n[section_a]\nkey: val\n";
        files["b.cfg"] = "[include a.cfg]\n[section_b]\nkey: val\n";

        auto result = editor.resolve_includes(files, "a.cfg");
        REQUIRE(result.count("section_a") == 1);
        REQUIRE(result.count("section_b") == 1);
    }

    SECTION("Caps recursion depth at max_depth") {
        std::map<std::string, std::string> files;
        files["l0.cfg"] = "[include l1.cfg]\n[s0]\nk: v\n";
        files["l1.cfg"] = "[include l2.cfg]\n[s1]\nk: v\n";
        files["l2.cfg"] = "[include l3.cfg]\n[s2]\nk: v\n";
        files["l3.cfg"] = "[include l4.cfg]\n[s3]\nk: v\n";
        files["l4.cfg"] = "[include l5.cfg]\n[s4]\nk: v\n";
        files["l5.cfg"] = "[include l6.cfg]\n[s5]\nk: v\n";
        files["l6.cfg"] = "[deep]\nk: v\n";

        // With max_depth=5, l6.cfg should NOT be reached
        auto result = editor.resolve_includes(files, "l0.cfg", 5);
        REQUIRE(result.count("s0") == 1);
        REQUIRE(result.count("s5") == 1);
        REQUIRE(result.count("deep") == 0);
    }

    SECTION("Handles missing included file gracefully") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include nonexistent.cfg]\n[printer]\nkinematics: corexy\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        REQUIRE(result.count("printer") == 1);
    }

    SECTION("Resolves relative paths from including file directory") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include hardware/sensors.cfg]\n";
        files["hardware/sensors.cfg"] = "[include probe.cfg]\n";
        files["hardware/probe.cfg"] = "[probe]\npin: PA1\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        REQUIRE(result.count("probe") == 1);
        REQUIRE(result["probe"].file_path == "hardware/probe.cfg");
    }

    SECTION("Resolves glob patterns") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include macros/*.cfg]\n[printer]\nkinematics: corexy\n";
        files["macros/start.cfg"] = "[gcode_macro START]\ngcode:\n    G28\n";
        files["macros/end.cfg"] = "[gcode_macro END]\ngcode:\n    M84\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        REQUIRE(result.count("gcode_macro START") == 1);
        REQUIRE(result.count("gcode_macro END") == 1);
    }

    SECTION("Last section wins for duplicates") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include override.cfg]\n[probe]\npin: PA1\n";
        files["override.cfg"] = "[probe]\npin: PB6\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        REQUIRE(result.count("probe") == 1);
        // printer.cfg is processed after its includes, so its [probe] wins
        REQUIRE(result["probe"].file_path == "printer.cfg");
    }
}

// ============================================================================
// apply_edits() tests
// ============================================================================

TEST_CASE("apply_edits changes existing key value", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content = "[extruder]\ncontrol: pid\npid_kp: 22.865\n";

    std::vector<ConfigEdit> edits = {
        {ConfigEdit::Type::SET_VALUE, "control", "mpc"},
    };

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE(result.has_value());
    REQUIRE(result->find("control: mpc") != std::string::npos);
    // Other keys unchanged
    REQUIRE(result->find("pid_kp: 22.865") != std::string::npos);
}

TEST_CASE("apply_edits adds new key to section", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content = "[extruder]\ncontrol: mpc\n";

    std::vector<ConfigEdit> edits = {
        {ConfigEdit::Type::ADD_KEY, "heater_power", "50"},
    };

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE(result.has_value());
    REQUIRE(result->find("heater_power: 50") != std::string::npos);
}

TEST_CASE("apply_edits ADD_KEY uses SET if key already exists", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content = "[extruder]\ncontrol: pid\nheater_power: 40\n";

    std::vector<ConfigEdit> edits = {
        {ConfigEdit::Type::ADD_KEY, "heater_power", "50"},
    };

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE(result.has_value());
    REQUIRE(result->find("heater_power: 50") != std::string::npos);
    // Should not have duplicate heater_power lines
    auto first = result->find("heater_power");
    auto second = result->find("heater_power", first + 1);
    REQUIRE(second == std::string::npos);
}

TEST_CASE("apply_edits preserves formatting and comments", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content =
        "# Extruder config\n[extruder]\n# Control algorithm\ncontrol: pid\npid_kp: 22.865\n\n"
        "[printer]\nkinematics: corexy\n";

    std::vector<ConfigEdit> edits = {
        {ConfigEdit::Type::SET_VALUE, "control", "mpc"},
    };

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE(result.has_value());
    REQUIRE(result->find("# Extruder config") != std::string::npos);
    REQUIRE(result->find("# Control algorithm") != std::string::npos);
    REQUIRE(result->find("[printer]") != std::string::npos);
    REQUIRE(result->find("kinematics: corexy") != std::string::npos);
}

TEST_CASE("apply_edits returns nullopt for missing section", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content = "[printer]\nkinematics: corexy\n";

    std::vector<ConfigEdit> edits = {
        {ConfigEdit::Type::SET_VALUE, "extruder", "mpc"},
    };

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("apply_edits with empty edits returns content unchanged", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content = "[extruder]\ncontrol: pid\n";

    std::vector<ConfigEdit> edits;

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE(result.has_value());
    REQUIRE(*result == content);
}

TEST_CASE("apply_edits changes control type and adds heater_power", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content =
        "[extruder]\ncontrol: pid\npid_kp: 22.865\npid_ki: 1.292\npid_kd: 101.178\n";

    std::vector<ConfigEdit> edits = {
        {ConfigEdit::Type::SET_VALUE, "control", "mpc"},
        {ConfigEdit::Type::ADD_KEY, "heater_power", "50"},
    };

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE(result.has_value());
    REQUIRE(result->find("control: mpc") != std::string::npos);
    REQUIRE(result->find("heater_power: 50") != std::string::npos);
    // PID values preserved (Kalico ignores them when control is mpc)
    REQUIRE(result->find("pid_kp: 22.865") != std::string::npos);
}

TEST_CASE("apply_edits REMOVE_KEY comments out the key", "[mpc_config_edit]") {
    KlipperConfigEditor editor;
    std::string content = "[extruder]\ncontrol: pid\npid_kp: 22.865\npid_ki: 1.292\n";

    std::vector<ConfigEdit> edits = {
        {ConfigEdit::Type::REMOVE_KEY, "pid_kp", ""},
    };

    auto result = editor.apply_edits(content, "extruder", edits);
    REQUIRE(result.has_value());
    REQUIRE(result->find("#pid_kp: 22.865") != std::string::npos);
    // Other keys unchanged
    REQUIRE(result->find("control: pid") != std::string::npos);
    REQUIRE(result->find("pid_ki: 1.292") != std::string::npos);
}

// ============================================================================
// Glob includes — the conf.d/*.cfg layout
//
// Reproduces the layout on a Voron whose printer.cfg carries
// `[include conf.d/*.cfg]` and whose [input_shaper] lives in
// conf.d/options.cfg. Editing input shaper settings has to find that file.
// ============================================================================

TEST_CASE("KlipperConfigEditor - glob include resolution", "[config][includes][glob]") {
    KlipperConfigEditor editor;

    SECTION("Resolves a section declared in a glob-included file") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n";
        files["conf.d/options.cfg"] = "[input_shaper]\nshaper_freq_x: 47.4\nshaper_type_x: mzv\n"
                                      "shaper_freq_y: 35.0\nshaper_type_y: mzv\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        REQUIRE(result.count("input_shaper") == 1);
        CHECK(result["input_shaper"].file_path == "conf.d/options.cfg");
    }

    SECTION("Glob matches every file in the directory, not just the first") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n";
        files["conf.d/aaa.cfg"] = "[stepper_x]\nstep_pin: PA1\n";
        files["conf.d/options.cfg"] = "[input_shaper]\nshaper_type_x: mzv\n";
        files["conf.d/zzz.cfg"] = "[probe]\npin: PB6\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        CHECK(result.count("stepper_x") == 1);
        CHECK(result.count("input_shaper") == 1);
        CHECK(result.count("probe") == 1);
        CHECK(result["input_shaper"].file_path == "conf.d/options.cfg");
    }

    SECTION("Glob does not reach into subdirectories") {
        std::map<std::string, std::string> files;
        files["printer.cfg"] = "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n";
        files["conf.d/options.cfg"] = "[input_shaper]\nshaper_type_x: mzv\n";
        files["conf.d/nested/deep.cfg"] = "[bed_mesh]\nspeed: 120\n";

        auto result = editor.resolve_includes(files, "printer.cfg");
        CHECK(result.count("input_shaper") == 1);
        CHECK(result.count("bed_mesh") == 0);
    }
}

// ============================================================================
// Mock-backed config file serving
//
// The file-transfer mock reads from assets/test_gcodes/ by basename, so a
// nested config path like conf.d/options.cfg can never be served. An
// injectable in-memory map is what makes the async config-editing paths
// testable at all.
// ============================================================================

TEST_CASE("MoonrakerAPIMock serves injected config files by full path", "[config][editor][mock]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.set_config_files({
        {"printer.cfg", "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n"},
        {"conf.d/options.cfg", "[input_shaper]\nshaper_type_x: mzv\n"},
    });

    SECTION("download_file serves a nested path exactly, not by basename") {
        std::string got;
        bool failed = false;
        api.transfers().download_file(
            "config", "conf.d/options.cfg", [&](const std::string& c) { got = c; },
            [&](const MoonrakerError&) { failed = true; });

        CHECK_FALSE(failed);
        CHECK(got == "[input_shaper]\nshaper_type_x: mzv\n");
    }

    SECTION("list_files reports the injected paths under the config root") {
        std::vector<FileInfo> listed;
        api.files().list_files(
            "config", "", true, [&](const std::vector<FileInfo>& f) { listed = f; },
            [](const MoonrakerError&) {});

        std::set<std::string> paths;
        for (const auto& f : listed)
            paths.insert(f.path.empty() ? f.filename : f.path);

        CHECK(paths.count("printer.cfg") == 1);
        CHECK(paths.count("conf.d/options.cfg") == 1);
    }

    SECTION("upload_file records what was written and download_file serves it back") {
        bool uploaded = false;
        api.transfers().upload_file(
            "config", "conf.d/options.cfg", "[input_shaper]\nshaper_type_x: ei\n",
            [&]() { uploaded = true; }, [](const MoonrakerError&) {});
        CHECK(uploaded);

        auto recorded = api.get_uploaded_config("conf.d/options.cfg");
        REQUIRE(recorded.has_value());
        CHECK(*recorded == "[input_shaper]\nshaper_type_x: ei\n");

        std::string got;
        api.transfers().download_file(
            "config", "conf.d/options.cfg", [&](const std::string& c) { got = c; },
            [](const MoonrakerError&) {});
        CHECK(got == "[input_shaper]\nshaper_type_x: ei\n");
    }

    SECTION("a path that was never injected still errors") {
        bool failed = false;
        api.transfers().download_file(
            "config", "conf.d/missing.cfg", [](const std::string&) {},
            [&](const MoonrakerError&) { failed = true; });
        CHECK(failed);
    }
}

// ============================================================================
// load_config_files() over Moonraker — the glob-include layout
//
// resolve_includes() is pure and already handles globs, but the download step
// used to skip any include containing '*', so a section living in a
// glob-included file was never in the map and safe_multi_edit() failed with
// "Section not found".
// ============================================================================

TEST_CASE("KlipperConfigEditor::load_config_files finds sections behind a glob include",
          "[config][editor][glob]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.set_config_files({
        {"printer.cfg", "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n"},
        {"conf.d/options.cfg", "[input_shaper]\nshaper_freq_x: 47.4\nshaper_type_x: mzv\n"
                               "shaper_freq_y: 35.0\nshaper_type_y: mzv\n"},
    });

    KlipperConfigEditor editor;
    std::map<std::string, SectionLocation> section_map;
    bool completed = false;
    std::string error;

    editor.load_config_files(
        api,
        [&](std::map<std::string, SectionLocation> map) {
            section_map = std::move(map);
            completed = true;
        },
        [&](const std::string& err) { error = err; });
    helix::ui::UpdateQueue::instance().drain_until_empty();

    REQUIRE(error.empty());
    REQUIRE(completed);

    REQUIRE(section_map.count("input_shaper") == 1);
    CHECK(section_map["input_shaper"].file_path == "conf.d/options.cfg");
    CHECK(section_map.count("printer") == 1);

    // safe_multi_edit() reads the file it is about to rewrite out of this cache,
    // so a section map entry with no cached content is useless.
    auto cached = editor.get_cached_file("conf.d/options.cfg");
    REQUIRE(cached.has_value());
    CHECK(cached->find("shaper_freq_x: 47.4") != std::string::npos);
}

TEST_CASE("KlipperConfigEditor::load_config_files stops at a printer teardown mid-walk",
          "[config][editor][include_walk_lifetime]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    auto api = std::make_unique<MoonrakerAPIMock>(client, state);
    api->set_config_files({{"printer.cfg", "[include hardware.cfg]\n[printer]\n"},
                           {"hardware.cfg", "[stepper_x]\nstep_pin: PA1\n"}});

    KlipperConfigEditor editor;
    bool completed = false;
    std::string error;
    editor.load_config_files(
        *api, [&](std::map<std::string, SectionLocation>) { completed = true; },
        [&](const std::string& err) { error = err; });

    helix::ui::UpdateQueue::instance().drain(); // printer.cfg arrives
    api.reset();
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(completed);
    CHECK(error.find("disconnected") != std::string::npos);
}

TEST_CASE("KlipperConfigEditor::load_config_files still resolves plain includes",
          "[config][editor][glob]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Non-glob chain, two levels deep — the shape the probe overlay and the PID
    // migration path rely on.
    api.set_config_files({
        {"printer.cfg", "[include hardware.cfg]\n[printer]\nkinematics: cartesian\n"},
        {"hardware.cfg", "[include probe.cfg]\n[stepper_x]\nstep_pin: PA1\n"},
        {"probe.cfg", "[probe]\npin: PB6\nz_offset: 1.5\n"},
        {"orphan.cfg", "[bed_mesh]\nspeed: 120\n"},
    });

    KlipperConfigEditor editor;
    std::map<std::string, SectionLocation> section_map;
    bool completed = false;

    editor.load_config_files(
        api,
        [&](std::map<std::string, SectionLocation> map) {
            section_map = std::move(map);
            completed = true;
        },
        [](const std::string&) {});
    helix::ui::UpdateQueue::instance().drain_until_empty();

    REQUIRE(completed);
    REQUIRE(section_map.count("probe") == 1);
    CHECK(section_map["probe"].file_path == "probe.cfg");
    CHECK(section_map["stepper_x"].file_path == "hardware.cfg");
    CHECK(section_map["printer"].file_path == "printer.cfg");

    // A .cfg sitting in the config dir that nothing includes must NOT be active.
    CHECK(section_map.count("bed_mesh") == 0);
    // Nor downloaded: config_backups/ alone holds dozens of these on a real printer.
    CHECK_FALSE(editor.get_cached_file("orphan.cfg").has_value());

    CHECK(editor.get_cached_file("probe.cfg").has_value());
}

// ============================================================================
// Creating a section that is not in the file yet
//
// A printer that has never been shaper-calibrated has no [input_shaper] at all,
// so "edit the section" is not enough — the section itself has to be created,
// above the SAVE_CONFIG block Klipper owns.
// ============================================================================

TEST_CASE("apply_edits creates an absent section", "[config][editor]") {
    KlipperConfigEditor editor;

    SECTION("Appends the section and its keys, preserving what was there") {
        std::string content = "# my printer\n[printer]\nkinematics: corexy\nmax_velocity: 300\n";

        std::vector<ConfigEdit> edits = {
            {ConfigEdit::Type::ADD_KEY, "shaper_type_x", "mzv"},
            {ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "47.4"},
        };

        auto result = editor.apply_edits(content, "input_shaper", edits);
        REQUIRE(result.has_value());

        // Original content survives, comment included
        CHECK(result->find("# my printer") != std::string::npos);
        CHECK(result->find("kinematics: corexy") != std::string::npos);
        CHECK(result->find("max_velocity: 300") != std::string::npos);

        // The new section parses back with both keys attached to it
        auto structure = editor.parse_structure(*result);
        REQUIRE(structure.sections.count("input_shaper") == 1);
        auto type_key = structure.find_key("input_shaper", "shaper_type_x");
        auto freq_key = structure.find_key("input_shaper", "shaper_freq_x");
        REQUIRE(type_key.has_value());
        REQUIRE(freq_key.has_value());
        CHECK(type_key->value == "mzv");
        CHECK(freq_key->value == "47.4");

        // Section header appears exactly once
        CHECK(result->find("[input_shaper]") == result->rfind("[input_shaper]"));
    }

    SECTION("Inserts before the SAVE_CONFIG block, never inside or after it") {
        std::string content =
            "[printer]\nkinematics: corexy\n"
            "\n"
            "#*# <---------------------- SAVE_CONFIG ---------------------->\n"
            "#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are auto-generated.\n"
            "#*#\n"
            "#*# [probe]\n"
            "#*# z_offset = 1.234\n";

        std::vector<ConfigEdit> edits = {
            {ConfigEdit::Type::ADD_KEY, "shaper_type_x", "mzv"},
        };

        auto result = editor.apply_edits(content, "input_shaper", edits);
        REQUIRE(result.has_value());

        auto section_pos = result->find("[input_shaper]");
        auto save_pos = result->find("#*# <-");
        REQUIRE(section_pos != std::string::npos);
        REQUIRE(save_pos != std::string::npos);
        CHECK(section_pos < save_pos);

        // The key landed with the section, also above the block
        auto key_pos = result->find("shaper_type_x: mzv");
        REQUIRE(key_pos != std::string::npos);
        CHECK(key_pos < save_pos);

        // Klipper's block is untouched
        CHECK(result->find("#*# z_offset = 1.234") != std::string::npos);
        CHECK(result->find("#*# [probe]") != std::string::npos);
    }

    SECTION("Edits an existing section in place instead of duplicating it") {
        std::string content = "[input_shaper]\n# tuned by hand\nshaper_type_x: ei\n"
                              "shaper_freq_x: 60.0\n\n[printer]\nkinematics: corexy\n";

        std::vector<ConfigEdit> edits = {
            {ConfigEdit::Type::ADD_KEY, "shaper_type_x", "mzv"},
            {ConfigEdit::Type::ADD_KEY, "shaper_freq_y", "35.0"},
        };

        auto result = editor.apply_edits(content, "input_shaper", edits);
        REQUIRE(result.has_value());

        // Exactly one [input_shaper] header
        CHECK(result->find("[input_shaper]") == result->rfind("[input_shaper]"));
        CHECK(result->find("shaper_type_x: mzv") != std::string::npos);
        CHECK(result->find("shaper_type_x: ei") == std::string::npos);
        CHECK(result->find("shaper_freq_x: 60.0") != std::string::npos);
        CHECK(result->find("shaper_freq_y: 35.0") != std::string::npos);
        CHECK(result->find("# tuned by hand") != std::string::npos);
    }

    SECTION("SET_VALUE on an absent section is still an error") {
        std::string content = "[printer]\nkinematics: corexy\n";
        std::vector<ConfigEdit> edits = {
            {ConfigEdit::Type::SET_VALUE, "shaper_type_x", "mzv"},
        };
        CHECK_FALSE(editor.apply_edits(content, "input_shaper", edits).has_value());
    }

    SECTION("Empty edit list does not invent a section") {
        std::string content = "[printer]\nkinematics: corexy\n";
        CHECK_FALSE(editor.apply_edits(content, "input_shaper", {}).has_value());
    }
}

TEST_CASE("add_section places a new section above SAVE_CONFIG", "[config][editor]") {
    KlipperConfigEditor editor;

    SECTION("Appends to the end when there is no SAVE_CONFIG block") {
        std::string content = "[printer]\nkinematics: corexy\n";
        auto result = editor.add_section(content, "input_shaper");
        REQUIRE(result.has_value());
        auto structure = editor.parse_structure(*result);
        CHECK(structure.sections.count("input_shaper") == 1);
        CHECK(structure.sections.count("printer") == 1);
    }

    SECTION("Refuses to create a section that already exists") {
        std::string content = "[input_shaper]\nshaper_type_x: ei\n";
        CHECK_FALSE(editor.add_section(content, "input_shaper").has_value());
    }
}

// ============================================================================
// safe_multi_edit() and a section no active file declares
//
// apply_edits() creates an absent section for an all-ADD_KEY list, but it never
// used to get the chance: safe_multi_edit() bailed at the section-map lookup, so
// Save on a printer that had never been shaper-calibrated failed outright. The
// fallback picks printer.cfg - and it must stay scoped to ADD_KEY, or a typo'd
// section name in a SET_VALUE caller would silently create a new section
// instead of erroring.
//
// Both cases here return before any HttpExecutor work is submitted, so neither
// leaves a worker behind.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "safe_multi_edit creates an absent section for an all-ADD_KEY edit list",
                 "[config][editor]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.set_config_files({
        {"printer.cfg", "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n"},
        {"conf.d/options.cfg", "[stepper_x]\nstep_pin: PA1\n"},
    });

    // Never seeing a disconnect is what the health monitor reads as a fast
    // restart, i.e. the success path. Leaving the mock DISCONNECTED would send
    // it down the revert branch instead.
    helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                             helix::ConnectionState::CONNECTED);

    KlipperConfigEditor editor;
    bool succeeded = false;
    std::string error;

    editor.safe_multi_edit(
        api, "input_shaper",
        {
            {ConfigEdit::Type::ADD_KEY, "shaper_type_x", "mzv"},
            {ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"},
        },
        [&]() { succeeded = true; }, [&](const std::string& err) { error = err; },
        /*restart_timeout_ms=*/300);
    // The config walk downloads from the main thread: run it up to the write.
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // The health monitor polls on an HttpExecutor::fast() worker that captured
    // `api` and `editor` BY REFERENCE. Returning while it is still running is a
    // use-after-free that detonates in whatever runs next, so joining is not
    // optional bookkeeping - it is the thing keeping this test honest.
    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 15000));
    helix::ui::UpdateQueue::instance().drain();

    CHECK(error.empty());
    CHECK(succeeded);

    auto written = api.get_uploaded_config("printer.cfg");
    REQUIRE(written.has_value());
    CHECK(written->find("[input_shaper]") != std::string::npos);
    CHECK(written->find("shaper_type_x: mzv") != std::string::npos);
    CHECK(written->find("shaper_freq_x: 41.6") != std::string::npos);

    // The root's own content is not collateral damage.
    CHECK(written->find("kinematics: corexy") != std::string::npos);
    CHECK(written->find("[include conf.d/*.cfg]") != std::string::npos);

    // The mock's FIRMWARE_RESTART also queues a deferred SHUTDOWN that arms a
    // 1s "back to READY" timer. Run it out here, or it lands inside whatever
    // test comes next and reads as that test leaking.
    process_lvgl(1200);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "safe_multi_edit still refuses a missing section when an edit is not ADD_KEY",
                 "[config][editor]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    const std::string root = "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n";
    api.set_config_files({
        {"printer.cfg", root},
        {"conf.d/options.cfg", "[stepper_x]\nstep_pin: PA1\n"},
    });

    KlipperConfigEditor editor;
    bool succeeded = false;
    std::string error;

    editor.safe_multi_edit(
        api, "extrudr", // deliberate typo: the section genuinely does not exist
        {
            {ConfigEdit::Type::SET_VALUE, "control", "mpc"},
            {ConfigEdit::Type::ADD_KEY, "heater_power", "40"},
        },
        [&]() { succeeded = true; }, [&](const std::string& err) { error = err; });
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // Bails at the section lookup, so no worker was ever submitted - but assert
    // that rather than assume it, since a regression that reaches the upload
    // would leave one holding `api` past the end of this scope.
    REQUIRE(helix::http::HttpExecutor::fast().inflight() == 0);

    CHECK_FALSE(succeeded);
    CHECK(error.find("not found") != std::string::npos);

    // Nothing was written anywhere - not a new [extrudr], not a backup.
    auto written = api.get_uploaded_config("printer.cfg");
    REQUIRE(written.has_value());
    CHECK(*written == root);
}

// ============================================================================
// The FIRMWARE_RESTART health monitor must not outlive the editor
//
// The monitor runs on an HttpExecutor::fast() worker and captures `this` (a
// KlipperConfigEditor that is a MEMBER of InputShaperPanel / PIDCalibrationPanel)
// and `api` BY REFERENCE, then polls for up to 30s. Shutdown destroys the panels
// (StaticPanelRegistry::destroy_all, application.cpp) well before the executor
// workers are stopped, so the destructor is what has to cancel the monitor and
// wait for it - otherwise the poll loop dereferences a freed editor.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "KlipperConfigEditor destructor cancels and joins the restart health monitor",
                 "[config][editor]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.set_config_files({
        {"printer.cfg", "[printer]\nkinematics: corexy\n[input_shaper]\nshaper_freq_x: 40\n"},
    });

    // Stays CONNECTED, so the monitor sits in its phase-1 "wait for the restart
    // disconnect" poll for the whole timeout. That poll is the window in which
    // shutdown destroys the panel that owns the editor.
    helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                             helix::ConnectionState::CONNECTED);

    // Idempotent - an earlier test may have left the lane stopped.
    helix::http::HttpExecutor::fast().start();

    bool succeeded = false;
    std::string error;

    auto editor = std::make_unique<KlipperConfigEditor>();
    editor->safe_multi_edit(
        api, "input_shaper", {{ConfigEdit::Type::SET_VALUE, "shaper_freq_x", "41.6"}},
        [&]() { succeeded = true; }, [&](const std::string& err) { error = err; },
        /*restart_timeout_ms=*/3000);

    // Assert the monitor is genuinely running before destroying the editor.
    // Without this, a regression that never submits it would pass everything
    // below vacuously.
    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 1; }, 2000));

    auto t0 = std::chrono::steady_clock::now();
    editor.reset(); // The destructor must cancel the monitor AND wait for it.
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    // Provably finished by the time ~KlipperConfigEditor() returned. The 250ms
    // budget is slack for HttpExecutor's inflight counter, which the worker
    // decrements a few instructions AFTER satisfying the future the destructor
    // waited on - it is not time for a live monitor to finish, since one polls
    // at 500ms and runs for 3000ms.
    CHECK(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 250));
    // Cancelled at the next poll wake-up, not after the full 3000ms timeout -
    // this runs on the main thread during shutdown, so it may not stall.
    CHECK(elapsed < std::chrono::milliseconds(1500));

    // Cancelled means cancelled: no success, no error. Both callbacks capture
    // panel state that shutdown is in the middle of freeing.
    CHECK_FALSE(succeeded);
    CHECK(error.empty());

    // Safety net for a red run (no-op once green): never return into the next
    // test with a monitor still polling a freed editor.
    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 8000));

    // The mock's FIRMWARE_RESTART queued a deferred SHUTDOWN with a 1s timer.
    // Run it out here so it doesn't land inside the next test.
    process_lvgl(1200);
    helix::ui::UpdateQueue::instance().drain();
}

// safe_edit_value() arms its own copy of the same monitor. It is a separate
// submit() call site, so it needs its own coverage - wiring one site and not the
// other compiles and passes every test above.
TEST_CASE_METHOD(LVGLTestFixture,
                 "KlipperConfigEditor destructor also joins the safe_edit_value monitor",
                 "[config][editor]") {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.set_config_files({
        {"printer.cfg", "[printer]\nkinematics: corexy\n[input_shaper]\nshaper_freq_x: 40\n"},
    });
    helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                             helix::ConnectionState::CONNECTED);
    helix::http::HttpExecutor::fast().start();

    bool succeeded = false;
    std::string error;

    auto editor = std::make_unique<KlipperConfigEditor>();

    // safe_edit_value() edits through the cached section map (safe_multi_edit()
    // loads it itself), so prime it first or the edit bails before the restart.
    bool loaded = false;
    editor->load_config_files(
        api, [&](std::map<std::string, SectionLocation>) { loaded = true; },
        [&](const std::string& err) { error = err; });
    REQUIRE(wait_until([&]() { return loaded; }, 2000));

    editor->safe_edit_value(
        api, "input_shaper", "shaper_freq_x", "41.6", [&]() { succeeded = true; },
        [&](const std::string& err) { error = err; }, /*restart_timeout_ms=*/3000);

    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 1; }, 2000));

    auto t0 = std::chrono::steady_clock::now();
    editor.reset();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    // Same inflight-counter slack as the safe_multi_edit case above.
    CHECK(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 250));
    CHECK(elapsed < std::chrono::milliseconds(1500));
    CHECK_FALSE(succeeded);
    CHECK(error.empty());

    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 8000));
    process_lvgl(1200);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("KlipperConfigEditor - duplicate keys edit the last occurrence", "[config][editor]") {
    KlipperConfigEditor editor;

    SECTION("set_value rewrites the last copy only") {
        std::string content = "[s]\ncut: False\ncut: True\n";
        auto result = editor.set_value(content, "s", "cut", "X");
        REQUIRE(result.has_value());
        CHECK(*result == "[s]\ncut: False\ncut: X\n");
    }

    SECTION("remove_key comments out the last copy only") {
        std::string content = "[s]\ncut: False\ncut: True\n";
        auto result = editor.remove_key(content, "s", "cut");
        REQUIRE(result.has_value());
        CHECK(*result == "[s]\ncut: False\n#cut: True\n");
    }

    SECTION("a last copy with continuation lines keeps its own range") {
        std::string content = "[s]\ng: A\ng:\n  B\n  C\n[t]\nx: 1\n";
        auto removed = editor.remove_key(content, "s", "g");
        REQUIRE(removed.has_value());
        CHECK(*removed == "[s]\ng: A\n#g:\n#  B\n#  C\n[t]\nx: 1\n");
    }
}
