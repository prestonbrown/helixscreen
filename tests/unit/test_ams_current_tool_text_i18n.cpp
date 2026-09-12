// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_current_tool_text_i18n.cpp
 * @brief The ams_current_tool_text subject must hold a translated position
 *        label whole, not just the English form it started as.
 */

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "translation_loader.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// LVGL has no pack-unregister API, so selecting a language nothing has loaded
// makes every subsequent lookup miss again — the same restore idiom
// test_translation_loader.cpp uses.
class ScopedLanguage {
  public:
    ScopedLanguage() = default;
    ~ScopedLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ams_current_tool_text survives a long translated label whole",
                 "[ams][ams_state][i18n]") {
    ScopedLanguage restore_lang;

    // PrinterState first: AmsState::init_subjects() binds an observer to its
    // print_state_enum subject, which must already exist.
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");

    // The Russian word for LaneNoun::Toolhead ("Печатающая головка") is the
    // longest noun any backend uses today, so a two-digit position after it is
    // the longest label the real production path
    // (src/ui/ui_ams_tool_text.cpp) can hand this subject.
    const std::string label = helix::ui::lane_label(helix::ui::LaneNoun::Toolhead, 15);
    REQUIRE(label.size() > 16); // otherwise this test cannot distinguish old from new
    CHECK(label == std::string(lv_tr("Toolhead")) + " 16");

    lv_subject_copy_string(ams.get_current_tool_text_subject(), label.c_str());

    // A shrunken buffer truncates mid-multibyte-character instead of failing
    // loudly, so byte-for-byte equality is the only check that catches it.
    const std::string round_tripped(lv_subject_get_string(ams.get_current_tool_text_subject()));
    CHECK(round_tripped == label);
}
