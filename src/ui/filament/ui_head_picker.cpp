// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_head_picker.h"

#include "display_numbering.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <string>

namespace helix::ui {

void HeadPicker::init_subjects(std::string_view prefix, SubjectManager& subjects) {
    UI_MANAGED_SUBJECT_INT(multi_tool_, 0, fmt::format("{}_multi_tool", prefix).c_str(), subjects);
    UI_MANAGED_SUBJECT_INT(tool_count_, 1, fmt::format("{}_tool_count", prefix).c_str(), subjects);
    for (int i = 0; i < kMaxHeads; ++i) {
        UI_MANAGED_SUBJECT_INT(tool_selected_[i], i == 0 ? 1 : 0,
                               fmt::format("{}_tool_selected_{}", prefix, i).c_str(), subjects);
        UI_MANAGED_SUBJECT_STRING(tool_label_[i], tool_label_buf_[i], "",
                                  fmt::format("{}_tool_label_{}", prefix, i).c_str(), subjects);
    }
}

void HeadPicker::refresh() {
    auto& tools = helix::ToolState::instance();
    const auto heads = tools.heated_heads();
    const int count = std::clamp(static_cast<int>(heads.size()), 1, kMaxHeads);
    lv_subject_set_int(&multi_tool_, tools.has_multiple_extruders() ? 1 : 0);
    if (selected_ >= count) {
        selected_ = 0;
    }
    const int active = tools.active_tool_index();
    for (int i = 0; i < kMaxHeads; ++i) {
        lv_subject_set_int(&tool_selected_[i], i == selected_ ? 1 : 0);
        std::string label;
        if (i < static_cast<int>(heads.size())) {
            label = tool_label(heads[static_cast<size_t>(i)].representative_tool(active));
        }
        lv_subject_copy_string(&tool_label_[i], label.c_str());
    }
    lv_subject_set_int(&tool_count_, count);
}

void HeadPicker::select(int head) {
    selected_ = std::clamp(head, 0, kMaxHeads - 1);
    refresh();
}

void HeadPicker::select_active() {
    auto& tools = helix::ToolState::instance();
    select(std::max(0, head_index_for_tool(tools.heated_heads(), tools.active_tool_index())));
}

std::optional<PhysicalHead> HeadPicker::selected_head() const {
    const auto heads = helix::ToolState::instance().heated_heads();
    if (heads.empty()) {
        return std::nullopt;
    }
    return heads[static_cast<size_t>(std::clamp(selected_, 0, static_cast<int>(heads.size()) - 1))];
}

bool HeadPicker::selected_holds_active_tool() const {
    const auto head = selected_head();
    return !head || head->holds_tool(helix::ToolState::instance().active_tool_index());
}

} // namespace helix::ui
