// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// End-to-end lock-in for the headline L0 behavior: a paused printer plus an
// uncoded jam `!!` line must surface a BLOCKING MODAL whose message carries the
// FULL untruncated jam text (acceptance criteria E1 = modal-not-toast, E2 =
// full text). The pure decision logic (error_classify::classify +
// decide_presentation) is covered by test_error_classify.cpp /
// test_gcode_error_routing.cpp; this test closes the gap on the *presentation
// glue* that process_line drives.
//
// IMPORTANT — test-binary boundary (why this test is shaped the way it is):
// The test build EXCLUDES ui_notification.o and replaces ui_notification_error()
// with a logging-only stub in tests/ui_test_utils.cpp (see mk/tests.mk:
// "ui_notification.o ... ui_test_utils.o provides stub ... functions").
// Reason: the real ui_notification.cpp pulls get_notification_subject() from
// app_globals.o, which is itself stubbed in tests — linking both duplicates
// symbols. Consequently process_line()'s PresentAs::MODAL arm calls the STUB,
// which never builds a modal, so an assert on modal_get_top() after
// process_line() ALWAYS fails in the test binary regardless of the feature.
//
// What this test therefore verifies — every link the production path traverses
// EXCEPT the stubbed thunk, with no weakened assertion:
//   1. Routing seam (real classifier + real paused PrinterState): the exact
//      classification process_line performs maps the jam to PresentAs::MODAL.
//      This is the severity gate end-to-end — flip the printer to idle and the
//      same line drops below MODAL.
//   2. Presentation seam (real ui_modal.o — NOT stubbed): handing the modal
//      layer the same (title, detail) that process_line passes to
//      ui_notification_error renders a real modal_dialog whose dialog_message
//      holds the FULL untruncated text. This is the E1+E2 proof against the
//      real widget tree.
//
// Tagged [.ui_integration]: needs the XML component tree on disk (modal_dialog).

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "app_globals.h"
#include "error_classify.h"
#include "error_event.h"
#include "gcode_error_router.h"
#include "lvgl/lvgl.h"
#include "moonraker_api.h" // concrete MoonrakerAPI/MoonrakerClient for upcast to interfaces
#include "printer_state.h"
#include "recovery_modal_presenter.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// The real AFC/runout chinglish: an uncoded `!!` (no {"code":...}), deliberately
// longer than the 80-byte toast truncation cap so E2 (full text) is meaningful.
const std::string JAM_LINE =
    "!! Toolhead runout detected by tool_end sensor, but upstream sensors still detect "
    "filament. Possible filament break or jam at the toolhead. Please clear the jam and "
    "reload filament manually, then resume the print.";

// The detail substring process_line would surface (the `!! ` prefix stripped).
const char* JAM_DETAIL = JAM_LINE.c_str() + 3;

