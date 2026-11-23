// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget_size_harness.h"

#include <string>
#include <vector>

namespace helix::test {

/// Findings of kind Text for one label: empty when its string fits the box the layout
/// gave it. A single-line label (CLIP) is measured on width; a WRAP label is measured on
/// height, which can only fail when the label's height is fixed (a content-sized WRAP
/// label grows to whatever it needs). DOTS and SCROLL labels declare truncation, so
/// collect_overflow does not look at them.
inline std::vector<OverflowFinding> label_text_findings(lv_obj_t* label) {
    std::vector<OverflowFinding> text;
    for (const auto& f : collect_overflow(label).findings) {
        if (f.kind == OverflowKind::Text) {
            text.push_back(f);
        }
    }
    return text;
}

/// Why @p label is not a label that fits, one line per problem; empty when it is one:
/// visible, shows some text in a box with a width, not in a truncating long mode (DOTS,
/// SCROLL), and its text fits that box without clipping. Says nothing about the wording.
/// A label that ellipsizes or scrolls on purpose is not "meant to fit"; assert its box
/// another way.
inline std::vector<std::string> label_fit_problems(lv_obj_t* label) {
    std::vector<std::string> problems;
    if (!label) {
        problems.push_back("label is null");
        return problems;
    }
    lv_obj_update_layout(label);
    if (lv_obj_is_hidden(label)) {
        problems.push_back("label is hidden");
    }
    const char* txt = lv_label_get_text(label);
    if (!txt || txt[0] == '\0') {
        problems.push_back("label has no text");
    }
    if (lv_obj_get_content_width(label) <= 0) {
        problems.push_back("label box has no width");
    }
    const lv_label_long_mode_t mode = lv_label_get_long_mode(label);
    if (mode == LV_LABEL_LONG_MODE_DOTS || mode == LV_LABEL_LONG_MODE_SCROLL ||
        mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR) {
        problems.push_back("label truncates by design (DOTS/SCROLL)");
    }
    for (const auto& f : label_text_findings(label)) {
        problems.push_back(f.path + ": " + f.detail);
    }
    return problems;
}

/// Fails, listing every problem, unless label_fit_problems(@p label) is empty.
inline void require_label_fits(lv_obj_t* label, const std::string& context = {}) {
    for (const auto& problem : label_fit_problems(label)) {
        FAIL_CHECK(context << ": " << problem);
    }
}

} // namespace helix::test
