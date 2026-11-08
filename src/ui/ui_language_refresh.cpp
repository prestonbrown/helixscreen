// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_language_refresh.h"

#include "ui_ams_tool_text.h"

#include "ams_state.h"
#include "app_globals.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_subject_registry.h"
#include "system_settings_manager.h"
#include "tool_state.h"

namespace helix::ui {

namespace {

ObserverGuard s_language_observer;
ObserverGuard s_numbering_observer;
bool s_deinit_registered = false;

/// Re-format every label C++ built and holds on to.
void refresh_formatted_labels(ToolState* tools) {
    tools->refresh_display_labels();
    get_printer_state().refresh_translated_texts();
    // AmsState formats its status texts (clog meter, dryer, loaded
    // lane) and slot texts as it syncs from each backend.
    auto& ams = AmsState::instance();
    for (int i = 0; i < ams.backend_count(); ++i) {
        ams.sync_backend(i);
    }
}

} // namespace

void init_language_refresh() {
    // Replacing the guard drops any observer left on settings subjects that were
    // since torn down and rebuilt, so calling this again re-arms it.
    s_language_observer = observe_language_change(&ToolState::instance(), refresh_formatted_labels);
    auto& settings = SettingsManager::instance();
    lv_subject_t* numbering = settings.subject_number_lanes_from_zero();
    // Held by pointer so the non-mutable handler can update it.
    auto shown = std::make_shared<int>(lv_subject_get_int(numbering));
    s_numbering_observer = observe<int>(
        numbering, &ToolState::instance(),
        [shown](ToolState* tools, int value) {
            if (value == *shown) {
                return;
            }
            *shown = value;
            refresh_formatted_labels(tools);
            // Slot badges repaint on this version, and a renumber changes no slot data.
            AmsState::instance().bump_slots_version();
        },
        settings.get_subjects_lifetime());
    if (s_deinit_registered) {
        return;
    }
    s_deinit_registered = true;
    StaticSubjectRegistry::instance().register_deinit("LanguageRefresh", []() {
        s_language_observer.reset();
        s_numbering_observer.reset();
        s_deinit_registered = false;
    });
}

} // namespace helix::ui
