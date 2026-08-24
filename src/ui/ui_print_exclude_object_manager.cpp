// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_exclude_object_manager.h"

#include "ui_error_reporting.h"
#include "ui_gcode_viewer.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::ui {

// ============================================================================
// Constants
// ============================================================================

/// Undo window duration in milliseconds
constexpr uint32_t EXCLUDE_UNDO_WINDOW_MS = 5000;

// ============================================================================
// PrintExcludeObjectManager Implementation
// ============================================================================

PrintExcludeObjectManager::PrintExcludeObjectManager(IMoonrakerAPI* api,
                                                     PrinterState& printer_state,
                                                     lv_obj_t* gcode_viewer)
    : api_(api), printer_state_(printer_state), gcode_viewer_(gcode_viewer) {
    spdlog::debug("[PrintExcludeObjectManager] Constructed");
}

PrintExcludeObjectManager::~PrintExcludeObjectManager() {
    // lifetime_ destructor calls invalidate() automatically

    // Clean up timer if LVGL is still active
    if (lv_is_initialized() && exclude_undo_timer_) {
        lv_timer_delete(exclude_undo_timer_);
        exclude_undo_timer_ = nullptr;
    }

    spdlog::trace("[PrintExcludeObjectManager] Destroyed");
}

void PrintExcludeObjectManager::init() {
    if (initialized_) {
        spdlog::warn("[PrintExcludeObjectManager] init() called twice - ignoring");
        return;
    }

    // Subscribe to excluded objects changes from PrinterState
    excluded_objects_observer_ = helix::ui::observe_int_sync<PrintExcludeObjectManager>(
        printer_state_.get_excluded_objects_version_subject(), this,
        [](PrintExcludeObjectManager* self, int) { self->on_excluded_objects_changed(); },
        printer_state_.get_subjects_lifetime());

    // Watchdog: if the print ends (cancel, complete, error, standby) while we still
    // hold optimistic visuals for unconfirmed exclusions, drop them. Guards against
    // the stuck-visual edge case where both the status push and the RPC return fail
    // to arrive (Klipper crash, network drop, etc). Done silently per UX review —
    // cancels are usually deliberate and a toast would just be noise.
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - asks whether a live gcode
    // queue exists; see on_print_state_changed().
    print_state_observer_ = helix::ui::observe_print_state<PrintExcludeObjectManager>(
        printer_state_.get_print_state_enum_subject(), this,
        [](PrintExcludeObjectManager* self, PrintJobState state) {
            self->on_print_state_changed(state);
        },
        printer_state_.get_subjects_lifetime());

    // Register long-press callback on gcode viewer
    if (gcode_viewer_) {
        ui_gcode_viewer_set_object_long_press_callback(gcode_viewer_, on_object_long_pressed, this);
        spdlog::debug("[PrintExcludeObjectManager] Registered long-press callback");
    }

    initialized_ = true;
    spdlog::debug("[PrintExcludeObjectManager] Initialized");
}

void PrintExcludeObjectManager::deinit() {
    if (!initialized_) {
        return;
    }

    // Clean up timer
    if (exclude_undo_timer_ && lv_is_initialized()) {
        lv_timer_delete(exclude_undo_timer_);
        exclude_undo_timer_ = nullptr;
    }

    // Unregister long-press callback
    if (gcode_viewer_ && lv_is_initialized()) {
        ui_gcode_viewer_set_object_long_press_callback(gcode_viewer_, nullptr, nullptr);
    }

    // Observer cleanup happens automatically via ObserverGuard destructor

    initialized_ = false;
    spdlog::debug("[PrintExcludeObjectManager] Deinitialized");
}

void PrintExcludeObjectManager::set_gcode_viewer(lv_obj_t* gcode_viewer) {
    // Unregister from old viewer
    if (gcode_viewer_ && initialized_ && lv_is_initialized()) {
        ui_gcode_viewer_set_object_long_press_callback(gcode_viewer_, nullptr, nullptr);
    }

    gcode_viewer_ = gcode_viewer;

    // Register on new viewer
    if (gcode_viewer_ && initialized_) {
        ui_gcode_viewer_set_object_long_press_callback(gcode_viewer_, on_object_long_pressed, this);
        spdlog::debug(
            "[PrintExcludeObjectManager] Re-registered long-press callback on new viewer");
    }
}

// ============================================================================
// Long-press Handler
// ============================================================================

