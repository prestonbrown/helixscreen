// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_modal.h
 * @brief Unified modal system with RAII lifecycle, backdrop, stacking, and animations
 *
 * @pattern RAII lifecycle; subclass hooks (on_show/on_ok/on_cancel); ModalStack singleton
 * @threading Main thread only
 * @gotchas Both static and instance show() methods; mark_exiting() flag for animation state
 */

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

// Forward declaration
class Modal;

/**
 * @brief Severity levels for modal dialogs
 */
enum class ModalSeverity {
    Info = 0,
    Warning = 1,
    Error = 2,
};

// ============================================================================
// MODAL CLASS
// ============================================================================

/**
 * @brief Unified modal dialog system
 *
 * Combines the functionality of ModalBase (OOP hooks) and ModalManager
 * (stack tracking, animations). Provides:
 *
 * - RAII lifecycle (destructor auto-hides if visible)
 * - Backdrop created in C++ (not in XML)
 * - Modal stacking with proper z-order
 * - Backdrop click-to-close and ESC handling
 * - Standard Ok/Cancel button wiring
 * - Move semantics support
 *
 * ## Usage - Simple Modals (no subclass):
 * @code
 * lv_obj_t* modal = Modal::show("print_cancel_confirm_modal");
 * // ...later...
 * Modal::hide(modal);
 * @endcode
 *
 * ## Usage - Complex Modals (subclass):
 * @code
 * class SpoolEditModal : public Modal {
 * public:
 *     const char* get_name() const override { return "Spool Edit"; }
 *     const char* component_name() const override { return "spoolman_edit_modal"; }
 *
 * protected:
 *     void on_ok() override {
 *         save_changes();
 *         Modal::on_ok();
 *     }
 * };
 * @endcode
 */
class Modal {
  public:
    Modal();
    virtual ~Modal();

    // Non-copyable
    Modal(const Modal&) = delete;
    Modal& operator=(const Modal&) = delete;

    // Movable
    Modal(Modal&& other) noexcept;
    Modal& operator=(Modal&& other) noexcept;

    // ========================================================================
    // STATIC FACTORY API (for simple modals)
    // ========================================================================

    /**
     * @brief Show a simple modal (no subclass needed)
     *
     * Creates and displays a modal from the specified XML component.
     * Backdrop is created in C++ and XML content is placed inside it.
     *
     * @param component_name XML component name
     * @param attrs Optional XML attributes (NULL-terminated)
     * @return Pointer to the modal's dialog object (for button wiring etc.)
     */
    static lv_obj_t* show(const char* component_name, const char** attrs = nullptr);

    /**
     * @brief Hide a modal by its dialog pointer
     * @param dialog Dialog object returned by show()
     *
     * Delegates to the owning instance's hide() when the dialog was shown
     * through the instance API, so on_hide() and lifetime_ invalidation still
     * run. A dialog from the static factory or the confirmation/alert helpers
     * has no owner and therefore no on_hide(): nothing observes that close.
     */
    static void hide(lv_obj_t* dialog);

    /**
     * @brief Get the topmost modal's dialog
     * @return Dialog object, or nullptr if no modals visible
     */
    static lv_obj_t* get_top();

    /**
     * @brief Check if any modals are visible
     */
    static bool any_visible();

    /**
     * @brief Rebuild the top modal by hiding it and re-showing the same component
     *
     * Dev-only hook for XML hot-reload. Does nothing if no modal is visible.
     *
     * An instance-backed modal (shown through a Modal subclass) is only hidden,
     * never rebuilt: re-creating it from XML alone would skip on_show() and the
     * subclass's button wiring, leaving a dialog whose buttons do nothing.
     *
     * @return true if a modal was rebuilt, false if there was nothing to rebuild
     *         or the top modal is instance-backed and was hidden instead
     */
    static bool rebuild_top();

    // ========================================================================
    // INSTANCE API (for subclassed modals)
    // ========================================================================

