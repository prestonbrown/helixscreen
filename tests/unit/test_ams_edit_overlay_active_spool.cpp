// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"

#include <cstdlib>
#include <filesystem>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// ============================================================================
// TestAccess helper — exposes AmsEditOverlay internals for white-box testing
// ============================================================================

class AmsEditOverlayTestAccess {
  public:
    explicit AmsEditOverlayTestAccess(AmsEditOverlay& overlay) : overlay_(overlay) {}

    void set_original_info(const SlotInfo& info) {
        overlay_.original_info_ = info;
    }
    void set_working_info(const SlotInfo& info) {
        overlay_.working_info_ = info;
    }
    void set_api(MoonrakerAPI* api) {
        overlay_.api_ = api;
    }
    void set_slot_index(int idx) {
        overlay_.slot_index_ = idx;
    }
    void set_save_to_spoolman_opt_in(bool opt_in) {
        overlay_.save_to_spoolman_opt_in_ = opt_in;
    }
    void set_completion_callback(AmsEditOverlay::CompletionCallback cb) {
        overlay_.completion_callback_ = std::move(cb);
        overlay_.completion_fired_ = false;
    }

    void init_subjects() {
        overlay_.init_subjects();
    }

    // View subject: 0 = overview/form, 1 = Spoolman picker (kView* constants).
    int get_view_mode() {
        return lv_subject_get_int(&overlay_.view_mode_subject_);
    }
    void call_handle_save() {
        overlay_.handle_save();
    }
    void call_handle_back() {
        overlay_.handle_back();
    }
    bool completion_fired() const {
        return overlay_.completion_fired_;
    }

    // Forward the private static create-gate predicate (friend access).
    static bool should_create_new_spool(const SlotInfo& working_info, bool save_to_spoolman) {
        return AmsEditOverlay::should_create_new_spool(working_info, save_to_spoolman);
    }
    static bool is_material_identity_change(const SlotInfo& original, const SlotInfo& edited) {
        return AmsEditOverlay::is_material_identity_change(original, edited);
    }
    static bool needs_identity_confirmation(const SlotInfo& original, const SlotInfo& edited) {
        return AmsEditOverlay::needs_identity_confirmation(original, edited);
    }
    static AmsEditOverlay::WeightStaging
    decide_weight_staging(bool entered_tracked, bool remaining_filled, bool total_filled) {
        return AmsEditOverlay::decide_weight_staging(entered_tracked, remaining_filled,
                                                     total_filled);
    }
    static bool may_write_spoolman_now(const SlotInfo& original, const SlotInfo& edited) {
        return AmsEditOverlay::may_write_spoolman_now(original, edited);
    }

  private:
    AmsEditOverlay& overlay_;
};

// ============================================================================
// OverlayCommitFixture — the backend/API wiring of CommitFixture
// (test_ams_state_commit_slot.cpp) plus the temp-config isolation of
// ExternalSpoolCommitFixture (test_external_spool.cpp). The completion
// consumers commit through AmsState::commit_slot_edit /
// commit_external_spool_edit now, so an overlay save in these tests must
// reach a mock backend AND a mock Spoolman API without contaminating the
// real config.
// ============================================================================