void PrintExcludeObjectManager::on_object_long_pressed(lv_obj_t* viewer, const char* object_name,
                                                       void* user_data) {
    (void)viewer;
    auto* self = static_cast<PrintExcludeObjectManager*>(user_data);
    if (self && object_name && object_name[0] != '\0') {
        self->handle_object_long_press(object_name);
    }
}

void PrintExcludeObjectManager::handle_object_long_press(const char* object_name) {
    if (!object_name || object_name[0] == '\0') {
        spdlog::debug("[PrintExcludeObjectManager] Long-press on empty area (no object)");
        return;
    }

    // Defense in depth: if Klipper's [exclude_object] module isn't configured, or the slicer
    // didn't emit EXCLUDE_OBJECT_DEFINE markers, `defined_objects` will be empty and the
    // EXCLUDE_OBJECT gcode would be a silent no-op on the printer. Bail here rather than
    // showing a confirmation dialog that won't accomplish anything.
    if (printer_state_.get_defined_objects().empty()) {
        spdlog::info("[PrintExcludeObjectManager] Long-press on '{}' ignored: no defined objects "
                     "(exclude_object module unconfigured or slicer didn't label objects)",
                     object_name);
        return;
    }

    // Check if already excluded
    if (excluded_objects_.count(object_name) > 0) {
        spdlog::info("[PrintExcludeObjectManager] Object '{}' already excluded - ignoring",
                     object_name);
        return;
    }

    // Check if there's already a pending exclusion
    if (!pending_exclude_object_.empty()) {
        spdlog::warn(
            "[PrintExcludeObjectManager] Already have pending exclusion for '{}' - ignoring new",
            pending_exclude_object_);
        return;
    }

    spdlog::info("[PrintExcludeObjectManager] Long-press on object: '{}' - showing confirmation",
                 object_name);

    // Store the object name for when confirmation happens
    pending_exclude_object_ = object_name;

    // Configure and show the modal
    exclude_modal_.set_object_name(object_name);
    auto token = lifetime_.token();
    exclude_modal_.set_on_confirm([this, token]() {
        if (token.expired())
            return;
        handle_exclude_confirmed();
    });
    exclude_modal_.set_on_cancel([this, token]() {
        if (token.expired())
            return;
        handle_exclude_cancelled();
    });

    std::string message = fmt::format(
        lv_tr("Stop printing \"{}\"?\n\nThis cannot be undone after 5 seconds."), object_name);
    const char* attrs[] = {"title", lv_tr("Exclude Object?"), "message", message.c_str(), nullptr};

    if (!exclude_modal_.show(lv_screen_active(), attrs)) {
        spdlog::error("[PrintExcludeObjectManager] Failed to show exclude confirmation modal");
        pending_exclude_object_.clear();
    }
}

void PrintExcludeObjectManager::request_exclude(const std::string& object_name) {
    handle_object_long_press(object_name.c_str());
}

// ============================================================================
// Modal Confirmation Handlers
// ============================================================================

void PrintExcludeObjectManager::handle_exclude_confirmed() {
    spdlog::info("[PrintExcludeObjectManager] Exclusion confirmed for '{}'",
                 pending_exclude_object_);

    if (pending_exclude_object_.empty()) {
        spdlog::error("[PrintExcludeObjectManager] No pending object for exclusion");
        return;
    }

    // Immediately update visual state in G-code viewer (red/semi-transparent)
    if (gcode_viewer_) {
        std::unordered_set<std::string> visual_excluded = excluded_objects_;
        visual_excluded.insert(pending_exclude_object_);
        ui_gcode_viewer_set_excluded_objects(gcode_viewer_, visual_excluded);
        spdlog::debug("[PrintExcludeObjectManager] Updated viewer with visual exclusion");
    }

    // Start undo timer - when it fires, we send EXCLUDE_OBJECT to Klipper
    if (exclude_undo_timer_) {
        lv_timer_delete(exclude_undo_timer_);
    }
    exclude_undo_timer_ = lv_timer_create(exclude_undo_timer_cb, EXCLUDE_UNDO_WINDOW_MS, this);
    lv_timer_set_repeat_count(exclude_undo_timer_, 1);

    // Show toast with "Undo" action button
    std::string toast_msg = "Excluding \"" + pending_exclude_object_ + "\"...";
    ToastManager::instance().show_with_action(
        ToastSeverity::WARNING, toast_msg.c_str(), "Undo",
        [](void* user_data) {
            auto* self = static_cast<PrintExcludeObjectManager*>(user_data);
            if (self) {
                self->handle_exclude_undo();
            }
        },
        this, EXCLUDE_UNDO_WINDOW_MS);

    spdlog::info("[PrintExcludeObjectManager] Started {}ms undo window for '{}'",
                 EXCLUDE_UNDO_WINDOW_MS, pending_exclude_object_);
}

