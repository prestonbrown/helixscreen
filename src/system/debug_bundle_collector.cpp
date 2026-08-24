// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/debug_bundle_collector.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "data_root_resolver.h"
#include "helix_version.h"
#include "host_identity.h"
#include "http_executor.h"
#include "hv/requests.h"
#include "i_moonraker_api.h"
#include "logging_init.h"
#include "platform_capabilities.h"
#include "platform_info.h"
#include "printer_state.h"
#include "system/crash_history.h"
#include "system/helix_paths.h"
#include "system/log_collector.h"
#include "system/moonraker_local_probe.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#ifdef __ANDROID__
#include "system/http_android.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <zlib.h>

using json = nlohmann::json;
namespace helix {

// =============================================================================
// Main collect
// =============================================================================

json DebugBundleCollector::collect(const BundleOptions& options) {
    json bundle;

    bundle["version"] = HELIX_VERSION;

    if (!options.user_note.empty()) {
        bundle["user_note"] = sanitize_value(options.user_note);
    }

    // ISO 8601 timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char time_buf[64];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t_now));
    bundle["timestamp"] = time_buf;

    try {
        bundle["system"] = collect_system_info();
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect system info: {}", e.what());
        bundle["system"] = json{{"error", e.what()}};
    }

    // Why the in-app updater is (or isn't) usable on this device. Kept as a
    // top-level sibling of `system` because the answer to "cannot update" is a
    // handful of specific predicates, not general host facts.
    try {
        bundle["update"] = collect_update_info();
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect update info: {}", e.what());
        bundle["update"] = json{{"error", e.what()}};
    }

    try {
        // upload_async() captures this on the main thread; a direct caller is
        // main-thread itself, so taking it inline there is equally safe.
        bundle["printer"] = collect_printer_info(
            options.printer.captured ? options.printer : snapshot_printer_state());
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect printer info: {}", e.what());
        bundle["printer"] = json{{"error", e.what()}};
    }

    // Sanitized like klipper_log/moonraker_log below. This is defence in depth,
    // not the primary control: it catches MACs, tokens and emails, but an SSID
    // is an arbitrary string no regex can recognise, so SSIDs are kept out of
    // the ring at the log call site instead (see include/log_redact.h).
    try {
        auto log_tail = collect_log_tail();
        if (!log_tail.empty()) {
            bundle["log_tail"] = log_tail;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect log tail: {}", e.what());
    }

    // log_meta records the active sink target, the persistent level, and whether
    // the log_tail came from the live ring buffer or the on-disk cascade — so a
    // future reader knows whether debug detail was even being captured.
    try {
        bundle["log_meta"] = collect_log_meta();
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect log meta: {}", e.what());
    }

    try {
        auto crash_txt = collect_crash_txt();
        if (!crash_txt.empty()) {
            bundle["crash_txt"] = crash_txt;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect crash.txt: {}", e.what());
    }

    // Crash data: report text, history, and device ID for R2 cross-referencing.
    // Use the canonical resolver so we look in the same directory CrashReporter
    // wrote to (honors $HELIX_CONFIG_DIR set by ZMOD/RatOS/etc.).
    try {
        const std::string config_dir = helix::get_user_config_dir();

        auto crash_report = collect_crash_report_txt(config_dir);
        if (!crash_report.empty()) {
            bundle["crash_report"] = crash_report;
        }

        // Fallback: include the raw active crash.txt if the reporter hasn't
        // produced a human-readable crash_report.txt yet. This happens when the
        // watchdog auto-restarts faster than the reporter can run on next boot,
        // or when a fresh crash hasn't been processed at the time the user
        // uploads the bundle.
        auto crash_txt = collect_crash_txt(config_dir);
        if (!crash_txt.empty()) {
            bundle["crash_txt"] = crash_txt;
        }

        auto crash_history = CrashHistory::instance().to_json();
        if (!crash_history.empty()) {
            bundle["crash_history"] = sanitize_json(crash_history);
        }

        auto device_id = collect_device_id(config_dir);
        if (!device_id.empty()) {
            bundle["device_id"] = device_id;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect crash data: {}", e.what());
    }

    try {
        bundle["settings"] = collect_sanitized_settings();
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect settings: {}", e.what());
        bundle["settings"] = json{{"error", e.what()}};
    }

    try {
        bundle["moonraker"] = collect_moonraker_info();
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect moonraker info: {}", e.what());
        bundle["moonraker"] = json{{"error", e.what()}};
    }

    // Always, not only when the REST calls failed: knowing Moonraker was up AND
    // where it was bound is the control case that makes the down-case readable.
    try {
        bundle["moonraker_local"] = collect_moonraker_local_probe();
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to probe local moonraker: {}", e.what());
        bundle["moonraker_local"] = json{{"error", e.what()}};
    }

    try {
        bundle["filament_system"] = collect_filament_system_info();
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect filament system info: {}", e.what());
        bundle["filament_system"] = json{{"error", e.what()}};
    }

    try {
        auto platform_files = collect_platform_files();
        if (!platform_files.empty()) {
            bundle["platform_files"] = platform_files;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect platform files: {}", e.what());
        bundle["platform_files"] = json{{"error", e.what()}};
    }

    try {
        auto printer_config = collect_printer_config();
        if (!printer_config.empty()) {
            bundle["printer_config"] = printer_config;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[DebugBundle] Failed to collect printer config: {}", e.what());
        bundle["printer_config"] = json{{"error", e.what()}};
    }

    if (options.include_klipper_logs) {
        try {
            auto klipper_log = collect_klipper_log_tail();
            if (!klipper_log.empty()) {
                bundle["klipper_log"] = klipper_log;
            }
        } catch (const std::exception& e) {
            spdlog::warn("[DebugBundle] Failed to collect klipper log: {}", e.what());
        }
    }

    if (options.include_moonraker_logs) {
        try {
            auto moonraker_log = collect_moonraker_log_tail();
            if (!moonraker_log.empty()) {
                bundle["moonraker_log"] = moonraker_log;
            }
        } catch (const std::exception& e) {
            spdlog::warn("[DebugBundle] Failed to collect moonraker log: {}", e.what());
        }
    }

    return bundle;
}

// =============================================================================
// System info
// =============================================================================

// Map a platform key ("ad5x", "ad5m", etc.) to the display-name root the
// printer database uses for that hardware. The dashboard's title generator
// can compare this against the user-picked model name (printer.model) and
// surface the mismatch instead of trusting the wizard pick blindly. The
// AD5X/AD5M Pro pair is the prototypical mismatch — same Klipper config,
// different hardware; a wizard pick of "Adventurer 5M Pro" on an AD5X
// platform is structurally wrong but has no local way to self-correct
// without reflashing or re-running the wizard.
//
// Generic dev/SBC platforms (pi, pi32, x86) have no specific printer hardware
// to compare against, so platform_model is omitted for them.
static bool platform_has_printer_hardware(const std::string& key) {
    return key != "pi" && key != "pi32" && key != "x86";
}

json DebugBundleCollector::collect_system_info() {
    json sys;

    sys["platform"] = UpdateChecker::get_platform_key();
    sys["host_arch"] = helix::host_arch_string();

    auto caps = PlatformCapabilities::detect();
    sys["total_ram_mb"] = caps.total_ram_mb;
    sys["cpu_cores"] = caps.cpu_cores;

    // Read uptime from /proc/uptime if available
    std::ifstream uptime_file("/proc/uptime");
    if (uptime_file.good()) {
        double uptime_sec = 0.0;
        uptime_file >> uptime_sec;
        sys["uptime_seconds"] = static_cast<int>(uptime_sec);
    }

    return sys;
}

// =============================================================================
// Update diagnostics
// =============================================================================

// Readable name for the last check's status. The raw enum ordinal means nothing
// to a human reading a bundle or to the dashboard rendering it.
static const char* update_status_name(UpdateChecker::Status status) {
    switch (status) {
    case UpdateChecker::Status::Idle:
        return "idle";
    case UpdateChecker::Status::Checking:
        return "checking";
    case UpdateChecker::Status::UpdateAvailable:
        return "update_available";
    case UpdateChecker::Status::UpToDate:
        return "up_to_date";
    case UpdateChecker::Status::Error:
        return "error";
    }
    return "unknown";
}

json DebugBundleCollector::build_update_info(const UpdateDiagnostics& diag) {
    json upd;

    // install_root is a filesystem path and can embed a username
    // (/home/pi/helixscreen). It goes through sanitize_value() like every other
    // string that leaves the machine, which catches credential/email/MAC shapes
    // but deliberately leaves the directory layout intact: WHICH root the app
    // installed into is the entire diagnostic value of this field, and the same
    // path already appears verbatim throughout log_tail and crash_report.
    upd["install_root"] = sanitize_value(diag.install_root);

    // The predicates the update UI actually gates on. `suppressed` is the one
    // that decides whether the "Check for Updates" / "Install Update" rows
    // exist at all (show_update_settings = !update_checks_suppressed()); the
    // rest say which cause fired.
    //
    // The two writability terms are reported separately from self_update_supported
    // (their OR with root escalation) because each names a different update route
    // and a different fix. parent writable → install.sh takes the atomic swap;
    // only the root writable → it takes the in-place replacement; neither, with
    // self_update_supported still true → it is leaning on sudo, which the shipped
    // systemd unit forbids (NoNewPrivileges=true); neither, with
    // self_update_supported false → genuinely read-only, and the user needs to
    // re-run the installer rather than wait for the in-app updater.
    upd["install_parent_writable"] = diag.install_parent_writable;
    upd["install_root_writable"] = diag.install_root_writable;
    upd["self_update_supported"] = diag.self_update_supported;
    upd["externally_managed"] = diag.externally_managed;
    upd["suppressed"] =
        compute_update_install_suppressed(diag.externally_managed, diag.self_update_supported);

    // These two come from UpdateChecker's main-thread config snapshot, which is
    // empty until init() runs. Report that as "unknown" rather than "" so a
    // bundle reader can tell "updater never initialised" from "channel is
    // stable" — an empty string reads like a failed lookup either way.
    upd["channel"] = diag.channel.empty() ? "unknown" : diag.channel;
    upd["r2_base_url"] =
        diag.r2_base_url.empty() ? std::string("unknown") : sanitize_value(diag.r2_base_url);
    upd["last_check_status"] = diag.last_check_status;
    upd["platform_asset_name"] = diag.platform_asset_name;

    if (!diag.available_version.empty()) {
        upd["available_version"] = diag.available_version;
    }
    if (!diag.last_check_error.empty()) {
        upd["last_check_error"] = sanitize_value(diag.last_check_error);
    }

    return upd;
}

json DebugBundleCollector::collect_update_info() {
    UpdateDiagnostics diag;

    diag.install_root = app_get_install_root();
    // Probe the two writability terms directly rather than through
    // compute_self_update_supported(): that function ORs them together (plus
    // escalation), so routing either one through it reports the OR under a name
    // that promises one term. Which term is open decides which update route
    // install.sh will take, and that is the whole diagnostic value here.
    if (!diag.install_root.empty()) {
        const std::string parent = std::filesystem::path(diag.install_root).parent_path().string();
        diag.install_parent_writable = !parent.empty() && helix::paths::is_writable_dir(parent);
        diag.install_root_writable = helix::paths::is_writable_dir(diag.install_root);
    }
    diag.self_update_supported = self_update_supported();
    diag.externally_managed = updates_externally_managed();

    // upload_async() collects on HttpExecutor::slow(), so nothing here may touch
    // LVGL or Config. get_status() is atomic; get_cached_update(),
    // get_error_message() and config_snapshot() copy under UpdateChecker's own
    // mutex; platform_asset_name() is pure. The channel and manifest URL come
    // from the snapshot rather than get_channel()/effective_r2_base_url()
    // because those two read Config, which is main-thread-only (config.h).
    const auto& checker = UpdateChecker::instance();
    const auto snapshot = checker.config_snapshot();
    diag.channel = snapshot.channel;
    diag.r2_base_url = snapshot.r2_base_url;
    diag.last_check_status = update_status_name(checker.get_status());
    diag.platform_asset_name = UpdateChecker::platform_asset_name();

    if (auto cached = checker.get_cached_update()) {
        diag.available_version = cached->version;
    }
    diag.last_check_error = checker.get_error_message();

    return build_update_info(diag);
}

// =============================================================================
// Printer info
// =============================================================================

PrinterSnapshot DebugBundleCollector::snapshot_printer_state() {
    PrinterSnapshot snap;
    try {
        auto& ps = get_printer_state();

        // Copy, do not bind: get_printer_type() returns a reference to a member
        // that set_printer_type() reassigns without a mutex.
        snap.model = ps.get_printer_type();

        if (auto* kv_subj = ps.get_klipper_version_subject()) {
            const char* kv = lv_subject_get_string(kv_subj);
            if (kv && kv[0] != '\0')
                snap.klipper_version = kv;
        }
        if (auto* conn_subj = ps.get_printer_connection_state_subject())
            snap.connection_state = lv_subject_get_int(conn_subj);
        if (auto* klippy_subj = ps.get_klippy_state_subject())
            snap.klippy_state = lv_subject_get_int(klippy_subj);

        snap.captured = true;
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Failed to snapshot printer state: {}", e.what());
    }
    return snap;
}

json DebugBundleCollector::collect_printer_info(const PrinterSnapshot& snap) {
    json printer;

    try {
        const std::string& user_model = snap.model;
        printer["model"] = user_model;

        // Platform-derived canonical hardware name. Hardware platform is
        // detected at build/runtime (e.g. /usr/prog or /ZMOD on AD5X) and is
        // ground truth; printer.model is whatever the user picked in the
        // wizard, which can disagree (the AD5X/AD5M-Pro pair is the typical
        // case — same Klipper config, different hardware). Dashboard title
        // generation should prefer platform_model when it differs from model
        // so AD5X devices stop showing as "5M Pro" in the bundle list.
        const std::string platform = UpdateChecker::get_platform_key();
        const std::string platform_model = platform_has_printer_hardware(platform)
                                               ? UpdateChecker::get_platform_display_name(platform)
                                               : std::string{};
        if (!platform_model.empty()) {
            printer["platform_model"] = platform_model;
            // Substring match handles trim variations ("5M" vs "5M Pro"). If
            // the user-picked model doesn't even contain the platform name,
            // surface the mismatch as a flag the dashboard can render.
            if (!user_model.empty() && user_model.find(platform_model) == std::string::npos &&
                platform_model.find(user_model) == std::string::npos) {
                printer["platform_model_mismatch"] = true;
            }
        }

        if (!snap.klipper_version.empty()) {
            printer["klipper_version"] = snap.klipper_version;
        }

        // Connection state
        const char* state_names[] = {"disconnected", "connecting", "connected", "reconnecting",
                                     "failed"};
        if (snap.connection_state >= 0 && snap.connection_state < 5) {
            printer["connection_state"] = state_names[snap.connection_state];
        }

        // Klippy state
        const char* klippy_names[] = {"ready", "startup", "shutdown", "error"};
        if (snap.klippy_state >= 0 && snap.klippy_state < 4) {
            printer["klippy_state"] = klippy_names[snap.klippy_state];
        }
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Failed to collect printer info: {}", e.what());
        printer["error"] = e.what();
    }

    return printer;
}

// =============================================================================
// Log tail — cascades file → syslog → journal (see helix::logs for ordering)
// =============================================================================

size_t DebugBundleCollector::resolve_log_tail_lines(int requested, size_t ring_capacity) {
    // Historical fixed size, and logging_init's own floor (MIN_RING_LINES). Used
    // when there is no ring to measure, so the file/syslog cascade still gets a
    // bound rather than an open-ended read.
    constexpr size_t FALLBACK_LOG_TAIL_LINES = 2000;

    if (requested > 0) {
        return static_cast<size_t>(requested);
    }
    return ring_capacity > 0 ? ring_capacity : FALLBACK_LOG_TAIL_LINES;
}

std::string DebugBundleCollector::collect_log_tail(int num_lines) {
    const size_t lines = resolve_log_tail_lines(num_lines, helix::logging::ring_buffer_capacity());

    // Sanitized here rather than at the call site so every consumer of the log
    // tail is covered. The ring captures at debug regardless of the user's
    // configured verbosity, so this text leaves the machine on every bundle.
    return sanitize_text_block(helix::logs::tail_best(static_cast<int>(lines)));
}

// =============================================================================
// Log meta — tells a bundle reader whether debug was being captured and where
// the log_tail actually came from. Cheap, no I/O, no network.
// =============================================================================

json DebugBundleCollector::collect_log_meta() {
    json meta;
    // The destination the persistent (file/syslog/journal/console) sinks write
    // to, and the level they run at. If level is "warn", the on-disk logs only
    // have WARN+; the ring buffer (below) is the only place debug survives.
    meta["target"] = helix::logging::effective_destination();
    meta["level"] = spdlog::level::to_string_view(helix::logging::effective_log_level()).data();
    meta["ring_lines"] = helix::logging::ring_buffer_capacity();

    // Where collect_log_tail() got its content. The ring buffer is preferred
    // when this process is alive; an empty ring (crash-reporter next boot)
    // falls through to the file/syslog/journal cascade.
    meta["log_tail_source"] =
        helix::logging::tail_ring_buffer(1).empty() ? "file_cascade" : "ring_buffer";
    return meta;
}

// =============================================================================
// Crash file
// =============================================================================

std::string DebugBundleCollector::collect_crash_txt() {
    // Build list of config directories to search
    std::vector<std::string> config_dirs = {"config"};

    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        config_dirs.push_back(std::string(home) + "/helixscreen/config");
    }

    // Absolute paths for embedded platforms (AD5M, AD5X, K1, etc.)
    config_dirs.push_back("/opt/helixscreen/config");
    config_dirs.push_back("/srv/helixscreen/config");
    config_dirs.push_back("/usr/data/helixscreen/config");

    // Try crash.txt first, then rotated files (crash_1.txt, crash_2.txt, crash_3.txt).
    // The crash reporter rotates crash.txt → crash_1.txt after consuming it,
    // so the raw file is usually only available as a rotated copy.
    static constexpr const char* suffixes[] = {"crash.txt", "crash_1.txt", "crash_2.txt",
                                               "crash_3.txt"};

    for (const auto& suffix : suffixes) {
        for (const auto& dir : config_dirs) {
            std::string path = dir + "/" + suffix;
            std::ifstream file(path);
            if (!file.good()) {
                continue;
            }

            std::ostringstream content;
            content << file.rdbuf();
            std::string result = content.str();

            if (!result.empty()) {
                spdlog::debug("[DebugBundle] Read {} from {}", suffix, path);
                return sanitize_text_block(result);
            }
        }
    }

    return {};
}

// =============================================================================
// Sanitized settings
// =============================================================================

bool DebugBundleCollector::is_sensitive_key(const std::string& key) {
    // Case-insensitive substring match for sensitive patterns
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    static const std::vector<std::string> sensitive_patterns = {
        "token", "password", "secret", "key", "webhook", "credential", "auth", "bearer"};

    for (const auto& pattern : sensitive_patterns) {
        if (lower_key.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string DebugBundleCollector::sanitize_value(const std::string& value) {
    if (value.empty())
        return value;

    // Skip regex on very long strings to prevent ReDoS / performance issues
    if (value.size() > 4096) {
        return "[REDACTED_LONG_VALUE]";
    }

    // Check webhook URLs first (full replacement)
    if (value.find("discord.com/api/webhooks") != std::string::npos ||
        value.find("hooks.slack.com") != std::string::npos ||
        value.find("api.telegram.org/bot") != std::string::npos ||
        value.find("api.pushover.net") != std::string::npos ||
        value.find("ntfy.sh/") != std::string::npos ||
        value.find("maker.ifttt.com") != std::string::npos) {
        return "[REDACTED_WEBHOOK]";
    }

    try {
        // Check for long token-like strings (40+ chars of hex/base64/alphanum with prefix)
        static const std::regex token_re(
            R"(^(?:ghp_|gho_|glpat-|xoxb-|xoxp-)?[A-Za-z0-9+/=_-]{36,}$)");
        if (std::regex_match(value, token_re)) {
            return "[REDACTED_TOKEN]";
        }

        std::string result = value;

        // Redact URL credentials: ://user:pass@ -> ://[REDACTED_CREDENTIALS]@
        static const std::regex cred_url_re(R"(://[^@/\s]+:[^@/\s]+@)");
        result = std::regex_replace(result, cred_url_re, "://[REDACTED_CREDENTIALS]@");

        // Redact email addresses
        static const std::regex email_re(R"(\b[\w.+-]+@[\w-]+\.[\w.]+\b)");
        result = std::regex_replace(result, email_re, "[REDACTED_EMAIL]");

        // Redact MAC addresses (aa:bb:cc:dd:ee:ff or AA-BB-CC-DD-EE-FF)
        static const std::regex mac_re(R"(\b([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}\b)");
        result = std::regex_replace(result, mac_re, "[REDACTED_MAC]");

        return result;
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] sanitize_value regex failed: {}", e.what());
        return value; // Return unsanitized rather than crash
    }
}

json DebugBundleCollector::sanitize_json(const json& input, int depth) {
    if (depth > 32) {
        spdlog::debug("[DebugBundle] sanitize_json hit depth limit, passing through");
        return input;
    }

    if (input.is_object()) {
        json result = json::object();
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (is_sensitive_key(it.key())) {
                result[it.key()] = "[REDACTED]";
            } else {
                result[it.key()] = sanitize_json(it.value(), depth + 1);
            }
        }
        return result;
    }

    if (input.is_array()) {
        json result = json::array();
        for (const auto& element : input) {
            result.push_back(sanitize_json(element, depth + 1));
        }
        return result;
    }

    // Sanitize string values for PII patterns
    if (input.is_string()) {
        return sanitize_value(input.get<std::string>());
    }

    // Non-string primitives pass through unchanged
    return input;
}

json DebugBundleCollector::collect_sanitized_settings() {
    // Try common config locations for settings.json
    std::vector<std::string> settings_paths = {
        helix::writable_path("settings.json"),
        helix::writable_path("helixconfig.json"),
    };

    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        settings_paths.push_back(std::string(home) + "/helixscreen/config/settings.json");
        settings_paths.push_back(std::string(home) + "/helixscreen/config/helixconfig.json");
    }

    for (const auto& path : settings_paths) {
        std::ifstream file(path);
        if (!file.good()) {
            continue;
        }

        try {
            json settings = json::parse(file);
            spdlog::debug("[DebugBundle] Read settings from {}", path);
            return sanitize_json(settings);
        } catch (const json::parse_error& e) {
            spdlog::debug("[DebugBundle] Failed to parse settings from {}: {}", path, e.what());
        }
    }

    return json::object();
}

// =============================================================================
// Moonraker REST collection
// =============================================================================

std::string DebugBundleCollector::get_moonraker_url() {
    auto* api = get_moonraker_api();
    if (!api)
        return {};
    return api->get_http_base_url();
}

namespace {

// Result of a raw HTTP GET. status==0 means the request never produced a
// response (network failure, timeout, or hv exception); callers should treat
// that as a hard failure distinct from an HTTP error code.
struct RawHttpResult {
    int status = 0;
    std::string body;
};

// Shared HTTP-GET primitive used by moonraker_get() (JSON) and platform-file
// fetchers (TEXT). Joins base + endpoint with a single slash if neither side
// supplies one, so callers can pass either form. Never throws.
RawHttpResult http_get_raw(const std::string& base_url, const std::string& endpoint,
                           int timeout_sec) {
    RawHttpResult result;
    if (base_url.empty()) {
        return result;
    }
    std::string url = base_url;
    if (!endpoint.empty() && endpoint[0] != '/' && !url.empty() && url.back() != '/') {
        url += '/';
    }
    url += endpoint;

    try {
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = url;
        req->timeout = timeout_sec;

        auto resp = requests::request(req);
        if (!resp) {
            return result;
        }
        result.status = static_cast<int>(resp->status_code);
        result.body = std::move(resp->body);
    } catch (const std::exception&) {
        // status stays 0 — caller treats as network failure.
    }
    return result;
}

} // namespace

// Sanitize a multi-line text block by sanitize_value()-ing each line. Avoids
// sanitize_value()'s 4 KB ReDoS guard kicking in on whole-file inputs (which
// would redact the entire content as [REDACTED_LONG_VALUE]).
std::string DebugBundleCollector::sanitize_text_block(const std::string& body) {
    std::string result;
    result.reserve(body.size());
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos)
            nl = body.size();
        result += DebugBundleCollector::sanitize_value(body.substr(pos, nl - pos));
        if (nl < body.size()) {
            result += '\n';
        }
        pos = nl + 1;
    }
    return result;
}

json DebugBundleCollector::moonraker_get(const std::string& base_url, const std::string& endpoint,
                                         int timeout_sec) {
    if (base_url.empty()) {
        return json{{"error", "Moonraker not connected"}};
    }

    auto raw = http_get_raw(base_url, endpoint, timeout_sec);
    if (raw.status == 0) {
        return json{{"error", "No response from " + endpoint}};
    }
    if (raw.status < 200 || raw.status >= 300) {
        return json{{"error", "HTTP " + std::to_string(raw.status) + " from " + endpoint}};
    }
    try {
        return json::parse(raw.body);
    } catch (const json::parse_error& e) {
        return json{{"error", "JSON parse error from " + endpoint + ": " + e.what()}};
    }
}

json DebugBundleCollector::collect_moonraker_info() {
    json mr;
    std::string base_url = get_moonraker_url();

    if (base_url.empty()) {
        spdlog::debug("[DebugBundle] Moonraker not connected, skipping moonraker info");
        mr["server_info"] = json{{"error", "Not connected"}};
        mr["printer_info"] = json{{"error", "Not connected"}};
        mr["system_info"] = json{{"error", "Not connected"}};
        mr["printer_state"] = json{{"error", "Not connected"}};
        mr["config"] = json{{"error", "Not connected"}};
        return mr;
    }

    spdlog::info("[DebugBundle] Collecting Moonraker info from {}", base_url);

    // Server info — version, components, klippy state
    try {
        mr["server_info"] = sanitize_json(moonraker_get(base_url, "/server/info"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] server_info collection failed: {}", e.what());
        mr["server_info"] = json{{"error", e.what()}};
    }

    // Printer info — hostname, klipper version, state
    try {
        mr["printer_info"] = sanitize_json(moonraker_get(base_url, "/printer/info"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] printer_info collection failed: {}", e.what());
        mr["printer_info"] = json{{"error", e.what()}};
    }

    // System info — OS, CPU, memory, network (heavy sanitization for MACs/IPs)
    try {
        mr["system_info"] = sanitize_json(moonraker_get(base_url, "/machine/system_info"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] system_info collection failed: {}", e.what());
        mr["system_info"] = json{{"error", e.what()}};
    }

    // Current printer state — temps, positions, fans, print progress
    try {
        mr["printer_state"] = sanitize_json(
            moonraker_get(base_url, "/printer/objects/query"
                                    "?heater_bed&extruder&print_stats&toolhead&motion_report"
                                    "&fan&display_status&virtual_sdcard"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] printer_state collection failed: {}", e.what());
        mr["printer_state"] = json{{"error", e.what()}};
    }

    // Full Moonraker config — heavily sanitized
    try {
        mr["config"] = sanitize_json(moonraker_get(base_url, "/server/config"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] config collection failed: {}", e.what());
        mr["config"] = json{{"error", e.what()}};
    }

    return mr;
}

json DebugBundleCollector::collect_moonraker_local_probe() {
    json probe;

    // The endpoint comes from the API's base URL, NOT from Config: this runs on
    // HttpExecutor::slow() and Config is main-thread-only (see the note in
    // collect_update_info()). get_moonraker_url() is the same accessor
    // collect_moonraker_info() already uses from this thread.
    std::string host;
    uint16_t port = 7125;
    const std::string base_url = get_moonraker_url();
    if (!helix::diag::split_host_port(base_url, host, port)) {
        probe["probed"] = false;
        probe["reason"] = "no moonraker endpoint configured";
        return probe;
    }

    const bool same_host = helix::is_moonraker_on_same_host(host);
    probe["same_host"] = same_host;
    probe["port"] = port;
    if (!same_host) {
        // Deliberately no listener/process data: those would describe THIS
        // machine, and a reader would take them for the printer's.
        return probe;
    }

    try {
        const auto listeners = helix::diag::listeners_on_port(port);
        probe["listening"] = !listeners.empty();
        json addrs = json::array();
        for (const auto& a : listeners) {
            addrs.push_back(a);
        }
        probe["listen_addrs"] = addrs;
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] listener probe failed: {}", e.what());
        probe["listen_error"] = e.what();
    }

    try {
        const auto procs = helix::diag::find_moonraker_processes();
        json arr = json::array();
        for (const auto& p : procs) {
            arr.push_back(json{{"pid", p.pid}, {"cmdline", sanitize_value(p.cmdline)}});
        }
        probe["processes"] = arr;
        probe["process_count"] = static_cast<int>(procs.size());
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] process probe failed: {}", e.what());
        probe["process_error"] = e.what();
    }

    return probe;
}

// =============================================================================
// Filament system info (AFC, Happy Hare, ACE, Spoolman, tool changers)
// =============================================================================

json DebugBundleCollector::filter_filament_objects(const json& object_list) {
    static const std::vector<std::string> prefixes = {
        "AFC", "mmu", "toolchanger", "tool ", "filament_switch_sensor", "filament_motion_sensor",
        // Creality CFS (K2 family): [box] is the CFS controller, [filament_rack]
        // is the slot-occupancy gate. Both expose state via printer.objects.query.
        "box", "filament_rack"};

    json result = json::array();
    if (!object_list.is_array())
        return result;

    for (const auto& obj : object_list) {
        if (!obj.is_string())
            continue;
        std::string name = obj.get<std::string>();
        for (const auto& prefix : prefixes) {
            if (name.compare(0, prefix.size(), prefix) == 0) {
                result.push_back(name);
                break;
            }
        }
    }
    return result;
}

json DebugBundleCollector::extract_gcode_macro_names(const json& object_list) {
    static constexpr const char* PREFIX = "gcode_macro ";
    static constexpr size_t PREFIX_LEN = 12; // strlen("gcode_macro ")

    json result = json::array();
    if (!object_list.is_array())
        return result;

    for (const auto& obj : object_list) {
        if (!obj.is_string())
            continue;
        const std::string name = obj.get<std::string>();
        if (name.compare(0, PREFIX_LEN, PREFIX) != 0)
            continue;
        // Store the bare name: "gcode_macro A_CHANGE_FILAMENT" -> "A_CHANGE_FILAMENT".
        // A macro named exactly "gcode_macro " with nothing after it is not a
        // thing Klipper accepts, but an empty push would read as a real entry.
        if (name.size() <= PREFIX_LEN)
            continue;
        result.push_back(name.substr(PREFIX_LEN));
        if (result.size() >= MAX_GCODE_MACRO_NAMES)
            break;
    }
    return result;
}

json DebugBundleCollector::collect_filament_system_info() {
    json fs;
    std::string base_url = get_moonraker_url();

    if (base_url.empty()) {
        spdlog::debug("[DebugBundle] Moonraker not connected, skipping filament system info");
        fs["object_list"] = json::array();
        fs["gcode_macros"] = json::array();
        fs["object_state"] = json{{"error", "Not connected"}};
        fs["spoolman_status"] = json{{"error", "Not connected"}};
        fs["afc_version"] = json{{"error", "Not connected"}};
        fs["mmu_version"] = json{{"error", "Not connected"}};
        return fs;
    }

    spdlog::info("[DebugBundle] Collecting filament system info from {}", base_url);

    // Phase 1: Discover filament-related Klipper objects
    json discovered = json::array();
    json macro_names = json::array();
    size_t total_macros = 0;
    try {
        auto objects_resp = moonraker_get(base_url, "/printer/objects/list");
        if (objects_resp.contains("result") && objects_resp["result"].contains("objects")) {
            const json& objects = objects_resp["result"]["objects"];
            discovered = filter_filament_objects(objects);
            macro_names = extract_gcode_macro_names(objects);
            if (objects.is_array()) {
                for (const auto& obj : objects) {
                    if (obj.is_string() && obj.get<std::string>().rfind("gcode_macro ", 0) == 0) {
                        ++total_macros;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] object_list discovery failed: {}", e.what());
    }
    fs["object_list"] = discovered;
    // Names only - see extract_gcode_macro_names(). NOT folded into
    // object_list, which drives the objects/query batch below.
    fs["gcode_macros"] = macro_names;
    if (total_macros > macro_names.size()) {
        fs["gcode_macros_truncated"] =
            json{{"captured", macro_names.size()}, {"total", total_macros}};
        spdlog::info("[DebugBundle] gcode_macro names truncated: {} of {} captured",
                     macro_names.size(), total_macros);
    }

    // Phase 2: Batch query all discovered objects
    if (!discovered.empty()) {
        try {
            // Build query string with URL-encoded object names
            std::string query = "/printer/objects/query?";
            for (size_t i = 0; i < discovered.size(); ++i) {
                if (i > 0)
                    query += '&';
                // Percent-encode spaces in object names (e.g. "AFC_stepper lane1")
                std::string name = discovered[i].get<std::string>();
                std::string encoded;
                encoded.reserve(name.size());
                for (char c : name) {
                    if (c == ' ')
                        encoded += "%20";
                    else
                        encoded += c;
                }
                query += encoded;
            }
            fs["object_state"] = sanitize_json(moonraker_get(base_url, query));
        } catch (const std::exception& e) {
            spdlog::debug("[DebugBundle] filament object_state query failed: {}", e.what());
            fs["object_state"] = json{{"error", e.what()}};
        }
    } else {
        fs["object_state"] = json::object();
    }

    // Phase 3: Additional endpoints
    try {
        fs["spoolman_status"] = sanitize_json(moonraker_get(base_url, "/server/spoolman/status"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] spoolman_status failed: {}", e.what());
        fs["spoolman_status"] = json{{"error", e.what()}};
    }

    try {
        fs["afc_version"] =
            sanitize_json(moonraker_get(base_url, "/server/database/item?namespace=afc-install"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] afc_version failed: {}", e.what());
        fs["afc_version"] = json{{"error", e.what()}};
    }

    try {
        fs["mmu_version"] =
            sanitize_json(moonraker_get(base_url, "/server/database/item?namespace=mmu-install"));
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] mmu_version failed: {}", e.what());
        fs["mmu_version"] = json{{"error", e.what()}};
    }

    return fs;
}

// =============================================================================
// Platform-specific diagnostic files
// =============================================================================

enum class PlatformFileFormat { JSON, TEXT };

struct PlatformFile {
    std::string name;           // Logical name used as bundle key
    std::string moonraker_path; // Path rooted at Moonraker base URL
    PlatformFileFormat format;
};

// Returns the platform-specific files to capture. moonraker_path is rooted
// at the Moonraker base URL (e.g. "/server/files/config/Adventurer5M.json").
// Empty list = nothing to collect.
//
// Keep entries small (a few KB each) — this is the debug-bundle hot path. For
// larger files, route through a dedicated capture with a size cap.
static std::vector<PlatformFile> platform_diagnostic_files(const std::string& platform) {
    if (platform == "ad5x" || platform == "ad5m") {
        return {
            // ZMOD's authoritative IFS slot truth: color, material, lessWaste
            // pairings. Polling this is our primary change-detection mechanism
            // in AmsBackendAd5xIfs::poll_adventurer_json(); having it in the
            // bundle lets us verify what zmod wrote vs. what the UI cached.
            {"Adventurer5M.json", "/server/files/config/Adventurer5M.json",
             PlatformFileFormat::JSON},
            // zmod's user-defined filament types (PLA+, RPLA, HELIX, ...). Read
            // by the COLOR gcode macro at print time. helix-screen's edit modal
            // currently restricts to the firmware whitelist and silently
            // normalises user types away on save (#904); the file in the bundle
            // lets us see exactly what types were defined and how the macro
            // consumes them.
            {"user.cfg", "/server/files/config/mod_data/user.cfg", PlatformFileFormat::TEXT},
        };
    }
    return {};
}

// Fetch a text file from Moonraker. Returns body + HTTP status; an HTTP-status
// of 404 is a normal "not present on this device" signal callers should treat
// as skip-silently. Truncates the body at MAX_TEXT_BYTES to keep bundles small.
static RawHttpResult http_get_text(const std::string& base_url, const std::string& endpoint,
                                   int timeout_sec) {
    auto raw = http_get_raw(base_url, endpoint, timeout_sec);
    // Cap text-file capture at 256 KB. Diagnostic files we currently ship are
    // < 8 KB; the cap is a guardrail against future entries that grow large.
    constexpr size_t MAX_TEXT_BYTES = 256 * 1024;
    if (raw.body.size() > MAX_TEXT_BYTES) {
        raw.body.resize(MAX_TEXT_BYTES);
        raw.body += "\n[truncated]\n";
    }
    return raw;
}

json DebugBundleCollector::collect_platform_files() {
    json result = json::object();
    const std::string platform = UpdateChecker::get_platform_key();
    auto files = platform_diagnostic_files(platform);
    if (files.empty()) {
        return result;
    }

    const std::string base_url = get_moonraker_url();
    if (base_url.empty()) {
        spdlog::debug("[DebugBundle] Moonraker not connected, skipping platform files");
        return result;
    }

    spdlog::info("[DebugBundle] Collecting {} platform file(s) for platform '{}'", files.size(),
                 platform);

    for (const auto& f : files) {
        if (f.format == PlatformFileFormat::JSON) {
            json fetched = moonraker_get(base_url, f.moonraker_path);
            // Files that Moonraker can't serve come back as {"error": "..."};
            // skip 404 silently (file simply doesn't exist on this device —
            // common on non-zmod AD5M installs), keep other errors inline.
            if (fetched.is_object() && fetched.contains("error")) {
                std::string err = fetched["error"].get<std::string>();
                if (err.find("HTTP 404") != std::string::npos) {
                    spdlog::debug("[DebugBundle] platform file '{}' not present (404)", f.name);
                    continue;
                }
                result[f.name] = fetched;
                continue;
            }
            result[f.name] = sanitize_json(fetched);
        } else {
            auto raw = http_get_text(base_url, f.moonraker_path, 15);
            if (raw.status == 404) {
                spdlog::debug("[DebugBundle] platform file '{}' not present (404)", f.name);
                continue;
            }
            if (raw.status < 200 || raw.status >= 300) {
                result[f.name] = json{
                    {"error", "HTTP " + std::to_string(raw.status) + " from " + f.moonraker_path}};
                continue;
            }
            // Line-by-line sanitize so the multi-line file body doesn't trip
            // sanitize_value()'s 4 KB ReDoS guard (which would redact the
            // whole file as [REDACTED_LONG_VALUE]).
            result[f.name] = sanitize_text_block(raw.body);
        }
    }

    return result;
}

// =============================================================================
// printer.cfg + its [include] tree
// =============================================================================

std::vector<std::string> DebugBundleCollector::parse_include_patterns(const std::string& body) {
    std::vector<std::string> patterns;
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip a trailing CR so CRLF configs parse (Klipper accepts them).
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Klipper section headers must start at column 0; a leading space makes
        // the line a continuation of the previous option, not a new section.
        // Comments (# or ;) are not section headers either.
        if (line.compare(0, 9, "[include ") != 0) {
            continue;
        }
        const size_t close = line.find(']', 9);
        if (close == std::string::npos) {
            continue;
        }
        std::string pattern = line.substr(9, close - 9);
        // Trim surrounding whitespace: "[include  foo.cfg ]" is valid.
        const size_t first = pattern.find_first_not_of(" \t");
        const size_t last = pattern.find_last_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        patterns.push_back(pattern.substr(first, last - first + 1));
    }
    return patterns;
}

bool DebugBundleCollector::glob_match(const std::string& pattern, const std::string& path) {
    // Iterative wildcard match with backtracking. '*' and '?' do not cross '/',
    // matching Python's glob (which is what Klipper's configfile.py uses), so
    // "mod/*.cfg" does not reach into "mod/sub/".
    size_t p = 0, s = 0;
    size_t star = std::string::npos; // last '*' in the pattern
    size_t star_s = 0;               // where that '*' started consuming
    while (s < path.size()) {
        const bool lit_match =
            p < pattern.size() && (pattern[p] == '?' ? path[s] != '/' : pattern[p] == path[s]);
        if (lit_match) {
            ++p;
            ++s;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_s = s;
        } else if (star != std::string::npos && path[star_s] != '/') {
            // Give the '*' one more character, unless that character is a
            // separator it is not allowed to swallow.
            p = star + 1;
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

std::vector<std::string>
DebugBundleCollector::resolve_include_pattern(const std::string& pattern,
                                              const std::string& including_file,
                                              const std::vector<std::string>& available) {
    // Klipper resolves an include relative to the directory of the file that
    // contains it, so an include inside "mod/a.cfg" of "b.cfg" means
    // "mod/b.cfg".
    std::string base;
    const size_t slash = including_file.find_last_of('/');
    if (slash != std::string::npos) {
        base = including_file.substr(0, slash + 1);
    }
    std::string full = (!pattern.empty() && pattern.front() == '/') ? pattern : base + pattern;

    // Collapse a leading "./" so "./foo.cfg" matches the listing's "foo.cfg".
    if (full.compare(0, 2, "./") == 0) {
        full = full.substr(2);
    }

    std::vector<std::string> matches;
    for (const auto& path : available) {
        if (glob_match(full, path)) {
            matches.push_back(path);
        }
    }
    return matches;
}

json DebugBundleCollector::walk_include_tree(const std::string& root,
                                             const std::vector<std::string>& available,
                                             const ConfigFetcher& fetch, std::string* truncated_out,
                                             size_t* bytes_out) {
    json files = json::object();
    size_t total_bytes = 0;
    std::string truncate_reason;

    // Breadth-first over the include tree, deduped: a config included from two
    // places is fetched once, and an include cycle terminates.
    std::vector<std::string> queue{root};
    std::set<std::string> seen{root};

    for (size_t i = 0; i < queue.size(); ++i) {
        // By value, not by reference: the include loop below pushes onto `queue`,
        // and the reallocation that follows would leave a reference to queue[i]
        // dangling for every pattern after the first match.
        const std::string path = queue[i];

        if (files.size() >= MAX_CONFIG_FILES) {
            truncate_reason = "file count";
            break;
        }
        if (total_bytes >= MAX_CONFIG_BYTES) {
            truncate_reason = "byte budget";
            break;
        }

        const ConfigFetchResult raw = fetch(path);
        if (raw.status == 404) {
            // A stale [include] of a deleted file is a Klipper startup error,
            // not our problem to report; note it and move on.
            spdlog::debug("[DebugBundle] config '{}' not present (404)", path);
            continue;
        }
        if (raw.status < 200 || raw.status >= 300) {
            files[path] = json{{"error", "HTTP " + std::to_string(raw.status)}};
            continue;
        }

        total_bytes += raw.body.size();
        // Per-LINE sanitize: sanitize_value() replaces any string over 4 KB
        // with [REDACTED_LONG_VALUE], so handing it a whole config would redact
        // the entire file. Line granularity still catches the things that
        // actually appear in a printer.cfg - notification macros carrying
        // Pushover/Telegram tokens, camera and Spoolman URLs with embedded
        // credentials, [include] paths carrying a home-directory username.
        files[path] = sanitize_text_block(raw.body);

        for (const auto& pattern : parse_include_patterns(raw.body)) {
            for (const auto& match : resolve_include_pattern(pattern, path, available)) {
                if (seen.insert(match).second) {
                    queue.push_back(match);
                }
            }
        }
    }

    if (truncated_out)
        *truncated_out = truncate_reason;
    if (bytes_out)
        *bytes_out = total_bytes;
    return files;
}

json DebugBundleCollector::collect_printer_config() {
    json result = json::object();
    const std::string base_url = get_moonraker_url();
    if (base_url.empty()) {
        spdlog::debug("[DebugBundle] Moonraker not connected, skipping printer config");
        return result;
    }

    // The config-root listing is what turns a glob include into filenames. If
    // it fails we can still ship printer.cfg itself, just without its tree.
    std::vector<std::string> available;
    try {
        auto listing = moonraker_get(base_url, "/server/files/list?root=config");
        if (listing.contains("result") && listing["result"].is_array()) {
            for (const auto& entry : listing["result"]) {
                if (entry.is_object() && entry.contains("path") && entry["path"].is_string()) {
                    available.push_back(entry["path"].get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] config listing failed: {}", e.what());
    }

    size_t total_bytes = 0;
    std::string truncate_reason;
    json files = walk_include_tree(
        "printer.cfg", available,
        [&base_url](const std::string& path) {
            auto raw = http_get_text(base_url, "/server/files/config/" + path, 15);
            return ConfigFetchResult{raw.status, std::move(raw.body)};
        },
        &truncate_reason, &total_bytes);

    result["files"] = files;
    result["bytes"] = total_bytes;
    if (!truncate_reason.empty()) {
        result["truncated"] = truncate_reason;
        spdlog::info("[DebugBundle] printer config capture truncated ({}) after {} file(s)",
                     truncate_reason, files.size());
    }
    spdlog::info("[DebugBundle] Collected {} config file(s), {} bytes", files.size(), total_bytes);
    return result;
}

// =============================================================================
// Klipper / Moonraker log tails (via HTTP Range for memory safety)
// =============================================================================

/// Collapse digit runs so lines that differ only by numbers share a shape:
/// "Stats 14645.9: ... sysload=0.60" and "Stats 14646.9: ... sysload=0.58"
/// both become "Stats N.N: ... sysload=N.N".
static std::string line_shape(const std::string& line) {
    std::string shape;
    shape.reserve(line.size());
    bool in_digits = false;
    for (char c : line) {
        const bool is_digit = (c >= '0' && c <= '9');
        if (is_digit) {
            if (!in_digits) {
                shape += 'N';
                in_digits = true;
            }
            continue;
        }
        in_digits = false;
        shape += c;
    }
    return shape;
}

/// Klipper's config dump markers (klippy/configfile.py, PrinterConfig::log_config).
/// The whole of printer.cfg is written between them on every start and on every
/// log rollover.
static constexpr const char* KLIPPER_CONFIG_HEADER = "===== Config file =====";
static constexpr const char* KLIPPER_CONFIG_FOOTER = "=======================";

/// Klipper's per-second runtime stats line. Used as the discriminator for an
/// orphan footer: the config dump contains no line starting with "Stats " (the
/// dump indents every continuation with a tab, and config keys are lowercase),
/// while a live log window is saturated with them. Measured on Vger1700's
/// printer.log: 0 before the footer, 3284 after.
static constexpr const char* KLIPPER_STATS_PREFIX = "Stats ";

/// Positional backstop for a lone footer, used alongside the "no Stats yet"
/// rule above. Deliberately generous: a real AD5X+ZMOD dump is 6668 lines, not
/// the ~1300 this was first written against, so a tight bound silently skips
/// the elision whenever the fetch cuts near the top of a dump. The Stats rule
/// is what actually prevents over-reach; this only caps the damage in a window
/// that somehow contains no runtime output at all.
static constexpr size_t ORPHAN_FOOTER_MAX_INDEX = 25000;

/// Drop Klipper's config dump(s) from a raw log window, in place.
///
/// Every line of printer.cfg is a distinct shape, so shape-collapse keeps all of
/// them and the dump crowds out the events the bundle was uploaded to explain.
/// It is not incidental: pressing "Restart Klipper" on HelixScreen's own Klipper
/// recovery dialog re-dumps the config, so the bundles most likely to carry a
/// shutdown are the ones most likely to have buried it.
///
/// Two shapes, both real:
///   - Paired: header and footer both in the window. Drop the span.
///   - Head-truncated: the fetch starts mid-dump, so only the footer survives.
///     This is what actually ships — all three AD5X bundles measured
///     (4QA7SZAM 84%, LYGVE39Y 63%, XSNN7PX5 58% of the payload) look like this,
///     and a paired-only rule would have recovered nothing from any of them.
///
/// A header with no footer is left alone: the dump runs past the end of the
/// window, and dropping to end-of-input would discard the newest lines, which is
/// where the shutdown lives.
static void strip_klipper_config_dumps(std::vector<std::string>& lines) {
    std::vector<std::string> out;
    out.reserve(lines.size());

    size_t header_at = std::string::npos; // index of an open, unterminated header
    size_t dumps_closed = 0;
    bool runtime_output_seen = false; // a "Stats " line means we are past any dump
    auto note_elision = [&out](size_t count) {
        if (count == 0)
            return;
        out.push_back("[helix] elided " + std::to_string(count) + " lines of Klipper config dump");
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];

        if (header_at != std::string::npos) {
            if (line == KLIPPER_CONFIG_FOOTER) {
                note_elision(i - header_at + 1);
                header_at = std::string::npos;
                ++dumps_closed;
            }
            continue; // inside the dump: header, body, and footer all go
        }

        if (line == KLIPPER_CONFIG_HEADER) {
            header_at = i;
            continue;
        }

        // Orphan footer: the byte-range fetch cut the header off mid-dump, so
        // everything before it is config body and goes with it. Three conditions
        // keep that from reaching across real content:
        //   - no dump has closed yet, so this stays a head-of-window rule;
        //   - no runtime "Stats " line has been seen, which is what actually
        //     distinguishes a cut-off dump from a stray rule line in a live log;
        //   - a generous positional backstop for a window with no runtime output.
        if (line == KLIPPER_CONFIG_FOOTER && dumps_closed == 0 && !runtime_output_seen &&
            i <= ORPHAN_FOOTER_MAX_INDEX) {
            out.clear();
            note_elision(i + 1);
            ++dumps_closed;
            continue;
        }

        if (!runtime_output_seen && line.rfind(KLIPPER_STATS_PREFIX, 0) == 0) {
            runtime_output_seen = true;
        }
        out.push_back(line);
    }

    // Header with no footer: the dump runs past the end of the window. Ship its
    // body rather than drop the newest lines, which is where the shutdown lives.
    if (header_at != std::string::npos) {
        out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(header_at), lines.end());
    }

    lines = std::move(out);
}

std::string DebugBundleCollector::condense_klipper_log(const std::string& raw, int max_repeats,
                                                       int tail_lines) {
    std::vector<std::string> lines;
    {
        std::istringstream stream(raw);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(std::move(line));
        }
    }
    if (lines.empty()) {
        return {};
    }

    // Before shape-collapse: the dump is pure unique shapes, so it survives the
    // collapse intact and would spend the whole line budget on printer.cfg.
    strip_klipper_config_dumps(lines);
    if (lines.empty()) {
        return {};
    }

    const size_t keep_repeats = max_repeats < 0 ? 0 : static_cast<size_t>(max_repeats);
    const size_t keep_tail =
        std::min(lines.size(), tail_lines < 0 ? size_t{0} : static_cast<size_t>(tail_lines));
    const size_t cut = lines.size() - keep_tail; // [cut, end) ships verbatim

    // Pass 1: how often does each shape occur outside the verbatim tail?
    std::unordered_map<std::string, size_t> shape_total;
    for (size_t i = 0; i < cut; ++i) {
        shape_total[line_shape(lines[i])]++;
    }

    // Pass 2: for a shape that recurs more than keep_repeats times, keep only its
    // LAST keep_repeats occurrences — in place, so ordering is never disturbed.
    // Recent repeats beat old ones: the values near the failure are the ones
    // worth reading.
    std::unordered_map<std::string, size_t> shape_seen;
    std::string out;
    auto emit = [&out](const std::string& s) {
        if (!out.empty())
            out += '\n';
        out += s;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        if (i >= cut) { // verbatim tail: the shutdown dump lives here
            emit(lines[i]);
            continue;
        }
        const std::string shape = line_shape(lines[i]);
        const size_t total = shape_total[shape];
        if (total <= keep_repeats) {
            emit(lines[i]); // rare enough to be interesting on its own
            continue;
        }
        if (++shape_seen[shape] > total - keep_repeats) {
            emit(lines[i]);
        }
    }

    return out;
}

std::string DebugBundleCollector::fetch_log_tail(const std::string& base_url,
                                                 const std::string& endpoint, int num_lines,
                                                 int tail_bytes, int condense_max_repeats,
                                                 int* raw_bytes_out) {
    if (raw_bytes_out)
        *raw_bytes_out = 0;
    try {
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = base_url + endpoint;
        req->timeout = 15;

        // Request only the last chunk using HTTP Range
        // "bytes=-N" means "last N bytes of the file"
        req->headers["Range"] = "bytes=-" + std::to_string(tail_bytes);

        auto resp = requests::request(req);
        if (!resp) {
            spdlog::debug("[DebugBundle] No response fetching {}", endpoint);
            return {};
        }

        int status = static_cast<int>(resp->status_code);

        // 206 = partial content (range honored), 200 = full file (range not supported)
        if (status == 200) {
            // Server returned the full file -- check size before processing
            if (resp->body.size() > 5 * 1024 * 1024) {
                spdlog::warn("[DebugBundle] {} is too large ({} bytes), skipping", endpoint,
                             resp->body.size());
                return {};
            }
        } else if (status == 416) {
            // Range not satisfiable (file smaller than requested range) — retry without Range
            spdlog::debug("[DebugBundle] 416 for {}, retrying without Range header", endpoint);
            auto retry_req = std::make_shared<HttpRequest>();
            retry_req->method = HTTP_GET;
            retry_req->url = base_url + endpoint;
            retry_req->timeout = 15;
            resp = requests::request(retry_req);
            if (!resp)
                return {};
            status = static_cast<int>(resp->status_code);
            if (status < 200 || status >= 300)
                return {};
            // Small file — no size concern, fall through to line parsing
        } else if (status != 206) {
            spdlog::debug("[DebugBundle] HTTP {} fetching {}", status, endpoint);
            return {};
        }

        std::string body = std::move(resp->body);

        // Report the fetched size before any condensing: this is what the caller
        // compares against tail_bytes to decide whether the window was actually
        // spent. Measured here rather than on the return value, which condensing
        // shrinks ~10x and which would make "the log filled its budget" never true.
        if (raw_bytes_out)
            *raw_bytes_out = static_cast<int>(body.size());

        // If we got a partial response (206), the first line is likely truncated -- drop it
        if (status == 206) {
            size_t nl = body.find('\n');
            body = (nl == std::string::npos) ? std::string{} : body.substr(nl + 1);
        }

        // Condense BEFORE the num_lines cap: the whole point is to spend the
        // line budget on events rather than on Klipper's per-second Stats spam.
        if (condense_max_repeats > 0) {
            size_t raw_bytes = body.size();
            body = condense_klipper_log(body, condense_max_repeats);
            spdlog::debug("[DebugBundle] Condensed {} to {} bytes from {}", raw_bytes, body.size(),
                          endpoint);
        }

        // Take last N lines from the response
        std::istringstream stream(body);
        std::deque<std::string> lines;
        std::string line;

        while (std::getline(stream, line)) {
            lines.push_back(std::move(line));
            if (static_cast<int>(lines.size()) > num_lines) {
                lines.pop_front();
            }
        }

        std::string joined;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0)
                joined += '\n';
            joined += lines[i];
        }

        spdlog::debug("[DebugBundle] Fetched {} lines from {} (HTTP {})", lines.size(), endpoint,
                      status);
        return sanitize_text_block(joined);
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Exception fetching {}: {}", endpoint, e.what());
        return {};
    }
}

// Both log collectors fetch through Moonraker, which is unavailable in exactly
// the case where these logs matter most: AD5X bundles TAU4PW4H / 865DXBQ7 carry
// no klipper_log and no moonraker_log because the HTTP endpoint serving them was
// refused, while both files sat on the local disk the whole time. moonraker.log
// in particular is the one artefact that would say why Moonraker stopped.
std::string DebugBundleCollector::collect_local_log_tail(const std::string& log_name, int num_lines,
                                                         int condense_max_repeats) {
    std::string host;
    uint16_t port = 7125;
    if (!helix::diag::split_host_port(get_moonraker_url(), host, port))
        return {};
    // A remote printer's logs are not on our disk, and reading a same-named file
    // here would attribute this machine's logs to the printer.
    if (!helix::is_moonraker_on_same_host(host))
        return {};

    const auto paths =
        helix::diag::candidate_log_paths(helix::diag::find_moonraker_processes(), log_name);
    if (paths.empty()) {
        spdlog::debug("[DebugBundle] No local path found for {} (no daemon argv to derive it from)",
                      log_name);
        return {};
    }

    std::string body = collect_log_tail_from_paths(paths, num_lines);
    if (body.empty())
        return {};
    spdlog::info("[DebugBundle] Read {} from local disk ({} candidate path(s))", log_name,
                 paths.size());
    // Defaults, as at the HTTP call site: the second parameter is stats_context,
    // not a line count.
    return condense_max_repeats > 0 ? condense_klipper_log(body, condense_max_repeats) : body;
}

std::string DebugBundleCollector::pick_rotated_sibling(const std::vector<LogFileEntry>& listing,
                                                       const std::vector<std::string>& stems) {
    const LogFileEntry* best = nullptr;

    for (const auto& e : listing) {
        // Root-level only. A nested "mod/init.log.1" is another component's file
        // even when the basename would match.
        if (e.path.find('/') != std::string::npos)
            continue;

        for (const auto& stem : stems) {
            // Exactly "<stem>." + a non-empty suffix. The trailing dot is what
            // keeps "printer.log" from claiming "printer.log_backup.1" or
            // "printer.logger.2", and requiring a suffix excludes the active file.
            if (e.path.size() <= stem.size() + 1)
                continue;
            if (e.path.compare(0, stem.size(), stem) != 0 || e.path[stem.size()] != '.')
                continue;

            if (best == nullptr || e.modified > best->modified)
                best = &e;
            break;
        }
    }

    return best ? best->path : std::string{};
}

std::vector<DebugBundleCollector::LogFileEntry>
DebugBundleCollector::fetch_log_listing(const std::string& base_url) {
    std::vector<LogFileEntry> out;
    if (base_url.empty())
        return out;

    try {
        auto resp = moonraker_get(base_url, "/server/files/list?root=logs");
        if (!resp.is_object() || !resp.contains("result") || !resp["result"].is_array())
            return out;

        for (const auto& item : resp["result"]) {
            if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
                continue;
            LogFileEntry e;
            e.path = item["path"].get<std::string>();
            if (item.contains("size") && item["size"].is_number())
                e.size = item["size"].get<uint64_t>();
            if (item.contains("modified") && item["modified"].is_number())
                e.modified = item["modified"].get<double>();
            out.push_back(std::move(e));
        }
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Log listing failed: {}", e.what());
    }
    return out;
}

/// Prepend the newest rotated predecessor when the active log is too short to
/// have used its byte budget.
///
/// The predicate is the point: if the active log already fills the window we
/// were willing to spend, it reaches as far back as we can afford and there is
/// nothing to gain. It is only when the active log is SHORT that the window has
/// unspent room — and a short active log is exactly the fingerprint of the case
/// that burned us, a restart or reboot after the incident. So the extra GET
/// costs nothing in the common case and fires precisely when the evidence has
/// moved next door.
///
/// `active_raw_bytes` must be the PRE-CONDENSE fetch size. Measuring
/// active_body.size() instead compares a condensed body (~340 KB) against a raw
/// budget (4 MiB), so the predicate is true for every log that ever existed and
/// the "cheap in the common case" branch becomes a second multi-MiB GET plus a
/// second full condense pass on every bundle — on the 473 MB devices this code
/// exists to serve.
std::string DebugBundleCollector::prepend_rotated_predecessor(const std::string& base_url,
                                                              const std::vector<std::string>& stems,
                                                              const std::string& active_body,
                                                              int active_raw_bytes, int tail_bytes,
                                                              int num_lines,
                                                              int condense_max_repeats) {
    const auto used = active_raw_bytes;
    if (used >= tail_bytes)
        return active_body; // window already spent; nothing older is affordable

    auto listing = fetch_log_listing(base_url);
    const std::string sibling = pick_rotated_sibling(listing, stems);
    if (sibling.empty())
        return active_body;

    const int remaining = tail_bytes - used;
    auto older = fetch_log_tail(base_url, "/server/files/logs/" + sibling, num_lines, remaining,
                                condense_max_repeats);
    if (older.empty())
        return active_body;

    spdlog::info("[DebugBundle] Active log short ({} B); prepended {} ({} B)", used, sibling,
                 older.size());
    // Older first: the two halves are contiguous in time, and a reader scanning
    // downward should move forward through the incident, not backward.
    return older + "\n[helix] ---- rotated boundary: " + sibling +
           " above, active log below ----\n" + active_body;
}

std::string DebugBundleCollector::collect_klipper_log_tail(int num_lines) {
    std::string base_url = get_moonraker_url();
    if (base_url.empty())
        return collect_local_log_tail("klippy.log", num_lines, KLIPPER_CONDENSE_MAX_REPEATS);
    // 4 MiB of raw klippy.log is ~80 minutes of Klipper's 1-Stats-line-per-second
    // output, versus ~10 minutes for the old 512 KiB tail. condense_klipper_log()
    // then strips the Stats padding, so the retained payload stays in the same
    // ballpark as before while reaching far enough back to contain the incident
    // (bundle UJCCQP6S: 615 of 635 captured lines were Stats, and the MCU
    // shutdown being investigated had scrolled off hours earlier).
    constexpr int KLIPPER_TAIL_BYTES = 4 * 1024 * 1024;
    int raw_bytes = 0;
    auto body = fetch_log_tail(base_url, "/server/files/klippy.log", num_lines, KLIPPER_TAIL_BYTES,
                               KLIPPER_CONDENSE_MAX_REPEATS, &raw_bytes);
    if (body.empty())
        return collect_local_log_tail("klippy.log", num_lines, KLIPPER_CONDENSE_MAX_REPEATS);

    // klippy.log is the fragile one. Klipper's handler rotates on a clock jump,
    // and an RTC-less printer jumps its clock on every boot — Vger1700's device
    // carried both printer.log.1970-01-01 and printer.log.2025-12-31 as proof.
    // "klippy.log" is a Moonraker alias; on AD5M/AD5X the real file is
    // printer.log, so rotations must be matched under both names.
    static const std::vector<std::string> STEMS = {"klippy.log", "printer.log"};
    return prepend_rotated_predecessor(base_url, STEMS, body, raw_bytes, KLIPPER_TAIL_BYTES,
                                       num_lines, KLIPPER_CONDENSE_MAX_REPEATS);
}

std::string DebugBundleCollector::collect_moonraker_log_tail(int num_lines) {
    // moonraker.log used to ship raw against a 512 KiB window. Both were wrong in
    // the same direction: the condenser is shape-based rather than Klipper-specific,
    // so it collapses moonraker's log_request() padding just as well (1524 KiB of a
    // real file down to 240 KiB), and once the payload shrinks a bigger fetch costs
    // little. Measured on Vger1700's logs, 512 KiB reached 7656 lines for 167 KB
    // shipped; 2 MiB reached all 23862 for 240 KB.
    //
    // This matters more than the klippy budget does. moonraker.log carries the only
    // record of the gcode queue stalling (klippy_connection.wait() pending ages) and
    // the only host-CPU trace across a shutdown (proc_stats), it timestamps in wall
    // clock rather than uptime seconds, it sees every client rather than just us,
    // and it outlives the Klipper tree: on LYGVE39Y a rollback to Klipper 12 took
    // klippy.log with it while moonraker.log kept the whole incident.
    std::string base_url = get_moonraker_url();
    if (base_url.empty())
        return collect_local_log_tail("moonraker.log", num_lines, MOONRAKER_CONDENSE_MAX_REPEATS);
    int raw_bytes = 0;
    auto body = fetch_log_tail(base_url, "/server/files/moonraker.log", num_lines,
                               MOONRAKER_TAIL_BYTES, MOONRAKER_CONDENSE_MAX_REPEATS, &raw_bytes);
    if (body.empty())
        return collect_local_log_tail("moonraker.log", num_lines, MOONRAKER_CONDENSE_MAX_REPEATS);

    static const std::vector<std::string> STEMS = {"moonraker.log"};
    return prepend_rotated_predecessor(base_url, STEMS, body, raw_bytes, MOONRAKER_TAIL_BYTES,
                                       num_lines, MOONRAKER_CONDENSE_MAX_REPEATS);
}

// =============================================================================
// Crash report (human-readable, persists after crash.txt consumed)
// =============================================================================

std::string DebugBundleCollector::collect_crash_report_txt(const std::string& config_dir) {
    std::string path = config_dir + "/crash_report.txt";
    try {
        std::ifstream file(path);
        if (!file.good()) {
            return {};
        }

        std::ostringstream content;
        content << file.rdbuf();
        std::string result = content.str();

        if (!result.empty()) {
            spdlog::debug("[DebugBundle] Read crash_report.txt from {}", path);
        }
        return sanitize_text_block(result);
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Failed to read crash_report.txt: {}", e.what());
        return {};
    }
}

std::string DebugBundleCollector::collect_crash_txt(const std::string& config_dir) {
    // Raw signal-handler dump (JSON written by crash_handler::install). Present
    // when the user uploads a bundle after a crash but before next-boot
    // reporting has had a chance to rotate it into crash_1.txt.
    std::string path = config_dir + "/crash.txt";
    try {
        std::ifstream file(path);
        if (!file.good()) {
            return {};
        }

        std::ostringstream content;
        content << file.rdbuf();
        std::string result = content.str();

        if (!result.empty()) {
            spdlog::debug("[DebugBundle] Read crash.txt from {}", path);
        }
        return sanitize_text_block(result);
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Failed to read crash.txt: {}", e.what());
        return {};
    }
}

// =============================================================================
// Crash history (past crash submissions from crash_history.json)
// =============================================================================

json DebugBundleCollector::collect_crash_history(const std::string& config_dir) {
    std::string path = config_dir + "/crash_history.json";
    try {
        std::ifstream file(path);
        if (!file.good()) {
            return json::array();
        }

        json arr = json::parse(file);
        if (!arr.is_array()) {
            spdlog::warn("[DebugBundle] crash_history.json is not an array");
            return json::array();
        }

        spdlog::debug("[DebugBundle] Read {} crash history entries from {}", arr.size(), path);
        return arr;
    } catch (const json::parse_error& e) {
        spdlog::warn("[DebugBundle] Failed to parse crash_history.json: {}", e.what());
        return json::array();
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Failed to read crash_history.json: {}", e.what());
        return json::array();
    }
}

// =============================================================================
// Device ID (double-hashed for R2 cross-referencing)
// =============================================================================

std::string DebugBundleCollector::collect_device_id(const std::string& config_dir) {
    std::string path = config_dir + "/telemetry_device.json";
    try {
        std::ifstream file(path);
        if (!file.good()) {
            return {};
        }

        json data = json::parse(file);
        if (!data.contains("uuid") || !data["uuid"].is_string() || !data.contains("salt") ||
            !data["salt"].is_string()) {
            spdlog::debug("[DebugBundle] telemetry_device.json missing uuid/salt");
            return {};
        }

        std::string uuid = data["uuid"].get<std::string>();
        std::string salt = data["salt"].get<std::string>();

        // Use the same double-hash as TelemetryManager for consistency
        return TelemetryManager::hash_device_id(uuid, salt);
    } catch (const std::exception& e) {
        spdlog::debug("[DebugBundle] Failed to read device ID: {}", e.what());
        return {};
    }
}

// =============================================================================
// Log tail from explicit paths (used by tests to pin path resolution order)
// =============================================================================

std::string DebugBundleCollector::collect_log_tail_from_paths(const std::vector<std::string>& paths,
                                                              int num_lines) {
    return helix::logs::tail_file(paths, num_lines);
}

// =============================================================================
// Gzip compression
// =============================================================================

std::vector<uint8_t> DebugBundleCollector::gzip_compress(const std::string& data) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        spdlog::error("[DebugBundle] deflateInit2 failed");
        return {};
    }

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::vector<uint8_t> output;
    output.resize(deflateBound(&zs, zs.avail_in));

    zs.next_out = output.data();
    zs.avail_out = static_cast<uInt>(output.size());

    int ret = deflate(&zs, Z_FINISH);
    if (ret != Z_STREAM_END) {
        spdlog::error("[DebugBundle] deflate failed with code: {}", ret);
        deflateEnd(&zs);
        return {};
    }

    output.resize(zs.total_out);
    deflateEnd(&zs);
    return output;
}

// =============================================================================
// Async upload
// =============================================================================

void DebugBundleCollector::upload_async(const BundleOptions& options, ResultCallback callback) {
    // Read PrinterState and its subjects HERE, on the main thread, and carry the
    // result into the worker as plain data. Everything past submit() runs on the
    // slow lane, where touching either is a data race (see PrinterSnapshot).
    BundleOptions opts = options;
    opts.printer = snapshot_printer_state();

    // Large compressed upload — route through HttpExecutor::slow() (1-worker lane)
    // to avoid head-of-line blocking REST calls AND to avoid raw std::thread spawn,
    // which crashes with std::terminate on AD5M under thread exhaustion (#837, #724).
    helix::http::HttpExecutor::slow().submit([opts, callback = std::move(callback)]() {
        BundleResult result;

        try {
            spdlog::info("[DebugBundle] Collecting debug bundle...");
            json bundle = collect(opts);
            std::string json_str = bundle.dump();

            spdlog::info("[DebugBundle] Compressing {} bytes...", json_str.size());
            auto compressed = gzip_compress(json_str);

            if (compressed.empty()) {
                result.error_message = "Compression failed";
                helix::ui::queue_update([callback, result]() { callback(result); });
                return;
            }

            spdlog::info("[DebugBundle] Uploading {} bytes (compressed from {})...",
                         compressed.size(), json_str.size());

            std::string ua = std::string("HelixScreen/") + HELIX_VERSION;
            int status;
            std::string response_body;

#ifdef __ANDROID__
            // libhv is built without SSL on Android (no NDK OpenSSL), so route
            // the gzip-compressed bundle through the platform TLS stack via JNI.
            // The binary bridge avoids corrupting gzip bytes through a Java
            // String — the existing httpsPost takes String body and would
            // mangle arbitrary binary. Same pattern as update_checker and
            // crash_reporter.
            auto [s, body] = helix::android::https_post_binary(
                WORKER_URL, compressed, "application/json", "gzip", ua, INGEST_API_KEY, 30);
            status = s;
            response_body = body;
#else
            auto req = std::make_shared<HttpRequest>();
            req->method = HTTP_POST;
            req->url = WORKER_URL;
            req->timeout = 30;
            req->headers["Content-Type"] = "application/json";
            req->headers["Content-Encoding"] = "gzip";
            req->headers["User-Agent"] = ua;
            req->headers["X-API-Key"] = INGEST_API_KEY;
            req->body.assign(reinterpret_cast<const char*>(compressed.data()), compressed.size());

            auto resp = requests::request(req);
            status = resp ? static_cast<int>(resp->status_code) : 0;
            response_body = resp ? resp->body : "";
#endif

            if (status >= 200 && status < 300) {
                // Parse share_code from response
                try {
                    json resp_json = json::parse(response_body);
                    if (resp_json.contains("share_code")) {
                        result.share_code = resp_json["share_code"].get<std::string>();
                    }
                } catch (const json::parse_error&) {
                    // Response might not be JSON, but upload succeeded
                }
                result.success = true;
                spdlog::info("[DebugBundle] Upload successful (HTTP {}), share_code: {}", status,
                             result.share_code);
            } else {
                result.error_message =
                    "HTTP " + std::to_string(status) +
                    (response_body.empty() ? ": no response" : ": " + response_body.substr(0, 200));
                spdlog::warn("[DebugBundle] Upload failed: {}", result.error_message);
            }
        } catch (const std::exception& e) {
            result.error_message = std::string("Exception: ") + e.what();
            spdlog::error("[DebugBundle] Upload exception: {}", e.what());
        }

        helix::ui::queue_update([callback, result]() { callback(result); });
    });
}

} // namespace helix
