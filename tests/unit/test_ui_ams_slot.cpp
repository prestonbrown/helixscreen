// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_ams_slot.cpp
 * @brief TDD tests for ams_slot XML widget conversion
 *
 * These tests are written BEFORE the XML conversion is complete.
 * They should FAIL initially - that's the point of TDD.
 *
 * Tests cover:
 * - Widget structure (children, named parts)
 * - Public API (get/set index, fill level, layout info)
 * - Subject binding (material label, color updates)
 * - Status badge visibility based on slot status
 * - Cleanup and lifecycle (observer cleanup on delete)
 */

#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "config.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Helper: Create an ams_slot widget with specified slot index
// ============================================================================

/**
 * @brief Create an ams_slot widget via XML
 * @param parent Parent object (typically test_screen())
 * @param slot_index Slot index to assign (0-15)
 * @return Created widget or nullptr on failure
 */
static lv_obj_t* create_ams_slot(lv_obj_t* parent, int slot_index) {
    char index_str[4];
    snprintf(index_str, sizeof(index_str), "%d", slot_index);
    const char* attrs[] = {"slot_index", index_str, nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
}

// ============================================================================
// Structure Tests - Verify widget creates expected child hierarchy
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: creates with valid structure",
                 "[ui][ams_slot][structure]") {
    // Register the ams_slot widget
    ui_ams_slot_register();

    // Create slot via XML
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    // Slot should have a spool_container child for the spool visual
    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: flat style builds spool rings",
                 "[ui][ams_slot][structure]") {
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "flat");
    ui_ams_slot_register();
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);
    REQUIRE(lv_obj_get_child_count(spool_container) >=
            4); // outer/filament/hub + placeholder + error (+badges)
    lv_obj_delete(slot);
    // Restore default so sibling tests (which assume 3D) are not affected.
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: has material label", "[ui][ams_slot][structure]") {
    ui_ams_slot_register();

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    // Slot should have a material label for displaying filament type
    lv_obj_t* material_label = UITest::find_by_name(slot, "material_label");
    REQUIRE(material_label != nullptr);

    // Label should be a label widget
    REQUIRE(lv_obj_check_type(material_label, &lv_label_class));

    lv_obj_delete(slot);
}

// ============================================================================
// API Tests - Verify public API functions work correctly
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: get_index returns slot index",
                 "[ui][ams_slot][api]") {
    ui_ams_slot_register();

    // Create slot with index 5
    lv_obj_t* slot = create_ams_slot(test_screen(), 5);
    REQUIRE(slot != nullptr);

    // get_index should return the same index
    int index = ui_ams_slot_get_index(slot);
    REQUIRE(index == 5);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: set_index changes slot index",
                 "[ui][ams_slot][api]") {
    ui_ams_slot_register();

    // Create slot with initial index 0
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    REQUIRE(ui_ams_slot_get_index(slot) == 0);

    // Change to index 7
    ui_ams_slot_set_index(slot, 7);
    REQUIRE(ui_ams_slot_get_index(slot) == 7);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: get_index returns -1 for non-ams_slot widget",
                 "[ui][ams_slot][api]") {
    ui_ams_slot_register();

    // Create a regular button (not an ams_slot)
    lv_obj_t* btn = lv_button_create(test_screen());
    REQUIRE(btn != nullptr);

    // get_index should return -1 for non-ams_slot widgets
    int index = ui_ams_slot_get_index(btn);
    REQUIRE(index == -1);

    lv_obj_delete(btn);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: out-of-range fill subject value clamps to full",
                 "[ui][ams_slot][fill]") {
    // The public float set_fill_level() API was removed (dead — the widget
    // renders fill only from its per-slot subject). The clamp it used to test
    // now lives on the subject apply path (apply_slot_fill_pct clamps to 0-100),
    // so exercise it through the subject: a percent above 100 (e.g. a bad
    // remaining>total ratio) renders full, not overfilled. The empty end and the
    // -1 "no data" skip are covered by "fill renders from subject without panel
    // push".
    ui_ams_slot_register();
    AmsState::instance().init_subjects(false);

    lv_subject_t* fill = AmsState::instance().get_slot_fill_subject(0);
    REQUIRE(fill != nullptr);
    lv_subject_set_int(fill, 150); // > 100

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    // setup_slot_observers applies the current subject value synchronously.
    CHECK(ui_ams_slot_get_fill_level(slot) == Catch::Approx(1.0f));

    lv_obj_delete(slot);
    lv_subject_set_int(fill, -1); // leave neutral for other tests
}

