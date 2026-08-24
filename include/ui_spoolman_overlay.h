// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_spoolman_overlay.h
 * @brief Spoolman settings overlay
 *
 * This overlay allows users to configure Spoolman integration settings:
 * - Enable/disable automatic weight sync
 * - Configure polling refresh interval
 *
 * Settings are persisted in Moonraker database under "helix-screen" namespace.
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Main thread only
 */

#pragma once

#include "moonraker_config_manager.h"
#include "overlay_base.h"
#include "system/moonraker_local_probe.h"

#include <lvgl/lvgl.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward declarations
class IMoonrakerAPI;
class SpoolmanOverlayTestAccess;

namespace helix::ui {

/**
 * @class SpoolmanOverlay
 * @brief Overlay for configuring Spoolman integration settings
 *
 * This overlay provides settings for Spoolman weight synchronization:
 * - Sync toggle: Enable/disable automatic polling
 * - Refresh interval: How often to poll for weight updates (30s, 60s, 120s, 300s)
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::ui::get_spoolman_overlay();
 * if (!overlay.are_subjects_initialized()) {
 *     overlay.init_subjects();
 *     overlay.register_callbacks();
 * }
 * overlay.show(parent_screen);
 * @endcode
 */
class SpoolmanOverlay : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     */
    SpoolmanOverlay();

    /**
     * @brief Destructor
     */
    ~SpoolmanOverlay() override;

    // Non-copyable
    SpoolmanOverlay(const SpoolmanOverlay&) = delete;
    SpoolmanOverlay& operator=(const SpoolmanOverlay&) = delete;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize subjects for reactive binding
     *
     * Registers subjects for:
     * - ams_spoolman_sync_enabled: Whether sync is enabled (0/1)
     * - ams_spoolman_refresh_interval: Polling interval in seconds
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks for toggle and dropdown changes.
     */
    void register_callbacks() override;

    /**
     * @brief Create the overlay UI (called lazily)
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Spoolman"
     */
    const char* get_name() const override {
        return "Spoolman";
    }

    /**
     * @brief Null widget pointers after destroy-on-close
     */
    void on_ui_destroyed() override;

    /**
     * @brief Refresh UI state when overlay becomes visible
     *
     * Re-syncs the Barcode Scanner row description subject from SettingsManager
     * so that changes made in the scanner settings sub-overlay are reflected
     * when the user returns here.
     */
    void on_activate() override;

    /**
     * @brief Release this overlay's Spoolman poll reference when it is dismissed
     *
     * The overlay takes a reference while sync is enabled; without giving it
     * back on dismissal the manager's refcount can never reach zero and the
     * poll timer runs for the life of the process.
     */
    void on_deactivate() override;

    //
    // === Public API ===
    //

    /**
     * @brief Show the overlay
     *
     * This method:
     * 1. Ensures overlay is created (lazy init)
     * 2. Loads current settings from Moonraker database
     * 3. Updates subject values
     * 4. Pushes overlay onto navigation stack
     *
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    /**
     * @brief Refresh settings from Moonraker database
     *
     * Re-loads current values from the database and updates UI.
     */
    void refresh();

    /**
     * @brief Set IMoonrakerAPI for database access
     *
     * @param api IMoonrakerAPI instance (not owned)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

  private:
    //
    // === Internal Methods ===
    //

    /**
     * @brief Load settings from Moonraker database
     *
     * Queries helix-screen namespace for:
     * - ams_spoolman_sync_enabled
     * - ams_weight_refresh_interval
     */
    void load_from_database();

    /**
     * @brief Save sync enabled setting to database
     *
     * @param enabled Whether sync is enabled
     */
    void save_sync_enabled(bool enabled);

    /**
     * @brief Save refresh interval to database
     *
     * @param interval_seconds Interval in seconds (30, 60, 120, 300)
     */
    void save_refresh_interval(int interval_seconds);

    /**
     * @brief Convert dropdown index to interval seconds
     *
     * @param index Dropdown index (0-3)
     * @return Interval in seconds
     */
    static int dropdown_index_to_seconds(int index);

    /**
     * @brief Convert interval seconds to dropdown index
     *
     * @param seconds Interval in seconds
     * @return Dropdown index (0-3)
     */
    static int seconds_to_dropdown_index(int seconds);

