// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file update_checker.cpp
 * @brief Async update checker implementation
 *
 * SAFETY:
 * - Downloads and installs require explicit user confirmation
 * - Downloads are blocked while a print is in progress
 * - All errors are caught and logged, never thrown
 * - Network failures are gracefully handled
 * - Rate limited to avoid hammering GitHub API
 */

#include "system/update_checker.h"

#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_panel_settings.h"
#include "ui_settings_about.h"
#include "ui_timer_guard.h"
#include "ui_update_queue.h"

#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "hv/requests.h"
#include "json_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"
#include "system/helix_paths.h"
#include "system/log_path_probe.h"
#include "system/sha256_util.h"
#include "system/telemetry_manager.h"
#ifdef __ANDROID__
#include "system/http_android.h"
#endif
#include "version.h"

#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "hv/json.hpp"

// Compile-time installer filename from Makefile (-DINSTALLER_FILENAME=...)
#ifndef INSTALLER_FILENAME
#define INSTALLER_FILENAME "install.sh"
#endif

using namespace helix;

using json = nlohmann::json;

namespace {

/// GitHub API URL for latest release
constexpr const char* GITHUB_API_URL =
    "https://api.github.com/repos/prestonbrown/helixscreen/releases/latest";

/// GitHub API URL for all releases (beta channel uses this)
constexpr const char* GITHUB_RELEASES_URL =
    "https://api.github.com/repos/prestonbrown/helixscreen/releases";

/// HTTP request timeout in seconds
constexpr int HTTP_TIMEOUT_SECONDS = 30;

/// Perform an HTTP GET using the best available stack.
///
/// On Android, libhv is compiled without SSL (no NDK OpenSSL) so we route
/// through Android's Java HttpURLConnection via JNI. Everywhere else we use
/// libhv's `requests::`.
///
/// Returns {status_code, body}. A status_code of 0 means transport failure
/// (DNS, connection, TLS, JNI) and body carries a short error message.
static std::pair<int, std::string> do_http_get(const std::string& url,
                                               const std::string& accept = "") {
    const std::string ua = std::string("HelixScreen/") + HELIX_VERSION;

#ifdef __ANDROID__
    return helix::android::https_get(url, ua, accept, HTTP_TIMEOUT_SECONDS);
#else
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = HTTP_TIMEOUT_SECONDS;
    req->headers["User-Agent"] = ua;
    if (!accept.empty()) {
        req->headers["Accept"] = accept;
    }
    auto resp = requests::request(req);
    if (!resp) {
        return {0, ""};
    }
    return {resp->status_code, resp->body};
#endif
}

/// Pre-update backup paths (fallback tier, defined in app_constants.h)
using AppConstants::Update::config_backup_fallback;
using AppConstants::Update::env_backup_fallback;

/**
 * @brief Strip 'v' or 'V' prefix from version tag
 *
 * GitHub releases use "v1.2.3" format, but version comparison needs "1.2.3"
 */
std::string strip_version_prefix(const std::string& tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) {
        return tag.substr(1);
    }
    return tag;
}

/**
 * @brief Safely get string value from JSON, handling null
 */
std::string json_string_or_empty(const json& j, const std::string& key) {
    if (!j.contains(key)) {
        return "";
    }
    const auto& val = j[key];
    if (val.is_null()) {
        return "";
    }
    if (val.is_string()) {
        return val.get<std::string>();
    }
    return "";
}

/**
 * @brief Parse ReleaseInfo from a GitHub release JSON object (already parsed)
 */
bool parse_github_release(const json& j, UpdateChecker::ReleaseInfo& info, std::string& error) {
    info.tag_name = json_string_or_empty(j, "tag_name");
    info.release_notes = json_string_or_empty(j, "body");
    info.published_at = json_string_or_empty(j, "published_at");
    info.version = strip_version_prefix(info.tag_name);

    if (info.version.empty()) {
        error = "Invalid release format: missing tag_name";
        return false;
    }

    if (!helix::version::parse_version(info.version).has_value()) {
        error = "Invalid version format: " + info.tag_name;
        return false;
    }

    // Find platform-specific binary asset. The release flow uploads one
    // unversioned zip per platform ("helixscreen-<plat>.zip") alongside the
    // versioned tar.gz ("helixscreen-<plat>-v1.2.3.tar.gz"). We prefer the
    // tar.gz: pre-v0.99.31 in-app updaters call gunzip on whatever URL the
    // asset list hands them, so picking the .zip on this fallback path
    // yields "Corrupt download" for clients still on v0.99.30 or earlier
    // (mirrors the R2 manifest --include-zip gate). Once telemetry shows
    // v0.99.30 adoption is gone, this preference can flip back to zip.
    if (j.contains("assets") && j["assets"].is_array()) {
        const std::string platform_key = UpdateChecker::get_platform_key();
        const std::string zip_name = "helixscreen-" + platform_key + ".zip";
        const std::string platform_prefix = "helixscreen-" + platform_key + "-";
        spdlog::info("[UpdateChecker] Platform key: '{}', prefer prefix '{}...tar.gz' else '{}'",
                     platform_key, platform_prefix, zip_name);

        std::string zip_url;
        size_t zip_size = 0;
        for (const auto& asset : j["assets"]) {
            std::string name = asset.value("name", "");
            if (name.find(platform_prefix) == 0 && name.find(".tar.gz") != std::string::npos) {
                info.download_url = asset.value("browser_download_url", "");
                info.download_bytes = asset.value("size", static_cast<size_t>(0));
                spdlog::info("[UpdateChecker] Selected asset: {} ({} bytes)", name,
                             info.download_bytes);
                break;
            }
            if (zip_url.empty() && name == zip_name) {
                zip_url = asset.value("browser_download_url", "");
                zip_size = asset.value("size", static_cast<size_t>(0));
            }
        }
        if (info.download_url.empty() && !zip_url.empty()) {
            info.download_url = zip_url;
            info.download_bytes = zip_size;
            spdlog::info("[UpdateChecker] Selected zip asset (no tar.gz present): {} ({} bytes)",
                         zip_name, info.download_bytes);
        }
        // No fallback to arbitrary assets — wrong-platform binaries can brick devices
        if (info.download_url.empty()) {
            spdlog::warn("[UpdateChecker] No asset found for platform '{}' in release {}",
                         platform_key, info.version);
        }
    }

    return true;
}

/**
 * @brief Extract a version's section from CHANGELOG.md content
 *
 * Parses Keep a Changelog format: finds "## [version]" header and returns
 * everything until the next "## [" header or end of content.
 */
std::string extract_changelog_section(const std::string& changelog, const std::string& version) {
    // Find "## [version]" (with or without 'v' prefix)
    std::string needle_bare = "## [" + version + "]";
    std::string needle_v = "## [v" + version + "]";

    size_t start = changelog.find(needle_bare);
    if (start == std::string::npos) {
        start = changelog.find(needle_v);
    }
    if (start == std::string::npos) {
        return "";
    }

    // Skip past the header line
    size_t content_start = changelog.find('\n', start);
    if (content_start == std::string::npos) {
        return "";
    }
    content_start++; // skip the newline

    // Find the next "## [" section header
    size_t end = changelog.find("\n## [", content_start);
    if (end == std::string::npos) {
        end = changelog.size();
    }

    // Trim leading/trailing whitespace
    std::string section = changelog.substr(content_start, end - content_start);
    while (!section.empty() && (section.front() == '\n' || section.front() == '\r')) {
        section.erase(section.begin());
    }
    while (!section.empty() && (section.back() == '\n' || section.back() == '\r')) {
        section.pop_back();
    }
    return section;
}

/**
 * @brief Fetch changelog for a version from CHANGELOG.md on GitHub
 *
 * Fetches the raw CHANGELOG.md from the repo's default branch and extracts
 * the section for the given version. Best-effort: returns empty on failure.
 */
std::string fetch_changelog_for_version(const std::string& version) {
    if (version.empty())
        return "";

    std::string url =
        "https://raw.githubusercontent.com/prestonbrown/helixscreen/main/CHANGELOG.md";

    spdlog::debug("[UpdateChecker] Fetching CHANGELOG.md for v{}", version);
    auto [status, body] = do_http_get(url);

    if (status != 200) {
        spdlog::debug("[UpdateChecker] CHANGELOG.md fetch failed (HTTP {})", status);
        return "";
    }

    auto section = extract_changelog_section(body, version);
    if (section.empty()) {
        spdlog::debug("[UpdateChecker] No changelog section found for v{}", version);
    } else {
        spdlog::debug("[UpdateChecker] Got changelog for v{} ({} bytes)", version, section.size());
    }
    return section;
}

/**
 * @brief Parse ReleaseInfo from GitHub API JSON response string
 */
bool parse_github_release(const std::string& json_str, UpdateChecker::ReleaseInfo& info,
                          std::string& error) {
    try {
        auto j = json::parse(json_str);
        return parse_github_release(j, info, error);
    } catch (const json::exception& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        error = std::string("Parse error: ") + e.what();
        return false;
    }
}

/**
 * @brief Check if update is available by comparing versions
 *
 * @param current_version Current installed version
 * @param latest_version Latest release version
 * @return true if latest > current
 */
bool is_update_available(const std::string& current_version, const std::string& latest_version) {
    auto current = helix::version::parse_version(current_version);
    auto latest = helix::version::parse_version(latest_version);

    if (!current || !latest) {
        return false; // Can't determine, assume no update
    }

    return *latest > *current;
}

/**
 * @brief Resolve a system tool to an absolute path, falling back to bare name.
 *
 * Searches well-known absolute locations before falling back to the bare name
 * (which relies on $PATH). This is critical for systemd services: they run
 * with a minimal PATH that may not include /usr/bin or /bin, so bare-name
 * execvp calls for tar/cp/gunzip silently fail with exit code 127.
 *
 * @param name Tool name (e.g., "tar", "cp", "gunzip")
 * @return Full absolute path if found, bare name as fallback (relies on $PATH)
 */
constexpr const char* TOOL_SEARCH_DIRS[] = {"/usr/bin", "/bin",           "/usr/sbin",
                                            "/sbin",    "/usr/local/bin", nullptr};

