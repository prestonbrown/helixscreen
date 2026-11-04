// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_unit_disconnected.cpp
 * @brief A unit its backend reports offline reads disconnected on every
 *        surface that names it: the per-unit subject, the viewed-unit and
 *        whole-system subjects, and the unit card that binds them.
 */

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "theme_manager.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct UnitShape {
    bool connected;
    bool absent;
};

class ConnectionMock : public AmsBackendMock {
  public:
    ConnectionMock(std::vector<UnitShape> shapes, std::string type_name)
        : AmsBackendMock(static_cast<int>(shapes.size()) * 4), shapes_(std::move(shapes)),
          type_name_(std::move(type_name)) {}

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (!type_name_.empty()) {
            info.type_name = type_name_;
        }
        info.units.clear();
        for (size_t u = 0; u < shapes_.size(); ++u) {
            AmsUnit unit;
            unit.unit_index = static_cast<int>(u);
            // Unit 0 is named the way AFC names an OpenAMS unit, so its logo is color art.
            unit.name = u == 0 ? "OpenAMS AMS_1" : "conn_unit_" + std::to_string(u);
            unit.slot_count = 4;
            unit.first_slot_global_index = static_cast<int>(u) * 4;
            unit.connected = shapes_[u].connected;
            unit.absent = shapes_[u].absent;
            for (int s = 0; s < 4; ++s) {
                SlotInfo slot;
                slot.slot_index = s;
                slot.global_index = unit.first_slot_global_index + s;
                unit.slots.push_back(slot);
            }
            info.units.push_back(unit);
        }
        info.total_slots = static_cast<int>(shapes_.size()) * 4;
        return info;
    }

  private:
    std::vector<UnitShape> shapes_;
    std::string type_name_;
};

/// @p type_name replaces the system's type name when not empty.
void install(std::vector<UnitShape> shapes, std::string type_name = {}) {
    AmsState::instance().deinit_subjects();
    auto mock = std::make_unique<ConnectionMock>(std::move(shapes), std::move(type_name));
    REQUIRE(mock->start().success());
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(AmsState::instance().get_backend()->get_system_info().units.size() >= 2);
}

int subject_int(const std::string& name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name.c_str());
    REQUIRE(subj != nullptr);
    return lv_subject_get_int(subj);
}

} // namespace

TEST_CASE("AmsUnit reads connected until a backend says otherwise", "[ams][disconnected]") {
    CHECK(AmsUnit{}.connected);
}

