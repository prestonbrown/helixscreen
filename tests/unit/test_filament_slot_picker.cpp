// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_slot_picker.cpp
 * @brief The slot picker is filament_slot_picker.xml bound to the filament_picker_* subjects:
 *        its rows, their order and reasons, and what each tap answers.
 *
 * Run with: ./build/bin/helix-tests "[slot_picker]"
 */

#include "ui_filament_slot_picker.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AvailableSlot;
using helix::ui::FilamentSlotPicker;

namespace {

struct SlotPickerFixture : public LVGLUITestFixture {
    SlotPickerFixture() {
        trigger = lv_obj_create(lv_screen_active());
        lv_obj_set_size(trigger, 100, 40);
        lv_obj_set_pos(trigger, 10, 10);
    }
    ~SlotPickerFixture() override {
        picker.hide();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    static std::vector<AvailableSlot> slots(int n) {
        std::vector<AvailableSlot> out;
        for (int i = 0; i < n; ++i) {
            AvailableSlot s{i, 0, 0xFF0000u + static_cast<uint32_t>(i), "PLA", false, -1};
            s.local_slot_index = i;
            out.push_back(s);
        }
        return out;
    }

    void show(const std::vector<AvailableSlot>& s, const FilamentSlotPicker::Options& options,
              FilamentSlotPicker::Selection current = {}) {
        picker.show(
            lv_screen_active(), trigger, s, current,
            [this](const FilamentSlotPicker::Selection& sel) { chosen = sel; }, options);
    }

    static lv_obj_t* find(const std::string& name) {
        return lv_obj_find_by_name(lv_screen_active(), name.c_str());
    }

    static std::string text_of(lv_obj_t* parent, const char* name) {
        lv_obj_t* label = lv_obj_find_by_name(parent, name);
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    static bool hidden(lv_obj_t* obj) {
        return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    static void tap(lv_obj_t* obj) {
        REQUIRE(obj != nullptr);
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* trigger = nullptr;
    FilamentSlotPicker picker;
    std::optional<FilamentSlotPicker::Selection> chosen;
};

} // namespace

TEST_CASE_METHOD(SlotPickerFixture, "The slot picker shows one row per slot", "[slot_picker]") {
    show(slots(3), {});
    REQUIRE(picker.is_visible());
    for (int k = 0; k < 3; ++k) {
        lv_obj_t* row = find("filament_picker_row_" + std::to_string(k));
        REQUIRE(row != nullptr);
        CHECK(text_of(row, "slot_label") == "Slot " + std::to_string(k + 1));
        CHECK(text_of(row, "slot_material") == "PLA");
        CHECK(hidden(lv_obj_find_by_name(row, "slot_reason")));
    }
    CHECK(find("filament_picker_row_3") == nullptr);
    CHECK(hidden(find("auto_choice")));
    CHECK(hidden(find("extra_choice")));
    CHECK(hidden(find("cancel_choice")));
}

TEST_CASE_METHOD(SlotPickerFixture, "The slot picker lists rows in the order given, with reasons",
                 "[slot_picker]") {
    FilamentSlotPicker::Options options;
    options.order = {2, 0, 1};
    options.reasons = {"exact match", "color differs", "empty"};
    show(slots(3), options);

    const char* const labels[] = {"Slot 3", "Slot 1", "Slot 2"};
    for (int k = 0; k < 3; ++k) {
        lv_obj_t* row = find("filament_picker_row_" + std::to_string(k));
        REQUIRE(row != nullptr);
        CHECK(text_of(row, "slot_label") == labels[k]);
        CHECK(text_of(row, "slot_reason") == options.reasons[static_cast<std::size_t>(k)]);
        CHECK_FALSE(hidden(lv_obj_find_by_name(row, "slot_reason")));
    }
}

TEST_CASE_METHOD(SlotPickerFixture, "A tap on a slot row answers that row's slot",
                 "[slot_picker]") {
    FilamentSlotPicker::Options options;
    options.order = {2, 0, 1};
    options.reasons = {"a", "b", "c"};
    show(slots(3), options);

    tap(find("filament_picker_row_0"));
    REQUIRE(chosen.has_value());
    CHECK(chosen->slot_index == 2);
    CHECK(chosen->backend_index == 0);
    CHECK_FALSE(chosen->is_auto);
    CHECK_FALSE(chosen->is_extra);
    CHECK_FALSE(picker.is_visible());
}

TEST_CASE_METHOD(SlotPickerFixture, "The current slot is highlighted and Auto answers is_auto",
                 "[slot_picker]") {
    FilamentSlotPicker::Options options;
    options.offer_auto = true;
    show(slots(2), options, FilamentSlotPicker::Selection{1, 0, false});

    lv_obj_t* auto_row = find("auto_choice");
    REQUIRE(auto_row != nullptr);
    CHECK_FALSE(hidden(auto_row));
    CHECK(text_of(auto_row, "slot_label") == "Auto");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_picker_row_1_selected")) == 1);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_picker_row_0_selected")) == 0);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_picker_auto_selected")) == 0);