/// Walk TOOL_SEARCH_DIRS for an executable `name`. Returns absolute path on
/// the first hit, empty string when nothing is found.
std::string find_tool_path(const std::string& name) {
    for (int i = 0; TOOL_SEARCH_DIRS[i]; ++i) {
        std::string path = std::string(TOOL_SEARCH_DIRS[i]) + "/" + name;
        if (access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }
    return "";
}

std::string resolve_tool(const std::string& name) {
    auto path = find_tool_path(name);
    if (!path.empty()) {
        return path;
    }
    spdlog::warn("[UpdateChecker] resolve_tool: '{}' not found in standard paths, using bare name",
                 name);
    return name; // fallback: rely on PATH
}

bool tool_available(const std::string& name) {
    return !find_tool_path(name).empty();
}

/// Populate ReleaseInfo download URLs from a per-platform manifest asset
/// object (R2 + dev manifest schema). Prefers `zip_url`/`zip_sha256`, falls
/// back to legacy `url`/`sha256` (tar.gz) only when no zip is in the manifest.
void populate_release_urls_from_manifest(const json& platform_asset,
                                         UpdateChecker::ReleaseInfo& info) {
    std::string zip_url = json_string_or_empty(platform_asset, "zip_url");
    if (!zip_url.empty()) {
        info.download_url = zip_url;
        info.sha256 = json_string_or_empty(platform_asset, "zip_sha256");
        // Manifest currently only carries `size` for the legacy tar.gz; if a
        // future schema adds `zip_size` we'll prefer it here.
        info.download_bytes = platform_asset.value("zip_size", platform_asset.value("size", 0ULL));
    } else {
        info.download_url = json_string_or_empty(platform_asset, "url");
        info.sha256 = json_string_or_empty(platform_asset, "sha256");
        info.download_bytes = platform_asset.value("size", 0ULL);
    }
}

/// Which half of update_install_suppressed() actually fired. The two causes need
/// completely different follow-up — one is a deliberate firmware opt-out, the
/// other is a machine that cannot apply the swap — and a support report often has
/// nothing but this log line to go on. Only call when installing is suppressed;
/// the check gate has one cause and names it inline.
const char* suppression_reason() {
    if (updates_externally_managed()) {
        return "firmware-managed (HELIX_DISABLE_AUTO_UPDATES is set)";
    }
    return "install tree not writable and root not obtainable";
}

/**
 * @brief Execute a command safely via fork/exec (no shell interpretation)
 *
 * Avoids command injection by bypassing the shell entirely.
 * Stdout/stderr are redirected to /dev/null.
 *
 * @param program Full path to executable (or name for PATH lookup)
 * @param args Argument list (argv[0] should be the program name)
 * @return Exit code of the child process, or -1 on fork/exec failure
 */
int safe_exec(const std::vector<std::string>& args, bool capture_stderr = false) {
    if (args.empty()) {
        return -1;
    }

    // Optionally capture stderr via pipe for error diagnostics
    int stderr_pipe[2] = {-1, -1};
    if (capture_stderr) {
        if (pipe(stderr_pipe) < 0) {
            capture_stderr = false; // fall back to /dev/null
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("[UpdateChecker] fork() failed: {}", strerror(errno));
        if (capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        return -1;
    }

    if (pid == 0) {
        // Child process — redirect stdout to /dev/null, stderr to pipe or /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            if (!capture_stderr) {
                dup2(devnull, STDERR_FILENO);
            }
            close(devnull);
        }
        if (capture_stderr) {
            close(stderr_pipe[0]); // close read end in child
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stderr_pipe[1]);
        }

        // Ensure child has a usable PATH — tools like tar and gunzip spawn
        // subprocesses (gzip, sh) that need PATH to work.  Systemd services
        // may clear PATH entirely, breaking those internal lookups.
        const char* cur_path = std::getenv("PATH");
        if (!cur_path || cur_path[0] == '\0') {
            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        }

        // Build C-style argv array
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // Use execvp for PATH lookup (e.g. gunzip on BusyBox embedded systems)
        execvp(argv[0], argv.data());
        // If execvp returns, it failed
        _exit(127);
    }

    // Parent — read stderr if capturing, then wait for child
    std::string stderr_output;
    if (capture_stderr) {
        close(stderr_pipe[1]); // close write end in parent
        char buf[1024];
        ssize_t n;
        while ((n = read(stderr_pipe[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            stderr_output.append(buf, static_cast<size_t>(n));
            if (stderr_output.size() > 4096)
                break; // cap captured output
        }
        close(stderr_pipe[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        spdlog::error("[UpdateChecker] waitpid() failed: {}", strerror(errno));
        return -1;
    }

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    // Log captured stderr on failure
    if (capture_stderr && exit_code != 0 && !stderr_output.empty()) {
        // Trim trailing whitespace
        while (!stderr_output.empty() &&
               (stderr_output.back() == '\n' || stderr_output.back() == '\r')) {
            stderr_output.pop_back();
        }
        spdlog::error("[UpdateChecker] stderr from '{}': {}", args[0], stderr_output);
    }

    return exit_code;
}

/**
 * @brief Clean up stale .old backup directory left by a previous update.
 *
 * When the in-app updater runs install.sh under systemd's NoNewPrivileges
 * constraint, the cleanup_old_install step can fail silently (sudo is blocked).
 * This leaves a helixscreen.old directory that wastes disk space.  We clean it
 * up at startup since we know the new version launched successfully.
 */
void cleanup_stale_old_install() {
    std::string install_root = app_get_install_root();
    if (install_root.empty()) {
        return;
    }

    std::string old_dir = install_root + ".old";
    struct stat st {};
    if (stat(old_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return; // no .old directory
    }

    spdlog::info("[UpdateChecker] Found stale backup from previous update: {}", old_dir);

    const std::string rm_bin = resolve_tool("rm");
    int ret = safe_exec({rm_bin, "-rf", old_dir});
    if (ret == 0) {
        spdlog::info("[UpdateChecker] Cleaned up stale backup: {}", old_dir);
    } else {
        spdlog::warn("[UpdateChecker] Could not remove stale backup {} (exit {})", old_dir, ret);
    }

    // Rolling backups in /var/lib/ and $HOME/.helixscreen/ are maintained by Config::save()
    // and Config::init() — do NOT delete them here. They protect against Moonraker
    // wiping the install directory during updates.
}

/**
 * @brief Extract a single member from a .tar.gz tarball.
 *
 * Tries GNU tar xzf first; falls back to cp+gunzip+tar for BusyBox compat.
 * The fallback avoids gunzip -k (keep-original) which is absent on older BusyBox.
 *
 * @param tarball_path  Path to the .tar.gz file
 * @param extract_dir   Directory to extract into
 * @param tar_member    Archive member path (e.g., "helixscreen/install.sh")
 * @return 0 on success, non-zero on failure
 */
int extract_tar_member(const std::string& tarball_path, const std::string& extract_dir,
                       const std::string& tar_member) {
    const std::string tar_bin = resolve_tool("tar");

    // Try GNU tar first (handles -z natively on most systems)
    auto ret = safe_exec({tar_bin, "xzf", tarball_path, "-C", extract_dir, tar_member});
    if (ret == 0) {
        return 0;
    }

    // BusyBox tar may not support the -z flag for gzip decompression.
    // Fallback: copy the tarball and decompress the copy with gunzip -f.
    // We deliberately avoid gunzip -k (keep-original) because that flag is
    // absent from older BusyBox gunzip builds (pre-1.30 era), which is exactly
    // the environment where we need this fallback to succeed.
    const std::string cp_bin = resolve_tool("cp");
    const std::string gunzip_bin = resolve_tool("gunzip");

    std::string tmp_copy = extract_dir + "/tmp_copy.tar.gz";
    if (safe_exec({cp_bin, tarball_path, tmp_copy}) == 0) {
        if (safe_exec({gunzip_bin, "-f", tmp_copy}) == 0) {
            std::string tmp_tar = extract_dir + "/tmp_copy.tar";
            ret = safe_exec({tar_bin, "xf", tmp_tar, "-C", extract_dir, tar_member});
            std::remove(tmp_tar.c_str());
        } else {
            std::remove(tmp_copy.c_str());
        }
    }
    return ret;
}

/// python3 snippet: extract argv[2] from zip argv[1] into argv[3], restoring the
/// member's unix mode bits (zipfile.extract() drops them) and forcing the exec
/// bit on bin/* and *.sh so an extracted installer or binary is runnable.
constexpr const char* PY_EXTRACT_SCRIPT =
    "import os, stat, sys, zipfile\n"
    "zip_path, member, destdir = sys.argv[1], sys.argv[2], sys.argv[3]\n"
    "try:\n"
    "    with zipfile.ZipFile(zip_path) as zf:\n"
    "        info = zf.getinfo(member)\n"
    "        zf.extract(info, destdir)\n"
    "        target = os.path.join(destdir, info.filename)\n"
    "        mode = (info.external_attr >> 16) & 0o777\n"
    "        if mode:\n"
    "            os.chmod(target, mode)\n"
    "        parts = info.filename.split('/')\n"
    "        if member.endswith('.sh') or (len(parts) > 1 and parts[0] == 'bin'):\n"
    "            st = os.stat(target).st_mode\n"
    "            os.chmod(target, st | stat.S_IXUSR)\n"
    "except Exception:\n"
    "    sys.exit(1)\n";

/// True if path ends with ".zip" (case-sensitive — we only produce lowercase).
bool path_is_zip(const std::string& path) {
    if (path.size() < 4) {
        return false;
    }
    return path.compare(path.size() - 4, 4, ".zip") == 0;
}

/// Log-and-flush macros for install diagnostics. Ensures every message reaches
/// journalctl immediately — spdlog buffers by default and a systemd cgroup kill
/// during self-update can lose all buffered output.
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define FLOG(level, ...)                                                                           \
    do {                                                                                           \
        spdlog::level(__VA_ARGS__);                                                                \
        spdlog::default_logger()->flush();                                                         \
    } while (false)
#define flog_info(...) FLOG(info, __VA_ARGS__)
#define flog_warn(...) FLOG(warn, __VA_ARGS__)
#define flog_error(...) FLOG(error, __VA_ARGS__)
#define flog_debug(...) FLOG(debug, __VA_ARGS__)
// NOLINTEND(cppcoreguidelines-macro-usage)

/// Strip ANSI escape sequences (\033[...m) for display in the UI
std::string strip_ansi_codes(const std::string& s) {
    std::string clean;
    clean.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm')
                ++i;
        } else {
            clean += s[i];
        }
    }
    return clean;
}

} // anonymous namespace

// ============================================================================
// Channel Version Comparison
// ============================================================================

// Deliberately at global scope, not in the anonymous namespace above with
// is_update_available(): this one is declared in the header and exercised
// directly by tests/unit/test_update_checker.cpp. (is_update_available() has
// internal linkage, which is why that test file carries its own copy of it.)
ChannelVersionRelation compare_channel_version(const std::string& installed,
                                               const std::string& channel_version) {
    auto current = helix::version::parse_version(installed);
    auto served = helix::version::parse_version(channel_version);

    if (!current || !served) {
        return ChannelVersionRelation::Unknown;
    }
    if (*served == *current) {
        return ChannelVersionRelation::Same;
    }
    return *served > *current ? ChannelVersionRelation::Newer : ChannelVersionRelation::Older;
}

// ============================================================================
// Singleton Instance
// ============================================================================

UpdateChecker& UpdateChecker::instance() {
    static UpdateChecker instance;
    return instance;
}

UpdateChecker::~UpdateChecker() {
    // NOTE: Don't use spdlog here - during exit(), spdlog may already be destroyed
    // which causes a crash. Just silently clean up.

    // Signal cancellation to any running threads
    cancelled_ = true;
    download_cancelled_ = true;
    shutting_down_ = true;

    // Application shutdown calls stop_auto_check() explicitly, so this only
    // matters on a path that skips it. Silent (no spdlog) and self-guarding on
    // lv_is_initialized(), which is what makes it safe from a static's destructor.
    cancel_auto_check_timer();

    // MUST join threads if joinable, regardless of status.
    // A completed check still has a joinable thread.
    // Destroying a joinable std::thread without join() calls std::terminate()!
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
}

// Forward declaration - defined before show_update_notification()
static void register_notify_callbacks();

// ============================================================================
// Lifecycle
// ============================================================================

void UpdateChecker::init() {
    if (initialized_) {
        return;
    }

    // Reset cancellation flags from any previous shutdown
    shutting_down_ = false;
    cancelled_ = false;
    download_cancelled_ = false;

    init_subjects();
    register_notify_callbacks();

    // Snapshot the Config-derived settings while we are provably on the main
    // thread, so the debug bundle can report channel/manifest URL from its
    // worker thread even when no check has ever run.
    refresh_config_snapshot();

    // Clean up stale .old backup from a previous in-app update that may have
    // failed to remove it (e.g., NoNewPrivileges blocked sudo rm).
    cleanup_stale_old_install();

    // Repair a stale/missing asset_name in release_info.json before anything can
    // ask Moonraker to update us. Self-healing is the point: an install with a
    // bad asset_name cannot receive the fix through the channel it breaks
    // (prestonbrown/helixscreen#993).
    repair_release_info(app_get_install_root());

    spdlog::debug("[UpdateChecker] Initialized");
    initialized_ = true;
}

void UpdateChecker::shutdown() {
    if (!initialized_) {
        return;
    }

    spdlog::debug("[UpdateChecker] Shutting down");

    // Stop auto-check timer
    stop_auto_check();

    // Signal cancellation
    cancelled_ = true;
    download_cancelled_ = true;
    shutting_down_ = true;

    // Wait for worker thread to finish
    if (worker_thread_.joinable()) {
        spdlog::debug("[UpdateChecker] Joining worker thread");
        worker_thread_.join();
    }

    // Wait for download thread to finish
    if (download_thread_.joinable()) {
        spdlog::debug("[UpdateChecker] Joining download thread");
        download_thread_.join();
    }

    // Clear callback to prevent stale references, and drop the diagnostics
    // snapshot — once the checker is down, a stale channel/URL would read as
    // live config in a debug bundle. init() takes a fresh one.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_callback_ = nullptr;
        config_snapshot_ = {};
    }

    // Cleanup subjects
    if (subjects_initialized_) {
        // Expire any worker-thread callback still queued on the UpdateQueue
        // before the subjects it writes are torn down (#1165, #1146).
        async_lifetime_.invalidate();
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    initialized_ = false;
    spdlog::debug("[UpdateChecker] Shutdown complete");
}

void UpdateChecker::init_subjects() {
    if (subjects_initialized_)
        return;

    UI_MANAGED_SUBJECT_INT(status_subject_, static_cast<int>(Status::Idle), "update_status",
                           subjects_);
    UI_MANAGED_SUBJECT_STRING(version_text_subject_, version_text_buf_, "", "update_version_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(new_version_subject_, new_version_buf_, "", "update_new_version",
                              subjects_);

    // Download subjects
    UI_MANAGED_SUBJECT_INT(download_status_subject_, static_cast<int>(DownloadStatus::Idle),
                           "download_status", subjects_);
    UI_MANAGED_SUBJECT_INT(download_progress_subject_, 0, "download_progress", subjects_);
    UI_MANAGED_SUBJECT_STRING(download_text_subject_, download_text_buf_, "", "download_text",
                              subjects_);

    // Notification subjects
    UI_MANAGED_SUBJECT_STRING(release_notes_subject_, release_notes_buf_, "",
                              "update_release_notes", subjects_);
    UI_MANAGED_SUBJECT_INT(changelog_visible_subject_, 0, "update_changelog_visible", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[UpdateChecker] LVGL subjects initialized");
}

// ============================================================================
// Subject Accessors
// ============================================================================

lv_subject_t* UpdateChecker::status_subject() {
    return &status_subject_;
}
lv_subject_t* UpdateChecker::version_text_subject() {
    return &version_text_subject_;
}
lv_subject_t* UpdateChecker::new_version_subject() {
    return &new_version_subject_;
}
lv_subject_t* UpdateChecker::download_status_subject() {
    return &download_status_subject_;
}
lv_subject_t* UpdateChecker::download_progress_subject() {
    return &download_progress_subject_;
}
lv_subject_t* UpdateChecker::download_text_subject() {
    return &download_text_subject_;
}
lv_subject_t* UpdateChecker::release_notes_subject() {
    return &release_notes_subject_;
}
lv_subject_t* UpdateChecker::changelog_visible_subject() {
    return &changelog_visible_subject_;
}

// ============================================================================
// Download Getters
// ============================================================================

UpdateChecker::DownloadStatus UpdateChecker::get_download_status() const {
    return download_status_.load();
}

int UpdateChecker::get_download_progress() const {
    return download_progress_.load();
}

std::string UpdateChecker::get_download_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return download_error_;
}

// Safety floor: even if the remote claims a tiny size, never accept a
// candidate dir below this. Small enough to allow CI fixtures to pass while
// large enough that a near-full embedded device doesn't slip through.
static constexpr uint64_t DOWNLOAD_SPACE_FLOOR_BYTES = 50ULL * 1024 * 1024;

// Used when the caller doesn't know the remote size (manifest schemas that
// omit size/zip_size, or any parse failure). Sized to cover the largest
// current platform tarball (~75 MB on x86) with ~1.6× safety margin without
// over-blocking users on tight rootfs. Bump in lockstep if archives grow.
static constexpr uint64_t DOWNLOAD_SPACE_DEFAULT_BYTES = 120ULL * 1024 * 1024;

// The filename used to stage the downloaded archive in TMP_DIR. The name
// matches whatever format the release URL points at: zips keep a .zip
// extension so downstream extract_installer_from_tarball() can dispatch on
// the path. get_download_path() always returns the legacy tar.gz name for
// back-compat with external callers; do_download() rewrites the path when
// the URL is a .zip (see below).
static const char* const DOWNLOAD_FILENAME = "helixscreen-update.tar.gz";
static const char* const DOWNLOAD_FILENAME_ZIP = "helixscreen-update.zip";

uint64_t UpdateChecker::required_download_space_bytes(uint64_t download_bytes) {
    // 20% headroom over the wire size + a small fixed buffer for the .partial
    // tail and filesystem overhead. install.sh runs its own ≥100 MB check
    // before extracting, so we don't need to oversize the staging here.
    constexpr uint64_t FIXED_BUFFER = 10ULL * 1024 * 1024;
    if (download_bytes == 0) {
        return DOWNLOAD_SPACE_DEFAULT_BYTES;
    }
    uint64_t need = (download_bytes * 6 / 5) + FIXED_BUFFER;
    return need < DOWNLOAD_SPACE_FLOOR_BYTES ? DOWNLOAD_SPACE_FLOOR_BYTES : need;
}

namespace {

// True if `child` is within-or-equal-to `parent` (both already stripped of
// trailing slashes): child == parent, or child starts with parent + "/".
bool is_within_or_equal(const std::string& child, const std::string& parent) {
    if (parent.empty()) {
        return false;
    }
    if (child == parent) {
        return true;
    }
    return child.rfind(parent + "/", 0) == 0;
}

} // namespace

std::string UpdateChecker::compute_update_staging_dir(const std::string& tarball_path,
                                                      const std::string& install_root) {
    std::string base = helix::paths::dirname(tarball_path);

    // SAFETY: the staging dir must live OUTSIDE install_root. TMP_DIR is
    // rm -rf'd on installer cleanup, and the installer's --update flow does
    // dotfile `rm -rf` inside INSTALL_DIR plus `mv INSTALL_DIR ...` on the
    // atomic-swap path — a staging dir under INSTALL_DIR would be deleted or
    // relocated out from under the extracted new tree. When the download dir
    // sits within-or-equal-to the install root (the common self-update case
    // where both are e.g. /home/pi/helixscreen), relocate the base to the
    // install root's PARENT — a sibling of the install dir on the same
    // partition. The ancestor case (base is already a parent of install_root)
    // is left untouched: it's already a sibling location.
    const std::string norm_root = helix::paths::strip_trailing_slash(install_root);
    const std::string norm_base = helix::paths::strip_trailing_slash(base);
    if (!norm_root.empty() && is_within_or_equal(norm_base, norm_root)) {
        base = helix::paths::dirname(norm_root);
        if (base.empty()) {
            base = ".";
        }
    }

    // ALWAYS a dot-prefixed subdir — never the bare dir. TMP_DIR is rm -rf'd on
    // installer cleanup; handing it a bare mount/install dir would wipe live
    // data (past incident wiped a device partition passed as TMP_DIR).
    static constexpr const char* STAGING_NAME = ".helix-update-staging";
    if (base == "/") {
        return std::string("/") + STAGING_NAME;
    }
    return base + "/" + STAGING_NAME;
}

std::string UpdateChecker::get_download_path(DownloadPathDiag* diag,
                                             uint64_t threshold_bytes) const {
    if (threshold_bytes == 0) {
        threshold_bytes = DOWNLOAD_SPACE_DEFAULT_BYTES;
    } else if (threshold_bytes < DOWNLOAD_SPACE_FLOOR_BYTES) {
        threshold_bytes = DOWNLOAD_SPACE_FLOOR_BYTES;
    }
    // Candidate directories, checked exhaustively — we pick the one with
    // the MOST free space so we don't fill up a tiny tmpfs or crowd out
    // gcode storage on an embedded device.
    std::vector<std::string> candidates;

    // Install root partition — by definition writable (the running binary
    // lives there) and on most embedded devices it sits on the largest
    // dedicated user partition, e.g. CC1 /user-resource (~6 GB),
    // Snapmaker U1 /opt/lava, K1/K2 /usr/data. The standard temp/data
    // fallbacks below routinely miss this because they assume FHS layout.
    // We add both the install dir and its parent — both yield the same
    // f_bavail (same filesystem) but the parent helps when an installer
    // tightens permissions on the install dir itself.
    const std::string install_root = app_get_install_root();
    if (!install_root.empty()) {
        candidates.emplace_back(install_root);
        const auto last_slash = install_root.find_last_of('/');
        if (last_slash != std::string::npos && last_slash > 0) {
            candidates.emplace_back(install_root.substr(0, last_slash));
        }
    }

    // Environment variables next
    for (const char* env_name : {"TMPDIR", "TMP", "TEMP"}) {
        const char* val = std::getenv(env_name);
        if (val != nullptr && val[0] != '\0') {
            candidates.emplace_back(val);
        }
    }

    // Home directory
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        candidates.emplace_back(home);
    }

    // Standard temp locations
    candidates.emplace_back("/tmp");
    candidates.emplace_back("/var/tmp");
    candidates.emplace_back("/mnt/tmp");

    // Persistent storage (embedded devices often have more room here)
    candidates.emplace_back("/data");
    candidates.emplace_back("/mnt/data");
    candidates.emplace_back("/usr/data");

    // Home variants (embedded devices with root user)
    candidates.emplace_back("/root");
    candidates.emplace_back("/home/root");

    // Evaluate all candidates — pick the one with the most free space.
    // Track best across all writable dirs (even those below threshold) so we
    // can produce an actionable error message when no candidate qualifies.
    std::string best_dir; // best dir meeting threshold (used for return)
    uint64_t best_space = 0;
    std::string best_dir_overall; // best writable dir regardless of threshold (for diag)
    uint64_t best_space_overall = 0;

    for (const auto& dir : candidates) {
        if (!helix::paths::is_writable_dir(dir)) {
            spdlog::debug("[UpdateChecker] Skipping {} (not writable)", dir);
            continue;
        }

        auto space = helix::paths::available_space(dir);

        if (space > best_space_overall) {
            best_space_overall = space;
            best_dir_overall = dir;
        }

        if (space < threshold_bytes) {
            // Promoted to info — this is the only signal we have when an
            // update fails for disk-space reasons. Worth the small log cost.
            spdlog::info("[UpdateChecker] Skipping {} ({:.1f} MB free, need {:.0f} MB)", dir,
                         static_cast<double>(space) / (1024.0 * 1024.0),
                         static_cast<double>(threshold_bytes) / (1024.0 * 1024.0));
            continue;
        }

        if (space > best_space) {
            best_space = space;
            best_dir = dir;
        }
    }

    if (diag != nullptr) {
        diag->best_dir = best_dir_overall;
        diag->best_free_bytes = best_space_overall;
        diag->threshold_bytes = threshold_bytes;
    }

    if (best_dir.empty()) {
        spdlog::error("[UpdateChecker] No writable directory with {} MB free space "
                      "(best candidate: {} with {:.1f} MB)",
                      threshold_bytes / (1024 * 1024),
                      best_dir_overall.empty() ? "<none>" : best_dir_overall,
                      static_cast<double>(best_space_overall) / (1024.0 * 1024.0));
        return {}; // Caller must handle empty path
    }

    spdlog::info("[UpdateChecker] Download directory: {} ({:.0f} MB free)", best_dir,
                 static_cast<double>(best_space) / (1024.0 * 1024.0));

    // Ensure trailing slash
    if (best_dir.back() != '/') {
        best_dir += '/';
    }
    return best_dir + DOWNLOAD_FILENAME;
}

std::string UpdateChecker::platform_asset_name() {
    // Unversioned zip matches the release.yml upload layout and the name
    // Moonraker Update Manager looks up via release_info.json's asset_name.
    //
    // Static and single-sourced on purpose. #993 was caused by this name being
    // encoded in three places that drifted; repair_release_info() must compare
    // against the same expression the rest of the code means by "our asset",
    // not a copy of it.
    return "helixscreen-" + get_platform_key() + ".zip";
}

std::string UpdateChecker::get_platform_asset_name() const {
    return platform_asset_name();
}

UpdateChecker::ReleaseInfoRepair
UpdateChecker::repair_release_info(const std::string& install_root) {
    if (install_root.empty()) {
        return ReleaseInfoRepair::Absent;
    }

    const std::string path = install_root + "/release_info.json";
    const std::string want = platform_asset_name();

    // Parse defensively: this is on-disk, user-facing JSON that may be absent,
    // empty, truncated, or not an object.
    json existing = json::object();
    bool have_file = false;
    {
        std::ifstream in(path);
        if (in.is_open()) {
            have_file = true;
            json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded()) {
                spdlog::info("[UpdateChecker] {} is not valid JSON — rewriting", path);
            } else if (!parsed.is_object()) {
                spdlog::info("[UpdateChecker] {} is not a JSON object — rewriting", path);
            } else {
                existing = std::move(parsed);
            }
        }
    }

    if (!have_file) {
        // A dev build resolves its install root to the source checkout (the
        // resolver accepts .../build/bin too), so creating the file whenever it
        // is missing would drop an untracked release_info.json into the repo.
        // Only a deployed layout — binary directly under <root>/bin — gets one.
        const std::string deployed_bin = install_root + "/bin/helix-screen";
        struct stat st {};
        if (stat(deployed_bin.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            spdlog::debug("[UpdateChecker] No release_info.json at {} and no deployed "
                          "install layout — nothing to repair",
                          path);
            return ReleaseInfoRepair::Absent;
        }
    }

    std::string current;
    if (auto it = existing.find("asset_name"); it != existing.end() && it->is_string()) {
        current = it->get<std::string>();
    }
    if (current == want) {
        // Already correct — do not touch the file. No boot-time disk churn, and
        // it keeps the repair log line below meaningful when it does appear.
        spdlog::debug("[UpdateChecker] release_info.json asset_name is correct ({})", want);
        return ReleaseInfoRepair::NotNeeded;
    }

    // Preserve every other key (Moonraker tolerates extras), replacing only the
    // fields that are missing or unusable.
    auto sane_field = [&existing](const char* key, const std::string& fallback) {
        const std::string v = helix::json_util::safe_string(existing, key, fallback);
        return v.empty() ? fallback : v;
    };
    json repaired = existing;
    repaired["project_name"] = sane_field("project_name", "helixscreen");
    repaired["project_owner"] = sane_field("project_owner", "prestonbrown");
    repaired["version"] = sane_field("version", std::string("v") + HELIX_VERSION);
    repaired["asset_name"] = want;

    spdlog::info("[UpdateChecker] Repairing release_info.json asset_name: '{}' -> '{}' ({})",
                 current.empty() ? "<missing>" : current, want, path);

    // Resolve symlinks BEFORE renaming. The installer symlinks install-dir files
    // out to printer_data, and rename(2) onto a symlink replaces the symlink
    // itself rather than writing through it (prestonbrown/helixscreen#1176).
    std::string target_path = path;
    {
        std::error_code ec;
        if (std::filesystem::is_symlink(path, ec)) {
            auto real = std::filesystem::canonical(path, ec);
            if (!ec) {
                spdlog::debug("[UpdateChecker] Resolved symlink {} -> {}", path, real.string());
                target_path = real.string();
            }
        }
    }

    const std::string tmp_path = target_path + ".tmp";
    {
        std::ofstream o(tmp_path);
        if (!o.is_open()) {
            // Read-only rootfs or a root-owned install dir. Never fatal: the app
            // boots fine, self-update just stays broken until the installer runs.
            spdlog::warn("[UpdateChecker] Cannot repair release_info.json — open {} failed: {}",
                         tmp_path, strerror(errno));
            return ReleaseInfoRepair::Failed;
        }
        o << repaired.dump() << std::endl;
        o.flush();
        if (!o.good()) {
            spdlog::warn("[UpdateChecker] Cannot repair release_info.json — write {} failed: {}",
                         tmp_path, strerror(errno));
            o.close();
            std::remove(tmp_path.c_str());
            return ReleaseInfoRepair::Failed;
        }
    }

    // fsync the temp file before the rename, and the parent dir after, so a
    // power cut on flash-backed storage cannot leave a zero-length file (#943).
    {
        int fd = ::open(tmp_path.c_str(), O_RDONLY);
        if (fd >= 0) {
            (void)::fsync(fd);
            ::close(fd);
        }
    }

    if (std::rename(tmp_path.c_str(), target_path.c_str()) != 0) {
        spdlog::warn("[UpdateChecker] Cannot repair release_info.json — rename {} -> {} failed: {}",
                     tmp_path, target_path, strerror(errno));
        std::remove(tmp_path.c_str());
        return ReleaseInfoRepair::Failed;
    }

    {
        const std::string dir = std::filesystem::path(target_path).parent_path().string();
        if (!dir.empty()) {
            int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) {
                (void)::fsync(dfd);
                ::close(dfd);
            }
        }
    }

    spdlog::info("[UpdateChecker] Repaired release_info.json at {}", target_path);
    return ReleaseInfoRepair::Repaired;
}

void UpdateChecker::report_download_status(DownloadStatus status, int progress,
                                           const std::string& text, const std::string& error) {
    if (shutting_down_.load())
        return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        download_status_ = status;
        download_progress_ = progress;
        download_error_ = error;
    }

    async_lifetime_.defer("UpdateChecker::set_download_status", [this, status, progress, text]() {
        if (subjects_initialized_) {
            lv_subject_set_int(&download_status_subject_, static_cast<int>(status));
            lv_subject_set_int(&download_progress_subject_, progress);
            lv_subject_copy_string(&download_text_subject_, text.c_str());
        }
    });
}

