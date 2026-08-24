// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_has_any_preprint_options.cpp
 * @brief Truth-table coverage for the has_any_preprint_options aggregate
 *
 * `PrinterCompositeVisibilityState::update_visibility()` computes:
 *
 *   has_any_preprint_options =
 *       (plugin_installed && (bed_mesh || qgl || z_tilt || nozzle_clean || purge_line))
 *       || timelapse
 *       || framework_option_count > 0
 *
 * `print_file_detail.xml` binds this single subject to hide the entire PRINT
 * OPTIONS card when no row would render. The five per-op `can_show_*` subjects
 * that used to back this expression were retired (no consumers) — this file is
 * the replacement coverage. It exercises the aggregate end-to-end through
 * `PrinterState`, which is the only path that actually drives the inputs.
 */

#include "ui_update_queue.h"

#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "pre_print_option.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

/// Build a PrinterDiscovery with the requested objects present in `objects/list`.
/// Mirrors how Moonraker reports object names; the discovery class then derives
/// the various has_* booleans.
PrinterDiscovery hardware_with(bool bed_mesh, bool qgl, bool z_tilt, bool nozzle_clean,
                               bool timelapse) {
    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array();
    if (bed_mesh)
        objects.push_back("bed_mesh");
    if (qgl)
        objects.push_back("quad_gantry_level");
    if (z_tilt)
        objects.push_back("z_tilt");
    if (nozzle_clean)
        objects.push_back("gcode_macro CLEAN_NOZZLE");
    if (timelapse)
        objects.push_back("timelapse");
    hw.parse_objects(objects);
    return hw;
}

int read_aggregate(PrinterState& state) {
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
    return lv_subject_get_int(state.get_has_any_preprint_options_subject());
}

PrinterState& fresh_state() {
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    return state;
}

} // namespace

TEST_CASE("has_any_preprint_options: empty inputs → 0",
          "[printer_state][composite_visibility][aggregate]") {
    lv_init_safe();
    PrinterState& state = fresh_state();

    // No plugin, no hardware, no framework type, no timelapse.
    state.set_helix_plugin_installed(false);
    state.set_hardware(hardware_with(false, false, false, false, false));

    REQUIRE(read_aggregate(state) == 0);
}

TEST_CASE("has_any_preprint_options: plugin gate works for plugin-only caps",
          "[printer_state][composite_visibility][aggregate]") {
    lv_init_safe();
    PrinterState& state = fresh_state();

    SECTION("plugin=false + all plugin-gated caps present → 0 (gated off)") {
        state.set_helix_plugin_installed(false);
        state.set_hardware(hardware_with(true, true, true, true, false));
        REQUIRE(read_aggregate(state) == 0);
    }

    SECTION("plugin=true + no caps → 0 (nothing to gate-on)") {
        state.set_helix_plugin_installed(true);
        state.set_hardware(hardware_with(false, false, false, false, false));
        REQUIRE(read_aggregate(state) == 0);
    }

    SECTION("plugin=true + bed_mesh only → 1") {
        state.set_helix_plugin_installed(true);
        state.set_hardware(hardware_with(true, false, false, false, false));
        REQUIRE(read_aggregate(state) == 1);
    }

    SECTION("plugin=true + qgl only → 1 (any one cap is enough)") {
        state.set_helix_plugin_installed(true);
        state.set_hardware(hardware_with(false, true, false, false, false));
        REQUIRE(read_aggregate(state) == 1);
    }

    SECTION("plugin=true + nozzle_clean only → 1 (macro-detected cap)") {
        state.set_helix_plugin_installed(true);
        state.set_hardware(hardware_with(false, false, false, true, false));
        REQUIRE(read_aggregate(state) == 1);
    }
}

TEST_CASE("has_any_preprint_options: timelapse bypasses the plugin gate",
          "[printer_state][composite_visibility][aggregate]") {
    lv_init_safe();
    PrinterState& state = fresh_state();

    // Plugin off, timelapse on. Old code with five can_show_* subjects would
    // have ORed in printer_has_timelapse; this verifies the simplified
    // expression preserves that bypass.
    state.set_helix_plugin_installed(false);
    state.set_hardware(hardware_with(false, false, false, false, true));

    REQUIRE(read_aggregate(state) == 1);
}

