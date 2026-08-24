// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/**
 * @brief PrinterState/LVGL-derived inputs to the bundle's `printer` section.
 *
 * Captured on the main thread by snapshot_printer_state(), then carried into
 * the collect worker as plain data. The section used to read the subjects
 * itself, which meant lv_subject_get_string() / lv_subject_get_int() and a
 * reference into PrinterState's unguarded printer_type_ all ran on the
 * HttpExecutor slow lane — a torn read at best, a use-after-free if
 * set_printer_type() landed mid-copy. Same shape as UpdateDiagnostics below,
 * and it makes the section assemblable in a test without a live PrinterState.
 */
struct PrinterSnapshot {
    bool captured = false;       ///< false = default-constructed, never filled
    std::string model;           ///< PrinterState::get_printer_type(), copied
    std::string klipper_version; ///< klipper_version subject, "" if unset
    int connection_state = -1;   ///< printer_connection_state subject; -1 = unavailable
    int klippy_state = -1;       ///< klippy_state subject; -1 = unavailable
};

struct BundleOptions {
    bool include_klipper_logs = false;
    bool include_moonraker_logs = false;
    std::string user_note;
    /// Filled by upload_async() on the main thread. Left uncaptured by direct
    /// collect() callers, which are main-thread themselves and get a snapshot
    /// taken inline instead.
    PrinterSnapshot printer;
};

struct BundleResult {
    bool success = false;
    std::string share_code;
    std::string error_message;
};

/// One config file as the include walker sees it. `status` is the HTTP status
/// (404 = a stale [include] of a deleted file, skipped silently).
struct ConfigFetchResult {
    int status = 0;
    std::string body;
};

/// Injection point for walk_include_tree(): maps a config path to its contents.
/// Production passes a Moonraker GET; tests pass a table.
using ConfigFetcher = std::function<ConfigFetchResult(const std::string& path)>;

/**
 * @brief Inputs to the bundle's `update` section.
 *
 * Plain data so the section can be assembled — and unit-tested — for both the
 * suppressed and not-suppressed cases without mutating the process-wide caches
 * behind updates_externally_managed() / self_update_supported().
 */
struct UpdateDiagnostics {
    std::string install_root;            ///< app_get_install_root() ("" if unresolvable)
    bool install_parent_writable = true; ///< dirname(install_root) writable → atomic swap
    bool install_root_writable = true;   ///< install_root itself writable → in-place update
    bool self_update_supported = true;   ///< self_update_supported(): either of those OR root
    bool externally_managed = false;     ///< updates_externally_managed()
    std::string channel;                 ///< "stable"|"beta"|"dev"; "" → "unknown"
    std::string r2_base_url;             ///< effective manifest base URL; "" → "unknown"
    std::string last_check_status;       ///< UpdateChecker::Status as a readable string
    std::string available_version;       ///< cached update version, "" if none
    std::string last_check_error;        ///< last check's error text, "" if none
    std::string platform_asset_name;     ///< exact release artifact this device requests
};

class DebugBundleCollector {
  public:
    /// Collect all debug data into JSON
    static nlohmann::json collect(const BundleOptions& options = {});

    /// Collect, compress, and upload asynchronously.
    /// Callback is invoked on the UI thread via helix::ui::queue_update().
    using ResultCallback = std::function<void(const BundleResult&)>;
    static void upload_async(const BundleOptions& options, ResultCallback callback);

    /// Read PrinterState and its LVGL subjects into plain data.
    ///
    /// MAIN THREAD ONLY. lv_subject_get_string() hands back the subject's live
    /// buffer, and PrinterState::get_printer_type() returns a reference to a
    /// member with no mutex, so both must be copied where the writer cannot be
    /// running concurrently.
    static PrinterSnapshot snapshot_printer_state();

    /// Individual collectors (public for testing)
    static nlohmann::json collect_system_info();
    /// Pure assembly from a snapshot — touches no LVGL and no PrinterState, so
    /// it is safe on the collect worker and testable without either.
    static nlohmann::json collect_printer_info(const PrinterSnapshot& snap);
    /// `num_lines <= 0` (the default) ships the whole ring — see
    /// resolve_log_tail_lines().
    static std::string collect_log_tail(int num_lines = 0);

