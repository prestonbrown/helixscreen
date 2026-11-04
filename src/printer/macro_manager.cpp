// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_manager.h"

#include "core/data_root_resolver.h"
#include "core/text_io.h"
#include "helix_regex.h"
#include "preprint_skip_wrappers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

namespace helix {

// ============================================================================
// File Loading Helpers
// ============================================================================

namespace {

/**
 * @brief Load macro content from config file
 * @return File content, or empty string if not found
 *
 * Resolves via find_readable: writable config dir (user override), then
 * shipped seed under HELIX_DATA_DIR/assets/config/. Falls back to
 * /opt/helixscreen/config/ for legacy AD5M Klipper-mod installs.
 */
std::string load_macro_file() {
    const std::vector<std::string> paths = {
        helix::find_readable("helix_macros.cfg"),
        "/opt/helixscreen/config/helix_macros.cfg", // Legacy AD5M install
    };

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            spdlog::debug("[MacroManager] Loaded macro file from {}", path);
            return buffer.str();
        }
    }

    spdlog::warn("[MacroManager] Could not find helix_macros.cfg in any expected location");
    return "";
}

/**
 * @brief Parse version from file header comment
 * @param content File content
 * @return Version string (e.g., "2.0.0"), or empty if not found
 *
 * Looks for pattern: # helix_macros v<version>
 */
std::string parse_file_version(const std::string& content) {
    static const helix::Regex version_pattern(R"(#\s*helix_macros\s+v(\d+\.\d+\.\d+))");
    helix::RegexMatch match;
    if (helix::regex_search(content, match, version_pattern)) {
        return match[1].str();
    }
    return "";
}

/**
 * @brief Parse macro names from file content
 * @param content File content
 * @return Vector of macro names
 */
std::vector<std::string> parse_macro_names(const std::string& content) {
    std::vector<std::string> names;
    // Anchored to line start: a real Klipper section header always starts in
    // column 0, so this does not also match one quoted inside a comment.
    static const helix::Regex macro_pattern(R"(^\[gcode_macro\s+(\w+)\])");

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        helix::RegexMatch match;
        if (helix::regex_search(line, match, macro_pattern)) {
            std::string name = match[1].str();
            // Skip internal state macros
            if (!name.empty() && name[0] != '_') {
                names.push_back(name);
            }
        }
    }

    return names;
}

/**
 * @brief Infer the installed pack version from which rung macros exist
 * @param hardware Discovery snapshot with the printer's macro names
 *
 * The installed pack exposes no queryable version, so infer it from which
 * macros exist - each rung is the newest macro whose presence brackets the
 * install. Adding a macro to the pack means adding a rung here (and a
 * matching version bump in helix_macros.cfg).
 */
std::optional<std::string> parse_installed_version(const PrinterDiscovery& hardware) {
    if (hardware.has_helix_macro("HELIX_UNLOAD_FILAMENT")) {
        return "2.1.0";
    }

    if (hardware.has_helix_macro("HELIX_READY")) {
        return "2.0.0";
    }

    // Check for legacy v1.x macros
    if (hardware.has_helix_macro("HELIX_START_PRINT")) {
        return "1.0.0";
    }

    return std::nullopt;
}

/**
 * @brief Timestamped sibling name for a printer.cfg backup
 *
 * The upload overwrites printer.cfg wholesale, so the pre-edit content must
 * be recoverable from the printer itself; a timestamp keeps every install's
 * backup distinct.
 */
std::string printer_cfg_backup_name() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[24] = {};
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_buf);
    return std::string("printer.cfg.helixbak-") + stamp;
}

std::string include_line(const std::string& filename) {
    return "[include " + filename + "]";
}

/// printer.cfg with the include placed after its last [include] line, or at
/// the very top when it has none.
std::string with_include_after_last(const std::string& content, const std::string& filename) {
    std::istringstream input(content);
    std::string line;
    size_t last_include_end = 0;
    size_t current_pos = 0;
    while (std::getline(input, line)) {
        current_pos += line.length() + 1; // +1 for newline
        std::string lower_line = helix::text_io::to_lower(line);
        if (lower_line.find("[include ") == 0 || lower_line.find("[include\t") == 0) {
            last_include_end = current_pos;
        }
    }
    if (last_include_end > 0) {
        return content.substr(0, last_include_end) + include_line(filename) + "\n" +
               content.substr(last_include_end);
    }
    return include_line(filename) + "\n" + content;
}