struct OverlayCommitFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    AmsBackendMock* backend = nullptr;
    std::string temp_dir;
    std::string config_path;

    OverlayCommitFixture() : api(client, get_printer_state()) {
        temp_dir = std::filesystem::temp_directory_path().string() + "/helix_overlay_commit_" +
                   std::to_string(rand());
        std::filesystem::create_directories(temp_dir);
        config_path = temp_dir + "/settings.json";

        // Same cross-test contamination guard as TempConfigFixture.
        std::filesystem::remove(AppConstants::Update::config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::legacy_config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::env_backup_fallback());
        Config::get_instance()->init(config_path);
        SettingsManager::instance().clear_external_spool_info();

        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects observes PrinterState's print-state subject;
        // it must exist first or the observer attaches to nothing.
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);

        // A previous test file's SpoolmanManager::deinit_subjects() may have
        // latched its shutdown flag — unlatch it (see CommitFixture).
        SpoolmanManager::instance().init_subjects();
        SpoolmanManager::clear_identity_cache();

        ams.set_moonraker_api(&api);
    }

    ~OverlayCommitFixture() override {
        // Detach the mocks BEFORE members are destroyed and while LVGL still
        // runs (base-class teardown has not happened yet).
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        // Drain while AmsState's subjects are still alive; queued backend-event
        // syncs from this test must not leak into the next one.
        UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::clear_identity_cache();
        Config::get_instance()->clear_path();
        std::filesystem::remove_all(temp_dir);
    }

    /// Install a mock backend whose slot 0 carries spoolman_id.
    void install_backend(int slot0_spoolman_id) {
        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        AmsState::instance().set_backend(std::move(owned));

        SlotInfo slot = backend->get_slot_info(0);
        slot.spoolman_id = slot0_spoolman_id;
        backend->set_slot_info(0, slot, /*persist=*/false);
    }
};

// ============================================================================
// Tests: a save syncs the active spool with Moonraker via the completion
// consumer's AmsState commit (the overlay no longer pre-fires it itself)
// ============================================================================

TEST_CASE_METHOD(OverlayCommitFixture, "handle_save sets active spool when spool assigned (0 -> N)",
                 "[ams_edit_overlay][spoolman][active_spool]") {
    AmsEditOverlay overlay;
    AmsEditOverlayTestAccess access(overlay);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 0;

    SlotInfo working;
    working.spoolman_id = 42;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditOverlay::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
        // The external-spool consumers (AmsPanel / AmsOverviewPanel /
        // FilamentPanel) route the completion through the AmsState commit.
        AmsState::instance().commit_external_spool_edit(result.slot_info);
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 42);
}

TEST_CASE_METHOD(OverlayCommitFixture, "handle_save sets active spool when spool changed (N -> M)",
                 "[ams_edit_overlay][spoolman][active_spool]") {
    api.spoolman_mock().set_active_spool(42, nullptr, nullptr);

    AmsEditOverlay overlay;
    AmsEditOverlayTestAccess access(overlay);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 42;

    SlotInfo working;
    working.spoolman_id = 99;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditOverlay::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
        AmsState::instance().commit_external_spool_edit(result.slot_info);
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 99);
}

TEST_CASE_METHOD(OverlayCommitFixture,
                 "handle_save clears active spool when spool unlinked (N -> 0)",
                 "[ams_edit_overlay][spoolman][active_spool]") {
    // The pre-edit assignment was persisted (that is how production gets
    // here) — the commit's clear arm fires on the stored link, not on the
    // overlay's in-memory original.
    SlotInfo seeded;
    seeded.spoolman_id = 42;
    AmsState::instance().set_external_spool_info(seeded);

    api.spoolman_mock().set_active_spool(42, nullptr, nullptr);

    AmsEditOverlay overlay;
    AmsEditOverlayTestAccess access(overlay);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 42;

    SlotInfo working;
    working.spoolman_id = 0;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditOverlay::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
        AmsState::instance().commit_external_spool_edit(result.slot_info);
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE_METHOD(OverlayCommitFixture, "handle_save re-syncs active spool on unchanged linked save",
                 "[ams_edit_overlay][spoolman][active_spool]") {
    // Simulate Moonraker having lost the active-spool state (e.g. after restart).
    api.spoolman_mock().set_active_spool(7, nullptr, nullptr);

    AmsEditOverlay overlay;
    AmsEditOverlayTestAccess access(overlay);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 42;

    SlotInfo working;
    working.spoolman_id = 42; // Same spool — re-save, not a change

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditOverlay::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
        AmsState::instance().commit_external_spool_edit(result.slot_info);
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    // Re-save always re-syncs so Moonraker recovers lost state.
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 42);
}