    /// How many lines collect_log_tail() actually asks for (public for testing).
    ///
    /// The ring's capacity scales with device RAM (logging_init.cpp
    /// ring_capacity_for_ram(): 16 lines/MB, clamped to [2000, 20000]), but the
    /// collector used to ask for a hardcoded 2000 — the *floor* of that range.
    /// So every device above the smallest boards paid the RAM to retain lines
    /// the bundle then discarded: on a 473 MB AD5X the ring holds 7568 and we
    /// shipped 26% of it, which on bundle LYGVE39Y meant 12 minutes of history
    /// against a 9-minute stall that started before the window opened.
    ///
    /// A positive `requested` is honoured verbatim. Otherwise ship the whole
    /// ring, falling back to the floor when no ring is installed (watchdog
    /// build, or before logging init) so the on-disk cascade stays bounded.
    static size_t resolve_log_tail_lines(int requested, size_t ring_capacity);

    /// Metadata about the log pipeline so a bundle reader knows whether debug
    /// was being captured: { target, level, ring_lines, log_tail_source }.
    static nlohmann::json collect_log_meta();

    /**
     * @brief In-app update diagnostics: why the update UI is (or is not) usable.
     *
     * The About screen gates the two rows separately: "Check for Updates" on
     * !update_checks_suppressed() (firmware opt-out only), "Install Update"
     * additionally on !update_install_suppressed() (that, or an install tree this
     * user cannot write). Without this section a "cannot update" report carries no
     * evidence of which predicate fired, or of which of the two writability terms
     * behind the second one was missing.
     *
     * No LVGL access — every value comes from a plain C++ getter, so this is
     * safe from the HttpExecutor thread that upload_async() collects on.
     */
    static nlohmann::json collect_update_info();

    /// Assemble the `update` section from explicit inputs. Pure and static so
    /// both suppression branches are unit-testable.
    static nlohmann::json build_update_info(const UpdateDiagnostics& diag);

    static std::string collect_crash_txt();
    static nlohmann::json collect_sanitized_settings();
    static std::string collect_klipper_log_tail(int num_lines = 2000);

    /// Read klippy.log / moonraker.log straight off the local disk, for when
    /// Moonraker cannot serve them. Only for a same-host printer; the paths come
    /// from the daemons' own argv (see candidate_log_paths()), so an unknown
    /// platform layout returns empty rather than a guess.
    static std::string collect_local_log_tail(const std::string& log_name, int num_lines,
                                              int condense_max_repeats = 0);
    /// Line cap for moonraker.log. Deliberately far above what the byte budget
    /// yields (a 2 MiB condensed window measured 4319 lines) so MOONRAKER_TAIL_BYTES
    /// is what binds, not an arbitrary line count.
    static std::string collect_moonraker_log_tail(int num_lines = 8000);

    /// One entry from Moonraker's `logs` file root.
    struct LogFileEntry {
        std::string path; ///< relative to the logs root, may contain '/'
        uint64_t size = 0;
        double modified = 0.0;
    };

    /// Newest rotated predecessor of `stems` in a Moonraker `logs` listing, or ""
    /// (public for testing).
    ///
    /// Why this exists: a printer that just crashed is a printer that is about to
    /// be rebooted, and the reboot starts a fresh log. On bundle LYGVE39Y the
    /// incident lived entirely in the ROTATED moonraker.log.2026-08-11 while the
    /// active moonraker.log held nothing but the restart. We only ever fetched
    /// the active file, so the evidence was one HTTP GET away and we never made
    /// it.
    ///
    /// `stems` is a list because the klippy log is not called the same thing
    /// everywhere: Raspberry Pi installs use klippy.log, while AD5M/AD5X (and
    /// Vger1700's box) use printer.log. Rotated suffixes vary too — dated
    /// (`.2026-08-11`), dated+hour (`.2026-06-13_15`), and numeric (`.1`) all
    /// occur on real devices.
    ///
    /// Selection is deliberately narrow. A logs root holds other daemons' files,
    /// and on a live Pi `crowsnest.log.2026-08-11` is 940 KB and NEWER than every
    /// klippy rotation — so "newest rotated log" would ship a webcam log instead
    /// of the crash. Only `<stem>.<suffix>` at the root matches; nested paths
    /// (`mod/init.log.1`) and the active file itself never do.
    static std::string pick_rotated_sibling(const std::vector<LogFileEntry>& listing,
                                            const std::vector<std::string>& stems);

