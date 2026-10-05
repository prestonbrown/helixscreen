// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <functional>

/**
 * @file ui_runout_guidance_modal.h
 * @brief Runout guidance modal with 6 action buttons
 *
 * Shown when filament runout is detected during a print pause.
 * Provides buttons for: Load Filament, Unload Filament, Purge,
 * Resume Print, Cancel Print, and OK (dismiss when idle).
 *
 * Button-to-callback mapping:
 * - btn_load_filament   -> on_ok()        (primary action)
 * - btn_unload_filament -> on_quaternary() (unload before loading new)
 * - btn_purge           -> on_quinary()   (purge after loading)
 * - btn_resume          -> on_cancel()    (resume paused print)
 * - btn_cancel_print    -> on_tertiary()  (cancel print)
 * - btn_ok              -> on_senary()    (dismiss when idle)
 *
 * @example
 *   runout_modal_.set_on_load_filament([this]() { start_load(); });
 *   runout_modal_.set_on_resume([this]() { resume_print(); });
 *   runout_modal_.show(lv_screen_active());
 */

/**
 * @brief Runout guidance modal with 6 action buttons
 *
 * Derives from Modal base class for RAII lifecycle management.
 */
class RunoutGuidanceModal : public Modal {
  public:
    using Callback = std::function<void()>;

    RunoutGuidanceModal() {
        init_subjects();
    }

    /**
     * @brief Get human-readable name for logging
     * @return "Runout Guidance"
     */
    const char* get_name() const override {
        return "Runout Guidance";
    }

    /**
     * @brief Set whether the active backend recovers filament on resume.
     *
     * Drives the capability-aware layout: when true (e.g. Snapmaker U1, where
     * Resume runs AUTO_FEEDING), the message reads "Refill the spool, then
     * Resume.", the manual Load/Unload/Purge row is demoted (muted + "Manual"
     * label), and Resume is emphasized. When false (basic runout sensor), Load
     * stays prominent and the message reads "Load filament, then Resume."
     *
     * Safe to call only on the main thread (the modal is shown from the main
     * thread). Updates the C++-owned, component-scoped subject the XML binds to.
     *
     * @param autofeed_capable true if Resume alone recovers a runout
     */
    void set_autofeed_capable(bool autofeed_capable) {
        lv_subject_set_int(&autofeed_capable_subject_, autofeed_capable ? 1 : 0);
    }

    /**
     * @brief Say whether this showing is advisory or a real runout.
     *
     * Drives the component-scoped `runout_is_advisory` subject the header icons
     * bind to: 0 picks the `alert` warning icon, 1 the neutral `filament` icon.
     * A deliberate tap on the home Filament tile is advisory; a runout that
     * paused a print is not.
     *
     * EVERY show site must state its own value rather than inheriting the
     * previous one's. The subject is static and component-scoped (it survives
     * show/hide and outlives any one modal instance), so a surface that only
     * sets it in the advisory direction latches the neutral icon on for the rest
     * of the session and silently downgrades the warning affordance on the next
     * real runout dialog.
     *
     * Main thread only.
     *
     * @param advisory true for a user-initiated informational show, false for a
     *                 real runout
     */
    void set_advisory(bool advisory) {
        lv_subject_set_int(&advisory_subject_, advisory ? 1 : 0);
    }

    /**
     * @brief Gate the Resume button on first-gate (port) filament presence (#991).
     *
     * Drives the component-scoped `runout_resume_blocked` subject the XML binds
     * to btn_resume's disabled state. When true, Resume is greyed/disabled —
     * used on auto-feed backends while no filament is present at the active
     * tool's port. When false, Resume is enabled (the default for non-auto-feed
     * or unknown backends, which must never be gated).
     *
     * Safe to call only on the main thread. The FilamentRunoutHandler maintains
     * this from an observer on AmsState's active-tool port-present subject.
     *
     * @param blocked true to disable Resume, false to enable it
     */
    void set_resume_blocked(bool blocked) {
        lv_subject_set_int(&resume_blocked_subject_, blocked ? 1 : 0);
    }

    /// Subject the handler observes/binds; exposed so the handler can read the
    /// current gate state. Component-scoped, statically stored (survives
    /// show/hide). Never null after construction (init_subjects runs in ctor).
    static lv_subject_t* resume_blocked_subject() {
        return &resume_blocked_subject_;
    }

