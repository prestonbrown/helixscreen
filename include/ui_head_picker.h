// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/subject.h"
#include "subject_managed_panel.h"
#include "tool_state.h"

#include <lvgl.h>
#include <optional>
#include <string_view>

namespace helix::ui {

/**
 * @brief The chip row that picks one heated head to calibrate or tune
 *
 * Owns the subjects a `<repeat count="<prefix>_tool_count">` chip row binds,
 * all named under @p prefix:
 * - <prefix>_multi_tool (int) - 1 when the printer has several heated heads
 * - <prefix>_tool_count (int) - chips to draw, at least 1
 * - <prefix>_tool_selected_N (int), <prefix>_tool_label_N (string)
 *
 * The heads are ToolState::heated_heads(): a heater shared by several docked
 * nozzles is one chip, and AMS lanes into one hotend are none.
 */
class HeadPicker {
  public:
    /// Heads offered as chips; a changer with more shows the first.
    static constexpr int kMaxHeads = 8;

    void init_subjects(std::string_view prefix, SubjectManager& subjects);

    /// Re-read the tool list: labels, chip count, the multi-head flag. A
    /// selection past the end falls back to the first head.
    void refresh();

    void select(int head);
    /// The head printing through the active tool, else the first.
    void select_active();

    [[nodiscard]] int selected() const {
        return selected_;
    }
    /// The selected head, or nullopt when the tool list names no heated head.
    [[nodiscard]] std::optional<PhysicalHead> selected_head() const;
    /// The selected head prints through the active tool.
    [[nodiscard]] bool selected_holds_active_tool() const;

  private:
    int selected_ = 0;

    helix::Subject multi_tool_{};
    helix::Subject tool_count_{};
    helix::Subject tool_selected_[kMaxHeads]{};
    helix::Subject tool_label_[kMaxHeads]{};
    char tool_label_buf_[kMaxHeads][8] = {};
};

} // namespace helix::ui
