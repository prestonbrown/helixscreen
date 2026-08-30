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

#include <functional>
#include <memory>
#include <optional>
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

/**
 * @brief What closed a modal, for hooks that must tell caller closes from
 *        user/environment ones.
 *
 * Both hide() entry points default to Programmatic: the caller closed its own
 * dialog and already knows. The paths a caller does not control - backdrop tap,
 * ESC, a dialog button, an XML hot-reload rebuild - pass their own value, so
 * on_dismiss can mean exactly "closed by something other than you" instead of
 * "closed without a button callback", which also fired for the caller's own
 * teardown close (deferred one tick past a destructor - the UAF shape behind
 * the #1380 revert).
 */
enum class ModalCloseReason {
    Programmatic, ///< The caller's own code closed the dialog (hide() default)
    BackdropTap,  ///< User tapped the backdrop
    EscKey,       ///< User pressed ESC
    ButtonPress,  ///< A dialog button was pressed
    HotReload,    ///< XML hot reload replaced the dialog (dev builds)
    External,     ///< A system sweep closed it (ctl reset, fault-modal
                  ///< dismissal) - not the caller, so a dismissal reports
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

    // Non-copyable, non-movable. A moved Modal's wired buttons still carry the
    // moved-from pointer as per-callback user_data, so the move operations were
    // a latent hazard with zero production callers - only the two tests written
    // to test them ever exercised the retargeting.
    Modal(const Modal&) = delete;
    Modal& operator=(const Modal&) = delete;
    Modal(Modal&&) = delete;
    Modal& operator=(Modal&&) = delete;

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
     * @brief Show a heap modal and hand its lifetime to ModalStack
     *
     * The stack frees the instance when its entry leaves - exit-animation
     * completion after any close, or clear() at teardown. This replaces the
     * self-delete-from-on_hide() idiom, whose only free path was on_hide():
     * any teardown that bypassed hide() (ModalStack::clear() at soft restart,
     * hide()'s untracked-backdrop early return) leaked the object (#1382).
     *
     * One-shot modals only: an instance owned by the stack must never be
     * reshown, because its entry - and with it the instance - is destroyed
     * at the first close. Modals that are reshown stay owned by their member
     * or unique_ptr holder and use show().
     *
     * @param modal The instance to show; freed here on either outcome
     * @param parent Passed through to show() (ignored; always the active screen)
     * @param attrs Optional XML attributes
     * @return true if shown; on failure @p modal is destroyed, not leaked
     */
    static bool show_owned(std::unique_ptr<Modal> modal, lv_obj_t* parent,
                           const char** attrs = nullptr);

    /**
     * @brief Hide a modal by its dialog pointer
     * @param dialog Dialog object returned by show()
     * @param reason What closed it; Programmatic (the default) means the caller
     *        closed its own dialog, which does not fire a confirmation's
     *        on_dismiss. Modal's own input paths pass their reason so a
     *        dismissal is still reported.
     *
     * Delegates to the owning instance's hide() when the dialog has one, so
     * on_hide() and lifetime_ invalidation still run. The confirmation/alert
     * helpers are owned (see ConfirmationModal). A dialog from the bare static
     * Modal::show() factory has no owner and therefore no on_hide(): nothing
     * observes that close.
     */
    static void hide(lv_obj_t* dialog, ModalCloseReason reason = ModalCloseReason::Programmatic);

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
     * @param reason What closed it. Defaults to Programmatic - a caller closing
     *        its own modal. Modal's backdrop/ESC/hot-reload handlers pass their
     *        reason; a subclass that cares (ConfirmationModal) reads it from
     *        close_reason_ inside on_hide().
     *
     * Note: This overloads the static hide() - use modal.hide() for instances,
     * Modal::hide(dialog) for simple modals.
     */
    void hide(ModalCloseReason reason = ModalCloseReason::Programmatic);

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

    /// Why the in-progress (or most recent) hide() was initiated. Set on every
    /// hide() entry, before on_hide() runs; read it there to distinguish a
    /// caller's own close (Programmatic) from everything else.
    ModalCloseReason close_reason_ = ModalCloseReason::Programmatic;

    /// Async callback safety. Automatically invalidated on hide().
    /// Subclasses use lifetime_.defer(...) or lifetime_.token() for
    /// bg-thread callbacks that need to touch UI.
    helix::AsyncLifetimeGuard lifetime_;

    // Helpers
    lv_obj_t* find_widget(const char* name);

    /// Attach @p cb to the named button with `this` as per-callback user_data,
    /// which is what the on_ok()/on_cancel() hooks read back.
    void wire_button(const char* name, const char* role_name, lv_event_cb_t cb);

    /// Same, but with caller-supplied user_data passed through VERBATIM.
    ///
    /// Deliberately a separate entry point rather than a defaulted parameter: a
    /// `user_data ? user_data : this` fallback would substitute the owner
    /// whenever a caller's value is legitimately null, and callers do encode
    /// small integers in that pointer (tool_switcher_widget.cpp passes a tool
    /// index, so tool 0 IS nullptr). Modal::disarm_tree() only strips callbacks
    /// carrying the owner, so one wired here outlives the instance and must not
    /// dereference it.
    void wire_button_with(const char* name, const char* role_name, lv_event_cb_t cb,
                          void* user_data);
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

    /// Make a closing dialog inert: clear stale user_data, stop new presses,
    /// drop any press already in flight, and unwire the backdrop handlers.
    /// Shared by all three teardown paths (static hide, instance hide, ~Modal)
    /// so they cannot drift apart. Either argument may be null.
    /// @param owner When given, every event callback in the tree carrying this
    ///        pointer as per-callback user_data is removed. A stack-owned
    ///        instance is freed only when its entry goes, while its widgets
    ///        live out the exit animation first, and wire_button() parks `this`
    ///        on every button it wires - without this strip those are dangling.
    static void disarm_tree(lv_obj_t* backdrop, lv_obj_t* dialog, Modal* owner = nullptr);

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

    // Untrack a modal (called by Modal::destroy, animate_exit's no-animation
    // branch, and exit_animation_done). An entry that owns its instance frees
    // it here: synchronously when free_owned_now (timer context, preserves
    // instance-before-widget-tree order), else deferred one tick (hide()'s
    // frame is still on the stack).
    void remove(lv_obj_t* backdrop, bool free_owned_now = false);

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

    /// Take ownership of a shown modal's instance. The entry must exist (its
    /// show() succeeded); the instance is then freed when the entry leaves the
    /// stack - at exit-animation completion after a hide(), or in clear().
    /// This is the replacement for the self-delete-from-on_hide() idiom, which
    /// leaked whenever teardown bypassed hide() (ModalStack::clear() at soft
    /// restart, hide()'s untracked-backdrop early return) because nothing but
    /// on_hide() ever freed the object (#1382).
    void assume_ownership(lv_obj_t* backdrop, std::unique_ptr<Modal> instance);

    // Delete modal widgets, free owned instances, and clear tracking (used
    // during teardown after lv_anim_delete_all). Does NOT run on_hide(): a
    // subclass hook that queues work during final teardown has nothing left
    // to receive it.
    void clear();

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
        /// Set when this entry owns the instance (Modal::show_owned). Erasing
        /// the entry frees it, so every path that empties the stack also frees
        /// the modals it owns.
        std::unique_ptr<Modal> owned_instance;
    };

