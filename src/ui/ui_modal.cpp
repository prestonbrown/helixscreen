// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_effects.h"
#include "ui_event_safety.h"
#include "ui_keyboard_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "system/crash_handler.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <string>

using namespace helix;

// ============================================================================
// MODAL STYLE CONSTANTS
// ============================================================================
// Default backdrop opacity if globals.xml constant not found
static constexpr uint8_t DEFAULT_MODAL_BACKDROP_OPACITY = 100;

// Helper to get backdrop opacity from globals.xml
static uint8_t get_modal_backdrop_opacity() {
    const char* opacity_str = lv_xml_get_const(nullptr, "modal_backdrop_opacity");
    if (opacity_str) {
        int val = atoi(opacity_str);
        if (val >= 0 && val <= 255) {
            return static_cast<uint8_t>(val);
        }
    }
    return DEFAULT_MODAL_BACKDROP_OPACITY;
}

// ============================================================================
// ANIMATION CONSTANTS
// ============================================================================
// Duration values match globals.xml tokens for consistency
static constexpr int32_t MODAL_ENTRANCE_DURATION_MS = 250; // anim_normal
static constexpr int32_t MODAL_EXIT_DURATION_MS = 150;     // anim_fast

// Scale animation uses percentage values (0-256 in LVGL)
// We animate from 85% (218) to 100% (256), with slight overshoot for bounce
static constexpr int32_t MODAL_SCALE_START = 218; // ~85% scale
static constexpr int32_t MODAL_SCALE_END = 256;   // 100% scale

// ============================================================================
// MODAL DIALOG SUBJECTS (singleton state)
// ============================================================================
namespace {
bool g_subjects_initialized = false;
SubjectManager g_subjects;
lv_subject_t g_dialog_severity{};
lv_subject_t g_dialog_show_cancel{};
lv_subject_t g_dialog_primary_text{};
lv_subject_t g_dialog_cancel_text{};
constexpr const char* DEFAULT_PRIMARY_TEXT = "OK";
constexpr const char* DEFAULT_CANCEL_TEXT = "Cancel";
} // namespace

// Walk a widget tree depth-first, visiting every non-null object. The three
// disarm passes below differ only in what they do per object; expressing the
// skeleton once means a future pass cannot forget the child iteration or the
// null guard the recursive originals each carried separately.
template <typename Fn> static void for_each_in_tree(lv_obj_t* obj, Fn&& fn) {
    if (!obj)
        return;
    fn(obj);
    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        for_each_in_tree(lv_obj_get_child(obj, i), fn);
    }
}

// Clear user_data on an object tree to prevent stale pointer dispatch.
// Subclasses that stash `this` there rely on this: ClogDetectionConfigModal
// walks up looking for the first non-null user_data and casts it to its own type
// (clog_detection_config_modal.cpp:20), so a slot left set after teardown resolves
// to a freed instance.
//
// ui_button is the one thing that must be left alone. It owns its user_data slot
// (a heap UiButtonData) and button_delete_cb frees it by reading the slot back and
// checking a magic word - null it here and the allocation is stranded with nothing
// left to free it.
static void clear_user_data_in_tree(lv_obj_t* root) {
    for_each_in_tree(root, [](lv_obj_t* obj) {
        if (lv_obj_get_user_data(obj) != nullptr && !ui_button_owns_user_data(obj)) {
            lv_obj_set_user_data(obj, nullptr);
        }
    });
}

// Remove every event callback in the tree whose per-callback user_data is
// `owner`. lv_obj_remove_event_cb_with_user_data treats a null callback as a
// wildcard (lv_obj_event.c), so one pass catches wire_button()'s hooks and any
// a subclass added itself.
static void remove_owner_callbacks_in_tree(lv_obj_t* root, void* owner) {
    for_each_in_tree(root, [owner](lv_obj_t* obj) {
        lv_obj_remove_event_cb_with_user_data(obj, nullptr, owner);
    });
}

// Recursively remove LV_OBJ_FLAG_CLICKABLE from an object tree. lv_obj_hit_test
// rejects a non-clickable object (lv_obj_pos.c:1209), so a NEW press can no
// longer select anything in the tree. This does NOT stop a press that is already
// down - see Modal::disarm_tree.
static void disable_clicks_in_tree(lv_obj_t* root) {
    for_each_in_tree(root, [](lv_obj_t* obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE); });
}

// ============================================================================
// MODALSTACK IMPLEMENTATION
// ============================================================================

ModalStack& ModalStack::instance() {
    static ModalStack instance;
    return instance;
}

void ModalStack::push(lv_obj_t* backdrop, lv_obj_t* dialog, const std::string& component_name,
                      Modal* owner) {
    stack_.push_back({backdrop, dialog, component_name, false /* exiting */, owner});
    spdlog::debug("[ModalStack] Pushed modal '{}' (stack depth: {})", component_name,
                  stack_.size());
    crash_handler::breadcrumb::note("modal+", component_name.c_str(),
                                    static_cast<long>(stack_.size()));
}

void ModalStack::remove(lv_obj_t* backdrop) {
    auto it = std::find_if(stack_.begin(), stack_.end(),
                           [backdrop](const ModalEntry& e) { return e.backdrop == backdrop; });
    if (it != stack_.end()) {
        spdlog::debug("[ModalStack] Removed modal '{}' (stack depth: {})", it->component_name,
                      stack_.size() - 1);
        crash_handler::breadcrumb::note("modal-", it->component_name.c_str(),
                                        static_cast<long>(stack_.size() - 1));
        stack_.erase(it);
    }
}

lv_obj_t* ModalStack::top_dialog() const {
    // Return topmost non-exiting modal
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (!it->exiting) {
            return it->dialog;
        }
    }
    return nullptr;
}

std::string ModalStack::top_component_name() const {
    // Walk from top, skipping exiting entries (matches top_dialog semantics).
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (!it->exiting)
            return it->component_name;
    }
    return "";
}

lv_obj_t* ModalStack::backdrop_for(lv_obj_t* dialog) const {
    for (const auto& entry : stack_) {
        if (entry.dialog == dialog) {
            return entry.backdrop;
        }
    }
    return nullptr;
}

Modal* ModalStack::owner_for(lv_obj_t* dialog) const {
    for (const auto& entry : stack_) {
        if (entry.dialog == dialog) {
            return entry.owner;
        }
    }
    return nullptr;
}

void ModalStack::reassign_owner(lv_obj_t* backdrop, Modal* new_owner) {
    for (auto& entry : stack_) {
        if (entry.backdrop == backdrop) {
            entry.owner = new_owner;
            return;
        }
    }
}

bool ModalStack::backdrop_for_backdrop(lv_obj_t* backdrop) const {
    for (const auto& entry : stack_) {
        if (entry.backdrop == backdrop) {
            return true;
        }
    }
    return false;
}

bool ModalStack::empty() const {
    // Returns true if no visible (non-exiting) modals
    for (const auto& entry : stack_) {
        if (!entry.exiting) {
            return false;
        }
    }
    return true;
}

bool ModalStack::mark_exiting(lv_obj_t* backdrop) {
    for (auto& entry : stack_) {
        if (entry.backdrop == backdrop) {
            if (entry.exiting) {
                spdlog::debug("[ModalStack] Modal '{}' already exiting - ignoring",
                              entry.component_name);
                return false; // Already exiting
            }
            entry.exiting = true;
            spdlog::debug("[ModalStack] Marked modal '{}' as exiting", entry.component_name);
            return true;
        }
    }
    return false; // Not found
}