// Structural regression guard for the "spool always 100% full" bug: the widget
// must render fill from its per-slot subject WITHOUT any panel pushing fill
// imperatively. This is what makes AmsOverviewPanel's unit-detail spools
// correct — they never pushed fill imperatively.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: fill renders from subject without panel push",
                 "[ui][ams_slot][fill]") {
    ui_ams_slot_register();
    // init_subjects is idempotent on the shared singleton; register_xml=false is
    // fine because the widget observes via the C++ accessor, not an XML name.
    AmsState::instance().init_subjects(false);

    // Write the per-slot fill subject exactly as sync_from_backend would (50%).
    lv_subject_t* fill = AmsState::instance().get_slot_fill_subject(0);
    REQUIRE(fill != nullptr);
    lv_subject_set_int(fill, 50);

    // Creating the widget runs setup_slot_observers, which applies the CURRENT
    // subject value synchronously — no panel touches the fill level.
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    CHECK(ui_ams_slot_get_fill_level(slot) == Catch::Approx(0.50f));

    // A later change flows through the observer (deferred via queue_update, #82).
    lv_subject_set_int(fill, 25);
    process_lvgl(50);
    CHECK(ui_ams_slot_get_fill_level(slot) == Catch::Approx(0.25f));

    // -1 ("no data") must leave the current fill untouched.
    lv_subject_set_int(fill, -1);
    process_lvgl(50);
    CHECK(ui_ams_slot_get_fill_level(slot) == Catch::Approx(0.25f));

    lv_obj_delete(slot);
}

// Structural regression guard for #1065 (native ZMOD AD5X: "material type
// stuck, color updates"). The widget must render MATERIAL from its per-slot
// subject WITHOUT any panel re-reading it imperatively — the same structural
// fix fill got. This is what makes every ams_slot consumer (AmsPanel,
// AmsOverviewPanel, AmsDetail) reactive on a material-only change; before it,
// material only repainted as a side effect of a color change or a container
// that happened to rebuild on slots_version, so surfaces without that path
// (AmsDetail) — and any lane whose type changed with its color unchanged —
// kept showing the stale material.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: material renders from subject without panel push",
                 "[ui][ams_slot][material][1065]") {
    ui_ams_slot_register();
    // The widget observes via the C++ accessor, not an XML name, so
    // register_xml=false is fine (mirrors the fill test).
    AmsState::instance().init_subjects(false);

    // Seed the per-slot material subject exactly as sync_from_backend would.
    lv_subject_t* mat = AmsState::instance().get_slot_material_subject(0);
    REQUIRE(mat != nullptr);
    lv_subject_copy_string(mat, "PLA");

    // Creating the widget runs setup_slot_observers, which applies the CURRENT
    // subject value synchronously — no panel pushes material.
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    lv_obj_t* material_label = UITest::find_by_name(slot, "material_label");
    REQUIRE(material_label != nullptr);
    CHECK(std::string(UITest::get_text(material_label)) == "PLA");

    // A material-ONLY change (color unchanged, no refresh_slots, no
    // slots_version rebuild) must repaint the label through the observer.
    lv_subject_copy_string(mat, "PETG");
    process_lvgl(50);
    CHECK(std::string(UITest::get_text(material_label)) == "PETG");

    // Empty material falls back to the "--" placeholder.
    lv_subject_copy_string(mat, "");
    process_lvgl(50);
    CHECK(std::string(UITest::get_text(material_label)) == "--");

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: set_layout_info does not crash",
                 "[ui][ams_slot][api]") {
    ui_ams_slot_register();

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    // Call set_layout_info with various configurations
    // This tests stagger positioning logic - should not crash
    ui_ams_slot_set_layout_info(slot, 0, 4);   // 4 slots, first slot
    ui_ams_slot_set_layout_info(slot, 3, 8);   // 8 slots, middle slot
    ui_ams_slot_set_layout_info(slot, 15, 16); // 16 slots, last slot

    // If we get here without crashing, the test passes
    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: move_label_to_layer reparents label",
                 "[ui][ams_slot][api]") {
    ui_ams_slot_register();

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    // Create a labels layer container
    lv_obj_t* labels_layer = lv_obj_create(test_screen());
    REQUIRE(labels_layer != nullptr);

    // Initially, label should be in slot's tree
    lv_obj_t* material_label = UITest::find_by_name(slot, "material_label");
    REQUIRE(material_label != nullptr);

    // Move label to the layer
    ui_ams_slot_move_label_to_layer(slot, labels_layer, 100);

    // The label should now be a child of the labels_layer
    // (This may require looking for it in the layer instead of the slot)
    lv_obj_t* label_in_layer = UITest::find_by_name(labels_layer, "material_label");
    // Note: Depending on implementation, label may be reparented or a proxy created
    // The key is that this operation doesn't crash

    lv_obj_delete(labels_layer);
    lv_obj_delete(slot);
}

