// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file update_checker.h
 * @brief Async update checker for HelixScreen
 *
 * Checks the R2 CDN manifest for newer versions of HelixScreen, falling back
 * to the GitHub releases API when the manifest is unreachable.
 * Uses background thread to avoid blocking the UI during network operations.
 *
 * SAFETY: Downloads and installs require explicit user confirmation and are
 * blocked while a print is in progress. All errors are handled gracefully
 * to ensure the printer is never affected.
 */

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief How the selected channel's served version relates to what is installed
 */
enum class ChannelVersionRelation {
    Unknown = 0, ///< Either version failed to parse — treat as "do nothing"
    Same,        ///< Channel serves exactly the installed version
    Newer,       ///< Channel is ahead: an ordinary update
    Older        ///< Channel is behind: taking it moves this install backward
};

/**
 * @brief Compare the installed version against what a channel serves
 *
 * Split out of the check worker so the three-way outcome is unit-testable. The
 * Older case exists because channels are user-selectable: someone who ran the
 * devel track and switched back to stable is AHEAD of the channel they now
 * want, and a strict "offer only if newer" rule strands them there forever —
 * the check reports "Already up to date" and there is no way back short of a
 * manual reinstall.
 *
 * @param installed Currently running version (HELIX_VERSION)
 * @param channel_version Version the selected channel's manifest serves
 * @return Relation, or Unknown if either string fails to parse
 */
ChannelVersionRelation compare_channel_version(const std::string& installed,
                                               const std::string& channel_version);

/**
 * @brief Async update checker for HelixScreen
 *
 * Checks the R2 CDN manifest (GitHub releases API as fallback) to determine if
 * a newer version is available. Rate-limited to one check per MIN_CHECK_INTERVAL.
 *
 * Usage:
 * @code
 * auto& checker = UpdateChecker::instance();
 * checker.init();
 * checker.check_for_updates([](UpdateChecker::Status status,
 *                              std::optional<UpdateChecker::ReleaseInfo> info) {
 *     if (status == UpdateChecker::Status::UpdateAvailable && info) {
 *         spdlog::info("Update available: {}", info->version);
 *     }
 * });
 * // ... on shutdown:
 * checker.shutdown();
 * @endcode
 */
class UpdateChecker {
  public:
    static constexpr const char* DEFAULT_R2_BASE_URL = "https://releases.helixscreen.org";

    /**
     * @brief Release information from GitHub
     */
    struct ReleaseInfo {
        std::string version;       ///< Stripped version (e.g., "1.2.3")
        std::string tag_name;      ///< Original tag (e.g., "v1.2.3")
        std::string download_url;  ///< Asset download URL for binary
        std::string release_notes; ///< Body markdown
        std::string published_at;  ///< ISO 8601 timestamp
        std::string sha256;        ///< SHA-256 hash (for dev channel verification)
        size_t download_bytes = 0; ///< Asset size in bytes (0 if unknown)
        /// True when this release is OLDER than what is installed — the selected
        /// channel is behind, typically right after switching from a faster
        /// channel back to a slower one. Offered explicitly and confirmed, never
        /// auto-notified.
        bool is_downgrade = false;
    };

    /**
     * @brief Update check status
     */
    enum class Status {
        Idle = 0,            ///< No check in progress
        Checking = 1,        ///< HTTP request pending
        UpdateAvailable = 2, ///< New version found
        UpToDate = 3,        ///< Already on latest
        Error = 4            ///< Check failed
    };

    /**
     * @brief Update channel selection
     */
    enum class UpdateChannel { Stable = 0, Beta = 1, Dev = 2 };

    /**
     * @brief Download and install status
     */
    enum class DownloadStatus {
        Idle = 0,        ///< No download in progress
        Confirming = 1,  ///< User confirming download
        Downloading = 2, ///< Download in progress
        Verifying = 3,   ///< Verifying tarball integrity
        Installing = 4,  ///< Running install.sh
        Complete = 5,    ///< Install succeeded
        Error = 6,       ///< Download/install failed
        Restarting = 7   ///< Static "restarting" screen before _exit(0)
    };