    /**
     * @brief Show this modal instance
     * @param parent Ignored — always uses lv_screen_active() to avoid stale pointers
     * @param attrs Optional XML attributes
     * @return true if shown successfully
     *
     * Note: This overloads the static show() - use modal.show(parent) for instances,
     * Modal::show("name", config) for simple modals.
     */
    bool show(lv_obj_t* parent, const char** attrs = nullptr);

    /**
     * @brief Hide this modal instance
     *
     * Note: This overloads the static hide() - use modal.hide() for instances,
     * Modal::hide(dialog) for simple modals.
     */
    void hide();

    /**
     * @brief Check if this modal is currently visible
     */
    bool is_visible() const {
        return backdrop_ != nullptr;
    }

    /**
     * @brief Get this modal's dialog object
     */
    lv_obj_t* dialog() const {
        return dialog_;
    }

    /**
     * @brief Get this modal's backdrop object
     */
    lv_obj_t* backdrop() const {
        return backdrop_;
    }

    // ========================================================================
    // PURE VIRTUAL (must implement in subclass)
    // ========================================================================

    /**
     * @brief Human-readable name for logging
     */
    virtual const char* get_name() const = 0;

    /**
     * @brief XML component name for lv_xml_create()
     */
    virtual const char* component_name() const = 0;

    // ========================================================================
    // HOOKS (override in subclass)
    // ========================================================================

    /**
     * @brief Called after modal is created and visible
     */
    virtual void on_show() {}

    /**
     * @brief Called before modal is destroyed
     */
    virtual void on_hide() {}

    /**
     * @brief Called when Ok button is clicked (default: hides)
     */
    virtual void on_ok() {
        hide();
    }

    /**
     * @brief Called when Cancel button is clicked (default: hides)
     */
    virtual void on_cancel() {
        hide();
    }

    /**
     * @brief Called when Tertiary button is clicked (default: hides)
     *
     * Used for 3-button modals like runout guidance (Load/Resume/Cancel Print).
     */
    virtual void on_tertiary() {
        hide();
    }

    /**
     * @brief Called when Quaternary button is clicked (default: hides)
     *
     * Used for 4+ button modals like extended runout guidance.
     */
    virtual void on_quaternary() {
        hide();
    }

    /**
     * @brief Called when Quinary button is clicked (default: hides)
     */
    virtual void on_quinary() {
        hide();
    }

    /**
     * @brief Called when Senary button is clicked (default: hides)
     */
    virtual void on_senary() {
        hide();
    }

  protected:
    // Modal state
    lv_obj_t* backdrop_ = nullptr;
    lv_obj_t* dialog_ = nullptr;
    lv_obj_t* parent_ = nullptr;

    /// Async callback safety. Automatically invalidated on hide().
    /// Subclasses use lifetime_.defer(...) or lifetime_.token() for
    /// bg-thread callbacks that need to touch UI.
    helix::AsyncLifetimeGuard lifetime_;

    // Helpers
    lv_obj_t* find_widget(const char* name);
    void wire_ok_button(const char* name = "btn_primary");
    void wire_cancel_button(const char* name = "btn_secondary");
    void wire_tertiary_button(const char* name = "btn_tertiary");
    void wire_quaternary_button(const char* name = "btn_quaternary");
    void wire_quinary_button(const char* name = "btn_quinary");
    void wire_senary_button(const char* name = "btn_senary");

  private:
    // Internal implementation
    bool create_and_show(lv_obj_t* parent, const char* comp_name, const char** attrs);
    void destroy();
    void wire_button(const char* name, const char* role_name, lv_event_cb_t cb);

    // Static event handlers
    static void backdrop_click_cb(lv_event_t* e);
    static void esc_key_cb(lv_event_t* e);
    static void ok_button_cb(lv_event_t* e);
    static void cancel_button_cb(lv_event_t* e);
    static void tertiary_button_cb(lv_event_t* e);
    static void quaternary_button_cb(lv_event_t* e);
    static void quinary_button_cb(lv_event_t* e);
    static void senary_button_cb(lv_event_t* e);
};

// ============================================================================
// MODAL MANAGER (internal stack tracking)
// ============================================================================

/**
 * @brief Internal singleton for modal stack management
 *
 * Not meant to be used directly - use Modal::show() instead.
 */
