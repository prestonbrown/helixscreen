// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file env_refusal_notice.cpp
 * @brief Surface a refused helixscreen.env to the user
 *        (prestonbrown/helixscreen#1712)
 *
 * The launcher refuses to evaluate a helixscreen.env it cannot trust, and
 * skips individual lines it refuses (shell syntax, a key the file may not
 * set, bad quoting). Both were log-only: the display came up on defaults
 * with no on-screen trace. The launcher exports a structured record of the
 * whole-file refusal (HELIX_ENV_FILE_REFUSED, kind|detail|expected|path) and
 * the skipped lines (HELIX_ENV_LINES_SKIPPED); environment inherits through
 * the watchdog's execv, so every launch path carries them. This module words
 * the warnings - one toast line naming the single command the problem needs,
 * the full hint stays in the launcher's log - and sends them through the same
 * startup toast channel config-restore warnings use.
 */

#include "env_refusal_notice.h"

#include "ui_notification.h"

#include "i18n_plural.h"
#include "lvgl.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix {

std::vector<EnvLineSkip> parse_env_lines_skipped(const std::string& value) {
    std::vector<EnvLineSkip> entries;
    std::size_t start = 0;
    while (start != std::string::npos && start <= value.size()) {
        const std::size_t bar = value.find('|', start);
        const std::string entry =
            value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        // First colon only: reasons may themselves contain colons.
        const std::size_t colon = entry.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < entry.size()) {
            entries.push_back({entry.substr(0, colon), entry.substr(colon + 1)});
        }
        start = (bar == std::string::npos) ? std::string::npos : bar + 1;
    }
    return entries;
}

EnvFileRefusal parse_env_file_refused(const std::string& value) {
    EnvFileRefusal refusal;
    const std::size_t p1 = value.find('|');
    const std::size_t p2 = p1 == std::string::npos ? std::string::npos : value.find('|', p1 + 1);
    const std::size_t p3 = p2 == std::string::npos ? std::string::npos : value.find('|', p2 + 1);
    if (p3 == std::string::npos || p3 + 1 >= value.size()) {
        return refusal;
    }
    refusal.kind = value.substr(0, p1);
    refusal.detail = value.substr(p1 + 1, p2 - p1 - 1);
    refusal.expected = value.substr(p2 + 1, p3 - p2 - 1);
    refusal.path = value.substr(p3 + 1);
    refusal.valid = refusal.kind == "mode" || refusal.kind == "owner" || refusal.kind == "chain" ||
                    refusal.kind == "other";
    return refusal;
}

EnvRefusalCopy env_refusal_copy(const EnvFileRefusal& refusal) {
    EnvRefusalCopy copy;
    if (refusal.kind == "mode") {
        copy.message = lv_tr("helixscreen.env ignored: writable by other users");
        copy.fix =
            fmt::format(fmt::runtime(lv_tr("Fix: chmod 644 {}, then restart")), refusal.path);
    } else if (refusal.kind == "owner") {
        copy.message = fmt::format(fmt::runtime(lv_tr("helixscreen.env ignored: owned by uid {}")),
                                   refusal.detail);
        copy.fix = fmt::format(fmt::runtime(lv_tr("Fix: chown {} {}, then restart")),
                               refusal.expected, refusal.path);
    } else if (refusal.kind == "chain") {
        copy.message = lv_tr("helixscreen.env ignored: untrusted symlink chain");
        copy.fix = lv_tr("See the log");
    } else {
        copy.message =
            fmt::format(fmt::runtime(lv_tr("helixscreen.env ignored: {}")), refusal.detail);
        copy.fix = lv_tr("See the log");
    }
    return copy;
}

EnvRefusalNotice decide_env_refusal_notice(const char* file_refused, const char* lines_skipped) {
    EnvRefusalNotice notice;
    if (file_refused != nullptr && *file_refused != '\0') {
        notice.refusal = parse_env_file_refused(file_refused);
    }
    if (lines_skipped != nullptr && *lines_skipped != '\0') {
        notice.skipped = parse_env_lines_skipped(lines_skipped);
        notice.skipped_lines = notice.skipped.size();
        for (const EnvLineSkip& skip : notice.skipped) {
            if (skip.label == kEnvSkipSentinelLabel) {
                notice.skipped_truncated = true;
                break;
            }
        }
    }
    return notice;
}

void surface_env_refusal_from_launcher() {
    const EnvRefusalNotice notice = decide_env_refusal_notice(
        std::getenv("HELIX_ENV_FILE_REFUSED"), std::getenv("HELIX_ENV_LINES_SKIPPED"));

    if (notice.refusal.valid) {
        spdlog::warn("[EnvFile] launcher refused helixscreen.env: {} ({})", notice.refusal.kind,
                     notice.refusal.path);
        // Sticky: the whole file was refused, so every setting in it is being
        // ignored - an auto-dismissing toast would re-hide that before the
        // user has read it.
        const EnvRefusalCopy copy = env_refusal_copy(notice.refusal);
        ui_notification_warning_sticky(copy.message.c_str(), copy.fix.c_str());
    }

    if (notice.skipped_lines == 0) {
        return;
    }
    std::string detail;
    for (const EnvLineSkip& skip : notice.skipped) {
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += skip.label + ": " + skip.reason;
    }
    // The sentinel entry is the launcher saying "the list was capped", not a
    // skipped line: count it out and word the total as a lower bound.
    const std::size_t listed =
        notice.skipped_truncated ? notice.skipped_lines - 1 : notice.skipped_lines;
    std::string message;
    if (notice.skipped_truncated) {
        message =
            fmt::format(fmt::runtime(ui::tr_plural(
                            nullptr, "At least {} lines in helixscreen.env were ignored", listed)),
                        listed);
    } else {
        message = fmt::format(
            fmt::runtime(ui::tr_plural("1 line in helixscreen.env was ignored",
                                       "{} lines in helixscreen.env were ignored", listed)),
            listed);
    }
    ui_notification_warning_with_detail(message.c_str(), detail.c_str());
}

} // namespace helix