    tap(auto_row);
    REQUIRE(chosen.has_value());
    CHECK(chosen->is_auto);
    CHECK_FALSE(chosen->is_extra);
}

TEST_CASE_METHOD(SlotPickerFixture, "The extra row answers is_extra and Cancel answers nothing",
                 "[slot_picker]") {
    FilamentSlotPicker::Options options;
    options.extra_choice = "More options";
    options.cancel_choice = "Cancel";
    show(slots(2), options);

    SECTION("extra") {
        lv_obj_t* extra = find("extra_choice");
        REQUIRE(extra != nullptr);
        CHECK_FALSE(hidden(extra));
        CHECK(text_of(extra, "slot_label") == "More options");
        tap(extra);
        REQUIRE(chosen.has_value());
        CHECK(chosen->is_extra);
        CHECK(chosen->slot_index == -1);
    }
    SECTION("cancel") {
        tap(find("cancel_choice"));
        CHECK_FALSE(chosen.has_value());
        CHECK_FALSE(picker.is_visible());
    }
}

TEST_CASE_METHOD(SlotPickerFixture,
                 "More slots than the row cap writes no subject past it, and the card scrolls",
                 "[slot_picker]") {
    constexpr int kSlots = static_cast<int>(FilamentSlotPicker::kMaxPickerRows) + 8;
    show(slots(kSlots), {});

    const auto last = std::to_string(FilamentSlotPicker::kMaxPickerRows - 1);
    const auto past = std::to_string(FilamentSlotPicker::kMaxPickerRows);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_picker_row_count")) ==
          static_cast<int>(FilamentSlotPicker::kMaxPickerRows));
    CHECK(find("filament_picker_row_" + last) != nullptr);
    CHECK(find("filament_picker_row_" + past) == nullptr);
    CHECK(lv_xml_get_subject(nullptr, ("filament_picker_row_" + past + "_label").c_str()) ==
          nullptr);

    lv_obj_t* card = find("filament_picker_card");
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);
    CHECK(lv_obj_get_scroll_bottom(card) > 0);

    // The last listed row answers the last listed slot.
    tap(find("filament_picker_row_" + last));
    REQUIRE(chosen.has_value());
    CHECK(chosen->slot_index == static_cast<int>(FilamentSlotPicker::kMaxPickerRows) - 1);
}

TEST_CASE_METHOD(SlotPickerFixture, "Two single-unit backends list their rows apart",
                 "[slot_picker]") {
    std::vector<AvailableSlot> s = slots(1);
    AvailableSlot other = s[0];
    other.backend_index = 1;
    s.push_back(other);
    show(s, {});
    const auto label = [](int k) {
        const std::string name = "filament_picker_row_" + std::to_string(k) + "_label";
        return std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, name.c_str())));
    };
    CHECK(label(0) == "1 \xc2\xb7 Slot 1");
    CHECK(label(1) == "2 \xc2\xb7 Slot 1");
}

TEST_CASE_METHOD(SlotPickerFixture, "Auto as the current answer is ringed on screen",
                 "[slot_picker]") {
    FilamentSlotPicker::Options options;
    options.offer_auto = true;
    options.extra_choice = "More options";
    show(slots(2), options, FilamentSlotPicker::Selection{-1, -1, true});
    lv_obj_t* auto_row = find("auto_choice");
    lv_obj_t* extra = find("extra_choice");
    REQUIRE(auto_row != nullptr);
    REQUIRE(extra != nullptr);
    CHECK(lv_obj_get_style_bg_opa(auto_row, LV_PART_MAIN) == 40);
    CHECK(lv_obj_get_style_bg_opa(extra, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_bg_opa(find("filament_picker_row_0"), LV_PART_MAIN) == 0);
}