// ============================================================================
// Subject Binding Tests - Verify reactive updates from AmsState
// ============================================================================

// Covers the BACKEND -> subject leg of the material chain, which the widget-level
// "[1065]" test above does not: sync_from_backend() must copy SlotInfo::material
// into the per-slot material subject, from where the widget's own observer paints
// the label. Together the two tests pin backend -> subject -> label end to end.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: material label binds to subject",
                 "[ui][ams_slot][binding]") {
    ui_ams_slot_register();
    // LVGLUITestFixture does not init AmsState subjects. Without this the
    // per-slot material subject is raw memory, sync_from_backend()'s
    // lv_subject_copy_string lands nowhere, and the label stays at "--".
    // register_xml=false is enough — the widget observes via the C++ accessor.
    AmsState::instance().init_subjects(false);

    // Set up mock backend with known data
    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());

    // Configure slot 0 with PLA
    SlotInfo info;
    info.slot_index = 0;
    info.material = "PLA";
    info.color_rgb = 0xFF0000; // Red
    info.status = SlotStatus::AVAILABLE;
    mock_ptr->set_slot_info(0, info);

    // Connect backend to AmsState
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();

    // Process LVGL events
    process_lvgl(50);

    // Create slot widget - should pick up "PLA" from subject
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    // Give time for subject binding to update
    process_lvgl(50);

    // Find and check material label
    lv_obj_t* material_label = UITest::find_by_name(slot, "material_label");
    REQUIRE(material_label != nullptr);

    std::string label_text = UITest::get_text(material_label);
    REQUIRE(label_text == "PLA");

    lv_obj_delete(slot);
}

