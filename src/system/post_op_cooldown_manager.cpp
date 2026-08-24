// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "post_op_cooldown_manager.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "temperature_controller.h"

#include <spdlog/spdlog.h>

PostOpCooldownManager& PostOpCooldownManager::instance() {
    static PostOpCooldownManager inst;
    return inst;
}

void PostOpCooldownManager::init() {
    initialized_ = true;
    spdlog::info("[PostOpCooldown] Initialized");
}

void PostOpCooldownManager::schedule() {
    if (!initialized_) {
        spdlog::warn("[PostOpCooldown] schedule() called before init");
        return;
    }

    auto* cfg = helix::Config::get_instance();

    // Read config rather than the SettingsManager subject: schedule() is callable
    // from any thread, and the config accessors are the only thread-safe half.
    if (cfg && !cfg->get<bool>(cfg->df() + "filament/auto_cooldown", true)) {
        spdlog::debug("[PostOpCooldown] Skipping — auto-cooldown disabled in settings");
        return;
    }

    int delay_seconds =
        cfg ? cfg->get<int>(cfg->df() + "filament/cooldown_delay_seconds", 120) : 120;

    // A zero/negative delay means "off" — an lv_timer with a 0ms period would
    // otherwise fire on the next tick and cool the nozzle immediately.
    if (delay_seconds <= 0) {
        spdlog::debug("[PostOpCooldown] Skipping — cooldown_delay_seconds={}", delay_seconds);
        return;
    }

    spdlog::info("[PostOpCooldown] Scheduling cooldown in {}s", delay_seconds);

    // Reach the manager through instance() rather than a captured `this`, for the
    // same reason the timer callback below does: it keeps the queued lambda from
    // holding a pointer whose validity it cannot check (#1165).
    helix::ui::queue_update("PostOpCooldownManager::schedule", [delay_seconds]() {
        auto& self = PostOpCooldownManager::instance();

        // Delete existing timer if any
        if (self.timer_) {
            lv_timer_delete(self.timer_);
            self.timer_ = nullptr;
        }

        self.timer_ = lv_timer_create(
            [](lv_timer_t* /*t*/) {
                auto& self = PostOpCooldownManager::instance();
                self.timer_ = nullptr;

                auto& state = get_printer_state();

                // Skip while a job owns the toolhead. Preparing counts: a
                // print that is starting will heat the nozzle itself, so cooling
                // it down now is work the pre-start block immediately undoes.
                const auto lifecycle = state.get_print_lifecycle();
                if (job_holds_machine(lifecycle)) {
                    spdlog::info("[PostOpCooldown] Skipping cooldown — print active");
                    return;
                }

                // Check extruder target (decidegrees, > 0 means heater is on)
                auto* target_subj = state.get_active_extruder_target_subject();
                if (!target_subj || lv_subject_get_int(target_subj) == 0) {
                    spdlog::debug("[PostOpCooldown] Skipping cooldown — extruder already off");
                    return;
                }

                spdlog::info("[PostOpCooldown] Turning off extruder heater ({})",
                             state.active_extruder_name());
                if (auto* c = get_temperature_controller()) {
                    c->set_target(helix::HeaterType::Nozzle, 0, {.toast = false});
                }
            },
            static_cast<uint32_t>(delay_seconds) * 1000, nullptr);
        lv_timer_set_repeat_count(self.timer_, 1);
    });
}

void PostOpCooldownManager::cancel() {
    if (!initialized_)
        return;

    spdlog::debug("[PostOpCooldown] Cancelling pending cooldown");

    helix::ui::queue_update("PostOpCooldownManager::cancel", []() {
        auto& self = PostOpCooldownManager::instance();
        if (self.timer_) {
            lv_timer_delete(self.timer_);
            self.timer_ = nullptr;
        }
    });
}

void PostOpCooldownManager::shutdown() {
    if (!initialized_)
        return;

    spdlog::info("[PostOpCooldown] Shutting down");

    // Shutdown runs on main thread, so we can delete directly
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    initialized_ = false;
}