TEST_CASE_METHOD(OverlayCommitFixture, "handle_save does NOT crash when no API available",
                 "[ams_edit_overlay][spoolman][active_spool]") {
    // Neither the overlay nor AmsState holds an API — the commit's S1 arm is
    // skipped, the local stores still update.
    AmsState::instance().set_moonraker_api(nullptr);

    AmsEditOverlay overlay;
    AmsEditOverlayTestAccess access(overlay);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 0;

    SlotInfo working;
    working.spoolman_id = 42;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(nullptr);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditOverlay::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
        AmsState::instance().commit_external_spool_edit(result.slot_info);
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();
    REQUIRE(completion_fired);
}

// The backend-slot route: an overlay save for slot 0 must land BOTH the
// backend slot info AND the server active spool through the completion
// consumer's commit_slot_edit — the overlay's old pre-fire is gone.
TEST_CASE_METHOD(OverlayCommitFixture,
                 "overlay save commits backend slot and active spool via consumer commit",
                 "[ams_edit_overlay][spoolman][active_spool][commit]") {
    install_backend(0); // slot 0 starts unlinked

    AmsEditOverlay overlay;
    AmsEditOverlayTestAccess access(overlay);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 0;

    SlotInfo working;
    working.spoolman_id = 169;
    working.material = "PLA";

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(0);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditOverlay::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
        REQUIRE(result.slot_index == 0);
        // Mirrors the AmsPanel / AmsOverviewPanel completion lambda: capture
        // the pre-edit slot BEFORE the commit (its unlink arm needs it), then
        // route through the single commit path.
        AmsBackend* commit_backend = AmsState::instance().get_backend();
        REQUIRE(commit_backend);
        SlotInfo pre_edit = commit_backend->get_slot_info(result.slot_index);
        AmsError err =
            AmsState::instance().commit_slot_edit(result.slot_index, pre_edit, result.slot_info);
        REQUIRE(err.success());
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    // S3 — the backend slot got the link...
    REQUIRE(backend->get_slot_info(0).spoolman_id == 169);
    // S1 — ...AND the server active spool was registered by the commit.
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 169);
}

// ============================================================================
// Tests: completion is fired exactly once (covered-vs-dismissed correctness)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "fire_completion is idempotent via completion_fired_",
                 "[ams_edit_overlay][lifecycle]") {
    AmsEditOverlay overlay;
    AmsEditOverlayTestAccess access(overlay);
    access.init_subjects();

    SlotInfo info;
    access.set_original_info(info);
    access.set_working_info(info);
    access.set_api(nullptr);
    access.set_slot_index(0);

    int fire_count = 0;
    access.set_completion_callback([&](const AmsEditOverlay::EditResult&) { fire_count++; });

    // Save fires completion; the overlay-close safety net firing afterwards
    // (backdrop tap path) must be a no-op.
    access.call_handle_save();
    UpdateQueue::instance().drain();
    REQUIRE(fire_count == 1);
    REQUIRE(access.completion_fired());

    access.call_handle_back(); // back after completion: must not re-fire
    UpdateQueue::instance().drain();
    REQUIRE(fire_count == 1);
}

// ============================================================================
// Tests: #1071 create-gate + identity-change predicates (unchanged semantics
// in this phase — Phase 5 rewrites the gate to the Save-to-Spoolman toggle)
// ============================================================================