// ============================================================================
// Download and Install
// ============================================================================

void UpdateChecker::start_download() {
    if (shutting_down_.load())
        return;

    // Firmware-managed devices own updates via their own package pipeline, and a
    // read-only / non-writable install tree can't be swapped at all. Never
    // self-download/install in either case — it would fight the firmware's setup
    // or fail the atomic directory rename.
    if (update_install_suppressed()) {
        spdlog::info("[UpdateChecker] Download skipped: installing is suppressed - {}",
                     suppression_reason());
        return;
    }

    // Safety: refuse download while a job owns the machine. Preparing counts —
    // a user who just committed to a print should not have the CPU and network
    // pulled out from under the pre-start block.
    const auto lifecycle = static_cast<PrintState>(
        lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
    if (job_holds_machine(lifecycle)) {
        spdlog::warn("[UpdateChecker] Cannot download update while printing");
        report_download_status(DownloadStatus::Error, 0,
                               lv_tr("Error: Cannot update while printing"),
                               "Stop the print before installing updates");
        TelemetryManager::instance().record_update_failure("print_in_progress", "",
                                                           get_platform_key());
        return;
    }

    // Must have a cached update to download
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cached_info_ || cached_info_->download_url.empty()) {
        spdlog::error("[UpdateChecker] start_download() called without cached update info");
        // Unlock before report_download_status (it also acquires mutex_)
        lock.unlock();
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: No update available"),
                               "No update information cached");
        TelemetryManager::instance().record_update_failure("no_cached_update", "",
                                                           get_platform_key());
        return;
    }

    // Don't start if already downloading
    auto current = download_status_.load();
    if (current == DownloadStatus::Downloading || current == DownloadStatus::Installing) {
        spdlog::warn("[UpdateChecker] Download already in progress");
        return;
    }

    // Join previous download thread (must release lock first to prevent deadlock)
    lock.unlock();
    if (download_thread_.joinable()) {
        download_thread_.join();
    }

    download_cancelled_ = false;
    report_download_status(DownloadStatus::Downloading, 0, lv_tr("Starting download..."));

    // Wrap — pthread_create EAGAIN under thread exhaustion throws
    // std::system_error; if this is invoked from a UI event-cb frame the
    // throw aborts via std::terminate ([L083]).
    try {
        download_thread_ = std::thread(&UpdateChecker::do_download, this);
    } catch (const std::system_error& e) {
        spdlog::error("[UpdateChecker] Failed to spawn download thread: {}", e.what());
        report_download_status(DownloadStatus::Error, 0, lv_tr("System busy — try again"));
    }
}

void UpdateChecker::cancel_download() {
    download_cancelled_ = true;
}

