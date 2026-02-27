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
#include "ui_update_queue.h"

#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "hv/requests.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"
#include "system/telemetry_manager.h"
#include "version.h"

#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
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

/// Shorthand for pre-update backup paths (defined in app_constants.h)
constexpr const char* PREUPDATE_CONFIG_BACKUP = AppConstants::Update::PREUPDATE_CONFIG_BACKUP;
constexpr const char* PREUPDATE_ENV_BACKUP = AppConstants::Update::PREUPDATE_ENV_BACKUP;

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

    // Find platform-specific binary asset
    if (j.contains("assets") && j["assets"].is_array()) {
        std::string platform_prefix = "helixscreen-" + UpdateChecker::get_platform_key() + "-";
        spdlog::info("[UpdateChecker] Platform key: '{}', looking for prefix '{}'",
                     UpdateChecker::get_platform_key(), platform_prefix);
        for (const auto& asset : j["assets"]) {
            std::string name = asset.value("name", "");
            if (name.find(platform_prefix) == 0 && name.find(".tar.gz") != std::string::npos) {
                info.download_url = asset.value("browser_download_url", "");
                spdlog::info("[UpdateChecker] Selected asset: {}", name);
                break;
            }
        }
        // No fallback to arbitrary .tar.gz — wrong-platform binaries can brick devices
        if (info.download_url.empty()) {
            spdlog::warn("[UpdateChecker] No asset found for platform '{}' in release {}",
                         UpdateChecker::get_platform_key(), info.version);
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

    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = HTTP_TIMEOUT_SECONDS;
    req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;

    spdlog::debug("[UpdateChecker] Fetching CHANGELOG.md for v{}", version);
    auto resp = requests::request(req);

    if (!resp || resp->status_code != 200) {
        spdlog::debug("[UpdateChecker] CHANGELOG.md fetch failed (HTTP {})",
                      resp ? resp->status_code : 0);
        return "";
    }

    auto section = extract_changelog_section(resp->body, version);
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
std::string resolve_tool(const std::string& name) {
    static const char* const SEARCH_DIRS[] = {"/usr/bin", "/bin",           "/usr/sbin",
                                              "/sbin",    "/usr/local/bin", nullptr};
    for (int i = 0; SEARCH_DIRS[i]; ++i) {
        std::string path = std::string(SEARCH_DIRS[i]) + "/" + name;
        if (access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }
    spdlog::warn("[UpdateChecker] resolve_tool: '{}' not found in standard paths, using bare name",
                 name);
    return name; // fallback: rely on PATH
}

/**
 * @brief Resolve the install root directory from /proc/self/exe.
 *
 * Strips /bin/helix-screen from the exe path to get the install root
 * (e.g., /home/pbrown/helixscreen).  Returns empty string on failure.
 */
std::string resolve_install_root() {
    char exe_buf[PATH_MAX] = {};
    ssize_t exe_len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (exe_len <= 0) {
        return {};
    }
    exe_buf[exe_len] = '\0';
    std::string exe_path(exe_buf);
    auto slash = exe_path.rfind('/');
    if (slash == std::string::npos) {
        return {};
    }
    std::string bin_dir = exe_path.substr(0, slash);
    if (bin_dir.size() >= 4 && bin_dir.substr(bin_dir.size() - 4) == "/bin") {
        return bin_dir.substr(0, bin_dir.size() - 4);
    }
    return {};
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
    std::string install_root = resolve_install_root();
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

    // Clean up .pre-update safety net files from a successful previous update.
    // These live in /var/log/ (outside INSTALL_DIR) so they survive the swap.
    // If we're running, the update succeeded and we no longer need them.
    std::remove(PREUPDATE_CONFIG_BACKUP);
    std::remove(PREUPDATE_ENV_BACKUP);
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

} // anonymous namespace

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

    // Clean up stale .old backup from a previous in-app update that may have
    // failed to remove it (e.g., NoNewPrivileges blocked sudo rm).
    cleanup_stale_old_install();

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

    // Clear callback to prevent stale references
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_callback_ = nullptr;
    }

    // Cleanup subjects
    if (subjects_initialized_) {
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
    UI_MANAGED_SUBJECT_INT(checking_subject_, 0, "update_checking", subjects_);
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
lv_subject_t* UpdateChecker::checking_subject() {
    return &checking_subject_;
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

// Minimum free space required to attempt download (50 MB)
static constexpr size_t MIN_DOWNLOAD_SPACE_BYTES = 50ULL * 1024 * 1024;

static const char* const DOWNLOAD_FILENAME = "helixscreen-update.tar.gz";

// Check if a directory is writable and return available bytes (0 on failure)
static size_t get_available_space(const std::string& dir) {
    struct statvfs stat {};
    if (statvfs(dir.c_str(), &stat) != 0) {
        return 0;
    }
    // Use f_bavail (blocks available to unprivileged users) * fragment size
    return static_cast<size_t>(stat.f_bavail) * stat.f_frsize;
}

// Check if we can actually write to a directory
static bool is_writable_dir(const std::string& dir) {
    return access(dir.c_str(), W_OK) == 0;
}

std::string UpdateChecker::get_download_path() const {
    // Candidate directories, checked exhaustively — we pick the one with
    // the MOST free space so we don't fill up a tiny tmpfs or crowd out
    // gcode storage on an embedded device.
    std::vector<std::string> candidates;

    // Environment variables first
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

    // Evaluate all candidates — pick the one with the most free space
    std::string best_dir;
    size_t best_space = 0;

    for (const auto& dir : candidates) {
        if (!is_writable_dir(dir)) {
            continue;
        }

        auto space = get_available_space(dir);
        if (space < MIN_DOWNLOAD_SPACE_BYTES) {
            spdlog::debug("[UpdateChecker] Skipping {} ({:.1f} MB free, need {:.0f} MB)", dir,
                          static_cast<double>(space) / (1024.0 * 1024.0),
                          static_cast<double>(MIN_DOWNLOAD_SPACE_BYTES) / (1024.0 * 1024.0));
            continue;
        }

        if (space > best_space) {
            best_space = space;
            best_dir = dir;
        }
    }

    if (best_dir.empty()) {
        spdlog::error("[UpdateChecker] No writable directory with {} MB free space",
                      MIN_DOWNLOAD_SPACE_BYTES / (1024 * 1024));
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

std::string UpdateChecker::get_platform_asset_name() const {
    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version = cached_info_ ? cached_info_->tag_name : "";
    }
    return "helixscreen-" + get_platform_key() + "-" + version + ".tar.gz";
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

    helix::ui::queue_update([this, status, progress, text]() {
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

    // Safety: refuse download while printing
    auto job_state = get_printer_state().get_print_job_state();
    if (job_state == PrintJobState::PRINTING || job_state == PrintJobState::PAUSED) {
        spdlog::warn("[UpdateChecker] Cannot download update while printing");
        report_download_status(DownloadStatus::Error, 0, "Error: Cannot update while printing",
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
        report_download_status(DownloadStatus::Error, 0, "Error: No update available",
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
    report_download_status(DownloadStatus::Downloading, 0, "Starting download...");

    download_thread_ = std::thread(&UpdateChecker::do_download, this);
}

void UpdateChecker::cancel_download() {
    download_cancelled_ = true;
}

void UpdateChecker::do_download() {
    std::string url;
    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cached_info_)
            return;
        url = cached_info_->download_url;
        version = cached_info_->version;
    }

    auto download_path = get_download_path();
    if (download_path.empty()) {
        report_download_status(DownloadStatus::Error, 0, "Error: No space for download",
                               "Could not find a writable directory with enough free space");
        TelemetryManager::instance().record_update_failure("no_disk_space", version,
                                                           get_platform_key());
        return;
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
            auto text = fmt::format("Downloading... {:.1f}/{:.1f} MB", mb_received, mb_total);
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
        report_download_status(DownloadStatus::Error, 0, "Error: Download failed",
                               "Failed to download update file");
        TelemetryManager::instance().record_update_failure("download_failed", version,
                                                           get_platform_key());
        return;
    }

    // Verify file size sanity (reject < 1MB or > 150MB)
    if (result < 1024 * 1024) {
        spdlog::error("[UpdateChecker] Downloaded file too small: {} bytes", result);
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, "Error: Invalid download",
                               "Downloaded file is too small");
        TelemetryManager::instance().record_update_failure(
            "file_too_small", version, get_platform_key(), -1, static_cast<int64_t>(result));
        return;
    }
    if (result > 150 * 1024 * 1024) {
        spdlog::error("[UpdateChecker] Downloaded file too large: {} bytes", result);
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, "Error: Invalid download",
                               "Downloaded file is too large");
        TelemetryManager::instance().record_update_failure(
            "file_too_large", version, get_platform_key(), -1, static_cast<int64_t>(result));
        return;
    }

    spdlog::info("[UpdateChecker] Download complete: {} bytes", result);
    report_download_status(DownloadStatus::Verifying, 100, "Verifying download...");

    // Verify gzip integrity (fork/exec to avoid shell injection)
    auto ret = safe_exec({resolve_tool("gunzip"), "-t", download_path});
    if (ret != 0) {
        spdlog::error("[UpdateChecker] Tarball verification failed");
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, "Error: Corrupt download",
                               "Downloaded file failed integrity check");
        TelemetryManager::instance().record_update_failure(
            "corrupt_download", version, get_platform_key(), -1, static_cast<int64_t>(result));
        return;
    }

    spdlog::info("[UpdateChecker] Tarball verified OK");

    // Validate architecture before installing
    if (!validate_elf_architecture(download_path)) {
        spdlog::error("[UpdateChecker] Downloaded update is for wrong architecture!");
        std::remove(download_path.c_str());
        report_download_status(DownloadStatus::Error, 0, "Error: Wrong architecture",
                               "Downloaded binary doesn't match this device's architecture");
        TelemetryManager::instance().record_update_failure("wrong_architecture", version,
                                                           get_platform_key());
        return;
    }

    do_install(download_path);
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
    } else {
        spdlog::info("[UpdateChecker] Platform '{}' — skipping ELF validation", platform);
        return true;
    }

    // Extract binary to temp location for inspection
    std::string temp_dir = tarball_path + ".validate";
    mkdir(temp_dir.c_str(), 0750);

    const std::string rm_bin = resolve_tool("rm");

    // Extract binary from tarball for inspection
    auto ret = extract_tar_member(tarball_path, temp_dir, "helixscreen/bin/helix-screen");
    if (ret != 0) {
        spdlog::warn("[UpdateChecker] Could not extract binary for validation, skipping");
        safe_exec({rm_bin, "-rf", temp_dir});
        return true;
    }

    std::string binary_path = temp_dir + "/helixscreen/bin/helix-screen";

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

    report_download_status(DownloadStatus::Installing, 100, "Installing update...");

    // Safety net: copy config to /var/log/ BEFORE calling install.sh.
    // The installer backs up config to TMP_DIR, but under systemd's PrivateTmp=true
    // that backup lives in a volatile mount that's cleaned up on service restart.
    // If install.sh crashes between the atomic swap and config restore, the TMP_DIR
    // backup is lost.
    //
    // Why /var/log/? The installer does `mv INSTALL_DIR → INSTALL_DIR.old`, so any
    // backup inside INSTALL_DIR/ gets swept into .old/.  Under ProtectSystem=strict,
    // /var/log is the only writable path outside INSTALL_DIR (from ReadWritePaths).
    // Config::init() auto-restores from these files if the config is missing.
    {
        std::string install_root = resolve_install_root();
        if (!install_root.empty()) {
            std::string config_src = install_root + "/config/helixconfig.json";
            std::string env_src = install_root + "/config/helixscreen.env";
            const std::string cp_bin = resolve_tool("cp");

            struct stat st {};
            if (stat(config_src.c_str(), &st) == 0) {
                int ret = safe_exec({cp_bin, "-f", config_src, PREUPDATE_CONFIG_BACKUP});
                if (ret == 0) {
                    flog_info("[UpdateChecker] Pre-update config backup: {}",
                              PREUPDATE_CONFIG_BACKUP);
                } else {
                    flog_warn("[UpdateChecker] Failed to create pre-update config backup (exit {})",
                              ret);
                }
            }
            if (stat(env_src.c_str(), &st) == 0) {
                int ret = safe_exec({cp_bin, "-f", env_src, PREUPDATE_ENV_BACKUP});
                if (ret == 0) {
                    flog_info("[UpdateChecker] Pre-update env backup: {}", PREUPDATE_ENV_BACKUP);
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
        report_download_status(DownloadStatus::Error, 0, "Error: Installer not found",
                               "Cannot locate install.sh script");
        TelemetryManager::instance().record_update_failure("installer_not_found", version,
                                                           get_platform_key());
        return;
    }

    // Write installer output to a persistent log file so it survives even if this
    // process is killed mid-install (e.g. stop_service kills the cgroup).
    std::string install_log = tarball_path + ".install.log";

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

    // Fork install.sh with its output redirected to a persistent log file.
    // Using a file instead of a pipe means we get the full output even if this
    // process is killed by systemd's stop_service during the install step.
    int ret = -1;
    bool timed_out = false;
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
            const char* argv[] = {install_script.c_str(), "--local", tarball_path.c_str(),
                                  "--update", nullptr};
            execv(install_script.c_str(), const_cast<char**>(argv));
            _exit(127);
        } else {
            // Parent: close our copy of the log fd and wait with timeout
            flog_info("[UpdateChecker] Forked install.sh as pid={}, waiting...", pid);
            if (log_fd >= 0)
                close(log_fd);

            constexpr int timeout_seconds = 120;
            int status = 0;
            bool exited = false;

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
                // Log progress every 10 seconds so we can see the wait is active
                if (elapsed > 0 && elapsed % 10 == 0) {
                    flog_info("[UpdateChecker] Still waiting for install.sh pid={} ({}s/{}s)", pid,
                              elapsed, timeout_seconds);
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

        // Read back the install log and emit every line through spdlog
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
            }
            flog_info("[UpdateChecker] ---- end install.sh output ({} lines) ----", line_count);
            fclose(lf);
        } else {
            flog_error("[UpdateChecker] Could not read install log {}: {}", install_log,
                       strerror(errno));
        }
        std::remove(install_log.c_str());
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
        report_download_status(DownloadStatus::Error, 0, "Error: Installation failed",
                               "install.sh returned error code " + std::to_string(ret));
        std::string reason = timed_out ? "install_timeout" : "install_failed";
        TelemetryManager::instance().record_update_failure(reason, version, get_platform_key(), -1,
                                                           -1, ret);
        return;
    }

    spdlog::info("[UpdateChecker] Update installed successfully!");

    // Write update success flag for telemetry (picked up on next boot)
    TelemetryManager::write_update_success_flag("config", version, HELIX_VERSION,
                                                get_platform_key());

    report_download_status(DownloadStatus::Complete, 100,
                           "v" + version + " installed! Restarting...");

    // Trigger self-restart: _exit(0) causes the watchdog to restart helix-screen
    // with the newly installed binary ("Normal exit (code 0) — restart silently").
    // install.sh skips systemctl restart under NoNewPrivileges, so we own the restart.
    //
    // _exit() not exit(): calling exit() from a background thread races with
    // destructors on other threads (LVGL loop, WebSocket threads) and causes
    // SIGSEGV (code 139). The watchdog treats that as a crash, shows the recovery
    // dialog, and Config::init() recreates helixconfig.json from defaults — wiping
    // dev_url and channel settings. _exit() terminates immediately at the OS level
    // with no C++ cleanup, so exit code 0 reaches the watchdog cleanly.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        spdlog::info("[UpdateChecker] Restarting to apply update");
        spdlog::default_logger()->flush();
        ::_exit(0);
    }).detach();
}

// Done with flush-on-write macros — only needed in do_install()
#undef flog_info
#undef flog_warn
#undef flog_error
#undef flog_debug
#undef FLOG

// ============================================================================
// Static helpers
// ============================================================================

std::string UpdateChecker::extract_installer_from_tarball(const std::string& tarball_path,
                                                          const std::string& extract_dir) {
    std::string tar_member = std::string("helixscreen/") + INSTALLER_FILENAME;

    auto ext_ret = extract_tar_member(tarball_path, extract_dir, tar_member);

    std::string installer = extract_dir + "/helixscreen/" + INSTALLER_FILENAME;
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

    // Try resolving from /proc/self/exe → strip /bin/helix-screen → install root
    char exe_buf[PATH_MAX] = {};
    ssize_t exe_len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (exe_len > 0) {
        exe_buf[exe_len] = '\0';
        std::string exe_dir(exe_buf);
        auto slash = exe_dir.rfind('/');
        if (slash != std::string::npos) {
            exe_dir = exe_dir.substr(0, slash); // strip binary name → bin/
            if (exe_dir.size() >= 4 && exe_dir.substr(exe_dir.size() - 4) == "/bin") {
                std::string install_root = exe_dir.substr(0, exe_dir.size() - 4);
                search_paths.push_back(install_root + "/" + INSTALLER_FILENAME);
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
    cached_r2_base_url_ = config ? config->get<std::string>("/update/r2_url", "") : "";
    if (cached_r2_base_url_.empty()) {
        cached_r2_base_url_ = DEFAULT_R2_BASE_URL;
    }
    // Normalize: strip trailing slash
    if (!cached_r2_base_url_.empty() && cached_r2_base_url_.back() == '/') {
        cached_r2_base_url_.pop_back();
    }

    const char* channel_name = (cached_channel_ == UpdateChannel::Beta)  ? "beta"
                               : (cached_channel_ == UpdateChannel::Dev) ? "dev"
                                                                         : "stable";
    spdlog::debug("[UpdateChecker] check_for_updates: channel={} dev_url='{}' r2_base_url='{}'",
                  channel_name, cached_dev_url_.empty() ? "(none)" : cached_dev_url_,
                  cached_r2_base_url_);

    // Update subjects on LVGL thread (check_for_updates is public, could be called from any thread)
    if (subjects_initialized_) {
        helix::ui::queue_update([this]() {
            lv_subject_set_int(&checking_subject_, 1);
            lv_subject_set_int(&status_subject_, static_cast<int>(Status::Checking));
            lv_subject_copy_string(&version_text_subject_, "Checking...");
        });
    }

    // Spawn worker thread
    worker_thread_ = std::thread(&UpdateChecker::do_check, this);
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

// ============================================================================
// Worker Thread
// ============================================================================

void UpdateChecker::do_check() {
    spdlog::debug("[UpdateChecker] Worker thread started");

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

    if (is_update_available(current_version, info.version)) {
        spdlog::info("[UpdateChecker] Update available: {} -> {}", current_version, info.version);
        report_result(Status::UpdateAvailable, info, "");
    } else {
        spdlog::info("[UpdateChecker] Already up to date ({})", current_version);
        // Pass info even for UpToDate so callbacks (e.g., --release-notes) can access it
        report_result(Status::UpToDate, info, "");
    }

    spdlog::debug("[UpdateChecker] Worker thread finished");
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
    switch (channel) {
    case 1:
        return UpdateChannel::Beta;
    case 2:
        return UpdateChannel::Dev;
    default:
        return UpdateChannel::Stable;
    }
}

std::string UpdateChecker::get_platform_key() {
#ifdef HELIX_PLATFORM_AD5M
    return "ad5m";
#elif defined(HELIX_PLATFORM_CC1)
    return "cc1";
#elif defined(HELIX_PLATFORM_MIPS)
    // Same binary runs on K1 and AD5X — detect at runtime.
    // AD5X has /usr/prog (FlashForge layout), K1 does not.
    {
        struct stat st;
        if (stat("/usr/prog", &st) == 0 && S_ISDIR(st.st_mode)) {
            return "ad5x";
        }
        return "k1";
    }
#elif defined(HELIX_PLATFORM_K2)
    return "k2";
#elif defined(HELIX_PLATFORM_PI32)
    return "pi32";
#else
    return "pi";
#endif
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
    if (auto_check_timer_) {
        spdlog::debug("[UpdateChecker] Auto-check timer already running");
        return;
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

                // Skip if version is dismissed
                if (self->is_version_dismissed(info->version)) {
                    spdlog::info("[UpdateChecker] Auto-check: version {} is dismissed",
                                 info->version);
                    return;
                }

                // Skip if printer is printing or paused
                auto job_state = get_printer_state().get_print_job_state();
                if (job_state == PrintJobState::PRINTING || job_state == PrintJobState::PAUSED) {
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

void UpdateChecker::stop_auto_check() {
    if (auto_check_timer_) {
        lv_timer_delete(auto_check_timer_);
        auto_check_timer_ = nullptr;
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
    get_global_settings_panel().show_update_download_modal();
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

    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = manifest_url;
    req->timeout = HTTP_TIMEOUT_SECONDS;
    req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;

    spdlog::debug("[UpdateChecker] Requesting R2 manifest: {}", manifest_url);
    auto resp = requests::request(req);

    if (cancelled_)
        return false;

    if (!resp) {
        error = "Connection failed (" + manifest_url + ")";
        return false;
    }

    if (resp->status_code != 200) {
        error = "R2 HTTP " + std::to_string(resp->status_code);
        return false;
    }

    // Parse manifest (same format as dev channel - generated by generate-manifest.sh)
    try {
        auto j = json::parse(resp->body);

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

        const auto& platform_asset = assets[platform];
        info.download_url = json_string_or_empty(platform_asset, "url");
        info.sha256 = json_string_or_empty(platform_asset, "sha256");

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

    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = GITHUB_API_URL;
    req->timeout = HTTP_TIMEOUT_SECONDS;
    req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;
    req->headers["Accept"] = "application/vnd.github.v3+json";

    spdlog::debug("[UpdateChecker] Requesting: {}", GITHUB_API_URL);
    auto resp = requests::request(req);

    if (cancelled_)
        return false;

    if (!resp) {
        error = "Connection failed (R2 + GitHub)";
        return false;
    }

    if (resp->status_code != 200) {
        const char* status_msg = resp->status_message();
        error = "HTTP " + std::to_string(resp->status_code);
        if (status_msg && status_msg[0] != '\0') {
            error += ": ";
            error += status_msg;
        }
        return false;
    }

    return parse_github_release(resp->body, info, error);
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

    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = GITHUB_RELEASES_URL;
    req->timeout = HTTP_TIMEOUT_SECONDS;
    req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;
    req->headers["Accept"] = "application/vnd.github.v3+json";

    spdlog::debug("[UpdateChecker] Requesting (beta): {}", GITHUB_RELEASES_URL);
    auto resp = requests::request(req);

    if (cancelled_)
        return false;

    if (!resp) {
        error = "Connection failed (R2 + GitHub)";
        return false;
    }

    if (resp->status_code != 200) {
        const char* status_msg = resp->status_message();
        error = "HTTP " + std::to_string(resp->status_code);
        if (status_msg && status_msg[0] != '\0') {
            error += ": ";
            error += status_msg;
        }
        return false;
    }

    // Parse JSON array of releases
    try {
        auto releases = json::parse(resp->body);

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

        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = manifest_url;
        req->timeout = HTTP_TIMEOUT_SECONDS;
        req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;

        spdlog::debug("[UpdateChecker] Requesting (dev): {}", manifest_url);
        auto resp = requests::request(req);

        if (cancelled_)
            return false;

        if (!resp) {
            error = "Connection failed (" + manifest_url + ")";
            return false;
        }

        if (resp->status_code != 200) {
            error = "HTTP " + std::to_string(resp->status_code) + " (" + manifest_url + ")";
            return false;
        }

        // Parse dev manifest
        try {
            auto j = json::parse(resp->body);

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

            const auto& platform_asset = assets[platform];
            info.download_url = json_string_or_empty(platform_asset, "url");
            info.sha256 = json_string_or_empty(platform_asset, "sha256");

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
    helix::ui::queue_update([this, callback, status, info, error]() {
        spdlog::debug("[UpdateChecker] Executing on LVGL thread");

        // Update LVGL subjects
        if (subjects_initialized_) {
            lv_subject_set_int(&status_subject_, static_cast<int>(status));
            lv_subject_set_int(&checking_subject_, 0); // Done checking

            if (status == Status::UpdateAvailable && info) {
                snprintf(version_text_buf_, sizeof(version_text_buf_), "v%s available",
                         info->version.c_str());
                lv_subject_copy_string(&version_text_subject_, version_text_buf_);
                lv_subject_copy_string(&new_version_subject_, info->version.c_str());
            } else if (status == Status::UpToDate) {
                lv_subject_copy_string(&version_text_subject_, "Up to date");
                lv_subject_copy_string(&new_version_subject_, "");
            } else if (status == Status::Error) {
                snprintf(version_text_buf_, sizeof(version_text_buf_), "Error: %s", error.c_str());
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
