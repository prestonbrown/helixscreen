// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_accounting.h"
#include "fit_check.h"
#include "helix_plugin_features.h"
#include "pa_calibration.h"
#include "plr_backend.h"
#include "print_photos.h"
#include "standing_print_options.h"
#include "storage_report.h"
#include "subject_managed_panel.h"

#include <array>
#include <functional>
#include <lvgl.h>
#include <optional>

namespace helix {

/// A plugin feature with a switch in the settings. Each gets two XML subjects:
/// `mode_subject` (PluginSwitchMode, which a plugin_feature_toggle_row binds
/// through its `feature` name) and, unless the feature owns its switch,
/// `on_subject` (the switch's state).
struct PluginSwitchFeature {
    const char* name;
    int min_version;
    /// The app does the feature itself when the plugin cannot
    bool has_fallback;
    /// Acts on a print or a spool, so it is off until opted into
    bool acts;
    const char* mode_subject;
    /// nullptr for a feature that owns its switch and publishes its own state
    const char* on_subject;
};

inline constexpr std::array<PluginSwitchFeature, 7> kPluginSwitchFeatures = {{
    {kPlrFeatureName, kPlrFeatureMinVersion, false, true, "plugin_feature_mode_plr", nullptr},
    {kPrintHooksFeature, kPrintHooksMinVersion, true, true, "plugin_feature_mode_print_hooks",
     "plugin_feature_on_print_hooks"},
    {storage::kAutoCleanupFeature, storage::kAutoCleanupMinVersion, false, true,
     "plugin_feature_mode_storage_auto_cleanup", "plugin_feature_on_storage_auto_cleanup"},
    {kFilamentAccountingFeature, kFilamentAccountingMinVersion, true, true,
     "plugin_feature_mode_filament_accounting", "plugin_feature_on_filament_accounting"},
    {kPhotosFeature, kPhotosMinVersion, true, false, "plugin_feature_mode_photos",
     "plugin_feature_on_photos"},
    {kFitCheckFeature, kFitCheckMinVersion, false, true,
     "plugin_feature_mode_fit_check_blocks_autostart",
     "plugin_feature_on_fit_check_blocks_autostart"},
    {pacal::kPaGaugeFeature, pacal::kPaGaugeMinVersion, false, true, "plugin_feature_mode_pa_gauge",
     "plugin_feature_on_pa_gauge"},
}};

/// helix_plugin_installed values once the probe has answered (-1 before). A
/// plugin set to `enabled: False` serves nothing, so it reads as absent.
constexpr int kHelixPluginAbsent = 0;
constexpr int kHelixPluginInstalled = 1;

enum class MacroInstallStatus;

/**
 * @brief UI-facing status of the HelixScreen helper macro pack
 *
 * Extends the printer-side detection states (MacroInstallStatus) with
 * RESTART_PENDING — the post-staging truth: the files are on the printer
 * but the macros only load at the next Klipper restart. Numeric values are
 * the helix_macros_status subject values the XML rows bind against.
 */
enum class HelixMacrosStatus {
    Unknown = -1,       ///< No objects list from the printer yet
    NotInstalled = 0,   ///< Discovered; the printer has no Helix macros
    Installed = 1,      ///< Current pack version active
    Outdated = 2,       ///< Older pack active; the update offer applies
    RestartPending = 3, ///< Files staged; activate at the next Klipper restart
};

/**
 * @brief Manages HelixPrint plugin status subjects for UI feature gating
 *
 * Tracks whether the HelixPrint Klipper plugin is installed and the install
 * status of the HelixScreen helper macro pack. The plugin subjects use
 * tri-state semantics: -1=unknown, 0=not installed, 1=installed.
 *
 * The macro status subject composes a discovery-derived base value with a
 * restart-pending flag set by the install flow (see HelixMacrosStatus).
 *
 * The unknown (-1) state allows the UI to distinguish between:
 * - "Still checking" (show loading/spinner)
 * - "Definitely not available" (show install prompt)
 * - "Available" (show feature controls)
 *
 * @note set_helix_plugin_installed triggers composite visibility updates
 *       in PrinterState (the has_any_preprint_options aggregate subject).
 */
class PrinterPluginStatusState {
  public:
    PrinterPluginStatusState() = default;
    ~PrinterPluginStatusState() = default;

    // Non-copyable
    PrinterPluginStatusState(const PrinterPluginStatusState&) = delete;
    PrinterPluginStatusState& operator=(const PrinterPluginStatusState&) = delete;

