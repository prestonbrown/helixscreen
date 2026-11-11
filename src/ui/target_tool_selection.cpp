// SPDX-License-Identifier: GPL-3.0-or-later
#include "target_tool_selection.h"

#include "observer_factory.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

void TargetToolSelection::on_active_tool_changed(int active_tool) {
    if (active_tool >= 0) {
        selected_ = active_tool;
    }
}

void TargetToolSelection::pick(int tool) {
    selected_ = tool;
}

int TargetToolSelection::current(int tool_count) const {
    return selected_ >= 0 && selected_ < tool_count ? selected_ : 0;
}

TargetToolAction decide_target_tool_action(int target_tool, int active_tool, bool multiple_heads,
                                           bool supports_park) {
    if (!multiple_heads) {
        return TargetToolAction::None;
    }
    if (target_tool != active_tool) {
        return TargetToolAction::PickUp;
    }
    return supports_park ? TargetToolAction::Park : TargetToolAction::None;
}

ToolArrivalWait::~ToolArrivalWait() {
    cancel();
}

void ToolArrivalWait::start(int tool, uint32_t timeout_ms, std::function<void()> on_arrived,
                            std::function<void()> on_timeout) {
    cancel();
    tool_ = tool;
    on_arrived_ = std::move(on_arrived);
    on_timeout_ = std::move(on_timeout);
    start_active_ = ToolState::instance().active_tool_index();
    if (start_active_ == tool) {
        finish(true);
        return;
    }
    observer_ = observe<int>(
        ToolState::instance().get_active_tool_subject(), this,
        [](ToolArrivalWait* self, int active) {
            if (!self->armed()) {
                return;
            }
            if (active == self->tool_) {
                self->finish(true);
            } else if (active >= 0 && active != self->start_active_) {
                // Another tool settled on the carriage: the change was
                // overridden, and the step was meant for this one. The tool
                // mounted at start is the attach replay, or a change not yet
                // under way.
                self->finish(false);
            }
        },
        ToolState::instance().get_subjects_lifetime());
    lv_timer_t* timer = lv_timer_create(on_timer, timeout_ms, this);
    lv_timer_set_repeat_count(timer, 1);
    timer_.reset(timer);
}

void ToolArrivalWait::cancel() {
    on_arrived_ = nullptr;
    on_timeout_ = nullptr;
    observer_.reset();
    timer_.reset();
}

void ToolArrivalWait::finish(bool arrived) {
    // Move the step out first: it may start another wait on this object.
    auto step = std::move(arrived ? on_arrived_ : on_timeout_);
    if (!arrived) {
        spdlog::warn("[ToolArrivalWait] T{} not on the carriage after the change; step dropped",
                     tool_);
    }
    cancel();
    if (step) {
        step();
    }
}

void ToolArrivalWait::on_timer(lv_timer_t* timer) {
    auto* self = static_cast<ToolArrivalWait*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }
    // A repeat-count-1 timer deletes itself after this callback; the guard
    // must not cancel it again.
    self->timer_.release();
    if (self->armed()) {
        self->finish(false);
    }
}

} // namespace helix::ui