/// Whether printer.cfg has this include as a live line; a commented-out one
/// does not count.
bool has_include(const std::string& content, const std::string& filename) {
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        if (helix::text_io::trim(line) == include_line(filename)) {
            return true;
        }
    }
    return false;
}

/// printer.cfg with both includes the install needs. The skip wrappers go on
/// the very first line: Klipper merges same-named macro sections with the later
/// one winning, so every user macro must load after ours.
std::string with_includes(const std::string& content, bool with_skips) {
    std::string out = content;
    if (!has_include(out, kHelixMacrosFilename)) {
        out = with_include_after_last(out, kHelixMacrosFilename);
    }
    if (with_skips && !has_include(out, skip_wrappers::FILENAME)) {
        out = include_line(skip_wrappers::FILENAME) + "\n" + out;
    }
    return out;
}

/// printer.cfg without the live include lines for these files. Other lines,
/// commented-out includes among them, are kept byte for byte.
std::string without_includes(const std::string& content, const std::vector<std::string>& files) {
    std::string out;
    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find('\n', start);
        end = end == std::string::npos ? content.size() : end + 1;
        const std::string_view line(content.data() + start, end - start);
        const auto trimmed = helix::text_io::trim(line);
        const bool drop = std::any_of(files.begin(), files.end(), [&](const std::string& f) {
            return trimmed == include_line(f);
        });
        if (!drop) {
            out.append(line);
        }
        start = end;
    }
    return out;
}

} // anonymous namespace

// ============================================================================
// MacroManager Implementation
// ============================================================================

MacroManager::MacroManager(IMoonrakerAPI& api, const PrinterDiscovery& hardware)
    : api_(api), hardware_(hardware) {}

MacroManager::~MacroManager() = default;

bool MacroManager::is_installed() const {
    return hardware_.has_helix_macros();
}

MacroInstallStatus MacroManager::get_status() const {
    return evaluate_status(hardware_);
}

MacroInstallStatus MacroManager::evaluate_status(const PrinterDiscovery& hardware) {
    // No objects list consumed yet: the printer has not told us anything,
    // and "no helix macros found" in that vacuum would be a claim, not an
    // observation.
    if (!hardware.objects_reported()) {
        return MacroInstallStatus::UNKNOWN;
    }

    if (!hardware.has_helix_macros()) {
        return MacroInstallStatus::NOT_INSTALLED;
    }

    auto installed_version = parse_installed_version(hardware);
    if (!installed_version) {
        // Has macros but can't determine version - assume installed
        return MacroInstallStatus::INSTALLED;
    }

    // Compare against version from local file
    std::string local_version = get_version();
    if (local_version.empty()) {
        // Can't read local file - assume installed
        return MacroInstallStatus::INSTALLED;
    }

    if (version_less(*installed_version, local_version)) {
        return MacroInstallStatus::OUTDATED;
    }

    // A step the skip wrappers could wrap but the loaded file does not: the
    // file or its include is missing, or the printer has a leveling section the
    // file lacks. Updating stages it.
    const auto& active = hardware.skip_active();
    for (auto op : hardware.skip_wrappable()) {
        if (std::find(active.begin(), active.end(), op) == active.end()) {
            return MacroInstallStatus::OUTDATED;
        }
    }

    return MacroInstallStatus::INSTALLED;
}

bool MacroManager::version_less(const std::string& a, const std::string& b) {
    auto next_component = [](const std::string& v, size_t& pos) {
        if (pos == std::string::npos) {
            return 0L;
        }
        size_t end = v.find('.', pos);
        std::string part = v.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? end : end + 1;
        // Non-numeric components fall back to 0 rather than aborting the parse
        long value = 0;
        try {
            value = std::stol(part);
        } catch (const std::exception&) {
        }
        return value;
    };

    size_t pa = 0, pb = 0;
    while (pa != std::string::npos || pb != std::string::npos) {
        long ca = next_component(a, pa);
        long cb = next_component(b, pb);
        if (ca != cb) {
            return ca < cb;
        }
    }
    return false;
}