TEST_CASE("AmsEditOverlay::should_create_new_spool is gated by the Save-to-Spoolman toggle",
          "[ams_edit_overlay][spoolman][toggle]") {
    // Complete untracked metadata — creation now requires the EXPLICIT opt-in
    // (spec §3.3 replaces the silent auto-create / user-edit heuristic).
    SlotInfo working;
    working.spoolman_id = 0;
    working.brand = "Generic";
    working.material = "PLA";
    working.color_rgb = 0xFF0000;
    REQUIRE(helix::SpoolmanSlotSaver::is_filament_complete(working));

    // Toggle off -> never create, no matter how complete the fields are.
    CHECK_FALSE(AmsEditOverlayTestAccess::should_create_new_spool(working,
                                                                  /*save_to_spoolman=*/false));

    // Toggle on + complete + unlinked -> create.
    CHECK(AmsEditOverlayTestAccess::should_create_new_spool(working, /*save_to_spoolman=*/true));

    // Already linked -> update path, never create.
    SlotInfo linked = working;
    linked.spoolman_id = 99;
    CHECK_FALSE(AmsEditOverlayTestAccess::should_create_new_spool(linked, true));

    // Incomplete metadata never creates, even opted-in.
    SlotInfo incomplete;
    incomplete.spoolman_id = 0;
    incomplete.material = "PLA"; // no brand, default color
    REQUIRE_FALSE(helix::SpoolmanSlotSaver::is_filament_complete(incomplete));
    CHECK_FALSE(AmsEditOverlayTestAccess::should_create_new_spool(incomplete, true));
}

// Weight staging on the untracked branch of handle_spool_edit_save.
//
// The original guard skipped staging entirely when the editor was OPENED on a
// linked slot, because detail_working_.spool_weight_g is then Spoolman's
// empty-spool CORE weight (~190g), not the filament total — staging it would
// clobber a correct 1000g total_weight_g.
//
// But that guard threw out remaining_weight_g too, and remaining is NOT
// ambiguous: it means the same thing whether or not the slot arrived linked.
// Since AFC's SET_WEIGHT is gated on remaining_weight_g > 0, dropping it meant
// unlinking-and-entering-a-weight in one save emitted no SET_WEIGHT at all, and
// the user had to reopen and save a second time. Observed on the .112 BoxTurtle:
// save at 19:13:24 emitted SET_COLOR + SET_MATERIAL and no SET_WEIGHT; the
// weight only landed on the following save at 19:13:40.
TEST_CASE("AmsEditOverlay::decide_weight_staging stages remaining even when unlinking in place",
          "[ams][edit_overlay][spoolman]") {
    SECTION("genuinely untracked slot stages both fields") {
        auto s = AmsEditOverlayTestAccess::decide_weight_staging(
            /*entered_tracked=*/false, /*remaining_filled=*/true, /*total_filled=*/true);
        CHECK(s.stage_remaining);
        CHECK(s.stage_total);
    }

    SECTION("unlinked-in-place stages remaining but NOT total (core-weight ambiguity)") {
        auto s = AmsEditOverlayTestAccess::decide_weight_staging(
            /*entered_tracked=*/true, /*remaining_filled=*/true, /*total_filled=*/true);
        CHECK(s.stage_remaining); // the bug: this was false, so SET_WEIGHT never fired
        CHECK_FALSE(s.stage_total);
    }

    SECTION("blank fields stage nothing (blank means unchanged)") {
        auto s = AmsEditOverlayTestAccess::decide_weight_staging(
            /*entered_tracked=*/false, /*remaining_filled=*/false, /*total_filled=*/false);
        CHECK_FALSE(s.stage_remaining);
        CHECK_FALSE(s.stage_total);
    }

    SECTION("blank remaining is not staged even on a genuinely untracked slot") {
        auto s = AmsEditOverlayTestAccess::decide_weight_staging(
            /*entered_tracked=*/false, /*remaining_filled=*/false, /*total_filled=*/true);
        CHECK_FALSE(s.stage_remaining);
        CHECK(s.stage_total);
    }
}