TEST_CASE_METHOD(LVGLUITestFixture, "A disconnected unit publishes its own subject",
                 "[ams][disconnected]") {
    // Unit 2 is an absent placeholder: it has its own "Not connected" state and
    // must not also read disconnected.
    install({{true, false}, {false, false}, {false, true}});

    CHECK(subject_int(AmsState::unit_disconnected_subject_name(0)) == 0);
    CHECK(subject_int(AmsState::unit_disconnected_subject_name(1)) == 1);
    CHECK(subject_int(AmsState::unit_disconnected_subject_name(2)) == 0);
    CHECK(subject_int("ams_all_units_disconnected") == 0);

    SECTION("the viewed-unit subject follows the unit being viewed") {
        CHECK(subject_int("ams_viewed_unit_disconnected") == 0);
        AmsState::instance().set_viewed_unit(1);
        CHECK(subject_int("ams_viewed_unit_disconnected") == 1);
        AmsState::instance().set_viewed_unit(0);
        CHECK(subject_int("ams_viewed_unit_disconnected") == 0);
        AmsState::instance().set_viewed_unit(-1);
        CHECK(subject_int("ams_viewed_unit_disconnected") == 0);
    }

    SECTION(
        "the unit card fades its contents, stays tappable and shows its chip at full contrast") {
        const std::string absent = AmsState::unit_absent_subject_name(1);
        const std::string disc = AmsState::unit_disconnected_subject_name(1);
        const char* attrs[] = {"absent",     absent.c_str(), "disconnected",
                               disc.c_str(), nullptr,        nullptr};
        auto* card = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_unit_card", attrs));
        REQUIRE(card != nullptr);
        CHECK_FALSE(lv_obj_has_state(card, LV_STATE_DISABLED));
        CHECK(lv_obj_get_style_opa(card, LV_PART_MAIN) == LV_OPA_COVER);
        lv_obj_t* bars = lv_obj_find_by_name(card, "bars_container");
        REQUIRE(bars != nullptr);
        CHECK(lv_obj_get_style_opa(bars, LV_PART_MAIN) < LV_OPA_COVER);
        lv_obj_t* header = lv_obj_find_by_name(card, "unit_header");
        REQUIRE(header != nullptr);
        CHECK(lv_obj_get_style_opa(header, LV_PART_MAIN) < LV_OPA_COVER);
        lv_obj_t* chip = lv_obj_find_by_name(card, "unit_disconnected_chip");
        REQUIRE(chip != nullptr);
        CHECK_FALSE(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_style_opa(chip, LV_PART_MAIN) == LV_OPA_COVER);
        CHECK(lv_obj_get_style_opa(lv_obj_get_parent(chip), LV_PART_MAIN) == LV_OPA_COVER);
        lv_obj_t* count = lv_obj_find_by_name(card, "slot_count");
        REQUIRE(count != nullptr);
        CHECK(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));
        lv_obj_delete(card);
    }

    SECTION("a connected unit's card shows no chip") {
        const std::string absent = AmsState::unit_absent_subject_name(0);
        const std::string disc = AmsState::unit_disconnected_subject_name(0);
        const char* attrs[] = {"absent",     absent.c_str(), "disconnected",
                               disc.c_str(), nullptr,        nullptr};
        auto* card = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_unit_card", attrs));
        REQUIRE(card != nullptr);
        CHECK_FALSE(lv_obj_has_state(card, LV_STATE_DISABLED));
        CHECK(lv_obj_get_style_opa(card, LV_PART_MAIN) == LV_OPA_COVER);
        lv_obj_t* bars = lv_obj_find_by_name(card, "bars_container");
        REQUIRE(bars != nullptr);
        CHECK(lv_obj_get_style_opa(bars, LV_PART_MAIN) == LV_OPA_COVER);
        lv_obj_t* chip = lv_obj_find_by_name(card, "unit_disconnected_chip");
        REQUIRE(chip != nullptr);
        CHECK(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));
        lv_obj_delete(card);
    }

    AmsState::instance().set_viewed_unit(-1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Every present unit offline is the whole system offline",
                 "[ams][disconnected]") {
    install({{false, false}, {false, true}});
    CHECK(subject_int("ams_all_units_disconnected") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "A unit card tints a mono logo and draws a color logo as is",
                 "[ams][logo]") {
    install({{true, false}, {true, false}});
    const std::string absent = AmsState::unit_absent_subject_name(0);
    const std::string disc = AmsState::unit_disconnected_subject_name(0);
    const std::string logo_color = AmsState::unit_logo_color_subject_name(0);

    // The card follows its unit's logo subject: 0 is a mono mark, 1 is color art.
    for (int color : {0, 1}) {
        INFO("logo_color=" << color);
        lv_subject_set_int(lv_xml_get_subject(nullptr, logo_color.c_str()), color);
        const char* attrs[] = {"absent",     absent.c_str(),     "disconnected", disc.c_str(),
                               "logo_color", logo_color.c_str(), nullptr,        nullptr};
        auto* card = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_unit_card", attrs));
        REQUIRE(card != nullptr);
        lv_obj_t* logo = lv_obj_find_by_name(card, "unit_logo");
        REQUIRE(logo != nullptr);
        const auto opa = lv_obj_get_style_image_recolor_opa(logo, LV_PART_MAIN);
        if (color == 0) {
            CHECK(opa == LV_OPA_COVER);
            CHECK(lv_color_eq(lv_obj_get_style_image_recolor(logo, LV_PART_MAIN),
                              theme_manager_get_color("text")));
        } else {
            CHECK(opa == LV_OPA_TRANSP);
        }
        lv_obj_delete(card);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "The per-unit logo subject follows the unit's logo",
                 "[ams][logo]") {
    install({{true, false}, {true, false}});
    CHECK(subject_int(AmsState::unit_logo_color_subject_name(0)) == 1); // OpenAMS: color
    CHECK(subject_int(AmsState::unit_logo_color_subject_name(1)) == 0); // no color art
    // Past the cap the placeholder reads 0, a tinted mono mark.
    CHECK(subject_int(AmsState::unit_logo_color_subject_name(AmsState::kMaxUnits)) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "The system logo subject follows the system's logo art",
                 "[ams][logo]") {
    install({{true, false}, {true, false}}, "OpenAMS");
    CHECK(subject_int("ams_system_logo_color") == 1);
    CHECK(subject_int("ams_system_has_logo") == 1);

    install({{true, false}, {true, false}}, "AFC");
    CHECK(subject_int("ams_system_logo_color") == 0);
    CHECK(subject_int("ams_system_has_logo") == 1);
}