std::string MacroManager::get_installed_version() const {
    auto version = parse_installed_version(hardware_);
    return version.value_or("");
}

bool MacroManager::update_available() const {
    return get_status() == MacroInstallStatus::OUTDATED;
}

void MacroManager::install_files(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Staging macro files...");

    // upload_macro_file() lands both callbacks on the main thread, so this
    // continuation runs there. Every chain that follows hops its own terminal
    // callbacks to main too (see the queue pin test), because the caller's
    // continuation touches LVGL-adjacent state.
    upload_macro_file(
        kHelixMacrosFilename, macro_content_for(hardware_),
        [this, on_success, on_error]() {
            upload_skips_file(
                [this, on_success, on_error]() {
                    spdlog::info("[HelixMacroManager] Macro files uploaded, adding includes...");
                    add_include_to_config(stages_skips(), on_success, on_error);
                },
                on_error);
        },
        on_error);
}

void MacroManager::update_files(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Staging macro update...");

    upload_macro_file(
        kHelixMacrosFilename, macro_content_for(hardware_),
        [this, on_success, on_error]() {
            if (!stages_skips()) {
                if (on_success) {
                    on_success();
                }
                return;
            }
            // printer.cfg may still lack the helix_skips.cfg include.
            upload_skips_file([this, on_success,
                               on_error]() { add_include_to_config(true, on_success, on_error); },
                              on_error);
        },
        on_error);
}

bool MacroManager::stages_skips() const {
    return !hardware_.skip_wrappable().empty();
}

void MacroManager::request_restart(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Requesting Klipper restart...");

    // printer.restart completes on the WebSocket loop; the caller's
    // continuation touches LVGL-adjacent state, so hop it to main.
    api_.restart_klipper(
        lifetime_.bg_cb("MacroManager::restart_done",
                        [on_success]() {
                            spdlog::info("[HelixMacroManager] Klipper restart initiated");
                            if (on_success) {
                                on_success();
                            }
                        }),
        lifetime_.bg_cb("MacroManager::restart_failed", [on_error](const MoonrakerError& err) {
            spdlog::error("[HelixMacroManager] Klipper restart "
                          "request failed: {}",
                          err.message);
            if (on_error) {
                on_error(err);
            }
        }));
}

void MacroManager::uninstall(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Starting macro uninstall...");

    auto token = lifetime_.token();

    remove_include_from_config(
        {kHelixMacrosFilename, skip_wrappers::FILENAME},
        [this, token, on_success, on_error]() {
            // L081 Mechanism C: defer chained this-> work to main thread
            // (remove_include cb fires on HTTP bg thread).
            token.defer("MacroManager::uninstall_delete", [this, on_success, on_error]() {
                delete_macro_file(
                    kHelixMacrosFilename,
                    [this, on_success, on_error]() {
                        delete_macro_file(
                            skip_wrappers::FILENAME,
                            [this, on_success, on_error]() {
                                request_restart(on_success, on_error);
                            },
                            on_error);
                    },
                    on_error);
            });
        },
        on_error);
}

void MacroManager::remove_skips(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::warn("[HelixMacroManager] Removing {} and its include", skip_wrappers::FILENAME);

    auto token = lifetime_.token();
    remove_include_from_config(
        {skip_wrappers::FILENAME},
        [this, token, on_success, on_error]() {
            token.defer("MacroManager::remove_skips_delete", [this, on_success, on_error]() {
                delete_macro_file(skip_wrappers::FILENAME, on_success, on_error);
            });
        },
        on_error);
}

std::string MacroManager::get_macro_content() {
    return load_macro_file();
}

std::string MacroManager::with_visible_signal_names(std::string content) {
    for (const char* hidden : {"_HELIX_READY", "_HELIX_ENDED", "_HELIX_RESET", "_HELIX_PHASE_"}) {
        for (size_t pos = content.find(hidden); pos != std::string::npos;
             pos = content.find(hidden, pos)) {
            content.erase(pos, 1);
        }
    }
    return content;
}

