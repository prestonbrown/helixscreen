// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "filament_op_router.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
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

#include "hv/json.hpp"

namespace helix::test {

/// A real FilamentPanel over a mock printer, so the gcode every op sends is
/// observable. With no AMS backend every op reaches the configured-macro tier.
/// @tparam Api the MoonrakerAPI the panel talks to; a test subclass can hold replies.
template <typename Api = MoonrakerAPI> struct BasicFilamentPanelHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    Api api{client, state};
    std::unique_ptr<FilamentPanel> panel;

    int prompt_count = 0;
    std::string prompted_macro;
    std::map<std::string, std::string> prompted_prefill;

    /// @p backend, when given, is installed before the panel is built. More than one
    /// of @p extruders makes a multi-tool printer with one tool per extruder, in order.
    explicit BasicFilamentPanelHarness(std::unique_ptr<AmsBackend> backend = nullptr,
                                  const std::vector<std::string>& extruders = {"extruder"}) {
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        if (backend) {
            AmsState::instance().set_backend(std::move(backend));
        }
        state.init_subjects(false);
        state.init_extruders(extruders);
        state.set_klippy_state_sync(helix::KlippyState::READY);

        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"gcode_macro LOAD_FILAMENT", "gcode_macro UNLOAD_FILAMENT",
                                  "gcode_macro PURGE"};
        for (const auto& extruder : extruders) {
            objects.push_back(extruder);
        }
        hardware.parse_objects(objects);
        if (extruders.size() > 1) {
            ToolState::instance().init_tools(hardware);
        }
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);

        helix::MacroParamCache::instance().clear();
        helix::ui::set_filament_param_prompter(
            [this](const std::string& macro, const helix::CachedMacroInfo&,
                   const std::map<std::string, std::string>& prefill, helix::MacroExecuteCallback) {
                ++prompt_count;
                prompted_macro = macro;
                prompted_prefill = prefill;
            });

        panel = std::make_unique<FilamentPanel>(state, &api);
        panel->init_subjects();
        client.clear_gcode_script_history();
    }

    ~BasicFilamentPanelHarness() {
        // A macro that ran queued its completion callbacks, which reach the panel,
        // so they run while it is still alive. Completing an op can schedule the
        // post-op cooldown, whose timer would otherwise outlive this test.
        helix::ui::UpdateQueue::instance().drain();
        PostOpCooldownManager::instance().cancel();
        helix::ui::UpdateQueue::instance().drain();

        helix::ui::set_filament_param_prompter({});
        panel.reset();
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }

    BasicFilamentPanelHarness(const BasicFilamentPanelHarness&) = delete;
    BasicFilamentPanelHarness& operator=(const BasicFilamentPanelHarness&) = delete;

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
    [[nodiscard]] std::vector<std::string> sent_for(const std::string& macro) const {
        std::vector<std::string> out;
        for (const auto& script : client.gcode_script_history()) {
            if (script == macro || script.rfind(macro + " ", 0) == 0) {
                out.push_back(script);
            }
        }
        return out;
    }
};

using FilamentPanelHarness = BasicFilamentPanelHarness<>;

} // namespace helix::test
