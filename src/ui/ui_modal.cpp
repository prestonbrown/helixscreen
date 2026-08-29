// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal.h"

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

// Recursively clear user_data on an object tree to prevent stale pointer dispatch
static void clear_user_data_recursive(lv_obj_t* obj) {
    if (!obj)
        return;
    if (lv_obj_get_user_data(obj) != nullptr) {
        lv_obj_set_user_data(obj, nullptr);
    }
    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        clear_user_data_recursive(lv_obj_get_child(obj, i));
    }
}

// Recursively remove LV_OBJ_FLAG_CLICKABLE from an object tree to prevent
// LVGL from delivering queued/delayed touch events during exit animation.
// Belt-and-suspenders defense alongside clear_user_data_recursive: even if a
// nested XML component button retains non-null user_data, its click callback
// won't fire because LVGL skips non-clickable objects in indev processing.
static void disable_clicks_recursive(lv_obj_t* obj) {
    if (!obj)
        return;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        disable_clicks_recursive(lv_obj_get_child(obj, i));
    }
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
            // Clear user_data on all wired buttons to prevent stale dispatch
            // after destruction — matches what hide() does for animated exits
            clear_user_data_recursive(dialog_);
            disable_clicks_recursive(dialog_);
        }

        // Remove event callbacks that hold `this` as user_data to prevent
        // stale Modal* dispatch if events fire after destruction
        lv_obj_remove_event_cb(backdrop_, backdrop_click_cb);
        lv_obj_remove_event_cb(backdrop_, esc_key_cb);

        // Hide immediately without calling virtual on_hide() - derived class already destroyed
        // Note: lv_obj_safe_delete handles focus group cleanup (helix::ui::defocus_tree)
        stack.remove(backdrop_);
        helix::ui::safe_delete(backdrop_);
        // dialog_ is a child of backdrop_ and was destroyed with it
        dialog_ = nullptr;
    }
    spdlog::trace("[Modal] Destroyed");
}

// ============================================================================
// MODAL CLASS - MOVE SEMANTICS
// ============================================================================

Modal::Modal(Modal&& other) noexcept
    : backdrop_(other.backdrop_), dialog_(other.dialog_), parent_(other.parent_) {
    // Note: lifetime_ is intentionally NOT moved — AsyncLifetimeGuard is non-movable.
    // The moved-to Modal gets a fresh guard; the moved-from Modal's guard expires
    // on destruction, correctly cancelling any outstanding callbacks.
    other.backdrop_ = nullptr;
    other.dialog_ = nullptr;
    other.parent_ = nullptr;

    // The stack entry still names the moved-from object as its owner. Retarget
    // it so an owner-aware hide reaches the instance that now holds the widgets.
    if (backdrop_) {
        ModalStack::instance().reassign_owner(backdrop_, this);
    }
}

