// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_mini_status_active_lane.cpp
 * @brief The mini-status strip's active lane comes from per-slot authority
 *
 * The active-lane highlight has one source of truth: the per-slot active-loaded
 * subject (AmsBackend::slot_is_actively_loaded(i)), the same read ui_ams_slot.cpp
 * makes. These tests pin the two ways the old `i == current_slot` derivation
 * diverged from it — an idle unload, and a backend whose per-slot parse disagrees
 * with firmware's current_slot (#1194) — plus the empty-lane presentation and
 * fill floor that must read the same on the strip as on the AMS panel.
 */

#include "ui_ams_mini_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/**
 * @brief Mock whose aggregate load state, per-slot authority, and per-slot
 *        status are all settable.
 *
 * Three things need patching that AmsBackendMock does not expose:
 *
 *  - The aggregate pair (current_slot / filament_loaded), so an idle unload can
 *    be staged without driving the mock's timed unload simulation.
 *  - has_per_slot_loaded_authority(), to exercise the AFC/CFS rule.
 *  - Per-slot SlotStatus. AmsBackendMock::set_slot_info() deliberately copies
 *    only the filament fields (color, material, brand, weights, Spoolman ids) —
 *    status is firmware-derived there, so writing it through set_slot_info() is
 *    silently dropped.
 *
 * Both read paths are patched, because they are genuinely different call sites:
 * AmsState::sync_from_backend() reads slots out of get_system_info(), while the
 * mini-status widget and slot_is_actively_loaded()'s per-slot rule both call
 * get_slot_info(). Patching one and not the other would let the test's own
 * fixture disagree with itself.
 */
class ActiveLaneMock : public AmsBackendMock {
  public:
    static constexpr int MAX_SLOTS = 16;

    explicit ActiveLaneMock(int slot_count) : AmsBackendMock(slot_count) {}

    bool per_slot_authority = false;
    bool aggregate_loaded = true;
    int current = 0;

    /// Force a lane's status (the field set_slot_info() drops).
    void set_status(int slot_index, SlotStatus status) {
        if (slot_index >= 0 && slot_index < MAX_SLOTS) {
            status_override_[slot_index] = status;
        }
    }

    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return per_slot_authority;
    }
    [[nodiscard]] bool is_filament_loaded() const override {
        return aggregate_loaded;
    }
    [[nodiscard]] int get_current_slot() const override {
        return current;
    }
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override {
        SlotInfo slot = AmsBackendMock::get_slot_info(slot_index);
        apply_status_override(slot_index, slot);
        return slot;
    }
    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.current_slot = current;
        info.filament_loaded = aggregate_loaded;
        for (auto& unit : info.units) {
            for (int s = 0; s < static_cast<int>(unit.slots.size()); ++s) {
                apply_status_override(unit.first_slot_global_index + s, unit.slots[s]);
            }
        }
        return info;
    }

  private:
    void apply_status_override(int slot_index, SlotInfo& slot) const {
        if (slot_index >= 0 && slot_index < MAX_SLOTS && status_override_[slot_index]) {
            slot.status = *status_override_[slot_index];
        }
    }

    std::array<std::optional<SlotStatus>, MAX_SLOTS> status_override_{};
};

ActiveLaneMock* install_mock(int slot_count) {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<ActiveLaneMock>(slot_count);
    auto* raw = mock.get();
    ams.set_backend(std::move(mock));
    return raw;
}

void teardown_ams() {
    auto& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
}

/**
 * @brief Give a lane a known identity and status.
 *
 * Filament fields go through the backend's own set_slot_info(); status goes
 * through the mock's override, since set_slot_info() drops it.
 */
void set_slot(ActiveLaneMock* mock, int index, SlotStatus status, const char* material,
              float total_g = 1000.0f, float remaining_g = 500.0f) {
    SlotInfo s = mock->get_slot_info(index);
    s.material = material;
    s.brand.clear();
    s.spool_name.clear();
    s.spoolman_id = 0;
    s.total_weight_g = total_g;
    s.remaining_weight_g = remaining_g;
    mock->set_slot_info(index, s);
    mock->set_status(index, status);
}

bool badge_is_active(lv_obj_t* widget, int lane) {
    lv_obj_t* badge = UITest::find_by_name(widget, ("spool_badge_" + std::to_string(lane)).c_str());
    REQUIRE(badge != nullptr);
    return lv_color_eq(lv_obj_get_style_bg_color(badge, LV_PART_MAIN),
                       theme_manager_get_color("success"));
}

