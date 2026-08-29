// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_preflight_check_modal.h"
#include "ui_update_queue.h"

#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "preflight_validator.h"

#include <memory>

// LVGLUITestFixture registers ALL XML components (via
// helix::register_xml_components()), including preflight_check_modal.xml and
// preflight_check_tool_row.xml, so the modal builds its real widget tree here.
#include "../lvgl_ui_test_fixture.h"

#include "../catch_amalgamated.hpp"

// Regression guard for two stacked bugs that shipped the enriched pre-flight
// check modal with its per-tool rows completely invisible (born broken in
// dcd73ef24, fixed in 40f0f0583):
//
//   Bug 1: build_rows() created "preflight_tool_row" (the <view name>), but
//          lv_xml resolves components by FILENAME — the component is registered
//          as "preflight_check_tool_row". Every row create failed silently, so
//          the list had zero children.
//   Bug 2: preflight_tool_list used flex_grow="1" inside a height="content"
//          dialog. A flex-grow child has no free space to claim in a
//          content-sized parent, so the list collapsed to zero height — the
//          rows were invisible even once they created.
//
// The modal self-deletes via on_hide() (heap-allocated in production), so these
// tests heap-allocate and drain the async self-delete with process_lvgl().

namespace {

helix::ToolCheck make_check(int tool, uint32_t color, const char* material, int slot, bool present,
                            helix::ToolCheck::Severity sev) {
    helix::ToolCheck c;
    c.tool_index = tool;
    c.intended_color = color;
    c.intended_material = material;
    c.mapped_slot = slot;
    c.slot_present = present;
    c.color_ok = sev != helix::ToolCheck::Severity::ColorMismatch;
    c.material_ok = sev != helix::ToolCheck::Severity::MaterialMismatch;
    c.severity = sev;
    return c;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "PreflightCheckModal renders one visible row per tool",
                 "[preflight][modal][ui]") {
    helix::PreflightResult pf;
    pf.checks = {
        make_check(0, 0x2E8B57, "PLA", 0, true, helix::ToolCheck::Severity::Ok),
        make_check(1, 0xE23B3B, "PLA", 1, true, helix::ToolCheck::Severity::ColorMismatch),
        make_check(2, 0xF5A623, "PETG", -1, false, helix::ToolCheck::Severity::EmptySlot),
    };

    auto* modal = new helix::ui::PreflightCheckModal();
    modal->set_checks(pf);
    REQUIRE(modal->show(test_screen()));
    REQUIRE(modal->is_visible());

    // Run the event loop, then force a synchronous layout pass so geometry
    // resolves deterministically before we read heights.
    process_lvgl(50);
    lv_obj_update_layout(test_screen());

    lv_obj_t* list = lv_obj_find_by_name(test_screen(), "preflight_tool_list");
    REQUIRE(list != nullptr);

    // Bug 1: every tool check must produce a row. Zero rows means the row
    // component failed to resolve — the create-name/filename mismatch regressed.
    REQUIRE(lv_obj_get_child_count(list) == pf.checks.size());

    // Bug 2: the list must actually occupy vertical space. A flex_grow child in a
    // content-sized parent collapses to zero height, hiding the rows even once
    // they create. Assert the list is at least as tall as its rows stack up to.
    lv_coord_t list_h = lv_obj_get_height(list);
    lv_coord_t rows_h = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++) {
        rows_h += lv_obj_get_height(lv_obj_get_child(list, i));
    }
    INFO("list_h=" << list_h << " rows_h=" << rows_h);
    REQUIRE(list_h >= rows_h);
    REQUIRE(list_h > 0);

    modal->hide(); // on_hide() -> async self-delete
    process_lvgl(50);
}

// The Remap affordance is offered only when the backend can actually carry out
// the pick — the declared route AND its readiness. Asking the route alone
// offered the button on an AD5X before `_IFS_VARS` discovery, where
// set_tool_mapping() writes local state the firmware replays nothing from, so
// the user's pick was accepted and silently dropped at print start.
//
// Driven through the mock's readiness knob, which is the only way to reach that
// shape without a real AD5X: HELIX_MOCK_REMAP_READY=0 does the same thing in a
// --test run.
namespace {

/// Installs a mock backend into AmsState for the life of the case.
struct ScopedAmsBackend {
    AmsBackendMock* backend = nullptr;

    explicit ScopedAmsBackend(int slot_count) {
        auto& ams = AmsState::instance();
        ams.init_subjects(false);
        auto owned = std::make_unique<AmsBackendMock>(slot_count);
        backend = owned.get();
        backend->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        backend->start();
    }
    ~ScopedAmsBackend() {
        helix::ui::UpdateQueue::instance().drain();
        if (backend) {
            backend->stop();
        }
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }
};

/// Show the modal and report whether the Remap button is offered.
bool remap_button_offered(lv_obj_t* screen, void (*process)(lv_obj_t*)) {
    helix::PreflightResult pf;
    pf.checks = {make_check(0, 0x2E8B57, "PLA", 0, true, helix::ToolCheck::Severity::Ok)};

    auto* modal = new helix::ui::PreflightCheckModal();
    modal->set_checks(pf);
    REQUIRE(modal->show(screen));
    process(screen);

    lv_obj_t* btn = lv_obj_find_by_name(screen, "btn_tertiary");
    REQUIRE(btn != nullptr);
    const bool offered = !lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN);

    modal->hide();
    process(screen);
    return offered;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PreflightCheckModal offers Remap only when the route is usable",
                 "[preflight][modal][ui][remap]") {
    static LVGLUITestFixture* self = nullptr;
    self = this;
    auto pump = [](lv_obj_t* s) {
        self->process_lvgl(50);
        lv_obj_update_layout(s);
    };

    SECTION("a ready backend that declares a route is offered the button") {
        ScopedAmsBackend ams(4);
        REQUIRE(helix::printer::can_remap(*ams.backend));
        CHECK(remap_button_offered(test_screen(), pump));
    }

    SECTION("a backend with no route at all is not") {
        ScopedAmsBackend ams(4);
        ams.backend->set_remap_strategy(AmsBackend::RemapStrategy::None);
        REQUIRE_FALSE(helix::printer::can_remap(*ams.backend));
        CHECK_FALSE(remap_button_offered(test_screen(), pump));
    }

    SECTION("a declared route that is not READY is not either") {
        // The case the route-only gate got wrong: strategy says Native, the
        // firmware object it writes through has not been discovered.
        ScopedAmsBackend ams(4);
        ams.backend->set_remap_ready(false);
        REQUIRE(ams.backend->get_remap_strategy() != AmsBackend::RemapStrategy::None);
        REQUIRE_FALSE(helix::printer::can_remap(*ams.backend));
        CHECK_FALSE(remap_button_offered(test_screen(), pump));
    }
}