void PrintExcludeObjectManager::handle_exclude_cancelled() {
    spdlog::info("[PrintExcludeObjectManager] Exclusion cancelled for '{}'",
                 pending_exclude_object_);

    // Clear pending state
    pending_exclude_object_.clear();

    // Clear selection in viewer
    if (gcode_viewer_) {
        std::unordered_set<std::string> empty_set;
        ui_gcode_viewer_set_highlighted_objects(gcode_viewer_, empty_set);
    }
}

void PrintExcludeObjectManager::handle_exclude_undo() {
    if (pending_exclude_object_.empty()) {
        spdlog::warn("[PrintExcludeObjectManager] Undo called but no pending exclusion");
        return;
    }

    spdlog::info("[PrintExcludeObjectManager] Undo pressed - cancelling exclusion of '{}'",
                 pending_exclude_object_);

    // Cancel the timer
    if (exclude_undo_timer_) {
        lv_timer_delete(exclude_undo_timer_);
        exclude_undo_timer_ = nullptr;
    }

    // Restore visual state - remove from visual exclusion
    if (gcode_viewer_) {
        ui_gcode_viewer_set_excluded_objects(gcode_viewer_, excluded_objects_);
    }

    // Clear pending
    pending_exclude_object_.clear();

    // Show confirmation that undo succeeded
    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Exclusion cancelled"), 2000);
}

// ============================================================================
// Timer Callback
// ============================================================================

void PrintExcludeObjectManager::exclude_undo_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<PrintExcludeObjectManager*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }

    self->exclude_undo_timer_ = nullptr; // Timer auto-deletes after single shot

    if (self->pending_exclude_object_.empty()) {
        spdlog::warn("[PrintExcludeObjectManager] Undo timer fired but no pending object");
        return;
    }

    std::string object_name = self->pending_exclude_object_;
    self->pending_exclude_object_.clear();

    spdlog::info(
        "[PrintExcludeObjectManager] Undo window expired - sending EXCLUDE_OBJECT for '{}'",
        object_name);

    // Capture token for async callback safety
    auto token = self->lifetime_.token();

    // Actually send the command to Klipper via IMoonrakerAPI.
    //
    // Truth model: the `exclude_object.excluded_objects` status subscription drives
    // `excluded_objects_` via on_excluded_objects_changed(). The RPC success callback is
    // advisory — it just means Klipper finished running the gcode (which during pre-print
    // may take many minutes, because printer.gcode.script blocks on the gcode queue).
    //
    // A TIMEOUT-type error from the RPC is NOT a real failure: the request may still
    // execute on Klipper and the status push will confirm it. Only non-timeout errors
    // (validation, connection lost, JSON-RPC error, etc.) warrant reverting the visual
    // and surfacing an error toast.
    self->awaiting_confirmation_.insert(object_name);
    if (self->api_) {
        self->api_->exclude_object(
            object_name,
            [self, token, object_name]() {
                if (token.expired())
                    return;
                spdlog::info("[PrintExcludeObjectManager] EXCLUDE_OBJECT '{}' RPC returned success",
                             object_name);
                // Note: we do NOT insert into excluded_objects_ here. The subscription-driven
                // on_excluded_objects_changed() path is the single source of truth; inserting
                // here would create a second path that could drift if Klipper's internal state
                // diverges from what it told us via gcode.script return.
            },
            [self, token, object_name](const MoonrakerError& err) {
                token.defer(
                    "PrintExcludeObjectManager::dispatch_rpc_error",
                    [self, object_name, err]() { self->on_exclude_rpc_error(object_name, err); });
            });
    } else {
        spdlog::warn("[PrintExcludeObjectManager] No API available - simulating exclusion");
        self->excluded_objects_.insert(object_name);
    }
}

// ============================================================================
// Observer Callback
// ============================================================================

// excluded_objects_observer_cb migrated to lambda in init()