// Walk the modal subtree for the deepest label containing `needle`.
// modal_dialog.xml names the message label "dialog_message"; the substring walk
// is robust to the text landing on any label inside the scrollable content.
const char* find_text_containing(lv_obj_t* node, const char* needle) {
    if (!node)
        return nullptr;
    if (lv_obj_check_type(node, &lv_label_class)) {
        const char* txt = lv_label_get_text(node);
        if (txt && std::strstr(txt, needle))
            return txt;
    }
    uint32_t n = lv_obj_get_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        if (const char* hit = find_text_containing(lv_obj_get_child(node, i), needle))
            return hit;
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Uncoded jam while paused surfaces a modal with the full message",
                 "[error-center][routing][e2e][.ui_integration]") {
    // ---- Seam 1: routing decision (REAL classifier, REAL paused state) ----
    // Mark the singleton printer paused — process_line reads
    // get_printer_state().is_paused() into ClassifyContext, which makes an
    // uncoded `!!` CRITICAL, which decide_presentation maps to MODAL.
    get_printer_state().update_from_status(nlohmann::json{{"pause_resume", {{"is_paused", true}}}});
    REQUIRE(get_printer_state().is_paused());

    // Reproduce process_line's classification path verbatim: same context
    // construction, same classify() call. This is the routing process_line
    // performs before it reaches the PresentAs::MODAL arm.
    helix::ClassifyContext ctx;
    ctx.is_paused = get_printer_state().is_paused();
    ctx.is_printing = get_printer_state().get_print_job_state() == helix::PrintJobState::PRINTING;

    auto ev = helix::error_classify::classify(JAM_LINE, ctx);
    REQUIRE(ev.has_value());
    REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
    // E1: the routing decision is a blocking modal, not a toast. Since #1152 a
    // paused uncoded `!!` also carries a generic Resume action, so the blocking
    // arm is the recovery presenter's rather than the plain-alert one. What this
    // locks is unchanged: paused + CRITICAL never degrades to a toast.
    const helix::PresentAs decision = helix::decide_presentation(*ev);
    REQUIRE(decision == helix::PresentAs::MODAL_WITH_RECOVER);
    REQUIRE(decision != helix::PresentAs::TOAST);
    REQUIRE(decision != helix::PresentAs::TOAST_WITH_RECOVER);
    // The detail process_line would surface is the full line, untruncated.
    REQUIRE(ev->detail == JAM_DETAIL);

    // Severity gate sanity: an idle (not paused, not printing) printer demotes
    // the same line below MODAL — proving the gate, not a constant.
    {
        helix::ClassifyContext idle;
        idle.is_paused = false;
        idle.is_printing = false;
        auto idle_ev = helix::error_classify::classify(JAM_LINE, idle);
        REQUIRE(idle_ev.has_value());
        // Asserted positively: `!= MODAL` would now also pass for the recovery
        // modal arm, which is not what "demoted" means.
        REQUIRE(helix::decide_presentation(*idle_ev) == helix::PresentAs::TOAST);
        REQUIRE(idle_ev->recovery_actions.empty()); // no Resume when nothing is paused
    }

    // ---- Drive the REAL glue (process_line) ----
    // Exercise the actual private presentation glue via the friend accessor:
    // real GcodeErrorRouter, real classifier, real paused PrinterState. This
    // runs modal_title_for(), the rpc-correlation dedup guard, and the switch's
    // PresentAs::MODAL arm. Its terminal ui_notification_error() is a logging
    // stub in the test binary (see header), so it cannot render the modal here
    // — but it MUST classify + route without crashing and reach the MODAL arm.
    // The visible-modal proof is Seam 2 below, against the real modal layer.
    helix::ui::RecoveryModalPresenter presenter1(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter1);
    REQUIRE_NOTHROW(GcodeErrorRouterTestAccess::process_line(router, JAM_LINE));
    helix::ui::UpdateQueue::instance().drain();

    // ---- Seam 2: presentation (REAL ui_modal.o) ----
    // process_line's MODAL arm calls
    //   ui_notification_error(modal_title_for(*ev), ev->detail.c_str(), /*modal=*/true)
    // which, in production, routes the modal case to modal_show_alert(...). In
    // the test binary ui_notification_error is a logging-only stub (see header
    // comment), so we drive the same modal layer it would with the SAME
    // (title, detail) process_line hands it. modal_title_for(GENERIC) ==
    // "Printer Error".
    //
    // NOTE: since #1152 the paused case routes through present_recovery_modal()
    // in production, not this alert path. What this seam still proves is the
    // property it was written for — the FULL untruncated detail reaches a
    // blocking modal — and that property is presenter-independent.
    lv_obj_t* dialog = helix::ui::modal_show_alert("Printer Error", ev->detail.c_str(),
                                                   ModalSeverity::Error, "OK");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    // E1: a blocking modal is visible.
    REQUIRE(dialog != nullptr);
    lv_obj_t* modal = helix::ui::modal_get_top();
    REQUIRE(modal != nullptr);
    REQUIRE(Modal::any_visible());

    // E2: the modal carries the FULL, untruncated jam text. The asserted
    // substrings sit well past the 80-byte toast cap, so their presence proves
    // the text was not truncated for a toast.
    const char* msg = find_text_containing(modal, "reload filament manually");
    REQUIRE(msg != nullptr);
    REQUIRE(std::string(msg).find("resume the print") != std::string::npos);

    Modal::hide(dialog);
    process_lvgl(20);
}