bool ModalStack::is_exiting(lv_obj_t* backdrop) const {
    for (const auto& entry : stack_) {
        if (entry.backdrop == backdrop) {
            return entry.exiting;
        }
    }
    return false;
}

void ModalStack::animate_entrance(lv_obj_t* dialog) {
    if (!lv_obj_is_valid(dialog)) {
        spdlog::warn("[ModalStack] animate_entrance called with invalid dialog object");
        return;
    }

    // Find backdrop for this dialog
    lv_obj_t* backdrop = backdrop_for(dialog);
    if (!backdrop) {
        return;
    }

    if (!lv_obj_is_valid(backdrop)) {
        spdlog::warn("[ModalStack] animate_entrance: backdrop is invalid");
        return;
    }

    // Safety: verify dialog's screen is still valid before style operations
    lv_obj_t* screen = lv_obj_get_screen(dialog);
    if (!screen) {
        return;
    }

    // Set transform pivot to center so scaling happens from center, not corner
    lv_obj_set_style_transform_pivot_x(dialog, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(dialog, LV_PCT(50), LV_PART_MAIN);

    // Skip animation if disabled - show in final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_opa(backdrop, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(dialog, MODAL_SCALE_END, LV_PART_MAIN);
        lv_obj_set_style_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::debug("[ModalStack] Animations disabled - showing modal instantly");
        return;
    }

    // Start backdrop transparent
    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, LV_PART_MAIN);

    // Start dialog scaled down and transparent
    lv_obj_set_style_transform_scale(dialog, MODAL_SCALE_START, LV_PART_MAIN);
    lv_obj_set_style_opa(dialog, LV_OPA_TRANSP, LV_PART_MAIN);

    // Fade in backdrop
    lv_anim_t backdrop_anim;
    lv_anim_init(&backdrop_anim);
    lv_anim_set_var(&backdrop_anim, backdrop);
    lv_anim_set_values(&backdrop_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&backdrop_anim, MODAL_ENTRANCE_DURATION_MS);
    lv_anim_set_path_cb(&backdrop_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&backdrop_anim, [](void* obj, int32_t value) {
        // No lv_obj_is_valid() guard: LVGL's lv_obj_destructor cancels an object's
        // animations when it is freed (lv_obj.c), so this exec cb can never fire on
        // a freed/recycled object. lv_obj_is_valid() here would be an O(n) recursive
        // tree walk every frame and returns TRUE on recycled memory ([L076]).
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&backdrop_anim);

    // Scale up dialog with overshoot
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, dialog);
    lv_anim_set_values(&scale_anim, MODAL_SCALE_START, MODAL_SCALE_END);
    lv_anim_set_duration(&scale_anim, MODAL_ENTRANCE_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        // No lv_obj_is_valid() guard — see the opacity cbs above ([L076]).
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(value),
                                         LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    // Fade in dialog
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, dialog);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, MODAL_ENTRANCE_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        // No lv_obj_is_valid() guard: LVGL's lv_obj_destructor cancels an object's
        // animations when it is freed (lv_obj.c), so this exec cb can never fire on
        // a freed/recycled object. lv_obj_is_valid() here would be an O(n) recursive
        // tree walk every frame and returns TRUE on recycled memory ([L076]).
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::debug("[ModalStack] Started entrance animation");
}

void ModalStack::exit_animation_done(lv_anim_t* anim) {
    lv_obj_t* backdrop = static_cast<lv_obj_t*>(anim->var);

    // Guard against freed backdrop - animation may fire after object deletion
    if (!backdrop || !lv_obj_is_valid(backdrop)) {
        spdlog::debug("[ModalStack] Exit animation complete - backdrop already freed");
        return;
    }

    // Safety check: if backdrop was already removed from stack (e.g., by
    // Modal::~Modal or clear()), it's already been deleted — nothing to do.
    auto& stack = ModalStack::instance();
    if (!stack.backdrop_for_backdrop(backdrop)) {
        spdlog::debug("[ModalStack] Exit animation complete - backdrop already removed from stack");
        return;
    }

    // Cancel all remaining animations on the backdrop and its dialog child
    // before hiding or deleting. The dialog scale/fade animations may still
    // be running and their exec callbacks trigger lv_obj_set_style_*() →
    // lv_obj_invalidate() → blur_walk_cb(), which crashes if the backdrop
    // is mid-deletion.
    lv_obj_t* dialog = lv_obj_get_child(backdrop, 0);
    if (dialog) {
        lv_anim_delete(dialog, nullptr);
    }
    lv_anim_delete(backdrop, nullptr);

    // Remove from stack (animation is complete, safe to remove)
    stack.remove(backdrop);

    // Hide immediately, then defer actual deletion to the next LVGL tick.
    // MUST use lv_obj_delete_async() (not a custom lv_async_call lambda) so
    // that obj_delete_core() can cancel the pending async if something else
    // deletes the backdrop first (e.g., Modal::~Modal via safe_delete).
    // Custom lambdas aren't cancelled by LVGL's built-in cancellation logic
    // which only matches lv_obj_delete_async_cb — crash #399.
    spdlog::debug("[ModalStack] Exit animation complete - deferring backdrop deletion");
    helix::ui::safe_delete_deferred_raw(backdrop);
}