class ModalStack {
  public:
    static ModalStack& instance();

    // Track a modal (called by Modal::create_and_show).
    // `owner` is the Modal instance that shows and tears the dialog down, or
    // nullptr for modals created through the static Modal::show() factory.
    void push(lv_obj_t* backdrop, lv_obj_t* dialog, const std::string& component_name,
              Modal* owner = nullptr);

    // Untrack a modal (called by Modal::destroy)
    void remove(lv_obj_t* backdrop);

    /// Return the Modal instance that owns this dialog, or nullptr when the
    /// dialog is untracked or was created through the static factory.
    Modal* owner_for(lv_obj_t* dialog) const;

    /// Point an existing entry at a different owner (nullptr = no owner).
    /// No-op if the backdrop is not tracked.
    void reassign_owner(lv_obj_t* backdrop, Modal* new_owner);

    // Get topmost dialog
    lv_obj_t* top_dialog() const;

    /// Return the component name of the top (most recent non-exiting) modal, or "" if empty.
    std::string top_component_name() const;

    // Get backdrop for a dialog
    lv_obj_t* backdrop_for(lv_obj_t* dialog) const;

    // Check if a backdrop is still tracked in the stack
    bool backdrop_for_backdrop(lv_obj_t* backdrop) const;

    // Check if any modals are visible (not counting those in exit animation)
    bool empty() const;

    // Check if stack is completely empty (including exiting)
    bool stack_empty() const {
        return stack_.empty();
    }

    // Delete modal widgets and clear tracking (used during teardown after lv_anim_delete_all)
    void clear() {
        for (auto& entry : stack_) {
            // Skip backdrops that LVGL already freed. A modal whose exit
            // animation never completed stays in the stack as exiting=true with
            // its remove() still owed to exit_animation_done(). If the backdrop's
            // ancestor (e.g. the screen it was created on) is deleted first, LVGL
            // frees the backdrop as part of that subtree — leaving a stale stack
            // entry that points at freed memory. Deleting it again walks a
            // poisoned object (obj->class_p) and crashes. Guard the same way
            // exit_animation_done() and ~Modal already do (lv_obj_is_valid).
            if (!entry.backdrop || !lv_obj_is_valid(entry.backdrop)) {
                continue;
            }
            // Cancel animations before deletion — exit animation exec callbacks
            // trigger lv_obj_set_style_*() which crashes on freed objects
            lv_anim_delete(entry.backdrop, nullptr);
            lv_obj_t* dialog = lv_obj_get_child(entry.backdrop, 0);
            if (dialog) {
                lv_anim_delete(dialog, nullptr);
            }
            lv_obj_delete(entry.backdrop);
        }
        stack_.clear();
    }

    // Mark a modal as exiting (animation in progress, ignore further hide() calls)
    // Returns true if found and marked, false if not found or already exiting
    bool mark_exiting(lv_obj_t* backdrop);

    // Check if a modal is currently in exit animation
    bool is_exiting(lv_obj_t* backdrop) const;

    // Animation helpers
    void animate_entrance(lv_obj_t* dialog);
    void animate_exit(lv_obj_t* backdrop, lv_obj_t* dialog);

  private:
    ModalStack() = default;

    struct ModalEntry {
        lv_obj_t* backdrop;
        lv_obj_t* dialog;
        std::string component_name;
        bool exiting; /**< true = exit animation in progress, ignore hide() calls */
        Modal* owner; /**< owning instance, or nullptr for static Modal::show() modals */
    };

    std::vector<ModalEntry> stack_;

    static void exit_animation_done(lv_anim_t* anim);
};

// ============================================================================
// MODAL FREE FUNCTIONS (in helix::ui namespace)
// ============================================================================