void UpdateChecker::do_download() {
    std::string url;
    std::string version;
    size_t download_bytes = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cached_info_)
            return;
        url = cached_info_->download_url;
        version = cached_info_->version;
        download_bytes = cached_info_->download_bytes;
    }

    // Installing a zip release needs *some* way to read a zip. `unzip` is the
    // usual one, but the K2's OpenWrt firmware ships no unzip binary and no
    // BusyBox unzip applet — only python3, which can do the job. Fail solely
    // when neither exists, rather than demanding unzip specifically.
    if (path_is_zip(url) && available_zip_tool() == ZipTool::None) {
        spdlog::error("[UpdateChecker] Neither `unzip` nor a python3 with zipfile+zlib is "
                      "available — cannot install zip releases on this system");
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: `unzip` not installed"),
                               "Install unzip (or a python3 with zlib), then retry the update");
        TelemetryManager::instance().record_update_failure("missing_unzip", version,
                                                           get_platform_key());
        return;
    }

    DownloadPathDiag diag;
    auto download_path = get_download_path(&diag, required_download_space_bytes(download_bytes));
    if (download_path.empty()) {
        // Build an actionable subtitle that names the dir we tried and the
        // shortfall, so users can see *where* to free up space rather than
        // staring at a generic message.
        const auto need_mb = diag.threshold_bytes / (1024 * 1024);
        std::string detail;
        if (diag.best_dir.empty()) {
            detail = fmt::format("No writable directory found. Need {} MB free.", need_mb);
        } else {
            const auto have_mb = static_cast<double>(diag.best_free_bytes) / (1024.0 * 1024.0);
            detail = fmt::format("Need {} MB free; {} has {:.0f} MB. Free up disk space and retry.",
                                 need_mb, diag.best_dir, have_mb);
        }
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Not enough disk space"),
                               detail);
        TelemetryManager::instance().record_update_failure("no_disk_space", version,
                                                           get_platform_key());
        return;
    }

    // Rewrite the staged filename to match the URL's archive format. The
    // extraction path (extract_installer_from_tarball) dispatches on extension,
    // so a .zip URL must land in a .zip file — not the legacy .tar.gz name.
    if (path_is_zip(url)) {
        auto slash = download_path.find_last_of('/');
        std::string dir = (slash == std::string::npos) ? "" : download_path.substr(0, slash + 1);
        download_path = dir + DOWNLOAD_FILENAME_ZIP;
    }

    spdlog::info("[UpdateChecker] Downloading {} to {}", url, download_path);

    // Progress callback -- dispatches to LVGL thread
    auto progress_cb = [this](size_t received, size_t total) {
        if (download_cancelled_.load())
            return;

        int percent = 0;
        if (total > 0) {
            percent = static_cast<int>((100 * received) / total);
        }

        // Throttle UI updates to every 2%
        int current = download_progress_.load();
        if (percent - current >= 2 || percent == 100) {
            auto mb_received = static_cast<double>(received) / (1024.0 * 1024.0);
            auto mb_total = static_cast<double>(total) / (1024.0 * 1024.0);
            auto text =
                fmt::format(lv_tr("Downloading... {:.1f}/{:.1f} MB"), mb_received, mb_total);
            report_download_status(DownloadStatus::Downloading, percent, text);
        }
    };

    // Download the file using libhv
    size_t result = requests::downloadFile(url.c_str(), download_path.c_str(), progress_cb);

    if (download_cancelled_.load()) {
        spdlog::info("[UpdateChecker] Download cancelled");
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Idle, 0, "");
        return;
    }

    if (result == 0) {
        spdlog::error("[UpdateChecker] Download failed from {}", url);
        std::remove(download_path.c_str()); // Clean up partial download
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Download failed"),
                               "Failed to download update file");
        TelemetryManager::instance().record_update_failure("download_failed", version,
                                                           get_platform_key());
        return;
    }

    // Verify file size sanity (reject < 1MB or > 150MB)
    if (result < 1024 * 1024) {
        spdlog::error("[UpdateChecker] Downloaded file too small: {} bytes", result);
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Invalid download"),
                               "Downloaded file is too small");
        TelemetryManager::instance().record_update_failure(
            "file_too_small", version, get_platform_key(), -1, static_cast<int64_t>(result));
        return;
    }
    if (result > 150 * 1024 * 1024) {
        spdlog::error("[UpdateChecker] Downloaded file too large: {} bytes", result);
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Invalid download"),
                               "Downloaded file is too large");
        TelemetryManager::instance().record_update_failure(
            "file_too_large", version, get_platform_key(), -1, static_cast<int64_t>(result));
        return;
    }

    spdlog::info("[UpdateChecker] Download complete: {} bytes", result);
    report_download_status(DownloadStatus::Verifying, 100, lv_tr("Verifying download..."));

    // Verify archive integrity (fork/exec to avoid shell injection)
    bool corrupt = false;
    if (path_is_zip(download_path)) {
        switch (verify_zip_integrity(download_path)) {
        case ZipIntegrity::Ok:
            break;
        case ZipIntegrity::Corrupt:
            corrupt = true;
            break;
        case ZipIntegrity::Unverifiable:
            // Nothing on this system can test the archive. Don't fail the
            // update on a missing tool — the SHA256 check below is the real
            // integrity gate whenever the manifest supplies a hash.
            spdlog::warn("[UpdateChecker] No tool available to test zip integrity; "
                         "relying on SHA256 verification");
            break;
        }
    } else {
        corrupt = safe_exec({resolve_tool("gunzip"), "-t", download_path}) != 0;
    }
    if (corrupt) {
        spdlog::error("[UpdateChecker] Archive verification failed");
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Corrupt download"),
                               "Downloaded file failed integrity check");
        TelemetryManager::instance().record_update_failure(
            "corrupt_download", version, get_platform_key(), -1, static_cast<int64_t>(result));
        return;
    }

    spdlog::info("[UpdateChecker] Archive verified OK");

    // Validate architecture before installing
    if (!validate_elf_architecture(download_path)) {
        spdlog::error("[UpdateChecker] Downloaded update is for wrong architecture!");
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Wrong architecture"),
                               "Downloaded binary doesn't match this device's architecture");
        TelemetryManager::instance().record_update_failure("wrong_architecture", version,
                                                           get_platform_key());
        return;
    }

    // Verify SHA256 hash if available (R2 manifest provides this; GitHub fallback does not)
    {
        std::string expected_sha256;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_info_)
                expected_sha256 = cached_info_->sha256;
        }

        if (expected_sha256.empty()) {
            spdlog::info("[UpdateChecker] No SHA256 hash available, skipping verification");
        } else {
            report_download_status(DownloadStatus::Verifying, 100,
                                   lv_tr("Verifying SHA256 checksum..."));
            auto actual_sha256 = helix::compute_file_sha256(download_path);
            if (actual_sha256.empty()) {
                spdlog::error("[UpdateChecker] Failed to compute SHA256 of {}", download_path);
                std::remove(download_path.c_str());
                report_download_status(DownloadStatus::Error, 0,
                                       lv_tr("Error: Verification failed"),
                                       "Could not compute checksum of downloaded file");
                TelemetryManager::instance().record_update_failure("sha256_compute_failed", version,
                                                                   get_platform_key());
                return;
            }

            if (actual_sha256 != expected_sha256) {
                spdlog::error("[UpdateChecker] SHA256 mismatch! expected={} actual={}",
                              expected_sha256, actual_sha256);
                std::remove(download_path.c_str());
                report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Checksum mismatch"),
                                       "Downloaded file does not match expected checksum");
                TelemetryManager::instance().record_update_failure("sha256_mismatch", version,
                                                                   get_platform_key());
                return;
            }

            spdlog::info("[UpdateChecker] SHA256 verified OK: {}", actual_sha256);
        }
    }

    do_install(download_path);
}

UpdateChecker::ZipTool UpdateChecker::available_zip_tool() {
    if (!find_tool_path("unzip").empty()) {
        return ZipTool::Unzip;
    }
    // No unzip binary (K2's OpenWrt firmware). python3 can read zips itself,
    // but only with zipfile AND zlib — release archives are deflated.
    const std::string py_bin = find_tool_path("python3");
    if (!py_bin.empty() && safe_exec({py_bin, "-c", "import zipfile, zlib"}) == 0) {
        return ZipTool::Python;
    }
    return ZipTool::None;
}

namespace {

/// Force the owner-exec bit on an extracted installer or binary. do_install()
/// runs the extracted install.sh via execv(), which fails with EACCES if the
/// archive stored the member without mode bits — so neither extraction path may
/// rely on the zip carrying them.
void ensure_member_executable(const std::string& extract_dir, const std::string& member) {
    const bool is_script = member.size() >= 3 && member.compare(member.size() - 3, 3, ".sh") == 0;
    const bool in_bin = member.rfind("bin/", 0) == 0 || member.find("/bin/") != std::string::npos;
    if (!is_script && !in_bin) {
        return;
    }
    const std::string path = extract_dir + "/" + member;
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        chmod(path.c_str(), st.st_mode | S_IXUSR);
    }
}

} // namespace

int UpdateChecker::extract_zip_member(const std::string& zip_path, const std::string& extract_dir,
                                      const std::string& member) {
    // -q quiet, -o overwrite without prompting (no TTY during in-app updates).
    const std::string unzip_bin = find_tool_path("unzip");
    if (!unzip_bin.empty()) {
        int ret = safe_exec({unzip_bin, "-q", "-o", zip_path, member, "-d", extract_dir});
        if (ret == 0) {
            ensure_member_executable(extract_dir, member);
        }
        return ret;
    }

    const std::string py_bin = find_tool_path("python3");
    if (!py_bin.empty()) {
        return safe_exec({py_bin, "-c", PY_EXTRACT_SCRIPT, zip_path, member, extract_dir});
    }

    spdlog::error("[UpdateChecker] No unzip binary and no python3 — cannot extract '{}'", member);
    return -1;
}

UpdateChecker::ZipIntegrity UpdateChecker::verify_zip_integrity(const std::string& zip_path) {
    // python3's zipfile does a real per-entry CRC test. Prefer it over
    // `unzip -t`, which is either rejected outright or a silent no-op on the
    // BusyBox builds we ship to (see the header comment for the specifics).
    const std::string py_bin = find_tool_path("python3");
    if (!py_bin.empty()) {
        // Exit codes: 0 = intact, 1 = corrupt, 2 = this python cannot test zips.
        // Code 2 matters on the AD5M, whose python3.7 is built without zlib:
        // ZipFile() raises "Compression requires the (missing) zlib module" for
        // a deflated release zip, which must NOT be read as corruption.
        static constexpr const char* TEST_SCRIPT =
            "import sys\n"
            "try:\n"
            "    import zipfile, zlib\n"
            "except Exception:\n"
            "    sys.exit(2)\n"
            "try:\n"
            "    with zipfile.ZipFile(sys.argv[1]) as zf:\n"
            "        sys.exit(1 if zf.testzip() is not None else 0)\n"
            "except RuntimeError:\n"
            "    sys.exit(2)\n"
            "except Exception:\n"
            "    sys.exit(1)\n";
        int ret = safe_exec({py_bin, "-c", TEST_SCRIPT, zip_path});
        if (ret == 0) {
            return ZipIntegrity::Ok;
        }
        if (ret == 1) {
            return ZipIntegrity::Corrupt;
        }
        // ret == 2 (or a crashed interpreter): fall through to the unzip probe.
    }

    // No usable python zipfile. Fall back to `unzip -l`, which reads the central
    // directory on both BusyBox and info-zip: it catches truncation and garbage
    // without false-failing a valid archive the way `-t` does on BusyBox 1.31.
    const std::string unzip_bin = find_tool_path("unzip");
    if (!unzip_bin.empty()) {
        int ret = safe_exec({unzip_bin, "-l", zip_path});
        return (ret == 0) ? ZipIntegrity::Ok : ZipIntegrity::Corrupt;
    }

    return ZipIntegrity::Unverifiable;
}

bool UpdateChecker::validate_elf_architecture(const std::string& tarball_path) {
    // Use the compile-time platform key to determine expected architecture.
    // uname().machine is unreliable: Pi4 with 64-bit kernel + 32-bit userspace
    // reports "aarch64" even though only 32-bit ARM binaries can execute.
    std::string platform = get_platform_key();
    spdlog::info("[UpdateChecker] Platform key: {}", platform);

    uint8_t expected_class = 0;
    uint16_t expected_machine = 0;
    std::string expected_arch_name;

    if (platform == "pi32" || platform == "ad5m") {
        expected_class = 1;      // ELFCLASS32
        expected_machine = 0x28; // EM_ARM
        expected_arch_name = "ARM 32-bit";
    } else if (platform == "pi") {
        expected_class = 2;      // ELFCLASS64
        expected_machine = 0xB7; // EM_AARCH64
        expected_arch_name = "AARCH64 64-bit";
    } else if (platform == "x86") {
        expected_class = 2;      // ELFCLASS64
        expected_machine = 0x3E; // EM_X86_64
        expected_arch_name = "x86_64 64-bit";
    } else if (platform == "k1" || platform == "ad5x") {
        expected_class = 1;      // ELFCLASS32
        expected_machine = 0x08; // EM_MIPS
        expected_arch_name = "MIPS 32-bit";
    } else {
        spdlog::info("[UpdateChecker] Platform '{}' — skipping ELF validation", platform);
        return true;
    }

    // Extract binary to temp location for inspection
    std::string temp_dir = tarball_path + ".validate";
    mkdir(temp_dir.c_str(), 0750);

    const std::string rm_bin = resolve_tool("rm");

    // Extract binary from archive for inspection — member path differs by
    // format (see extract_installer_from_tarball comment).
    const bool is_zip = path_is_zip(tarball_path);
    const std::string binary_member =
        is_zip ? std::string("bin/helix-screen") : std::string("helixscreen/bin/helix-screen");

    int ret;
    if (is_zip) {
        ret = extract_zip_member(tarball_path, temp_dir, binary_member);
    } else {
        ret = extract_tar_member(tarball_path, temp_dir, binary_member);
    }
    if (ret != 0) {
        spdlog::warn("[UpdateChecker] Could not extract binary for validation, skipping");
        safe_exec({rm_bin, "-rf", temp_dir});
        return true;
    }

    const std::string binary_path =
        is_zip ? (temp_dir + "/bin/helix-screen") : (temp_dir + "/helixscreen/bin/helix-screen");

    // Read ELF header (first 20 bytes)
    FILE* f = fopen(binary_path.c_str(), "rb");
    if (!f) {
        spdlog::warn("[UpdateChecker] Could not open extracted binary for validation");
        safe_exec({rm_bin, "-rf", temp_dir});
        return true;
    }

    uint8_t header[20];
    size_t nread = fread(header, 1, sizeof(header), f);
    fclose(f);

    // Clean up extracted files
    safe_exec({rm_bin, "-rf", temp_dir});

    if (nread < 20) {
        spdlog::error("[UpdateChecker] Binary too small to be valid ELF ({} bytes)", nread);
        return false;
    }

    // Check ELF magic: 0x7f 'E' 'L' 'F'
    if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        spdlog::error("[UpdateChecker] Downloaded binary is not a valid ELF file");
        return false;
    }

    // Check class (byte 4): 1=32-bit, 2=64-bit
    uint8_t elf_class = header[4];

    // Check machine type (bytes 18-19, little-endian): 0x28=ARM, 0xB7=AARCH64
    uint16_t elf_machine =
        static_cast<uint16_t>(header[18]) | (static_cast<uint16_t>(header[19]) << 8);

    const char* class_name = (elf_class == 1) ? "32-bit" : (elf_class == 2) ? "64-bit" : "unknown";
    const char* machine_name = (elf_machine == 0x28)   ? "ARM"
                               : (elf_machine == 0xB7) ? "AARCH64"
                                                       : "unknown";

    spdlog::info("[UpdateChecker] Binary: {} {} (class={}, machine=0x{:x})", machine_name,
                 class_name, elf_class, elf_machine);

    if (elf_class != expected_class || elf_machine != expected_machine) {
        spdlog::error("[UpdateChecker] Architecture mismatch! Runtime is {} but binary is {} {}",
                      expected_arch_name, machine_name, class_name);
        return false;
    }

    spdlog::info("[UpdateChecker] Architecture validation passed ({})", expected_arch_name);
    return true;
}

