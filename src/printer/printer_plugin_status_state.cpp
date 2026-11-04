// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_plugin_status_state.cpp
 * @brief HelixPrint plugin status management extracted from PrinterState
 *
 * Manages plugin installation subjects for UI feature gating.
 * Uses tri-state semantics (-1=unknown, 0=no, 1=yes) to distinguish between
 * "still checking" and "definitely not available" states.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_plugin_status_state.h"

#include "core/gcode_filename.h"
#include "macro_manager.h"
#include "plr_backend.h"
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

namespace helix {

void PrinterPluginStatusState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterPluginStatusState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterPluginStatusState] Initializing subjects (register_xml={})",
                  register_xml);

    // Plugin status subjects use tri-state: -1=unknown, 0=no, 1=yes
    // Unknown state allows UI to show "checking..." vs "not available"
    INIT_SUBJECT_INT(helix_plugin_installed, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(helix_macros_status, static_cast<int>(HelixMacrosStatus::Unknown), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(plr_feature_state, static_cast<int>(PluginFeatureState::Absent), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(plr_saving_enabled, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(plr_mesh_include, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(helix_plugin_probe_seq, 0, subjects_, register_xml);
    for (size_t i = 0; i < kPluginSwitchFeatures.size(); ++i) {
        const PluginSwitchFeature& f = kPluginSwitchFeatures[i];
        lv_subject_init_int(&switch_mode_[i], static_cast<int>(PluginSwitchMode::Hidden));
        subjects_.register_subject(&switch_mode_[i], register_xml ? f.mode_subject : nullptr);
        if (register_xml) {
            helix::xml::register_subject_in_current_scope(f.mode_subject, &switch_mode_[i]);
        }
        lv_subject_init_int(&switch_on_[i], 0);
        const char* on_name = register_xml ? f.on_subject : nullptr;
        subjects_.register_subject(&switch_on_[i], on_name);
        if (on_name != nullptr) {
            helix::xml::register_subject_in_current_scope(on_name, &switch_on_[i]);
        }
    }

    // Fresh subjects mean a fresh session: re-initialization (test fixtures
    // re-init the shared PrinterState between cases) must not read a base
    // status or pending flag from the previous one.
    macros_base_status_ = static_cast<int>(HelixMacrosStatus::Unknown);
    macros_restart_pending_ = false;

    subjects_initialized_ = true;
    spdlog::trace("[PrinterPluginStatusState] Subjects initialized successfully");
}

void PrinterPluginStatusState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterPluginStatusState] Deinitializing subjects");

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterPluginStatusState::set_installed(bool installed) {
    lv_subject_set_int(&helix_plugin_installed_,
                       installed ? kHelixPluginInstalled : kHelixPluginAbsent);
    spdlog::info("[PrinterPluginStatusState] HelixPrint plugin installed: {}", installed);
    publish_features();
}

void PrinterPluginStatusState::publish_features() {
    if (!subjects_initialized_) {
        return;
    }
    for (size_t i = 0; i < kPluginSwitchFeatures.size(); ++i) {
        const PluginSwitchFeature& f = kPluginSwitchFeatures[i];
        // Until the probe answers, nothing is known to offer or switch.
        const PluginSwitchMode mode =
            helix_plugin_state() < 0
                ? PluginSwitchMode::Hidden
                : plugin_switch_mode(service_has_helix_plugin(), features_, f.name, f.min_version,
                                     f.has_fallback, screen_local());
        lv_subject_set_int(&switch_mode_[i], static_cast<int>(mode));
        const auto it = features_.find(f.name);
        if (mode == PluginSwitchMode::Hidden && it != features_.end()) {
            spdlog::debug("[PrinterPluginStatusState] {} unavailable here: {}", f.name,
                          it->second.reason);
        }
        // The plugin's own `enabled` while it reports the feature, else the
        // stored opt-in the app's fallback acts on.
        const bool on = it != features_.end() && (it->second.available || !f.has_fallback)
                            ? it->second.enabled
                            : plugin_feature_opted_in(stored_opt_ins_, f.name, f.acts);
        lv_subject_set_int(&switch_on_[i], on ? 1 : 0);
    }

    const PluginFeatureState state = plugin_feature(kPlrFeatureName, kPlrFeatureMinVersion);
    lv_subject_set_int(&plr_feature_state_, static_cast<int>(state));
    if (state == PluginFeatureState::Absent) {
        set_plr_mesh_include(std::nullopt);
    }
    bool saving = state == PluginFeatureState::Ready;
    if (state != PluginFeatureState::Absent) {
        const auto it = features_.find(kPlrFeatureName);
        if (it != features_.end() && it->second.saving) {
            saving = *it->second.saving;
        }
    }
    lv_subject_set_int(&plr_saving_enabled_, saving ? 1 : 0);
}

void PrinterPluginStatusState::set_plr_mesh_include(std::optional<bool> present) {
    lv_subject_set_int(&plr_mesh_include_, present.has_value() ? (*present ? 1 : 0) : -1);
}

void PrinterPluginStatusState::set_link_dir(const std::string& dir) {
    link_dir_ = dir;
    gcode::set_plugin_link_dir(dir);
}

void PrinterPluginStatusState::set_helix_macros_base_status(HelixMacrosStatus base) {
    macros_base_status_ = static_cast<int>(base);
    if (base == HelixMacrosStatus::Installed) {
        // Discovery sees the CURRENT pack active: a Klipper restart landed and
        // any staged install/update offer is resolved. An Outdated base does
        // NOT clear the flag — until the restart, discovery still reports the
        // old rung, which is exactly the state the flag describes.
        macros_restart_pending_ = false;
    }
    publish_helix_macros_status();
    spdlog::info("[PrinterPluginStatusState] Helper macros base status: {}", macros_base_status_);
}

void PrinterPluginStatusState::set_helix_macros_base_status(MacroInstallStatus install) {
    switch (install) {
    case MacroInstallStatus::NOT_INSTALLED:
        set_helix_macros_base_status(HelixMacrosStatus::NotInstalled);
        break;
    case MacroInstallStatus::INSTALLED:
        set_helix_macros_base_status(HelixMacrosStatus::Installed);
        break;
    case MacroInstallStatus::OUTDATED:
        set_helix_macros_base_status(HelixMacrosStatus::Outdated);
        break;
    case MacroInstallStatus::UNKNOWN:
        set_helix_macros_base_status(HelixMacrosStatus::Unknown);
        break;
    }
}

void PrinterPluginStatusState::set_helix_macros_restart_pending(bool pending) {
    macros_restart_pending_ = pending;
    publish_helix_macros_status();
    spdlog::info("[PrinterPluginStatusState] Helper macro restart pending: {}", pending);
}

void PrinterPluginStatusState::publish_helix_macros_status() {
    int composed = macros_base_status_;
    // RestartPending masks both staging bases: NotInstalled (fresh install)
    // and Outdated (update whose new pack is not loaded yet). Installed is
    // never masked — macros active at the current version is the one state
    // where nothing can be pending.
    if (macros_restart_pending_ &&
        (macros_base_status_ == static_cast<int>(HelixMacrosStatus::NotInstalled) ||
         macros_base_status_ == static_cast<int>(HelixMacrosStatus::Outdated))) {
        composed = static_cast<int>(HelixMacrosStatus::RestartPending);
    }
    lv_subject_set_int(&helix_macros_status_, composed);
}

} // namespace helix