    std::vector<ModalEntry> stack_;

    static void exit_animation_done(lv_anim_t* anim);
};

// ============================================================================
// MODAL FREE FUNCTIONS (in helix::ui namespace)
// ============================================================================

namespace helix::ui {

// Canonical widget name carrying a modal's title text. Every C++ site that
// reads or writes a modal title by name (duplicate-title suppression in
// ui_notification, the reconnect auto-close in moonraker_manager,
// ActionPromptModal's title population) uses this constant; the XML side
// names the widget with the same literal, which is the one remaining copy.
// A modal whose title is not reachable under this name is invisible to those
// behaviors (issue #1389).
inline constexpr const char* kModalTitleWidgetName = "dialog_title";

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
 * @param on_dismiss Called when the dialog is closed by something other than
 *        the caller - a backdrop tap, ESC, a hot-reload rebuild, or a button
 *        press whose side carries no callback. The caller's own
 *        Modal::hide(dialog) does NOT fire it: the caller already knows. Pass
 *        this whenever the caller holds state the buttons are meant to resolve
 *        (a re-entry guard, a pending flag, a stored handle); without it that
 *        state leaks on a dismissal. It must not capture anything that can die
 *        before the dialog - the dialog outlives its exit animation.
 * @return The created dialog widget, or nullptr on failure
 *
 * @warning user_data is held by the button callbacks for as long as the dialog
 *          lives, which includes its exit animation. It must outlive the dialog.
 *          The std::function form (modal_confirm) has no user_data at all.
 */
lv_obj_t* modal_show_confirmation(const char* title, const char* message, ModalSeverity severity,
                                  const char* confirm_text, lv_event_cb_t on_confirm,
                                  lv_event_cb_t on_cancel, void* user_data,
                                  const char* cancel_text = nullptr,
                                  std::function<void()> on_dismiss = nullptr,
                                  std::optional<helix::LifetimeToken> dismiss_token = std::nullopt);

/**
 * @brief The optional tail of modal_confirm(), gathered into one struct
 *
 * C++17 has no designated initializers, so as positional parameters this tail
 * forced every caller that wanted only a dismissal or only a token to thread
 * nullptrs through the parameters in between - and each future parameter
 * would have re-multiplied them. As a struct, a caller sets exactly the
 * fields it means and leaves the rest defaulted.
 */
struct ConfirmOptions {
    /// Cancel-button callback; unset for a close-only cancel
    std::function<void()> on_cancel;

