// SPDX-License-Identifier: GPL-3.0-or-later

// offer_edit_follow_up(): the question a backend asks after a slot edit
// commits, and the follow-up it runs only on yes.

#include "ui_ams_detail.h"
#include "ui_ams_edit_follow_up.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_mock.h"
#include "ams_edit_follow_up.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A mock backend that offers a fixed follow-up, or none, and counts runs.
class FollowUpBackend : public AmsBackendMock, public IEditFollowUp {
  public:
    FollowUpBackend() : AmsBackendMock(4) {}

    [[nodiscard]] void* capability(helix::TypeTag tag) override {
        if (tag == helix::type_tag<IEditFollowUp>()) {
            return static_cast<IEditFollowUp*>(this);
        }
        return AmsBackendMock::capability(tag);
    }

    std::optional<EditFollowUp> edit_follow_up(int, const SlotInfo&,
                                               const SlotInfo&) const override {
        if (!offer) {
            return std::nullopt;
        }
        return EditFollowUp{"Update model?", "Tool 2 now holds PETG", "Update"};
    }
    AmsError run_edit_follow_up(int slot_index, const SlotInfo&, const SlotInfo&) override {
        ++runs;
        last_slot = slot_index;
        return AmsErrorHelper::success();
    }

    bool offer = true;
    int runs = 0;
    int last_slot = -1;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "edit follow-up: offered, confirmed, run once",
                 "[edit_follow_up]") {
    test::RegisteredBackend<FollowUpBackend> harness;
    SlotInfo before;
    SlotInfo after;
    after.material = "PETG";

    ui::offer_edit_follow_up(1, before, after);
    process_lvgl(20);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK_THAT(test::top_modal_text(), Catch::Matchers::ContainsSubstring("Tool 2 now holds PETG"));
    CHECK(harness->runs == 0);

    test::answer_top_modal("btn_primary");
    process_lvgl(20);
    CHECK(harness->runs == 1);
    CHECK(harness->last_slot == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "edit follow-up: Not now runs nothing", "[edit_follow_up]") {
    test::RegisteredBackend<FollowUpBackend> harness;
    ui::offer_edit_follow_up(1, SlotInfo{}, SlotInfo{});
    process_lvgl(20);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    test::answer_top_modal("btn_secondary");
    process_lvgl(20);
    CHECK(harness->runs == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "edit follow-up: no offer shows nothing", "[edit_follow_up]") {
    test::RegisteredBackend<FollowUpBackend> harness;
    harness->offer = false;
    const lv_obj_t* before = ModalStack::instance().top_dialog();
    ui::offer_edit_follow_up(1, SlotInfo{}, SlotInfo{});
    process_lvgl(20);
    CHECK(ModalStack::instance().top_dialog() == before);
    CHECK(harness->runs == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "edit follow-up: a cleared slot asks the backend too",
                 "[edit_follow_up]") {
    test::RegisteredBackend<FollowUpBackend> harness;
    const lv_obj_t* before = ModalStack::instance().top_dialog();
    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 1, nullptr));
    process_lvgl(20);
    REQUIRE(ModalStack::instance().top_dialog() != before);
    CHECK_THAT(test::top_modal_text(), Catch::Matchers::ContainsSubstring("Tool 2 now holds PETG"));
    test::answer_top_modal("btn_secondary");
    CHECK(harness->runs == 0);
}