// The logistics two-PATCH in handle_spool_edit_save() ran BEFORE
// commit_and_close() evaluated needs_identity_confirmation(), so a save that was
// about to ask "Different filament?" had already written to Spoolman by the time
// the dialog appeared — and Cancel, documented as a true abort, could not take it
// back. Observed on the .112 BoxTurtle:
//
//   19:11:55  Updating spool 86 with 1 fields
//   19:11:55  Spool 86 updated successfully
//   19:11:55  set_active_spool(86)
//   19:11:56  Confirmation dialog shown: 'Different filament?'
//   19:12:17  fire_completion saved=false        <- user cancelled
//
// Invariant: if the edit will prompt, nothing may be written first.
TEST_CASE("AmsEditOverlay::may_write_spoolman_now withholds writes until identity is confirmed",
          "[ams][edit_overlay][spoolman]") {
    SlotInfo linked;
    linked.spoolman_id = 86;
    linked.brand = "Likesilk";
    linked.material = "ASA";
    linked.color_rgb = 0x1A1A1A;
    linked.remaining_weight_g = 509.0f;

    SECTION("materially different identity must not write before the prompt") {
        SlotInfo edited = linked;
        edited.material = "PLA";
        edited.color_rgb = 0xE53935;

        REQUIRE(AmsEditOverlayTestAccess::needs_identity_confirmation(linked, edited));
        CHECK_FALSE(AmsEditOverlayTestAccess::may_write_spoolman_now(linked, edited));
    }

    SECTION("weight-only edit on a linked spool writes immediately, no prompt") {
        SlotInfo edited = linked;
        edited.remaining_weight_g = 400.0f;

        REQUIRE_FALSE(AmsEditOverlayTestAccess::needs_identity_confirmation(linked, edited));
        CHECK(AmsEditOverlayTestAccess::may_write_spoolman_now(linked, edited));
    }

    SECTION("unlinked slot has no spool to clobber, so writes are always allowed") {
        SlotInfo untracked;
        untracked.spoolman_id = 0;
        untracked.material = "PLA";
        untracked.color_rgb = 0xE53935;

        SlotInfo edited = untracked;
        edited.material = "PETG";

        CHECK(AmsEditOverlayTestAccess::may_write_spoolman_now(untracked, edited));
    }

    SECTION("unchanged linked spool writes immediately") {
        CHECK(AmsEditOverlayTestAccess::may_write_spoolman_now(linked, linked));
    }
}

// Unlink zeroed only spoolman_id, leaving spoolman_filament_id and
// spoolman_vendor_id behind. Those stale ids then fed the repoint decision in
// SpoolmanSlotSaver (`filament_id == original_filament_id` -> skip repoint), so
// a later edit could compare against a filament belonging to a spool the lane
// is no longer linked to.
TEST_CASE("SlotInfo::clear_spoolman_link clears the whole Spoolman identity",
          "[ams][edit_overlay][spoolman]") {
    SlotInfo slot;
    slot.spoolman_id = 86;
    slot.spoolman_filament_id = 79;
    slot.spoolman_vendor_id = 22;
    slot.spool_name = "Black ASA";
    slot.brand = "Likesilk";
    slot.material = "ASA";
    slot.color_rgb = 0x1A1A1A;
    slot.remaining_weight_g = 509.0f;
    slot.catalog_id = "sunlu-pla-plus-2-0";
    slot.product_name = "PLA+ 2.0";

    slot.clear_spoolman_link();

    CHECK(slot.spoolman_id == 0);
    CHECK(slot.spoolman_filament_id == 0);
    CHECK(slot.spoolman_vendor_id == 0);
    CHECK(slot.spool_name.empty());

    // Identity the user can still see and edit locally is deliberately KEPT —
    // unlinking is "stop tracking this in Spoolman", not "forget the filament".
    CHECK(slot.brand == "Likesilk");
    CHECK(slot.material == "ASA");
    CHECK(slot.color_rgb == 0x1A1A1A);
    CHECK(slot.remaining_weight_g == Catch::Approx(509.0f));

    // The catalog pick is local identity, not a Spoolman handle — the product
    // came from assets/filaments.json and has no Spoolman record behind it.
    // Clearing it on unlink would drop the user back to the alphabetically-first
    // variant on the next open, which is the bug this field exists to fix.
    CHECK(slot.catalog_id == "sunlu-pla-plus-2-0");
    CHECK(slot.product_name == "PLA+ 2.0");
}