    /// GET /server/files/list?root=logs, parsed. Empty on any failure.
    static std::vector<LogFileEntry> fetch_log_listing(const std::string& base_url);

    /// Read crash_report.txt from config_dir (persists after crash.txt consumed)
    static std::string collect_crash_report_txt(const std::string& config_dir);

    /// Read raw crash.txt from config_dir (active crash, before next-boot rotation)
    static std::string collect_crash_txt(const std::string& config_dir);

    /// Read crash_history.json from config_dir (past crash submissions)
    static nlohmann::json collect_crash_history(const std::string& config_dir);

    /// Get double-hashed device ID from telemetry_device.json (for R2 cross-ref)
    static std::string collect_device_id(const std::string& config_dir);

    /// Read log tail from an explicit ordered list of paths (testable)
    static std::string collect_log_tail_from_paths(const std::vector<std::string>& paths,
                                                   int num_lines);

    /// Collect Moonraker state via REST (server info, printer state, config)
    static nlohmann::json collect_moonraker_info();

    /// Local evidence about a same-host Moonraker, gathered from /proc rather
    /// than from Moonraker. Everything in collect_moonraker_info() goes through
    /// Moonraker's own HTTP API, so a bundle uploaded while Moonraker is down
    /// carries only "No response" and cannot distinguish "not running" from
    /// "running, bound to an address we did not dial" (AD5X bundles TAU4PW4H /
    /// 865DXBQ7). Returns `{"same_host": false}` for a remote printer, where our
    /// own /proc says nothing about it.
    static nlohmann::json collect_moonraker_local_probe();

    /// Collect filament system data (AFC, Happy Hare, ACE, Spoolman, tool changers)
    static nlohmann::json collect_filament_system_info();

    /// Filter a Klipper object list to filament-related objects (public for testing)
    static nlohmann::json filter_filament_objects(const nlohmann::json& object_list);

    /// Extract bare `gcode_macro` NAMES from a Klipper object list (public for testing).
    ///
    /// Names only, and deliberately not merged into filter_filament_objects():
    /// that list feeds the objects/query batch in phase 2, and querying a few
    /// hundred macros would pull every macro's variables into the bundle. This
    /// answers "does macro X exist on this printer", which is the question the
    /// stripped config dump used to answer and no longer can -
    /// strip_klipper_config_dumps() drops printer.cfg before shape-collapse, so
    /// on an AD5X the 6668-line ZMOD config is simply gone. Concretely: whether
    /// `A_CHANGE_FILAMENT` (what AmsBackendAd5xIfs::do_change_tool dispatches)
    /// exists is answerable from neither ZMOD's source nor the stock AD5X
    /// printer.cfg, and none of the seven AD5X bundles on hand could settle it.
    ///
    /// Truncates at MAX_GCODE_MACRO_NAMES, reporting the drop rather than
    /// silently shortening the list.
    static nlohmann::json extract_gcode_macro_names(const nlohmann::json& object_list);

    /// Cap on captured macro names. A stock AD5X config defines 5 macros and a
    /// ZMOD one a few hundred; this only bounds a pathological config, and the
    /// bundle records `gcode_macros_truncated` when it bites.
    static constexpr size_t MAX_GCODE_MACRO_NAMES = 600;

    /// Collect printer.cfg and every config it `[include]`s, sanitized.
    ///
    /// Distinct from the config dump Klipper writes into klippy.log, which
    /// strip_klipper_config_dumps() deliberately removes: that dump is pure
    /// unique shapes, so it survived shape-collapse and spent the whole log
    /// line budget on config (84/63/58% of klipper_log on AD5X bundles
    /// 4QA7SZAM / LYGVE39Y / XSNN7PX5, commit ce4f21914). Fetching the files
    /// into their own field gives back the content without putting it back in
    /// competition with the incident window, and beats the log dump anyway -
    /// the log copy arrives head-truncated when the fetch slices through it.
    ///
    /// Every file body goes through sanitize_text_block() (per-LINE
    /// sanitize_value; see MAX_CONFIG_BYTES for why not whole-file).
    static nlohmann::json collect_printer_config();