TEST_CASE("has_any_preprint_options: framework option count drives card alone",
          "[printer_state][composite_visibility][aggregate]") {
    lv_init_safe();
    PrinterState& state = fresh_state();

    // Plugin off, no hardware caps, no timelapse. K2 Plus contributes
    // framework options (bed_mesh + ai_detect) via printer-type DB lookup —
    // the aggregate must see those even though plugin is off.
    state.set_helix_plugin_installed(false);
    state.set_hardware(hardware_with(false, false, false, false, false));
    REQUIRE(read_aggregate(state) == 0);

    state.set_printer_type_sync("Creality K2 Plus");
    REQUIRE(read_aggregate(state) == 1);
}

// ---------------------------------------------------------------------------
// #1094: the synthesized timelapse pre-print option must seed its default from
// the global moonraker-timelapse `enabled` setting, not a hardcoded false.
// Otherwise starting a print with the toggle untouched (default OFF) silently
// writes enabled=False globally — turning off a user's global timelapse config.
// ---------------------------------------------------------------------------

const PrePrintOption* find_timelapse_option(PrinterState& state) {
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
    return state.get_pre_print_option_set().find("timelapse");
}

TEST_CASE("timelapse pre-print default reflects global enabled=true (#1094)",
          "[printer_state][preprint][timelapse]") {
    lv_init_safe();
    PrinterState& state = fresh_state();

    state.set_timelapse_available(true);
    state.set_timelapse_default_enabled(true);

    const PrePrintOption* tl = find_timelapse_option(state);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->default_enabled == true);
}

TEST_CASE("timelapse pre-print default reflects global enabled=false (#1094)",
          "[printer_state][preprint][timelapse]") {
    lv_init_safe();
    PrinterState& state = fresh_state();

    state.set_timelapse_available(true);
    state.set_timelapse_default_enabled(false);

    const PrePrintOption* tl = find_timelapse_option(state);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->default_enabled == false);
}

TEST_CASE("timelapse pre-print label is English text, not a semantic i18n key",
          "[printer_state][preprint][timelapse][i18n]") {
    // English registers no translation pack (translation_loader.cpp skips
    // kIdentityLocale), so lv_tr(key) returns the key itself in the English
    // UI. A label_key like "pre_print_option.timelapse.label" therefore
    // renders as the raw dotted identifier on the toggle row — the v0.99.114
    // regression. Keys must be their own English text.
    lv_init_safe();
    PrinterState& state = fresh_state();

    state.set_timelapse_available(true);

    const PrePrintOption* tl = find_timelapse_option(state);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->label_key == "Timelapse");
}

TEST_CASE("has_any_preprint_options: simplified expression equivalent to old per-op OR",
          "[printer_state][composite_visibility][aggregate][equivalence]") {
    // The old code computed five (plugin && has_X) products and ORed them
    // together. The new code factors the plugin out: plugin && (any has_X).
    // Pick a few mixed states to verify the factored form lands on the same
    // value the legacy form would have produced.
    lv_init_safe();
    PrinterState& state = fresh_state();

    SECTION("plugin=true, mixed caps (bed_mesh+z_tilt) → 1") {
        state.set_helix_plugin_installed(true);
        state.set_hardware(hardware_with(true, false, true, false, false));
        REQUIRE(read_aggregate(state) == 1);
    }

    SECTION("plugin=false, mixed caps + timelapse → 1 (only timelapse contributes)") {
        state.set_helix_plugin_installed(false);
        state.set_hardware(hardware_with(true, true, true, true, true));
        REQUIRE(read_aggregate(state) == 1);
    }

    SECTION("plugin=true, all caps off → 0") {
        state.set_helix_plugin_installed(true);
        state.set_hardware(hardware_with(false, false, false, false, false));
        REQUIRE(read_aggregate(state) == 0);
    }
}