/** bars_container -> slot column `lane` -> bar_bg (the outlined element). */
lv_obj_t* bar_bg_for(lv_obj_t* widget, int lane) {
    lv_obj_t* bars = UITest::find_by_name(widget, "ams_bars_container");
    REQUIRE(bars != nullptr);
    REQUIRE(static_cast<int>(lv_obj_get_child_count(bars)) > lane);
    lv_obj_t* column = lv_obj_get_child(bars, lane);
    REQUIRE(column != nullptr);
    REQUIRE(lv_obj_get_child_count(column) > 0);
    return lv_obj_get_child(column, 0);
}

/** style_slot_bar() gives only the loaded lane a 2px outline. */
bool bar_is_loaded(lv_obj_t* widget, int lane) {
    return lv_obj_get_style_border_width(bar_bg_for(widget, lane), LV_PART_MAIN) == 2;
}

} // namespace

// After an idle unload the firmware's current_slot still names the lane it last
// used, but nothing is seated. The strip must clear with the AMS panel — the
// exact divergence ui_ams_slot.cpp's "SINGLE SOURCE OF TRUTH" comment describes.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: idle unload clears the lane highlight",
                 "[ams][mini_status]") {
    auto* mock = install_mock(2);
    mock->current = 0;
    mock->aggregate_loaded = true;
    set_slot(mock, 0, SlotStatus::LOADED, "PETG");
    set_slot(mock, 1, SlotStatus::AVAILABLE, "ABS");

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 260); // spool mode
    helix::ui::UpdateQueue::instance().drain();

    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(badge_is_active(w, 0));
    REQUIRE_FALSE(badge_is_active(w, 1));

    // Idle unload: filament leaves the toolhead, current_slot is NOT reset.
    mock->aggregate_loaded = false;
    set_slot(mock, 0, SlotStatus::AVAILABLE, "PETG");
    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    // current_slot still points at lane 0 — the stale signal the strip used to read.
    REQUIRE(lv_subject_get_int(AmsState::instance().get_current_slot_subject()) == 0);
    REQUIRE(lv_subject_get_int(AmsState::instance().get_slot_active_loaded_subject(0)) == 0);
    CHECK_FALSE(badge_is_active(w, 0));
    CHECK_FALSE(badge_is_active(w, 1));

    lv_obj_delete(w);
    teardown_ams();
}

// The bar view (colspan 1) reads the same authority as the spool view.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: idle unload clears the bar-view outline",
                 "[ams][mini_status]") {
    auto* mock = install_mock(2);
    mock->current = 0;
    mock->aggregate_loaded = true;
    set_slot(mock, 0, SlotStatus::LOADED, "PETG");
    set_slot(mock, 1, SlotStatus::AVAILABLE, "ABS");

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 130); // bar mode
    helix::ui::UpdateQueue::instance().drain();

    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(bar_is_loaded(w, 0));
    REQUIRE_FALSE(bar_is_loaded(w, 1));

    mock->aggregate_loaded = false;
    set_slot(mock, 0, SlotStatus::AVAILABLE, "PETG");
    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(lv_subject_get_int(AmsState::instance().get_current_slot_subject()) == 0);
    CHECK_FALSE(bar_is_loaded(w, 0));

    lv_obj_delete(w);
    teardown_ams();
}

// AFC/CFS-shaped disagreement (#1194): the backend's per-slot parse says lane 1
// is seated while firmware's current_slot still says lane 0. Per-slot authority
// wins, so the strip highlights lane 1 — the same lane the AMS panel glows.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: per-slot authority beats a stale current_slot",
                 "[ams][mini_status]") {
    auto* mock = install_mock(2);
    mock->per_slot_authority = true;
    mock->current = 0; // firmware's aggregate pointer — stale
    mock->aggregate_loaded = true;
    set_slot(mock, 0, SlotStatus::AVAILABLE, "PETG"); // per-slot parse: NOT seated
    set_slot(mock, 1, SlotStatus::LOADED, "ABS");     // per-slot parse: seated

    // Fixture precondition. AmsBackendMock::set_slot_info() silently drops
    // status, so ActiveLaneMock's override is the only thing staging this — if
    // it ever stops working, fail HERE rather than quietly asserting about a
    // lane whose status is still the mock's ctor default.
    REQUIRE(mock->get_slot_info(0).status == SlotStatus::AVAILABLE);
    REQUIRE(mock->get_slot_info(1).status == SlotStatus::LOADED);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();

    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(lv_subject_get_int(AmsState::instance().get_current_slot_subject()) == 0);
    REQUIRE(lv_subject_get_int(AmsState::instance().get_slot_active_loaded_subject(0)) == 0);
    REQUIRE(lv_subject_get_int(AmsState::instance().get_slot_active_loaded_subject(1)) == 1);

    CHECK_FALSE(badge_is_active(w, 0)); // NOT the current_slot answer
    CHECK(badge_is_active(w, 1));

    lv_obj_delete(w);
    teardown_ams();
}