    /**
     * @brief Update UI controls from current subject values
     */
    void update_ui_from_subjects();

    /**
     * @brief Take or release this overlay's single SpoolmanManager poll reference
     *
     * SpoolmanManager's polling is refcounted, so every start must be matched by
     * exactly one stop. The sync setting is applied more than once per visit —
     * load_from_database() re-runs on every show()/refresh() and its key-fallback
     * chain can call apply_sync() again — so the calls have to be idempotent.
     * Tracking ownership here means a repeated apply cannot take a second
     * reference and a repeated release cannot steal another panel's.
     *
     * Mirrors AmsPanel's holds_poll_ref_ guard.
     *
     * @param want_ref true to hold a poll reference, false to release it
     */
    void set_poll_ref(bool want_ref);

    //
    // === Static Callbacks ===
    //

    /**
     * @brief Callback for sync toggle change
     *
     * Called when user toggles the sync enable switch.
     * Saves setting to database and starts/stops polling.
     */
    static void on_sync_toggled(lv_event_t* e);

    /**
     * @brief Callback for interval dropdown change
     *
     * Called when user changes the polling interval.
     * Saves setting to database.
     */
    static void on_interval_changed(lv_event_t* e);

    //
    // === State ===
    //

    /// Alias for overlay_root_ to match existing pattern
    lv_obj_t*& overlay_ = overlay_root_;

    /// Sync toggle widget
    lv_obj_t* sync_toggle_ = nullptr;

    /// Interval dropdown widget
    lv_obj_t* interval_dropdown_ = nullptr;

    /// Subject for sync enabled state (0=disabled, 1=enabled)
    lv_subject_t sync_enabled_subject_;

    /// Subject for refresh interval in seconds
    lv_subject_t refresh_interval_subject_;

    /// IMoonrakerAPI for database access (not owned)
    IMoonrakerAPI* api_ = nullptr;

    /// True while this overlay holds one SpoolmanManager poll reference
    bool holds_poll_ref_ = false;

    /// Where Moonraker's loaded config lives, resolved from server.config before each
    /// write. Defaults to the config root, which is correct on stock Klipper installs.
    helix::ConfigPathInfo config_paths_{true, "", "moonraker.conf", ""};

    /// How the [spoolman] section gets written.
    enum class SpoolmanWriteMode {
        IncludeFile, ///< Fresh setup: write helixscreen.conf + [include] it from the primary config
        InPlace      ///< A loaded config already defines [spoolman]; update it where it lives
    };
    SpoolmanWriteMode write_mode_ = SpoolmanWriteMode::IncludeFile;

    /// Config-root-relative path of the file holding [spoolman].
    ///
    /// Intentionally empty until resolve_spoolman_target() runs. Defaulting this to
    /// "helixscreen.conf" is exactly the silent assumption that made Remove no-op
    /// against a natively-configured Moonraker while reporting success.
    std::string spoolman_config_path_;

    /// Absolute path of the file manager's writable "config" root, from
    /// server.files.roots. Empty when Moonraker did not report one, which is the
    /// pre-existing behaviour: an absolute config path is then out of reach.
    std::string config_root_abs_;

    /// The local-write fallback in force for the current setup attempt.
    ///
    /// Non-viable for every ordinary printer. Cleared at the start of each attempt
    /// so a plan from a previous one can never redirect a healthy write to disk.
    helix::diag::LocalIncludePlan local_plan_;

    /// Default values
    static constexpr bool DEFAULT_SYNC_ENABLED = true;
    static constexpr int DEFAULT_REFRESH_INTERVAL_SECONDS = 30;

#if HELIX_HAS_LABEL_PRINTER
    // Label printer sub-panel launcher
    static void on_label_printer_clicked(lv_event_t* e);
#endif

    // === Barcode Scanner Picker ===
    lv_subject_t scanner_device_status_subject_;
    char scanner_status_buf_[64] = {0};

    static void on_barcode_scanner_clicked(lv_event_t* e);
    void handle_barcode_scanner_clicked();
    void update_scanner_status_text();

    // === Server Setup Methods ===
    void probe_spoolman_server(const std::string& host, const std::string& port);