TEST_CASE("AmsEditOverlay::needs_identity_confirmation applies to ALL Spoolman backends (§6)",
          "[ams_edit_overlay][spoolman][identity_confirm]") {
    SlotInfo original;
    original.spoolman_id = 42;
    original.material = "PLA";
    original.color_rgb = 0xFF0000;
    original.remaining_weight_g = 500.0f;

    // Linked + material identity change -> confirm (no AD5X gate anymore).
    SlotInfo diff_mat = original;
    diff_mat.material = "PETG";
    CHECK(AmsEditOverlayTestAccess::needs_identity_confirmation(original, diff_mat));

    // Linked + far-apart color -> confirm.
    SlotInfo diff_color = original;
    diff_color.color_rgb = 0x0000FF;
    CHECK(AmsEditOverlayTestAccess::needs_identity_confirmation(original, diff_color));

    // Linked + same identity (weight-only edit) -> no confirm.
    SlotInfo weight_only = original;
    weight_only.remaining_weight_g = 400.0f;
    CHECK_FALSE(AmsEditOverlayTestAccess::needs_identity_confirmation(original, weight_only));

    // Unlinked working slot -> no linked spool to clobber, no confirm.
    SlotInfo unlinked = diff_mat;
    unlinked.spoolman_id = 0;
    CHECK_FALSE(AmsEditOverlayTestAccess::needs_identity_confirmation(original, unlinked));

    // No changes at all -> no confirm.
    SlotInfo same = original;
    CHECK_FALSE(AmsEditOverlayTestAccess::needs_identity_confirmation(original, same));
}

TEST_CASE("AmsEditOverlay::is_material_identity_change flags different-spool edits (#1071)",
          "[ams_edit_overlay][spoolman][1071]") {
    SlotInfo original;
    original.material = "PLA";
    original.color_rgb = 0xFF0000;

    SlotInfo same = original;
    CHECK_FALSE(AmsEditOverlayTestAccess::is_material_identity_change(original, same));

    SlotInfo nudged = original;
    nudged.color_rgb = 0xFE0101;
    CHECK_FALSE(AmsEditOverlayTestAccess::is_material_identity_change(original, nudged));

    SlotInfo recased = original;
    recased.material = "pla";
    CHECK_FALSE(AmsEditOverlayTestAccess::is_material_identity_change(original, recased));

    SlotInfo diff_mat = original;
    diff_mat.material = "PETG";
    CHECK(AmsEditOverlayTestAccess::is_material_identity_change(original, diff_mat));

    SlotInfo diff_color = original;
    diff_color.color_rgb = 0x0000FF;
    CHECK(AmsEditOverlayTestAccess::is_material_identity_change(original, diff_color));
}

// ============================================================================
// Tests: #1071 initial-view routing on the singleton overlay. show_for_slot
// pushes via NavigationManager (async) — drain before asserting, and pop the
// overlay afterwards so the next test starts clean.
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "show_for_slot opens on the Spoolman picker when requested (#1071)",
                 "[ams_edit_overlay][spoolman][ui_integration][1071]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayTestAccess access(overlay);

    SlotInfo info;
    info.slot_index = 0;

    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, api(), nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.get_view_mode() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture, "show_for_slot opens on the overview by default (#1071)",
                 "[ams_edit_overlay][spoolman][ui_integration][1071]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayTestAccess access(overlay);

    SlotInfo info;
    info.slot_index = 0;

    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, api(), nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.get_view_mode() == AmsEditOverlay::VIEW_OVERVIEW);

    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(10);
}
