// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/print_status_widget.h"

namespace helix {

// Friend access to PrintStatusWidget internals that production code reaches
// only through a live dashboard. The header forward-declares this class inside
// namespace helix, so the definition must live in the same namespace — and in
// ONE place: two test translation units each defining their own version of the
// class would be an ODR violation.
//
//  - idle_thumb_path_subject(): a private static subject.
//  - dispatch_load(): the idle runout dialog's "Load filament" action, which
//    otherwise needs a live modal, an attached widget, and a real runout sensor
//    reading to press.
//  - reset_to_idle(): the idle thumbnail resolve. Production reaches it from
//    attach(), a print-state change and a history change, all of which route
//    through lv_async_call or UpdateQueue — and pumping either of those also
//    drains the queue, which destroys the ordering a staleness test has to
//    control (park one load's result, supersede it, then let it land).
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API.
class PrintStatusWidgetTestAccess {
  public:
    static lv_subject_t* idle_thumb_path_subject() {
        return &PrintStatusWidget::idle_thumb_path_subject_;
    }

    static bool nozzle_picker_available(const PrintStatusWidget& widget) {
        return widget.nozzle_picker_available();
    }

    // The nozzle fit over a detailed card built straight from its XML, which a
    // live dashboard would otherwise have to host.
    static void fit_nozzle_picker(PrintStatusWidget& widget, lv_obj_t* root) {
        widget.widget_obj_ = root;
        widget.fit_nozzle_picker();
        widget.widget_obj_ = nullptr;
    }

    static void fit_row(PrintStatusWidget& widget, lv_obj_t* root, const char* row_name,
                        lv_subject_t* fits) {
        widget.widget_obj_ = root;
        widget.fit_row(row_name, fits);
        widget.widget_obj_ = nullptr;
    }

    static void fit_speed_row(PrintStatusWidget& widget, lv_obj_t* root) {
        widget.widget_obj_ = root;
        widget.fit_speed_row();
        widget.widget_obj_ = nullptr;
    }

    static void dispatch_load(PrintStatusWidget& widget) {
        widget.dispatch_load();
    }

    static void reset_to_idle(PrintStatusWidget& widget) {
        widget.reset_print_card_to_idle();
    }

    // Bumped once by every idle thumbnail resolve that gets past its guards,
    // so it counts resolves without a fetch having to be observable.
    static uint32_t idle_thumb_generation(const PrintStatusWidget& widget) {
        return widget.idle_thumb_generation_.load();
    }

    // The idle runout guard chain. Production runs it from a subject observer on
    // an attached widget with a live sensor reading; a test that cares about the
    // ORDER of its gates needs to call it straight.
    static void check_idle_runout(PrintStatusWidget& widget) {
        widget.check_and_show_idle_runout_modal();
    }

    static bool runout_modal_shown(const PrintStatusWidget& widget) {
        return widget.runout_modal_shown_;
    }

    static bool runout_modal_visible(const PrintStatusWidget& widget) {
        return widget.runout_modal_.is_visible();
    }

    // "The guidance dialog is already up." Production gets there by actually
    // showing the modal, which needs a registered XML component tree.
    static void set_runout_modal_shown(PrintStatusWidget& widget, bool shown) {
        widget.runout_modal_shown_ = shown;
    }

    // The idle thumbnail key and the freshness stamp it is validated against.
    // Production resolves both inside reset_print_card_to_idle(), where the
    // answer is only observable after a cache probe or a pool round-trip; a
    // test that only wants to know WHICH history job was selected reads them
    // straight from the two resolvers.
    static std::string last_print_thumbnail_path(const PrintStatusWidget& widget) {
        return widget.get_last_print_thumbnail_path();
    }

    static time_t last_print_source_modified(const PrintStatusWidget& widget) {
        return widget.get_last_print_source_modified();
    }

    // The cached tree: the root and the children every deferred path writes to.
    static lv_obj_t* widget_obj(const PrintStatusWidget& widget) {
        return widget.widget_obj_;
    }

    static lv_obj_t* thumb(const PrintStatusWidget& widget) {
        return widget.print_card_thumb_;
    }

    static lv_obj_t* active_thumb(const PrintStatusWidget& widget) {
        return widget.print_card_active_thumb_;
    }

    static bool idle_reset_pending(const PrintStatusWidget& widget) {
        return widget.idle_reset_timer_.pending();
    }

    static bool active_apply_pending(const PrintStatusWidget& widget) {
        return widget.active_apply_timer_.pending();
    }

    static lv_obj_t* library_row_last(const PrintStatusWidget& widget) {
        return widget.library_row_last_;
    }
};

} // namespace helix
