// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The three per-instance subjects a stacked home widget (temp_stack, fan_stack)
// publishes so its XML can bind the arrangement chosen by
// classify_stack_layout(): C++ decides, XML draws.

#include "core/subject.h"
#include "stacked_row_layout.h"
#include "subject_managed_panel.h"

#include <string>
#include <vector>

namespace helix {

struct StackShapeSubjects {
    helix::Subject columns{};  ///< 1: the rows sit side by side (the widget flows across)
    helix::Subject stacked{};  ///< 1: each row is icon over value (IconAbove or Columns)
    helix::Subject captions{}; ///< 1: the caption under each value shows

    /// Publish under `prefix` and append the matching XML prop pairs.
    void publish(SubjectManager& mgr, const std::string& prefix,
                 std::vector<std::string>& attr_storage) {
        auto one = [&](helix::Subject& subject, const char* role) {
            subject.init_int(0);
            const std::string name = prefix + "_" + role;
            mgr.publish(name.c_str(), &subject);
            attr_storage.insert(attr_storage.end(), {std::string(role) + "_subject", name});
        };
        one(columns, "columns");
        one(stacked, "stacked");
        one(captions, "captions");
    }

    void apply(const StackLayout& layout) {
        lv_subject_set_int(&columns, layout.shape == StackShape::Columns ? 1 : 0);
        lv_subject_set_int(&stacked, layout.shape != StackShape::Beside ? 1 : 0);
        lv_subject_set_int(&captions,
                           layout.shape == StackShape::Columns && layout.captions ? 1 : 0);
    }
};

} // namespace helix