void UpdateChecker::do_install(const std::string& tarball_path) {
    flog_info("[UpdateChecker] do_install() ENTER: tarball={}", tarball_path);

    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version = cached_info_ ? cached_info_->version : "unknown";
    }

    if (download_cancelled_.load()) {
        flog_info("[UpdateChecker] do_install() cancelled, aborting");
        std::remove(tarball_path.c_str());
        report_download_status(DownloadStatus::Idle, 0, "");
        return;
    }

    report_download_status(DownloadStatus::Installing, 100, "");

    // Resolve install root BEFORE install.sh runs.  After the installer swaps
    // the directory, /proc/self/exe shows "(deleted)" and app_get_install_root()
    // fails.  We need this path to chdir() after install.sh finishes (see below).
    const std::string install_root = app_get_install_root();

    // Safety net: copy config to fallback dir BEFORE calling install.sh.
    // The installer backs up config to TMP_DIR, but under systemd's PrivateTmp=true
    // that backup lives in a volatile mount that's cleaned up on service restart.
    // These backups supplement the rolling backups in /var/lib/helixscreen/ maintained
    // by Config::save(). Config::init() auto-restores from either location.
    //
    // Prefer HELIX_CONFIG_DIR when set (Yocto-packaged baselines, where settings
    // live outside the RO install tree) — otherwise fall back to install_root.
    {
        if (!install_root.empty()) {
            std::string config_src_dir = install_root + "/config";
            if (const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
                env_dir != nullptr && env_dir[0] != '\0') {
                config_src_dir = env_dir;
            }
            std::string config_src = config_src_dir + "/settings.json";
            std::string env_src = config_src_dir + "/helixscreen.env";
            const std::string cp_bin = resolve_tool("cp");
            const std::string mkdir_bin = resolve_tool("mkdir");
            const std::string config_bak = config_backup_fallback();
            const std::string env_bak = env_backup_fallback();

            // Ensure fallback dir exists
            std::string fallback_dir = AppConstants::Update::backup_fallback_dir();
            safe_exec({mkdir_bin, "-p", fallback_dir});

            struct stat st {};
            if (stat(config_src.c_str(), &st) == 0) {
                int ret = safe_exec({cp_bin, "-f", config_src, config_bak});
                if (ret == 0) {
                    flog_info("[UpdateChecker] Pre-update config backup: {}", config_bak);
                } else {
                    flog_warn("[UpdateChecker] Failed to create pre-update config backup (exit {})",
                              ret);
                }
            }
            if (stat(env_src.c_str(), &st) == 0) {
                int ret = safe_exec({cp_bin, "-f", env_src, env_bak});
                if (ret == 0) {
                    flog_info("[UpdateChecker] Pre-update env backup: {}", env_bak);
                } else {
                    flog_warn("[UpdateChecker] Failed to create pre-update env backup (exit {})",
                              ret);
                }
            }
        } else {
            flog_warn("[UpdateChecker] Could not resolve install root for pre-update backup");
        }
    }

    // Extract install.sh from the NEW tarball so we always run the version-matched
    // installer. This prevents failures when the local install.sh is outdated and
    // missing functions that the new version's main() calls.
    std::string install_script;
    std::string extracted_dir = tarball_path + ".installer";
    bool extracted_from_tarball = false;

    flog_debug("[UpdateChecker] Creating extracted_dir: {}", extracted_dir);
    int mkdir_ret = mkdir(extracted_dir.c_str(), 0750);
    flog_debug("[UpdateChecker] mkdir returned {} (errno={})", mkdir_ret,
               mkdir_ret < 0 ? strerror(errno) : "ok");

    const std::string rm_bin = resolve_tool("rm");

    flog_debug("[UpdateChecker] Extracting installer from tarball...");
    install_script = extract_installer_from_tarball(tarball_path, extracted_dir);
    if (!install_script.empty()) {
        extracted_from_tarball = true;
        flog_info("[UpdateChecker] Using installer extracted from tarball: {}", install_script);
    } else {
        // Fall back to local install.sh (best effort for older tarballs without it)
        flog_warn(
            "[UpdateChecker] Could not extract install.sh from tarball, falling back to local");
        safe_exec({rm_bin, "-rf", extracted_dir});
        install_script = find_local_installer();
        flog_info("[UpdateChecker] Local installer search result: '{}'", install_script);
    }

    if (install_script.empty()) {
        flog_error("[UpdateChecker] Cannot find install.sh");
        report_download_status(DownloadStatus::Error, 0, lv_tr("Error: Installer not found"),
                               "Cannot locate install.sh script");
        TelemetryManager::instance().record_update_failure("installer_not_found", version,
                                                           get_platform_key());
        return;
    }

    // Write installer output to a persistent log in /var/log/ so it survives
    // process restart and is available for post-update debugging.  Fall back
    // to the tarball directory if /var/log/ isn't writable — read-only rootfs,
    // wedged tmpfs, etc.  See system/log_path_probe.h for why a real-write
    // probe is needed: open(O_CREAT|O_TRUNC) succeeds on a full tmpfs and
    // install.sh's first printf then dies silently under `set -e` with a
    // 0-byte log (the CC1 failure mode).
    //
    // Sizing the headroom: install.sh emits ~20-50 KB on the happy path and
    // up to ~500 KB on a cascading-error path.  On top of that the same
    // tmpfs typically hosts syslog, dropbear, wpa_supplicant, and install.sh's
    // own child processes, all writing during the install window.  5 MB is
    // ~10× the log's worst case and leaves enough room that "passed the
    // probe" means the FS is genuinely healthy, not marginal.
    constexpr uint64_t MIN_INSTALL_LOG_FREE_BYTES = 5 * 1024 * 1024; // 5 MB

    std::string install_log = "/var/log/helixscreen-install.log";
    {
        auto probe =
            helix::system::probe_log_path_writable(install_log, MIN_INSTALL_LOG_FREE_BYTES);
        if (!probe.ok) {
            const std::string fallback = tarball_path + ".install.log";
            flog_warn("[UpdateChecker] {} not writable ({}), falling back to {}", install_log,
                      probe.error, fallback);
            install_log = fallback;
            probe = helix::system::probe_log_path_writable(install_log, MIN_INSTALL_LOG_FREE_BYTES);
            if (!probe.ok) {
                flog_error("[UpdateChecker] fallback log {} also not writable ({}); "
                           "install.sh stdout/stderr will be lost",
                           install_log, probe.error);
            }
        }
    }

    flog_info("[UpdateChecker] Running: {} --local {} --update", install_script, tarball_path);
    flog_info("[UpdateChecker] install_script access(X_OK)={} tarball access(R_OK)={}",
              access(install_script.c_str(), X_OK), access(tarball_path.c_str(), R_OK));
    flog_info("[UpdateChecker] install_log={}", install_log);
    {
        char cwd_buf[PATH_MAX] = {};
        const char* cwd = getcwd(cwd_buf, sizeof(cwd_buf));
        flog_info("[UpdateChecker] cwd={} uid={} euid={} pid={}", cwd ? cwd : "(error)", getuid(),
                  geteuid(), getpid());
    }
    {
        struct stat st {};
        if (stat(tarball_path.c_str(), &st) == 0) {
            flog_info("[UpdateChecker] tarball size: {} bytes", st.st_size);
        } else {
            flog_error("[UpdateChecker] stat({}) failed: {}", tarball_path, strerror(errno));
        }
    }

    // Hand our already-probed staging dir to install.sh via TMP_DIR so it does
    // NOT re-probe its own hardcoded candidate list (which omits the install
    // dir and dies at mkdir on read-only-/tmp boxes like the OrangePi Zero3).
    // compute_update_staging_dir() guarantees a dot-prefixed dir OUTSIDE the
    // install root — never under INSTALL_DIR, which the installer's --update
    // flow rm -rf's (dotfile loop) and mv's (atomic swap). Only hand it off
    // when its parent is actually writable; otherwise leave TMP_DIR unset and
    // let install.sh's own (hardened) detect_tmp_dir probe take over. Computed
    // + logged on the parent side because logging may not flush from the child
    // after fork().
    const std::string staging_dir = compute_update_staging_dir(tarball_path, install_root);
    const std::string staging_parent = helix::paths::dirname(staging_dir);
    const bool staging_writable = helix::paths::is_writable_dir(staging_parent);
    if (staging_writable) {
        flog_info("[UpdateChecker] Handing staging dir to installer: TMP_DIR={}", staging_dir);
    } else {
        flog_info("[UpdateChecker] Staging parent {} not writable; leaving TMP_DIR unset for "
                  "installer to probe",
                  staging_parent);
    }

    // Fork install.sh with its output redirected to a persistent log file.
    // Using a file instead of a pipe means we get the full output even if this
    // process is killed by systemd's stop_service during the install step.
    int ret = -1;
    bool timed_out = false;
    std::string last_error_line;   // last line containing ERROR or FAILED
    std::string last_warning_line; // last line containing WARNING (lower priority)
    {
        int log_fd = open(install_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
        if (log_fd < 0) {
            flog_error("[UpdateChecker] Could not open install log {}: {}", install_log,
                       strerror(errno));
        } else {
            flog_info("[UpdateChecker] Install log fd={}", log_fd);
        }

        pid_t pid = fork();
        if (pid < 0) {
            flog_error("[UpdateChecker] fork() for install failed: {}", strerror(errno));
            if (log_fd >= 0)
                close(log_fd);
        } else if (pid == 0) {
            // Child: redirect stdout+stderr to log file
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
            // setsid() gives install.sh its own session so it isn't killed by
            // SIGTERM propagation via the process group.  Note: this does NOT
            // escape the systemd cgroup — install.sh therefore must not rely on
            // surviving a `systemctl stop helixscreen` mid-install.  The script
            // handles this by deferring the service stop to a final
            // `systemctl restart` that systemd completes even if install.sh is
            // killed during the stop phase (see scripts/install.sh main()).
            setsid();
            // Tell install.sh this is an in-app self-update so it skips
            // stop_service/start_service on SysV — the watchdog handles restart.
            setenv("HELIX_SELF_UPDATE", "1", 1);
            // Pass the app-validated staging dir (computed on the parent above)
            // only when its parent is writable; else let the installer probe.
            if (staging_writable) {
                setenv("TMP_DIR", staging_dir.c_str(), 1);
            }
            const char* argv[] = {install_script.c_str(), "--local", tarball_path.c_str(),
                                  "--update", nullptr};
            execv(install_script.c_str(), const_cast<char**>(argv));
            _exit(127);
        } else {
            // Parent: close our copy of the log fd and wait with timeout
            flog_info("[UpdateChecker] Forked install.sh as pid={}, waiting...", pid);
            if (log_fd >= 0)
                close(log_fd);

            constexpr int timeout_seconds = 600;
            int status = 0;
            bool exited = false;

            static constexpr const char* install_message_keys[] = {
                "Still working...", "Almost there...", "Just a bit more...",
                "Hang tight...",    "Finishing up...",
            };
            constexpr int num_messages =
                sizeof(install_message_keys) / sizeof(install_message_keys[0]);

            for (int elapsed = 0; elapsed < timeout_seconds; ++elapsed) {
                pid_t result = waitpid(pid, &status, WNOHANG);
                if (result < 0) {
                    flog_error("[UpdateChecker] waitpid(install) failed: {} (errno={})",
                               strerror(errno), errno);
                    break;
                }
                if (result > 0) {
                    exited = true;
                    flog_info("[UpdateChecker] install.sh (pid={}) exited after ~{}s", pid,
                              elapsed);
                    break;
                }
                if (elapsed > 0 && elapsed % 10 == 0) {
                    flog_info("[UpdateChecker] Still waiting for install.sh pid={} ({}s/{}s)", pid,
                              elapsed, timeout_seconds);
                    if (elapsed >= 30) {
                        int idx = ((elapsed - 30) / 10) % num_messages;
                        report_download_status(DownloadStatus::Installing, 100,
                                               lv_tr(install_message_keys[idx]));
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (exited) {
                bool normal_exit = WIFEXITED(status);
                bool signaled = WIFSIGNALED(status);
                ret = normal_exit ? WEXITSTATUS(status) : -1;
                int sig = signaled ? WTERMSIG(status) : 0;
                flog_info(
                    "[UpdateChecker] install.sh exit: code={} normal={} signaled={} signal={}", ret,
                    normal_exit, signaled, sig);
            } else {
                timed_out = true;
                flog_error("[UpdateChecker] install.sh timed out after {}s, killing",
                           timeout_seconds);
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0); // reap zombie
            }
        }

        // Read install log: emit to spdlog, and capture the last error/warning
        // line to show in the UI (the user can't see the log file from the touchscreen).
        {
            struct stat log_stat {};
            if (stat(install_log.c_str(), &log_stat) == 0) {
                flog_info("[UpdateChecker] Install log exists: {} bytes", log_stat.st_size);
            } else {
                flog_error("[UpdateChecker] Install log MISSING {}: {}", install_log,
                           strerror(errno));
            }
        }
        FILE* lf = fopen(install_log.c_str(), "r");
        if (lf) {
            char line[512];
            int line_count = 0;
            flog_info("[UpdateChecker] ---- install.sh output ----");
            while (fgets(line, sizeof(line), lf)) {
                // Strip trailing newline
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                    line[--len] = '\0';
                }
                spdlog::info("[install.sh] {}", line);
                line_count++;
                // Capture last ERROR/WARNING line for UI display.
                // Prioritize ERROR/FAILED over WARNING so rollback messages
                // don't mask the actual failure reason.
                std::string s(line);
                if (s.find("ERROR") != std::string::npos || s.find("FAILED") != std::string::npos) {
                    last_error_line = strip_ansi_codes(s);
                } else if (s.find("WARNING") != std::string::npos) {
                    last_warning_line = strip_ansi_codes(s);
                }
            }
            flog_info("[UpdateChecker] ---- end install.sh output ({} lines) ----", line_count);
            fclose(lf);
        } else {
            flog_error("[UpdateChecker] Could not read install log {}: {}", install_log,
                       strerror(errno));
        }
        // Log persists at /var/log/helixscreen-install.log for post-update debugging
    }

    // Clean up tarball and extracted installer regardless of result
    flog_info("[UpdateChecker] Cleaning up: tarball={} extracted={} (from_tarball={})",
              tarball_path, extracted_dir, extracted_from_tarball);
    std::remove(tarball_path.c_str());
    if (extracted_from_tarball) {
        safe_exec({rm_bin, "-rf", extracted_dir});
    }

    if (ret != 0) {
        flog_error("[UpdateChecker] Install script failed with code {}", ret);
        // Build a user-visible error message with detail from the install log
        std::string ui_text =
            timed_out ? lv_tr("Installation timed out") : lv_tr("Installation failed");
        const auto& detail = !last_error_line.empty() ? last_error_line : last_warning_line;
        if (!detail.empty()) {
            ui_text += "\n" + detail;
        }
        report_download_status(DownloadStatus::Error, 0, ui_text,
                               "install.sh returned error code " + std::to_string(ret));
        std::string reason = timed_out ? "install_timeout" : "install_failed";
        TelemetryManager::instance().record_update_failure(reason, version, get_platform_key(), -1,
                                                           -1, ret);
        return;
    }

    spdlog::info("[UpdateChecker] Update installed successfully!");

    // Restore CWD after install.sh moved/deleted the old install directory.
    // The init script does `cd $DAEMON_DIR` before launching, so our CWD was
    // inside the install dir.  install.sh does:
    //   mv /opt/helixscreen /opt/helixscreen.old
    //   mv new_install /opt/helixscreen
    //   rm -rf /opt/helixscreen.old
    // After the rm, our CWD inode is deleted and any relative path operation
    // (e.g. telemetry writes to "config/") triggers SIGABRT on some platforms.
    // install_root was resolved via /proc/self/exe BEFORE the swap; now it
    // points to the new install directory at the same path.
    if (!install_root.empty()) {
        if (chdir(install_root.c_str()) == 0) {
            spdlog::info("[UpdateChecker] Restored CWD to {}", install_root);
        } else {
            spdlog::warn("[UpdateChecker] chdir({}) failed: {}", install_root, strerror(errno));
        }
    }

    // Write update success flag for telemetry (picked up on next boot)
    TelemetryManager::write_update_success_flag("config", version, HELIX_VERSION,
                                                get_platform_key());

    // Write restart marker so watchdog knows this exit is expected.
    // Safety net: even if _exit(0) below doesn't execute (e.g., SIGABRT from
    // stale CWD), the watchdog sees the marker and skips the crash dialog.
    {
        std::string marker = AppConstants::Update::update_restart_marker_path();
        std::ofstream ofs(marker);
        if (ofs) {
            spdlog::info("[UpdateChecker] Wrote update restart marker: {}", marker);
        } else {
            spdlog::warn("[UpdateChecker] Failed to write update restart marker: {}", marker);
        }
    }

    // Write sentinel so helixscreen-update.service (triggered by release_info.json
    // change) knows the watchdog is already handling the restart.  Without this,
    // both the watchdog and the path-watcher race to restart, causing a double-start.
    // The sentinel is NOT deleted by the update service — the path watcher can fire
    // multiple times after an atomic swap.  Cleaned up on next successful startup.
    //
    // Write to ~/.helixscreen/ (survives PrivateTmp, accessible from update.service).
    // Also write legacy /tmp sentinel for backward compat with old service files.
    {
        std::string sentinel =
            AppConstants::Update::backup_fallback_dir() + "/self_restart_sentinel";
        std::ofstream ofs(sentinel);
        if (ofs) {
            spdlog::info("[UpdateChecker] Wrote self-restart sentinel: {}", sentinel);
        } else {
            spdlog::warn("[UpdateChecker] Failed to write sentinel: {}", sentinel);
        }
        // Legacy location (may not work with PrivateTmp=true)
        std::ofstream ofs_legacy("/tmp/helixscreen_self_restart");
    }

    report_download_status(DownloadStatus::Complete, 100,
                           fmt::format(lv_tr("v{} installed! Restarting..."), version));

    // Restart strategy depends on whether we're supervised.
    // _exit(0) is used in all cases to avoid racing with destructors on other
    // threads — the binary on disk has been replaced and normal shutdown may
    // touch stale resources.
    bool supervised = getenv("INVOCATION_ID") || getenv("HELIX_SUPERVISED");

    if (!supervised) {
        // No watchdog/systemd — fork+exec the new binary before exiting,
        // otherwise _exit(0) just kills the process with nothing to restart it.
        // Use the filesystem path (not /proc/self/exe) because install.sh has
        // replaced the binary at this path — /proc/self/exe still points to
        // the old (deleted) inode.
        char** argv = app_get_stored_argv();
        std::string bin_path = install_root + "/bin/helix-screen";

        spdlog::info("[UpdateChecker] Not supervised — fork+exec restart: {}", bin_path);
        spdlog::default_logger()->flush();

        pid_t pid = fork();
        if (pid < 0) {
            spdlog::error("[UpdateChecker] Fork failed: {} — falling back to _exit(0)",
                          strerror(errno));
            spdlog::default_logger()->flush();
        } else if (pid == 0) {
            // Child: brief delay for parent to exit, then exec new binary
            usleep(200000); // 200ms
            execv(bin_path.c_str(), argv);
            // execv failed — nothing we can do
            ::_exit(127);
        }
        // Parent: fall through to _exit(0)
    }

    spdlog::info("[UpdateChecker] Restarting to apply update (supervised={})", supervised);
    spdlog::default_logger()->flush();
    ::_exit(0);
}

// Done with flush-on-write macros — only needed in do_install()
#undef flog_info
#undef flog_warn
#undef flog_error
#undef flog_debug
#undef FLOG

// ============================================================================
// External update detection (Moonraker notify_update_response)
// ============================================================================

void UpdateChecker::handle_external_update_complete() {
    spdlog::info("[UpdateChecker] Moonraker completed external update of HelixScreen — restarting");

    // Write sentinel so helixscreen-update.service (systemd path watcher) skips
    // its restart — we're handling it here.  Same sentinel as self-update path.
    {
        std::string sentinel =
            AppConstants::Update::backup_fallback_dir() + "/self_restart_sentinel";
        std::ofstream ofs(sentinel);
        if (ofs) {
            spdlog::info("[UpdateChecker] Wrote self-restart sentinel: {}", sentinel);
        } else {
            spdlog::warn("[UpdateChecker] Failed to write sentinel: {}", sentinel);
        }
        std::ofstream ofs_legacy("/tmp/helixscreen_self_restart");
    }

    // Write restart marker so watchdog knows this exit is expected
    {
        std::string marker = AppConstants::Update::update_restart_marker_path();
        std::ofstream ofs(marker);
        if (ofs) {
            spdlog::info("[UpdateChecker] Wrote update restart marker: {}", marker);
        } else {
            spdlog::warn("[UpdateChecker] Failed to write update restart marker: {}", marker);
        }
    }

    // Restart strategy: same as do_install() but simpler — Moonraker already
    // replaced the binary on disk, we just need to restart the process.
    bool supervised = getenv("INVOCATION_ID") || getenv("HELIX_SUPERVISED");

    if (!supervised) {
        // No watchdog/systemd — fork+exec the new binary before exiting.
        // app_get_install_root() strips the " (deleted)" suffix from /proc/self/exe
        // to get the filesystem path where Moonraker extracted the new binary.
        char** argv = app_get_stored_argv();
        std::string install_root = app_get_install_root();
        std::string bin_path = install_root.empty() ? "" : install_root + "/bin/helix-screen";

        if (!bin_path.empty() && argv) {
            spdlog::info("[UpdateChecker] Not supervised — fork+exec restart: {}", bin_path);
            spdlog::default_logger()->flush();

            pid_t pid = fork();
            if (pid < 0) {
                spdlog::error("[UpdateChecker] Fork failed: {} — falling back to _exit(0)",
                              strerror(errno));
                spdlog::default_logger()->flush();
            } else if (pid == 0) {
                usleep(200000); // 200ms for parent to exit
                execv(bin_path.c_str(), argv);
                ::_exit(127);
            }
        }
    }

    spdlog::info("[UpdateChecker] Restarting to apply external update (supervised={})", supervised);
    spdlog::default_logger()->flush();
    ::_exit(0);
}

// ============================================================================
// Static helpers
// ============================================================================

std::string UpdateChecker::extract_installer_from_tarball(const std::string& tarball_path,
                                                          const std::string& extract_dir) {
    // Member paths differ by archive format. The tar.gz has a top-level
    // helixscreen/ prefix ("helixscreen/install.sh") while the zip has a
    // FLAT layout ("install.sh") — that's the Moonraker Update Manager
    // contract, see mk/cross.mk release-* targets.
    const bool is_zip = path_is_zip(tarball_path);
    const std::string member =
        is_zip ? std::string(INSTALLER_FILENAME) : std::string("helixscreen/") + INSTALLER_FILENAME;

    int ext_ret;
    if (is_zip) {
        ext_ret = extract_zip_member(tarball_path, extract_dir, member);
    } else {
        ext_ret = extract_tar_member(tarball_path, extract_dir, member);
    }

    const std::string installer = is_zip ? (extract_dir + "/" + INSTALLER_FILENAME)
                                         : (extract_dir + "/helixscreen/" + INSTALLER_FILENAME);
    if (ext_ret == 0 && access(installer.c_str(), R_OK) == 0) {
        chmod(installer.c_str(), 0755);
        return installer;
    }

    return "";
}

std::string
UpdateChecker::find_local_installer(const std::vector<std::string>& extra_search_paths) {
    std::vector<std::string> search_paths;

    // Caller-supplied paths first (e.g., exe-relative)
    for (const auto& p : extra_search_paths) {
        search_paths.push_back(p);
    }

    // Resolve the install root via the canonical accessor rather than
    // re-parsing /proc/self/exe here. app_get_install_root() is the single
    // source of truth used elsewhere in this file (do_install, get_download_path,
    // the external-update restart) and additionally handles the /build/bin dev
    // layout, the " (deleted)" suffix during self-update, and config overrides —
    // all of which this local readlink block missed.
    const std::string install_root = app_get_install_root();
    if (!install_root.empty()) {
        search_paths.push_back(install_root + "/" + INSTALLER_FILENAME);
    }

    // Belt-and-suspenders: also derive the root directly from /proc/self/exe
    // (strip the binary name and a trailing /bin) WITHOUT the is_valid_data_root()
    // gate app_get_install_root() applies. This deprioritizes — never removes —
    // the candidate the pre-refactor code offered, covering a partial install
    // whose root lacks ui_xml/ (rejected by the canonical resolver) but still has
    // install.sh. Deduped against the canonical candidate above.
    char exe_buf[PATH_MAX] = {};
    ssize_t exe_len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (exe_len > 0) {
        exe_buf[exe_len] = '\0';
        std::string exe_dir(exe_buf);
        auto slash = exe_dir.rfind('/');
        if (slash != std::string::npos) {
            exe_dir = exe_dir.substr(0, slash); // strip binary name → bin/
            if (exe_dir.size() >= 4 && exe_dir.substr(exe_dir.size() - 4) == "/bin") {
                std::string raw_root = exe_dir.substr(0, exe_dir.size() - 4);
                if (raw_root != install_root) {
                    search_paths.push_back(raw_root + "/" + INSTALLER_FILENAME);
                }
            }
        }
    }

    // Well-known install locations as fallback
    std::string fname = INSTALLER_FILENAME;
    search_paths.push_back("/opt/helixscreen/" + fname);
    search_paths.push_back("/root/printer_software/helixscreen/" + fname);
    search_paths.push_back("/usr/data/helixscreen/" + fname);
    search_paths.push_back("/home/biqu/helixscreen/" + fname);
    search_paths.push_back("/home/pi/helixscreen/" + fname);
    search_paths.push_back("scripts/" + fname); // development fallback

    for (const auto& path : search_paths) {
        if (access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }

    return "";
}

// ============================================================================
// Public API
// ============================================================================

void UpdateChecker::check_for_updates(Callback callback) {
    // Don't start new checks during shutdown
    if (shutting_down_) {
        spdlog::debug("[UpdateChecker] Ignoring check_for_updates during shutdown");
        return;
    }

    // Firmware-managed devices own updates externally, so looking is pointless
    // there. Everything else checks, INCLUDING a tree we cannot write: the check
    // is a manifest fetch and touches no files, and reporting the available
    // version is what lets a user on a non-updatable install know to re-run the
    // installer. start_download() is where applying is refused.
    if (update_checks_suppressed()) {
        spdlog::info("[UpdateChecker] Check skipped: updates are firmware-managed");
        return;
    }

    // Use mutex for entire operation to prevent race conditions.
    // This is safe because we join the previous thread before spawning a new one,
    // so we won't deadlock with the worker thread.
    std::unique_lock<std::mutex> lock(mutex_);

    // Atomic check if already checking
    if (status_ == Status::Checking) {
        spdlog::debug("[UpdateChecker] Check already in progress, ignoring");
        return;
    }

    // Rate limiting: return cached result if checked recently.
    // Skipped for dev channel (local server) — no risk of hammering remote APIs.
    auto now = std::chrono::steady_clock::now();
    auto time_since_last = now - last_check_time_;
    bool dev_channel = (get_channel() == UpdateChannel::Dev);

    if (!dev_channel && last_check_time_.time_since_epoch().count() > 0 &&
        time_since_last < MIN_CHECK_INTERVAL) {
        auto minutes_remaining =
            std::chrono::duration_cast<std::chrono::minutes>(MIN_CHECK_INTERVAL - time_since_last)
                .count();
        spdlog::debug("[UpdateChecker] Rate limited, {} minutes until next check allowed",
                      minutes_remaining);

        // Return cached result via callback
        if (callback) {
            auto cached = cached_info_;
            auto status = status_.load();
            // Release lock before dispatching (callback may call back into UpdateChecker)
            lock.unlock();
            // Dispatch to LVGL thread
            helix::ui::queue_update([callback, status, cached]() { callback(status, cached); });
        }
        return;
    }

    spdlog::info("[UpdateChecker] Starting update check");

    // CRITICAL: Join any previous thread before starting new one.
    // If a previous check completed naturally, the thread is still joinable
    // even though status is not Checking. Assigning to a joinable std::thread
    // causes std::terminate()!
    //
    // We must release the lock before joining to prevent deadlock - the worker
    // thread's report_result() also acquires this mutex.
    lock.unlock();
    if (worker_thread_.joinable()) {
        spdlog::debug("[UpdateChecker] Joining previous worker thread");
        worker_thread_.join();
    }
    lock.lock();

    // Re-check state after reacquiring lock (another thread may have started)
    if (status_ == Status::Checking || shutting_down_) {
        spdlog::debug("[UpdateChecker] State changed while joining, aborting");
        return;
    }

    // Store callback and reset state - all under lock
    pending_callback_ = callback;
    error_message_.clear();
    status_ = Status::Checking;
    cancelled_ = false;

    // Cache channel config on main thread (Config is NOT thread-safe)
    cached_channel_ = get_channel();
    auto* config = Config::get_instance();
    cached_dev_url_ = config ? config->get<std::string>("/update/dev_url", "") : "";
    cached_r2_base_url_ = effective_r2_base_url();

    spdlog::debug("[UpdateChecker] check_for_updates: channel={} dev_url='{}' r2_base_url='{}'",
                  channel_name(cached_channel_),
                  cached_dev_url_.empty() ? "(none)" : cached_dev_url_, cached_r2_base_url_);

    // Update subjects on LVGL thread (check_for_updates is public, could be called from any thread)
    if (subjects_initialized_) {
        async_lifetime_.defer("UpdateChecker::check_for_updates", [this]() {
            lv_subject_set_int(&status_subject_, static_cast<int>(Status::Checking));
            lv_subject_copy_string(&version_text_subject_, lv_tr("Checking..."));
        });
    }

    // Spawn worker thread. Wrap — pthread_create EAGAIN under thread
    // exhaustion throws std::system_error; would abort via std::terminate
    // if this is invoked from an event-cb frame ([L083]).
    try {
        worker_thread_ = std::thread(&UpdateChecker::do_check, this);
    } catch (const std::system_error& e) {
        spdlog::error("[UpdateChecker] Failed to spawn check thread: {}", e.what());
        // Roll back state we set above so the next check_for_updates() call
        // can run, and notify the caller's callback with an Error result so
        // it doesn't hang waiting for a do_check() that never started.
        Callback cb_to_fire = std::move(pending_callback_);
        pending_callback_ = nullptr;
        status_ = Status::Error;
        error_message_ = "system busy";
        if (subjects_initialized_) {
            async_lifetime_.defer("UpdateChecker::check_for_updates_spawn_failed", [this]() {
                lv_subject_set_int(&status_subject_, static_cast<int>(Status::Error));
            });
        }
        if (cb_to_fire) {
            helix::ui::queue_update([cb_to_fire]() { cb_to_fire(Status::Error, std::nullopt); });
        }
    }
}

UpdateChecker::Status UpdateChecker::get_status() const {
    return status_.load();
}

std::optional<UpdateChecker::ReleaseInfo> UpdateChecker::get_cached_update() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_info_;
}

bool UpdateChecker::has_update_available() const {
    return status_ == Status::UpdateAvailable && get_cached_update().has_value();
}

std::string UpdateChecker::get_error_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_message_;
}

