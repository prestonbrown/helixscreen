// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <string>

namespace helix {

/**
 * @brief The oldest Moonraker whose History API can undo a job-file rewrite.
 *
 * Below this, History.save_job() and a list-shaped auxiliary_data are missing,
 * so the HelixPrint plugin cannot put the original filename back on a finished
 * job. Everything else the plugin does works from 0.8.x up. This is NOT an
 * app-wide minimum: it gates one consequence of one feature, and it is stated
 * where that feature is offered rather than at startup.
 */
inline constexpr const char* kMinMoonrakerVersion = "0.9.0";

/**
 * @brief True only for a version we could both read AND place below the floor.
 *
 * server.info omits moonraker_version on some builds and discovery substitutes
 * the literal "unknown", which check_version_constraint() cannot parse and
 * reports false for. Reading that as "too old" would degrade every printer that
 * never announced a version, so anything unparseable answers false here.
 */
[[nodiscard]] bool moonraker_history_is_degraded(const std::string& version);

/**
 * @brief True for a version string that names no version.
 *
 * Empty, "?" (what Moonraker's server.info reports when `git describe` fails,
 * as on a tagless fork checkout) and "unknown" (what discovery substitutes for
 * a missing field), all ignoring case and surrounding whitespace.
 */
[[nodiscard]] bool is_placeholder_version(const std::string& version);

/**
 * @brief The Moonraker version update_manager knows, from a machine.update.status result.
 *
 * The fallback for a server.info that reports a placeholder. Takes
 * `version_info.moonraker.version` when it names a release; a checkout with no
 * release tags reads "v0.0.0-N" there, so it answers the short commit hash
 * (`current_hash`, 7 characters) instead, the form Kalico reports itself in.
 * "" when neither is present.
 */
[[nodiscard]] std::string moonraker_version_from_update_status(const nlohmann::json& result);

/**
 * @brief Manages software version subjects for UI display
 *
 * Tracks Klipper and Moonraker version strings for display in the Settings
 * panel About section. Both subjects are string subjects with 64-byte buffers.
 *
 * Extracted from PrinterState as part of god class decomposition.
 *
 * Default value for both subjects is "—" (em dash, U+2014) to indicate
 * version information has not yet been received from Moonraker.
 */
class PrinterVersionsState {
  public:
    PrinterVersionsState() = default;
    ~PrinterVersionsState() = default;

    // Non-copyable
    PrinterVersionsState(const PrinterVersionsState&) = delete;
    PrinterVersionsState& operator=(const PrinterVersionsState&) = delete;

    /**
     * @brief Initialize version subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    // ========================================================================
    // Setters (synchronous, must be called from UI thread)
    // ========================================================================

    /**
     * @brief Set Klipper version string (synchronous, must be on UI thread)
     *
     * This is a synchronous setter intended to be called from within
     * helix::ui::queue_update() by PrinterState, which handles the async
     * dispatch.
     *
     * @param version Version string (e.g., "v0.12.0-108-g2c7a9d58")
     */
    void set_klipper_version_internal(const std::string& version);

    /**
     * @brief Set Moonraker version string (synchronous, must be on UI thread)
     *
     * This is a synchronous setter intended to be called from within
     * helix::ui::queue_update() by PrinterState, which handles the async
     * dispatch.
     *
     * @param version Version string (e.g., "v0.8.0-143-g2c7a9d58")
     */
    void set_moonraker_version_internal(const std::string& version);

    /**
     * @brief Set OS version string (synchronous, must be on UI thread)
     *
     * @param version OS distribution name (e.g., "Forge-X 1.4.0")
     */
    void set_os_version_internal(const std::string& version);

    /**
     * @brief Set the firmware's name ("Klipper", "Kalico"; must be on UI thread)
     *
     * Labels the row that shows klipper_version, so a fork reads as itself.
     */
    void set_firmware_name_internal(const std::string& name);

    // ========================================================================
    // Subject accessors
    // ========================================================================

    /**
     * @brief Get Klipper version subject for XML binding
     * @return Pointer to string subject
     */
    lv_subject_t* get_klipper_version_subject() {
        return &klipper_version_;
    }

    /**
     * @brief Raw klipper version exactly as the host reported it
     *
     * The subject carries the DISPLAY form, where placeholders collapse to a
     * translated "Unknown" label. Data consumers (debug bundle) need the raw
     * string: "?" vs empty vs a real version distinguishes a placeholder
     * fork from a missing field from a real report.
     */
    const std::string& get_klipper_version_raw() const {
        return klipper_version_raw_;
    }

    /// Clear plain (non-subject) data members. Called by
    /// PrinterStateTestAccess::clear_data so one test's raw version string
    /// does not leak into the next on the shared singleton.
    void clear_data() {
        klipper_version_raw_.clear();
    }

    /// 1 when this Moonraker is below kMinMoonrakerVersion, 0 otherwise.
    /// Derived in set_moonraker_version_internal() from the RAW string, before
    /// the display form drops what a comparison needs.
    lv_subject_t* get_moonraker_history_degraded_subject() {
        return &moonraker_history_degraded_;
    }

    lv_subject_t* get_moonraker_version_subject() {
        return &moonraker_version_;
    }

    /**
     * @brief Get OS version subject for XML binding
     * @return Pointer to string subject
     */
    lv_subject_t* get_os_version_subject() {
        return &os_version_;
    }

  private:
    friend class PrinterVersionsStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Version subjects (string, 64-byte buffer each)
    lv_subject_t klipper_version_{};
    lv_subject_t moonraker_version_{};
    lv_subject_t os_version_{};
    lv_subject_t moonraker_history_degraded_{};
    lv_subject_t klipper_firmware_name_{};

    // String buffers for subject storage
    char klipper_version_buf_[64]{};
    char moonraker_version_buf_[64]{};
    char os_version_buf_[64]{};
    char klipper_firmware_name_buf_[32]{};

    // Raw form of the klipper version, as reported (see get_klipper_version_raw)
    std::string klipper_version_raw_;
};

} // namespace helix