    /**
     * @brief Get singleton instance
     */
    static UpdateChecker& instance();

    /**
     * @brief Callback invoked when check completes
     * @param status Final status of the check
     * @param info Release info if update is available, nullopt otherwise
     *
     * Callback is invoked on the LVGL thread (via ui_queue_update).
     */
    using Callback = std::function<void(Status, std::optional<ReleaseInfo>)>;

    /**
     * @brief Check for updates asynchronously
     *
     * Spawns background thread to fetch the channel manifest.
     * Callback is invoked on LVGL thread when check completes.
     *
     * Rate limited: If called within MIN_CHECK_INTERVAL of last check,
     * returns cached result immediately instead of making a new request.
     *
     * @param callback Optional callback for result notification
     */
    void check_for_updates(Callback callback = nullptr);

    /**
     * @brief Get current status (thread-safe)
     * @return Current status enum value
     */
    Status get_status() const;

    /**
     * @brief Get cached update info if available (thread-safe)
     * @return ReleaseInfo if update is cached, nullopt otherwise
     */
    std::optional<ReleaseInfo> get_cached_update() const;

    /**
     * @brief Check if an update is available (thread-safe)
     * @return true if update is available and cached
     */
    bool has_update_available() const;

    /**
     * @brief Get error message from last failed check (thread-safe)
     * @return Error message, or empty string if no error
     */
    std::string get_error_message() const;

    /**
     * @brief Clear cached update information
     *
     * Resets status to Idle and clears cached release info.
     */
    void clear_cache();

    /**
     * @brief Re-evaluate after the user selects a different update channel
     *
     * Drops the cached result (it describes the channel they just left),
     * re-reads the channel config for off-thread readers, clears the rate-limit
     * clock, and starts a fresh check. Main thread only.
     *
     * The rate-limit reset is the load-bearing part: the limiter predates
     * user-switchable channels, so without it the check returns the PREVIOUS
     * channel's verdict and the About row keeps advertising a version the newly
     * selected channel does not serve.
     */
    void on_channel_changed();

    /**
     * @brief Initialize the update checker
     *
     * Call once at startup. Idempotent - safe to call multiple times.
     */
    void init();

    /**
     * @brief Shutdown and cleanup
     *
     * Cancels any pending check and joins worker thread.
     * Idempotent - safe to call multiple times.
     */
    void shutdown();

    // LVGL subjects for UI binding (update check)
    lv_subject_t* status_subject();
    lv_subject_t* version_text_subject();
    lv_subject_t* new_version_subject();

    // Download and install
    void start_download();
    void cancel_download();
    DownloadStatus get_download_status() const;
    int get_download_progress() const;
    std::string get_download_error() const;

    // LVGL subjects for download UI
    lv_subject_t* download_status_subject();
    lv_subject_t* download_progress_subject();
    lv_subject_t* download_text_subject();

    // Download state reporting (public for tests and SettingsPanel)
    void report_download_status(DownloadStatus status, int progress, const std::string& text,
                                const std::string& error = "");
    /// Diagnostic info about candidate selection — populated even when no
    /// candidate met the free-space threshold, so callers can build a
    /// useful error message.
    struct DownloadPathDiag {
        std::string best_dir;         // best candidate found (regardless of threshold)
        uint64_t best_free_bytes = 0; // its free space (uint64_t — on 32-bit
                                      // platforms a 46 GiB rootfs wraps size_t)
        uint64_t threshold_bytes = 0; // threshold required
    };
    /// @param diag Optional diagnostic out-param (for error reporting)
    /// @param threshold_bytes Required free bytes; 0 → use a 120 MB default
    ///        for callers without a known download size.
    std::string get_download_path(DownloadPathDiag* diag = nullptr,
                                  uint64_t threshold_bytes = 0) const;

