// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_status_modal_live.cpp
 * @brief The Buffer Status modal follows the backend while it is open.
 */

#include "ui_buffer_slider.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "buffer_status_modal.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string subject_text(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

void set_pressure(AmsBackendMock& mock, float pressure) {
    BufferHealth fps;
    fps.fps_value = fps.smoothed_fps = pressure;
    fps.fps_set_point = 0.5f;
    fps.fps_reported = true;
    mock.set_unit_buffer_health(0, fps);
}

/// What a backend event does: sync, then announce it.
void land_backend_update() {
    AmsState::instance().sync_from_backend();
    AmsState::instance().bump_data_revision();
    ui::UpdateQueue::instance().drain();
}

} // namespace

/// Reaches the open modal's slider.
class BufferStatusModalProbe : public BufferStatusModal {
  public:
    const ui::UiBufferSlider* slider() const {
        return slider_.get();
    }
};

TEST_CASE_METHOD(LVGLUITestFixture, "Buffer Status modal follows the backend while open",
                 "[modals][buffer_status][live]") {
    AmsState::instance().init_subjects(true);
    test::RegisteredBackend<AmsBackendMock> mock(4);
    // Neither Happy Hare nor AFC: the modal's own filament-pressure view.
    mock->set_tool_changer_mode(true);
    set_pressure(*mock, 0.32f);
    land_backend_update();

    BufferStatusModal modal;
    REQUIRE(modal.show(test_screen()));
    ui::UpdateQueue::instance().drain();

    REQUIRE(subject_int("buf_type") == 3);
    CHECK(subject_int("buf_status") == static_cast<int>(ui::ClogMeterStatus::Warning));
    CHECK(subject_text("buf_value") == "FPS 32%");
    CHECK(subject_text("buf_target") == "target 50%");
    CHECK(subject_text("buf_description") == "Running tight");

    SECTION("a new reading lands in the open modal") {
        set_pressure(*mock, 0.71f);
        land_backend_update();
        CHECK(subject_text("buf_value") == "FPS 71%");
        CHECK(subject_text("buf_description") == "Running loose");
    }

    SECTION("the trace has its own panel, clear of the slider") {
        lv_obj_update_layout(modal.dialog());
        lv_area_t slider;
        lv_area_t trace;
        lv_obj_get_coords(lv_obj_find_by_name(modal.dialog(), "buf_slider"), &slider);
        lv_obj_get_coords(lv_obj_find_by_name(modal.dialog(), "buf_trace"), &trace);
        CHECK(trace.x1 > slider.x2 + 1);
    }

    SECTION("the X is the only way out") {
        CHECK(lv_obj_find_by_name(modal.dialog(), "btn_close") != nullptr);
        CHECK(lv_obj_find_by_name(modal.dialog(), "btn_primary") == nullptr);
        CHECK(lv_obj_find_by_name(modal.dialog(), "btn_secondary") == nullptr);
    }

    SECTION("the backend vanishing falls back to the unsupported message") {
        AmsState::instance().clear_backends();
        ui::UpdateQueue::instance().drain();
        CHECK(subject_int("buf_type") == 0);
        CHECK_FALSE(subject_text("buf_unsupported").empty());
    }

    modal.hide();
    ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Buffer Status modal trace keeps scrolling while open",
                 "[modals][buffer_status][live]") {
    AmsState::instance().init_subjects(true);
    test::RegisteredBackend<AmsBackendMock> mock(4);
    mock->set_tool_changer_mode(true);
    set_pressure(*mock, 0.32f);
    land_backend_update();

    BufferStatusModalProbe modal;
    REQUIRE(modal.show(test_screen()));
    ui::UpdateQueue::instance().drain();
    REQUIRE(modal.slider() != nullptr);
    CHECK(modal.slider()->bias() == Catch::Approx(-0.36f));

    // The harness only runs timers with a finite repeat count.
    lv_timer_set_repeat_count(modal.slider()->timer_for_test(), 100);
    process_lvgl(2100);
    CHECK(modal.slider()->trace_ticks() == 2);

    modal.hide();
    ui::UpdateQueue::instance().drain();
}