    /// Breadth-first walk of `root`'s `[include]` tree (public for testing).
    ///
    /// Split out from collect_printer_config() so the traversal is reachable
    /// without Moonraker: the loop grows its own work queue while iterating it,
    /// which is exactly the shape that shipped a use-after-free in v0.99.112,
    /// and it had no coverage because the only caller needed a live printer.
    ///
    /// Returns a JSON object of path -> sanitized body (or `{"error": ...}` for
    /// a non-2xx that is not 404). `truncated_out` receives "" or the reason the
    /// walk stopped early; `bytes_out` receives the raw byte total. Both
    /// optional.
    static nlohmann::json walk_include_tree(const std::string& root,
                                            const std::vector<std::string>& available,
                                            const ConfigFetcher& fetch,
                                            std::string* truncated_out = nullptr,
                                            size_t* bytes_out = nullptr);

    /// Klipper `[include <pattern>]` targets, in file order (public for testing).
    /// Returns the raw patterns; resolution against the config root is
    /// resolve_include_pattern()'s job.
    static std::vector<std::string> parse_include_patterns(const std::string& body);

    /// Shell-glob match used to resolve an `[include]` pattern against the
    /// config-root file listing (public for testing). `*` and `?` do NOT cross
    /// a '/', matching Python glob, so `[include mod/*.cfg]` picks up
    /// `mod/a.cfg` but not `mod/sub/a.cfg`.
    static bool glob_match(const std::string& pattern, const std::string& path);

    /// Resolve one `[include]` pattern, relative to the including file's
    /// directory, against a config-root-relative file listing (public for
    /// testing). Returns matches in listing order.
    static std::vector<std::string>
    resolve_include_pattern(const std::string& pattern, const std::string& including_file,
                            const std::vector<std::string>& available);

    /// Total byte budget for captured config files. sanitize_value() replaces
    /// any single string over 4 KB with [REDACTED_LONG_VALUE], which is why the
    /// bodies are sanitized per line rather than whole - the cap here is about
    /// bundle size, not that guard. A ZMOD AD5X config is ~6668 lines / ~250 KB
    /// across its includes, so this holds a full one.
    static constexpr size_t MAX_CONFIG_BYTES = 512 * 1024;

    /// Cap on how many config files are fetched, including printer.cfg itself.
    /// Guards a pathological include tree; the bundle records
    /// `truncated` when either cap bites.
    static constexpr size_t MAX_CONFIG_FILES = 40;

    /// Shape-collapse threshold for klippy.log. Tuned against Klipper's
    /// per-second Stats line and ZMOD's 4-line toolhead dump.
    static constexpr int KLIPPER_CONDENSE_MAX_REPEATS = 40;

    /// Shape-collapse threshold for moonraker.log. Higher than Klipper's because
    /// moonraker's most valuable repeated block is proc_stats._handle_shutdown()'s
    /// ~30 host-CPU samples, and shutdowns cluster: with 40, a second shutdown in
    /// the window starves the first one's block down to 10 samples (measured on
    /// Vger1700's moonraker.log.2026-08-11, which had two 102 lines apart). 100
    /// keeps every block whole while still collapsing the log_request() padding
    /// that dominates a busy file.
    static constexpr int MOONRAKER_CONDENSE_MAX_REPEATS = 100;

    /// Byte window fetched for moonraker.log. Raised from the 512 KiB default
    /// because condensing shrinks the payload afterwards, so a bigger fetch buys
    /// history rather than bundle size: on a real 1524 KiB moonraker.log, 512 KiB
    /// reached 7656 lines and shipped 167 KB, while 2 MiB reached all 23862 and
    /// shipped 240 KB. moonraker.log is also the log that SURVIVES the events
    /// klippy.log does not — it lives outside the Klipper tree, so a rollback or
    /// reinstall leaves it intact (bundle LYGVE39Y).
    static constexpr int MOONRAKER_TAIL_BYTES = 2 * 1024 * 1024;