    /// Compute the disk-space threshold for an in-app download.
    /// Returns 1.2x download_bytes + a small buffer when known, else a fixed
    /// default sized for current release archives. Always honors a 50 MB
    /// safety floor.
    static uint64_t required_download_space_bytes(uint64_t download_bytes);

    /**
     * @brief Derive the installer staging dir handed to install.sh via TMP_DIR.
     * @param tarball_path Path to the downloaded update tarball.
     * @param install_root Resolved install root (app_get_install_root()), or ""
     *        if unknown.
     * @return A dot-prefixed staging dir guaranteed OUTSIDE install_root.
     *
     * Base is dirname(tarball_path). If that base is within-or-equal-to
     * install_root, the base is relocated to install_root's PARENT (a sibling
     * of the install dir). This is load-bearing: TMP_DIR is `rm -rf`'d on
     * cleanup AND the installer's --update flow (release.sh) does dotfile
     * `rm -rf` inside INSTALL_DIR and `mv INSTALL_DIR ...` during the atomic
     * swap — a staging dir under INSTALL_DIR would be deleted/relocated out
     * from under the extracted tree. Pure + static so the safety invariant is
     * unit-testable without running the installer.
     */
    static std::string compute_update_staging_dir(const std::string& tarball_path,
                                                  const std::string& install_root);

    std::string get_platform_asset_name() const;

    /// Single source of truth for the release asset name Moonraker's update
    /// manager resolves via release_info.json's asset_name. Static so
    /// repair_release_info() compares against the same expression rather than a
    /// copy of it — #993 was caused by exactly that name drifting between
    /// duplicates.
    static std::string platform_asset_name();

    /**
     * @brief Outcome of a release_info.json validate-and-repair pass.
     */
    enum class ReleaseInfoRepair {
        NotNeeded, ///< asset_name already matches the platform asset — nothing written
        Repaired,  ///< file rewritten with the correct asset_name
        Absent,    ///< no release_info.json to repair (and no deployed layout to create one in)
        Failed     ///< a repair was needed but could not be written (read-only / permissions)
    };

    /**
     * @brief Validate and, if needed, repair the installed release_info.json.
     *
     * Moonraker's `type: web` updater downloads the release asset named by this
     * file's `asset_name`. When that name is missing or matches no asset on the
     * release, Moonraker silently falls back to the alphabetically-FIRST asset —
     * never a zip — and extraction dies with "File is not a zip file"
     * (prestonbrown/helixscreen#993). The file is written once at install time
     * and was never revalidated, so a stale or wrong value permanently blocked
     * the very update that would have fixed it; SSH was the only escape.
     *
     * This runs at startup and rewrites the file when `asset_name` is absent,
     * empty, non-string, or != get_platform_asset_name(), preserving the other
     * fields. It never writes when the value is already correct, and a failure
     * is never fatal — a bad release_info.json must not stop the app booting.
     *
     * A MISSING file is only created when @p install_root looks like a deployed
     * install (`bin/helix-screen` present). A dev tree resolves its install root
     * to the source checkout, and creating the file there would litter the repo.
     *
     * Static and install-root-parameterised so it is unit-testable against a
     * temp directory.
     *
     * @param install_root Resolved install root (app_get_install_root()), or ""
     *        if unknown (bind-mounted layout) — then nothing is done.
     */
    static ReleaseInfoRepair repair_release_info(const std::string& install_root);

    /**
     * @brief Get the configured update channel.
     * @warning MAIN THREAD ONLY — reads Config, which is not thread-safe
     *          (include/config.h). Off-thread callers want config_snapshot().
     */
    UpdateChannel get_channel() const;

    /**
     * @brief Human-readable name for a channel ("stable", "beta", "dev").
     *
     * Single source of truth for the enum→string map so log lines and the debug
     * bundle's update section cannot drift from each other.
     */
    static const char* channel_name(UpdateChannel channel);