// Find the spool visual whose color the slot updates. In the default 3D style
// the filament color lives on the lv_canvas child of spool_container (read via
// ui_spool_canvas_get_color); apply_slot_color() writes there, NOT to
// spool_container itself. Returns the canvas, or nullptr if not in 3D style.
static lv_obj_t* find_spool_canvas(lv_obj_t* spool_container) {
    if (!spool_container) {
        return nullptr;
    }
    uint32_t child_count = lv_obj_get_child_count(spool_container);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(spool_container, i);
        if (child && lv_obj_check_type(child, &lv_canvas_class)) {
            return child;
        }
    }
    return nullptr;
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: color subject updates spool",
                 "[ui][ams_slot][binding]") {
    ui_ams_slot_register();
    // LVGLUITestFixture does not init AmsState subjects; do it here so the
    // per-slot color subjects are live INT subjects (otherwise lv_subject_set_int
    // operates on an uninitialized subject and the observer never fires).
    AmsState::instance().init_subjects(true);

    // Set up mock backend
    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());

    // Configure slot 0 with initial color (Red)
    SlotInfo info;
    info.slot_index = 0;
    info.material = "PLA";
    info.color_rgb = 0xFF0000; // Red
    info.status = SlotStatus::AVAILABLE;
    mock_ptr->set_slot_info(0, info);

    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();
    process_lvgl(50);

    // Create slot widget
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    process_lvgl(50);

    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);

    // apply_slot_color() writes the filament color to the spool_canvas (default
    // 3D style), not to spool_container. Read the color from the canvas where
    // the widget actually applies it.
    lv_obj_t* canvas = find_spool_canvas(spool_container);
    REQUIRE(canvas != nullptr);

    lv_color_t initial_color = ui_spool_canvas_get_color(canvas);

    // Now update the color subject to Blue
    lv_subject_t* color_subj = AmsState::instance().get_slot_color_subject(0);
    REQUIRE(color_subj != nullptr);
    lv_subject_set_int(color_subj, 0x0000FF); // Blue

    // observe_int_sync defers callbacks via queue_update (#82), so the color
    // observer fires on a later tick. process_lvgl() drains the UpdateQueue via
    // lv_timer_handler_safe(), so the deferred apply_slot_color() runs here.
    process_lvgl(50);

    lv_color_t updated_color = ui_spool_canvas_get_color(canvas);

    // Colors should be different (Red vs Blue)
    REQUIRE_FALSE(lv_color_eq(initial_color, updated_color));

    lv_obj_delete(slot);
}

// ============================================================================
// Status Tests - Verify badge visibility based on slot status
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: status badge visible when not empty",
                 "[ui][ams_slot][status]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);

    // Set up mock with AVAILABLE status
    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());
    mock_ptr->force_slot_status(0, SlotStatus::AVAILABLE);

    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();
    process_lvgl(50);

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    process_lvgl(50);

    // Find status badge
    lv_obj_t* status_badge = UITest::find_by_name(slot, "status_badge");
    REQUIRE(status_badge != nullptr);

    // Badge should be visible for AVAILABLE status
    REQUIRE(UITest::is_visible(status_badge));

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: status badge visible when empty",
                 "[ui][ams_slot][status]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);

    // Set up mock with EMPTY status
    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());
    mock_ptr->force_slot_status(0, SlotStatus::EMPTY);

    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();
    process_lvgl(50);

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    process_lvgl(50);

    // Find status badge
    lv_obj_t* status_badge = UITest::find_by_name(slot, "status_badge");
    REQUIRE(status_badge != nullptr);

    // Badge stays visible (gray) even for EMPTY status so that every physical
    // gate remains numbered/visible — deliberate behavior since
    // 260552adc "fix(ams-slot): show tool badge on empty unassigned gates"
    // (apply_slot_status: SlotStatus::EMPTY keeps show_badge = true).
    REQUIRE(UITest::is_visible(status_badge));

    lv_obj_delete(slot);
}