    /**
     * @brief Initialize plugin status subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    // ========================================================================
    // Setters
    // ========================================================================

    /**
     * @brief Set helix plugin installed status
     *
     * Called from within helix::ui::queue_update() by PrinterState, which
     * handles the async dispatch and the subsequent visibility update.
     *
     * @param installed True if HelixPrint plugin is installed
     */
    void set_installed(bool installed);

    /// The plugin's link directory (its status' `symlink_dir`); empty when it
    /// reported none. Also tells the filename helpers to recognise it.
    void set_link_dir(const std::string& dir);

    /// The plugin's link directory, or empty when unknown.
    const std::string& link_dir() const {
        return link_dir_;
    }

    /// The version the running plugin reports; empty when it did not answer.
    void set_version(const std::string& version) {
        version_ = version;
    }
    const std::string& version() const {
        return version_;
    }

    /// The plugin answered but is set to `enabled: False`: configured, and
    /// reading as not installed until an install turns it on.
    void set_disabled(bool disabled) {
        disabled_ = disabled;
    }
    bool disabled() const {
        return disabled_;
    }

    /// The features map the plugin's status reported; empty when it reported
    /// none. With FeatureAnnounce::IfChanged, the same map touches nothing.
    void set_features(PluginFeatureMap features,
                      FeatureAnnounce announce = FeatureAnnounce::Always) {
        if (announce == FeatureAnnounce::IfChanged && features == features_) {
            return;
        }
        features_ = std::move(features);
        publish_features();
        if (subjects_initialized_) {
            lv_subject_set_int(&helix_plugin_probe_seq_,
                               lv_subject_get_int(&helix_plugin_probe_seq_) + 1);
        }
    }
    const PluginFeatureMap& features() const {
        return features_;
    }

    /// Forget what the plugin's status reported, touching no subject. For
    /// PrinterStateTestAccess::clear_data, so one test's probe does not decide
    /// the next one's plugin features on the shared singleton.
    void clear_data() {
        set_link_dir("");
        version_.clear();
        disabled_ = false;
        features_.clear();
        stored_opt_ins_ = nlohmann::json::object();
    }

    /// Whether plugin feature @p name, at contract @p min_version or newer, is
    /// usable now. Gate plugin-backed behaviour on this, never on version().
    PluginFeatureState plugin_feature(const std::string& name, int min_version) const {
        return plugin_feature_state(service_has_helix_plugin(), features_, name, min_version);
    }

    /// Whether the plugin reports feature @p name at @p min_version or newer,
    /// even one it cannot run here.
    bool plugin_feature_reported(const std::string& name, int min_version) const {
        return helix::plugin_feature_reported(service_has_helix_plugin(), features_, name,
                                              min_version);
    }

    /**
     * @brief Set the discovery-derived helper-macro install status
     *
     * Called from PrinterState::set_hardware() on the main thread once a
     * discovery snapshot has been folded in. Only an Installed base clears
     * the restart-pending flag: discovery reporting the CURRENT pack active
     * means a restart landed. An Outdated base keeps it — until the restart,
     * discovery still reports the old rung.
     *
     * @param base Status derived via MacroManager::evaluate_status()
     */
    void set_helix_macros_base_status(HelixMacrosStatus base);

    /// The base status for a MacroManager::evaluate_status() verdict. An
    /// Installed verdict clears a restart-pending flag held for a staged
    /// install or update; Outdated keeps it, since the staged pack is not
    /// loaded until the restart.
    void set_helix_macros_base_status(MacroInstallStatus install);

    /**
     * @brief Mark helper-macro files as staged and awaiting a Klipper restart
     *
     * Set by the install flow after install_files()/update_files() succeeded
     * while a print made an immediate restart unsafe; cleared when discovery
     * reports the macros active at the current version. Main thread only
     * (fired from deferred callbacks).
     *
     * @param pending True while the staged files still await a restart
     */
    void set_helix_macros_restart_pending(bool pending);

    // ========================================================================
    // Subject accessors
    // ========================================================================

    /// -1 unknown, else a kHelixPlugin* value (0 absent, 1 installed and enabled)
    lv_subject_t* get_helix_plugin_installed_subject() {
        return &helix_plugin_installed_;
    }

    /// HelixMacrosStatus value the XML rows bind against
    lv_subject_t* get_helix_macros_status_subject() {
        return &helix_macros_status_;
    }