    /**
     * @brief Ask Moonraker where its config actually lives, then run configure_spoolman()
     *
     * Moonraker's file API root "config" maps to `<data_path>/config`, but the loaded
     * moonraker.conf may live elsewhere (stock Creality K2 loads it from
     * /usr/share/moonraker). Writing blind in that case produces a file Moonraker never
     * reads while the UI reports success. Resolve first, and fail loudly if the real
     * config is outside the uploadable root.
     */
    void resolve_config_location(const std::string& host, const std::string& port);

  public:
    /// Outcome of locating the config file that defines [spoolman].
    struct SpoolmanConfigTarget {
        enum class Status {
            Defined,    ///< exactly one loaded, reachable file defines [spoolman]
            Undefined,  ///< no loaded config file defines it — nothing is configured
            Ambiguous,  ///< more than one loaded file defines it; refuse to guess
            Unreachable ///< the relevant config cannot be read/written via the file API
        };
        Status status = Status::Unreachable;
        std::string path;    ///< config-root-relative path of the target file
        std::string content; ///< contents of `path` (Defined only)
        std::string detail;  ///< operator-facing explanation for Ambiguous / Unreachable

        /// Unreachable only: the file API was ASKED and answered, and the answer was
        /// that Moonraker's config is not under a root it serves.
        ///
        /// The distinction matters because Unreachable also covers "we could not
        /// tell" — a dropped WebSocket, a 500 from the file manager, a build that
        /// reports no section list. Those must not license
        /// try_local_config_fallback() to edit a vendor firmware file and restart
        /// Moonraker, because the ordinary path might well succeed on the next try.
        /// Set it only where every candidate was actually downloaded and judged, or
        /// where no addressable path exists at all.
        bool proved_out_of_reach = false;
    };

  private:
    using SpoolmanTargetCallback = std::function<void(const SpoolmanConfigTarget&)>;

    /**
     * @brief Locate the config file that actually defines [spoolman]
     *
     * The single resolution step shared by setup, the URL display, and Remove. Queries
     * server.config, picks the loaded file defining [spoolman] (counting a
     * helixscreen.conf pulled in by an earlier run's [include]), proves that file is
     * reachable through the file API, and screens for a stale duplicate.
     *
     * Every caller must go through this. Assuming helixscreen.conf without asking is
     * what made Remove silently no-op on a natively-configured printer.
     *
     * @param on_done Invoked on the MAIN thread with the resolution.
     */
    void resolve_spoolman_target(SpoolmanTargetCallback on_done);

    /**
     * @brief Resolution proper, once the file manager's config root is known
     *
     * Split out only so the root can be fetched first: Moonraker names a config
     * outside the root config's own directory by absolute path, and without the
     * root there is nothing to judge such a path against.
     *
     * @param config_root_abs Absolute path of the writable "config" root, or ""
     *                        when Moonraker did not report one — in which case
     *                        an absolute config path is treated as unreachable,
     *                        exactly as before.
     */
    void resolve_spoolman_target_with_root(const std::string& config_root_abs,
                                           SpoolmanTargetCallback on_done);

    /// Look up the writable "config" root, then run @p next on the MAIN thread.
    /// Delivers "" — never fails — when the roots are unavailable.
    void with_config_root(std::function<void(const std::string&)> next);

    /**
     * @brief Last resort when Moonraker's config is unreachable but it runs here
     *
     * Stock Creality K2 loads /usr/share/moonraker/moonraker.conf while the file
     * manager's config root is /mnt/UDISK/printer_data/config, so no Moonraker
     * call can edit the config. HelixScreen runs on that printer, so the file is
     * reachable as an ordinary local file. Falls back to fail_config_unreachable()
     * whenever it cannot prove all of that.
     */
    void try_local_config_fallback(const std::string& detail, const std::string& host,
                                   const std::string& port);

    /// Append the planned `[include]` to Moonraker's config on the local disk.
    void append_include_locally();

    /// Shared error surface: log the detail, show a status line and a red toast.
    void report_spoolman_error(const char* status_text, const char* toast_text,
                               const std::string& detail);