    /// The advisory subject set_advisory() drives. Same storage rules as
    /// resume_blocked_subject(): component-scoped, static, never null after
    /// construction. Exposed so a caller (or a test) can read the current form
    /// without going through an XML scope lookup.
    static lv_subject_t* advisory_subject() {
        return &advisory_subject_;
    }

    /**
     * @brief Get XML component name for lv_xml_create()
     * @return "runout_guidance_modal"
     */
    const char* component_name() const override {
        return "runout_guidance_modal";
    }

    /**
     * @brief Set callback for Load Filament button (btn_load_filament -> on_ok)
     * @param cb Callback function
     */
    void set_on_load_filament(Callback cb) {
        on_load_filament_ = std::move(cb);
    }

    /**
     * @brief Set callback for Unload Filament button (btn_unload_filament -> on_quaternary)
     * @param cb Callback function
     */
    void set_on_unload_filament(Callback cb) {
        on_unload_filament_ = std::move(cb);
    }

    /**
     * @brief Set callback for Purge button (btn_purge -> on_quinary)
     * @param cb Callback function
     */
    void set_on_purge(Callback cb) {
        on_purge_ = std::move(cb);
    }

    /**
     * @brief Set callback for Resume button (btn_resume -> on_cancel)
     * @param cb Callback function
     */
    void set_on_resume(Callback cb) {
        on_resume_ = std::move(cb);
    }

    /**
     * @brief Set callback for Cancel Print button (btn_cancel_print -> on_tertiary)
     * @param cb Callback function
     */
    void set_on_cancel_print(Callback cb) {
        on_cancel_print_ = std::move(cb);
    }

    /**
     * @brief Set callback for OK button when idle (btn_ok -> on_senary)
     * @param cb Callback function
     */
    void set_on_ok_dismiss(Callback cb) {
        on_ok_dismiss_ = std::move(cb);
    }

    /// @name Which handlers are currently installed
    ///
    /// Every on_*() below null-checks its callback and then hide()s (or stays
    /// open) regardless, so an unwired button is indistinguishable from a wired
    /// one at the UI: it closes the dialog and does nothing. That shipped once —
    /// the home tile's paused modal showed a live "Resume Print" that never
    /// resumed. These predicates let a test pin "this surface wired what its
    /// visible buttons promise" instead of asserting on a dialog that always
    /// looks correct.
    /// @{
    [[nodiscard]] bool has_load_filament_handler() const {
        return static_cast<bool>(on_load_filament_);
    }
    [[nodiscard]] bool has_unload_filament_handler() const {
        return static_cast<bool>(on_unload_filament_);
    }
    [[nodiscard]] bool has_purge_handler() const {
        return static_cast<bool>(on_purge_);
    }
    [[nodiscard]] bool has_resume_handler() const {
        return static_cast<bool>(on_resume_);
    }
    [[nodiscard]] bool has_cancel_print_handler() const {
        return static_cast<bool>(on_cancel_print_);
    }
    /// @}

  protected:
    /**
     * @brief Called after modal is created and visible
     *
     * Wires up all 6 buttons to their respective handlers.
     */
    void on_show() override;

    /**
     * @brief Called when user clicks Load Filament button
     *
     * Invokes the load filament callback if set and keeps the modal open
     * (like Unload/Purge) so the user can purge after loading before resuming.
     */
    void on_ok() override {
        if (on_load_filament_) {
            on_load_filament_();
        }
        // Don't hide - user may want to purge after loading before resuming
    }

    /**
     * @brief Called when user clicks Resume button
     *
     * Invokes the resume callback if set, then hides the modal.
     */
    void on_cancel() override {
        if (on_resume_) {
            on_resume_();
        }
        hide();
    }

    /**
     * @brief Called when user clicks Cancel Print button
     *
     * Invokes the cancel print callback if set. Does NOT hide the modal - the
     * callback owns that, and only on the confirmed path.
     *
     * Cancel Print raises a confirmation (helix::ui::dispatch_cancel_print), so
     * this press no longer decides anything; it asks a question. Hiding here put
     * the dialog into its exit animation while the confirmation was still going
     * up, so declining left a bare screen - and on the runout path it did not
     * come back: check_and_show_runout_guidance() early-returns on
     * runout_modal_shown_for_pause_, which clears only on a transition to
     * Printing/Idle/Complete/Cancelled/Error. A user who reconsidered a cancel
     * mid-pause lost Load, Unload, Purge and Resume for the rest of that pause.
     *
     * Both callers pass dispatch_cancel_print() an `on_confirmed` that closes
     * this dialog, so an accepted cancel still tears it down. A caller that wires
     * on_cancel_print_ without closing anything leaves the dialog open on press;
     * that is only reachable where the XML shows btn_cancel_print at all, i.e.
     * print_state_enum == 2.
     */
    void on_tertiary() override {
        if (on_cancel_print_) {
            on_cancel_print_();
        }
    }