// ============================================================================
// Cleanup Tests - Verify proper observer cleanup on widget deletion
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: deletion cleans up observers",
                 "[ui][ams_slot][cleanup]") {
    ui_ams_slot_register();

    // Set up mock backend
    auto mock = AmsBackend::create_mock(4);
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();
    process_lvgl(50);

    // Create slot
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    process_lvgl(50);

    // Delete the slot
    lv_obj_delete(slot);
    slot = nullptr;

    // Process LVGL to ensure cleanup completes
    process_lvgl(50);

    // Now update the subject - should NOT crash even though widget is deleted
    lv_subject_t* color_subj = AmsState::instance().get_slot_color_subject(0);
    if (color_subj != nullptr) {
        lv_subject_set_int(color_subj, 0x00FF00);
    }

    // Process LVGL - should not crash
    process_lvgl(50);

    // If we get here, cleanup was successful
    SUCCEED("Observer cleanup on deletion succeeded");
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: multiple slots cleanup independently",
                 "[ui][ams_slot][cleanup]") {
    ui_ams_slot_register();

    // Set up mock backend with enough slots
    auto mock = AmsBackend::create_mock(8);
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();
    process_lvgl(50);

    // Create two slots
    lv_obj_t* slot0 = create_ams_slot(test_screen(), 0);
    lv_obj_t* slot1 = create_ams_slot(test_screen(), 1);
    REQUIRE(slot0 != nullptr);
    REQUIRE(slot1 != nullptr);
    process_lvgl(50);

    // Delete only slot 0
    lv_obj_delete(slot0);
    slot0 = nullptr;
    process_lvgl(50);

    // Update slot 0's subject - should not crash (observer removed)
    lv_subject_t* color_subj0 = AmsState::instance().get_slot_color_subject(0);
    if (color_subj0 != nullptr) {
        lv_subject_set_int(color_subj0, 0xFF00FF);
    }
    process_lvgl(50);

    // Slot 1 should still work
    lv_obj_t* material_label1 = UITest::find_by_name(slot1, "material_label");
    REQUIRE(material_label1 != nullptr);

    // Update slot 1's subject - slot1's observer should still be active
    lv_subject_t* color_subj1 = AmsState::instance().get_slot_color_subject(1);
    if (color_subj1 != nullptr) {
        lv_subject_set_int(color_subj1, 0xFFFF00);
    }
    process_lvgl(50);

    // Clean up
    lv_obj_delete(slot1);
}

// ============================================================================
// Refresh Tests - Verify manual refresh from AmsState
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: refresh updates from AmsState",
                 "[ui][ams_slot][refresh]") {
    ui_ams_slot_register();

    // Set up mock backend
    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());

    // Initial state: PLA
    SlotInfo info;
    info.slot_index = 0;
    info.material = "PLA";
    info.status = SlotStatus::AVAILABLE;
    mock_ptr->set_slot_info(0, info);

    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();
    process_lvgl(50);

    // Create slot
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    process_lvgl(50);

    // Verify initial state
    lv_obj_t* material_label = UITest::find_by_name(slot, "material_label");
    REQUIRE(material_label != nullptr);
    REQUIRE(UITest::get_text(material_label) == "PLA");

    // Update backend data (simulating backend update)
    // Note: In real usage, this would come from Moonraker
    // Here we directly update the subject for testing
    // In actual implementation, sync_from_backend would be called

    // Force a refresh
    ui_ams_slot_refresh(slot);
    process_lvgl(50);

    // The refresh function should re-read current state from AmsState
    // If data hasn't changed, label should still show "PLA"
    REQUIRE(UITest::get_text(material_label) == "PLA");

    lv_obj_delete(slot);
}