std::string MacroManager::macro_content_for(const PrinterDiscovery& hardware) {
    std::string content = get_macro_content();
    if (hardware.has_helix_macro("HELIX_READY")) {
        return with_visible_signal_names(std::move(content));
    }
    return content;
}

std::string MacroManager::get_version() {
    std::string content = load_macro_file();
    return parse_file_version(content);
}

std::vector<std::string> MacroManager::get_macro_names() {
    std::string content = load_macro_file();
    if (content.empty()) {
        return {};
    }
    return parse_macro_names(content);
}

// ============================================================================
// Private Implementation
// ============================================================================

void MacroManager::upload_macro_file(const std::string& filename, const std::string& content,
                                     SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Uploading {} ({} bytes) to printer config directory",
                 filename, content.size());

    // Upload to config root (not gcodes)
    // The path is "" because we upload directly to the config directory.
    // Completion arrives on the HttpExecutor lane; every continuation in the
    // install/update chains touches api_ state or LVGL-adjacent subjects, so
    // both callbacks hop to the main thread here.
    api_.transfers().upload_file_with_name(
        "config", "", filename, content,
        lifetime_.bg_cb("MacroManager::upload_done",
                        [on_success, filename]() {
                            spdlog::info("[HelixMacroManager] Successfully uploaded {}", filename);
                            if (on_success) {
                                on_success();
                            }
                        }),
        lifetime_.bg_cb(
            "MacroManager::upload_failed", [on_error, filename](const MoonrakerError& err) {
                spdlog::error("[HelixMacroManager] Failed to upload {}: {}", filename, err.message);
                if (on_error) {
                    on_error(err);
                }
            }));
}

void MacroManager::upload_skips_file(SuccessCallback on_success, ErrorCallback on_error) {
    if (!stages_skips()) {
        if (on_success) {
            on_success();
        }
        return;
    }
    upload_macro_file(skip_wrappers::FILENAME, skip_wrappers::generate(hardware_.skip_wrappable()),
                      on_success, on_error);
}

void MacroManager::add_include_to_config(bool with_skips, SuccessCallback on_success,
                                         ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Adding include lines to printer.cfg");

    auto token = lifetime_.token();

    // Download printer.cfg
    api_.transfers().download_file(
        "config", "printer.cfg",
        // Download success — runs on HTTP bg thread.
        // L081 Mechanism C: do pure parsing/construction locally on BG, then defer
        // the api_-> kick-off to main thread.
        [this, token, with_skips, on_success, on_error](const std::string& content) {
            std::string modified_content = with_includes(content, with_skips);
            if (modified_content == content) {
                spdlog::info("[HelixMacroManager] Include lines already present in printer.cfg");
                token.defer("MacroManager::include_already_present", [on_success]() {
                    if (on_success) {
                        on_success();
                    }
                });
                return;
            }

            // Defer the upload kick-off to main thread (needs api_)
            const std::string backup_name = printer_cfg_backup_name();
            token.defer(
                "MacroManager::add_include_upload",
                [this, on_success, on_error, backup_name, original_content = content,
                 modified_content = std::move(modified_content)]() mutable {
                    // Back up the original first: if the backup upload
                    // fails, the overwrite must not happen. Its
                    // completion fires on the HTTP lane, so the
                    // follow-up upload kick-off hops back to main.
                    api_.transfers().upload_file_with_name(
                        "config", "", backup_name, original_content,
                        lifetime_.bg_cb(
                            "MacroManager::backup_done",
                            [this, on_success, on_error, backup_name,
                             modified_content = std::move(modified_content)]() mutable {
                                spdlog::info("[HelixMacroManager] Backed up printer.cfg "
                                             "to {}",
                                             backup_name);
                                api_.transfers().upload_file_with_name(
                                    "config", "", "printer.cfg", modified_content,
                                    lifetime_.bg_cb(
                                        "MacroManager::add_include_done",
                                        [on_success]() {
                                            spdlog::info("[HelixMacroManager] Successfully added "
                                                         "include to printer.cfg");
                                            if (on_success) {
                                                on_success();
                                            }
                                        }),
                                    lifetime_.bg_cb("MacroManager::add_include_failed",
                                                    [on_error](const MoonrakerError& err) {
                                                        spdlog::error(
                                                            "[HelixMacroManager] Failed to upload "
                                                            "modified printer.cfg: {}",
                                                            err.message);
                                                        if (on_error) {
                                                            on_error(err);
                                                        }
                                                    }));
                            }),
                        lifetime_.bg_cb("MacroManager::backup_failed",
                                        [on_error, backup_name](const MoonrakerError& err) {
                                            spdlog::error("[HelixMacroManager] Backup upload of {} "
                                                          "failed - refusing to overwrite "
                                                          "printer.cfg: {}",
                                                          backup_name, err.message);
                                            if (on_error) {
                                                on_error(err);
                                            }
                                        }));
                });
        },
        // Download error — fires on the HTTP lane; hop the caller's continuation.
        lifetime_.bg_cb("MacroManager::download_failed", [on_error](const MoonrakerError& err) {
            spdlog::error("[HelixMacroManager] Failed to download printer.cfg: {}", err.message);
            if (on_error) {
                on_error(err);
            }
        }));
}