void UpdateChecker::clear_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cached_info_.reset();
    error_message_.clear();
    status_ = Status::Idle;
    spdlog::debug("[UpdateChecker] Cache cleared");
}

void UpdateChecker::on_channel_changed() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached_info_.reset();
        error_message_.clear();
        status_ = Status::Idle;
        // Clearing the rate-limit clock is what makes the check below actually
        // go out. "Checked recently" only implies "the answer is still valid"
        // while the question is unchanged, and the channel IS the question.
        last_check_time_ = {};
    }

    // Off-thread readers (the debug bundle's update section) cannot consult
    // Config themselves; re-snapshot before the worker starts.
    refresh_config_snapshot();

    spdlog::info("[UpdateChecker] Update channel changed, re-checking");
    check_for_updates();
}

// ============================================================================
// Worker Thread
// ============================================================================

void UpdateChecker::do_check() {
    spdlog::debug("[UpdateChecker] Worker thread started");

    try {
        // Record check time at start (under mutex for thread safety)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_check_time_ = std::chrono::steady_clock::now();
        }

        if (cancelled_) {
            spdlog::debug("[UpdateChecker] Check cancelled before network request");
            return;
        }

        // Use channel cached on main thread (Config is NOT thread-safe)
        auto channel = cached_channel_;
        const char* channel_name = (channel == UpdateChannel::Beta)  ? "Beta"
                                   : (channel == UpdateChannel::Dev) ? "Dev"
                                                                     : "Stable";
        spdlog::info("[UpdateChecker] Checking {} channel", channel_name);

        ReleaseInfo info;
        std::string error;
        bool ok = false;

        switch (channel) {
        case UpdateChannel::Beta:
            ok = fetch_beta_release(info, error);
            break;
        case UpdateChannel::Dev:
            ok = fetch_dev_release(info, error);
            break;
        case UpdateChannel::Stable:
        default:
            ok = fetch_stable_release(info, error);
            break;
        }

        if (cancelled_) {
            spdlog::debug("[UpdateChecker] Check cancelled after network request");
            return;
        }

        if (!ok) {
            spdlog::warn("[UpdateChecker] {}", error);
            report_result(Status::Error, std::nullopt, error);
            return;
        }

        // Compare versions
        std::string current_version = HELIX_VERSION;
        spdlog::debug("[UpdateChecker] Current: {}, Latest: {}", current_version, info.version);

        switch (compare_channel_version(current_version, info.version)) {
        case ChannelVersionRelation::Newer:
            info.is_downgrade = false;
            spdlog::info("[UpdateChecker] Update available: {} -> {}", current_version,
                         info.version);
            report_result(Status::UpdateAvailable, info, "");
            break;

        case ChannelVersionRelation::Older:
            // The selected channel is behind this install — almost always
            // because the user just moved from a faster channel back to a
            // slower one. Offer it, or they are stranded on the old channel's
            // build with the check reporting "up to date" forever. Flagged so
            // the auto-check never raises this unprompted and the install path
            // asks first.
            info.is_downgrade = true;
            spdlog::info("[UpdateChecker] Channel is behind: {} -> {} (downgrade offered)",
                         current_version, info.version);
            report_result(Status::UpdateAvailable, info, "");
            break;

        case ChannelVersionRelation::Same:
        case ChannelVersionRelation::Unknown:
        default:
            spdlog::info("[UpdateChecker] Already up to date ({})", current_version);
            // Pass info even for UpToDate so callbacks (e.g., --release-notes) can access it
            report_result(Status::UpToDate, info, "");
            break;
        }

        spdlog::debug("[UpdateChecker] Worker thread finished");
    } catch (const std::exception& e) {
        spdlog::error("[UpdateChecker] Exception in worker thread: {}", e.what());
        report_result(Status::Error, std::nullopt, std::string("Internal error: ") + e.what());
    } catch (...) {
        spdlog::error("[UpdateChecker] Unknown exception in worker thread");
        report_result(Status::Error, std::nullopt, "Internal error (unknown exception)");
    }
}