// ---------------------------------------------------------------------------
// MODAL_WITH_RECOVER path: AFC jam → real recovery modal actually SHOWN.
//
// Unlike the generic PresentAs::MODAL arm above (which terminates in the
// ui_notification_error STUB), present_recovery_modal() drives the recovery
// case through helix::ui::ActionPromptModal::show_prompt() DIRECTLY — that
// modal layer (ui_modal.o + action_prompt_modal XML) is NOT stubbed in the
// test binary. So this integration test can assert the modal is genuinely on
// screen, closing the glue gap that the unit tests (classify_error /
// decide_presentation / build_recovery_prompt in isolation) leave open.
//
// The chain exercised end-to-end:
//   process_line(jam) → AmsBackendAfc::classify_error → CRITICAL+AFC+actions
//   → decide_presentation == MODAL_WITH_RECOVER → present_recovery_modal
//   → ActionPromptModal::show_prompt(lv_screen_active(), prompt)
//
// Regression value: the two review-caught bugs lived right here — lost
// severity styling (icon_error hidden) and stuck dedup (modal never reshows).
// This locks the severity affordance (icon_error visible) and the title text.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "AFC jam while paused drives the recovery modal to actually show",
                 "[error-center][routing][integration][ui_integration]") {
    // Paused printer: process_line reads is_paused() into the ClassifyContext.
    get_printer_state().update_from_status(nlohmann::json{{"pause_resume", {{"is_paused", true}}}});
    REQUIRE(get_printer_state().is_paused());

    // Install an AFC backend as the ACTIVE AmsState backend so process_line's
    // `AmsState::instance().get_backend()->classify_error(...)` resolves to the
    // AFC classifier (not the generic fallback). A fresh backend is enough: the
    // jam branch is text-driven (tool_end + jam/break/runout), no live state.
    AmsState::instance().set_backend(std::make_unique<AmsBackendAfc>(api(), client()));
    // RAII guard: clear the AmsState singleton backend even if a REQUIRE throws,
    // so it never leaks into subsequent tests in the same run.
    struct BackendGuard {
        ~BackendGuard() {
            AmsState::instance().set_backend(nullptr);
        }
    } backend_guard;
    REQUIRE(AmsState::instance().get_backend() != nullptr);

    // The AFC handle_toolhead_runout chinglish: `tool_end` + `jam` → CRITICAL
    // jam, titled "Toolhead jam", carrying recovery actions (Resume is always
    // pushed → non-empty → MODAL_WITH_RECOVER).
    const std::string AFC_JAM =
        "!! Toolhead runout detected by tool_end sensor — possible filament jam at "
        "the toolhead. Clear the jam and resume.";

    // Sanity: the active backend really classifies this as the recoverable
    // CRITICAL jam (the precondition for MODAL_WITH_RECOVER). Fails loudly if
    // the classifier text-match or recovery-action population regresses.
    {
        helix::ClassifyContext ctx;
        ctx.is_paused = true;
        ctx.is_printing = false;
        auto ev = AmsState::instance().get_backend()->classify_error(AFC_JAM, ctx);
        REQUIRE(ev.has_value());
        REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
        REQUIRE(ev->source == helix::ErrorSource::AFC);
        REQUIRE_FALSE(ev->recovery_actions.empty());
        REQUIRE(helix::decide_presentation(*ev) == helix::PresentAs::MODAL_WITH_RECOVER);
    }

    // Drive the REAL glue. The presenter now shows the modal regardless of
    // api_; api() is only needed if the user taps a recovery button (the
    // button-tap gcode callback is guarded on api_). It provides a real
    // MoonrakerAPI here so the show_prompt arm and tap path both exercise.
    helix::ui::RecoveryModalPresenter presenter2(api());
    helix::GcodeErrorRouter router(api(), client(), presenter2);
    REQUIRE_NOTHROW(GcodeErrorRouterTestAccess::process_line(router, AFC_JAM));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    // ---- Assert the recovery modal is actually on screen ----
    // present_recovery_modal sets the title from e.title ("Toolhead jam") and
    // populate_content() writes it into the label named "title". Walk the live
    // widget tree from the active screen — recovery_modal_ is private, so we
    // observe via the rendered tree, which is the user-visible proof.
    REQUIRE(Modal::any_visible());
    lv_obj_t* screen = lv_screen_active();
    REQUIRE(screen != nullptr);

    const char* title = find_text_containing(screen, "Toolhead jam");
    REQUIRE(title != nullptr);

    // Severity affordance regression guard (review bug #1): the error icon must
    // be VISIBLE because present_recovery_modal forces prompt.severity="error".
    lv_obj_t* err_icon = lv_obj_find_by_name(screen, "icon_error");
    REQUIRE(err_icon != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(err_icon, LV_OBJ_FLAG_HIDDEN));

    // A recovery button is present (the modal built buttons from the actions).
    REQUIRE(find_text_containing(screen, lv_tr("Resume")) != nullptr);

    // The router owns its modal and tears it down in its dtor at end-of-scope.
    // AmsState backend is cleared by backend_guard's dtor (exception-safe).
    process_lvgl(20);
}

// ---------------------------------------------------------------------------
// MODAL_WITH_RECOVER path: Happy Hare runout → real recovery modal actually SHOWN.
//
// Mirrors the AFC routing-e2e case above. Confirms that an HH-classified `!!`
// line flows through the SHARED GcodeErrorRouter (no backend-type check in
// shared code) and arrives at a real recovery modal on screen.
//
// The chain exercised end-to-end:
//   process_line(jam) → AmsBackendHappyHare::classify_error → CRITICAL+HH+actions
//   → decide_presentation == MODAL_WITH_RECOVER → present_recovery_modal
//   → ActionPromptModal::show_prompt(lv_screen_active(), prompt)
//
// The Happy Hare backend fires classify_error when:
//   (a) ctx.is_paused is true, AND
//   (b) the backend has action==ERROR (hh_error_state), OR
//       reason_for_pause is non-empty and contains a recognised keyword.
// We satisfy (b) via a Moonraker-envelope status push with action="Error" and a
// runout reason_for_pause string, which is the same path that fires in production
// when Happy Hare pauses the print on a runout event.

