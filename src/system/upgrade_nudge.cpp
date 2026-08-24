// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "upgrade_nudge.h"

#include "app_globals.h"
#include "config.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "system/update_checker.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

constexpr const char* INTENSITY_PTR = "/upgrade_nudge/intensity";
constexpr const char* DISMISSED_VERSION_PTR = "/upgrade_nudge/dismissed_version";

UpgradeNudge::Intensity parse_intensity(const std::string& s) {
    if (s == "aggressive")
        return UpgradeNudge::Intensity::Aggressive;
    if (s == "normal")
        return UpgradeNudge::Intensity::Normal;
    // Empty / unknown / "off" all mean Off.
    return UpgradeNudge::Intensity::Off;
}

const char* intensity_to_string(UpgradeNudge::Intensity i) {
    switch (i) {
    case UpgradeNudge::Intensity::Aggressive:
        return "aggressive";
    case UpgradeNudge::Intensity::Normal:
        return "normal";
    case UpgradeNudge::Intensity::Off:
        return "off";
    }
    return "off";
}

} // namespace

UpgradeNudge& UpgradeNudge::instance() {
    static UpgradeNudge singleton;
    return singleton;
}

UpgradeNudge::UpgradeNudge() {
    reload();
}

void UpgradeNudge::reload() {
    auto* cfg = Config::get_instance();
    if (!cfg) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    std::string raw = cfg->get<std::string>(INTENSITY_PTR, std::string("off"));
    intensity_ = parse_intensity(raw);
    dismissed_version_ = cfg->get<std::string>(DISMISSED_VERSION_PTR, std::string(""));
    spdlog::debug("[UpgradeNudge] Loaded intensity={} dismissed_version='{}'",
                  intensity_to_string(intensity_), dismissed_version_);
}

UpgradeNudge::Intensity UpgradeNudge::get_intensity() const {
    std::lock_guard<std::mutex> lock(mu_);
    return intensity_;
}

std::string UpgradeNudge::get_available_version() const {
    auto cached = UpdateChecker::instance().get_cached_update();
    return cached.has_value() ? cached->version : std::string();
}

bool UpgradeNudge::is_update_visible_now() const {
    // Must have an update cached. No update → nothing to nudge about.
    if (!UpdateChecker::instance().has_update_available()) {
        return false;
    }
    // Don't nudge while a job owns the machine — the printer is the priority,
    // not our prompts. This used to test PRINTING only, with no PAUSED arm; a
    // paused print is still mid-print, the neighbouring guard in
    // UpdateChecker::start_download() has always refused on PRINTING || PAUSED
    // for the same reason, and a user who has just committed to a print is the
    // last person who wants an upgrade prompt. Deliberately normalised: this
    // makes the nudge strictly rarer, never more frequent.
    const auto lifecycle = static_cast<PrintState>(
        lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
    if (job_holds_machine(lifecycle)) {
        return false;
    }
    return true;
}

bool UpgradeNudge::should_show_settings_badge() const {
    Intensity i;
    {
        std::lock_guard<std::mutex> lock(mu_);
        i = intensity_;
    }
    if (i == Intensity::Off) {
        return false;
    }
    // Both Normal and Aggressive want the badge visible.
    return is_update_visible_now();
}

bool UpgradeNudge::should_show_banner() const {
    Intensity i;
    std::string dismissed;
    {
        std::lock_guard<std::mutex> lock(mu_);
        i = intensity_;
        dismissed = dismissed_version_;
    }
    if (i != Intensity::Aggressive) {
        return false;
    }
    if (!is_update_visible_now()) {
        return false;
    }
    // Suppress if the user already dismissed this specific version.
    // A *new* version reappears because it differs from dismissed_version_.
    std::string available = get_available_version();
    if (!dismissed.empty() && dismissed == available) {
        return false;
    }
    return true;
}

void UpgradeNudge::dismiss_current_version() {
    std::string available = get_available_version();
    if (available.empty()) {
        spdlog::debug("[UpgradeNudge] dismiss requested but no available version");
        return;
    }

    auto* cfg = Config::get_instance();
    if (!cfg) {
        return;
    }
    cfg->set<std::string>(DISMISSED_VERSION_PTR, available);
    cfg->save();

    std::lock_guard<std::mutex> lock(mu_);
    dismissed_version_ = available;
    spdlog::info("[UpgradeNudge] Dismissed banner for version {}", available);
}

} // namespace helix
