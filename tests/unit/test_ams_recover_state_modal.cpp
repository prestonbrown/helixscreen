// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_recover_state_modal.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::AmsRecoverStateModal;

namespace {

AmsRecoverStateModal::Choices choices(bool has_bypass) {
    AmsRecoverStateModal::Choices c;
    c.slot_count = 4;
    c.has_bypass = has_bypass;
    return c;
}

/// Records what the modal asks for instead of acting on it.
class RecordingMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    AmsError recover_with_state(const helix::RecoverStateRequest& request) override {
        requests.push_back(request);
        return AmsErrorHelper::success();
    }
    std::vector<helix::RecoverStateRequest> requests;
};

RecordingMock* install(int slots) {
    auto mock = std::make_unique<RecordingMock>(slots);
    RecordingMock* raw = mock.get();
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_backend(std::move(mock));
    raw->start();
    return raw;
}

void click_primary() {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* btn = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
}

} // namespace

TEST_CASE("Recover state modal pre-fills the gate and leaves filament to detection",
          "[ams][recover_state]") {
    AmsSystemInfo info;
    info.current_slot = 3;
    info.filament_loaded = true;

    auto req = AmsRecoverStateModal::prefill(info, false);
    CHECK(req.slot == 3);
    CHECK_FALSE(req.bypass);
    // HH's Unknown positions read as not-loaded here, so the flag is never
    // echoed back as an assertion.
    CHECK_FALSE(req.loaded.has_value());

    info.current_slot = -2;
    info.filament_loaded = false;
    req = AmsRecoverStateModal::prefill(info, true);
    CHECK(req.slot == -1);
    CHECK(req.bypass);
    CHECK_FALSE(req.loaded.has_value());
}

TEST_CASE("Recover state modal selections map to 1-based rows after Keep current",
          "[ams][recover_state]") {
    helix::RecoverStateRequest req;
    req.slot = 3;
    req.loaded = false;
    auto s = AmsRecoverStateModal::selection_for(req, choices(true));
    CHECK(s.slot == 4);
    CHECK(s.loaded == 2);

    helix::RecoverStateRequest keep;
    s = AmsRecoverStateModal::selection_for(keep, choices(true));
    CHECK(s.slot == 0);
    CHECK(s.loaded == 0);

    helix::RecoverStateRequest bypass;
    bypass.bypass = true;
    CHECK(AmsRecoverStateModal::selection_for(bypass, choices(true)).slot == 5);
    // No bypass row to select: fall back to Keep current, not an out-of-range row.
    CHECK(AmsRecoverStateModal::selection_for(bypass, choices(false)).slot == 0);
}

TEST_CASE("Recover state modal round-trips every selection and never names a tool",
          "[ams][recover_state]") {
    const auto c = choices(true);
    for (uint32_t slot = 0; slot <= 5; ++slot) {
        for (uint32_t loaded = 0; loaded <= 2; ++loaded) {
            AmsRecoverStateModal::Selection in{slot, loaded};
            const auto req = AmsRecoverStateModal::request_for(in, c);
            INFO("slot=" << slot << " loaded=" << loaded);
            if (slot == 5) {
                CHECK(req.bypass);
                CHECK(req.slot == -1);
            } else {
                CHECK_FALSE(req.bypass);
                CHECK(req.slot == static_cast<int>(slot) - 1);
            }
            CHECK(req.loaded.has_value() == (loaded != 0));
            const auto out = AmsRecoverStateModal::selection_for(req, c);
            CHECK(out.slot == in.slot);
            CHECK(out.loaded == in.loaded);

            // HH's TOOL branch remaps the tool and rewrites the gate's status;
            // with LOADED=0 it wipes the gate's spool info.
            const std::string cmd = AmsBackendHappyHare::build_recover_command(req);
            CHECK(cmd.find("TOOL=") == std::string::npos);
        }
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recover state modal sends the pre-filled gate on confirm",
                 "[ams][recover_state]") {
    RecordingMock* mock = install(4);
    const int current = mock->get_system_info().current_slot;

    REQUIRE(AmsRecoverStateModal::show_owned());
    CHECK(ModalStack::instance().top_component_name() == "ams_recover_state_modal");
    click_primary();
    process_lvgl(20);

    REQUIRE(mock->requests.size() == 1);
    CHECK(mock->requests[0].slot == (current >= 0 ? current : -1));
    CHECK_FALSE(mock->requests[0].loaded.has_value());
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recover state modal sends nothing to a backend it never saw",
                 "[ams][recover_state]") {
    install(4);
    REQUIRE(AmsRecoverStateModal::show_owned());

    // The rows were built for a 4-gate system; a 6-gate one replaced it.
    RecordingMock* replacement = install(6);
    click_primary();
    process_lvgl(20);

    CHECK(replacement->requests.empty());
    CHECK(ModalStack::instance().top_component_name() != "ams_recover_state_modal");
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Recover state modal stays closed for a backend that cannot take it",
                 "[ams][recover_state]") {
    RecordingMock* mock = install(4);
    mock->set_afc_mode(true);
    REQUIRE_FALSE(mock->supports_recover_with_state());
    CHECK_FALSE(AmsRecoverStateModal::show_owned());
    CHECK(ModalStack::instance().top_component_name() != "ams_recover_state_modal");
    AmsState::instance().set_backend(nullptr);
}