    /// PluginFeatureState of the power-loss recovery feature (`plr`), as an
    /// int: 0 Absent, 1 Disabled, 2 Ready. The settings row binds it.
    lv_subject_t* get_plr_feature_state_subject() {
        return &plr_feature_state_;
    }

    /// Whether printer.cfg carries the plugin's mesh include, as the
    /// server.helix.plr.mesh_include status answered: -1 never asked (or the
    /// ask failed), 0 absent, 1 present. Reset to -1 whenever the plr feature
    /// reads Absent, so a stale answer cannot outlive the feature.
    lv_subject_t* get_plr_mesh_include_subject() {
        return &plr_mesh_include_;
    }

    void set_plr_mesh_include(std::optional<bool> present);

    /// Where screen_local() reads whether this screen runs on the printer's
    /// own host. PrinterState points it at its connection's locality verdict,
    /// the one place that fact is kept.
    void set_locality_source(std::function<bool()> is_local) {
        is_local_ = std::move(is_local);
    }

    /// The locality verdict changed: republish what depends on it. Main thread.
    void on_locality_changed() {
        publish_features();
    }

    /// This screen runs on the printer's own host (false while not known).
    /// Decides whether a fallback switch carries a hint.
    bool screen_local() const {
        return is_local_ && is_local_();
    }

    /// The helix-screen/plugin_features record as last read: the opt-ins a
    /// fallback switch shows while no plugin reports the feature. Main thread.
    void set_stored_opt_ins(nlohmann::json record) {
        stored_opt_ins_ = std::move(record);
        publish_features();
    }
    const nlohmann::json& stored_opt_ins() const {
        return stored_opt_ins_;
    }

    /// PluginSwitchMode of kPluginSwitchFeatures[@p index], as an int.
    lv_subject_t* get_switch_mode_subject(size_t index) {
        return &switch_mode_[index];
    }

    /// The switch state of kPluginSwitchFeatures[@p index]: its `enabled`.
    lv_subject_t* get_switch_on_subject(size_t index) {
        return &switch_on_[index];
    }

    /// Counts the plugin's status answers applied this session (0 before the
    /// first). Each discovery pass's probe answers after that pass has resolved
    /// the printer type, so a change here says the type is this connection's.
    lv_subject_t* get_helix_plugin_probe_seq_subject() {
        return &helix_plugin_probe_seq_;
    }

    /// 1 while the plugin reports power-loss saving switched on: the plr entry's
    /// `saving`, else its `enabled` from a plugin that reports no `saving`.
    lv_subject_t* get_plr_saving_enabled_subject() {
        return &plr_saving_enabled_;
    }

    // ========================================================================
    // Query methods
    // ========================================================================

    /**
     * @brief Plugin presence as published, without collapsing the unknown
     *
     * @return 1 installed, 0 absent, -1 not probed yet. Callers that must tell
     *         "not probed" apart from "absent" want this one.
     */
    int helix_plugin_state() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&helix_plugin_installed_));
    }

    /**
     * @brief Check if HelixPrint plugin is installed
     *
     * @return true only when value is 1 (installed), false for -1 (unknown) or 0 (not installed)
     */
    bool service_has_helix_plugin() const {
        return helix_plugin_state() == 1;
    }

  private:
    SubjectManager subjects_;
    bool subjects_initialized_ = false;
    std::string link_dir_;
    std::string version_;
    bool disabled_ = false;
    PluginFeatureMap features_;

    // Plugin status subjects (tri-state: -1=unknown, 0=no, 1=yes)
    lv_subject_t helix_plugin_installed_{}; // HelixPrint Klipper plugin

    lv_subject_t plr_feature_state_{};
    lv_subject_t helix_plugin_probe_seq_{};
    lv_subject_t plr_saving_enabled_{};
    lv_subject_t plr_mesh_include_{};
    std::function<bool()> is_local_;
    nlohmann::json stored_opt_ins_ = nlohmann::json::object();
    std::array<lv_subject_t, kPluginSwitchFeatures.size()> switch_mode_{};
    std::array<lv_subject_t, kPluginSwitchFeatures.size()> switch_on_{};
    /// Republish the feature subjects from the plugin state and features map.
    void publish_features();

    /// Composed HelixMacrosStatus; see publish_helix_macros_status()
    lv_subject_t helix_macros_status_{};
    int macros_base_status_ = static_cast<int>(HelixMacrosStatus::Unknown);
    bool macros_restart_pending_ = false;

    /// RestartPending only masks NotInstalled: once discovery reports the
    /// macros active, the pending flag is stale by definition.
    void publish_helix_macros_status();
};

} // namespace helix
