// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_detail_subjects.cpp
 * @brief Unit tests for print select detail view subject initialization
 *
 * Tests that pre-print option subjects are initialized with correct defaults:
 * - Skip switches (bed_mesh, qgl, z_tilt, nozzle_clean) default to ON (1)
 * - Add-on switches (timelapse) default to OFF (0)
 *
 * Bug context: Previously switches defaulted to OFF in XML, which caused
 * is_option_disabled() to return true even when user hadn't touched them.
 * This triggered false modification warnings when printing without plugin.
 *
 * Also covers the detail_mapping_ready skeleton-latch subject: 0 = chips not
 * authoritative (XML skeletons visible), 1 = authoritative chip state rendered.
 * It must track the tools-used cache (instant on re-prints) and the scan /
 * viewer-parse readiness the print-start gate waits on.
 */

#include "ui_callback_helpers.h"
#include "ui_print_select_detail_view.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "tools_used_cache.h"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Pre-print Option Subject Default Tests
// ============================================================================

TEST_CASE("Pre-print skip switches should default to ON (1)",
          "[print_select][detail_view][subjects]") {
    // Test the pattern used in PrintSelectDetailView::init_subjects()
    // Skip switches default to 1 (ON) = "don't skip, do what file says"

    SECTION("Bed mesh switch defaults to ON") {
        static lv_subject_t preprint_bed_mesh;
        lv_subject_init_int(&preprint_bed_mesh, 1); // Default: ON
        REQUIRE(lv_subject_get_int(&preprint_bed_mesh) == 1);
    }

    SECTION("QGL switch defaults to ON") {
        static lv_subject_t preprint_qgl;
        lv_subject_init_int(&preprint_qgl, 1); // Default: ON
        REQUIRE(lv_subject_get_int(&preprint_qgl) == 1);
    }

    SECTION("Z-tilt switch defaults to ON") {
        static lv_subject_t preprint_z_tilt;
        lv_subject_init_int(&preprint_z_tilt, 1); // Default: ON
        REQUIRE(lv_subject_get_int(&preprint_z_tilt) == 1);
    }

    SECTION("Nozzle clean switch defaults to ON") {
        static lv_subject_t preprint_nozzle_clean;
        lv_subject_init_int(&preprint_nozzle_clean, 1); // Default: ON
        REQUIRE(lv_subject_get_int(&preprint_nozzle_clean) == 1);
    }
}

TEST_CASE("Pre-print add-on switches should default to OFF (0)",
          "[print_select][detail_view][subjects]") {
    // Add-on switches default to 0 (OFF) = "don't add extras by default"

    SECTION("Timelapse switch defaults to OFF") {
        static lv_subject_t preprint_timelapse;
        lv_subject_init_int(&preprint_timelapse, 0); // Default: OFF
        REQUIRE(lv_subject_get_int(&preprint_timelapse) == 0);
    }
}

TEST_CASE("Pre-print subjects can be reset to defaults", "[print_select][detail_view][subjects]") {
    // Simulates what happens in show() - subjects reset to defaults for new file

    SECTION("Skip switch can be toggled OFF then reset to ON") {
        static lv_subject_t preprint_bed_mesh;
        lv_subject_init_int(&preprint_bed_mesh, 1); // Initial: ON

        // User toggles OFF
        lv_subject_set_int(&preprint_bed_mesh, 0);
        REQUIRE(lv_subject_get_int(&preprint_bed_mesh) == 0);

        // Reset to default when showing new file
        lv_subject_set_int(&preprint_bed_mesh, 1);
        REQUIRE(lv_subject_get_int(&preprint_bed_mesh) == 1);
    }

    SECTION("Add-on switch can be toggled ON then reset to OFF") {
        static lv_subject_t preprint_timelapse;
        lv_subject_init_int(&preprint_timelapse, 0); // Initial: OFF

        // User toggles ON
        lv_subject_set_int(&preprint_timelapse, 1);
        REQUIRE(lv_subject_get_int(&preprint_timelapse) == 1);

        // Reset to default when showing new file
        lv_subject_set_int(&preprint_timelapse, 0);
        REQUIRE(lv_subject_get_int(&preprint_timelapse) == 0);
    }
}

TEST_CASE("Subject value 1 means switch is checked (ON)", "[print_select][detail_view][subjects]") {
    // Documents the semantic meaning of subject values
    // Used by bind_state_if_eq in XML: ref_value="1" binds checked state

    SECTION("Value 1 = checked/enabled") {
        static lv_subject_t subject;
        lv_subject_init_int(&subject, 1);
        // In XML: <bind_state_if_eq subject="..." state="checked" ref_value="1"/>
        // When subject == 1, switch shows as checked (ON)
        REQUIRE(lv_subject_get_int(&subject) == 1);
    }

    SECTION("Value 0 = unchecked/disabled") {
        static lv_subject_t subject;
        lv_subject_init_int(&subject, 0);
        // When subject == 0, switch shows as unchecked (OFF)
        REQUIRE(lv_subject_get_int(&subject) == 0);
    }
}

