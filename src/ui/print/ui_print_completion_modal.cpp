// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_completion_modal.h"

#include "ui_confetti.h"
#include "ui_modal.h"
#include "ui_widget_ref.h"

#include "app_globals.h"
#include "core/subject.h"
#include "display_format.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "notice.h"
#include "print_cost_service.h"
#include "printer_state.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <lvgl.h>

namespace helix {

// --- Subjects for declarative XML bindings in print_completion_modal.xml ---
static helix::WidgetSubjects s_subjects{"PrintCompletion"};

// String subjects with backing buffers
static char s_title_buf[64];
static helix::Subject s_title_subject;

static char s_filename_buf[256];
static helix::Subject s_filename_subject;

static char s_duration_buf[64];
static helix::Subject s_duration_subject;

static char s_estimate_buf[64];
static helix::Subject s_estimate_subject;

static char s_layers_buf[64];
static helix::Subject s_layers_subject;

static char s_filament_buf[64];
static helix::Subject s_filament_subject;

static char s_cost_buf[48];
static helix::Subject s_cost_subject;

// Int subjects for visibility flags (1=visible, 0=hidden)
static helix::Subject s_has_estimate_subject;
static helix::Subject s_has_filament_subject;
static helix::Subject s_has_cost_subject;

void init_print_completion_subjects() {
    s_subjects.ensure_init([] {
        s_title_subject.init_string(s_title_buf, nullptr, sizeof(s_title_buf), "");
        s_filename_subject.init_string(s_filename_buf, nullptr, sizeof(s_filename_buf), "");
        s_duration_subject.init_string(s_duration_buf, nullptr, sizeof(s_duration_buf), "");
        s_estimate_subject.init_string(s_estimate_buf, nullptr, sizeof(s_estimate_buf), "");
        s_layers_subject.init_string(s_layers_buf, nullptr, sizeof(s_layers_buf), "");
        s_filament_subject.init_string(s_filament_buf, nullptr, sizeof(s_filament_buf), "");
        s_has_estimate_subject.init_int(1);
        s_has_filament_subject.init_int(1);
        s_cost_subject.init_string(s_cost_buf, nullptr, sizeof(s_cost_buf), "");
        s_has_cost_subject.init_int(0);

        s_subjects.publish("print_completion_title", &s_title_subject);
        s_subjects.publish("print_completion_filename", &s_filename_subject);
        s_subjects.publish("print_completion_duration", &s_duration_subject);
        s_subjects.publish("print_completion_estimate", &s_estimate_subject);
        s_subjects.publish("print_completion_layers", &s_layers_subject);
        s_subjects.publish("print_completion_filament", &s_filament_subject);
        s_subjects.publish("print_completion_has_estimate", &s_has_estimate_subject);
        s_subjects.publish("print_completion_has_filament", &s_has_filament_subject);
        s_subjects.publish("print_completion_cost", &s_cost_subject);
        s_subjects.publish("print_completion_has_cost", &s_has_cost_subject);
    });
}

CompletionStats build_completion_stats(int duration_secs, int estimated_secs, int total_layers,
                                       int filament_mm) {
    CompletionStats out;
    out.duration = format::duration_padded(duration_secs) + " " + lv_tr("elapsed");
    if (estimated_secs > 0) {
        out.estimate = std::string("est") + " " + format::duration_padded(estimated_secs);
    }
    out.layers = helix::display_format::format_layer_count(
        total_layers > 0 ? static_cast<uint32_t>(total_layers) : 0U);
    if (filament_mm > 0) {
        out.filament =
            format::format_filament_length(static_cast<double>(filament_mm)) + " " + lv_tr("used");
    }
    return out;
}

// The completion modal currently on screen, if any. Only one can be up: all
// three outcomes share print_completion_modal and it is shown from one place,
// so a single handle is enough where the fault registry needs a vector. A
// handle, not a pointer: it reads nullptr once the dialog dies by any route
// (the user's OK, a backdrop tap, ESC, our own dismissal, teardown), and a
// freed address can be reused by a later allocation, so a stale pointer could
// name an unrelated live widget and we would "dismiss" that instead.
static helix::ui::WidgetRef s_completion_modal;

// A completion modal is a statement about the job that just ended. Starting
// another job makes that statement false, so the dialog has outlived its
// condition and belongs off the screen — the user should not have to dismiss
// the last print's result to watch the next one begin.
static void dismiss_completion_modal_for_new_print() {
    lv_obj_t* dialog = s_completion_modal;
    if (!dialog) {
        return;
    }
    auto& stack = ::ModalStack::instance();
    lv_obj_t* backdrop = stack.backdrop_for(dialog);
    // No backdrop = already acknowledged and gone. Exiting = acknowledged a
    // frame ago and still animating out. Neither needs dismissing again.
    if (!backdrop || stack.is_exiting(backdrop)) {
        return;
    }
    // External, not Programmatic: this is not the dialog's caller closing it.
    ::Modal::hide(dialog, ::ModalCloseReason::External);
    spdlog::info("[PrintComplete] New print started - dismissed stale completion modal");
}

static void show_rich_completion_modal(PrintJobState state, const char* filename) {
    init_print_completion_subjects();

    auto& printer_state = get_printer_state();

    // Get print stats (wall-clock elapsed including prep time)
    int duration_secs = lv_subject_get_int(printer_state.print_state().get_print_elapsed_subject());
    int total_layers =
        lv_subject_get_int(printer_state.print_state().get_print_layer_total_subject());
    int estimated_secs = printer_state.print_state().get_estimated_print_time();
    int filament_mm =
        lv_subject_get_int(printer_state.print_state().get_print_filament_used_subject());

    spdlog::info("[PrintComplete] Stats: duration={}s, estimated={}s, layers={}, filament={}mm",
                 duration_secs, estimated_secs, total_layers, filament_mm);

    // Determine icon and title based on state
    const char* icon_src = "check_circle";
    const char* icon_variant = "success";
    const char* title = "Print Complete";

    switch (state) {
    case PrintJobState::COMPLETE:
        icon_src = "check_circle";
        icon_variant = "success";
        title = "Print Complete";
        break;
    case PrintJobState::CANCELLED:
        icon_src = "cancel";
        icon_variant = "warning";
        title = "Print Canceled";
        break;
    case PrintJobState::ERROR:
        icon_src = "alert";
        icon_variant = "error";
        title = "Print Failed";
        break;
    default:
        break;
    }

    // Set all subjects BEFORE showing modal so XML bindings resolve immediately
    lv_subject_set_string(&s_title_subject, lv_tr(title));
    lv_subject_set_string(&s_filename_subject, filename);

    const CompletionStats stats =
        build_completion_stats(duration_secs, estimated_secs, total_layers, filament_mm);

    lv_subject_set_string(&s_duration_subject, stats.duration.c_str());
    lv_subject_set_string(&s_layers_subject, stats.layers.c_str());

    if (!stats.estimate.empty()) {
        lv_subject_set_string(&s_estimate_subject, stats.estimate.c_str());
    }
    lv_subject_set_int(&s_has_estimate_subject, stats.estimate.empty() ? 0 : 1);

    if (!stats.filament.empty()) {
        lv_subject_set_string(&s_filament_subject, stats.filament.c_str());
    }
    lv_subject_set_int(&s_has_filament_subject, stats.filament.empty() ? 0 : 1);

    // The actual cost can arrive after the dialog opens: a shared record's
    // writer adds it when its own end-of-print handling runs.
    lv_subject_set_int(&s_has_cost_subject, 0);
    const int progress_pct =
        lv_subject_get_int(printer_state.print_state().get_print_progress_display_subject());
    print_cost::ActivePrintCost::instance().actual(
        duration_secs, filament_mm, progress_pct / 100.0, [](const std::string& text) {
            if (!s_cost_subject.is_live()) {
                return;
            }
            std::snprintf(s_cost_buf, sizeof(s_cost_buf), "%s", text.c_str());
            lv_subject_set_string(&s_cost_subject, s_cost_buf);
            lv_subject_set_int(&s_has_cost_subject, text.empty() ? 0 : 1);
        });

    // Show modal - XML bind_text and bind_flag_if_eq resolve from subjects above
    const char* attrs[] = {"icon", icon_src, "icon_variant", icon_variant, nullptr};
    lv_obj_t* dialog = helix::ui::ComponentModal::show_owned("print_completion_modal", attrs);

    if (!dialog) {
        spdlog::error("[PrintComplete] Failed to create print_completion_modal");
        return;
    }

    s_completion_modal = dialog;

    // Celebrate successful prints with confetti (respects animations setting)
    if (state == PrintJobState::COMPLETE &&
        DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_t* confetti = ui_confetti_create(lv_layer_top());
        if (confetti) {
            ui_confetti_burst(confetti, 100);
            spdlog::debug("[PrintComplete] Confetti burst for successful print");
        }
    }

    spdlog::info("[PrintComplete] Showing rich completion modal: {} ({})", title, filename);
}

namespace {

const UiRequestRegistration s_show_request{
    UiRequestKind::ShowPrintCompletion, [](const UiRequest& request) {
        show_rich_completion_modal(static_cast<PrintJobState>(request.code), request.text.c_str());
    }};
const UiRequestRegistration s_dismiss_request{
    UiRequestKind::DismissPrintCompletion,
    [](const UiRequest&) { dismiss_completion_modal_for_new_print(); }};

} // namespace

} // namespace helix
