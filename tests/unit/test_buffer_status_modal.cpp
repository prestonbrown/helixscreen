// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "buffer_status_modal.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Wrapper — accesses private members via friend declaration
// ============================================================================

class TestableBufferStatusModal : public BufferStatusModal {
  public:
    using BufferStatusModal::populate;

    // Read subject values for assertions (LVGL getters take non-const pointers)
    int type_value() {
        return lv_subject_get_int(&type_subject_);
    }
    int show_meter_value() {
        return lv_subject_get_int(&show_meter_subject_);
    }
    int show_espooler_value() {
        return lv_subject_get_int(&show_espooler_subject_);
    }
    int show_flow_value() {
        return lv_subject_get_int(&show_flow_subject_);
    }
    int show_distance_value() {
        return lv_subject_get_int(&show_distance_subject_);
    }

    const char* unsupported_value() {
        return lv_subject_get_string(&unsupported_subject_);
    }
    const char* description_value() {
        return lv_subject_get_string(&description_subject_);
    }
    const char* espooler_value() {
        return lv_subject_get_string(&espooler_value_subject_);
    }
    const char* gear_sync_value() {
        return lv_subject_get_string(&gear_sync_value_subject_);
    }
    const char* clog_value() {
        return lv_subject_get_string(&clog_value_subject_);
    }
    const char* flow_value() {
        return lv_subject_get_string(&flow_value_subject_);
    }
    const char* afc_state_value() {
        return lv_subject_get_string(&afc_state_subject_);
    }
    const char* afc_distance_value() {
        return lv_subject_get_string(&afc_distance_subject_);
    }
};

// ============================================================================
// Helpers
// ============================================================================

static AmsSystemInfo make_hh_info() {
    AmsSystemInfo info;
    info.type = AmsType::HAPPY_HARE;
    info.sync_feedback_bias = 0.15f;
    info.espooler_state = "rewind";
    info.sync_drive = true;
    info.clog_detection = 2;
    info.sync_feedback_flow_rate = 95.0f;
    info.encoder_flow_rate = -1;
    return info;
}

static AmsSystemInfo make_afc_info(int unit_count = 1) {
    AmsSystemInfo info;
    info.type = AmsType::AFC;
    for (int i = 0; i < unit_count; ++i) {
        AmsUnit unit;
        unit.unit_index = i;
        BufferHealth bh;
        bh.fault_detection_enabled = true;
        bh.distance_to_fault = 12.5f;
        bh.state = "Advancing";
        unit.buffer_health = bh;
        info.units.push_back(unit);
    }
    return info;
}