// ============================================================================
// detail_mapping_ready skeleton latch (tools-used cache + scan readiness)
// ============================================================================

namespace {

/// No-op stand-ins for the print_file_detail.xml event callbacks (normally
/// registered by PrintSelectPanel's init_subjects). The XML references them
/// at create() time; the handlers themselves don't matter to this subject.
void detail_noop_cb(lv_event_t* /*e*/) {}

/// Per-test temp cache dir for HELIX_CACHE_DIR — keeps ToolsUsedCache disk
/// state out of the real user cache. Saves/restores the env var so later
/// tests in this binary are unaffected (tests share the process env).
struct CacheDirGuard {
    std::filesystem::path dir;
    std::string prev_env_;
    bool had_prev_ = false;
    CacheDirGuard()
        : dir(std::filesystem::temp_directory_path() /
              ("detail_subjects_test_" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(dir);
        if (const char* old = ::getenv("HELIX_CACHE_DIR")) {
            prev_env_ = old;
            had_prev_ = true;
        }
        ::setenv("HELIX_CACHE_DIR", dir.c_str(), 1);
    }
    ~CacheDirGuard() {
        if (had_prev_) {
            ::setenv("HELIX_CACHE_DIR", prev_env_.c_str(), 1);
        } else {
            ::unsetenv("HELIX_CACHE_DIR");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "detail_mapping_ready tracks cache seed and scan readiness",
                 "[print_select][detail_view][subjects]") {
    CacheDirGuard guard;

    // The fixture doesn't init PrintSelectPanel, so the panel's XML callbacks
    // aren't registered — install no-ops before creating the detail view.
    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);

    lv_subject_t* ready = lv_xml_get_subject(nullptr, "detail_mapping_ready");
    REQUIRE(ready != nullptr);
    REQUIRE(lv_subject_get_int(ready) == 0); // fresh view: skeleton armed

    // The other half of the cache seed: render_authoritative_chips() decides
    // swatch-card visibility from the PRECISE used-tool set. No AMS backend is
    // registered here, so the mapping card stays hidden and the swatch card is
    // the surface that must reflect the seeded set.
    lv_subject_t* swatches = lv_xml_get_subject(nullptr, "color_swatches_visible");
    REQUIRE(swatches != nullptr);

    const std::vector<std::string> colors{"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    // The (path, size, mtime) triple show() is called with — the cache key.
    constexpr size_t kSize = 1234;
    constexpr time_t kMtime = 5678;

    // Production close flow: go_back is deferred, so drain runs on_deactivate
    // + the destroy-on-close callback before the view goes out of scope.
    auto pop_and_drain = [&view]() {
        view.hide();
        helix::ui::UpdateQueue::instance().drain();
    };

    SECTION("warmed cache: ready=1 and tools_used seeded before activation") {
        helix::ToolsUsedCache warmer;
        warmer.store("sub/flash.gcode", kSize, kMtime, {0, 2});

        view.show("flash.gcode", "sub", "PLA", colors, {}, kSize, kMtime);

        // No drain: show() itself must have published readiness from the
        // cache hit — the deferred push/on_activate hasn't even run yet.
        REQUIRE(lv_subject_get_int(ready) == 1);
        REQUIRE(view.get_tools_used() == std::set<int>{0, 2});
        // Authoritative chips rendered in the same show() call: 2 used tools on
        // a single-extruder printer clears swatches_card_visible_for()'s >1
        // threshold, and show() had just reset this subject to 0 — so a 1 here
        // can only come from the cache seed's render.
        REQUIRE(lv_subject_get_int(swatches) == 1);

        pop_and_drain();
        REQUIRE(lv_subject_get_int(ready) == 0); // latch re-arms on deactivate
    }

    SECTION("cold cache: skeleton (0) until the scan resolves") {
        view.show("flash.gcode", "sub", "PLA", colors, {}, kSize, kMtime);
        REQUIRE(lv_subject_get_int(ready) == 0);
        // Nothing authoritative to render yet — the swatch card stays in the
        // neutral "not yet known" state show() reset it to.
        REQUIRE(lv_subject_get_int(swatches) == 0);

        // Drain runs the deferred push → on_activate → scan kick-off. With no
        // API the degrade path marks the scan done immediately — the same
        // readiness flip the real scan-finish helper performs.
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(lv_subject_get_int(ready) == 1);

        pop_and_drain();
        REQUIRE(lv_subject_get_int(ready) == 0);
    }

    SECTION("stale cache entry (mtime changed) is a miss") {
        helix::ToolsUsedCache warmer;
        warmer.store("sub/flash.gcode", kSize, kMtime, {0, 2});

        view.show("flash.gcode", "sub", "PLA", colors, {}, kSize, kMtime + 1);
        REQUIRE(lv_subject_get_int(ready) == 0); // re-sliced file → skeleton

        pop_and_drain();
    }
}
