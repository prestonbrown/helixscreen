// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_filament.h"

#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "filament_op_router.h"
#include "json_fwd.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_types.h"
#include "post_op_cooldown_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "tool_state.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// A real FilamentPanel with no AMS backend over a mock printer, so every op
/// reaches the configured-macro tier and the gcode it sends is observable. The
/// shared executor sends Load and Unload through the process-wide API, so the
/// harness installs its own for its lifetime.
struct FilamentPanelMacroHarness {
    using ParamValues = std::map<std::string, std::string>;
    using Scripts = std::vector<std::string>;

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::unique_ptr<FilamentPanel> panel;

    int prompt_count = 0;
    std::string prompted_macro;
    ParamValues prompted_prefill;
    IMoonrakerAPI* previous_api = nullptr;

    /// @p backend, when given, is installed before the panel is built.
    explicit FilamentPanelMacroHarness(std::unique_ptr<AmsBackend> backend = nullptr) {
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        if (backend) {
            AmsState::instance().set_backend(std::move(backend));
        }
        state.init_subjects(false);
        state.temperature_state().init_extruders({"extruder"});
        state.set_klippy_state_sync(helix::KlippyState::READY);

        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro LOAD_FILAMENT",
                                  "gcode_macro UNLOAD_FILAMENT", "gcode_macro PURGE"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);

        helix::MacroParamCache::instance().clear();
        helix::ui::set_filament_param_prompter(
            [this](const std::string& macro, const helix::CachedMacroInfo&,
                   const ParamValues& prefill, helix::MacroExecuteCallback) {
                ++prompt_count;
                prompted_macro = macro;
                prompted_prefill = prefill;
            });

        previous_api = get_moonraker_api();
        set_moonraker_api(&api);

        panel = std::make_unique<FilamentPanel>(state, &api);
        panel->init_subjects();
        client.clear_gcode_script_history();
    }

    ~FilamentPanelMacroHarness() {
        // A macro that ran queued its completion callbacks, which reach the panel,
        // so they run while it is still alive. Completing an op can schedule the
        // post-op cooldown, whose timer would otherwise outlive this test.
        helix::ui::UpdateQueue::instance().drain();
        PostOpCooldownManager::instance().cancel();
        helix::ui::UpdateQueue::instance().drain();

        helix::ui::set_filament_param_prompter({});
        panel.reset();
        set_moonraker_api(previous_api);
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }

    /// populate_from_configfile() replaces the cache, so every macro goes in one call.
    static void cache_macros(std::initializer_list<std::pair<const char*, const char*>> macros) {
        nlohmann::json config;
        std::unordered_set<std::string> names;
        for (const auto& [name, gcode] : macros) {
            config[std::string("gcode_macro ") + name]["gcode"] = gcode;
            names.insert(name);
        }
        helix::MacroParamCache::instance().populate_from_configfile(config, names);
    }

    /// The extruder target Klipper reports, in degrees.
    void set_extruder_target(double degrees) {
        state.update_from_status({{"extruder", {{"target", degrees}}}});
    }

    /// The printer's safety limits as discovery hands them to the panel: Klipper's
    /// min_extrude_temp and the hotend's max_temp.
    void set_safety_limits(double min_extrude_c, double nozzle_max_c = 300.0) {
        SafetyLimits limits;
        limits.min_extrude_temp_celsius = min_extrude_c;
        limits.set_max_temp_for("extruder", nozzle_max_c);
        panel->set_limits(limits);
    }

    /// Two hotends, T0 on `extruder` and T1 on `extruder1`, as discovery builds them.
    void use_two_extruders() {
        state.temperature_state().init_extruders({"extruder", "extruder1"});
        helix::PrinterDiscovery hardware;
        hardware.parse_objects(nlohmann::json{"extruder", "extruder1"});
        ToolState::instance().init_tools(hardware);
        REQUIRE(ToolState::instance().tool_count() == 2);
    }

    /// `extruder` extrudes from 170 up to 300; `extruder1` from 200 up to 250. The
    /// primary extruder's floor is also the global one, as the config parse leaves it.
    static SafetyLimits two_extruder_limits() {
        SafetyLimits limits;
        limits.min_extrude_temp_celsius = 170.0;
        limits.set_min_extrude_temp_for("extruder", 170.0);
        limits.set_max_temp_for("extruder", 300.0);
        limits.set_min_extrude_temp_for("extruder1", 200.0);
        limits.set_max_temp_for("extruder1", 250.0);
        return limits;
    }

    /// Both extruder targets, in degrees.
    void set_extruder_targets(double extruder_c, double extruder1_c) {
        state.update_from_status(
            {{"extruder", {{"target", extruder_c}}}, {"extruder1", {{"target", extruder1_c}}}});
    }

    /// An external spool whose material heats to exactly @p nozzle_c: a name the
    /// filament database does not know, so the spool's own temperatures stand.
    static void set_external_spool(int nozzle_c) {
        SlotInfo spool;
        spool.material = "Spool Test Filament";
        spool.nozzle_temp_min = nozzle_c;
        spool.nozzle_temp_max = nozzle_c;
        AmsState::instance().set_external_spool_info_in_memory(spool);
    }

    /// Every script sent whose first word is @p macro.
    [[nodiscard]] Scripts sent_for(const std::string& macro) const {
        Scripts out;
        for (const auto& script : client.gcode_script_history()) {
            if (script == macro || script.rfind(macro + " ", 0) == 0) {
                out.push_back(script);
            }
        }
        return out;
    }
};

} // namespace helix::test