// ============================================================================
// Channel-specific fetch methods
// ============================================================================

UpdateChecker::UpdateChannel UpdateChecker::get_channel() const {
    auto* config = Config::get_instance();
    if (!config) {
        return UpdateChannel::Stable;
    }
    int channel = config->get<int>("/update/channel", 0);

    // /update/channel persists independently of /beta_features, but the dropdown
    // that sets it is gated on show_beta_features (about_settings_overlay.xml).
    // A user who unlocks beta with the 7-tap easter egg, picks Dev, then re-locks
    // keeps fetching from the arbitrary /update/dev_url with the dropdown hidden
    // and no route back to Stable. Clamp the EFFECTIVE channel here rather than
    // rewriting the stored value, so re-unlocking beta restores the channel the
    // user actually picked instead of silently resetting it to Stable.
    if (channel != 0 && !config->is_beta_features_enabled()) {
        spdlog::info("[UpdateChecker] Channel {} needs beta features (disabled) — using stable",
                     channel);
        return UpdateChannel::Stable;
    }

    switch (channel) {
    case 1:
        return UpdateChannel::Beta;
    case 2:
        return UpdateChannel::Dev;
    default:
        return UpdateChannel::Stable;
    }
}

const char* UpdateChecker::channel_name(UpdateChannel channel) {
    switch (channel) {
    case UpdateChannel::Beta:
        return "beta";
    case UpdateChannel::Dev:
        return "dev";
    case UpdateChannel::Stable:
        break;
    }
    return "stable";
}

void UpdateChecker::refresh_config_snapshot() {
    // Read Config on the caller's (main) thread, then publish under mutex_.
    ConfigSnapshot snap;
    snap.channel = channel_name(get_channel());
    snap.r2_base_url = effective_r2_base_url();

    std::lock_guard<std::mutex> lock(mutex_);
    config_snapshot_ = std::move(snap);
}

UpdateChecker::ConfigSnapshot UpdateChecker::config_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_snapshot_;
}

std::string UpdateChecker::effective_r2_base_url() {
    auto* config = Config::get_instance();
    std::string url = config ? config->get<std::string>("/update/r2_url", std::string{}) : "";
    if (url.empty()) {
        url = DEFAULT_R2_BASE_URL;
    }
    // Normalize: strip trailing slashes so callers can join with "/<path>".
    while (url.size() > 1 && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

std::string UpdateChecker::get_platform_key() {
#ifdef HELIX_PLATFORM_AD5M
    return "ad5m";
#elif defined(HELIX_PLATFORM_CC1)
    return "cc1";
#elif defined(HELIX_PLATFORM_AD5X)
    return "ad5x";
#elif defined(HELIX_PLATFORM_MIPS)
    // Same binary runs on K1 and AD5X — detect at runtime.
    // AD5X has /usr/prog dir (FlashForge layout) or /ZMOD file; K1 has neither.
    {
        struct stat st;
        if ((stat("/usr/prog", &st) == 0 && S_ISDIR(st.st_mode)) ||
            (stat("/ZMOD", &st) == 0 && S_ISREG(st.st_mode))) {
            return "ad5x";
        }
        return "k1";
    }
#elif defined(HELIX_PLATFORM_K1)
    // k1-dynamic build variant: dev/debug dynamic-linked K1 binary. Not in the
    // release matrix today — map to "k1" so if it ever ships, self-update
    // fetches the static K1 tarball instead of silently falling through to pi.
    return "k1";
#elif defined(HELIX_PLATFORM_K2)
    return "k2";
#elif defined(HELIX_PLATFORM_X86)
    return "x86";
#elif defined(HELIX_PLATFORM_SNAPMAKER_U1)
    return "snapmaker-u1";
#elif defined(HELIX_PLATFORM_PI32)
    return "pi32";
#elif defined(HELIX_PLATFORM_ESP32)
    return "esp32";
#else
    return "pi";
#endif
}

std::string UpdateChecker::get_platform_display_name(const std::string& key) {
    // Keep in sync with get_platform_key() and the known_platforms test.
    // debug_bundle_collector.cpp calls this; do NOT add a second copy there.
    if (key == "pi")
        return "Raspberry Pi";
    if (key == "pi32")
        return "Raspberry Pi (32-bit)";
    if (key == "x86")
        return "x86 Desktop";
    if (key == "ad5m")
        return "FlashForge Adventurer 5M";
    if (key == "ad5x")
        return "FlashForge Adventurer 5X";
    if (key == "k1")
        return "Creality K1";
    if (key == "k2")
        return "Creality K2 Plus";
    if (key == "cc1")
        return "Elegoo Centauri Carbon";
    if (key == "snapmaker-u1")
        return "Snapmaker U1";
    if (key == "esp32")
        return "BTT K-Touch";
    return key;
}

// ============================================================================
// Dismissed Version
// ============================================================================

bool UpdateChecker::is_version_dismissed(const std::string& version) const {
    auto* config = Config::get_instance();
    if (!config) {
        return false;
    }

    auto dismissed_str = config->get<std::string>("/update/dismissed_version", "");
    if (dismissed_str.empty()) {
        return false;
    }

    auto dismissed = helix::version::parse_version(dismissed_str);
    auto check = helix::version::parse_version(version);

    if (!dismissed || !check) {
        return false;
    }

    // Dismissed if the version is <= the dismissed version
    // (i.e., only a NEWER version than what was dismissed should trigger notification)
    return *check <= *dismissed;
}

void UpdateChecker::dismiss_current_version() {
    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cached_info_) {
            version = cached_info_->version;
        }
    }

    if (version.empty()) {
        spdlog::warn("[UpdateChecker] dismiss_current_version called without cached update");
        return;
    }

    auto* config = Config::get_instance();
    if (!config) {
        spdlog::error("[UpdateChecker] Cannot dismiss version: no config instance");
        return;
    }

    config->set<std::string>("/update/dismissed_version", version);
    config->save();
    spdlog::info("[UpdateChecker] Dismissed version: {}", version);

    // Add history-only notification so user can find the update later
    std::string msg = fmt::format(lv_tr("v{} is available. Tap to update."), version);
    ui_notification_info_with_action(lv_tr("Update Available"), msg.c_str(), "show_update_modal");
}

// ============================================================================
// Auto-Check Timer
// ============================================================================