void ModalStack::animate_exit(lv_obj_t* backdrop, lv_obj_t* dialog) {
    if (!backdrop || !dialog) {
        return;
    }

    if (!lv_obj_is_valid(dialog)) {
        spdlog::warn("[ModalStack] animate_exit called with invalid dialog object");
        return;
    }

    if (!lv_obj_is_valid(backdrop)) {
        spdlog::warn("[ModalStack] animate_exit called with invalid backdrop object");
        return;
    }

    // Deterministically cancel any in-flight ENTRANCE animations before starting the
    // exit. Entrance and exit use distinct exec_cb lambdas, so LVGL's
    // remove_concurrent_anims (matches var + exec_cb) does NOT replace them — a fast
    // dismiss otherwise leaves entrance anims fighting the exit over opa/scale. The
    // wildcard (nullptr) form deletes every anim whose var is this object. Combined
    // with the wildcard cancel in exit_animation_done and lv_obj_destructor's own
    // lv_anim_delete(obj, NULL), no exec cb can fire on a freed object.
    lv_anim_delete(backdrop, nullptr);
    lv_anim_delete(dialog, nullptr);

    // Skip animation if disabled
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_transform_scale(dialog, MODAL_SCALE_END, LV_PART_MAIN);
        lv_obj_set_style_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::debug(
            "[ModalStack] Animations disabled - removing from stack and deferring deletion");
        // Remove from stack BEFORE async deletion — exit_animation_done() handles
        // this for animated exits, but the no-animation path was missing it.
        // Without removal, stale exiting entries accumulate and if LVGL reuses
        // the address for a new backdrop, is_exiting() matches the stale entry,
        // causing hide() to bail out and the modal to become stuck.
        remove(backdrop);
        helix::ui::safe_delete_deferred_raw(backdrop);
        return;
    }

    // Fade out backdrop (triggers deletion on completion)
    lv_anim_t backdrop_anim;
    lv_anim_init(&backdrop_anim);
    lv_anim_set_var(&backdrop_anim, backdrop);
    lv_anim_set_values(&backdrop_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&backdrop_anim, MODAL_EXIT_DURATION_MS);
    lv_anim_set_path_cb(&backdrop_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&backdrop_anim, [](void* obj, int32_t value) {
        // No lv_obj_is_valid() guard: LVGL's lv_obj_destructor cancels an object's
        // animations when it is freed (lv_obj.c), so this exec cb can never fire on
        // a freed/recycled object. lv_obj_is_valid() here would be an O(n) recursive
        // tree walk every frame and returns TRUE on recycled memory ([L076]).
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_set_completed_cb(&backdrop_anim, exit_animation_done);
    lv_anim_start(&backdrop_anim);

    // Scale down dialog
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, dialog);
    lv_anim_set_values(&scale_anim, MODAL_SCALE_END, MODAL_SCALE_START);
    lv_anim_set_duration(&scale_anim, MODAL_EXIT_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        // No lv_obj_is_valid() guard — see the opacity cbs above ([L076]).
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(value),
                                         LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    // Fade out dialog
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, dialog);
    lv_anim_set_values(&fade_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_anim, MODAL_EXIT_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        // No lv_obj_is_valid() guard: LVGL's lv_obj_destructor cancels an object's
        // animations when it is freed (lv_obj.c), so this exec cb can never fire on
        // a freed/recycled object. lv_obj_is_valid() here would be an O(n) recursive
        // tree walk every frame and returns TRUE on recycled memory ([L076]).
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::debug("[ModalStack] Started exit animation");
}

// ============================================================================
// MODAL CLASS - CONSTRUCTOR / DESTRUCTOR
// ============================================================================

Modal::Modal() = default;

Modal::~Modal() {
    // RAII: auto-hide if still visible
    // Use safe delete to handle shutdown race conditions
    if (backdrop_) {
        // Defensive guard against a stale backdrop_ pointer. This can happen
        // when an owning C++ object (e.g. an overlay holding a unique_ptr<Modal>)
        // outlives the widget tree the backdrop was created in — for example,
        // after a screen switch that tore down lv_screen_active()'s children.
        // Touching the dangling LVGL object here triggered crash signature
        // 739584fa: ~Modal → lv_obj_remove_event_cb → lv_event_get_count UAF.
        //
        // If the backdrop is no longer a live LVGL object, or is no longer
        // tracked by ModalStack (another path already removed/deleted it),
        // just drop our pointers and skip cleanup.
        auto& stack = ModalStack::instance();
        if (!lv_obj_is_valid(backdrop_) || !stack.backdrop_for_backdrop(backdrop_)) {
            spdlog::debug("[Modal] ~Modal: backdrop already cleaned up externally, "
                          "skipping widget cleanup");
            backdrop_ = nullptr;
            dialog_ = nullptr;
            spdlog::trace("[Modal] Destroyed");
            return;
        }

        // If the backdrop is mid-exit-animation, exit_animation_done owns
        // final cleanup. Removing event callbacks or deleting the widget here
        // races with the animation and corrupts LVGL's event list. Leave the
        // widget alone and let the animation complete.
        if (stack.is_exiting(backdrop_)) {
            spdlog::debug("[Modal] ~Modal: backdrop mid-exit, deferring to animation");
            backdrop_ = nullptr;
            dialog_ = nullptr;
            spdlog::trace("[Modal] Destroyed");
            return;
        }

        // Cancel any exit animations BEFORE deleting — prevents exit_animation_done
        // callback from firing on the soon-to-be-freed backdrop
        lv_anim_delete(backdrop_, nullptr);
        if (dialog_) {
            lv_anim_delete(dialog_, nullptr);
        }
        // Same disarm as hide(); the derived class is already gone here, so
        // nothing may dispatch into it.
        disarm_tree(backdrop_, dialog_, this);

        // Hide immediately without calling virtual on_hide() - derived class already destroyed
        // Note: lv_obj_safe_delete handles focus group cleanup (helix::ui::defocus_tree)
        stack.remove(backdrop_);
        helix::ui::safe_delete(backdrop_);
        // dialog_ is a child of backdrop_ and was destroyed with it
        dialog_ = nullptr;
    }
    spdlog::trace("[Modal] Destroyed");
}

void Modal::disarm_tree(lv_obj_t* backdrop, lv_obj_t* dialog, Modal* owner) {
    if (dialog) {
        // Order matters only in that all three must happen before the widgets
        // start animating out; the dialog stays alive for MODAL_EXIT_DURATION_MS
        // after a hide, and every callback on it is still wired.
        clear_user_data_in_tree(dialog);
        disable_clicks_in_tree(dialog);

        // Clearing CLICKABLE stops a NEW press from selecting anything in the
        // tree, but it does NOT stop one that is already down: LVGL dispatches
        // LV_EVENT_CLICKED on release to the object captured at press time and
        // gates that on LV_STATE_DISABLED only, never on the flag
        // (lv_indev.c indev_proc_release). A finger held on the confirm button
        // while a background path closes the modal would still fire the
        // callback on lift-off. Dropping the captured object closes that.
        //
        // Note this resets the pressed object for every indev, not only one
        // pressing this dialog - lv_indev_reset's obj argument scopes
        // last_pressed/last_hovered but not act_obj. Correct here regardless: a
        // modal owns input behind a full-screen backdrop, so there is no other
        // legitimate press in flight to cancel.
        lv_indev_reset(nullptr, dialog);
    }

    if (backdrop) {
        // These carry `this` as per-callback user_data for an instance modal, so
        // they must not survive into the exit animation.
        lv_obj_remove_event_cb(backdrop, backdrop_click_cb);
        lv_obj_remove_event_cb(backdrop, esc_key_cb);
    }

    // An instance that frees itself from on_hide() is gone on the next tick
    // while these widgets live out MODAL_EXIT_DURATION_MS. Any callback still
    // carrying it - wire_button()'s hooks, or one a subclass added - would then
    // dispatch on freed memory. disable_clicks_in_tree() stops a finger from
    // reaching them, but not lv_obj_send_event (which `ctl click` and tests
    // use, and which ignores the CLICKABLE flag).
    if (owner) {
        // backdrop is the dialog's parent, so one walk from there covers both.
        remove_owner_callbacks_in_tree(backdrop ? backdrop : dialog, owner);
    }
}

// ============================================================================
// MODAL CLASS - STATIC FACTORY API
// ============================================================================

lv_obj_t* Modal::show(const char* component_name, const char** attrs) {
    if (!component_name) {
        spdlog::error("[Modal] show() called with null component_name");
        return nullptr;
    }

    spdlog::info("[Modal] Showing modal: {}", component_name);

    lv_obj_t* parent = lv_screen_active();
    if (!parent) {
        spdlog::warn("[Modal] No active screen - cannot show modal '{}'", component_name);
        return nullptr;
    }

    // Create backdrop using shared utility
    lv_obj_t* backdrop =
        helix::ui::create_fullscreen_backdrop(parent, get_modal_backdrop_opacity());
    if (!backdrop) {
        spdlog::error("[Modal] Failed to create backdrop");
        return nullptr;
    }

    // Create XML component inside backdrop
    lv_obj_t* dialog = static_cast<lv_obj_t*>(lv_xml_create(backdrop, component_name, attrs));
    if (!dialog) {
        spdlog::error("[Modal] Failed to create modal from XML: {}", component_name);
        helix::ui::safe_delete(backdrop);
        return nullptr;
    }

    // Position dialog centered
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);

    // Apply current theme palette to dialog tree (context-aware input styling)
    theme_apply_current_palette_to_tree(dialog);

    // Add backdrop click handler
    lv_obj_add_event_cb(backdrop, backdrop_click_cb, LV_EVENT_CLICKED, nullptr);

    // Add ESC key handler
    lv_obj_add_event_cb(backdrop, esc_key_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_group_t* group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, backdrop);
    }

    // Bring to foreground
    lv_obj_move_foreground(backdrop);

    // Add to stack
    ModalStack::instance().push(backdrop, dialog, component_name);

    // Animate entrance
    ModalStack::instance().animate_entrance(dialog);

    spdlog::info("[Modal] Modal shown successfully");
    return dialog;
}