    /**
     * @brief Prove by content which candidate path is the config Moonraker loaded
     *
     * Most Moonraker builds never expose their config file's absolute path, and the
     * name they do report cannot be trusted as a file-API path — so the path check
     * alone can prove nothing. This walks `candidates` from
     * MoonrakerConfigManager::candidate_config_paths(), downloading each through the
     * file API's "config" root and confirming it defines the sections Moonraker
     * reported. That content proof is what makes a speculative candidate safe.
     *
     * Moves to the next candidate ONLY on a genuine 404 or a Mismatch verdict; any
     * other error aborts, because walking the whole list on a flaky link would turn
     * a transport problem into a wrong-file write. Each step logs at info level, and
     * so does the winner — a live log has to say which path was chosen and why.
     *
     * Two candidates are held to a stricter standard than a plain Drifted verdict:
     * a `speculative` one, whose path was inferred from the tail of a foreign
     * absolute path rather than derived from the config root, must match Moonraker's
     * section list EXACTLY — drift tolerance and a guessed path compound into a
     * confident write to an unrelated file of the same name. And on the in-place
     * path the candidate must actually define `[spoolman]`, since that section is
     * the whole reason this file was selected.
     *
     * @param reported_name The name Moonraker gave, for messages and logs.
     * @param candidates    Ranked file-API paths, most trustworthy first.
     * @param index         Which candidate to try; callers start at 0.
     * @param speculative   From MoonrakerConfigManager::candidates_are_speculative().
     * @param last_detail   Why the previous candidate was rejected, carried forward so
     *                      the final "nothing matched" message can say which it was and
     *                      whether it was absent or merely the wrong file.
     */
    void verify_config_reachable(const std::string& reported_name,
                                 const std::vector<std::string>& candidates, size_t index,
                                 const std::vector<std::string>& required_sections, bool in_place,
                                 bool speculative, SpoolmanTargetCallback on_done,
                                 const std::string& last_detail = "");

    /**
     * @brief Guard against a stale, not-yet-loaded helixscreen.conf before writing in place
     *
     * files[] only reveals sections in files Moonraker actually loaded. If an earlier run
     * wrote [spoolman] into helixscreen.conf but its restart never took effect, that file
     * is invisible to files[] — and writing [spoolman] into the native config would
     * create the duplicate once the include finally loads. Only runs when the in-place
     * target is some file other than helixscreen.conf itself.
     */
    void check_stale_helix_conf(const std::string& target_path, const std::string& content,
                                SpoolmanTargetCallback on_done);

    /// Upsert [spoolman] directly into the file that already defines it, then restart.
    void write_spoolman_in_place(const std::string& content, const std::string& host,
                                 const std::string& port);

    /// Abort setup with a visible, actionable "config not writable" error.
    void fail_config_unreachable(const std::string& detail);

    /// Abort setup because more than one loaded config file defines [spoolman].
    void fail_config_ambiguous(const std::string& detail);

    void configure_spoolman(const std::string& host, const std::string& port);
    void finish_configure(const std::string& helix_content,
                          const std::vector<std::pair<std::string, std::string>>& entries);
    void ensure_moonraker_include();
    void restart_and_verify();
    void verify_spoolman_connected();
    void remove_spoolman_config();
    void update_server_url_display();
    void set_setup_status(const char* text, bool is_error = false);

    /**
     * @brief Toggle the Connect button's busy state
     *
     * When @p connecting is true, the button is disabled and its label swapped
     * to "Connecting...". When false, the original "Connect" label is restored
     * and the button is re-enabled. Called at the start of the probe flow and
     * from every terminal path (success, error, cancel).
     */
    void set_connecting(bool connecting);

    // === Server Setup Callbacks ===
    static void on_connect_clicked(lv_event_t* e);
    static void on_cancel_setup_clicked(lv_event_t* e);
    static void on_change_clicked(lv_event_t* e);
    static void on_remove_clicked(lv_event_t* e);

    // === Server Setup Widgets ===
    lv_obj_t* host_input_ = nullptr;
    lv_obj_t* port_input_ = nullptr;
    lv_obj_t* setup_status_text_ = nullptr;
    lv_obj_t* server_url_text_ = nullptr;
    lv_obj_t* connect_btn_ = nullptr;
    lv_obj_t* setup_card_ = nullptr;
    lv_obj_t* status_card_ = nullptr;

    friend class ::SpoolmanOverlayTestAccess;
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton SpoolmanOverlay
 */
SpoolmanOverlay& get_spoolman_overlay();

} // namespace helix::ui