// ============================================================================
// Construction / metadata
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal default construction",
                 "[modals][buffer_status]") {
    BufferStatusModal modal;

    REQUIRE(std::string(modal.get_name()) == "Buffer Status");
    REQUIRE(std::string(modal.component_name()) == "buffer_status_modal");
    REQUIRE(modal.is_visible() == false);
    REQUIRE(modal.dialog() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal destructor safe when not shown",
                 "[modals][buffer_status]") {
    auto modal = std::make_unique<BufferStatusModal>();
    REQUIRE_NOTHROW(modal.reset());
}

// ============================================================================
// Happy Hare populate
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate HH with bias",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_hh_info();

    SECTION("positive bias shows loose description") {
        info.sync_feedback_bias = 0.15f;
        modal.populate(info, 0);

        REQUIRE(modal.type_value() == 1);
        REQUIRE(modal.show_meter_value() == 1);
        REQUIRE(std::string(modal.description_value()) == "Filament is loose");
    }

    SECTION("negative bias shows pulling tight") {
        info.sync_feedback_bias = -0.3f;
        modal.populate(info, 0);

        REQUIRE(std::string(modal.description_value()) == "Filament is pulling tight");
    }

    SECTION("near-zero bias shows balanced") {
        info.sync_feedback_bias = 0.01f;
        modal.populate(info, 0);

        REQUIRE(std::string(modal.description_value()) == "Filament tension is balanced");
    }

    SECTION("exactly -0.02 is still balanced (abs < 0.02)") {
        info.sync_feedback_bias = -0.019f;
        modal.populate(info, 0);

        REQUIRE(std::string(modal.description_value()) == "Filament tension is balanced");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate HH without bias",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_hh_info();
    info.sync_feedback_bias = -2.0f; // unavailable (default)

    modal.populate(info, 0);

    REQUIRE(modal.type_value() == 1);
    REQUIRE(modal.show_meter_value() == 0);
    REQUIRE(std::string(modal.description_value()) == "");
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate HH bias boundary at -1.5",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_hh_info();

    SECTION("exactly -1.5 is no-bias (> not >=)") {
        info.sync_feedback_bias = -1.5f;
        modal.populate(info, 0);
        REQUIRE(modal.show_meter_value() == 0);
    }

    SECTION("just above -1.5 has bias") {
        info.sync_feedback_bias = -1.49f;
        modal.populate(info, 0);
        REQUIRE(modal.show_meter_value() == 1);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate HH espooler states",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_hh_info();

    SECTION("rewind") {
        info.espooler_state = "rewind";
        modal.populate(info, 0);
        REQUIRE(modal.show_espooler_value() == 1);
        REQUIRE(std::string(modal.espooler_value()) == "Rewinding");
    }

    SECTION("assist") {
        info.espooler_state = "assist";
        modal.populate(info, 0);
        REQUIRE(std::string(modal.espooler_value()) == "Assisting");
    }

    SECTION("unknown state passes through raw") {
        info.espooler_state = "custom_state";
        modal.populate(info, 0);
        REQUIRE(std::string(modal.espooler_value()) == "custom_state");
    }

    SECTION("empty hides row") {
        info.espooler_state = "";
        modal.populate(info, 0);
        REQUIRE(modal.show_espooler_value() == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate HH gear sync",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_hh_info();

    SECTION("sync active") {
        info.sync_drive = true;
        modal.populate(info, 0);
        REQUIRE(std::string(modal.gear_sync_value()) == "Active");
    }

    SECTION("sync inactive") {
        info.sync_drive = false;
        modal.populate(info, 0);
        REQUIRE(std::string(modal.gear_sync_value()) == "Inactive");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate HH clog detection",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_hh_info();

    SECTION("auto") {
        info.clog_detection = 2;
        modal.populate(info, 0);
        REQUIRE(std::string(modal.clog_value()) == "Automatic");
    }

    SECTION("manual") {
        info.clog_detection = 1;
        modal.populate(info, 0);
        REQUIRE(std::string(modal.clog_value()) == "Manual");
    }

    SECTION("off") {
        info.clog_detection = 0;
        modal.populate(info, 0);
        REQUIRE(std::string(modal.clog_value()) == "Off");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate HH flow rate",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_hh_info();

    SECTION("sync feedback flow rate preferred") {
        info.sync_feedback_flow_rate = 95.0f;
        info.encoder_flow_rate = 80;
        modal.populate(info, 0);
        REQUIRE(modal.show_flow_value() == 1);
        REQUIRE(std::string(modal.flow_value()) == "95%");
    }

    SECTION("encoder flow rate as fallback") {
        info.sync_feedback_flow_rate = -1;
        info.encoder_flow_rate = 80;
        modal.populate(info, 0);
        REQUIRE(modal.show_flow_value() == 1);
        REQUIRE(std::string(modal.flow_value()) == "80%");
    }

    SECTION("no flow rate hides row") {
        info.sync_feedback_flow_rate = -1;
        info.encoder_flow_rate = -1;
        modal.populate(info, 0);
        REQUIRE(modal.show_flow_value() == 0);
    }
}

// ============================================================================
// AFC populate
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate AFC with buffer health",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_afc_info(1);

    modal.populate(info, 0);

    REQUIRE(modal.type_value() == 2);
    REQUIRE(modal.show_meter_value() == 0);
    REQUIRE(std::string(modal.afc_state_value()) == "Feeding filament forward");
    REQUIRE(modal.show_distance_value() == 1);
    REQUIRE(std::string(modal.clog_value()) == "Active");
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate AFC state translations",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_afc_info(1);

    SECTION("Advancing") {
        info.units[0].buffer_health->state = "Advancing";
        modal.populate(info, 0);
        REQUIRE(std::string(modal.afc_state_value()) == "Feeding filament forward");
    }

    SECTION("Trailing") {
        info.units[0].buffer_health->state = "Trailing";
        modal.populate(info, 0);
        REQUIRE(std::string(modal.afc_state_value()) == "Pulling filament back");
    }

    SECTION("unknown state shows raw with prefix") {
        info.units[0].buffer_health->state = "Idle";
        modal.populate(info, 0);
        REQUIRE(std::string(modal.afc_state_value()) == "State: Idle");
    }

    SECTION("empty state sets empty string") {
        info.units[0].buffer_health->state = "";
        modal.populate(info, 0);
        REQUIRE(std::string(modal.afc_state_value()) == "");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate AFC fault detection off",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_afc_info(1);
    info.units[0].buffer_health->fault_detection_enabled = false;

    modal.populate(info, 0);

    REQUIRE(modal.show_distance_value() == 0);
    REQUIRE(std::string(modal.clog_value()) == "Inactive");
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate AFC no buffer health",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_afc_info(1);
    info.units[0].buffer_health = std::nullopt;

    modal.populate(info, 0);

    REQUIRE(std::string(modal.afc_state_value()) == "No buffer data available");
    REQUIRE(modal.show_distance_value() == 0);
    REQUIRE(std::string(modal.clog_value()) == "Unknown");
}

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate AFC out-of-range unit",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    auto info = make_afc_info(1);

    SECTION("unit index too high") {
        modal.populate(info, 5);
        REQUIRE(std::string(modal.afc_state_value()) == "No buffer data available");
    }

    SECTION("negative unit index") {
        modal.populate(info, -1);
        REQUIRE(std::string(modal.afc_state_value()) == "No buffer data available");
    }
}

// ============================================================================
// Unknown AMS type
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal populate unknown type",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;
    AmsSystemInfo info;
    info.type = AmsType::NONE;

    modal.populate(info, 0);

    REQUIRE(modal.type_value() == 0);
    REQUIRE(modal.show_meter_value() == 0);
}

// ============================================================================
// Systems with no buffer hardware at all
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal explains itself on a system with no buffer",
                 "[modals][buffer_status]") {
    // Stock CFS (and AD5X IFS, tool changers, ACE, Snapmaker, QIDI) report no
    // buffer or flow data whatsoever. Every body section in the XML binds hidden
    // unless buf_type is 1 or 2, so without a message the dialog renders as a
    // title and a button row over an empty box - which is exactly what a K2 Plus
    // owner saw. The modal is reachable from the AMS panel as well as the home
    // tile, so it has to answer for itself rather than lean on the tile's gate.
    TestableBufferStatusModal modal;
    AmsSystemInfo info;
    info.type = AmsType::CFS;

    modal.populate(info, 0);

    CHECK(modal.type_value() == 0);
    CHECK(modal.show_meter_value() == 0);
    CHECK(std::string(modal.unsupported_value()).empty() == false);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "BufferStatusModal clears the unsupported notice for a real buffer",
                 "[modals][buffer_status]") {
    // The subjects are static and outlive any one modal, so a notice left from a
    // previous open would sit underneath a supported backend's body.
    TestableBufferStatusModal modal;

    AmsSystemInfo none_info;
    none_info.type = AmsType::CFS;
    modal.populate(none_info, 0);
    REQUIRE(std::string(modal.unsupported_value()).empty() == false);

    auto hh = make_hh_info();
    hh.sync_feedback_bias = 0.15f;
    modal.populate(hh, 0);

    CHECK(modal.type_value() == 1);
    CHECK(std::string(modal.unsupported_value()).empty());
}