// If any visible (non-exiting) modal remains, bring the new topmost one to the
// foreground so it isn't left buried behind a backdrop that is on its way out.
static void raise_top_modal_to_foreground(ModalStack& stack) {
    lv_obj_t* top = stack.top_dialog();
    if (!top) {
        return;
    }
    lv_obj_t* top_backdrop = stack.backdrop_for(top);
    if (top_backdrop && !stack.is_exiting(top_backdrop)) {
        lv_obj_move_foreground(top_backdrop);
    }
}

static const char* close_reason_name(ModalCloseReason reason) {
    switch (reason) {
    case ModalCloseReason::Programmatic:
        return "programmatic";
    case ModalCloseReason::BackdropTap:
        return "backdrop tap";
    case ModalCloseReason::EscKey:
        return "esc key";
    case ModalCloseReason::ButtonPress:
        return "button press";
    case ModalCloseReason::HotReload:
        return "hot reload";
    case ModalCloseReason::External:
        return "external sweep";
    }
    return "unknown";
}

void Modal::hide(lv_obj_t* dialog, ModalCloseReason reason) {
    if (!dialog) {
        spdlog::error("[Modal] hide() called with null dialog");
        return;
    }

    // Check if LVGL is initialized (may be called during shutdown)
    if (!lv_is_initialized()) {
        spdlog::debug("[Modal] hide() called after LVGL shutdown - ignoring");
        return;
    }

    // Find backdrop for this dialog — if not in stack, it was already deleted
    auto& stack = ModalStack::instance();
    lv_obj_t* backdrop = stack.backdrop_for(dialog);
    if (!backdrop) {
        spdlog::warn("[Modal] Dialog not found in stack - already deleted or orphaned");
        return;
    }

    // Check if already exiting (animation in progress) - ignore double-hide
    if (stack.is_exiting(backdrop)) {
        spdlog::debug("[Modal] Modal already exiting - ignoring hide()");
        return;
    }

    // A dialog shown through the instance API needs the instance teardown:
    // on_hide(), async-lifetime invalidation, and button user_data clearing.
    // Callers reaching for the static overload (typically as
    // Modal::hide(Modal::get_top())) can't know which kind they hold, so
    // delegate. The owner is cleared when the entry is marked exiting, so the
    // is_exiting guard above guarantees this pointer is still live.
    if (Modal* owner = stack.owner_for(dialog)) {
        owner->hide(reason);
        raise_top_modal_to_foreground(stack);
        return;
    }

    spdlog::info("[Modal] Hiding modal ({})", close_reason_name(reason));

    // An owner-less dialog (the confirmation/alert helpers, and the static
    // Modal::show factory) has no instance teardown to delegate to, but the
    // window between "closing" and "freed" is the same one, so it gets the same
    // disarm. Without it a second tap re-runs the confirm action against state
    // the first already consumed, and because top_dialog() skips exiting
    // entries, a nested Modal::hide(Modal::get_top()) resolves to the modal
    // UNDERNEATH.
    disarm_tree(backdrop, dialog);

    // Remove entire tree from focus group to prevent scroll-on-focus during exit animation
    helix::ui::defocus_tree(backdrop);

    // Mark as exiting (stays in stack until animation completes)
    stack.mark_exiting(backdrop);

    // Animate exit (animation callback will remove from stack when done)
    stack.animate_exit(backdrop, dialog);

    raise_top_modal_to_foreground(stack);
}

lv_obj_t* Modal::get_top() {
    return ModalStack::instance().top_dialog();
}

bool Modal::any_visible() {
    return !ModalStack::instance().empty();
}

bool Modal::rebuild_top() {
    auto& stack = ModalStack::instance();
    lv_obj_t* dialog = stack.top_dialog();
    if (!dialog)
        return false;

    std::string name = stack.top_component_name();
    if (name.empty()) {
        spdlog::warn("[Modal::rebuild_top] top dialog has no component name — skipping");
        return false;
    }

    // No modal can be faithfully rebuilt from its XML alone, so hot reload
    // closes the top one instead of resurrecting a broken copy.
    //
    // An instance-backed modal loses on_show() population and its button
    // wiring. A statically shown one loses just as much: modal_show_confirmation
    // and modal_show_alert pass title/message as runtime attrs to lv_xml_create
    // and then attach their button callbacks with lv_obj_add_event_cb, and
    // neither survives a re-show. The result looked convincing and was inert -
    // resolve_params found no value for $title/$message and left the labels at
    // LVGL's default label text, so the dialog came back reading "Text"/"Text"
    // with dead buttons. The remaining static modals populate inputs after show
    // or cache the dialog handle in a member the rebuild would dangle.
    //
    // Re-opening the dialog is one interaction; a zombie that lies about its
    // contents is not worth the convenience.
    if (Modal* owner = stack.owner_for(dialog)) {
        spdlog::info("[Modal::rebuild_top] Modal '{}' is instance-backed - hiding instead of "
                     "rebuilding",
                     name);
        owner->hide(ModalCloseReason::HotReload);
        return false;
    }

    spdlog::info("[Modal::rebuild_top] Modal '{}' cannot be rebuilt from XML (runtime attrs and "
                 "post-show wiring are not recoverable) - hiding instead",
                 name);
    Modal::hide(dialog, ModalCloseReason::HotReload);
    return false;
}

// ============================================================================
// MODAL CLASS - INSTANCE API (for subclasses)
// ============================================================================

bool Modal::show(lv_obj_t* /*parent*/, const char** attrs) {
    if (backdrop_) {
        spdlog::warn("[{}] show() called while already visible - hiding first", get_name());
        hide();
    }

    // Always use the active screen — callers may pass a stale parent_screen_
    // pointer that becomes dangling after screen transitions (#522, #523, #524).
    parent_ = lv_screen_active();
    if (!parent_) {
        spdlog::warn("[{}] No active screen - cannot show modal", get_name());
        return false;
    }

    spdlog::info("[{}] Showing modal", get_name());

    // Register generic XML callbacks for backwards compatibility with modals
    // that use XML callback names without wire_*_button().
    register_xml_callbacks({
        {"on_modal_ok_clicked", ok_button_cb},
        {"on_modal_cancel_clicked", cancel_button_cb},
        {"on_modal_tertiary_clicked", tertiary_button_cb},
    });

    if (!create_and_show(parent_, component_name(), attrs)) {
        return false;
    }

    // Call hook for subclass customization
    on_show();

    spdlog::debug("[{}] Modal shown successfully", get_name());
    return true;
}