namespace helix::ui {

/**
 * @brief Initialize subjects for modal_dialog.xml bindings
 *
 * Call once during app startup before any modal_dialog is shown.
 */
void modal_init_subjects();

/**
 * @brief Deinitialize modal dialog subjects for clean shutdown
 */
void modal_deinit_subjects();

/**
 * @brief Configure modal_dialog before showing
 */
void modal_configure(ModalSeverity severity, bool show_cancel, const char* primary_text,
                     const char* cancel_text);

// Subject accessors
lv_subject_t* modal_get_severity_subject();
lv_subject_t* modal_get_show_cancel_subject();
lv_subject_t* modal_get_primary_text_subject();
lv_subject_t* modal_get_cancel_text_subject();

inline lv_obj_t* modal_show(const char* name, const char** attrs = nullptr) {
    return Modal::show(name, attrs);
}

inline void modal_hide(lv_obj_t* dialog) {
    Modal::hide(dialog);
}

inline lv_obj_t* modal_get_top() {
    return Modal::get_top();
}

/**
 * @brief Register a textarea for keyboard display within a modal
 *
 * Positions the keyboard at bottom-center and registers the textarea.
 * Automatically detects password mode for masking.
 *
 * @param modal The modal dialog (used for logging only)
 * @param textarea The textarea widget to register
 */
void modal_register_keyboard(lv_obj_t* modal, lv_obj_t* textarea);

/**
 * @brief Show a confirmation dialog with callbacks
 *
 * Consolidates the common pattern of:
 * 1. Configure modal severity and button text
 * 2. Show modal_dialog with title/message
 * 3. Wire up confirm/cancel button callbacks
 *
 * @param title Dialog title text
 * @param message Dialog message text
 * @param severity Visual severity (Info, Warning, Error)
 * @param confirm_text Primary button text (e.g., "Delete", "Proceed")
 * @param on_confirm Callback for confirm button (receives user_data)
 * @param on_cancel Callback for cancel button (receives user_data), or nullptr for no callback
 * @param user_data User data passed to callbacks
 * @param cancel_text Secondary button text, or nullptr to default to "Cancel"
 * @return The created dialog widget, or nullptr on failure
 *
 * @warning Dismissal is NOT reported. This pushes with no owner, so closing the
 *          dialog by backdrop tap, ESC, or Modal::rebuild_top() calls neither
 *          on_confirm nor on_cancel. If the caller holds state those callbacks
 *          are meant to resolve (a re-entry guard, a pending-request flag, a
 *          stored dialog pointer), it leaks on dismissal. Own a Modal subclass
 *          in that case and resolve in on_hide(), which always runs - see
 *          include/lan_client_auth_router.h.
 * @warning user_data is held by the button callbacks for as long as the dialog
 *          lives, which includes its exit animation. It must outlive the dialog.
 */
lv_obj_t* modal_show_confirmation(const char* title, const char* message, ModalSeverity severity,
                                  const char* confirm_text, lv_event_cb_t on_confirm,
                                  lv_event_cb_t on_cancel, void* user_data,
                                  const char* cancel_text = nullptr);

/**
 * @brief Show an info/alert dialog with single "OK" button
 *
 * Simplified version for informational dialogs with no cancel button.
 *
 * @param title Dialog title text
 * @param message Dialog message text
 * @param severity Visual severity (default: Info)
 * @param ok_text Button text (default: "OK")
 * @param on_ok Callback for OK button (receives user_data), or nullptr
 * @param user_data User data passed to callback
 * @return The created dialog widget, or nullptr on failure
 *
 * @warning Same ownership caveats as modal_show_confirmation(): no owner, so a
 *          dismissal reports nothing, and user_data must outlive the dialog.
 */
lv_obj_t* modal_show_alert(const char* title, const char* message,
                           ModalSeverity severity = ModalSeverity::Info, const char* ok_text = "OK",
                           lv_event_cb_t on_ok = nullptr, void* user_data = nullptr);

/**
 * @brief Show the "low RAM before resonance calibration" warning modal.
 *
 * Centralizes the (translated) copy and severity so the two calibration entry
 * points (input-shaper panel + wizard) can't diverge. Caller has already
 * decided RAM is below helix::RESONANCE_LOW_RAM_WARN_MB. Returns the dialog
 * handle (store it to dismiss on teardown) or nullptr on failure.
 */
lv_obj_t* show_low_ram_resonance_warning(size_t total_mb, lv_event_cb_t on_confirm,
                                         lv_event_cb_t on_cancel, void* user_data);

} // namespace helix::ui