    /**
     * @brief Resolve the R2 base URL the next check will actually use.
     *
     * Reads /update/r2_url, falls back to DEFAULT_R2_BASE_URL when unset, and
     * strips trailing slashes. Single source of truth for that resolution:
     * check_for_updates() caches its result into cached_r2_base_url_, and
     * refresh_config_snapshot() stores it for off-thread readers.
     *
     * @warning MAIN THREAD ONLY — reads Config, which is not thread-safe
     *          (include/config.h). Off-thread callers want config_snapshot().
     */
    static std::string effective_r2_base_url();

    /**
     * @brief Main-thread snapshot of the Config-derived update settings.
     *
     * Exists so diagnostics that run on a worker thread (the debug bundle is
     * collected on HttpExecutor::slow()) can report the channel and effective
     * manifest URL without reading Config off the main thread.
     */
    struct ConfigSnapshot {
        std::string channel;     ///< "stable" | "beta" | "dev"
        std::string r2_base_url; ///< effective, normalized manifest base URL
    };

    /**
     * @brief Re-read the Config-derived settings into the diagnostics snapshot.
     * @warning MAIN THREAD ONLY (reads Config).
     *
     * Called from init() so the snapshot is valid before any check has run, and
     * again whenever the user changes the channel, so a switch is visible
     * without waiting for a check.
     */
    void refresh_config_snapshot();

    /** @brief Thread-safe copy of the diagnostics snapshot. Safe from any thread. */
    ConfigSnapshot config_snapshot() const;

    /** @brief Get platform key for current build ("pi", "ad5m", "k1") */
    static std::string get_platform_key();

    /**
     * @brief Map a platform key to its human-readable display name.
     * @param key  Platform key as returned by get_platform_key().
     * @return Display name (e.g. "Raspberry Pi", "Creality K1").
     *         Falls back to @p key itself for unrecognised values.
     *
     * Canonical single source of truth for the key→name map; call sites in
     * debug_bundle_collector.cpp and elsewhere must use this instead of
     * maintaining their own copies.
     */
    static std::string get_platform_display_name(const std::string& key);

    /**
     * @brief Find a local install.sh by searching well-known paths
     * @param extra_search_paths Additional paths to search (prepended to default list)
     * @return Path to install.sh if found, empty string otherwise
     *
     * Searches exe-relative path first, then well-known install locations.
     * Used as fallback when installer cannot be extracted from update tarball.
     */
    static std::string
    find_local_installer(const std::vector<std::string>& extra_search_paths = {});

    /**
     * @brief Extract install.sh from a release tarball into a directory
     * @param tarball_path Path to the .tar.gz release tarball
     * @param extract_dir  Directory to extract into (helixscreen/ subdir created inside it)
     * @return Path to the extracted installer (chmod +x applied), or empty string on failure
     *
     * Tries GNU tar xzf first; falls back to cp+gunzip+tar for BusyBox compatibility.
     * The fallback avoids gunzip -k (keep-original) which is absent on older BusyBox builds.
     * Exposed as public static for unit testing.
     */
    static std::string extract_installer_from_tarball(const std::string& tarball_path,
                                                      const std::string& extract_dir);

    /** @brief Check if a version is dismissed (user chose to ignore) */
    bool is_version_dismissed(const std::string& version) const;

    /** @brief Dismiss the current cached update version (persists to config) */
    void dismiss_current_version();

    /**
     * @brief Handle notification that Moonraker has finished updating HelixScreen
     *
     * Called when we receive a notify_update_response indicating our own update
     * is complete. Writes the self-restart sentinel (so helixscreen-update.service
     * skips its restart on systemd) and triggers _exit(0) to let the
     * watchdog/systemd restart with the new binary.
     *
     * Works on all platforms (systemd, SysV, unsupervised) — the universal
     * replacement for the systemd-only helixscreen-update.path watcher.
     */
    static void handle_external_update_complete();