void Modal::hide(ModalCloseReason reason) {
    if (!backdrop_) {
        return; // Already hidden, safe to call multiple times
    }

    // Read by on_hide() (and anything it calls) to tell a caller's own close
    // from one the caller did not initiate. Every hide() sets it, so a bail-out
    // below can only leave a value the next real hide() overwrites.
    close_reason_ = reason;

    // Check if backdrop is still tracked — if not, it was deleted by another path
    if (!ModalStack::instance().backdrop_for_backdrop(backdrop_)) {
        spdlog::warn("[{}] hide() called but backdrop already removed from stack", get_name());
        backdrop_ = nullptr;
        dialog_ = nullptr;
        return;
    }

    // Check if already exiting (animation in progress)
    if (ModalStack::instance().is_exiting(backdrop_)) {
        spdlog::debug("[{}] Modal already exiting - ignoring hide()", get_name());
        return;
    }

    spdlog::info("[{}] Hiding modal ({})", get_name(), close_reason_name(reason));

    // Cancel all deferred async callbacks before any widget cleanup
    lifetime_.invalidate();

    // Make the tree inert before anything can dispatch into a subclass that
    // on_hide() may be about to delete (e.g. CrashReportModal's
    // async_call(delete this)).
    disarm_tree(backdrop_, dialog_, this);

    // Drop the stack's owner pointer before the hook runs. Two reasons: a
    // subclass that self-deletes from on_hide() via async_call(delete this) is
    // freed on the next LVGL tick while the entry lives until the exit
    // animation completes, and an on_hide() that reaches back into the static
    // Modal::hide(dialog) overload would otherwise be delegated straight back
    // into this function. Nothing may read the owner after this point.
    ModalStack::instance().reassign_owner(backdrop_, nullptr);

    // Call hook before destruction
    on_hide();

    // Save pointers before clearing (needed for animation)
    lv_obj_t* backdrop = backdrop_;
    lv_obj_t* dialog = dialog_;

    // Clear our pointers first (so is_visible() returns false during animation)
    backdrop_ = nullptr;
    dialog_ = nullptr;

    // Remove entire tree from focus group to prevent scroll-on-focus during exit animation
    helix::ui::defocus_tree(backdrop);

    // Mark as exiting (stays in stack until animation completes)
    ModalStack::instance().mark_exiting(backdrop);

    // Animate exit (animation callback will remove from stack when done)
    ModalStack::instance().animate_exit(backdrop, dialog);

    // Same as the static overload: a modal underneath this one has to come back
    // to the front, or it spends the exit animation dimmed behind an exiting
    // backdrop that still swallows taps.
    raise_top_modal_to_foreground(ModalStack::instance());

    spdlog::debug("[{}] Modal hidden", get_name());
}

// ============================================================================
// MODAL CLASS - HELPERS
// ============================================================================

lv_obj_t* Modal::find_widget(const char* name) {
    if (!dialog_ || !name) {
        return nullptr;
    }
    return lv_obj_find_by_name(dialog_, name);
}

void Modal::wire_button(const char* name, const char* role_name, lv_event_cb_t cb) {
    wire_button_with(name, role_name, cb, this);
}

void Modal::wire_button_with(const char* name, const char* role_name, lv_event_cb_t cb,
                             void* user_data) {
    lv_obj_t* btn = find_widget(name);
    if (btn) {
        // Per-callback user_data (4th param), NOT lv_obj_set_user_data() which
        // collides with ui_button's UiButtonData. Read via
        // lv_event_get_user_data(e). Passed through verbatim - null is a value.
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
        spdlog::trace("[{}] Wired {} button '{}'", get_name(), role_name, name);
    } else {
        spdlog::warn("[{}] {} button '{}' not found", get_name(), role_name, name);
    }
}

void Modal::wire_ok_button(const char* name) {
    wire_button(name, "OK", ok_button_cb);
}
void Modal::wire_cancel_button(const char* name) {
    wire_button(name, "Cancel", cancel_button_cb);
}
void Modal::wire_tertiary_button(const char* name) {
    wire_button(name, "Tertiary", tertiary_button_cb);
}
void Modal::wire_quaternary_button(const char* name) {
    wire_button(name, "Quaternary", quaternary_button_cb);
}
void Modal::wire_quinary_button(const char* name) {
    wire_button(name, "Quinary", quinary_button_cb);
}
void Modal::wire_senary_button(const char* name) {
    wire_button(name, "Senary", senary_button_cb);
}

// ============================================================================
// MODAL CLASS - INTERNAL
// ============================================================================

bool Modal::create_and_show(lv_obj_t* parent, const char* comp_name, const char** attrs) {
    // Create backdrop using shared utility
    backdrop_ = helix::ui::create_fullscreen_backdrop(parent, get_modal_backdrop_opacity());
    if (!backdrop_) {
        spdlog::error("[{}] Failed to create backdrop", get_name());
        return false;
    }

    // Create XML component inside backdrop
    dialog_ = static_cast<lv_obj_t*>(lv_xml_create(backdrop_, comp_name, attrs));
    if (!dialog_) {
        spdlog::error("[{}] Failed to create modal from XML component '{}'", get_name(), comp_name);
        helix::ui::safe_delete(backdrop_);
        return false;
    }

    // Position dialog centered
    lv_obj_align(dialog_, LV_ALIGN_CENTER, 0, 0);

    // Apply current theme palette to dialog tree (context-aware input styling)
    theme_apply_current_palette_to_tree(dialog_);

    // Add backdrop click handler
    lv_obj_add_event_cb(backdrop_, backdrop_click_cb, LV_EVENT_CLICKED, this);

    // Add ESC key handler
    lv_obj_add_event_cb(backdrop_, esc_key_cb, LV_EVENT_KEY, this);
    lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
    lv_group_t* group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, backdrop_);
    }

    // Bring to foreground
    lv_obj_move_foreground(backdrop_);

    // Add to stack, recording this instance as the owner so the static
    // Modal::hide(dialog) overload can delegate back to instance teardown
    ModalStack::instance().push(backdrop_, dialog_, comp_name, this);

    // Animate entrance
    ModalStack::instance().animate_entrance(dialog_);

    return true;
}

void Modal::destroy() {
    if (backdrop_) {
        // Cancel any exit animations before deleting
        lv_anim_delete(backdrop_, nullptr);
        if (dialog_) {
            lv_anim_delete(dialog_, nullptr);
        }
        ModalStack::instance().remove(backdrop_);
        helix::ui::safe_delete(backdrop_);
        // dialog_ is a child of backdrop_ and was destroyed with it
        dialog_ = nullptr;
    }
}

// ============================================================================
// MODAL CLASS - STATIC EVENT HANDLERS
// ============================================================================