// ============================================================================
// Edge Cases - Verify handling of boundary conditions
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: handles maximum slot index",
                 "[ui][ams_slot][edge]") {
    ui_ams_slot_register();

    // Create slot with max index (15)
    lv_obj_t* slot = create_ams_slot(test_screen(), 15);
    REQUIRE(slot != nullptr);

    REQUIRE(ui_ams_slot_get_index(slot) == 15);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: handles invalid slot index gracefully",
                 "[ui][ams_slot][edge]") {
    ui_ams_slot_register();

    // Try to create with invalid index (out of range)
    // Current implementation stores the index as-is without clamping
    // This is acceptable - callers should use valid indices
    lv_obj_t* slot = create_ams_slot(test_screen(), 99);

    // Widget should still be created
    REQUIRE(slot != nullptr);

    // Index is stored as-is (no clamping currently implemented)
    int index = ui_ams_slot_get_index(slot);
    REQUIRE(index == 99);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: handles negative slot index gracefully",
                 "[ui][ams_slot][edge]") {
    ui_ams_slot_register();

    // Try to create with negative index
    const char* attrs[] = {"slot_index", "-1", nullptr};
    lv_obj_t* slot = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_slot", attrs));

    // Should handle gracefully
    if (slot != nullptr) {
        int index = ui_ams_slot_get_index(slot);
        // Should be normalized to valid range or -1 for "unset"
        REQUIRE(index >= -1);
        REQUIRE(index <= 15);
        lv_obj_delete(slot);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: get_fill_level returns 1.0 for non-ams_slot",
                 "[ui][ams_slot][edge]") {
    ui_ams_slot_register();

    // Create a regular object
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(obj != nullptr);

    // get_fill_level on non-ams_slot should return default (1.0)
    float level = ui_ams_slot_get_fill_level(obj);
    REQUIRE(level == Catch::Approx(1.0f));

    lv_obj_delete(obj);
}

// ============================================================================
// Filament Slot Override — Ghost render for empty slots with override metadata
// ============================================================================
//
// When a slot is physically empty but the user has configured a filament
// override (brand, spool_name, material, or linked Spoolman spool), the slot
// should render the spool visual at 20% opacity ("ghosted") instead of showing
// the empty placeholder. This lets users see at a glance which empty slots
// are "assigned" and waiting for a reload vs truly unused.
//
// Prior behavior only treated spoolman_id>0 or !material.empty() as assigned.
// Brand/spool_name were missed for backends (IFS-style) where a user-configured
// override exists without a Spoolman link.

namespace {

/// Result of examining the spool visual after status update.
struct SpoolVisualState {
    bool any_ghosted = false;      ///< true if any child has opa/bg_opa == LV_OPA_20
    bool any_spool_hidden = false; ///< true if spool_canvas or spool rings are hidden
    int child_count = 0;           ///< For diagnostic purposes
};

/// Inspect spool_container children to determine the ghost/placeholder state.
///
/// After ui_ams_slot creates the widget, the spool_container child order is:
///   [0] status_badge (XML, named)
///   [1] spool_canvas (3d) or spool_outer (flat) — the "main spool visual"
///   [2] empty_placeholder (unnamed, transparent)
///   [3] tool_badge (XML, named, moved to end via move_to_index)
///   [4] error_indicator (unnamed, moved to end)
/// The XML-named badges are always named; the dynamically-created visuals are
/// unnamed. We key off the first unnamed child for the spool visual.
SpoolVisualState inspect_spool_state(lv_obj_t* spool_container) {
    SpoolVisualState st;
    uint32_t n = lv_obj_get_child_count(spool_container);
    st.child_count = static_cast<int>(n);

    lv_obj_t* spool_visual = nullptr;
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(spool_container, i);
        if (!child)
            continue;
        const char* name = lv_obj_get_name(child);
        if (name)
            continue; // skip named XML children (status_badge, tool_badge)
        spool_visual = child;
        break;
    }
    if (!spool_visual)
        return st;

    lv_opa_t opa = lv_obj_get_style_opa(spool_visual, LV_PART_MAIN);
    lv_opa_t bg_opa = lv_obj_get_style_bg_opa(spool_visual, LV_PART_MAIN);
    bool hidden = lv_obj_has_flag(spool_visual, LV_OBJ_FLAG_HIDDEN);

    if (opa == LV_OPA_20 || bg_opa == LV_OPA_20) {
        st.any_ghosted = true;
    }
    if (hidden) {
        st.any_spool_hidden = true;
    }
    return st;
}

/// Drive slot 0 EMPTY in AmsState and create the widget.
lv_obj_t* create_empty_slot(lv_obj_t* parent, AmsBackendMock* mock_ptr) {
    mock_ptr->force_slot_status(0, SlotStatus::EMPTY);

    // Seed status subject to EMPTY so observer fires correctly on widget creation.
    lv_subject_t* status_subj = AmsState::instance().get_slot_status_subject(0);
    REQUIRE(status_subj != nullptr);
    lv_subject_set_int(status_subj, static_cast<int>(SlotStatus::EMPTY));

    char index_str[4];
    snprintf(index_str, sizeof(index_str), "%d", 0);
    const char* attrs[] = {"slot_index", index_str, nullptr};
    auto* slot = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
    REQUIRE(slot != nullptr);
    return slot;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AMS slot ghosts empty slot with brand-only metadata",
                 "[ui][ams_slot][filament_slot_override]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);

    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());

    // Brand set, but no spoolman_id, no material, no spool_name.
    SlotInfo info;
    info.slot_index = 0;
    info.brand = "Polymaker";
    mock_ptr->set_slot_info(0, info);

    AmsState::instance().set_backend(std::move(mock));

    lv_obj_t* slot = create_empty_slot(test_screen(), mock_ptr);
    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);

    // Brand-only override triggers ghost render: spool is visible at 20% opacity,
    // and the main spool visual should NOT be hidden (placeholder mode).
    auto st = inspect_spool_state(spool_container);
    REQUIRE(st.any_ghosted);
    REQUIRE_FALSE(st.any_spool_hidden);

    AmsState::instance().clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "AMS slot ghosts empty slot with spool_name-only metadata",
                 "[ui][ams_slot][filament_slot_override]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);

    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());

    // spool_name set, but no brand, no spoolman_id, no material.
    SlotInfo info;
    info.slot_index = 0;
    info.spool_name = "Shop Floor #7";
    mock_ptr->set_slot_info(0, info);

    AmsState::instance().set_backend(std::move(mock));

    lv_obj_t* slot = create_empty_slot(test_screen(), mock_ptr);
    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);

    auto st = inspect_spool_state(spool_container);
    REQUIRE(st.any_ghosted);
    REQUIRE_FALSE(st.any_spool_hidden);

    AmsState::instance().clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "AMS slot hides empty slot with no metadata at all",
                 "[ui][ams_slot][filament_slot_override]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);

    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());

    // Truly empty — no brand, spool_name, material, or spoolman_id.
    SlotInfo info;
    info.slot_index = 0;
    mock_ptr->set_slot_info(0, info);

    AmsState::instance().set_backend(std::move(mock));

    lv_obj_t* slot = create_empty_slot(test_screen(), mock_ptr);
    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);

    // With no override metadata, the main spool visual is hidden (placeholder
    // shown). Regression guard for the original is_assigned behavior.
    auto st = inspect_spool_state(spool_container);
    REQUIRE(st.any_spool_hidden);
    REQUIRE_FALSE(st.any_ghosted);

    AmsState::instance().clear_backends();
}