// #1071/#1065: an eject keeps the lane's identity, so the lane must read
// "assigned, not present" — retained material, ghosted — not "empty and unknown".
TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini: assigned-but-ejected lane ghosts, unassigned reads Empty",
                 "[ams][mini_status]") {
    auto* mock = install_mock(2);
    mock->current = -1;
    mock->aggregate_loaded = false;
    set_slot(mock, 0, SlotStatus::EMPTY, "PETG", 1000.0f, 0.0f); // ejected, identity retained
    set_slot(mock, 1, SlotStatus::EMPTY, "", 0.0f, 0.0f);        // never assigned

    // Fixture precondition (see the authority case): both lanes must really be
    // EMPTY, and lane 1 must really carry no identity, or the branches under
    // test are never reached.
    REQUIRE(mock->get_slot_info(0).status == SlotStatus::EMPTY);
    REQUIRE(mock->get_slot_info(1).status == SlotStatus::EMPTY);
    REQUIRE(mock->get_slot_info(0).material == "PETG");
    REQUIRE(mock->get_slot_info(1).material.empty());
    REQUIRE(mock->get_slot_info(1).spoolman_id == 0);
    REQUIRE(mock->get_slot_info(1).brand.empty());
    REQUIRE(mock->get_slot_info(1).spool_name.empty());

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();

    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* mat0 = UITest::find_by_name(w, "spool_material_0");
    lv_obj_t* mat1 = UITest::find_by_name(w, "spool_material_1");
    REQUIRE(mat0 != nullptr);
    REQUIRE(mat1 != nullptr);

    // Assigned lane: material kept, and dimmed in lockstep with the spool visual.
    CHECK(std::string(lv_label_get_text(mat0)) == "PETG");
    CHECK(lv_obj_get_style_text_opa(mat0, LV_PART_MAIN) == LV_OPA_20);

    // Unassigned lane: names its purpose, at full strength (nothing to ghost).
    CHECK(std::string(lv_label_get_text(mat1)) == std::string(lv_tr("Empty")));
    CHECK(lv_obj_get_style_text_opa(mat1, LV_PART_MAIN) == LV_OPA_COVER);

    lv_obj_delete(w);
    teardown_ams();
}

// The strip and the overview's mini bars must floor fill identically: a present
// lane with nothing left keeps a visible sliver rather than vanishing on one
// surface and not the other.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: spent lane keeps the shared fill floor",
                 "[ams][mini_status]") {
    auto* mock = install_mock(1);
    mock->current = -1;
    mock->aggregate_loaded = false;
    set_slot(mock, 0, SlotStatus::AVAILABLE, "PLA", 1000.0f, 0.0f); // present, 0% left

    // Fixture precondition: the floor only applies to a PRESENT lane.
    REQUIRE(mock->get_slot_info(0).is_present());

    // The floor the overview's mini bars use — derived from the shared helper,
    // not hardcoded here.
    const int shared_floor = ams_draw::fill_percent_from_slot(mock->get_slot_info(0));
    REQUIRE(shared_floor > 0);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 130); // bar mode exposes the fill element
    helix::ui::UpdateQueue::instance().drain();

    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* bar_bg = bar_bg_for(w, 0);
    REQUIRE(lv_obj_get_child_count(bar_bg) > 0);
    lv_obj_t* bar_fill = lv_obj_get_child(bar_bg, 0);
    REQUIRE(bar_fill != nullptr);
    // min_pct 0 hid the fill entirely; the shared default keeps the sliver.
    CHECK_FALSE(lv_obj_has_flag(bar_fill, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_height(bar_fill, LV_PART_MAIN) == LV_PCT(shared_floor));

    lv_obj_delete(w);
    teardown_ams();
}
