// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_current_tool_text_i18n.cpp
 * @brief AmsState's text subjects must hold their longest translated form
 *        whole, not just the English form they started as.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "data_root_resolver.h"
#include "display_numbering.h"
#include "printer_state.h"
#include "translation_loader.h"

#include <filesystem>
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

TEST_CASE_METHOD(LVGLTestFixture,
                 "ams_action_detail holds the longest translated composition whole",
                 "[ams][ams_state][i18n]") {
    ScopedLanguage restore_lang;

    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    // Read the actual worst composition off the shipped translation packs
    // rather than pinning one locale by hand: which pack is longest shifts as
    // translations are edited, so a hardcoded copy stops proving anything the
    // moment it drifts from the catalog. CFS's load-failure verdict
    // (AmsBackendCfs::phase_verdict_message) is the longest known producer.
    const std::string cfs_verdict_key =
        "The filament did not reach the nozzle. The CFS reported no error, but "
        "the toolhead sensor still sees no filament. Check that the spool is "
        "seated and the path is clear, then try again.";
    std::string worst_detail;
    for (const auto& entry :
         std::filesystem::directory_iterator(helix::asset_path("ui_xml/translations"))) {
        const std::string stem = entry.path().stem().string();
        if (entry.path().extension() != ".xml" || stem == "translations") {
            continue; // translations.xml is the merged catalog, not a per-locale pack
        }
        helix::ui::ensure_translation_loaded(stem);
        lv_translation_set_language(stem.c_str());
        const std::string detail(lv_tr(cfs_verdict_key.c_str()));
        if (detail.size() > worst_detail.size()) {
            worst_detail = detail;
        }
    }

    REQUIRE(worst_detail.size() > 128); // otherwise this test cannot distinguish old from new

    // A toolchange narration an earlier test left latched outranks the
    // operation detail in recompute_action_detail(), so clear it first
    // (the same reset idiom test_afc_console_corpus.cpp uses).
    ams.set_narration_phase(-1, "");
    ams.set_action_detail(worst_detail);

    const std::string round_tripped(lv_subject_get_string(ams.get_ams_action_detail_subject()));
    CHECK(round_tripped == worst_detail);

    // last_operation_detail_ persists in the singleton; clear it so later
    // tests derive the detail from their own state.
    ams.set_action_detail("");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ams_action_detail truncates an overrunning composition on a codepoint boundary",
                 "[ams][ams_state][i18n]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.set_narration_phase(-1, "");

    // A raw firmware message (AFC's message.message) is passed through with no
    // length bound at all, so a composition can still overrun the buffer no
    // matter how generously it is sized for the translated producers. Build
    // one out of a repeated 3-byte CJK codepoint so a byte-oriented cut always
    // has a chance to land mid-character.
    const std::string glyph = "\xE6\xB1\x9A"; // U+6C61, 3 bytes
    std::string oversized;
    for (int i = 0; i < 300; ++i) {
        oversized += glyph;
    }
    ams.set_action_detail(oversized);

    const std::string stored(lv_subject_get_string(ams.get_ams_action_detail_subject()));
    REQUIRE(stored.size() < oversized.size()); // otherwise this input doesn't overrun the buffer
    // Every glyph is 3 identical bytes, so a byte count that isn't a multiple
    // of 3 is the signature of a cut landing inside one of them.
    CHECK(stored.size() % glyph.size() == 0);

    ams.set_action_detail("");
}

TEST_CASE_METHOD(LVGLTestFixture, "clog_meter_mode_text holds the longest translated mode whole",
                 "[ams][ams_state][i18n]") {
    ScopedLanguage restore_lang;

    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");

    AmsSystemInfo info{};
    info.encoder_info.enabled = true;
    info.encoder_info.detection_mode = 1; // manual: "Clog Manual" -> ru below
    AmsStateTestAccess::sync_clog_meter(ams, info);

    const std::string mode(lv_subject_get_string(ams.get_clog_meter_mode_text_subject()));
    // ru "Засор: вручную" is 24 UTF-8 bytes; a 24-byte buffer truncates it
    // mid-codepoint, so the guard below is what lets this distinguish the
    // resized buffer from the original.
    REQUIRE(mode.size() >= 24);
    CHECK(mode == "Засор: вручную");

    // The clog meter subjects are the singleton's, read by later tests.
    AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
}

TEST_CASE_METHOD(LVGLTestFixture, "clog_meter endpoint labels hold СПУТЫВАНИЕ whole",
                 "[ams][ams_state][i18n]") {
    ScopedLanguage restore_lang;

    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");

    AmsSystemInfo info{};
    info.flowguard_info.enabled = true; // flowguard fills both endpoint labels
    AmsStateTestAccess::sync_clog_meter(ams, info);

    const std::string left(lv_subject_get_string(ams.get_clog_meter_label_left_subject()));
    REQUIRE(left.size() > 16); // otherwise this test cannot distinguish old from new
    CHECK(left == "СПУТЫВАНИЕ");

    AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
}
