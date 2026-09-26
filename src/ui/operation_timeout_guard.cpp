// SPDX-License-Identifier: GPL-3.0-or-later

#include "operation_timeout_guard.h"

#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

OperationTimeoutGuard::~OperationTimeoutGuard() {
    cancel_timer();
}

void OperationTimeoutGuard::init_subject(const char* subject_name, SubjectManager& subjects) {
    UI_MANAGED_SUBJECT_INT(subject_, 0, subject_name, subjects);
    has_subject_ = true;
}

void OperationTimeoutGuard::begin(uint32_t timeout_ms, std::function<void()> on_timeout) {
    // Cancel existing timer if re-entering
    cancel_timer();

    active_ = true;
    on_timeout_ = std::move(on_timeout);

    if (has_subject_) {
        lv_subject_set_int(&subject_, 1);
    }

    timer_ = lv_timer_create(timer_callback, timeout_ms, this);
    lv_timer_set_repeat_count(timer_, 1);
}

void OperationTimeoutGuard::end() {
    if (!active_ && !timer_) {
        return; // Already ended — idempotent
    }

    active_ = false;
    cancel_timer();

    if (has_subject_) {
        lv_subject_set_int(&subject_, 0);
    }
}

// lv_timer_delete() rather than lv_timer_cancel_safe(), so the timer is gone the
// moment the guard is. Deleting a timer while lv_timer_handler() is dispatching it
// is safe only because lv_timer_exec() re-checks state.timer_deleted after the
// callback returns; the "LVGL tolerates a timer deleting itself in its callback"
// canary in test_operation_timeout_guard.cpp pins that (#1576).
void OperationTimeoutGuard::cancel_timer() {
    if (timer_ && lv_is_initialized()) {
        lv_timer_delete(timer_);
    }
    timer_ = nullptr;
}

void OperationTimeoutGuard::timer_callback(lv_timer_t* timer) {
    auto* self = static_cast<OperationTimeoutGuard*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }

    // Timer has fired — LVGL already deleted it (repeat_count=1)
    self->timer_ = nullptr;
    self->active_ = false;

    if (self->has_subject_) {
        lv_subject_set_int(&self->subject_, 0);
    }

    spdlog::warn("[OperationTimeoutGuard] Operation timed out");

    if (self->on_timeout_) {
        self->on_timeout_();
    }
}