Modal& Modal::operator=(Modal&& other) noexcept {
    if (this != &other) {
        // Clean up our modal first
        // NOTE: Do NOT call virtual on_hide() here - it's unsafe to call virtual
        // functions during move operations. Callers should call hide() before
        // move-assigning if they need lifecycle hooks.
        if (backdrop_) {
            // Cancel animations before deleting
            lv_anim_delete(backdrop_, nullptr);
            if (dialog_) {
                lv_anim_delete(dialog_, nullptr);
            }
            // Note: lv_obj_safe_delete handles focus group cleanup (helix::ui::defocus_tree)
            ModalStack::instance().remove(backdrop_);
            helix::ui::safe_delete(backdrop_);
            // dialog_ is a child of backdrop_ and was destroyed with it
            dialog_ = nullptr;
        }

        // Move state
        backdrop_ = other.backdrop_;
        dialog_ = other.dialog_;
        parent_ = other.parent_;

        // Clear source
        other.backdrop_ = nullptr;
        other.dialog_ = nullptr;
        other.parent_ = nullptr;

        // The stack entry still names the moved-from object as its owner.
        // Retarget it so an owner-aware hide reaches the instance that now
        // holds the widgets.
        if (backdrop_) {
            ModalStack::instance().reassign_owner(backdrop_, this);
        }
    }
    return *this;
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

void Modal::hide(lv_obj_t* dialog) {
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
        owner->hide();
        raise_top_modal_to_foreground(stack);
        return;
    }

    spdlog::info("[Modal] Hiding modal");

    // An owner-less dialog keeps every button callback live, still holding the
    // caller's user_data, for the whole MODAL_EXIT_DURATION_MS exit animation.
    // That window is long enough for a second tap, and the second tap is not a
    // cosmetic double-fire: it re-runs the confirm action against state the
    // first one already consumed, and because top_dialog() skips exiting
    // entries, a nested Modal::hide(Modal::get_top()) then resolves to the
    // modal UNDERNEATH. Instance hide() closes this window at the equivalent
    // point; the static path has to close it too, since the helper factories
    // (modal_show_confirmation / modal_show_alert) never set an owner.
    //
    // Deliberately NOT clear_user_data_recursive(): the helpers keep their
    // user_data in per-callback storage (lv_event_get_user_data), which that
    // function does not touch, so it would buy nothing here - while nulling an
    // lv_obj's user_data strands ui_button's UiButtonData, whose
    // button_delete_cb frees it only when the magic still reads back.
    disable_clicks_recursive(dialog);

    // Mirrors the instance teardown. Both handlers already refuse to act on a
    // stale target on their own - backdrop_click_cb compares against the live
    // top backdrop, and defocus_tree() below takes the tree out of the focus
    // group before any ESC can reach it - so this is consistency and defence in
    // depth rather than a fix for a reachable path.
    lv_obj_remove_event_cb(backdrop, backdrop_click_cb);
    lv_obj_remove_event_cb(backdrop, esc_key_cb);

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
        owner->hide();
        return false;
    }

    spdlog::info("[Modal::rebuild_top] Modal '{}' cannot be rebuilt from XML (runtime attrs and "
                 "post-show wiring are not recoverable) - hiding instead",
                 name);
    Modal::hide(dialog);
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

