// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/static_subject_registry.h"
#include "core/subject.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace helix::ui {

/// The int subject a single-choice list's rows bind to by name. A picker_option_row or
/// choice_chip passes @p name as `selected_subject` and its own index as `selected_value`,
/// and is lit while the subject holds that index. One subject per kind of list, published
/// under @p name (a string literal) on first use and kept for the process: a picker shows
/// one list of its kind at a time, so every instance of its owner shares the subject and
/// sets it (set_choice) before building the rows.
inline lv_subject_t* choice_subject(const char* name) {
    struct Entry {
        const char* name;
        std::unique_ptr<Subject> subject;
        bool published;
    };
    struct Registry {
        SubjectManager subjects;
        std::vector<Entry> entries;
        bool hooked = false;
    };
    static Registry registry;

    Entry* entry = nullptr;
    for (auto& e : registry.entries) {
        if (std::strcmp(e.name, name) == 0) {
            entry = &e;
            break;
        }
    }
    if (!entry) {
        registry.entries.push_back({name, std::make_unique<Subject>(), false});
        entry = &registry.entries.back();
    }
    if (!entry->published) {
        entry->subject->init_int(-1);
        registry.subjects.publish(entry->name, entry->subject->get());
        entry->published = true;
    }
    if (!registry.hooked) {
        registry.hooked = true;
        StaticSubjectRegistry::instance().register_deinit(
            "ChoiceSubjects",
            []() {
                registry.subjects.deinit_all();
                for (auto& e : registry.entries) {
                    e.published = false;
                }
                registry.hooked = false;
            },
            StaticSubjectRegistry::Lifetime::Process);
    }
    return entry->subject->get();
}

/// Lights the row of the choice_subject(@p name) list whose index is @p index; -1 lights none.
inline void set_choice(const char* name, int index) {
    lv_subject_set_int(choice_subject(name), static_cast<int32_t>(index));
}

} // namespace helix::ui
