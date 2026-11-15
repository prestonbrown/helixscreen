// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/temp_unit_subject.h"

#include "core/static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

namespace helix::units {
namespace {

struct UnitSubjectState {
    lv_subject_t subject{};
    SubjectManager subjects;
    bool ready = false;
};

UnitSubjectState& state() {
    static UnitSubjectState s;
    return s;
}

void deinit_unit_subject() {
    auto& s = state();
    if (!s.ready) {
        return;
    }
    s.subjects.deinit_all();
    s.ready = false;
}

} // namespace

lv_subject_t* temp_unit_subject() {
    auto& s = state();
    if (s.ready) {
        return &s.subject;
    }
    if (!lv_is_initialized()) {
        return nullptr;
    }
    lv_subject_init_int(&s.subject, static_cast<int>(display_temp_unit()));
    s.subjects.register_subject(&s.subject, nullptr);
    s.ready = true;
    StaticSubjectRegistry::instance().register_deinit("TempUnitSubject", deinit_unit_subject,
                                                      StaticSubjectRegistry::Lifetime::Process);
    return &s.subject;
}

SubjectLifetime temp_unit_subject_lifetime() {
    return state().subjects.get_subjects_lifetime();
}

void publish_temp_unit() {
    if (lv_subject_t* subject = temp_unit_subject()) {
        lv_subject_set_int(subject, static_cast<int>(display_temp_unit()));
    }
}

} // namespace helix::units
