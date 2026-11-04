// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_versions_state.cpp
 * @brief Software version state management extracted from PrinterState
 *
 * Manages Klipper and Moonraker version subjects for UI display in the
 * Settings panel About section.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_versions_state.h"

#include "core/text_io.h"
#include "json_utils.h"
#include "state/subject_macros.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix {

bool moonraker_history_is_degraded(const std::string& version) {
    if (!helix::version::parse_version(version)) {
        return false;
    }
    // Moonraker ships git-describe versions ("v0.9.0-16-g0f1e2d3"). SemVer ranks
    // a prerelease below its own release, so the constraint check is the right
    // comparison: it answers on the core triple alone.
    return !helix::version::check_version_constraint(std::string(">=") + kMinMoonrakerVersion,
                                                     version);
}

namespace {

std::string trim(const std::string& text) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    const auto first = std::find_if(text.begin(), text.end(), not_space);
    const auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return first < last ? std::string(first, last) : std::string();
}

// A placeholder collapses to a translated "Unknown" label: some forks (QIDI's
// on the Q2 / Max 4) report "?" rather than omit the field.
std::string display_version(const std::string& version) {
    if (is_placeholder_version(version)) {
        return lv_tr("Unknown");
    }
    return trim(version);
}

// update_manager's version for a git checkout with no release tags.
bool is_untagged_checkout_version(const std::string& version) {
    const auto parsed = helix::version::parse_version(version);
    return parsed && parsed->major == 0 && parsed->minor == 0 && parsed->patch == 0;
}

} // namespace

bool is_placeholder_version(const std::string& version) {
    const std::string trimmed = trim(version);
    return trimmed.empty() || trimmed == "?" || helix::text_io::to_lower(trimmed) == "unknown";
}

std::string moonraker_version_from_update_status(const nlohmann::json& result) {
    const auto* version_info = helix::json_util::find_member(result, "version_info");
    const auto* moonraker =
        version_info ? helix::json_util::find_member(*version_info, "moonraker") : nullptr;
    if (!moonraker || !moonraker->is_object()) {
        return {};
    }
    const std::string version = trim(helix::json_util::safe_string(*moonraker, "version"));
    if (!is_placeholder_version(version) && !is_untagged_checkout_version(version)) {
        return version;
    }
    constexpr size_t kShortHashLength = 7;
    const std::string hash = trim(helix::json_util::safe_string(*moonraker, "current_hash"));
    if (hash.size() >= kShortHashLength && !is_placeholder_version(hash)) {
        return hash.substr(0, kShortHashLength);
    }
    return {};
}

void PrinterVersionsState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterVersionsState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterVersionsState] Initializing subjects (register_xml={})", register_xml);

    // Initialize string subjects with em dash default
    INIT_SUBJECT_STRING(klipper_version, "—", subjects_, register_xml);
    INIT_SUBJECT_STRING(moonraker_version, "—", subjects_, register_xml);
    INIT_SUBJECT_STRING(os_version, "—", subjects_, register_xml);
    INIT_SUBJECT_STRING(klipper_firmware_name, "Klipper", subjects_, register_xml);
    // 0 until a version arrives: an unknown Moonraker is not a degraded one.
    INIT_SUBJECT_INT(moonraker_history_degraded, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterVersionsState] Subjects initialized successfully");
}

void PrinterVersionsState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterVersionsState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterVersionsState::set_klipper_version_internal(const std::string& version) {
    klipper_version_raw_ = version;
    lv_subject_copy_string(&klipper_version_, display_version(version).c_str());
    spdlog::debug("[PrinterVersionsState] Klipper version set: {}", version);
}

void PrinterVersionsState::set_moonraker_version_internal(const std::string& version) {
    lv_subject_copy_string(&moonraker_version_, display_version(version).c_str());
    // Derived from the raw string, not the display form: display_version()
    // collapses placeholders to a translated label that no comparison can read.
    const bool degraded = moonraker_history_is_degraded(version);
    lv_subject_set_int(&moonraker_history_degraded_, degraded ? 1 : 0);
    spdlog::debug("[PrinterVersionsState] Moonraker version set: {} (history degraded: {})",
                  version, degraded);
}

void PrinterVersionsState::set_firmware_name_internal(const std::string& name) {
    if (name != klipper_firmware_name_buf_) {
        lv_subject_copy_string(&klipper_firmware_name_, name.c_str());
    }
}

void PrinterVersionsState::set_os_version_internal(const std::string& version) {
    lv_subject_copy_string(&os_version_, display_version(version).c_str());
    spdlog::debug("[PrinterVersionsState] OS version set: {}", version);
}

} // namespace helix
