// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget_size_harness.h"

#include <string>

namespace helix::test {

/// Findings of kind Text (clipped or unexpectedly wrapped) for one label, empty when its
/// string fits the box the layout gave it.
inline std::vector<OverflowFinding> label_text_findings(lv_obj_t* label) {
    std::vector<OverflowFinding> text;
    for (const auto& f : collect_overflow(label).findings) {
        if (f.kind == OverflowKind::Text) {
            text.push_back(f);
        }
    }
    return text;
}

/// Passes when @p label exists, is visible, shows some text, and that text fits its box
/// without clipping or an unexpected wrap. Says nothing about the wording.
inline void require_label_fits(lv_obj_t* label, const std::string& context = {}) {
    INFO("label: " << context);
    REQUIRE(label != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(label));
    const char* txt = lv_label_get_text(label);
    CHECK((txt && txt[0] != '\0'));
    for (const auto& f : label_text_findings(label)) {
        FAIL_CHECK(context << " [" << f.path << "] " << f.detail);
    }
}

} // namespace helix::test