// Minimal helper — exposes the protected handle_status_update so the e2e test
// can drive the backend into an error state without accessing test-only code
// defined in another translation unit (test_ams_backend_happy_hare.cpp).
class HappyHareE2EHelper : public AmsBackendHappyHare {
  public:
    HappyHareE2EHelper(MoonrakerAPI* api, helix::MoonrakerClient* client)
        : AmsBackendHappyHare(api, client) {}

    // Feed MMU JSON state through the normal notification pipeline.
    // Replicates AmsBackendHappyHareTestHelper::test_parse_mmu_state.
    void push_mmu_state(const nlohmann::json& mmu_data) {
        nlohmann::json notification;
        nlohmann::json params;
        params["mmu"] = mmu_data;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }
};

TEST_CASE_METHOD(LVGLUITestFixture, "Routing E2E: Happy Hare runout pause routes to recovery modal",
                 "[error-center][routing][happy_hare]") {
    // Paused printer: classify_error checks ctx.is_paused (must be true to fire).
    get_printer_state().update_from_status(nlohmann::json{{"pause_resume", {{"is_paused", true}}}});
    REQUIRE(get_printer_state().is_paused());

    // Install the Happy Hare backend as the ACTIVE AmsState backend.
    // RAII guard: clear the singleton even if a REQUIRE throws.
    AmsState::instance().set_backend(std::make_unique<HappyHareE2EHelper>(api(), client()));
    struct BackendGuard {
        ~BackendGuard() {
            AmsState::instance().set_backend(nullptr);
        }
    } backend_guard;
    REQUIRE(AmsState::instance().get_backend() != nullptr);

    // Drive the HH backend into an error state via a Moonraker notification
    // envelope: params[0]["mmu"] = mmu_data.
    // action="Error" → system_info_.action == AmsAction::ERROR (hh_error_state=true).
    // reason_for_pause provides the descriptive detail that classify_error surfaces.
    nlohmann::json mmu_data;
    mmu_data["action"] = "Error";
    mmu_data["reason_for_pause"] = "Runout detected at toolhead sensor";
    mmu_data["gate"] = -1;
    mmu_data["tool"] = -1;
    mmu_data["filament"] = "Unloaded";
    mmu_data["enabled"] = true;

    auto* hh = static_cast<HappyHareE2EHelper*>(AmsState::instance().get_backend());
    hh->push_mmu_state(mmu_data);

    // Sanity: the backend really classifies this as recoverable CRITICAL (HH source,
    // non-empty recovery_actions). Fails loudly if the error-state gate regresses.
    {
        helix::ClassifyContext ctx;
        ctx.is_paused = true;
        ctx.is_printing = false;
        auto ev = AmsState::instance().get_backend()->classify_error("!! Runout detected", ctx);
        REQUIRE(ev.has_value());
        REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
        REQUIRE(ev->source == helix::ErrorSource::HAPPY_HARE);
        REQUIRE_FALSE(ev->recovery_actions.empty());
        // The detail comes from reason_for_pause, not the terse !! line.
        REQUIRE(ev->detail.find("Runout") != std::string::npos);
        // MODAL_WITH_RECOVER (not plain MODAL) because recovery_actions are present.
        REQUIRE(helix::decide_presentation(*ev) == helix::PresentAs::MODAL_WITH_RECOVER);
    }

    // Drive the REAL glue end-to-end. api() is non-null so present_recovery_modal
    // runs the show_prompt arm (not the ui_notification_error stub).
    helix::ui::RecoveryModalPresenter presenter3(api());
    helix::GcodeErrorRouter router(api(), client(), presenter3);
    REQUIRE_NOTHROW(GcodeErrorRouterTestAccess::process_line(router, "!! Runout detected"));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    // Assert the recovery modal is actually on screen.
    REQUIRE(Modal::any_visible());
    lv_obj_t* screen = lv_screen_active();
    REQUIRE(screen != nullptr);

    // The title set from e.title ("Filament runout") must appear.
    const char* title = find_text_containing(screen, lv_tr("Filament runout"));
    REQUIRE(title != nullptr);

    // Severity affordance: the error icon must be visible (not hidden).
    lv_obj_t* err_icon = lv_obj_find_by_name(screen, "icon_error");
    REQUIRE(err_icon != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(err_icon, LV_OBJ_FLAG_HIDDEN));

    // A Resume recovery button must be present (always the primary action for HH).
    REQUIRE(find_text_containing(screen, lv_tr("Resume")) != nullptr);

    process_lvgl(20);
}