    /**
     * @brief Called when user clicks Unload Filament button
     *
     * Invokes the unload callback if set. Does not hide modal
     * since user may want to load after unload.
     */
    void on_quaternary() override {
        if (on_unload_filament_) {
            on_unload_filament_();
        }
        // Don't hide - user may want to load after unload
    }

    /**
     * @brief Called when user clicks Purge button
     *
     * Invokes the purge callback if set. Does not hide modal
     * since user may want to purge multiple times.
     */
    void on_quinary() override {
        if (on_purge_) {
            on_purge_();
        }
        // Don't hide - user may want to purge multiple times
    }

    /**
     * @brief Called when user clicks OK button (dismiss when idle)
     *
     * Invokes the ok dismiss callback if set, then hides the modal.
     */
    void on_senary() override {
        if (on_ok_dismiss_) {
            on_ok_dismiss_();
        }
        hide();
    }

  private:
    Callback on_load_filament_;
    Callback on_unload_filament_;
    Callback on_purge_;
    Callback on_resume_;
    Callback on_cancel_print_;
    Callback on_ok_dismiss_;

    // Capability subject driving the capable-aware layout (0 = manual Load
    // prominent, 1 = autofeed: Resume-first, manual row demoted). C++-owned and
    // registered into the modal's component scope — NOT an XML <subjects> block.
    // XML <subjects> are heap-freed before modal callbacks fire; a component-
    // scoped, statically-stored subject survives across show/hide cycles and
    // multiple instantiations. Pattern mirrors ShutdownModal::view_state_subject_.
    static inline lv_subject_t autofeed_capable_subject_{};
    // Resume-gate subject (#991): 1 = block (disable Resume), 0 = allow.
    // Default 0 so non-auto-feed / unknown backends are never gated.
    // Component-scoped like autofeed_capable_subject_.
    static inline lv_subject_t resume_blocked_subject_{};
    // Header-icon subject: 1 = advisory (neutral filament icon), 0 = real runout
    // (alert icon). Lives here, with the modal's other two component-scoped
    // subjects, rather than on any one consumer — it describes THIS dialog, and
    // an owner-specific home made it look like one surface's private state when
    // all three share it. Default 0: a caller that forgets set_advisory() gets
    // the warning icon, which is the safe direction to fail.
    static inline lv_subject_t advisory_subject_{};
    static inline bool subjects_initialized_ = false;

    static void init_subjects() {
        if (subjects_initialized_)
            return;
        subjects_initialized_ = true;

        lv_subject_init_int(&autofeed_capable_subject_, 0);
        lv_subject_init_int(&resume_blocked_subject_, 0);
        lv_subject_init_int(&advisory_subject_, 0);

        auto* scope = lv_xml_component_get_scope("runout_guidance_modal");
        if (scope) {
            lv_xml_register_subject(scope, "runout_autofeed_capable", &autofeed_capable_subject_);
            lv_xml_register_subject(scope, "runout_resume_blocked", &resume_blocked_subject_);
            lv_xml_register_subject(scope, "runout_is_advisory", &advisory_subject_);
        } else {
            spdlog::warn("[RunoutGuidanceModal] Component scope not found — "
                         "ensure runout_guidance_modal.xml is registered first");
        }

        StaticSubjectRegistry::instance().register_deinit("RunoutGuidanceModal", []() {
            if (!subjects_initialized_)
                return;
            if (auto* scope = lv_xml_component_get_scope("runout_guidance_modal")) {
                lv_xml_unregister_subject(scope, "runout_autofeed_capable");
                lv_xml_unregister_subject(scope, "runout_resume_blocked");
                lv_xml_unregister_subject(scope, "runout_is_advisory");
            }
            lv_subject_deinit(&autofeed_capable_subject_);
            lv_subject_deinit(&resume_blocked_subject_);
            lv_subject_deinit(&advisory_subject_);
            subjects_initialized_ = false;
        });
    }
};