    /** @brief Start automatic update checking (15s initial, then 24h periodic) */
    void start_auto_check();

    /** @brief Stop automatic update checking */
    void stop_auto_check();

    // LVGL subjects for notification modal
    lv_subject_t* release_notes_subject();
    lv_subject_t* changelog_visible_subject();

    /** @brief Show the update notification modal */
    void show_update_notification();

    /** @brief Hide the update notification modal */
    void hide_update_notification();

    /** @brief Result of a portable zip integrity probe. */
    enum class ZipIntegrity {
        Ok,           ///< Archive verified intact.
        Corrupt,      ///< Archive is damaged or not a zip at all.
        Unverifiable, ///< No tool on this system can test it — caller falls back to SHA256.
    };

    /**
     * @brief Test a .zip archive's integrity with whatever tool can do it.
     *
     * `unzip -t` cannot be the primary check: support depends on the firmware's
     * BusyBox vintage (prestonbrown/helixscreen#993). Verified on-device:
     *
     * | Platform                 | BusyBox | `unzip -t`            |
     * |--------------------------|---------|-----------------------|
     * | FlashForge AD5M          | 1.29.3  | absent — rejects `-t` |
     * | Creality K1              | 1.31.1  | absent — rejects `-t` |
     * | Elegoo Centauri Carbon   | 1.36.1  | present, correct      |
     *
     * On the first two, `unzip -tqq` exits 1 with "invalid option -- 't'", so an
     * intact download was reported as a corrupt archive and every in-app update
     * failed. python3's `zipfile.testzip()` does a real per-entry CRC check and
     * behaves identically everywhere, so it is tried first — but it needs zlib
     * as well as zipfile (release zips are deflated, and the AD5M's python3.7
     * has no zlib), so a python that cannot test reports Unverifiable rather
     * than Corrupt and we fall back to `unzip -l`.
     *
     * Public and static so it can be unit-tested directly.
     */
    static ZipIntegrity verify_zip_integrity(const std::string& zip_path);

    /** @brief Which tool this system can use to read a .zip. */
    enum class ZipTool {
        Unzip,  ///< An `unzip` binary is available.
        Python, ///< No unzip, but python3 with zipfile+zlib can do the job.
        None,   ///< Neither — zip releases cannot be handled at all.
    };

    /**
     * @brief Decide how (or whether) this system can read zip archives.
     *
     * Not every supported platform ships `unzip`: the Creality K2's OpenWrt
     * firmware has no unzip binary and no BusyBox unzip applet, but does carry
     * python3 with zipfile+zlib. Requiring unzip outright made every in-app
     * update on that firmware fail before it even downloaded, with an
     * apt-get hint that does not apply there.
     */
    static ZipTool available_zip_tool();

    /**
     * @brief Extract a single member from a .zip archive.
     *
     * Prefers `unzip -q -o`, falling back to python3's zipfile when no unzip
     * binary exists (K2). Restores the member's mode bits on the python path —
     * zipfile.extract() drops them, and an install.sh or bin/helix-screen that
     * lands without its exec bit is useless.
     *
     * Public and static so it can be unit-tested directly.
     *
     * @return 0 on success, non-zero on failure.
     */
    static int extract_zip_member(const std::string& zip_path, const std::string& extract_dir,
                                  const std::string& member);

  private:
    // Test-only seam (tests/test_helpers/update_checker_test_access.h). Seeds
    // cached_info_ + status_ so has_update_available() answers true without a
    // network round trip — the gate every "an update exists" consumer sits
    // behind. Kept out of the production API so no shipped code can fake one.
    friend class UpdateCheckerTestAccess;

    UpdateChecker() = default;
    ~UpdateChecker();

    // Non-copyable
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    /**
     * @brief Worker thread entry point
     */
    void do_check();

    // Channel-specific fetch methods
    bool fetch_stable_release(ReleaseInfo& info, std::string& error);
    bool fetch_beta_release(ReleaseInfo& info, std::string& error);
    bool fetch_dev_release(ReleaseInfo& info, std::string& error);

