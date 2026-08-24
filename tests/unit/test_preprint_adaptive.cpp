// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preprint_adaptive.cpp
 * @brief Adaptive bed mesh as a property of the SINGLE bed_mesh pre-print option.
 *
 * There is ONE bed_mesh toggle. When adaptive meshing is available for the
 * printer (the option declares a MacroParam adaptive_param, the firmware has
 * [exclude_object], and there's no custom bed_mesh_gcode template),
 * PrinterState::apply_dynamic_options() sets PrePrintOption::adaptive_active on
 * it. That single flag drives BOTH:
 *   - the label: "Adaptive Bed Mesh" (active) vs "Auto Bed Mesh" (not),
 *   - emission: ENABLED + active -> START_PRINT gets enable param AND adaptive
 *     token, e.g. "SKIP_LEVELING=0 ADAPTIVE=1".
 *
 * Non-adaptive printers are unchanged: label "Auto Bed Mesh"; ENABLED emits no
 * params (macro default); DISABLED emits the skip param only.
 *
 * These would FAIL if the single-toggle relabel/emission were removed or if a
 * separate sub-row reappeared.
 */

#include "ui_print_preparation_manager.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"

// Friend accessors (mirror test_print_preparation_manager.cpp).
class PrintPreparationManagerTestAccess {
  public:
    static std::vector<std::pair<std::string, std::string>>
    get_skip_params(const helix::ui::PrintPreparationManager& m) {
        return m.collect_macro_skip_params();
    }
};

#include "ui_pre_print_options_renderer.h"

namespace helix::ui {
// Exposes the private static label_for() so the relabel logic is unit-testable.
class PrePrintOptionsRendererTestAccess {
  public:
    static std::string label_for(const PrePrintOption& opt) {
        return PrePrintOptionsRenderer::label_for(opt);
    }
};
} // namespace helix::ui

#include "ui_update_queue.h"

#include "../ui_test_utils.h"
#include "app_globals.h"
#include "pre_print_option.h"
#include "printer_detector.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::PrePrintOptionsRendererTestAccess;
using helix::ui::PrintPreparationManager;

namespace {

// Mock option-state provider: 1=ENABLED, 0=DISABLED, -1/absent=not bound.
struct MockOptionState {
    std::map<std::string, int> values;
    void enable(const std::string& id) {
        values[id] = 1;
    }
    void disable(const std::string& id) {
        values[id] = 0;
    }
    [[nodiscard]] std::function<int(const std::string&)> provider() {
        return [this](const std::string& id) -> int {
            auto it = values.find(id);
            return (it != values.end()) ? it->second : -1;
        };
    }
};

// Build a bed_mesh option set. `adaptive_param` empty => no adaptive support.
// `adaptive_active` is the runtime flag normally set by apply_dynamic_options().
PrePrintOptionSet make_bed_mesh_set(const std::string& adaptive_param, bool adaptive_active,
                                    const std::string& adaptive_value = "1") {
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";

    PrePrintOption bed;
    bed.id = "bed_mesh";
    bed.category = PrePrintCategory::Mechanical;
    bed.order = 10;
    bed.default_enabled = true;
    bed.strategy_kind = PrePrintStrategyKind::MacroParam;
    PrePrintStrategyMacroParam mp;
    mp.param_name = "SKIP_LEVELING";
    mp.enable_value = "0";
    mp.skip_value = "1";
    mp.adaptive_param = adaptive_param;
    mp.adaptive_value = adaptive_value;
    bed.strategy = mp;
    bed.adaptive_active = adaptive_active;
    set.options.push_back(std::move(bed));
    return set;
}

bool has_param(const std::vector<std::pair<std::string, std::string>>& params,
               const std::string& name, const std::string& value) {
    for (const auto& [k, v] : params) {
        if (k == name && v == value) {
            return true;
        }
    }
    return false;
}

bool has_param_name(const std::vector<std::pair<std::string, std::string>>& params,
                    const std::string& name) {
    for (const auto& [k, v] : params) {
        if (k == name) {
            return true;
        }
    }
    return false;
}

// Configure an existing manager against a synthetic option set + provider.
// (PrintPreparationManager is non-movable, so we configure in place.)
//
// `has_plugin` decides whether the adaptive params can be DELIVERED: emitting
// them rewrites the START_PRINT call in the file, which needs the HelixPrint
// plugin (or a pre-start mechanism — see the setup_gcode case below). Default
// true so the emission tests state the params' content, not their gating.
void configure_manager(PrintPreparationManager& manager, PrinterState& ps, PrePrintOptionSet set,
                       MockOptionState& state, bool has_plugin = true) {
    // set_helix_plugin_installed() defers through UpdateQueue (it also refreshes
    // composite visibility subjects), so the subject is unchanged until the queue
    // runs. Without this drain service_has_helix_plugin() still reads -1 and every
    // plugin-present case silently tests the plugin-absent path.
    ps.set_helix_plugin_installed(has_plugin);
    helix::ui::UpdateQueue::instance().drain();
    PrinterStateTestAccess::set_option_set(ps, std::move(set));
    manager.set_dependencies(nullptr, &ps);
    manager.set_option_state_provider(state.provider());
}

} // namespace

// ============================================================================
// Label: single toggle relabels based on adaptive_active
// ============================================================================

TEST_CASE("PrePrint adaptive: bed_mesh label reflects adaptive_active", "[adaptive][preprint]") {
    lv_init_safe();

    SECTION("Adaptive active -> 'Adaptive Bed Mesh'") {
        PrePrintOption opt;
        opt.id = "bed_mesh";
        opt.adaptive_active = true;
        REQUIRE(PrePrintOptionsRendererTestAccess::label_for(opt) == "Adaptive Bed Mesh");
    }

    SECTION("Adaptive not active -> 'Auto Bed Mesh' (unchanged)") {
        PrePrintOption opt;
        opt.id = "bed_mesh";
        opt.adaptive_active = false;
        REQUIRE(PrePrintOptionsRendererTestAccess::label_for(opt) == "Auto Bed Mesh");
    }
}

// ============================================================================
// Emission: ENABLED + adaptive_active -> enable param AND adaptive token
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "PrePrint adaptive: enabled + active emits SKIP_LEVELING=0 and ADAPTIVE=1",
                 "[adaptive][preprint]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    MockOptionState state;
    state.enable("bed_mesh");

    PrintPreparationManager manager;
    configure_manager(manager, ps, make_bed_mesh_set("ADAPTIVE", /*active=*/true), state);
    auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);

    REQUIRE(has_param(params, "SKIP_LEVELING", "0"));
    REQUIRE(has_param(params, "ADAPTIVE", "1"));
}