    /// Collapse repeating noise in a raw klippy.log tail (public for testing).
    ///
    /// Lines are grouped by "shape" (digit runs folded to N), and any shape
    /// occurring more than `max_repeats` times keeps only its most recent
    /// `max_repeats` occurrences, in place. The final `tail_lines` ship verbatim
    /// so the shutdown dump is never thinned.
    ///
    /// Deliberately shape-based rather than a list of known-noisy prefixes. The
    /// first version of this special-cased Klipper's per-second "Stats " line,
    /// which was the only padding in the sample it was written against — on a
    /// real ZMOD AD5X log the dominant noise turned out to be a 4-line
    /// `toolhead: max_velocity/max_accel/...` dump repeated 7916 times, which
    /// sailed straight through and made the shipped payload reach *less* far
    /// than the unfiltered tail it replaced. Shape-collapse catches both, and
    /// whatever the next firmware invents.
    ///
    /// Measured on two real AD5X logs: a 4 MiB (~85 min) window of the 11.7 MB
    /// crash log condenses 36142 lines to ~1300 (~340 KiB) while keeping the MCU
    /// shutdown and all 22 tool changes; a quiet 512 KiB sample keeps all 21 of
    /// its event lines.
    static std::string condense_klipper_log(const std::string& raw, int max_repeats = 40,
                                            int tail_lines = 200);

    /// Collect platform-specific diagnostic files (e.g., AD5X Adventurer5M.json)
    /// served via Moonraker's /server/files/<root>/<path> endpoint. Files that
    /// don't exist (404) are skipped silently; other errors are recorded in-line.
    static nlohmann::json collect_platform_files();

    /// Sanitize a string value for PII patterns (emails, credentials, webhooks, tokens, MACs)
    static std::string sanitize_value(const std::string& value);

    /// Recursively strip sensitive keys from JSON (public for integration testing)
    static nlohmann::json sanitize_json(const nlohmann::json& input, int depth = 0);

    /**
     * @brief Run sanitize_value() over each line of a multi-line body.
     *
     * Applied to every text section that leaves the machine. Line-at-a-time so
     * sanitize_value()'s 4 KB ReDoS guard does not redact a whole log as one
     * oversized value.
     *
     * Catches MACs, tokens, credentials and emails. It cannot catch an SSID —
     * that is an arbitrary user-chosen string with no pattern — so SSIDs are
     * kept out of the logs at the call site instead (include/log_redact.h).
     */
    static std::string sanitize_text_block(const std::string& body);

    /// Gzip compression using zlib
    static std::vector<uint8_t> gzip_compress(const std::string& data);

  private:
    static constexpr const char* WORKER_URL = "https://crash.helixscreen.org/v1/debug-bundle";
    static constexpr const char* INGEST_API_KEY = "hx-tel-v1-a7f3c9e2d1b84056";

    /// Blocking HTTP GET to a Moonraker endpoint, returns parsed JSON or error object
    static nlohmann::json moonraker_get(const std::string& base_url, const std::string& endpoint,
                                        int timeout_sec = 10);

    /// Get the Moonraker HTTP base URL (from IMoonrakerAPI if connected)
    static std::string get_moonraker_url();

    /// Fetch the tail of a log file from Moonraker using HTTP Range requests.
    ///
    /// `condense_max_repeats` of 0 ships the window verbatim; anything positive
    /// runs it through condense_klipper_log() at that threshold. The condenser is
    /// shape-based, not Klipper-specific, so moonraker.log uses it too — with its
    /// own threshold, see MOONRAKER_CONDENSE_MAX_REPEATS.
    ///
    /// `raw_bytes_out`, when non-null, receives how many bytes the fetch actually
    /// pulled off the wire, before condensing and the line cap. That is the only
    /// honest measure of how much of `tail_bytes` the file was able to fill —
    /// the returned string is post-condense and is smaller by an order of
    /// magnitude. prepend_rotated_predecessor() needs the raw figure.
    static std::string fetch_log_tail(const std::string& base_url, const std::string& endpoint,
                                      int num_lines, int tail_bytes = 524288,
                                      int condense_max_repeats = 0, int* raw_bytes_out = nullptr);

    /// Prepend the newest rotated predecessor when the active log is too short to
    /// have used its byte budget. See the definition for why that predicate is
    /// the right trigger. `active_raw_bytes` is the pre-condense fetch size from
    /// fetch_log_tail()'s `raw_bytes_out`, NOT active_body.size().
    static std::string prepend_rotated_predecessor(const std::string& base_url,
                                                   const std::vector<std::string>& stems,
                                                   const std::string& active_body,
                                                   int active_raw_bytes, int tail_bytes,
                                                   int num_lines, int condense_max_repeats);

    /// Check if a key name matches a sensitive pattern
    static bool is_sensitive_key(const std::string& key);
};

} // namespace helix