void MacroManager::remove_include_from_config(std::vector<std::string> filenames,
                                              SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Removing include lines from printer.cfg");

    auto token = lifetime_.token();

    // Download printer.cfg
    api_.transfers().download_file(
        "config", "printer.cfg",
        // Download success — runs on HTTP bg thread.
        // L081 Mechanism C: do pure parsing/construction locally on BG, then defer
        // the api_-> kick-off to main thread.
        [this, token, filenames = std::move(filenames), on_success,
         on_error](const std::string& content) {
            std::string modified_content = without_includes(content, filenames);
            if (modified_content == content) {
                spdlog::info("[HelixMacroManager] Include lines not found in printer.cfg");
                token.defer("MacroManager::include_not_found", [on_success]() {
                    if (on_success) {
                        on_success();
                    }
                });
                return;
            }

            // Defer the upload kick-off to main thread (needs api_)
            token.defer("MacroManager::remove_include_upload", [this, on_success, on_error,
                                                                modified_content = std::move(
                                                                    modified_content)]() mutable {
                // Upload modified printer.cfg
                api_.transfers().upload_file_with_name(
                    "config", "", "printer.cfg", modified_content,
                    lifetime_.bg_cb("MacroManager::remove_include_done",
                                    [on_success]() {
                                        spdlog::info("[HelixMacroManager] Successfully removed "
                                                     "include from printer.cfg");
                                        if (on_success) {
                                            on_success();
                                        }
                                    }),
                    lifetime_.bg_cb("MacroManager::remove_include_failed",
                                    [on_error](const MoonrakerError& err) {
                                        spdlog::error(
                                            "[HelixMacroManager] Failed to upload modified "
                                            "printer.cfg: {}",
                                            err.message);
                                        if (on_error) {
                                            on_error(err);
                                        }
                                    }));
            });
        },
        // Download error - fires on the HTTP lane; hop the caller's continuation.
        lifetime_.bg_cb("MacroManager::remove_include_download_failed",
                        [on_error](const MoonrakerError& err) {
                            spdlog::error("[HelixMacroManager] Failed to download printer.cfg: {}",
                                          err.message);
                            if (on_error) {
                                on_error(err);
                            }
                        }));
}

void MacroManager::delete_macro_file(const std::string& filename, SuccessCallback on_success,
                                     ErrorCallback on_error) {
    // Use IMoonrakerAPI to delete the file. Both completions hop to main like
    // every other terminal callback in this class.
    api_.files().delete_file(
        "config/" + filename,
        lifetime_.bg_cb("MacroManager::delete_done",
                        [on_success]() {
                            if (on_success) {
                                on_success();
                            }
                        }),
        lifetime_.bg_cb("MacroManager::delete_failed",
                        [on_success, on_error](const MoonrakerError& err) {
                            // File might not exist - that's OK for uninstall
                            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                                spdlog::debug("[HelixMacroManager] Macro file already deleted");
                                if (on_success) {
                                    on_success(); // Continue with success path
                                }
                            } else if (on_error) {
                                on_error(err);
                            }
                        }));
}

} // namespace helix