TEST_CASE_METHOD(HelixTestFixture, "PrePrint adaptive: honors custom adaptive param name + value",
                 "[adaptive][preprint]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    MockOptionState state;
    state.enable("bed_mesh");

    PrintPreparationManager manager;
    configure_manager(manager, ps,
                      make_bed_mesh_set("ADAPTIVE_MESH", /*active=*/true, /*value=*/"true"), state);
    auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);

    REQUIRE(has_param(params, "ADAPTIVE_MESH", "true"));
    REQUIRE_FALSE(has_param_name(params, "ADAPTIVE"));
}

// ============================================================================
// Non-adaptive printer: unchanged behavior (no ADAPTIVE)
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "PrePrint adaptive: non-adaptive printer enabled emits no params",
                 "[adaptive][preprint]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    MockOptionState state;
    state.enable("bed_mesh");

    // adaptive_active false (e.g. no exclude_object) — behaves exactly as before.
    PrintPreparationManager manager;
    configure_manager(manager, ps, make_bed_mesh_set("ADAPTIVE", /*active=*/false), state);
    auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);

    REQUIRE_FALSE(has_param_name(params, "ADAPTIVE"));
    // ENABLED non-adaptive emits no param (relies on the macro default).
    REQUIRE_FALSE(has_param_name(params, "SKIP_LEVELING"));
}

// ============================================================================
// Disabled: skip param only, never ADAPTIVE (adaptive or not)
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "PrePrint adaptive: disabled emits SKIP_LEVELING=1 and no ADAPTIVE",
                 "[adaptive][preprint]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    MockOptionState state;
    state.disable("bed_mesh");

    // Even with adaptive available, a DISABLED bed mesh must not run — no ADAPTIVE.
    PrintPreparationManager manager;
    configure_manager(manager, ps, make_bed_mesh_set("ADAPTIVE", /*active=*/true), state);
    auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);

    REQUIRE(has_param(params, "SKIP_LEVELING", "1"));
    REQUIRE_FALSE(has_param_name(params, "ADAPTIVE"));
}

