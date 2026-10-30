// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_has_any_preprint_options.cpp
 * @brief Truth-table coverage for the has_any_preprint_options aggregate
 *
 * `PrinterCompositeVisibilityState::update_visibility()` computes:
 *
 *   has_any_preprint_options =
 *       (plugin_installed && macro_option_count > 0)
 *       || timelapse
 *       || framework_option_count > 0
 *
 * `print_file_detail.xml` binds this single subject to hide the entire PRINT
 * OPTIONS card when no row would render. Hardware alone never shows it: a
 * printer with bed_mesh/QGL whose PRINT_START offers no skip parameter has no
 * row to draw. It exercises the aggregate end-to-end through `PrinterState`,
 * which is the only path that actually drives the inputs.
 */

#include "ui_update_queue.h"

#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "pre_print_option.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <algorithm>

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
    return lv_subject_get_int(
        state.composite_visibility_state().get_has_any_preprint_options_subject());
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

TEST_CASE("has_any_preprint_options: leveling hardware alone shows no card",
          "[printer_state][composite_visibility][aggregate]") {
    // A Voron with the plugin, bed_mesh and QGL, whose PRINT_START runs both
    // unconditionally: nothing could skip either, so there is no row to show.
    lv_init_safe();
    PrinterState& state = fresh_state();

    state.set_helix_plugin_installed(true);
    state.set_hardware(hardware_with(true, true, true, true, false));
    REQUIRE(lv_subject_get_int(state.capabilities_state().subject(Capability::HasBedMesh)) == 1);

    REQUIRE(read_aggregate(state) == 0);
}

TEST_CASE("has_any_preprint_options: macro rows show the card only with the plugin",
          "[printer_state][composite_visibility][aggregate]") {
    lv_init_safe();
    PrinterState& state = fresh_state();
    state.set_hardware(hardware_with(true, true, false, false, false));

    SECTION("plugin=true + a skippable macro op → 1") {
        state.set_helix_plugin_installed(true);
        state.set_macro_option_count(1);
        REQUIRE(read_aggregate(state) == 1);
    }

    SECTION("plugin=false + a skippable macro op → 0 (its skip needs the plugin)") {
        state.set_helix_plugin_installed(false);
        state.set_macro_option_count(1);
        REQUIRE(read_aggregate(state) == 0);
    }

    SECTION("the count dropping to zero hides the card again") {
        state.set_helix_plugin_installed(true);
        state.set_macro_option_count(2);
        REQUIRE(read_aggregate(state) == 1);
        state.set_macro_option_count(0);
        REQUIRE(read_aggregate(state) == 0);
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
    return state.profile_state().pre_print_option_set().find("timelapse");
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

// ---------------------------------------------------------------------------
// A firmware that owns timelapse declares an option for that capability in the
// database. Synthesising the plugin row on top of it gives the user two
// timelapse toggles, and where the plugin API is served by a compatibility stub
// the synthesised one silently does nothing.
// ---------------------------------------------------------------------------

TEST_CASE("a database option owning the timelapse capability suppresses the synthesized row",
          "[printer_state][preprint][timelapse][capability]") {
    lv_init_safe();
    PrinterState& state = fresh_state();

    state.set_printer_type_sync("Snapmaker U1");
    state.set_timelapse_available(true);
    state.set_timelapse_default_enabled(true);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    const PrePrintOptionSet& set = state.profile_state().pre_print_option_set();
    REQUIRE(set.declares_capability("timelapse"));
    REQUIRE(set.find("u1_timelapse") != nullptr);
    REQUIRE(set.find("timelapse") == nullptr);

    const int timelapse_rows = static_cast<int>(
        std::count_if(set.options.begin(), set.options.end(),
                      [](const PrePrintOption& o) { return o.capability_key() == "timelapse"; }));
    REQUIRE(timelapse_rows == 1);
}

TEST_CASE("a printer with no database timelapse option still gets the synthesized row",
          "[printer_state][preprint][timelapse][capability]") {
    // The control for the case above: suppression must key on the database
    // declaring the capability, not on the plugin being available.
    lv_init_safe();
    PrinterState& state = fresh_state();

    state.set_printer_type_sync("Voron 2.4");
    state.set_timelapse_available(true);
    state.set_timelapse_default_enabled(true);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    // The synthesized row carries id "timelapse", so it declares that
    // capability itself - what distinguishes the two cases is WHICH row
    // provides it, and that there is still exactly one.
    const PrePrintOptionSet& set = state.profile_state().pre_print_option_set();
    REQUIRE(set.find("timelapse") != nullptr);
    REQUIRE(set.find("u1_timelapse") == nullptr);

    const int timelapse_rows = static_cast<int>(
        std::count_if(set.options.begin(), set.options.end(),
                      [](const PrePrintOption& o) { return o.capability_key() == "timelapse"; }));
    REQUIRE(timelapse_rows == 1);
}

TEST_CASE("helix_plugin_installed keeps unknown (-1) apart from not installed (0)",
          "[printer_state][plugin][tristate]") {
    lv_init_safe();
    PrinterState& state = fresh_state();
    lv_subject_t* installed = state.plugin_status_state().get_helix_plugin_installed_subject();

    // Not yet probed: the UI can tell "still checking" from "missing".
    CHECK(lv_subject_get_int(installed) == -1);
    CHECK_FALSE(state.plugin_status_state().service_has_helix_plugin());

    state.set_helix_plugin_installed(false);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
    CHECK(lv_subject_get_int(installed) == 0);
    CHECK_FALSE(state.plugin_status_state().service_has_helix_plugin());

    state.set_helix_plugin_installed(true);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
    CHECK(lv_subject_get_int(installed) == 1);
    CHECK(state.plugin_status_state().service_has_helix_plugin());
}
