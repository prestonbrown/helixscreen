// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_result.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"
#include "subject_managed_panel.h"

#include <cstdio>
#include <fmt/format.h>

namespace helix::calibration {

std::string result_keep_line(const ResultChoices& choices) {
    if (!choices.offer_save) {
        return lv_tr("Nothing waits to be saved.");
    }
    if (choices.must_save) {
        return lv_tr("The heater's control changed in printer.cfg. It heats again only once this "
                     "result is saved.");
    }
    return choices.keep == KeepMeaning::InUse
               ? lv_tr("In use now. A restart forgets it unless you save it.")
               : lv_tr("Not in use yet. Klipper holds it until a save or a restart.");
}

std::string result_save_line(const ResultChoices& choices) {
    if (!choices.offer_save) {
        return {};
    }
    std::string line;
    if (choices.must_save) {
        line = choices.save_restarts ? lv_tr("Save to Config restarts Klipper.")
                                     : lv_tr("Save to Config keeps Klipper running.");
    } else {
        line = choices.save_restarts
                   ? lv_tr("Save to Config and Discard both restart Klipper.")
                   : lv_tr("Save to Config keeps Klipper running. Discard restarts it.");
    }
    if (!choices.also_staged.empty()) {
        std::string names;
        for (const auto& section : choices.also_staged) {
            names += names.empty() ? section : ", " + section;
        }
        line += " ";
        line += choices.offer_discard
                    ? fmt::format(fmt::runtime(lv_tr("Also waiting to be saved: {}. Save and "
                                                     "Discard include them.")),
                                  names)
                    : fmt::format(fmt::runtime(lv_tr("Also waiting to be saved: {}. Save "
                                                     "includes them.")),
                                  names);
    }
    return line;
}

bool refuse_result_restart() {
    if (lv_subject_get_int(
            get_printer_state().print_state().get_machine_motion_blocked_subject()) == 0) {
        return false;
    }
    NOTIFY_WARNING(lv_tr("Cannot restart Klipper while printing"));
    return true;
}

CalibrationResultView::CalibrationResultView(const char* prefix) : prefix_(prefix) {}

void CalibrationResultView::init_text(SubjectManager& manager, TextSubject& t, const char* field) {
    t.name = prefix_ + "_" + field;
    helix::subject_init_string(t.subject, t.buf.data(), nullptr, t.buf.size(), "");
    helix::publish_managed_subject(manager, &t.subject, t.name.c_str(), LV_SUBJECT_TYPE_STRING,
                                   __FILE__, __LINE__);
}

void CalibrationResultView::init_int(SubjectManager& manager, IntSubject& s, const char* field,
                                     int value) {
    s.name = prefix_ + "_" + field;
    helix::subject_init_int(s.subject, value);
    helix::publish_managed_subject(manager, &s.subject, s.name.c_str(), LV_SUBJECT_TYPE_INT,
                                   __FILE__, __LINE__);
}

void CalibrationResultView::init_subjects(SubjectManager& manager) {
    if (initialized_) {
        return;
    }
    init_text(manager, title_, "title");
    init_text(manager, summary_, "summary");
    init_text(manager, status_, "status");
    init_text(manager, status_sub_, "status_sub");
    init_int(manager, hide_keep_, "hide_keep", 0);
    init_int(manager, hide_save_, "hide_save", 0);
    init_int(manager, hide_discard_, "hide_discard", 0);
    init_int(manager, hide_before_, "hide_before", 0);
    for (int i = 0; i < kRows; ++i) {
        const std::string n = std::to_string(i);
        Row& r = rows_[static_cast<size_t>(i)];
        init_text(manager, r.label, ("label_" + n).c_str());
        init_text(manager, r.before, ("before_" + n).c_str());
        init_text(manager, r.after, ("after_" + n).c_str());
        init_int(manager, r.in_run, ("in_run_" + n).c_str(), 0);
        init_int(manager, r.keep, ("keep_" + n).c_str(), 1);
    }
    initialized_ = true;
}

void CalibrationResultView::set(TextSubject& t, const std::string& value) {
    lv_subject_set_string(&t.subject, value.c_str());
}

void CalibrationResultView::publish(const std::string& title, const std::string& summary,
                                    const std::vector<ResultRow>& rows,
                                    const ResultChoices& choices) {
    choices_ = choices;
    if (!initialized_) {
        return;
    }
    set(title_, title);
    set(summary_, summary);
    for (int i = 0; i < kRows; ++i) {
        Row& r = rows_[static_cast<size_t>(i)];
        const bool shown = static_cast<size_t>(i) < rows.size();
        set(r.label, shown ? rows[static_cast<size_t>(i)].label : "");
        set(r.before, shown ? rows[static_cast<size_t>(i)].before : "");
        set(r.after, shown ? rows[static_cast<size_t>(i)].after : "");
        lv_subject_set_int(&r.in_run.subject, shown ? 1 : 0);
    }
    bool any_before = false;
    for (const auto& row : rows) {
        any_before = any_before || !row.before.empty();
    }
    lv_subject_set_int(&hide_before_.subject, any_before ? 0 : 1);
    lv_subject_set_int(&hide_discard_.subject, choices.offer_discard ? 0 : 1);
    set(status_, result_keep_line(choices));
    set(status_sub_, result_save_line(choices));
    lv_subject_set_int(&hide_keep_.subject, choices.offer_keep ? 0 : 1);
    lv_subject_set_int(&hide_save_.subject, choices.offer_save ? 0 : 1);
}

} // namespace helix::calibration
