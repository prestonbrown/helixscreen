// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_error_helper.cpp
 * @brief AmsErrorHelper is the one place every AMS backend spells a gcode tool
 * number and a physical position for the user.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_types.h"
#include "display_numbering.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "translation_loader.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// LVGL has no pack-unregister API, so selecting a language nothing has loaded
// makes every subsequent lookup miss again — the same restore idiom
// test_ams_current_tool_text_i18n.cpp uses.
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

TEST_CASE("tool_out_of_range names the tool as a gcode T-number", "[ams][numbering]") {
    const auto err = AmsErrorHelper::tool_out_of_range(7);
    CHECK(err.result == AmsResult::INVALID_TOOL);
    CHECK(err.technical_msg == "Tool T7 out of range");
    CHECK(err.user_msg == "Invalid tool number");
    CHECK(err.suggestion == "Select a valid tool");
}

TEST_CASE("tool_out_of_range matches tool_label for every non-negative tool", "[ams][numbering]") {
    for (int tool : {0, 1, 63}) {
        const auto err = AmsErrorHelper::tool_out_of_range(tool);
        CHECK(err.technical_msg == "Tool " + helix::ui::tool_label(tool) + " out of range");
    }
}

TEST_CASE("tool_out_of_range still names a negative tool in the technical detail",
          "[ams][numbering]") {
    // tool_label(-1) is empty (helix::ui has no T<n> spelling for a negative
    // index), and a backend does call this helper with one: both do_change_tool
    // and set_tool_mapping_impl guard on "< 0 || >= max" and pass tool_number
    // straight through on either side of that OR. The technical detail is for
    // logs, so it must still carry the value that was rejected.
    const auto err = AmsErrorHelper::tool_out_of_range(-1);
    CHECK(err.technical_msg == "Tool -1 out of range");
}

// --- Positions. A toast names the backend's own word and the number printed on
// the machine; the technical detail keeps the storage index a log reader wants.

TEST_CASE("a position error names the backend's noun, 1-based", "[ams][numbering]") {
    // The position is a bare prefix before a colon: the translated frame that
    // follows cannot agree with a noun that varies per backend and locale.
    CHECK(AmsErrorHelper::slot_not_available(ui::LaneNoun::Gate, 2).user_msg ==
          "Gate 3: No filament");
    // invalid_slot names the KIND, not a position: no index exists to compose a
    // label from, and "Invalid Feeder 8 number" is worse than either.
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Feeder, 7, 3).user_msg ==
          "Feeder: Invalid number");
}

TEST_CASE("a position error keeps the raw index in the technical detail", "[ams][numbering]") {
    CHECK(AmsErrorHelper::slot_not_available(ui::LaneNoun::Gate, 2).technical_msg ==
          "Slot 2 has no filament loaded");
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Gate, 7, 3).technical_msg ==
          "Slot 7 out of range (0-3)");
}

TEST_CASE("the suggested span is 1-based", "[ams][numbering]") {
    // max_slot arrives 0-based at every call site (NUM_PORTS - 1,
    // total_slots - 1, CFS_MAX_SLOTS - 1), so a four-position backend passes 3
    // and the user must be told 1-4, not 0-3 and not 1-3. The noun is not in
    // the suggestion: the user_msg prefix already names it, and a frame holding
    // it would need its adjective to agree with it.
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Lane, 9, 3).suggestion ==
          "Select a valid number (1-4)");
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Slot, 99, 15).suggestion ==
          "Select a valid number (1-16)");
}

TEST_CASE("the suggested span survives a one-position backend", "[ams][numbering]") {
    // A single-position backend passes max_slot 0 - ams_backend_ace.cpp passes
    // the literal, and any slot_count() - 1 site does it with one position. The
    // span must still be THERE and read 1-1: a guard testing the raw 0-based
    // value sees 0, decides there is no range, and silently drops it. The
    // four-position case above passes either way, so this one is not redundant.
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Slot, 4, 0).suggestion ==
          "Select a valid number (1-1)");
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Gate, 4, 0).suggestion ==
          "Select a valid number (1-1)");
}

TEST_CASE("a backend with no positions offers no span", "[ams][numbering]") {
    // max_slot -1 is "nothing reported yet", the one case with no range to give.
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Lane, 0, -1).suggestion ==
          "Select a valid number");
}

TEST_CASE("a position error still names a sentinel index", "[ams][numbering]") {
    // lane_label() has no spelling for a negative index, and backends do pass
    // one: every guard here reads "< 0 || >= max" and hands the value through
    // on either side of that OR. The helpers that name a specific position must
    // still say which value was rejected.
    CHECK(AmsErrorHelper::slot_not_available(ui::LaneNoun::Gate, -1).user_msg ==
          "Gate -1: No filament");
}

TEST_CASE_METHOD(LVGLTestFixture, "a position error translates frame and prefix as one locale",
                 "[ams][numbering][i18n]") {
    ScopedLanguage restore_lang;

    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");

    // Guard the setup: with no pack loaded lv_tr() returns the English tag, and
    // the assertions below would pass vacuously against English.
    const std::string frame = lv_tr("No filament");
    REQUIRE(frame != "No filament");

    // The prefix ("Шлюз 3") and the frame must read as one locale — an English
    // tail after a translated prefix is the mixed-script failure mode.
    const std::string msg = AmsErrorHelper::slot_not_available(ui::LaneNoun::Gate, 2).user_msg;
    CHECK(msg == std::string(lv_tr("Gate")) + " 3: " + frame);
    CHECK(msg.find("empty") == std::string::npos);

    const std::string suggestion =
        AmsErrorHelper::slot_not_available(ui::LaneNoun::Gate, 2).suggestion;
    REQUIRE(suggestion != "Load filament first");
    CHECK(suggestion == std::string(lv_tr("Load filament first")));
}

TEST_CASE("Happy Hare reports an out-of-range gate as a gate", "[ams][numbering]") {
    // The noun reaches the toast from the backend rather than from a default.
    // A backend that never started has no gates, so any index is out of range.
    helix::AmsBackendHappyHare backend(nullptr, nullptr);
    REQUIRE(backend.lane_noun() == ui::LaneNoun::Gate);

    const auto err = backend.set_slot_info(2, helix::SlotInfo{}, /*persist=*/false);
    CHECK(err.result == AmsResult::INVALID_SLOT);
    CHECK(err.user_msg == "Gate: Invalid number");
}

TEST_CASE("the mock backend reports a bad index without deadlocking", "[ams][numbering]") {
    // AmsBackendMock is the one backend whose noun comes from state mutex_
    // protects, and every operation already holds mutex_ when it rejects an
    // index. mutex_ is not recursive, so this call returning at all is half the
    // assertion.
    AmsBackendMock mock(4);
    REQUIRE(mock.get_type() == AmsType::HAPPY_HARE); // the mock's default persona
    const auto err = mock.set_slot_info(9, helix::SlotInfo{}, /*persist=*/false);
    CHECK(err.result == AmsResult::INVALID_SLOT);
    CHECK(err.user_msg == "Gate: Invalid number");
}