void PrintExcludeObjectManager::on_excluded_objects_changed() {
    // Sync excluded objects from PrinterState (Klipper/Moonraker)
    const auto& klipper_excluded = printer_state_.get_excluded_objects();

    // Mirror Klipper's excluded set into our local set. Klipper's `exclude_object.excluded_objects`
    // is the authoritative source of truth — objects appear here regardless of whether the
    // exclusion came from us, another client, or a webcam-style frontend. Clearing from
    // awaiting_confirmation_ on arrival lets our RPC path know the optimistic visual is now
    // backed by Klipper's own confirmation.
    //
    // Full sync (not merge): entries Klipper dropped (e.g. via RESET_EXCLUDE) must disappear
    // from our local set too, otherwise the local cache diverges from ground truth and the
    // gcode viewer keeps rendering objects as excluded after Klipper has cleared them.
    for (auto it = excluded_objects_.begin(); it != excluded_objects_.end();) {
        if (klipper_excluded.count(*it) == 0) {
            spdlog::info("[PrintExcludeObjectManager] Dropped excluded object no longer in "
                         "Klipper's set: '{}'",
                         *it);
            it = excluded_objects_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& obj : klipper_excluded) {
        if (excluded_objects_.count(obj) == 0) {
            excluded_objects_.insert(obj);
            spdlog::info("[PrintExcludeObjectManager] Synced excluded object from Klipper: '{}'",
                         obj);
        }
        awaiting_confirmation_.erase(obj);
    }

    // Update the G-code viewer visual state
    if (gcode_viewer_) {
        // Combine confirmed excluded with any pending exclusion for visual display
        std::unordered_set<std::string> visual_excluded = excluded_objects_;
        if (!pending_exclude_object_.empty()) {
            visual_excluded.insert(pending_exclude_object_);
        }
        ui_gcode_viewer_set_excluded_objects(gcode_viewer_, visual_excluded);
        spdlog::debug("[PrintExcludeObjectManager] Updated viewer with {} excluded objects",
                      visual_excluded.size());
    }
}

void PrintExcludeObjectManager::on_print_state_changed(helix::PrintJobState state) {
    // RAW_PRINT_STATE_OK: asks whether a live gcode queue exists for a pending
    // EXCLUDE_OBJECT to land on. During a host-side block the printer holds no
    // job, so there is none - and during a firmware-side PRINT_START the wire
    // already reads printing, which is the correct answer.
    const bool print_active =
        (state == helix::PrintJobState::PRINTING) || (state == helix::PrintJobState::PAUSED);
    if (print_active) {
        return;
    }
    if (awaiting_confirmation_.empty()) {
        return;
    }

    spdlog::info("[PrintExcludeObjectManager] Print ended (state={}) with {} unconfirmed "
                 "exclusion(s) — dropping optimistic visuals",
                 static_cast<int>(state), awaiting_confirmation_.size());
    awaiting_confirmation_.clear();

    // Refresh the viewer so any red-ghosted objects that never made it to
    // excluded_objects_ revert to normal rendering.
    if (gcode_viewer_) {
        ui_gcode_viewer_set_excluded_objects(gcode_viewer_, excluded_objects_);
    }
}

void PrintExcludeObjectManager::on_exclude_rpc_error(const std::string& object_name,
                                                     const MoonrakerError& err) {
    if (err.type == MoonrakerErrorType::TIMEOUT) {
        // Advisory path. printer.gcode.script blocks until Klipper executes the queued
        // gcode; during pre-print this can legitimately take >15 minutes (our ceiling).
        // If we DO hit that ceiling, we log silently and keep the optimistic visual in
        // place. Worst case: Klipper never ran the command and the visual is wrong until
        // the next print or the user manually reverts — tradeoff for avoiding the
        // false-positive toast that motivated this refactor. TODO(post-1.0): watchdog
        // that reverts visual if still awaiting_confirmation_ when print state leaves
        // the pre-print/printing phases.
        spdlog::warn("[PrintExcludeObjectManager] EXCLUDE_OBJECT '{}' RPC timed out ({}) — "
                     "continuing to wait for status subscription to confirm",
                     object_name, err.message);
        return;
    }

    spdlog::error("[PrintExcludeObjectManager] Failed to exclude '{}': {}", object_name,
                  err.message);

    // UI operations must happen on the main thread. We defer regardless of which thread
    // we were called on — lifetime_.defer is safe from the main thread and tok.defer
    // would have handled the background case via the dispatch path.
    auto defer_tok = lifetime_.token();
    defer_tok.defer("PrintExcludeObjectManager::exclude_error", [this, object_name,
                                                                 user_msg = err.user_message()]() {
        awaiting_confirmation_.erase(object_name);
        NOTIFY_ERROR(lv_tr("Failed to exclude '{}': {}"), object_name, user_msg);

        if (gcode_viewer_) {
            ui_gcode_viewer_set_excluded_objects(gcode_viewer_, excluded_objects_);
            spdlog::debug("[PrintExcludeObjectManager] Reverted visual exclusion for '{}'",
                          object_name);
        }
    });
}

} // namespace helix::ui