    /// Cancel-button text; nullptr defaults to "Cancel"
    const char* cancel_text = nullptr;

    /// Fired when the dialog is closed by something other than the caller -
    /// a backdrop tap, ESC, a hot-reload rebuild, or a button press whose
    /// side carries no callback. The caller's own Modal::hide(dialog) does
    /// NOT fire it: the caller already knows. Pass it whenever the caller
    /// holds state the buttons are meant to resolve (a re-entry guard, a
    /// pending flag, a stored handle); without it that state leaks on a
    /// dismissal.
    std::function<void()> on_dismiss;

    /// Gates ALL THREE callbacks, not just the dismissal: none of them runs
    /// once the token has expired. Pass it whenever a callback captures
    /// something that can die before the dialog does - which is the usual
    /// case, since the dialog outlives its exit animation. This is the one
    /// thing the lv_event_cb_t form cannot offer: its callbacks are invoked
    /// by LVGL directly off the button, so a capture there simply has to
    /// outlive the dialog.
    std::optional<helix::LifetimeToken> owner_token;
};

/**
 * @brief The optional tail of modal_alert() - see ConfirmOptions for why a
 *        struct. An alert has no cancel side, so only the dismissal report
 *        and the token remain.
 */
struct AlertOptions {
    /// See ConfirmOptions::on_dismiss
    std::function<void()> on_dismiss;

    /// See ConfirmOptions::owner_token
    std::optional<helix::LifetimeToken> owner_token;
};

/**
 * @brief Confirmation dialog whose callbacks never touch a widget
 *
 * The declarative form of modal_show_confirmation(). The callbacks are
 * std::function, invoked from the modal's own on_ok()/on_cancel() hooks, so
 * there is no lv_event_cb_t to attach to a button and no void* user_data to
 * outlive the dialog. Prefer this for new code; the lv_event_cb_t form remains
 * for the existing call sites (prestonbrown/helixscreen#1383).
 *
 * This form also closes the dialog itself when a button is pressed - the
 * lv_event_cb_t form leaves that to the caller.
 *
 * @param options The optional tail - cancel callback/text, dismissal report,
 *        owner token - one field per concern, nothing threaded positionally.
 * @return The created dialog widget, or nullptr on failure
 */
lv_obj_t* modal_confirm(const char* title, const char* message, ModalSeverity severity,
                        const char* confirm_text, std::function<void()> on_confirm,
                        const ConfirmOptions& options = {});

/**
 * @brief Single-button alert whose callback never touches a widget
 *
 * Declarative counterpart to modal_show_alert() - see modal_confirm().
 */
lv_obj_t* modal_alert(const char* title, const char* message,
                      ModalSeverity severity = ModalSeverity::Info, const char* ok_text = "OK",
                      std::function<void()> on_ok = nullptr, const AlertOptions& options = {});

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
 * @warning Same caveats as modal_show_confirmation(): user_data must outlive the
 *          dialog. Pass on_dismiss to learn about a close the caller did not
 *          initiate - the caller's own Modal::hide(dialog) reports nothing.
 */
lv_obj_t* modal_show_alert(const char* title, const char* message,
                           ModalSeverity severity = ModalSeverity::Info, const char* ok_text = "OK",
                           lv_event_cb_t on_ok = nullptr, void* user_data = nullptr,
                           std::function<void()> on_dismiss = nullptr,
                           std::optional<helix::LifetimeToken> dismiss_token = std::nullopt);

/**
 * @brief Show the "low RAM before resonance calibration" warning modal.
 *
 * Centralizes the (translated) copy and severity so the two calibration entry
 * points (input-shaper panel + wizard) can't diverge. Caller has already
 * decided RAM is below helix::RESONANCE_LOW_RAM_WARN_MB. Returns the dialog
 * handle (store it to dismiss on teardown) or nullptr on failure.
 *
 * @param on_dismiss Called when the dialog is closed by something other than
 *        the caller - a backdrop tap, ESC, or a hot-reload rebuild; the
 *        caller's own Modal::hide() reports nothing. Both callers gate
 *        re-entry on the returned handle being null, so a dismissal that
 *        leaves it set makes every later attempt a silent no-op - clear the
 *        handle from here. Pass @p dismiss_token too when the capture can die
 *        before the dialog. Do NOT hand-roll an
 *        LV_EVENT_DELETE hook for this: one that outlives its owner is the
 *        use-after-free that got prestonbrown/helixscreen#1380 reverted.
 */
lv_obj_t*
show_low_ram_resonance_warning(size_t total_mb, lv_event_cb_t on_confirm, lv_event_cb_t on_cancel,
                               void* user_data, std::function<void()> on_dismiss = nullptr,
                               std::optional<helix::LifetimeToken> dismiss_token = std::nullopt);

} // namespace helix::ui