void Modal::hide() {
    if (!backdrop_) {
        return; // Already hidden, safe to call multiple times
    }

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

    spdlog::info("[{}] Hiding modal", get_name());

    // Cancel all deferred async callbacks before any widget cleanup
    lifetime_.invalidate();

    // Clear user_data on all wired buttons in the dialog tree to prevent
    // stale vtable dispatch if events fire during exit animation after
    // the Modal subclass has been deleted (e.g. CrashReportModal's
    // async_call(delete this) in on_hide())
    if (dialog_) {
        clear_user_data_recursive(dialog_);
        // Also disable clickability on entire dialog tree so LVGL won't
        // deliver queued/delayed touch events during exit animation.
        // Defends against nested XML component buttons that
        // clear_user_data_recursive may not reach.
        disable_clicks_recursive(dialog_);
    }

    // Remove event callbacks from backdrop to prevent stale Modal* dispatch.
    // The backdrop has click and ESC handlers with `this` as user_data
    // (lv_event_get_user_data, NOT lv_obj_get_user_data). If on_hide()
    // schedules self-deletion, these callbacks would dispatch to freed memory.
    lv_obj_remove_event_cb(backdrop_, backdrop_click_cb);
    lv_obj_remove_event_cb(backdrop_, esc_key_cb);

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
    lv_obj_t* btn = find_widget(name);
    if (btn) {
        // Pass 'this' as the per-callback user_data (4th param), NOT via
        // lv_obj_set_user_data() which collides with ui_button's UiButtonData.
        // Per-callback user_data is read via lv_event_get_user_data(e).
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this);
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
        self->hide();
    } else {
        // Static modal - find in stack and close topmost
        auto& stack = ModalStack::instance();
        lv_obj_t* top_dialog = stack.top_dialog();
        lv_obj_t* top_backdrop = top_dialog ? stack.backdrop_for(top_dialog) : nullptr;

        if (top_backdrop == current_target) {
            spdlog::debug("[Modal] Backdrop clicked on topmost modal - closing");
            Modal::hide(top_dialog);
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
            Modal::hide(top);
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

lv_obj_t* helix::ui::modal_show_confirmation(const char* title, const char* message,
                                             ModalSeverity severity, const char* confirm_text,
                                             lv_event_cb_t on_confirm, lv_event_cb_t on_cancel,
                                             void* user_data, const char* cancel_text) {
    if (!title || !message) {
        spdlog::error("[Modal] show_confirmation: title and message are required");
        return nullptr;
    }

    // Build attributes array for modal_dialog
    const char* attrs[] = {"title", title, "message", message, nullptr};

    // Configure modal with severity and button text
    helix::ui::modal_configure(severity, true,
                               confirm_text ? confirm_text : "OK", // i18n: universal
                               cancel_text ? cancel_text : lv_tr("Cancel"));

    // Show the modal
    lv_obj_t* dialog = Modal::show("modal_dialog", attrs);
    if (!dialog) {
        spdlog::error("[Modal] Failed to create confirmation dialog: '{}'", title);
        return nullptr;
    }

    // Wire up cancel button (btn_secondary in modal_dialog)
    lv_obj_t* cancel_btn = lv_obj_find_by_name(dialog, "btn_secondary");
    if (cancel_btn) {
        lv_obj_add_event_cb(cancel_btn, on_cancel ? on_cancel : static_modal_close_cb,
                            LV_EVENT_CLICKED, on_cancel ? user_data : nullptr);
    }

    // Wire up confirm button (btn_primary in modal_dialog)
    lv_obj_t* confirm_btn = lv_obj_find_by_name(dialog, "btn_primary");
    if (confirm_btn) {
        lv_obj_add_event_cb(confirm_btn, on_confirm ? on_confirm : static_modal_close_cb,
                            LV_EVENT_CLICKED, on_confirm ? user_data : nullptr);
    }

    spdlog::debug("[Modal] Confirmation dialog shown: '{}'", title);
    return dialog;
}

lv_obj_t* helix::ui::modal_show_alert(const char* title, const char* message,
                                      ModalSeverity severity, const char* ok_text,
                                      lv_event_cb_t on_ok, void* user_data) {
    if (!title || !message) {
        spdlog::error("[Modal] show_alert: title and message are required");
        return nullptr;
    }

    // Build attributes array for modal_dialog
    const char* attrs[] = {"title", title, "message", message, nullptr};

    // Configure modal: no cancel button
    helix::ui::modal_configure(severity, false, ok_text ? ok_text : "OK",
                               nullptr); // i18n: universal

    // Show the modal
    lv_obj_t* dialog = Modal::show("modal_dialog", attrs);
    if (!dialog) {
        spdlog::error("[Modal] Failed to create alert dialog: '{}'", title);
        return nullptr;
    }

    // Wire up OK button - use provided callback or default close behavior
    lv_obj_t* ok_btn = lv_obj_find_by_name(dialog, "btn_primary");
    if (ok_btn) {
        lv_obj_add_event_cb(ok_btn, on_ok ? on_ok : static_modal_close_cb, LV_EVENT_CLICKED,
                            on_ok ? user_data : nullptr);
    }

    spdlog::debug("[Modal] Alert dialog shown: '{}'", title);
    return dialog;
}

lv_obj_t* helix::ui::show_low_ram_resonance_warning(size_t total_mb, lv_event_cb_t on_confirm,
                                                    lv_event_cb_t on_cancel, void* user_data) {
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
                                   lv_tr("Continue"), on_confirm, on_cancel, user_data);
}