// Helper macro to reduce boilerplate in button callbacks.
// All button callbacks follow the same pattern: extract Modal* from the
// per-callback user_data and call the appropriate virtual method.
// Uses lv_event_get_user_data (per-callback) not lv_obj_get_user_data
// (per-object) to avoid colliding with ui_button's internal UiButtonData.
#define MODAL_BUTTON_CB_IMPL(cb_name, method_name, button_label)                                   \
    void Modal::cb_name(lv_event_t* e) {                                                           \
        LVGL_SAFE_EVENT_CB_BEGIN("[Modal] " #cb_name);                                             \
        auto* self = static_cast<Modal*>(lv_event_get_user_data(e));                               \
        if (self) {                                                                                \
            spdlog::debug("[{}] " button_label " button clicked", self->get_name());               \
            self->method_name();                                                                   \
        }                                                                                          \
        LVGL_SAFE_EVENT_CB_END();                                                                  \
    }

void Modal::backdrop_click_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[Modal] backdrop_click_cb");

    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* current_target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only respond if click was directly on backdrop (not bubbled from child)
    if (target != current_target) {
        return;
    }

    auto* self = static_cast<Modal*>(lv_event_get_user_data(e));
    if (self) {
        // Instance modal
        spdlog::debug("[{}] Backdrop clicked - closing", self->get_name());
        self->hide(ModalCloseReason::BackdropTap);
    } else {
        // Static modal - find in stack and close topmost
        auto& stack = ModalStack::instance();
        lv_obj_t* top_dialog = stack.top_dialog();
        lv_obj_t* top_backdrop = top_dialog ? stack.backdrop_for(top_dialog) : nullptr;

        if (top_backdrop == current_target) {
            spdlog::debug("[Modal] Backdrop clicked on topmost modal - closing");
            Modal::hide(top_dialog, ModalCloseReason::BackdropTap);
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void Modal::esc_key_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[Modal] esc_key_cb");

    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_ESC) {
        return;
    }

    auto* self = static_cast<Modal*>(lv_event_get_user_data(e));
    if (self) {
        // Instance modal - call on_cancel to allow override
        spdlog::debug("[{}] ESC key pressed - closing", self->get_name());
        self->on_cancel();
    } else {
        // Static modal - hide topmost
        lv_obj_t* top = ModalStack::instance().top_dialog();
        if (top) {
            spdlog::debug("[Modal] ESC key pressed - closing topmost modal");
            Modal::hide(top, ModalCloseReason::EscKey);
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

// Generate all button callbacks using the macro
// Note: lv_event_get_user_data returns NULL for XML-registered callbacks,
// so we use lv_obj_get_user_data on the target button instead (set by wire_*_button)
MODAL_BUTTON_CB_IMPL(ok_button_cb, on_ok, "Ok")
MODAL_BUTTON_CB_IMPL(cancel_button_cb, on_cancel, "Cancel")
MODAL_BUTTON_CB_IMPL(tertiary_button_cb, on_tertiary, "Tertiary")
MODAL_BUTTON_CB_IMPL(quaternary_button_cb, on_quaternary, "Quaternary")
MODAL_BUTTON_CB_IMPL(quinary_button_cb, on_quinary, "Quinary")
MODAL_BUTTON_CB_IMPL(senary_button_cb, on_senary, "Senary")

#undef MODAL_BUTTON_CB_IMPL

// ============================================================================
// MODAL DIALOG SUBJECTS
// ============================================================================

// Static callback for modals using the static Modal::show() API
// Closes the topmost modal when clicked
static void static_modal_close_cb(lv_event_t* e) {
    (void)e;
    lv_obj_t* top = Modal::get_top();
    if (top) {
        Modal::hide(top);
    }
}

void helix::ui::modal_init_subjects() {
    if (g_subjects_initialized) {
        spdlog::warn("[Modal] Subjects already initialized - skipping");
        return;
    }

    spdlog::trace("[Modal] Initializing modal dialog subjects");

    // Initialize and register subjects with SubjectManager for automatic cleanup
    UI_MANAGED_SUBJECT_INT(g_dialog_severity, static_cast<int>(ModalSeverity::Info),
                           "dialog_severity", g_subjects);
    UI_MANAGED_SUBJECT_INT(g_dialog_show_cancel, 0, "dialog_show_cancel", g_subjects);
    UI_MANAGED_SUBJECT_POINTER(g_dialog_primary_text, const_cast<char*>(DEFAULT_PRIMARY_TEXT),
                               "dialog_primary_text", g_subjects);
    UI_MANAGED_SUBJECT_POINTER(g_dialog_cancel_text, const_cast<char*>(DEFAULT_CANCEL_TEXT),
                               "dialog_cancel_text", g_subjects);

    // Register event callbacks for modals using static Modal::show() API
    register_xml_callbacks({
        // Generic close callback - closes topmost modal (use for OK/Cancel that just dismiss)
        {"on_modal_close", static_modal_close_cb},
        // Legacy alias for print complete dialog
        {"on_print_complete_ok", static_modal_close_cb},
    });

    g_subjects_initialized = true;
    spdlog::trace("[Modal] Modal dialog subjects registered");
}

void helix::ui::modal_deinit_subjects() {
    if (!g_subjects_initialized) {
        return;
    }
    g_subjects.deinit_all();
    g_subjects_initialized = false;
    spdlog::debug("[Modal] Modal dialog subjects deinitialized");
}

void helix::ui::modal_configure(ModalSeverity severity, bool show_cancel, const char* primary_text,
                                const char* cancel_text) {
    if (!g_subjects_initialized) {
        spdlog::error("[Modal] Cannot configure - subjects not initialized!");
        return;
    }

    spdlog::debug("[Modal] Configuring dialog: severity={}, show_cancel={}, primary='{}', "
                  "cancel='{}'",
                  static_cast<int>(severity), show_cancel, primary_text ? primary_text : "(null)",
                  cancel_text ? cancel_text : "(null)");

    lv_subject_set_int(&g_dialog_severity, static_cast<int>(severity));
    lv_subject_set_int(&g_dialog_show_cancel, show_cancel ? 1 : 0);

    if (primary_text) {
        lv_subject_set_pointer(&g_dialog_primary_text, const_cast<char*>(primary_text));
    }
    if (cancel_text) {
        lv_subject_set_pointer(&g_dialog_cancel_text, const_cast<char*>(cancel_text));
    }
}

lv_subject_t* helix::ui::modal_get_severity_subject() {
    return &g_dialog_severity;
}

lv_subject_t* helix::ui::modal_get_show_cancel_subject() {
    return &g_dialog_show_cancel;
}

lv_subject_t* helix::ui::modal_get_primary_text_subject() {
    return &g_dialog_primary_text;
}

lv_subject_t* helix::ui::modal_get_cancel_text_subject() {
    return &g_dialog_cancel_text;
}

// ============================================================================
// KEYBOARD REGISTRATION
// ============================================================================

void helix::ui::modal_register_keyboard(lv_obj_t* modal, lv_obj_t* textarea) {
    if (!modal || !textarea) {
        spdlog::error("[Modal] Cannot register keyboard: modal={}, textarea={}", (void*)modal,
                      (void*)textarea);
        return;
    }

    // Position keyboard at bottom-center (default for modals).
    // Registration is handled automatically by the text_input XML widget.
    KeyboardManager::instance().set_position(LV_ALIGN_BOTTOM_MID, 0, 0);
}

// ============================================================================
// CONFIRMATION DIALOG HELPERS
// ============================================================================

namespace {

/// Restores the shared modal_dialog subjects if a show fails.
///
/// modal_configure() writes the global severity/button-text subjects, and the
/// dialog binds to them at XML-create time - so it must run BEFORE the build.
/// If the build then fails, any modal_dialog still on screen would keep
/// re-rendering through its bind_text/bind_flag_if_eq observers with the failed
/// dialog's icon and captions. Construct this BEFORE modal_configure() so the
/// snapshot is the previous state, and commit() on success.
///
/// The captions are snapshotted as POINTERS, not copies: modal_configure()
/// stores the pointer (lv_subject_set_pointer, no copy), so restoring a copy's
/// c_str() would park a dangling pointer in a long-lived subject. Putting the
/// original pointers back is exactly as safe as whatever put them there.
class ModalConfigRollback {
  public:
    ModalConfigRollback()
        : severity_(lv_subject_get_int(helix::ui::modal_get_severity_subject())),
          show_cancel_(lv_subject_get_int(helix::ui::modal_get_show_cancel_subject())),
          primary_(snapshot_caption(helix::ui::modal_get_primary_text_subject())),
          cancel_(snapshot_caption(helix::ui::modal_get_cancel_text_subject())) {}

    void commit() {
        committed_ = true;
    }

    ~ModalConfigRollback() {
        if (committed_) {
            return;
        }
        // modal_configure() stores these pointers into the shared caption
        // subjects, which fires the still-live previous dialog's bind_text
        // observers synchronously - so the pointers must be alive HERE. The
        // snapshot is by value because the previous dialog's caption strings
        // are routinely frame-locals by the time a rollback runs (the
        // print-gate chain's GateCheckResult is dead the moment its
        // run_gates_from() iteration returns). The subjects keep pointing at
        // these strings after this dtor finishes, but nothing reads a caption
        // subject until the next modal_configure() replaces it.
        helix::ui::modal_configure(static_cast<ModalSeverity>(severity_), show_cancel_ != 0,
                                   primary_.c_str(), cancel_.c_str());
    }

  private:
    static std::string snapshot_caption(lv_subject_t* subject) {
        const char* text = static_cast<const char*>(lv_subject_get_pointer(subject));
        return text ? std::string(text) : std::string();
    }

    int32_t severity_;
    int32_t show_cancel_;
    std::string primary_;
    std::string cancel_;
    bool committed_ = false;
};

/// Payload for a deferred dismissal callback.
/// The owner behind the confirmation/alert helpers.
///
/// They used to push through the static Modal::show() factory, which records no
/// owner, so nothing observed the close: no on_hide(), no lifetime_, no disarm,
/// and a dismissal reported nothing to the caller. That is the root of
/// prestonbrown/helixscreen#1380.
///
/// Owning an instance fixes it centrally: ModalStack has something to delegate
/// to, the static hide() overload runs real teardown, and on_hide() runs however
/// the dialog closed.
///
/// Self-deletes from on_hide(), the idiom used by InfoQrModal, CrashReportModal
/// and eight others. Modal::disarm_tree() strips every callback carrying `this`
/// before that happens, so the widgets outliving it by one exit animation cannot
/// dispatch into freed memory.
class ConfirmationModal : public Modal {
  public:
    ConfirmationModal(std::string title, std::string message, std::function<void()> on_dismiss,
                      std::optional<helix::LifetimeToken> owner_token)
        : title_(std::move(title)), message_(std::move(message)),
          on_dismiss_(std::move(on_dismiss)), owner_token_(std::move(owner_token)) {}

    /// Legacy form: raw lv_event_cb_t + void* user_data, wired straight to the
    /// buttons so the existing call sites' contract is untouched.
    void set_legacy_buttons(lv_event_cb_t on_confirm, lv_event_cb_t on_cancel, void* user_data,
                            bool has_cancel) {
        cb_confirm_ = on_confirm;
        cb_cancel_ = on_cancel;
        user_data_ = user_data;
        has_cancel_ = has_cancel;
    }

    /// Declarative form: the callback never touches a widget, so there is no
    /// user_data to outlive anything and nothing is attached (prestonbrown/helixscreen#1383).
    void set_callbacks(std::function<void()> on_confirm, std::function<void()> on_cancel,
                       bool has_cancel) {
        fn_confirm_ = std::move(on_confirm);
        fn_cancel_ = std::move(on_cancel);
        has_cancel_ = has_cancel;
        declarative_ = true;
    }

    /// Builds its own attrs so the strings outlive XML creation by construction
    /// rather than by relying on lv_xml_create not retaining the caller's
    /// pointers - several call sites pass a local std::string's c_str().
    bool show_dialog() {
        const char* attrs[] = {"title", title_.c_str(), "message", message_.c_str(), nullptr};
        // lv_screen_active() (not nullptr) disambiguates the instance show()
        // from the static factory overload; the parameter is ignored anyway.
        return show(lv_screen_active(), attrs);
    }

    const char* get_name() const override {
        return "Confirmation"; // i18n: do not translate - spdlog tag, never shown
    }
    const char* component_name() const override {
        return "modal_dialog";
    }

  protected:
    void on_show() override {
        wire_side("btn_primary", cb_confirm_, /*primary=*/true);
        if (has_cancel_) {
            wire_side("btn_secondary", cb_cancel_, /*primary=*/false);
        }
    }

    // Never reached: every button is routed to our own handler by wire_side(),
    // precisely so that on_cancel() below can mean one thing.
    void on_ok() override {
        hide(ModalCloseReason::ButtonPress);
    }

    void on_cancel() override {
        // esc_key_cb() is the ONLY caller - the cancel button goes to
        // secondary_clicked() instead. So this is unambiguously "closed with no
        // button pressed": hide and let on_hide() report the dismissal. That is
        // what makes the documented on_dismiss contract true for ESC as well as
        // for a backdrop tap, on every dialog shape.
        hide(ModalCloseReason::EscKey);
    }

    void on_hide() override {
        // Programmatic is the caller closing its own dialog; it already knows,
        // and a deferred on_dismiss there lands a tick after the caller's
        // teardown may have finished. Everything else - backdrop, ESC, a button
        // with no caller callback, hot reload - closed the dialog from outside
        // and is the caller's to learn about.
        if (!answered_ && on_dismiss_ && close_reason_ != ModalCloseReason::Programmatic) {
            // Deferred, for two reasons. on_hide() runs mid-teardown - the stack
            // entry is already un-owned but not yet marked exiting - so calling
            // back synchronously lets a reentrant modal_hide() run a SECOND full
            // teardown on the same backdrop. And the token has to be checked when
            // the callback actually runs, not when it was scheduled - which is
            // exactly LifetimeToken::defer(), pre-enqueue drop and telemetry
            // included, rather than a private fork of the same check.
            if (owner_token_) {
                owner_token_->defer("ConfirmationModal::on_dismiss", std::move(on_dismiss_));
            } else {
                // Untokened callers keep the legacy contract: the capture
                // outlives the dialog.
                helix::ui::queue_update(std::move(on_dismiss_));
            }
        }
        helix::ui::async_call([](void* data) { delete static_cast<ConfirmationModal*>(data); },
                              this);
    }

  private:
    /// Whether the caller that opened this dialog is still alive. Untokened
    /// callers answer true, which is the legacy contract: the capture simply has
    /// to outlive the dialog. Only the std::function callbacks can be gated -
    /// a legacy lv_event_cb_t is a second callback on the button, invoked by
    /// LVGL after ours returns, and there is no way to call it off from here.
    bool owner_alive() const {
        return !owner_token_ || !owner_token_->expired();
    }

    /// Route one side of the dialog. Our handler always goes on first, so
    /// answered_ is recorded even when the caller's callback closes the dialog.
    void wire_side(const char* name, lv_event_cb_t cb, bool primary) {
        const char* role = primary ? "confirm" : "cancel";
        wire_button_with(name, role,
                         primary ? &ConfirmationModal::primary_clicked
                                 : &ConfirmationModal::secondary_clicked,
                         this);
        if (!declarative_ && cb) {
            // DECLARATIVE_OK: the legacy overload's callback is a raw
            // lv_event_cb_t resolved at runtime, and XML's
            // <event_cb callback="name"/> binds a REGISTERED NAME - there is no
            // declarative spelling for an arbitrary function pointer. Its own
            // user_data is passed verbatim; disarm_tree() strips only ours.
            wire_button_with(name, role, cb, user_data_);
        }
    }

    static void primary_clicked(lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[Confirmation] primary_clicked");
        if (auto* self = static_cast<ConfirmationModal*>(lv_event_get_user_data(e))) {
            // Only an press that actually reaches a caller callback resolves
            // anything. With no callback on this side the caller learns nothing
            // from the press, so leave answered_ false and let on_hide() report
            // it as a dismissal - otherwise the state on_dismiss exists to clear
            // is stranded, which is the prestonbrown/helixscreen#1380 leak.
            if (self->fn_confirm_ || self->cb_confirm_) {
                self->answered_ = true;
            }
            if (self->fn_confirm_ && self->owner_alive()) {
                self->fn_confirm_();
            }
            // We own the close unless a legacy caller's callback does it. A
            // button press is not the caller's own close, so it carries its
            // reason even when no callback on this side latched answered_.
            if (self->declarative_ || !self->cb_confirm_) {
                self->hide(ModalCloseReason::ButtonPress);
            }
        }
        LVGL_SAFE_EVENT_CB_END();
    }

    static void secondary_clicked(lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[Confirmation] secondary_clicked");
        if (auto* self = static_cast<ConfirmationModal*>(lv_event_get_user_data(e))) {
            // Only an press that actually reaches a caller callback resolves
            // anything. With no callback on this side the caller learns nothing
            // from the press, so leave answered_ false and let on_hide() report
            // it as a dismissal - otherwise the state on_dismiss exists to clear
            // is stranded, which is the prestonbrown/helixscreen#1380 leak.
            if (self->fn_cancel_ || self->cb_cancel_) {
                self->answered_ = true;
            }
            if (self->fn_cancel_ && self->owner_alive()) {
                self->fn_cancel_();
            }
            // We own the close unless a legacy caller's callback does it. A
            // button press is not the caller's own close, so it carries its
            // reason even when no callback on this side latched answered_.
            if (self->declarative_ || !self->cb_cancel_) {
                self->hide(ModalCloseReason::ButtonPress);
            }
        }
        LVGL_SAFE_EVENT_CB_END();
    }

    std::string title_;
    std::string message_;
    std::function<void()> on_dismiss_;
    std::optional<helix::LifetimeToken> owner_token_;
    std::function<void()> fn_confirm_;
    std::function<void()> fn_cancel_;
    lv_event_cb_t cb_confirm_ = nullptr;
    lv_event_cb_t cb_cancel_ = nullptr;
    void* user_data_ = nullptr;
    bool has_cancel_ = false;
    bool answered_ = false;
    bool declarative_ = false;
};

/// The one build sequence behind all four helpers.
///
/// Ordering matters and used to be re-typed per helper: the rollback guard must
/// be constructed BEFORE modal_configure() overwrites the shared subjects, or it
/// snapshots the new dialog's own config and restores nothing. Four copies of
/// that meant four chances to get it wrong, and one of them did. Here it is
/// expressible once.
lv_obj_t* build_confirmation(const char* title, const char* message, ModalSeverity severity,
                             const char* primary_text, const char* cancel_text, bool has_cancel,
                             std::function<void()> on_dismiss,
                             std::optional<helix::LifetimeToken> owner_token,
                             const std::function<void(ConfirmationModal&)>& setup) {
    if (!title || !message) {
        spdlog::error("[Modal] title and message are required");
        return nullptr;
    }

    ModalConfigRollback rollback; // BEFORE configure - see above
    helix::ui::modal_configure(severity, has_cancel,
                               primary_text ? primary_text : "OK", // i18n: universal
                               has_cancel ? (cancel_text ? cancel_text : lv_tr("Cancel"))
                                          : nullptr);

    auto* owner =
        new ConfirmationModal(title, message, std::move(on_dismiss), std::move(owner_token));
    setup(*owner);
    if (!owner->show_dialog()) {
        spdlog::error("[Modal] Failed to create dialog: '{}'", title);
        delete owner;
        return nullptr;
    }
    rollback.commit();
    spdlog::debug("[Modal] Dialog shown: '{}'", title);
    return owner->dialog();
}

} // namespace

lv_obj_t* helix::ui::modal_show_confirmation(const char* title, const char* message,
                                             ModalSeverity severity, const char* confirm_text,
                                             lv_event_cb_t on_confirm, lv_event_cb_t on_cancel,
                                             void* user_data, const char* cancel_text,
                                             std::function<void()> on_dismiss,
                                             std::optional<helix::LifetimeToken> dismiss_token) {
    return build_confirmation(
        title, message, severity, confirm_text, cancel_text, /*has_cancel=*/true,
        std::move(on_dismiss), std::move(dismiss_token), [&](ConfirmationModal& m) {
            m.set_legacy_buttons(on_confirm, on_cancel, user_data, /*has_cancel=*/true);
        });
}

lv_obj_t* helix::ui::modal_show_alert(const char* title, const char* message,
                                      ModalSeverity severity, const char* ok_text,
                                      lv_event_cb_t on_ok, void* user_data,
                                      std::function<void()> on_dismiss,
                                      std::optional<helix::LifetimeToken> dismiss_token) {
    return build_confirmation(
        title, message, severity, ok_text, nullptr, /*has_cancel=*/false, std::move(on_dismiss),
        std::move(dismiss_token), [&](ConfirmationModal& m) {
            m.set_legacy_buttons(on_ok, nullptr, user_data, /*has_cancel=*/false);
        });
}

lv_obj_t* helix::ui::modal_confirm(const char* title, const char* message, ModalSeverity severity,
                                   const char* confirm_text, std::function<void()> on_confirm,
                                   const ConfirmOptions& options) {
    return build_confirmation(title, message, severity, confirm_text, options.cancel_text,
                              /*has_cancel=*/true, options.on_dismiss, options.owner_token,
                              [&](ConfirmationModal& m) {
                                  m.set_callbacks(std::move(on_confirm), options.on_cancel,
                                                  /*has_cancel=*/true);
                              });
}

lv_obj_t* helix::ui::modal_alert(const char* title, const char* message, ModalSeverity severity,
                                 const char* ok_text, std::function<void()> on_ok,
                                 const AlertOptions& options) {
    return build_confirmation(title, message, severity, ok_text, nullptr, /*has_cancel=*/false,
                              options.on_dismiss, options.owner_token, [&](ConfirmationModal& m) {
                                  m.set_callbacks(std::move(on_ok), nullptr,
                                                  /*has_cancel=*/false);
                              });
}

lv_obj_t* helix::ui::show_low_ram_resonance_warning(
    size_t total_mb, lv_event_cb_t on_confirm, lv_event_cb_t on_cancel, void* user_data,
    std::function<void()> on_dismiss, std::optional<helix::LifetimeToken> dismiss_token) {
    std::string msg;
    try {
        msg = fmt::format(lv_tr("This device has only {} MB of RAM. Resonance calibration is "
                                "memory-intensive and can make the printer firmware report a "
                                "\"Timer Too Close\" error or restart mid-test. Continue anyway?"),
                          total_mb);
    } catch (const std::exception& e) {
        // A mistranslated {} must never abort through the LVGL C dispatch frame.
        spdlog::warn("[Modal] low-RAM warning format failed: {}", e.what());
        msg = lv_tr("This device has very little RAM. Resonance calibration is "
                    "memory-intensive and may cause a \"Timer Too Close\" error. "
                    "Continue anyway?");
    }
    return modal_show_confirmation(lv_tr("Low Memory"), msg.c_str(), ModalSeverity::Warning,
                                   lv_tr("Continue"), on_confirm, on_cancel, user_data, nullptr,
                                   std::move(on_dismiss), std::move(dismiss_token));
}