    // R2 CDN fetch (used as primary source before GitHub fallback)
    bool fetch_r2_manifest(const std::string& channel, ReleaseInfo& info, std::string& error);
    std::string get_r2_base_url() const;

    /**
     * @brief Report result to callback on LVGL thread
     * @param status Final status
     * @param info Release info (nullopt if not available)
     * @param error Error message (empty if no error)
     */
    void report_result(Status status, std::optional<ReleaseInfo> info, const std::string& error);

    void init_subjects();

    // State (protected by mutex_)
    std::atomic<Status> status_{Status::Idle};
    std::optional<ReleaseInfo> cached_info_;
    std::string error_message_;
    mutable std::mutex mutex_;

    // Rate limiting
    std::chrono::steady_clock::time_point last_check_time_{};
    static constexpr auto MIN_CHECK_INTERVAL = std::chrono::minutes{10};

    // Threading
    std::thread worker_thread_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> initialized_{false};
    Callback pending_callback_;

    // Channel cached on main thread before worker spawns (Config is not thread-safe).
    // These are written ONCE in check_for_updates() before the worker is spawned,
    // which is the only thing that makes the worker's UNLOCKED reads of them
    // (do_check(), get_r2_base_url()) safe. Never write them anywhere else — a
    // refresh from the main thread would race an in-flight worker.
    UpdateChannel cached_channel_{UpdateChannel::Stable};
    std::string cached_dev_url_;
    std::string cached_r2_base_url_;

    // Diagnostics-only mirror of the two Config-derived values above, deliberately
    // SEPARATE from cached_* so it can be refreshed at any time on the main thread
    // without racing a running check. Written and read only under mutex_; no worker
    // thread ever touches it.
    ConfigSnapshot config_snapshot_;

    // LVGL subjects for UI binding (update check)
    lv_subject_t status_subject_{};
    lv_subject_t version_text_subject_{};
    lv_subject_t new_version_subject_{};

    // String buffers for string subjects (must outlive subjects)
    char version_text_buf_[256]{};
    char new_version_buf_[64]{};

    // Download state
    std::atomic<DownloadStatus> download_status_{DownloadStatus::Idle};
    std::atomic<int> download_progress_{0};
    std::string download_error_;
    std::thread download_thread_;
    std::atomic<bool> download_cancelled_{false};

    // Download LVGL subjects
    lv_subject_t download_status_subject_{};
    lv_subject_t download_progress_subject_{};
    lv_subject_t download_text_subject_{};
    char download_text_buf_[256]{};

    // Download internals
    void do_download();
    void do_install(const std::string& tarball_path);

    /** @brief Validate downloaded tarball contains binary for correct architecture */
    bool validate_elf_architecture(const std::string& tarball_path);

    /// Cancel the auto-check timer without logging. Split out of stop_auto_check()
    /// so the destructor can share it — spdlog may already be gone by then.
    void cancel_auto_check_timer();

    // Auto-check timer
    lv_timer_t* auto_check_timer_{nullptr};

    // Notification modal
    lv_obj_t* notify_modal_{nullptr};

    // Notification subjects
    lv_subject_t release_notes_subject_{};
    lv_subject_t changelog_visible_subject_{};
    char release_notes_buf_[2048]{};

    SubjectManager subjects_;
    bool subjects_initialized_{false};

    /// Expires the status/progress callbacks queued from the check and download
    /// worker threads. Declared after `subjects_` so reverse-order member
    /// destruction invalidates it before the subjects it protects; also
    /// invalidated by shutdown(), which is where this class tears its subjects
    /// down (it has no deinit_subjects()). The in-lambda `subjects_initialized_`
    /// tests are not a substitute — reading that flag is itself a member access
    /// on a possibly-freed `this` (#1165, #1146).
    helix::AsyncLifetimeGuard async_lifetime_;
};