// ============================================================================
// Deliverability gate (#1269): the adaptive pair is the ONLY emission that
// fires on ENABLED, so it has no plugin-gated toggle hiding it the way every
// DISABLED emitter does. Emitting params that cannot be written into the file
// made start_print() drop them and warn on EVERY print, for the default state,
// with no visible control that could stop it.
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "PrePrint adaptive: no plugin and no pre-start mechanism emits nothing",
                 "[adaptive][preprint][1269]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    MockOptionState state;
    state.enable("bed_mesh");

    // The #1269 Voron: adaptive-capable PRINT_START, bed mesh ENABLED (default),
    // no HelixPrint plugin, no setup_gcode. The rewrite that would carry these
    // params cannot happen, so collecting them only produces a warning.
    PrintPreparationManager manager;
    configure_manager(manager, ps, make_bed_mesh_set("ADAPTIVE", /*active=*/true), state,
                      /*has_plugin=*/false);
    auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);

    REQUIRE_FALSE(has_param_name(params, "ADAPTIVE"));
    REQUIRE_FALSE(has_param_name(params, "SKIP_LEVELING"));
    // Empty is the whole point: start_print() only reaches the capability check
    // (and its warning) when the skip-param list is non-empty.
    REQUIRE(params.empty());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrePrint adaptive: printer setup_gcode delivers params without the plugin",
                 "[adaptive][preprint][1269]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    MockOptionState state;
    state.enable("bed_mesh");

    // K1/K1C shape: a printer-level setup_gcode (PRINT_PREPARED) carries the
    // intent instead of a file rewrite, and it is TRIGGERED by the skip-param
    // list being non-empty. Suppressing the emit here would silently disarm it,
    // so the plugin-absent gate must not reach this printer.
    PrePrintOptionSet set = make_bed_mesh_set("ADAPTIVE", /*active=*/true);
    set.setup_gcode = "PRINT_PREPARED";

    PrintPreparationManager manager;
    configure_manager(manager, ps, std::move(set), state, /*has_plugin=*/false);
    auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);

    REQUIRE(has_param(params, "SKIP_LEVELING", "0"));
    REQUIRE(has_param(params, "ADAPTIVE", "1"));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrePrint adaptive: disabled still emits its skip param without the plugin",
                 "[adaptive][preprint][1269]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    MockOptionState state;
    state.disable("bed_mesh");

    // The gate is scoped to the adaptive ENABLED emit only. An explicit DISABLE
    // is a choice the user made and must still be collected — start_print()
    // warning that it cannot honor it is correct behavior there, not noise.
    PrintPreparationManager manager;
    configure_manager(manager, ps, make_bed_mesh_set("ADAPTIVE", /*active=*/true), state,
                      /*has_plugin=*/false);
    auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);

    REQUIRE(has_param(params, "SKIP_LEVELING", "1"));
    REQUIRE_FALSE(has_param_name(params, "ADAPTIVE"));
}

// ============================================================================
// apply_dynamic_options drives adaptive_active end to end
// ============================================================================

TEST_CASE("PrePrint adaptive: AD5M database entry forwards ADAPTIVE", "[adaptive][preprint]") {
    // The shipped FlashForge AD5M entry must carry adaptive_param on its
    // bed_mesh option (this is the demo printer + the load_cell_probe printer).
    auto caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M");
    REQUIRE_FALSE(caps.empty());

    const PrePrintOption* bed = caps.find("bed_mesh");
    REQUIRE(bed != nullptr);
    const auto* mp = std::get_if<PrePrintStrategyMacroParam>(&bed->strategy);
    REQUIRE(mp != nullptr);
    REQUIRE(mp->adaptive_param == "ADAPTIVE");
    REQUIRE(mp->adaptive_value == "1");
}

// Full-chain regression guard: the AD5M mock's reported identity (hostname) must
// resolve, via PrinterDetector, to a DB entry whose bed_mesh option carries a
// resolvable adaptive_param. Guards against silently shipping a demo that shows
// no options (the mock previously reported a generic hostname that matched
// nothing, leaving the printer type empty and the options card bare).
TEST_CASE("PrePrint adaptive: AD5M mock identity resolves to adaptive-capable options",
          "[adaptive][preprint]") {
    // EXACT hostname the FLASHFORGE_AD5M mock reports from printer.info
    // (src/api/moonraker_client_mock_server.cpp). If that string changes, this
    // test must be updated in lockstep — that coupling is the point.
    PrinterHardwareData hardware{};
    hardware.heaters = {"extruder", "heater_bed"};
    hardware.hostname = "ad5m-mock";

    auto result = PrinterDetector::detect(hardware);
    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge Adventurer 5M");

    auto caps = PrinterDetector::get_pre_print_option_set(result.type_name);
    REQUIRE_FALSE(caps.empty());
    const PrePrintOption* bed = caps.find("bed_mesh");
    REQUIRE(bed != nullptr);
    const auto* mp = std::get_if<PrePrintStrategyMacroParam>(&bed->strategy);
    REQUIRE(mp != nullptr);
    REQUIRE(mp->adaptive_param == "ADAPTIVE");
}
