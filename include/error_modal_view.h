// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "error_event.h"

#include <lvgl/lvgl.h> // lv_tr()

#include <string>

namespace helix::ui {

/// Title for a CRITICAL error modal (with or without recovery actions): the
/// event's own title, falling back to "Filament System Error" for an untitled
/// CFS fault and "Printer Error" otherwise.
///
/// The CFS fallback exists because error_classify::classify() names no title
/// for a key8xx code, so without it every CFS fault would read "Printer Error".
/// It is only a fallback: AmsBackendCfs::classify_error() titles its runout
/// event "Filament runout", and a runout must not be relabelled a generic
/// system error. No CFS producer other than that one sets a title, so this
/// changes nothing for the coded faults.
///
/// Shared by GcodeErrorRouter (the plain CRITICAL arm) and
/// RecoveryModalPresenter (the MODAL_WITH_RECOVER arm) so the CFS title rule
/// cannot drift between the two presentation paths.
inline const char* modal_title_for(const helix::ErrorEvent& e) {
    if (!e.title.empty()) {
        return e.title.c_str();
    }
    if (e.source == helix::ErrorSource::CFS) {
        return lv_tr("Filament System Error");
    }
    return lv_tr("Printer Error");
}

/// Maps RecoveryAction.style to PromptButton.color.
/// "primary" -> "primary", "danger" -> "error", anything else -> "" (neutral).
inline std::string color_for_style(const std::string& style) {
    if (style == "primary") {
        return "primary";
    }
    if (style == "danger") {
        return "error";
    }
    return ""; // neutral / theme default
}

} // namespace helix::ui