void UpdateChecker::start_auto_check() {
    // Firmware-managed devices own updates externally — nothing to schedule. A
    // non-writable install tree still auto-checks: the notification tells the user
    // a newer version exists, which on that layout is the only prompt they will
    // ever get to go re-run the installer.
    if (update_checks_suppressed()) {
        spdlog::info("[UpdateChecker] Auto-check disabled: updates are firmware-managed");
        return;
    }

    if (auto_check_timer_) {
        spdlog::debug("[UpdateChecker] Auto-check timer already running");
        return;
    }

    // A machine that can check but cannot install. Report it: this state shipped
    // in v0.99.96 and was found in v0.99.113 only because one user kept pushing on
    // Discord — in aggregate it would have shown as a cohort of installs that
    // never once fetched a manifest. Emitted here rather than from init() because
    // TelemetryManager::init() runs after UpdateChecker::init() (application.cpp),
    // and after the auto_check_timer_ guard so a Moonraker reconnect does not
    // re-send it. Firmware-managed installs returned above and are not reported:
    // that is a deliberate configuration, not a fault.
    //
    // The context is a SHAPE, not a path. install_root embeds a username and
    // record_error()'s contract is pre-defined strings only; which of the two
    // writability terms was missing is the whole diagnostic value anyway.
    if (update_install_suppressed()) {
        const std::string root = app_get_install_root();
        const std::string parent = root.empty() ? std::string() : helix::paths::dirname(root);
        TelemetryManager::instance().record_error(
            "updates", "install_suppressed",
            fmt::format("parent_writable={},root_writable={},escalate={}",
                        !parent.empty() && helix::paths::probe_writable(parent) ? 1 : 0,
                        !root.empty() && helix::paths::probe_writable(root) ? 1 : 0,
                        root_escalation_available() ? 1 : 0));
        spdlog::warn("[UpdateChecker] Install is not self-updatable ({}); checking anyway so the "
                     "user can be told to re-run the installer",
                     suppression_reason());
    }

    spdlog::info("[UpdateChecker] Starting auto-check (15s initial delay, 24h periodic)");

    // One-shot 15s timer for initial check after startup
    auto_check_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<UpdateChecker*>(lv_timer_get_user_data(timer));
            if (self->shutting_down_.load())
                return;

            spdlog::info("[UpdateChecker] Auto-check: performing initial check");

            // Perform check with notification callback
            self->check_for_updates([self](Status status, std::optional<ReleaseInfo> info) {
                if (status != Status::UpdateAvailable || !info) {
                    return;
                }

                // Never raise a downgrade unprompted. It is only actionable
                // because the user chose this channel; interrupting them with
                // "go back to an older build" is noise at best, and a transient
                // bad manifest would push it to the whole fleet at once.
                if (info->is_downgrade) {
                    spdlog::info("[UpdateChecker] Auto-check: {} is a downgrade, not notifying",
                                 info->version);
                    return;
                }

                // Skip if version is dismissed
                if (self->is_version_dismissed(info->version)) {
                    spdlog::info("[UpdateChecker] Auto-check: version {} is dismissed",
                                 info->version);
                    return;
                }

                // Skip while a job owns the machine, Preparing included.
                const auto lifecycle = static_cast<PrintState>(
                    lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
                if (job_holds_machine(lifecycle)) {
                    spdlog::info("[UpdateChecker] Auto-check: skipping notification during print");
                    return;
                }

                // Guard against shutdown race (callback queued before shutdown)
                if (self->shutting_down_.load()) {
                    return;
                }

                // Populate release notes subject
                if (self->subjects_initialized_) {
                    lv_subject_copy_string(&self->release_notes_subject_,
                                           info->release_notes.c_str());
                    lv_subject_set_int(&self->changelog_visible_subject_, 0);
                }

                // Show notification modal
                self->show_update_notification();
            });

            // Convert to 24h periodic timer
            lv_timer_set_period(timer, 24u * 60u * 60u * 1000u);
            lv_timer_reset(timer);
        },
        15000, this);

    lv_timer_set_repeat_count(auto_check_timer_, -1); // infinite repeats
}

void UpdateChecker::cancel_auto_check_timer() {
    // Neuter rather than delete: the timer's own callback runs inside
    // lv_timer_handler, where deleting a timer can corrupt the list (#750, #751).
    // lv_timer_cancel_safe() also no-ops once LVGL is gone, which is what makes
    // this callable from the destructor.
    if (auto_check_timer_ && lv_is_initialized()) {
        helix::ui::lv_timer_cancel_safe(auto_check_timer_);
    }
    auto_check_timer_ = nullptr;
}

void UpdateChecker::stop_auto_check() {
    if (auto_check_timer_) {
        cancel_auto_check_timer();
        spdlog::debug("[UpdateChecker] Auto-check timer stopped");
    }
}

// ============================================================================
// Notification Modal Callbacks
// ============================================================================

static void on_update_notify_install(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_notify_install");
    spdlog::info("[UpdateChecker] User chose to install update");
    UpdateChecker::instance().hide_update_notification();
    helix::settings::get_about_settings_overlay().show_update_download_modal(
        /*start_immediately=*/true);
    LVGL_SAFE_EVENT_CB_END();
}

static void on_update_notify_ignore(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_notify_ignore");
    spdlog::info("[UpdateChecker] User chose to ignore update");
    UpdateChecker::instance().dismiss_current_version();
    UpdateChecker::instance().hide_update_notification();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_update_notify_close(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_notify_close");
    spdlog::info("[UpdateChecker] User closed update notification (remind later)");
    UpdateChecker::instance().hide_update_notification();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_update_toggle_changelog(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_toggle_changelog");
    auto* subject = UpdateChecker::instance().changelog_visible_subject();
    int current = lv_subject_get_int(subject);
    lv_subject_set_int(subject, current ? 0 : 1);
    LVGL_SAFE_EVENT_CB_END();
}

static bool s_notify_callbacks_registered = false;

static void register_notify_callbacks() {
    if (s_notify_callbacks_registered)
        return;
    lv_xml_register_event_cb(nullptr, "on_update_notify_install", on_update_notify_install);
    lv_xml_register_event_cb(nullptr, "on_update_notify_ignore", on_update_notify_ignore);
    lv_xml_register_event_cb(nullptr, "on_update_notify_close", on_update_notify_close);
    lv_xml_register_event_cb(nullptr, "on_update_toggle_changelog", on_update_toggle_changelog);
    s_notify_callbacks_registered = true;
    spdlog::debug("[UpdateChecker] Notification callbacks registered");
}

void UpdateChecker::show_update_notification() {
    spdlog::info("[UpdateChecker] Show update notification");
    if (!notify_modal_) {
        notify_modal_ = helix::ui::modal_show("update_notify_modal");
    }
}

void UpdateChecker::hide_update_notification() {
    if (notify_modal_) {
        helix::ui::modal_hide(notify_modal_);
        notify_modal_ = nullptr;
    }
}

std::string UpdateChecker::get_r2_base_url() const {
    return cached_r2_base_url_;
}

bool UpdateChecker::fetch_r2_manifest(const std::string& channel, ReleaseInfo& info,
                                      std::string& error) {
    std::string base = get_r2_base_url();
    if (base.empty()) {
        error = "R2 base URL not configured";
        return false;
    }

    std::string manifest_url = base + "/" + channel + "/manifest.json";

    spdlog::debug("[UpdateChecker] Requesting R2 manifest: {}", manifest_url);
    auto [status, body] = do_http_get(manifest_url);

    if (cancelled_)
        return false;

    if (status == 0) {
        error = "Connection failed (" + manifest_url + ")";
        return false;
    }

    if (status != 200) {
        error = "R2 HTTP " + std::to_string(status);
        return false;
    }

    // Parse manifest (same format as dev channel - generated by generate-manifest.sh)
    try {
        auto j = json::parse(body);

        info.version = json_string_or_empty(j, "version");
        if (info.version.empty()) {
            error = "Missing 'version' field in R2 manifest";
            return false;
        }

        info.tag_name = json_string_or_empty(j, "tag");
        info.release_notes = json_string_or_empty(j, "notes");
        info.published_at = json_string_or_empty(j, "published_at");

        if (!j.contains("assets") || !j["assets"].is_object() || j["assets"].empty()) {
            error = "Missing or empty 'assets' in R2 manifest";
            return false;
        }

        std::string platform = get_platform_key();
        const auto& assets = j["assets"];
        if (!assets.contains(platform)) {
            error = "No asset for platform '" + platform + "' in R2 manifest";
            return false;
        }

        populate_release_urls_from_manifest(assets[platform], info);

        spdlog::debug("[UpdateChecker] R2 manifest parsed: {} ({})", info.version, channel);
        return true;

    } catch (const json::exception& e) {
        error = std::string("R2 JSON parse error: ") + e.what();
        return false;
    }
}

bool UpdateChecker::fetch_stable_release(ReleaseInfo& info, std::string& error) {
    // Try R2 CDN first (manifest has version/assets, but notes may be sparse)
    if (fetch_r2_manifest("stable", info, error)) {
        // Enrich with full changelog from CHANGELOG.md on GitHub
        auto changelog = fetch_changelog_for_version(info.version);
        if (!changelog.empty()) {
            info.release_notes = std::move(changelog);
        }
        return true;
    }
    std::string r2_error = error;
    spdlog::debug("[UpdateChecker] R2 stable fetch failed ({}), falling back to GitHub", r2_error);
    error.clear();

    spdlog::debug("[UpdateChecker] Requesting: {}", GITHUB_API_URL);
    auto [status, body] = do_http_get(GITHUB_API_URL, "application/vnd.github.v3+json");

    if (cancelled_)
        return false;

    if (status == 0) {
        error = "Connection failed (R2 + GitHub)";
        return false;
    }

    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }

    return parse_github_release(body, info, error);
}

bool UpdateChecker::fetch_beta_release(ReleaseInfo& info, std::string& error) {
    // Try R2 CDN first (manifest has version/assets, but notes may be sparse)
    if (fetch_r2_manifest("beta", info, error)) {
        // Enrich with full changelog from CHANGELOG.md on GitHub
        auto changelog = fetch_changelog_for_version(info.version);
        if (!changelog.empty()) {
            info.release_notes = std::move(changelog);
        }
        return true;
    }
    std::string r2_error = error;
    spdlog::debug("[UpdateChecker] R2 beta fetch failed ({}), falling back to GitHub", r2_error);
    error.clear();

    spdlog::debug("[UpdateChecker] Requesting (beta): {}", GITHUB_RELEASES_URL);
    auto [status, body] = do_http_get(GITHUB_RELEASES_URL, "application/vnd.github.v3+json");

    if (cancelled_)
        return false;

    if (status == 0) {
        error = "Connection failed (R2 + GitHub)";
        return false;
    }

    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }

    // Parse JSON array of releases
    try {
        auto releases = json::parse(body);

        if (!releases.is_array() || releases.empty()) {
            error = "Empty or invalid releases array";
            return false;
        }

        // First pass: find latest prerelease (GitHub returns newest-first)
        for (const auto& rel : releases) {
            if (rel.value("draft", false))
                continue;
            if (!rel.value("prerelease", false))
                continue;

            if (parse_github_release(rel, info, error)) {
                spdlog::debug("[UpdateChecker] Beta: selected prerelease {}", info.tag_name);
                return true;
            }
        }

        // Fallback: no prerelease found, use latest stable
        for (const auto& rel : releases) {
            if (rel.value("draft", false))
                continue;
            if (parse_github_release(rel, info, error)) {
                spdlog::debug("[UpdateChecker] Beta: no prerelease found, falling back to {}",
                              info.tag_name);
                return true;
            }
        }

        error = "No valid releases found";
        return false;

    } catch (const json::exception& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }
}

bool UpdateChecker::fetch_dev_release(ReleaseInfo& info, std::string& error) {
    // If dev_url is explicitly set, use it directly (backward compat)
    std::string dev_url = cached_dev_url_;
    if (!dev_url.empty()) {
        // Validate URL scheme
        if (dev_url.find("http://") != 0 && dev_url.find("https://") != 0) {
            error = "Dev URL must use http:// or https:// scheme";
            return false;
        }

        // Ensure trailing slash
        if (dev_url.back() != '/') {
            dev_url += '/';
        }
        std::string manifest_url = dev_url + "manifest.json";

        spdlog::debug("[UpdateChecker] Requesting (dev): {}", manifest_url);
        auto [status, body] = do_http_get(manifest_url);

        if (cancelled_)
            return false;

        if (status == 0) {
            error = "Connection failed (" + manifest_url + ")";
            return false;
        }

        if (status != 200) {
            error = "HTTP " + std::to_string(status) + " (" + manifest_url + ")";
            return false;
        }

        // Parse dev manifest
        try {
            auto j = json::parse(body);

            info.version = json_string_or_empty(j, "version");
            if (info.version.empty()) {
                error = "Missing 'version' field in manifest";
                return false;
            }

            info.tag_name = json_string_or_empty(j, "tag");
            info.release_notes = json_string_or_empty(j, "notes");
            info.published_at = json_string_or_empty(j, "published_at");

            if (!j.contains("assets") || !j["assets"].is_object() || j["assets"].empty()) {
                error = "Missing or empty 'assets' in manifest";
                return false;
            }

            std::string platform = get_platform_key();
            const auto& assets = j["assets"];
            if (!assets.contains(platform)) {
                error = "No asset for platform '" + platform + "'";
                return false;
            }

            populate_release_urls_from_manifest(assets[platform], info);

            return true;

        } catch (const json::exception& e) {
            error = std::string("JSON parse error: ") + e.what();
            return false;
        }
    }

    // No explicit dev_url -- use R2 default
    return fetch_r2_manifest("dev", info, error);
}

void UpdateChecker::report_result(Status status, std::optional<ReleaseInfo> info,
                                  const std::string& error) {
    // Don't report if cancelled
    if (cancelled_ || shutting_down_) {
        spdlog::debug("[UpdateChecker] Skipping result report (cancelled/shutting down)");
        return;
    }

    // Update state under lock
    Callback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
        error_message_ = error;

        if (status == Status::UpdateAvailable && info) {
            cached_info_ = info;
        } else if (status == Status::UpToDate) {
            // Clear cached info when up to date
            cached_info_.reset();
        }
        // On Error, keep previous cached_info_ in case it was valid

        callback = pending_callback_;
    }

    // Dispatch to LVGL thread for subject updates and callback
    spdlog::debug("[UpdateChecker] Dispatching to LVGL thread");
    async_lifetime_.defer(
        "UpdateChecker::do_check_complete", [this, callback, status, info, error]() {
            spdlog::debug("[UpdateChecker] Executing on LVGL thread");

            // Update LVGL subjects
            if (subjects_initialized_) {
                lv_subject_set_int(&status_subject_, static_cast<int>(status));

                if (status == Status::UpdateAvailable && info) {
                    // A downgrade is not "available" in the usual sense — say
                    // what it actually does, so the row does not read as a
                    // routine update when the channel is behind this install.
                    snprintf(version_text_buf_, sizeof(version_text_buf_),
                             info->is_downgrade ? lv_tr("Switch to v%s") : lv_tr("v%s available"),
                             info->version.c_str());
                    lv_subject_copy_string(&version_text_subject_, version_text_buf_);
                    lv_subject_copy_string(&new_version_subject_, info->version.c_str());
                } else if (status == Status::UpToDate) {
                    lv_subject_copy_string(&version_text_subject_, lv_tr("Up to date"));
                    lv_subject_copy_string(&new_version_subject_, "");
                } else if (status == Status::Error) {
                    snprintf(version_text_buf_, sizeof(version_text_buf_), lv_tr("Error: %s"),
                             error.c_str());
                    lv_subject_copy_string(&version_text_subject_, version_text_buf_);
                    lv_subject_copy_string(&new_version_subject_, "");
                }
            }

            // Execute callback if present
            if (callback) {
                callback(status, info);
            }
        });
}