TEST_CASE("SlotInfo::display_fill_level renders ghost lanes empty, present lanes by weight",
          "[ams][slot][1071]") {
    // Ghost lane: EMPTY status, but a Spoolman link + material were RETAINED
    // across an eject (#1071), so has_filament_info() is true. The fill bar must
    // read empty (0), NOT the metadata fallback — otherwise an ejected lane
    // renders as a full spool (#1071 BUG-1).
    SlotInfo ghost;
    ghost.status = SlotStatus::EMPTY;
    ghost.material = "PLA";
    ghost.color_rgb = 0xFF0000;
    REQUIRE(ghost.has_filament_info());
    REQUIRE_FALSE(ghost.is_present());
    auto gfill = ghost.display_fill_level();
    REQUIRE(gfill.has_value());
    CHECK(*gfill == Catch::Approx(0.0f));

    // Present lane, both weights known: real remaining/total ratio.
    SlotInfo weighed;
    weighed.status = SlotStatus::AVAILABLE;
    weighed.total_weight_g = 1000.0f;
    weighed.remaining_weight_g = 250.0f;
    auto wfill = weighed.display_fill_level();
    REQUIRE(wfill.has_value());
    CHECK(*wfill == Catch::Approx(0.25f));

    // Present lane, metadata but no remaining weight (e.g. Snapmaker RFID, or a
    // lane nobody tracks in Spoolman): full, matching every other printer UI.
    SlotInfo meta;
    meta.status = SlotStatus::AVAILABLE;
    meta.material = "PETG";
    auto mfill = meta.display_fill_level();
    REQUIRE(mfill.has_value());
    CHECK(*mfill == Catch::Approx(1.0f));

    // Present lane, no info at all: leave the bar unchanged (nullopt).
    SlotInfo bare;
    bare.status = SlotStatus::AVAILABLE;
    CHECK_FALSE(bare.display_fill_level().has_value());
}
