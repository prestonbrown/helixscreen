// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which tools a print uses must not depend on how the viewer happens to be
// holding the file. Reading ParsedGCodeFile directly answered EMPTY for every
// STREAMED file, and a tool changer with 961MB of RAM (Snapmaker U1) is forced
// to stream by MemoryInfo::should_force_streaming() - so on that printer the
// answer was empty 100% of the time. Both consumers went silently dead with
// it: the print-scoped filament-runout badge, and the U1 reprint's
// SET_PRINT_USED_EXTRUDERS preamble.
//
// The files are real slicer output rather than synthesised fixtures. The
// multi-tool one is what test_detail_gcode_download_integrity.cpp already uses
// for this question: Orca, four filaments, standalone T0-T3 lines, so a correct
// answer is {0,1,2,3} and the bug's answer is {}.

#include "ui_gcode_viewer.h"
#include "ui_panel_print_status.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "app_globals.h"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// HELIX_GCODE_STREAMING is the documented override for
/// get_gcode_streaming_mode(), read fresh on every call. Saved and restored:
/// Catch2 runs the suite in one process and a leaked value would redirect every
/// later gcode load in this binary.
struct StreamingModeGuard {
    std::string prev_;
    bool had_prev_ = false;
    explicit StreamingModeGuard(const char* mode) {
        if (const char* old = ::getenv("HELIX_GCODE_STREAMING")) {
            prev_ = old;
            had_prev_ = true;
        }
        ::setenv("HELIX_GCODE_STREAMING", mode, 1);
    }
    ~StreamingModeGuard() {
        if (had_prev_) {
            ::setenv("HELIX_GCODE_STREAMING", prev_.c_str(), 1);
        } else {
            ::unsetenv("HELIX_GCODE_STREAMING");
        }
    }
};

/// Resolve a shipped gcode asset from the repo root or a build subdirectory,
/// the way MoonrakerAPIMock does.
std::string find_test_asset(const std::string& filename) {
    for (const auto& prefix : {"", "../", "../../"}) {
        std::string path = std::string(prefix) + "assets/test_gcodes/" + filename;
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return {};
}

bool g_load_done = false;
bool g_load_ok = false;

void on_load_done(lv_obj_t*, void*, bool success) {
    g_load_done = true;
    g_load_ok = success;
}

class ToolsUsedFixture : public LVGLTestFixture {
  public:
    /// Load @p path into a fresh viewer under the given streaming mode and
    /// return the tool set it answers with.
    ///
    /// The viewer is given a real size because the streaming completion path
    /// reads the widget's coords to size its 2D renderer; a zero-sized widget
    /// would exercise a different branch than the panel does.
    std::set<int> tools_used_after_load(const std::string& path, const char* mode,
                                        lv_obj_t** out_viewer = nullptr) {
        StreamingModeGuard streaming(mode);

        lv_obj_t* viewer = ui_gcode_viewer_create(lv_screen_active());
        REQUIRE(viewer != nullptr);
        lv_obj_set_size(viewer, 240, 240);
        lv_obj_update_layout(viewer);

        g_load_done = false;
        g_load_ok = false;
        ui_gcode_viewer_set_load_callback(viewer, on_load_done, nullptr);
        ui_gcode_viewer_load_file(viewer, path.c_str());

        // Both paths finish on a worker and report through the UpdateQueue, so
        // this needs real sleeps as well as virtual ticks - process_lvgl()
        // alone never yields to the parse thread.
        REQUIRE(wait_until([] { return g_load_done; }, /*timeout_ms=*/60000));
        REQUIRE(g_load_ok);

        std::set<int> tools = ui_gcode_viewer_get_tools_used(viewer);
        if (out_viewer) {
            *out_viewer = viewer;
        } else {
            ui_gcode_viewer_clear(viewer);
            lv_obj_delete(viewer);
            process_lvgl(50);
        }
        return tools;
    }
};

} // namespace

TEST_CASE_METHOD(ToolsUsedFixture, "A streamed file reports the same tools as a fully loaded one",
                 "[gcode][viewer][tools_used][slow]") {
    const std::string asset = find_test_asset("u1_4color_ring.gcode");
    REQUIRE_FALSE(asset.empty()); // run helix-tests from the repo root

    // The reference answer, from the path that always worked.
    const std::set<int> full_load = tools_used_after_load(asset, "off");
    REQUIRE(full_load == std::set<int>{0, 1, 2, 3});

    // The same file, streamed. Empty here is the U1 report.
    const std::set<int> streamed = tools_used_after_load(asset, "on");
    CHECK_FALSE(streamed.empty());
    CHECK(streamed == full_load);
}

TEST_CASE_METHOD(ToolsUsedFixture, "A single-tool file still answers {0} when streamed",
                 "[gcode][viewer][tools_used][slow]") {
    // The single-extruder convention, which is the half the two paths do NOT
    // share by construction: GCodeParser::finalize() writes {0} for a file that
    // names no tool but carries a colour palette, while the streaming index
    // scan has no opinion about palettes and reports nothing. Both answers have
    // to agree for the same file, so the convention is applied where they meet.
    // 3DBenchy carries no standalone T line at all and one filament colour, so
    // the raw index scan reports nothing and the convention is the only thing
    // that can produce an answer.
    const std::string asset = find_test_asset("3DBenchy.gcode");
    REQUIRE_FALSE(asset.empty());

    const std::set<int> full_load = tools_used_after_load(asset, "off");
    const std::set<int> streamed = tools_used_after_load(asset, "on");
    CHECK(full_load == std::set<int>{0});
    CHECK(streamed == std::set<int>{0});
}

TEST_CASE_METHOD(ToolsUsedFixture, "PrintStatusPanel reads the tool set through the viewer",
                 "[gcode][viewer][tools_used][slow]") {
    // The panel is where the two consumers actually ask. Going back to
    // ui_gcode_viewer_get_parsed_file() here re-empties both of them on exactly
    // the printers that stream, with the viewer-level answer still correct.
    const std::string asset = find_test_asset("u1_4color_ring.gcode");
    REQUIRE_FALSE(asset.empty());

    lv_obj_t* viewer = nullptr;
    const std::set<int> streamed = tools_used_after_load(asset, "on", &viewer);
    REQUIRE(viewer != nullptr);
    REQUIRE(streamed == std::set<int>{0, 1, 2, 3});

    {
        // Same construction as FanPanelFixture: the process-wide PrinterState,
        // subjects up for the panel's lifetime, torn down before the frame goes
        // away so the global XML registry stops naming this stack slot.
        PrintStatusPanel panel(get_printer_state(), nullptr);
        panel.init_subjects();

        // No viewer at all is the honest empty answer, and it must not be
        // confused with the streamed-file empty this test exists for.
        CHECK(PrintStatusPanelTestAccess::tools_used(panel).empty());

        PrintStatusPanelTestAccess::set_gcode_viewer(panel, viewer);
        CHECK(PrintStatusPanelTestAccess::tools_used(panel) == streamed);

        PrintStatusPanelTestAccess::set_gcode_viewer(panel, nullptr);
        panel.deinit_subjects();
    }

    ui_gcode_viewer_clear(viewer);
    lv_obj_delete(viewer);
    process_lvgl(50);
}
