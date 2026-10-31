// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// View-level tests for the AMS slot editor overlay redesign: identity dropdown
// contents (tracked vs untracked), managed-state subject, pre-selection, and
// view transitions. Uses the full-UI fixture (real XML tree) plus the
// AmsEditOverlayViewTestAccess friend shim.

#include "ui_ams_edit_overlay.h"
#include "ui_ams_lane_settings.h"
#include "ui_component_keypad.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_swatch.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/scoped_platform_tier.h"
#include "../test_helpers/widget_in_tree.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "display_numbering.h"
#include "display_settings_manager.h"
#include "filament_favorites.h"
#include "filament_op_dispatch.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"
#include "src/ui/panel_widgets/active_spool_widget.h"
#include "text_io.h"
#include "tool_state.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::ui;

class AmsEditOverlayViewTestAccess {
  public:
    explicit AmsEditOverlayViewTestAccess(AmsEditOverlay& overlay) : overlay_(overlay) {}

    lv_obj_t* widget(const char* name) {
        return overlay_.find_widget(name);
    }
    void set_working_info(const SlotInfo& info) {
        overlay_.working_info_ = info;
    }
    void call_update_ui() {
        overlay_.update_ui();
    }
    int view() {
        return lv_subject_get_int(&overlay_.view_mode_subject_);
    }
    void call_set_view(int v) {
        overlay_.set_view(v);
    }
    int is_managed() {
        return lv_subject_get_int(&overlay_.is_managed_subject_);
    }
    int clear_visible() {
        return lv_subject_get_int(&overlay_.clear_visible_subject_);
    }
    void toggle_more() {
        overlay_.handle_more_toggle();
    }
    void set_cached_spools(std::vector<SpoolInfo> spools) {
        overlay_.cached_spools_ = std::move(spools);
        // Mirror populate_picker's precompute so direct-drive tests exercise
        // the same filter path as production.
        overlay_.cached_searchables_.clear();
        overlay_.cached_searchables_.reserve(overlay_.cached_spools_.size());
        for (const auto& spool : overlay_.cached_spools_) {
            overlay_.cached_searchables_.push_back(build_searchable_text(spool));
        }
    }
    void call_render_spool_list(const std::string& filter) {
        overlay_.render_spool_list(filter);
    }
    int picker_state() {
        return lv_subject_get_int(&overlay_.picker_state_subject_);
    }
    /// What the XML value_changed callback does for each keystroke.
    void type_picker_search(const std::string& text) {
        overlay_.picker_search_debounce_.schedule(text);
    }
    bool picker_search_pending() {
        return overlay_.picker_search_debounce_.pending();
    }
    void call_enter_editor() {
        overlay_.enter_editor();
    }
    void call_handle_save() {
        overlay_.handle_save();
    }
    void call_switch_to_picker() {
        overlay_.switch_to_picker();
    }
    void call_handle_spool_selected(int spool_id) {
        overlay_.handle_spool_selected(spool_id);
    }
    void call_switch_to_editor() {
        overlay_.set_view(AmsEditOverlay::kViewEditor);
    }
    /// Pick @p option in the named dropdown the way a touch does: set the
    /// widget's selection and send it value_changed, so the XML-registered
    /// callback runs. False when the option is not in the list.
    bool pick_dropdown(const char* name, const std::string& option) {
        lv_obj_t* dd = overlay_.find_widget(name);
        REQUIRE(dd != nullptr);
        const std::string options = lv_dropdown_get_options(dd);
        int index = 0;
        size_t at = 0;
        while (true) {
            const size_t nl = options.find('\n', at);
            if (options.substr(at, nl == std::string::npos ? nl : nl - at) == option) {
                lv_dropdown_set_selected(dd, static_cast<uint32_t>(index));
                lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
                return true;
            }
            if (nl == std::string::npos) {
                return false;
            }
            at = nl + 1;
            ++index;
        }
    }
    AmsEditOverlay::SpoolEditSelection sel() {
        return overlay_.sel_;
    }
    void set_sel(const AmsEditOverlay::SpoolEditSelection& sel) {
        overlay_.sel_ = sel;
        overlay_.rebuild_option_lists();
    }
    void call_apply_weight(float grams) {
        overlay_.apply_weight(grams);
    }
    void set_tag(const helix::ams::TagReading& tag) {
        overlay_.tag_ = tag;
    }
    void call_handle_reset_to_tag() {
        overlay_.handle_reset_to_tag();
    }
    void call_handle_add_to_spoolman() {
        overlay_.handle_add_to_spoolman();
    }
    /// Type into a More field: lv_textarea_set_text sends value_changed, so
    /// the XML-registered callback does the rest.
    void set_detail_field(const char* name, const char* text) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        lv_textarea_set_text(w, text);
    }
    /// Shown on screen: the widget and every ancestor up to the overlay root
    /// are free of the hidden flag.
    bool shown(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        for (lv_obj_t* o = w; o && o != overlay_.overlay_root_; o = lv_obj_get_parent(o)) {
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
                return false;
            }
        }
        return true;
    }
    std::string detail_comment() const {
        return overlay_.detail_working_.comment;
    }
    int detail_id() const {
        return overlay_.detail_working_.id;
    }
    std::string dropdown_options(const char* name) {
        lv_obj_t* dd = overlay_.find_widget(name);
        return dd ? lv_dropdown_get_options(dd) : std::string();
    }
    /// The "Browse catalog..." sheet is up.
    bool catalog_sheet_visible() const {
        return overlay_.catalog_sheet_ && overlay_.catalog_sheet_->is_visible();
    }
    void hide_catalog_sheet() {
        if (catalog_sheet_visible()) {
            overlay_.catalog_sheet_->hide();
        }
    }
    std::string dropdown_selected(const char* name) {
        lv_obj_t* dd = overlay_.find_widget(name);
        if (!dd) {
            return {};
        }
        char buf[64] = {0};
        lv_dropdown_get_selected_str(dd, buf, sizeof(buf) - 1);
        return buf;
    }
    void set_details_color(uint32_t rgb) {
        overlay_.details_color_ = rgb;
        overlay_.details_color_set_ = true;
    }
    SlotInfo working_info() {
        return overlay_.working_info_;
    }
    // Simulate the async Spoolman fetch in enter_spool_edit() that overwrites
    // detail_original_/detail_working_ wholesale with the fetched record
    // (spool_weight_g = empty-spool CORE weight, not the filament total) and
    // repopulates the on-screen fields — exactly what the fetch callback does.
    void seed_detail_fetch(const SpoolInfo& spool) {
        overlay_.detail_original_ = spool;
        overlay_.detail_working_ = spool;
        overlay_.populate_detail_fields();
    }
    bool is_dirty() {
        return overlay_.is_dirty();
    }
    static bool save_is_disabled(int view, bool save_in_flight, bool dirty) {
        return AmsEditOverlay::save_is_disabled(view, save_in_flight, dirty);
    }
    int save_disabled() {
        return lv_subject_get_int(&overlay_.save_disabled_subject_);
    }
    /// What the identity prompt's "It's a new spool" button runs.
    void call_do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent intent) {
        overlay_.do_spoolman_save(intent);
    }
    void call_repopulate() {
        overlay_.repopulate();
    }
    void call_open_color_view() {
        overlay_.open_color_view();
    }
    void call_apply_color(uint32_t rgb) {
        overlay_.apply_color(rgb);
    }
    uint32_t details_color() {
        return overlay_.details_color_;
    }
    bool details_color_set() {
        return overlay_.details_color_set_;
    }
    void call_update_tag_badges() {
        overlay_.update_tag_badges();
    }
    /// The touch box: the widget plus its ext_click_area, after layout.
    lv_area_t click_area(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        lv_obj_update_layout(w);
        lv_area_t a;
        lv_obj_get_click_area(w, &a);
        return a;
    }
    /// What NavigationManager's close callback runs when something other than
    /// the editor closes it (a navbar tap).
    void call_fire_completion(bool saved) {
        overlay_.fire_completion(saved);
    }
    bool has_tag() const {
        return overlay_.tag_.has_value();
    }
    void call_handle_back() {
        overlay_.handle_back();
    }
    void call_handle_weight_tap() {
        overlay_.handle_weight_tap();
    }
    void set_save_in_flight(bool in_flight) {
        overlay_.save_in_flight_ = in_flight;
        overlay_.update_sync_button_state();
    }
    /// What a cover (keypad, scanner pushed on top) does to screen-scoped work.
    void drop_screen_lifetime() {
        overlay_.lifetime_.invalidate();
    }
    std::string field_text(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        const char* t = lv_textarea_get_text(w);
        return t ? t : "";
    }
    void click(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        lv_obj_send_event(w, LV_EVENT_CLICKED, nullptr);
    }
    static void build_spool_patches(const SpoolInfo& original, const SpoolInfo& edited,
                                    nlohmann::json& spool_patch, nlohmann::json& filament_patch) {
        SpoolmanSlotSaver::build_spool_patches(original, edited, spool_patch, filament_patch);
    }

  private:
    AmsEditOverlay& overlay_;
};

namespace {

void close_editor_overlay() {
    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    lv_timer_handler(); // the overlay close callback runs on the next tick
}

SlotInfo untracked_slot() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 0;
    info.brand = "Generic";
    info.material = "PETG";
    info.color_rgb = 0xFF6600;
    info.color_name = "Orange";
    return info;
}

SlotInfo tracked_slot() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 7;
    info.brand = "Bambu Lab";
    info.material = "ASA";
    info.spool_name = "Bambu Lab ASA";
    info.color_rgb = 0x8A949E;
    info.color_name = "Gray ASA";
    return info;
}

// A slot as apply_spool_to_slot() leaves it: spool_name is the Spoolman
// filament name alone, with brand and material in their own fields. Nothing
// about the name repeats the other two, so the summary has to join all three.
SlotInfo tracked_slot_spoolman_named() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 42;
    info.brand = "Polymaker";
    info.material = "PLA";
    info.spool_name = "Ambrosia Pink";
    info.color_rgb = 0xFFB6C1;
    return info;
}

// A Spoolman-linked lane on AFC older than v1.2.0: the firmware publishes
// spool_id but no filament_name, so the slot carries the link and nothing to
// name it with. Seen live on a BoxTurtle at 192.168.1.112 (spool #106).
SlotInfo tracked_slot_named_only_in_cache() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 106;
    info.material = "PLA";
    info.color_rgb = 0x333333;
    // brand and spool_name deliberately empty — only the cache knows them.
    return info;
}

// Copy of untracked_slot() with weights explicitly marked unknown (-1
// sentinel, also SlotInfo's default) — models a slot with no Spoolman/manual
// weight data on record.
SlotInfo untracked_slot_without_weights() {
    SlotInfo info = untracked_slot();
    info.total_weight_g = -1.0f;
    info.remaining_weight_g = -1.0f;
    return info;
}

// Mirrors the show_for_slot() + drain + process_lvgl sequence used by every
// test in this file, seeded with a weightless slot. Takes the fixture so it
// can reach test_screen()/process_lvgl() (both LVGLUITestFixture members).
// TEST_MIRROR_OK: this helper CALLS the production show_for_slot() (see the
//                 REQUIRE below) — it factors out the repeated call+pump
//                 sequence, it does not reimplement what show_for_slot() does.
void show_overlay_for_mock_slot_without_weights(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, untracked_slot_without_weights(),
                                  nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

void show_overlay_for_mock_tracked_slot(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    // api=nullptr keeps the async Spoolman re-fetch out of the picture.
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

void show_overlay_for_mock_untracked_slot(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

/// The linked spool both refetch cases start from, on the server and on the
/// backend's slot 0. The mock inventory already carries id 7 and a GET serves
/// the first match, so this states the fields on that record rather than
/// pushing a second one the reads would never reach.
void seed_linked_spool(MoonrakerAPIMock& api, MoonrakerClientMock& client,
                       AmsBackendMock& backend) {
    auto& spools = client.spoolman_mock().get_mock_spools();
    auto it =
        std::find_if(spools.begin(), spools.end(), [](const SpoolInfo& s) { return s.id == 7; });
    if (it == spools.end()) {
        SpoolInfo added;
        added.id = 7;
        spools.push_back(added);
        it = std::prev(spools.end());
    }
    it->filament_id = 3;
    it->vendor = "Bambu Lab";
    it->material = "ASA";
    it->color_hex = "8A949E";
    it->initial_weight_g = 1000.0;
    it->remaining_weight_g = 850.0;
    backend.sync_external_identity(0, tracked_slot());

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    SpoolmanManager::instance().set_api(&api);
    UpdateQueue::instance().drain();
}

/// Fill the lane's Spoolman record from the server, so a case measures the
/// save's own read rather than the first record the lane ever gets.
void poll_lane_once(helix::ams::LaneId lane) {
    SpoolmanManager::instance().refresh_spoolman_weights();
    UpdateQueue::instance().drain();
    REQUIRE(helix::ams::lane_sources(lane).spoolman.has_value());
    REQUIRE(helix::ams::lane_sources(lane).spoolman->brand == "Bambu Lab");
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "show_for_slot skips the identity-sync fetch when Spoolman is unavailable",
                 "[ams_edit_overlay][card][spoolman]") {
    // A Spoolman-less printer must not fire a doomed identity-sync fetch (one
    // RPC + warn log) every time the slot editor opens. show_for_slot() gates
    // the open-time re-fetch on printer_has_spoolman, matching enter_spool_edit.
    //
    // The mock is fully ENABLED here, so if the fetch fired it would return
    // authoritative data and overwrite working_info_ (brand/material). The gate
    // under test is the UI-side availability subject, not the mock's own flag —
    // so an untouched working_info_ proves the fetch never ran.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    client.set_mock_spoolman_enabled(true);

    // Authoritative record whose vendor/material differ from the slot's initial
    // values, so a fired fetch is observable as a working_info_ change.
    client.spoolman_mock().get_mock_spools().clear();
    SpoolInfo authoritative;
    authoritative.id = 7;
    authoritative.vendor = "SpoolmanVendor";
    authoritative.material = "SpoolmanPETG";
    client.spoolman_mock().get_mock_spools().push_back(authoritative);

    SlotInfo slot = tracked_slot(); // spoolman_id = 7
    slot.brand = "SlotBrand";
    slot.material = "SlotPLA";

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SECTION("Spoolman unavailable -> no fetch, working_info_ untouched") {
        lv_subject_set_int(spoolman_subj, 0);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.working_info().brand == "SlotBrand");
        CHECK(access.working_info().material == "SlotPLA");

        close_editor_overlay();
    }

    SECTION("Spoolman available -> fetch fires, working_info_ synced") {
        lv_subject_set_int(spoolman_subj, 1);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.working_info().brand == "SpoolmanVendor");
        CHECK(access.working_info().material == "SpoolmanPETG");

        close_editor_overlay();
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker no longer offers a standalone unlink entry",
                 "[ams_edit_overlay][spool_edit][picker]") {
    // The picker_unlink_entry is retired — the single unlink path is the Save
    // Save with the toggle off (covered above).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_tracked_slot(*this);
    access.call_switch_to_picker();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.widget("picker_unlink_entry") == nullptr);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "color view from spool-edit stages the pending color and returns there",
                 "[ams_edit_overlay][color_view]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_open_color_view();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.view() == AmsEditOverlay::kViewColor);

    uint32_t before = access.working_info().color_rgb;
    access.call_apply_color(0x1E88E5);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::kViewEditor);
    CHECK(access.details_color() == 0x1E88E5);
    CHECK(access.details_color_set());
    CHECK(access.working_info().color_rgb == before); // slot untouched until Save

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "custom color view opens on the slot's black, not the grey sentinel",
                 "[ams_edit_overlay][color][1608]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo black = untracked_slot();
    black.color_rgb = 0x000000;
    black.color_name = "Black";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, black, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_open_color_view();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* hex_input = access.widget("ams_color_hex_input");
    REQUIRE(hex_input != nullptr);
    CHECK(std::string(lv_textarea_get_text(hex_input)) == "#000000");

    close_editor_overlay();

    // A sentinel slot keeps its grey starting point: the picker needs some
    // seed, and the sentinel grey is what the slot's own summary already shows.
    SlotInfo sentinel = untracked_slot();
    sentinel.color_rgb = kAmsDefaultSlotColor;
    sentinel.color_name.clear();
    REQUIRE(overlay.show_for_slot(test_screen(), 0, sentinel, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_open_color_view();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // close_editor_overlay() tears the widget tree down, so re-find the input
    // in the rebuilt tree rather than reusing the pointer from the first half.
    hex_input = access.widget("ams_color_hex_input");
    REQUIRE(hex_input != nullptr);
    CHECK(std::string(lv_textarea_get_text(hex_input)) == "#808080");

    close_editor_overlay();
}

namespace {
std::vector<SpoolInfo> two_spools() {
    SpoolInfo a;
    a.id = 11;
    a.vendor = "Polymaker";
    a.material = "PLA";
    a.color_hex = "1A1A2E";
    SpoolInfo b;
    b.id = 22;
    b.vendor = "eSUN";
    b.material = "PETG";
    b.color_hex = "00FF00";
    return {a, b};
}
} // namespace

// ============================================================================
// OverlayConsumerCommitFixture — for tests whose completion callback must
// mirror the production consumers (AmsPanel / AmsOverviewPanel): the overlay
// no longer pre-fires the server active-spool sync, so the consumer's
// commit_slot_edit() owns it, and the wiring that commit needs (a backend +
// the API registered on AmsState) has to exist in the test too. Wiring shape
// mirrors OverlayCommitFixture (test_ams_edit_overlay_active_spool.cpp) /
// CommitFixture (test_ams_state_commit_slot.cpp); no Config isolation —
// commit_slot_edit persists nothing (AmsBackendMock ignores the persist flag).
// ============================================================================

struct OverlayConsumerCommitFixture : LVGLUITestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    AmsBackendMock* backend = nullptr;

    OverlayConsumerCommitFixture() : api(client, get_printer_state()) {
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

        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        ams.set_backend(std::move(owned));
        ams.set_moonraker_api(&api);
    }

    ~OverlayConsumerCommitFixture() override {
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
        // The manager holds a raw pointer, and this fixture's api outlives it
        // by one member destruction at most.
        SpoolmanManager::instance().set_api(nullptr);
    }

    /// The production backend-slot completion-consumer body (AmsPanel /
    /// AmsOverviewPanel): capture the pre-edit slot BEFORE the commit — its
    /// unlink arm (clear the server active spool) needs the old link.
    void commit_like_consumer(const AmsEditOverlay::EditResult& r) {
        if (!r.saved || r.slot_index < 0)
            return;
        AmsBackend* commit_backend = AmsState::instance().get_backend();
        REQUIRE(commit_backend);
        SlotInfo original = commit_backend->get_slot_info(r.slot_index);
        AmsError err = AmsState::instance().commit_slot_edit(r.slot_index, original, r.slot_info);
        REQUIRE(err.success());
    }
};

TEST_CASE_METHOD(LVGLUITestFixture, "picker pre-selects the first row for unlinked slots",
                 "[ams_edit_overlay][picker][preselect]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    CHECK(lv_obj_has_state(lv_obj_get_child(list, 0), LV_STATE_CHECKED));
    CHECK_FALSE(lv_obj_has_state(lv_obj_get_child(list, 1), LV_STATE_CHECKED));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker row names a spool by its filament name",
                 "[ams_edit_overlay][picker][spool_label]") {
    // Two spools of one vendor and material differ only by Spoolman's filament
    // name, so the row's primary label (the only text a compact row shows)
    // has to carry it.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    SpoolInfo clear;
    clear.id = 12;
    clear.vendor = "Kingroon";
    clear.material = "PETG";
    clear.filament_name = "Kingroon Basic PETG Clear";
    clear.remaining_weight_g = 750;
    SpoolInfo white = clear;
    white.id = 13;
    white.filament_name = "Kingroon Basic PETG White";
    SpoolInfo unnamed;
    unnamed.id = 14;
    unnamed.vendor = "Polymaker";
    unnamed.material = "PLA";
    access.set_cached_spools({clear, white, unnamed});

    // Medium renders compact single-line rows, Large the two-line rich rows.
    const UiBreakpoint bp = GENERATE(UiBreakpoint::Medium, UiBreakpoint::Large);
    CAPTURE(to_int(bp));
    helix::test::ScopedBreakpoint scoped_bp(bp);
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 3);

    const auto row_for = [list](int spool_id) -> lv_obj_t* {
        for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
            lv_obj_t* row = lv_obj_get_child(list, static_cast<int32_t>(i));
            if (reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)) == spool_id) {
                return row;
            }
        }
        return nullptr;
    };
    const auto name_of = [&row_for](int spool_id) -> std::string {
        lv_obj_t* row = row_for(spool_id);
        REQUIRE(row != nullptr);
        lv_obj_t* label = lv_obj_find_by_name(row, "spool_name");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    };

    // The vendor and material the filament name already contains are not repeated.
    CHECK(name_of(12) == "#12 Kingroon Basic PETG Clear");
    CHECK(name_of(13) == "#13 Kingroon Basic PETG White");
    CHECK(name_of(14) == "#14 Polymaker PLA");

    // The row prints the filament name once, whatever the layout.
    const auto visible_text = [](lv_obj_t* root) {
        std::string text;
        std::vector<lv_obj_t*> stack{root};
        while (!stack.empty()) {
            lv_obj_t* obj = stack.back();
            stack.pop_back();
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            if (lv_obj_check_type(obj, &lv_label_class)) {
                text += lv_label_get_text(obj);
                text += '\n';
            }
            for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
                stack.push_back(lv_obj_get_child(obj, static_cast<int32_t>(i)));
            }
        }
        return text;
    };
    const auto occurrences = [](const std::string& haystack, const std::string& needle) {
        size_t count = 0;
        for (size_t pos = haystack.find(needle); pos != std::string::npos;
             pos = haystack.find(needle, pos + needle.size())) {
            ++count;
        }
        return count;
    };
    CHECK(occurrences(visible_text(row_for(12)), "Basic PETG Clear") == 1);
    CHECK(occurrences(visible_text(row_for(13)), "Basic PETG White") == 1);

    // A label that holds the name but lays out zero-sized shows nothing.
    lv_obj_update_layout(list);
    lv_obj_t* label = lv_obj_find_by_name(row_for(12), "spool_name");
    CHECK(lv_obj_get_height(label) > 0);
    CHECK(lv_obj_get_width(label) > 0);
    // The name leaves room for the weight beside or below it, inside the
    // details box that holds them both.
    lv_obj_t* weight = lv_obj_find_by_name(row_for(12), "spool_weight");
    lv_area_t details_area;
    lv_area_t weight_area;
    lv_obj_get_coords(lv_obj_get_parent(lv_obj_get_parent(weight)), &details_area);
    lv_obj_get_coords(weight, &weight_area);
    CHECK(weight_area.x2 <= details_area.x2);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker search burst renders once, after the pause",
                 "[ams_edit_overlay][picker][search]") {
    // Regression: every value_changed used to rebuild the whole spool list
    // synchronously - at a few hundred spools that froze the keyboard for the
    // length of the rebuild, per keystroke (K2 Plus report 2026-08-30).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);

    // Type a burst. Nothing may render synchronously or mid-burst: the list
    // keeps the previous content and the debounce stays armed.
    access.type_picker_search("e");
    access.type_picker_search("es");
    access.type_picker_search("esu");
    access.type_picker_search("esun");
    UpdateQueue::instance().drain();
    process_lvgl(10); // well under the 300ms delay
    CHECK(access.picker_search_pending());
    CHECK(lv_obj_get_child_count(list) == 2);

    // After the pause: ONE render with the final query ("esun" -> eSUN only).
    process_lvgl(320);
    UpdateQueue::instance().drain();
    CHECK_FALSE(access.picker_search_pending());
    CHECK(lv_obj_get_child_count(list) == 1);
    CHECK(access.picker_state() == 2);

    // Clearing applies immediately - no debounce wait, no timer.
    access.type_picker_search("");
    UpdateQueue::instance().drain();
    CHECK_FALSE(access.picker_search_pending());
    CHECK(lv_obj_get_child_count(list) == 2);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker zero-match shows no-match state, not fetch-error retry",
                 "[ams_edit_overlay][picker][search]") {
    // The empty-filter result and the failed fetch used to share state 1, so
    // a term that matches nothing flashed "No spools found - tap to retry"
    // mid-word. A successful fetch with no matches is state 3: informational,
    // not an error, and not clickable.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("zzzz nothing matches this");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.picker_state() == 3);
    lv_obj_t* no_match = access.widget("picker_no_match");
    REQUIRE(no_match != nullptr);
    CHECK_FALSE(lv_obj_has_flag(no_match, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* retry = access.widget("picker_empty");
    REQUIRE(retry != nullptr);
    CHECK(lv_obj_has_flag(retry, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    CHECK(lv_obj_get_child_count(list) == 0);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker-entry spool selection commits and closes the editor",
                 "[ams_edit_overlay][picker][header_save]") {
    // Task #13: when the editor is opened directly on the picker (context-menu
    // "Select spool"), choosing a spool is a one-tap commit — apply + close the
    // whole overlay via the header-Save commit path, firing completion with the
    // applied spool. Spoolman is left unavailable so commit_and_close takes the
    // synchronous local-close branch (no async PATCH seam needed here).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(
        test_screen(), 0, untracked_slot(), nullptr,
        [&](const AmsEditOverlay::EditResult& r) {
            fired = true;
            captured = r;
        },
        /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewSpoolPicker);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Select spool #22 (eSUN PETG) from the picker.
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Committed + closed in one step, completion fired with the applied spool.
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(captured.slot_info.material == "PETG");

    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "the editor header names the position in the active backend's own word",
                 "[ams_edit_overlay][i18n]") {
    // The fixture registers an AmsBackendMock, which reports Happy Hare, whose
    // noun is Gate. The title a user reads above the editor has to be the same
    // word the panel behind it used for the thing they tapped.
    REQUIRE(backend->lane_noun() == helix::ui::LaneNoun::Gate);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    info.slot_index = 2;
    REQUIRE(overlay.show_for_slot(test_screen(), 2, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // header_bar's title carries text_transform="uppercase", so the rendered
    // string is the composed title in caps.
    lv_obj_t* title = access.widget("header_title");
    REQUIRE(title != nullptr);
    CHECK(std::string(lv_label_get_text(title)) == "GATE 3");

    close_editor_overlay();
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "picker-entry relink to a different spool never prompts or PATCHes the old spool",
                 "[ams_edit_overlay][filament_picker][picker][header_save]") {
    // Task #16 regression: a slot linked to spool A, opened directly on the
    // picker, then switched to a DIFFERENT spool B is a pure RELINK — not an
    // edit of A's identity. The old "Different filament?" confirm + identity
    // PATCH would clobber the wrong Spoolman record. Assert: no confirm modal,
    // completion fires once with spool B, and NO spool/filament PATCH is sent.

    // Seed the currently-linked spool A (id 7) so the open-time re-fetch has an
    // authoritative record and leaves original_info_ as A's identity.
    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked_a);
    // The backend mirrors the overlay's initial info, the way a live backend
    // holds the tracked slot the editor was opened on.
    backend->sync_external_identity(0, tracked_slot());

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1); // is_spoolman_available() -> true
    get_printer_state().capabilities_state().set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(
        test_screen(), 0, tracked_slot(), &api,
        [&](const AmsEditOverlay::EditResult& r) {
            fired = true;
            captured = r;
            commit_like_consumer(r); // the consumer commit owns the server sync
        },
        /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewSpoolPicker);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Switch the link to spool #22 (eSUN PETG) — a different material/color.
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // No "Different filament?" confirm modal interposed.
    CHECK(ModalStack::instance().stack_empty());
    // Committed + closed in one step with the newly linked spool.
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(captured.slot_info.material == "PETG");
    // The old spool A was never touched: no identity/weight PATCH, no filament
    // repoint, no new filament created.
    CHECK(client.spoolman_mock().spool_updates.empty());
    CHECK(client.spoolman_mock().filament_updates.empty());
    // The new spool is registered as active on the server.
    CHECK(client.spoolman_mock().get_mock_active_spool_id() == 22);

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "two-step relink header Save never prompts or PATCHes the old spool",
                 "[ams_edit_overlay][filament_picker][picker][header_save]") {
    // Task #16, two-step variant: reach the picker via Change Filament (not the
    // one-tap entry), pick a different spool, return to the editor, then tap
    // header Save. Same relink semantics: no confirm, no PATCH of the old spool.

    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked_a);
    backend->sync_external_identity(0, tracked_slot());

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_switch_to_picker(); // Change-Filament entry: clears the shortcut
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewSpoolPicker);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_handle_spool_selected(22); // stages the relink, returns to the editor
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewEditor);
    REQUIRE_FALSE(fired); // not committed yet
    REQUIRE(access.working_info().spoolman_id == 22);

    access.call_handle_save(); // header Save on the editor
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(ModalStack::instance().stack_empty()); // no identity confirm
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(client.spoolman_mock().spool_updates.empty());
    CHECK(client.spoolman_mock().filament_updates.empty());
    CHECK(client.spoolman_mock().get_mock_active_spool_id() == 22);

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "active_spool widget completion routes the edit through commit_slot_edit",
                 "[ams_edit_overlay][active_spool][commit]") {
    // Final-review find: ActiveSpoolWidget is the sixth completion consumer of
    // the shared edit overlay. Its backend-slot arm used to write the backend
    // directly (apply_user_edit) — no server active-spool sync, no identity
    // invalidation. The consumer commit owns both; this drives the widget's
    // own click → editor → Save path and asserts the server sync fired.

    // The mock backend starts with slot 0 loaded and current by construction;
    // give that lane a Spoolman link the way a live one carries it.
    SlotInfo seeded = backend->get_slot_info(0);
    seeded.spoolman_id = 169;
    seeded.material = "PLA";
    backend->sync_external_identity(0, seeded);

    // Spoolman "unavailable" so the editor's Save takes the synchronous
    // local-close branch (no async PATCH seam).
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);
    get_printer_state().capabilities_state().set_spoolman_available(false);

    // Build the home-panel component + controller, then tap it the way a user
    // does — the widget's own completion wiring is the code under test.
    lv_obj_t* comp =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_active_spool", nullptr));
    REQUIRE(comp != nullptr);
    ActiveSpoolWidget widget(&api);
    widget.attach(comp, test_screen());

    lv_obj_t* btn = lv_obj_find_by_name(comp, "spoolman_btn");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The editor opened on the loaded slot; stage an edit and header-Save the
    // overlay the widget opened.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    // A linked spool owns the material, so the edit moves the colour, which is
    // the user's to change.
    SlotInfo edited = seeded;
    edited.color_rgb = 0x1E5AA8;
    access.set_working_info(edited);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // REQUIRED: the edit reached the backend slot through commit_slot_edit...
    const SlotInfo after = backend->get_slot_info(0);
    REQUIRE(after.color_rgb == 0x1E5AA8u);
    REQUIRE(after.spoolman_id == 169);
    // ...AND the server active-spool sync fired — the old direct-write arm
    // never did, which is exactly the branch regression this pins.
    REQUIRE(client.spoolman_mock().get_mock_active_spool_id() == 169);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "editing the linked spool's identity (same spool) still prompts to confirm",
                 "[ams_edit_overlay][filament_picker]") {
    // Contrast with the relink cases: when spoolman_id is UNCHANGED and the
    // material identity changes, this is a genuine edit of the linked spool —
    // the "Different filament?" confirmation MUST still interpose (task #16 must
    // not swallow it). Completion does not fire until the user answers.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Seed spool 7 so the open-time re-fetch leaves original_info_ as ASA.
    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked_a);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain(); // flush the queued availability update
    REQUIRE(get_printer_state().is_spoolman_available());

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Same linked spool (id 7), but change the material identity in place.
    SlotInfo edited = access.working_info();
    edited.material = "PLA"; // ASA -> PLA on the SAME spool 7
    access.set_working_info(edited);

    access.call_handle_save(); // header Save on the editor
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The relink short-circuit must NOT swallow a same-spool identity edit: it
    // still routes into the confirm path (no early close). Either the confirm
    // modal is up (completion pending) or the fallback save fired against the
    // linked spool — but never a silent no-op relink close.
    const bool confirm_pending = !ModalStack::instance().stack_empty() && !fired;
    const bool save_ran = !client.spoolman_mock().spool_updates.empty() ||
                          !client.spoolman_mock().filament_updates.empty();
    CHECK((confirm_pending || save_ran));

    // Dismiss any modal so teardown is clean.
    if (!ModalStack::instance().stack_empty()) {
        Modal::hide(Modal::get_top());
        UpdateQueue::instance().drain();
        process_lvgl(10);
    }
    if (!fired) {
        close_editor_overlay();
    }
    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
}

TEST_CASE_METHOD(LVGLUITestFixture, "a linked slot still saves a color while Spoolman is offline",
                 "[ams_edit_overlay][spool_edit][1653]") {
    // Colour is the lane's own, not the spool's, so the read-only identity does
    // not reach it: a swatch picked with Spoolman down still saves.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewEditor);

    access.set_details_color(0x112233);
    access.call_handle_save(); // header Save on spool-edit finishes the edit
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(fired);
    CHECK(captured.slot_info.color_rgb == 0x112233U);
    // A picked colour has no name: the old one described a different colour,
    // and nothing computed from the hex is stored in its place.
    REQUIRE_FALSE(tracked_slot().color_name.empty());
    CHECK(captured.slot_info.color_name.empty());
    CHECK(captured.slot_info.material == tracked_slot().material);
    CHECK(captured.slot_info.brand == tracked_slot().brand);
}

TEST_CASE_METHOD(LVGLUITestFixture, "header-picker selection returns to the editor without closing",
                 "[ams_edit_overlay][picker]") {
    // Contrast with the picker-entry shortcut: reaching the picker via the header
    // Spoolman button (switch_to_picker clears opened_on_picker_) keeps the two-step
    // flow — a selection returns to the editor for review, no completion.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_switch_to_picker(); // Change-Filament entry: clears the shortcut
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewSpoolPicker);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::kViewEditor);
    CHECK_FALSE(fired); // no commit — the editor's header Save commits later
    CHECK(access.working_info().spoolman_id == 22); // staged, awaiting review

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker always offers the setup entry",
                 "[ams_edit_overlay][picker][setup_entry]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* entry = access.widget("picker_setup_entry");
    REQUIRE(entry != nullptr);
    // Present regardless of picker fetch state (loading/empty/content) —
    // it is the only path forward when the spool list is empty.
    CHECK_FALSE(lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(entry, LV_OBJ_FLAG_CLICKABLE));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker pre-selects the current spool when linked",
                 "[ams_edit_overlay][picker][preselect]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo linked = tracked_slot();
    linked.spoolman_id = 22;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, linked, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    CHECK_FALSE(lv_obj_has_state(lv_obj_get_child(list, 0), LV_STATE_CHECKED));
    CHECK(lv_obj_has_state(lv_obj_get_child(list, 1), LV_STATE_CHECKED));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "header Save shows on the editor, hides on picker/color",
                 "[ams_edit_overlay][views]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto* hide_subj = lv_xml_get_subject(nullptr, "ams_edit_save_hidden");
    REQUIRE(hide_subj != nullptr);
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    REQUIRE(lv_subject_get_int(hide_subj) == 0); // editor: visible

    access.call_set_view(AmsEditOverlay::kViewSpoolPicker);
    REQUIRE(lv_subject_get_int(hide_subj) == 1); // picker: hidden

    // Editor: Save stays VISIBLE and is disabled while nothing is dirty —
    // the dropdowns stage into working_info_ only through Save.
    access.call_set_view(AmsEditOverlay::kViewEditor);
    REQUIRE(lv_subject_get_int(hide_subj) == 0);
    REQUIRE_FALSE(access.is_dirty());
    CHECK(lv_subject_get_int(save_dis) == 1);

    access.call_set_view(AmsEditOverlay::kViewColor);
    REQUIRE(lv_subject_get_int(hide_subj) == 1); // color: hidden

    access.call_set_view(AmsEditOverlay::kViewEditor);
    REQUIRE(lv_subject_get_int(hide_subj) == 0);
    // Overview: dirty-gated — a clean slot keeps Save disabled.
    CHECK(lv_subject_get_int(save_dis) == 1);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "weightless slot opens clean — no fabricated weights",
                 "[ams_edit_overlay][dirty]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_slot_without_weights(*this); // SlotInfo with total/remaining = -1
    access.call_update_ui();

    REQUIRE_FALSE(access.is_dirty());
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    REQUIRE(lv_subject_get_int(save_dis) == 1);             // Save stays disabled
    REQUIRE(access.working_info().total_weight_g <= 0);     // untouched
    REQUIRE(access.working_info().remaining_weight_g <= 0); // untouched

    close_editor_overlay();
}

TEST_CASE("build_spool_patches splits spool-level vs filament-level fields",
          "[ams_edit_overlay][spool_details]") {
    SpoolInfo original;
    original.id = 42;
    original.filament_id = 7;
    original.remaining_weight_g = 500.0;
    original.spool_weight_g = 140.0;
    original.price = 19.99;
    original.lot_nr = "LOT-A";
    original.location = "Shelf A";
    original.comment = "";
    original.color_hex = "#FF0000";

    SpoolInfo edited = original;
    edited.remaining_weight_g = 450.0;
    edited.spool_weight_g = 200.0;
    edited.price = 24.99;
    edited.lot_nr = "LOT-B";
    edited.location = "Shelf B";
    edited.comment = "dried 4h";
    edited.color_hex = "#00FF00";

    nlohmann::json spool_patch;
    nlohmann::json filament_patch;
    AmsEditOverlayViewTestAccess::build_spool_patches(original, edited, spool_patch,
                                                      filament_patch);

    CHECK(spool_patch["remaining_weight"] == Catch::Approx(450.0));
    CHECK(spool_patch["price"] == Catch::Approx(24.99));
    CHECK(spool_patch["lot_nr"] == "LOT-B");
    CHECK(spool_patch["location"] == "Shelf B");
    CHECK(spool_patch["comment"] == "dried 4h");
    CHECK(spool_patch["spool_weight"] == Catch::Approx(200.0));

    CHECK(filament_patch.count("spool_weight") == 0);
    CHECK(filament_patch["color_hex"] == "#00FF00");
    CHECK(filament_patch.count("remaining_weight") == 0);

    // No changes -> both empty
    nlohmann::json empty_spool;
    nlohmann::json empty_filament;
    AmsEditOverlayViewTestAccess::build_spool_patches(original, original, empty_spool,
                                                      empty_filament);
    CHECK(empty_spool.empty());
    CHECK(empty_filament.empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a failed Spoolman save keeps the editor open and commits nothing",
                 "[ams_edit_overlay][spool_edit]") {
    // A save that did not reach Spoolman has changed nothing anywhere, so the
    // editor stays on the user's edit rather than closing on a local commit
    // they would read as saved.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Generic";
    linked.material = "PLA";
    linked.color_hex = "112233";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Every Spoolman request from here on fails.
    client.set_mock_spoolman_enabled(false);
    helix::TextLogCapture log;

    // A brand move on a linked spool changes which filament the spool points
    // at, so it PROMPTS (Update in Spoolman / Unlink) exactly like a material
    // move - not silently writing. The prompt owns this save; nothing is
    // written until a button answers it.
    SlotInfo edited = access.working_info();
    edited.brand = "Sunlu";
    access.set_working_info(edited);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(ModalStack::instance().stack_empty()); // the Update/Unlink prompt
    CHECK_FALSE(log.contains("Spoolman save failed")); // no write was attempted
    CHECK_FALSE(fired);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a failed Spoolman write keeps the editor open on the staged edit",
                 "[ams_edit_overlay][spoolman][failure]") {
    // A weight-only edit (spool-level, no identity move) reaches the Spoolman
    // write with no prompt - and with every request failing, nothing is
    // written anywhere, so the editor stays on the user's edit rather than
    // closing on a local commit they would read as saved.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Bambu Lab";
    linked.material = "ASA";
    linked.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    client.set_mock_spoolman_enabled(false);
    helix::TextLogCapture log;

    SlotInfo weighed = access.working_info();
    weighed.remaining_weight_g = 500;
    access.set_working_info(weighed);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    // The toast is queued from inside the deferred save answer.
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK_FALSE(fired);
    CHECK(access.working_info().remaining_weight_g == 500);
    CHECK(log.contains("Spoolman save failed; nothing written"));
    // Save is available again: the edit is still staged and still unsaved.
    CHECK(access.save_disabled() == 0);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "an incomplete new spool keeps the editor open and names what is missing",
                 "[ams_edit_overlay][spool_edit]") {
    // "It's a new spool" creates a filament, and Spoolman identifies one by
    // vendor, material and colour. With a field missing there is nothing to
    // create, so the editor stays open rather than closing on a save that
    // wrote nothing.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    SlotInfo edited = access.working_info();
    edited.brand = "";
    edited.color_rgb = 0xE53935;
    access.set_working_info(edited);

    helix::TextLogCapture log;
    access.call_do_spoolman_save(SpoolmanSlotSaver::LinkIntent::CreateAndRebind);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    // The toast is queued from inside the deferred save answer.
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK_FALSE(fired);
    // The save stopped for the missing field rather than as a generic failure,
    // which is what decides the sentence the user is shown.
    CHECK(log.contains("missing a field it is identified by"));
    CHECK_FALSE(log.contains("Spoolman save failed"));
    CHECK(client.spoolman_mock().created_spools.empty());
    CHECK(client.spoolman_mock().created_filaments.empty());

    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "a successful Spoolman save re-reads the linked spool onto the lane before the "
                 "next poll",
                 "[ams_edit_overlay][spoolman][1653]") {
    // A linked spool resolves its identity and its weights from the lane's
    // Spoolman record, which only a fetch writes, and the poll is a whole
    // interval wide. The save is the moment that record is known stale, so it
    // re-reads the spool it just wrote.
    seed_linked_spool(api, client, *backend);
    const helix::ams::LaneId lane = backend->lane_id(0);
    poll_lane_once(lane);
    REQUIRE(helix::ams::lane_sources(lane).spoolman->remaining_weight_g == 850.0F);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // A weight move is an edit Spoolman has to write, and it never prompts.
    SlotInfo edited = access.working_info();
    edited.remaining_weight_g = 600.0F;
    access.set_working_info(edited);

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(fired);
    REQUIRE(ModalStack::instance().stack_empty());

    // The save reached Spoolman. Separated from the lane check below so a
    // failure names which of the two halves broke.
    const auto& spools = client.spoolman_mock().get_mock_spools();
    const auto served =
        std::find_if(spools.begin(), spools.end(), [](const SpoolInfo& s) { return s.id == 7; });
    REQUIRE(served != spools.end());
    REQUIRE(served->remaining_weight_g == 600.0);

    // The read is issued after the commit, and its answer is filed from a
    // later drain.
    UpdateQueue::instance().drain();
    process_lvgl(10);

    const auto record = helix::ams::lane_sources(lane).spoolman;
    REQUIRE(record.has_value());
    REQUIRE(record->remaining_weight_g.has_value());
    CHECK(*record->remaining_weight_g == 600.0F);

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "an updated spool is refiled on its lane before the next poll",
                 "[ams_edit_overlay][spoolman][1653]") {
    // A spool created by the save has never been fetched, so the lane still
    // carries the spool it was bound to before. The read follows the id the
    // commit just bound, not the one the editor opened on.
    seed_linked_spool(api, client, *backend);
    const helix::ams::LaneId lane = backend->lane_id(0);
    poll_lane_once(lane);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    AmsEditOverlay::EditResult captured;
    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // A material move is an identity change, so Save asks whose spool this is.
    SlotInfo edited = access.working_info();
    edited.material = "PETG";
    access.set_working_info(edited);

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE_FALSE(ModalStack::instance().stack_empty()); // "Different filament?"
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    lv_obj_t* confirm_btn = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(confirm_btn != nullptr);
    lv_obj_send_event(confirm_btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(fired);
    // "Update in Spoolman" REPOINTS the spool at the matching filament: the
    // spool keeps its id and the lane's record now states the new material.
    REQUIRE(captured.slot_info.spoolman_id != 0);
    REQUIRE(captured.slot_info.spoolman_id == 7);

    UpdateQueue::instance().drain();
    process_lvgl(10);

    const auto record = helix::ams::lane_sources(lane).spoolman;
    REQUIRE(record.has_value());
    CHECK(record->spoolman_id == captured.slot_info.spoolman_id);
    CHECK(record->material == "PETG");

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE("AmsEditOverlay::save_is_disabled holds Save shut while a save is in flight",
          "[ams_edit_overlay][spool_edit]") {
    // The rule as a rule, with no editor around it: the async seam a live save
    // runs through is what makes the in-flight moment unobservable otherwise.
    using Access = AmsEditOverlayViewTestAccess;

    SECTION("the editor gates on the edit") {
        CHECK(Access::save_is_disabled(AmsEditOverlay::kViewEditor, false, false));
        CHECK_FALSE(Access::save_is_disabled(AmsEditOverlay::kViewEditor, false, true));
    }

    SECTION("a save in flight holds it shut on any view") {
        CHECK(Access::save_is_disabled(AmsEditOverlay::kViewEditor, true, true));
        CHECK(Access::save_is_disabled(AmsEditOverlay::kViewEditor, true, false));
    }
}

// ============================================================================
// Identity dropdown form: pure selection rules + option publishing
// ============================================================================

TEST_CASE("selection_from_slot splits family/variant/brand", "[ams_edit_overlay][dropdown]") {
    SlotInfo info;
    info.material = "PLA-CF";
    info.brand = "eSUN";
    info.catalog_id = "esun-pla-cf-1kg";
    const auto sel = AmsEditOverlay::selection_from_slot(info);
    CHECK(sel.family == "PLA");
    CHECK(sel.variant == "CF");
    CHECK(sel.brand == "eSUN");
    CHECK(sel.product_id == "esun-pla-cf-1kg");

    // Unknown values pass through verbatim so an untouched open+save
    // round-trips them (the extra selected option).
    info.material = "MysteryBlend";
    info.brand = "NoName Co";
    const auto unknown = AmsEditOverlay::selection_from_slot(info);
    CHECK(unknown.family == "MysteryBlend");
    CHECK(unknown.variant == "Basic");
    CHECK(unknown.brand == "NoName Co");

    // An empty material opens on the common case; an empty brand is Generic.
    info.material.clear();
    info.brand.clear();
    const auto blank = AmsEditOverlay::selection_from_slot(info);
    CHECK(blank.family == "PLA");
    CHECK(blank.brand == "Generic");
}

TEST_CASE_METHOD(LVGLUITestFixture, "apply_family_change pins the brand/variant reset rules",
                 "[ams_edit_overlay][dropdown]") {
    auto catalog = helix::printer::FilamentCatalog::load_full();

    AmsEditOverlay::SpoolEditSelection sel;
    sel.family = "PLA";
    sel.variant = "Silk";
    sel.brand = "Generic";
    sel.product_id = "generic-silk-pla";

    // A brand that makes the new family keeps the brand: pick one that makes
    // both PLA and PETG from the catalog itself, so the case cannot rot when
    // the catalog changes.
    const auto petg_makers = catalog.brands_in_family("PETG");
    const auto pla_makers = catalog.brands_in_family("PLA");
    std::string dual;
    for (const auto& b : petg_makers) {
        if (std::find(pla_makers.begin(), pla_makers.end(), b) != pla_makers.end() &&
            b != "Generic") {
            dual = b;
            break;
        }
    }
    REQUIRE_FALSE(dual.empty());
    AmsEditOverlay::SpoolEditSelection branded = sel;
    branded.brand = dual;
    const auto kept = AmsEditOverlay::apply_family_change(branded, "PETG", catalog);
    CHECK(kept.family == "PETG");
    CHECK(kept.variant == "Basic");
    CHECK(kept.brand == dual);
    CHECK(kept.product_id.empty());

    // A brand that does not make the new family falls back to Generic.
    AmsEditOverlay::SpoolEditSelection odd;
    odd.family = "PPA";
    odd.variant = "CF";
    odd.brand = "OnlyPPA Inc";
    const auto dropped = AmsEditOverlay::apply_family_change(odd, "PLA", catalog);
    CHECK(dropped.brand == "Generic");
    CHECK(dropped.variant == "Basic");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "editor dropdowns publish families, variants, Generic-first brands",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot(); // PETG
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Material dropdown: the slot's family is selected, the list carries the
    // families and ends in the browse entry.
    const std::string materials = access.dropdown_options("dropdown_material");
    CHECK(materials.find("PETG") != std::string::npos);
    CHECK(access.dropdown_selected("dropdown_material") == "PETG");
    const std::string browse = "Browse catalog...";
    CHECK(materials.size() > browse.size() + 1);
    CHECK(materials.compare(materials.size() - browse.size(), browse.size(), browse) == 0);

    // Brand dropdown: Generic leads and the browse entry trails.
    const std::string brands = access.dropdown_options("dropdown_brand");
    CHECK(brands.find("Generic\n") == 0);
    CHECK(access.dropdown_selected("dropdown_brand") == "Generic");

    close_editor_overlay();
}

namespace {
/// The option list's entries, in order.
std::vector<std::string> split_options(const std::string& options) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= options.size()) {
        const size_t nl = options.find('\n', at);
        out.push_back(options.substr(at, nl == std::string::npos ? nl : nl - at));
        if (nl == std::string::npos) {
            break;
        }
        at = nl + 1;
    }
    return out;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "every identity dropdown stages its pick through value_changed",
                 "[ams_edit_overlay][dropdown][callback]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.save_disabled() == 1);

    // Material, through the stacked layout's dropdown.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    process_lvgl(5);
    CHECK(access.sel().family == "PLA");
    CHECK(access.save_disabled() == 0);

    // Variant, through the split layout's twin: both layouts are wired.
    const auto variants = split_options(access.dropdown_options("dropdown_variant_split"));
    REQUIRE(variants.size() > 1);
    REQUIRE(access.pick_dropdown("dropdown_variant_split", variants[1]));
    process_lvgl(5);
    CHECK(access.sel().variant == variants[1]);

    // Brand: the first maker after Generic, which reveals the product row.
    const auto brands = split_options(access.dropdown_options("dropdown_brand"));
    REQUIRE(brands.size() > 2);
    REQUIRE(brands[0] == "Generic");
    REQUIRE(access.pick_dropdown("dropdown_brand", brands[1]));
    process_lvgl(5);
    CHECK(access.sel().brand == brands[1]);

    // Product: any line that brand makes for the family.
    const auto products = split_options(access.dropdown_options("dropdown_product"));
    if (!products.empty() && !products[0].empty()) {
        REQUIRE(access.pick_dropdown("dropdown_product", products[0]));
        process_lvgl(5);
        CHECK_FALSE(access.sel().product_id.empty());
    }

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "unknown material shows as its own selected family option",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    info.material = "MysteryBlend";
    info.brand = "NoName Co";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The catalog does not know the family, so it is the dropdown's selected
    // entry and
    // an untouched open+save changes nothing (selection not moved).
    CHECK(access.dropdown_selected("dropdown_material") == "MysteryBlend");
    CHECK(access.sel().family == "MysteryBlend");
    CHECK(access.sel().brand == "NoName Co");
    CHECK_FALSE(access.is_dirty());
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    // Round-tripped: the commit carried the unknown values verbatim.
    CHECK(access.working_info().material == "MysteryBlend");
    CHECK(access.working_info().brand == "NoName Co");
}

TEST_CASE_METHOD(LVGLUITestFixture, "weight pill keypad stages remaining grams",
                 "[ams_edit_overlay][weight]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    info.remaining_weight_g = 750;
    info.total_weight_g = 1000;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_apply_weight(600);
    UpdateQueue::instance().drain();
    CHECK(access.working_info().remaining_weight_g == 600);
    CHECK(access.working_info().total_weight_g == 1000); // known total untouched
    CHECK(access.is_dirty());
    close_editor_overlay();

    // A weightless slot: the same number is the honest first total.
    SlotInfo bare = untracked_slot();
    bare.remaining_weight_g = -1;
    bare.total_weight_g = -1;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, bare, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_apply_weight(750);
    UpdateQueue::instance().drain();
    CHECK(access.working_info().remaining_weight_g == 750);
    CHECK(access.working_info().total_weight_g == 750);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Reset to tag restores identity, not just the colour",
                 "[ams_edit_overlay][dropdown][tag]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // The slot opens DRIFTED from the tag (the values firmware last showed);
    // only a working reset can bring the tag's values back.
    helix::ams::TagReading tag;
    tag.material = "PETG";
    tag.brand = "Polymaker";
    tag.color_rgb = 0x112233;
    tag.total_weight_g = 1000;

    SlotInfo info = untracked_slot();
    info.material = "PLA";
    info.brand = "eSUN";
    info.color_rgb = 0xFF6600;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.set_tag(tag);

    // The user moves the selection away, then resets.
    AmsEditOverlay::SpoolEditSelection moved = access.sel();
    moved.family = "ABS";
    access.set_sel(moved);
    access.click("btn_reset_tag");
    UpdateQueue::instance().drain();

    CHECK(access.working_info().material == "PETG");
    CHECK(access.working_info().brand == "Polymaker");
    CHECK(access.working_info().color_rgb == 0x112233);
    CHECK(access.working_info().total_weight_g == 1000);
    CHECK(access.sel().family == "PETG");
    CHECK(access.sel().brand == "Polymaker");

    // The reset moved the slot off its opened-on values, so Save is live.
    CHECK(access.is_dirty());

    close_editor_overlay();

    // The mirror case: a slot that OPENED on the tag's values resets clean.
    SlotInfo aligned = untracked_slot();
    aligned.material = *tag.material;
    aligned.brand = *tag.brand;
    aligned.color_rgb = *tag.color_rgb;
    aligned.total_weight_g = *tag.total_weight_g;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, aligned, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.set_tag(tag);
    AmsEditOverlay::SpoolEditSelection drift = access.sel();
    drift.family = "ABS";
    access.set_sel(drift);
    access.call_handle_reset_to_tag();
    UpdateQueue::instance().drain();
    CHECK(access.working_info().material == "PETG");
    CHECK_FALSE(access.is_dirty());

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a dropdown pick refreshes the temperature line",
                 "[ams_edit_overlay][dropdown][r10]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot(); // PETG opens with PETG's range
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* temps = lv_xml_get_subject(nullptr, "ams_edit_temp_text");
    REQUIRE(temps != nullptr);
    const std::string petg_range = lv_subject_get_string(temps);
    REQUIRE(petg_range.find("PETG") == std::string::npos); // range text, not name
    CHECK(!petg_range.empty());

    // Move the family to PLA through the dropdown path; the temps line must
    // follow the pick immediately, not only after Save.
    AmsEditOverlay::SpoolEditSelection sel = access.sel();
    REQUIRE(sel.family == "PETG");
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.sel().family == "PLA");
    CHECK(lv_subject_get_string(temps) != petg_range);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a More-section edit lights Save and stages on it",
                 "[ams_edit_overlay][more]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE_FALSE(access.is_dirty());

    // A Notes-only edit is a real edit: dirty, Save lit, and it survives
    // Save's staging.
    REQUIRE(access.save_disabled() == 1);
    access.set_detail_field("detail_field_comment", "keep dry");
    UpdateQueue::instance().drain();
    CHECK(access.is_dirty());
    CHECK(access.save_disabled() == 0);

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.detail_comment() == "keep dry");

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "the open More section scrolls into reach, unclipped",
                 "[ams_edit_overlay][more]") {
    // The More toggle exists only on a Spoolman-linked slot.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.is_managed() == 1);
    access.toggle_more();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* content = access.widget("overlay_content");
    lv_obj_t* comment = access.widget("detail_field_comment");
    REQUIRE(content);
    REQUIRE(comment);
    lv_obj_update_layout(content);
    REQUIRE_FALSE(
        lv_obj_has_flag(lv_obj_get_parent(lv_obj_get_parent(comment)), LV_OBJ_FLAG_HIDDEN));

    lv_area_t field;
    lv_area_t box;
    lv_obj_get_coords(comment, &field);
    lv_obj_get_coords(content, &box);
    // At 800x480 the open section runs past the viewport, so only scrolling reaches it.
    REQUIRE(field.y2 > box.y2);

    // No container between the field and the scroller clips it.
    for (lv_obj_t* p = lv_obj_get_parent(comment); p && p != content; p = lv_obj_get_parent(p)) {
        lv_area_t a;
        lv_obj_get_coords(p, &a);
        CAPTURE(lv_obj_get_name(p), a.y2, field.y2);
        CHECK(field.y2 <= a.y2);
    }
    // The scroller's extent covers the field's bottom edge.
    CHECK(field.y2 <= box.y2 + lv_obj_get_scroll_bottom(content));

    close_editor_overlay();
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a picker relink reseeds the More section to the new spool",
                 "[ams_edit_overlay][more][relink]") {
    // The More fields must describe the spool the slot NOW names, and the
    // Save PATCH must target that spool's record, never the one the editor
    // opened on.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo spool_b;
    spool_b.id = 22;
    spool_b.filament_id = 9;
    spool_b.vendor = "eSUN";
    spool_b.material = "PETG";
    spool_b.color_hex = "FF6600";
    spool_b.location = "Shelf B";
    client.spoolman_mock().get_mock_spools().push_back(spool_b);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Relink to spool B through the picker path.
    access.set_cached_spools({spool_b});
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The More section now carries B's identity, not A's.
    CHECK(access.detail_id() == 22);
    CHECK(access.working_info().spoolman_id == 22);

    // A Notes edit on B saves against B's record.
    access.set_detail_field("detail_field_comment", "on B");
    UpdateQueue::instance().drain();
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(20);
    UpdateQueue::instance().drain();
    process_lvgl(20);

    bool patched_b = false;
    for (const auto& upd : client.spoolman_mock().spool_updates) {
        if (upd.spool_id == 22) {
            patched_b = true;
        }
    }
    CHECK(patched_b);

    close_editor_overlay();
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a combined material+notes save loses neither",
                 "[ams_edit_overlay][more][combined]") {
    // The identity prompt owns the save, and the Notes edit rides the
    // Update arm instead of being dropped.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Bambu Lab";
    linked.material = "ASA";
    linked.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) { fired = r.saved; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Move the material AND edit the notes in one save.
    SlotInfo edited = access.working_info();
    edited.material = "PETG";
    access.set_working_info(edited);
    access.set_detail_field("detail_field_comment", "combined edit");
    UpdateQueue::instance().drain();

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE_FALSE(ModalStack::instance().stack_empty()); // Update / Unlink prompt

    // "Update in Spoolman" is the primary button.
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    lv_obj_t* confirm = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(confirm != nullptr);
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(30);
    UpdateQueue::instance().drain();
    process_lvgl(30);

    CHECK(fired);
    // The notes reached the spool record alongside the identity move.
    bool notes_reached_spool = false;
    for (const auto& upd : client.spoolman_mock().spool_updates) {
        if (upd.patch.dump().find("combined edit") != std::string::npos) {
            notes_reached_spool = true;
        }
    }
    CHECK(notes_reached_spool);

    if (!fired) {
        close_editor_overlay();
    }
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a linked slot's family change refreshes the temps line",
                 "[ams_edit_overlay][r10][linked]") {
    // A linked slot carries its spool's stored range; a family pick must
    // show the picked family's range, not the stored one.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Bambu Lab";
    linked.material = "ASA"; // ASA prints far hotter than PLA
    linked.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // The linked spool's OWN range, as a fetched record leaves it: hot ASA
    // numbers that a PLA pick must not keep showing.
    SlotInfo info = tracked_slot();
    info.material = "ASA";
    info.nozzle_temp_min = 260;
    info.nozzle_temp_max = 290;
    info.bed_temp = 100;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto* temps = lv_xml_get_subject(nullptr, "ams_edit_temp_text");
    REQUIRE(temps != nullptr);
    const std::string asa_range = lv_subject_get_string(temps); // ASA's stored range
    REQUIRE_FALSE(asa_range.empty());

    // Move the family to PLA through the dropdown; PLA's cooler range must
    // replace ASA's immediately.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    const std::string pla_range = lv_subject_get_string(temps);
    CHECK_FALSE(pla_range.empty()); // a range, not a blanked line
    CHECK(pla_range != asa_range);

    close_editor_overlay();
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
}

namespace {
/// The mock's record for @p id, added when the inventory lacks one (a GET
/// serves the first match, so an existing record is edited, not shadowed).
SpoolInfo& mock_spool(MoonrakerClientMock& client, int id) {
    auto& spools = client.spoolman_mock().get_mock_spools();
    auto it =
        std::find_if(spools.begin(), spools.end(), [id](const SpoolInfo& s) { return s.id == id; });
    if (it == spools.end()) {
        SpoolInfo added;
        added.id = id;
        spools.push_back(added);
        return spools.back();
    }
    return *it;
}

void set_spoolman_available(bool available) {
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, available ? 1 : 0);
    get_printer_state().capabilities_state().set_spoolman_available(available);
    UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "a relink never carries the old spool's More fields over",
                 "[ams_edit_overlay][more][relink]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo& a = mock_spool(client, 7);
    a.filament_id = 3;
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    a.location = "Dry box 1";
    a.comment = "opened 5/3";
    SpoolInfo& b = mock_spool(client, 22);
    b.filament_id = 9;
    b.vendor = "eSUN";
    b.material = "PETG";
    b.color_hex = "FF6600";
    b.location = "Shelf B";
    b.comment = "";
    const SpoolInfo spool_b = b;
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.field_text("detail_field_location") == "Dry box 1");

    access.set_cached_spools({spool_b});
    access.call_handle_spool_selected(22);
    // Before B's record lands the fields are blank, not A's.
    CHECK(access.field_text("detail_field_location").empty());
    CHECK(access.field_text("detail_field_comment").empty());
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.field_text("detail_field_location") == "Shelf B");

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(20);
    UpdateQueue::instance().drain();

    for (const auto& upd : client.spoolman_mock().spool_updates) {
        const std::string body = upd.patch.dump();
        CHECK(body.find("Dry box 1") == std::string::npos);
        CHECK(body.find("opened 5/3") == std::string::npos);
    }

    if (overlay.get_root() && NavigationManager::instance().is_panel_in_stack(overlay.get_root())) {
        close_editor_overlay();
    }
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a late spool record never overwrites a relink or a pick",
                 "[ams_edit_overlay][more][relink]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.location = "Dry box 1";
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    // The open-time fetch is queued; the user picks before it lands.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.sel().family == "PLA"); // the record did not reset the pick
    CHECK(access.field_text("detail_field_location") == "Dry box 1");
    close_editor_overlay();

    // A relink made before A's record lands keeps B's identity.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    SpoolInfo other;
    other.id = 31;
    other.vendor = "Sunlu";
    other.material = "PETG";
    access.set_cached_spools({other});
    access.call_handle_spool_selected(31);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.working_info().spoolman_id == 31);
    CHECK(access.working_info().brand == "Sunlu");
    CHECK(access.field_text("detail_field_location") != "Dry box 1");
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker-entry then Back builds the editor from this slot alone",
                 "[ams_edit_overlay][picker][session]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // A previous session on a tag-read ASA slot.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    helix::ams::TagReading tag;
    tag.material = "ASA";
    tag.brand = "Bambu Lab";
    access.set_tag(tag);
    close_editor_overlay();

    // Picker-entry on an untracked PETG slot, then Back to the editor.
    REQUIRE(overlay.show_for_slot(test_screen(), 1, untracked_slot(), nullptr, nullptr, true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewSpoolPicker);
    CHECK_FALSE(access.has_tag());
    access.call_handle_back();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::kViewEditor);
    CHECK(access.sel().family == "PETG");
    CHECK(access.sel().brand == "Generic");
    CHECK(access.dropdown_selected("dropdown_material") == "PETG");
    CHECK(access.dropdown_selected("dropdown_brand") == "Generic");
    auto* reset_visible = lv_xml_get_subject(nullptr, "ams_edit_reset_tag_visible");
    REQUIRE(reset_visible != nullptr);
    CHECK(lv_subject_get_int(reset_visible) == 0);
    CHECK_FALSE(access.is_dirty());

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "the editor reopens while its last close is still sliding out",
                 "[ams_edit_overlay][session]") {
    // Overlays slide out only on the full-effects tier with animations on.
    ScopedTier tier(helix::PlatformTier::STANDARD);
    auto& display = DisplaySettingsManager::instance();
    const bool animations_were_enabled = display.get_animations_enabled();
    display.set_animations_enabled(true);

    auto& overlay = get_ams_edit_overlay();
    auto& nav = NavigationManager::instance();
    std::vector<std::pair<int, bool>> completions;
    auto record = [&completions](const AmsEditOverlay::EditResult& r) {
        completions.emplace_back(r.slot_index, r.saved);
    };

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, record));
    process_lvgl(400);
    lv_obj_t* root = overlay.get_root();
    REQUIRE(nav.is_panel_on_top(root));

    nav.go_back();
    UpdateQueue::instance().drain();
    process_lvgl(50);
    // Mid slide-out: the close has not reached the tree yet.
    REQUIRE(overlay.get_root() == root);
    REQUIRE_FALSE(nav.is_panel_in_stack(root));

    for (int slot : {1, 2}) {
        REQUIRE(overlay.show_for_slot(test_screen(), slot, tracked_slot(), nullptr, record));
        process_lvgl(400);
        REQUIRE(helix::test::widget_in_tree(overlay.get_root()));
        CHECK(nav.is_panel_on_top(overlay.get_root()));
        CHECK_FALSE(lv_obj_has_flag(overlay.get_root(), LV_OBJ_FLAG_HIDDEN));

        nav.go_back();
        UpdateQueue::instance().drain();
        process_lvgl(50);
    }
    process_lvgl(400);
    CHECK(overlay.get_root() == nullptr);

    // Every session settles exactly once, the reopened-over ones included.
    const std::vector<std::pair<int, bool>> expected{{0, false}, {1, false}, {2, false}};
    CHECK(completions == expected);

    display.set_animations_enabled(animations_were_enabled);
}

TEST_CASE_METHOD(LVGLUITestFixture, "leaving the picker keeps the edits staged in the editor",
                 "[ams_edit_overlay][picker][session]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    access.set_detail_field("detail_field_comment", "keep dry");
    access.call_switch_to_picker();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // "Not in your spools? Set it up" returns to the editor, edits intact.
    access.click("picker_setup_entry");
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.view() == AmsEditOverlay::kViewEditor);
    CHECK(access.sel().family == "PLA");
    CHECK(access.detail_comment() == "keep dry");
    CHECK(access.is_dirty());

    access.set_detail_field("detail_field_comment", "");
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a save in flight holds Back and the keypad shut",
                 "[ams_edit_overlay][save][session]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    lv_obj_t* root = overlay.get_root();
    ui_keypad_init(test_screen());

    access.set_save_in_flight(true);
    access.call_handle_back();
    access.call_handle_weight_tap();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(fired);
    CHECK_FALSE(ui_keypad_is_visible());
    CHECK(NavigationManager::instance().is_panel_on_top(root));
    CHECK(access.save_disabled() == 1);
    close_editor_overlay();

    // The next session starts with Save governed by its own edits alone.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_apply_weight(321);
    CHECK(access.save_disabled() == 0);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a Spoolman save completes even if the editor was covered",
                 "[ams_edit_overlay][save][session]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.filament_id = 3;
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    a.comment = "";
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    bool saved = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) { saved = r.saved; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_detail_field("detail_field_comment", "covered");
    access.call_handle_save();
    access.drop_screen_lifetime(); // covered before the PATCH answers
    UpdateQueue::instance().drain();
    process_lvgl(20);
    UpdateQueue::instance().drain();
    process_lvgl(20);

    CHECK(saved);
    if (!saved) {
        close_editor_overlay();
    }
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "an untracked slot on a Spoolman printer shows the Add link",
                 "[ams_edit_overlay][more][add_link]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    set_spoolman_available(true);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.shown("add_to_spoolman_link"));
    CHECK_FALSE(access.shown("more_row")); // the logistics fields are Spoolman's
    close_editor_overlay();

    // Linked: the toggle shows and the link does not.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(access.shown("add_to_spoolman_link"));
    CHECK(access.shown("more_row"));
    close_editor_overlay();

    // No Spoolman: neither, and the bar is left holding only Clear Spool.
    set_spoolman_available(false);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(access.shown("add_to_spoolman_link"));
    CHECK_FALSE(access.shown("more_row"));
    CHECK(access.shown("btn_clear_spool"));
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a price the field rounds does not make the editor dirty",
                 "[ams_edit_overlay][more][price]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    SpoolInfo fetched;
    fetched.id = 7;
    fetched.price = 19.995; // shown as 20.00 (or 19.99)
    fetched.location = "Shelf A";
    access.seed_detail_fetch(fetched);
    UpdateQueue::instance().drain();
    CHECK_FALSE(access.is_dirty());
    CHECK(access.save_disabled() == 1);

    // A real edit of the price still counts.
    access.set_detail_field("detail_field_price", "21.50");
    CHECK(access.is_dirty());
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "the brand list names Generic once",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    info.material = "PLA";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    const auto brands = split_options(access.dropdown_options("dropdown_brand"));
    CHECK(std::count(brands.begin(), brands.end(), std::string("Generic")) == 1);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a material with few makers lists Generic once",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // HIPS has one maker besides Generic; EVA has only Generic. The slot's
    // brand spelling differs in case from the catalog's.
    for (const char* material : {"HIPS", "EVA"}) {
        CAPTURE(material);
        SlotInfo info = untracked_slot();
        info.material = material;
        info.brand = "generic";
        REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        const auto brands = split_options(access.dropdown_options("dropdown_brand"));
        const auto generics = std::count_if(brands.begin(), brands.end(), [](const auto& b) {
            return helix::text_io::equals_ignore_case(b, "Generic");
        });
        CHECK(generics == 1);
        REQUIRE_FALSE(brands.empty());
        CHECK(brands.front() == "Generic");
        CHECK(brands.back() == lv_tr("Browse catalog..."));
        CHECK(access.dropdown_selected("dropdown_brand") == "Generic");
        close_editor_overlay();
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "picking Browse catalog opens the catalog sheet",
                 "[ams_edit_overlay][dropdown][callback]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    const std::string browse = lv_tr("Browse catalog...");

    // EVA's brand list is two entries, HIPS three, PLA many.
    for (const char* material : {"EVA", "HIPS", "PLA"}) {
        for (const char* row : {"dropdown_brand", "dropdown_material"}) {
            CAPTURE(material, row);
            SlotInfo info = untracked_slot();
            info.material = material;
            REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
            UpdateQueue::instance().drain();
            process_lvgl(10);
            const std::string before = access.dropdown_selected(row);
            REQUIRE_FALSE(access.catalog_sheet_visible());

            REQUIRE(access.pick_dropdown(row, browse));
            process_lvgl(5);
            CHECK(access.catalog_sheet_visible());
            // The row keeps showing the real selection behind the sheet.
            CHECK(access.dropdown_selected(row) == before);
            CHECK(access.sel().family == material);

            access.hide_catalog_sheet();
            process_lvgl(5);
            close_editor_overlay();
        }
    }
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture, "a Material dropdown pick reaches the saved slot",
                 "[ams_edit_overlay][dropdown][callback][commit]") {
    SlotInfo start = untracked_slot();
    start.material = "TPU-85A";
    REQUIRE(backend->sync_external_identity(0, start).success());

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, start, nullptr,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(access.pick_dropdown("dropdown_material", "HIPS"));
    process_lvgl(5);
    SECTION("Brand left as it opened") {}
    SECTION("Brand re-picked as Generic") {
        REQUIRE(access.pick_dropdown("dropdown_brand", "Generic"));
        process_lvgl(5);
    }
    CHECK(access.dropdown_selected("dropdown_brand") == "Generic");

    // A colour change rides the same save, so Save is open whatever the
    // dropdowns staged.
    access.call_open_color_view();
    access.call_apply_color(0x1E88E5);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Header Save, through its XML-registered callback.
    lv_obj_t* save = access.widget("action_button");
    REQUIRE(save != nullptr);
    REQUIRE(access.save_disabled() == 0);
    lv_obj_send_event(save, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.material == "HIPS");
    CHECK(captured.slot_info.brand == "Generic");
    CHECK(backend->get_slot_info(0).material == "HIPS");
    CHECK(backend->get_slot_info(0).brand == "Generic");
    CHECK(backend->get_slot_info(0).color_rgb == 0x1E88E5);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a variant the catalog lacks shows as the selected entry",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    info.material = "PLA-Zebra";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(access.sel().family == "PLA");
    REQUIRE(access.sel().variant == "Zebra");
    CHECK(access.dropdown_selected("dropdown_variant") == "Zebra");
    CHECK_FALSE(access.is_dirty());
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Browse catalog leaves the row intact and a pick sets the temps",
                 "[ams_edit_overlay][dropdown][browse]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot(); // PETG, Generic
    info.nozzle_temp_min = 199;
    info.nozzle_temp_max = 201;
    info.bed_temp = 33;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* temps = lv_xml_get_subject(nullptr, "ams_edit_temp_text");
    REQUIRE(temps != nullptr);
    const std::string stored = lv_subject_get_string(temps);
    REQUIRE(stored.find("199") != std::string::npos);

    REQUIRE(access.pick_dropdown("dropdown_material", "Browse catalog..."));
    process_lvgl(10);
    // The sheet is up and the row still names the selection, not the entry.
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    CHECK(access.dropdown_selected("dropdown_material") == "PETG");

    // Pick the first product row the sheet lists, then Select.
    lv_obj_t* list = lv_obj_find_by_name(dlg, "product_list");
    REQUIRE(list != nullptr);
    const auto catalog = helix::printer::FilamentCatalog::load_full();
    lv_obj_t* row = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t* child = lv_obj_get_child(list, static_cast<int32_t>(i));
        const char* name = lv_obj_get_name(child);
        if (name && catalog.resolve_id(name)) {
            row = child;
            break;
        }
    }
    REQUIRE(row != nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* select = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(select != nullptr);
    lv_obj_send_event(select, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK_FALSE(access.sel().product_id.empty());
    CHECK(std::string(lv_subject_get_string(temps)).find("199") == std::string::npos);
    close_editor_overlay();
}

namespace {
int subject_int(const char* name) {
    auto* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_int(subj);
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "each field the tag states carries its own badge",
                 "[ams_edit_overlay][tag]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    info.material = "PETG";
    info.brand = "Polymaker";
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    info.bed_temp = 80;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    helix::ams::TagReading tag;
    tag.material = "PETG";
    tag.brand = "Polymaker";
    tag.color_rgb = 0x112233; // the slot shows another colour
    tag.nozzle_temp_min = 230;
    tag.nozzle_temp_max = 250;
    tag.bed_temp = 80;
    access.set_tag(tag);
    access.call_update_tag_badges();
    CHECK(subject_int("ams_edit_tag_badge_visible") == 1);
    CHECK(subject_int("ams_edit_tag_material") == 1);
    CHECK(subject_int("ams_edit_tag_brand") == 1);
    CHECK(subject_int("ams_edit_tag_temps") == 1);
    CHECK(subject_int("ams_edit_tag_color") == 0);
    CHECK(subject_int("ams_edit_tag_product") == 0);

    // Moving the material off the tag's drops that badge (and the temps it
    // brought) and keeps the brand's.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    process_lvgl(5);
    CHECK(subject_int("ams_edit_tag_material") == 0);
    CHECK(subject_int("ams_edit_tag_temps") == 0);
    CHECK(subject_int("ams_edit_tag_brand") == 1);
    close_editor_overlay();
}

namespace {
/// The "from tag" pill sharing a row with @p dropdown, or nullptr.
lv_obj_t* tag_pill_beside(AmsEditOverlayViewTestAccess& access, const char* dropdown) {
    lv_obj_t* dd = access.widget(dropdown);
    REQUIRE(dd != nullptr);
    lv_obj_t* row = lv_obj_get_parent(dd);
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        lv_obj_t* cell = lv_obj_get_child(row, static_cast<int32_t>(i));
        if (lv_obj_get_child_count(cell) != 1) {
            continue;
        }
        lv_obj_t* text = lv_obj_get_child(cell, 0);
        if (lv_obj_check_type(text, &lv_label_class) &&
            std::string(lv_label_get_text(text)) == "from tag") {
            return cell;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "the material and brand rows show their from-tag pill",
                 "[ams_edit_overlay][tag]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    info.material = "PETG";
    info.brand = "Polymaker";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    helix::ams::TagReading tag;
    tag.material = "PETG";
    tag.brand = "Polymaker";
    access.set_tag(tag);
    access.call_update_tag_badges();
    process_lvgl(5);

    // Both layouts carry the pills; only the breakpoint decides which shows.
    for (const char* dd : {"dropdown_material", "dropdown_material_split", "dropdown_brand",
                           "dropdown_brand_split"}) {
        INFO(dd);
        lv_obj_t* pill = tag_pill_beside(access, dd);
        REQUIRE(pill != nullptr);
        CHECK_FALSE(lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    }

    // A material the tag does not state loses its pill; the brand keeps its own.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    process_lvgl(5);
    CHECK(lv_obj_has_flag(tag_pill_beside(access, "dropdown_material"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(tag_pill_beside(access, "dropdown_material_split"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(tag_pill_beside(access, "dropdown_brand"), LV_OBJ_FLAG_HIDDEN));
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picking another colour drops the colour's from-tag badge",
                 "[ams_edit_overlay][tag][color]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    helix::ams::TagReading tag;
    tag.color_rgb = info.color_rgb;
    access.set_tag(tag);
    access.call_update_tag_badges();
    REQUIRE(subject_int("ams_edit_tag_color") == 1);

    // The first preset tile in the grid, tapped.
    lv_obj_t* grid = access.widget("color_grid");
    REQUIRE(grid != nullptr);
    REQUIRE(lv_obj_get_child_count(grid) > 1);
    lv_obj_t* preset = lv_obj_get_child(grid, 1);
    REQUIRE((lv_color_to_u32(helix::ui::swatch_color_of(preset)) & 0xFFFFFF) != info.color_rgb);
    lv_obj_send_event(preset, LV_EVENT_CLICKED, nullptr);
    process_lvgl(5);

    CHECK(access.details_color_set());
    CHECK(subject_int("ams_edit_tag_color") == 0);
    CHECK(subject_int("ams_edit_tag_badge_visible") == 0);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a colour no visible preset holds rings the custom tile",
                 "[ams_edit_overlay][color]") {
    auto& overlay = get_ams_edit_overlay();
    SlotInfo info = untracked_slot();
    info.color_rgb = 0x123457; // a hand-mixed colour, in no preset palette
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(subject_int("ams_edit_color_selected") == -2);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "the More section opens collapsed in every session",
                 "[ams_edit_overlay][more][session]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    set_spoolman_available(true);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(access.shown("detail_field_comment"));
    access.click("more_row");
    process_lvgl(5);
    REQUIRE(access.shown("detail_field_comment"));
    close_editor_overlay();

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(access.shown("detail_field_comment"));
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a cover that drops the open-time fetches re-runs them once",
                 "[ams_edit_overlay][more][session]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.location = "Dry box 1";

    // Spoolman comes up after the editor opened, so the fetches start from an
    // activation instead of from the open.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    set_spoolman_available(true);

    // What NavigationManager runs when something (keypad, scanner) covers the
    // editor and is dismissed.
    auto cover_and_return = [&]() {
        overlay.on_deactivate(DeactivateReason::NavigateAway);
        UpdateQueue::instance().drain();
        process_lvgl(10);
        const std::string under_cover = access.field_text("detail_field_location");
        overlay.on_activate();
        return under_cover;
    };

    cover_and_return(); // the return sends both fetches
    // Covered again before either answer is applied: both die with the cover.
    CHECK(cover_and_return().empty());
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.field_text("detail_field_location") == "Dry box 1");

    // Both landed: the next cover costs no request.
    const int sent = client.call_count("server.spoolman.proxy");
    cover_and_return();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(client.call_count("server.spoolman.proxy") == sent);

    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the editor's non-swatch targets reach 44px",
                 "[ams_edit_overlay][touch]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    auto check_44 = [&](const char* name) {
        INFO(name);
        REQUIRE(access.shown(name));
        const lv_area_t a = access.click_area(name);
        CHECK(lv_area_get_width(&a) >= 44);
        CHECK(lv_area_get_height(&a) >= 44);
    };

    set_spoolman_available(true);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* reset_visible = lv_xml_get_subject(nullptr, "ams_edit_reset_tag_visible");
    REQUIRE(reset_visible != nullptr);
    lv_subject_set_int(reset_visible, 1);
    process_lvgl(5);
    check_44("btn_custom_color");
    check_44("weight_pill");
    check_44("add_to_spoolman_link");
    check_44("btn_reset_tag");
    lv_subject_set_int(reset_visible, 0);
    close_editor_overlay();

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    check_44("more_row");
    close_editor_overlay();
    set_spoolman_available(false);
}

namespace {
bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}
bool area_within(const lv_area_t& inner, const lv_area_t& outer) {
    return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 &&
           inner.y2 <= outer.y2;
}
int y_centre(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return (a.y1 + a.y2) / 2;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the header's icon buttons are round targets on the title's line",
                 "[ams_edit_overlay][touch][header]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    set_spoolman_available(true);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* scan = lv_xml_get_subject(nullptr, "ams_edit_scan_enabled");
    REQUIRE(scan != nullptr);
    lv_subject_set_int(scan, 1);
    process_lvgl(5);

    lv_obj_t* title = access.widget("header_title");
    REQUIRE(title != nullptr);
    lv_obj_update_layout(title);
    for (const char* name : {"btn_hdr_spoolman", "btn_hdr_scan_qr"}) {
        INFO(name);
        REQUIRE(access.shown(name));
        lv_obj_t* btn = access.widget(name);
        // A filled circle, so it reads as a button and not a bare glyph.
        CHECK(lv_obj_get_width(btn) == lv_obj_get_height(btn));
        CHECK(lv_obj_get_style_bg_opa(btn, LV_PART_MAIN) == LV_OPA_COVER);
        CHECK(lv_obj_get_style_radius(btn, LV_PART_MAIN) >= lv_obj_get_height(btn) / 2);
        // A full touch target, all of it inside the parent's box: LVGL
        // never hit-tests a child outside its parent.
        const lv_area_t hit = access.click_area(name);
        CHECK(lv_area_get_width(&hit) >= 44);
        CHECK(lv_area_get_height(&hit) >= 44);
        lv_area_t parent;
        lv_obj_get_coords(lv_obj_get_parent(btn), &parent);
        CHECK(area_within(hit, parent));
        CHECK(std::abs(y_centre(btn) - y_centre(title)) <= 2);
    }
    // The two targets meet without overlapping, so neither steals the other's taps.
    const lv_area_t a = access.click_area("btn_hdr_spoolman");
    const lv_area_t b = access.click_area("btn_hdr_scan_qr");
    CHECK(a.x2 < b.x1);

    // The linked-spool badge is an indicator: no target of its own, so a tap
    // on it is the back button's.
    lv_obj_t* badge = access.widget("header_title_badge");
    REQUIRE(badge != nullptr);
    CHECK_FALSE(lv_obj_has_flag(badge, LV_OBJ_FLAG_CLICKABLE));

    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "every colour swatch's reach is inside the grid and its own",
                 "[ams_edit_overlay][touch][color][grid]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* grid = access.widget("color_grid");
    REQUIRE(grid != nullptr);
    lv_obj_update_layout(grid);
    lv_area_t box;
    lv_obj_get_coords(grid, &box);
    const uint32_t tiles = lv_obj_get_child_count(grid);
    REQUIRE(tiles > 2);
    lv_area_t prev{};
    for (uint32_t i = 0; i < tiles; ++i) {
        lv_obj_t* tile = lv_obj_get_child(grid, static_cast<int32_t>(i));
        lv_area_t hit;
        lv_obj_get_click_area(tile, &hit);
        INFO("tile " << i);
        // The edge rows and columns too: reach outside the grid is never hit-tested.
        CHECK(area_within(hit, box));
        CHECK(lv_area_get_width(&hit) >= 44);
        CHECK(lv_area_get_height(&hit) >= 44);
        if (i > 0 && hit.y1 == prev.y1) {
            CHECK(prev.x2 < hit.x1);
        }
        prev = hit;
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "colour swatches keep a 7mm pitch on every class's panel",
                 "[ams_edit_overlay][touch][color][grid]") {
    struct Panel {
        int32_t w;
        int32_t h;
        double inches;
    };
    // 7" 800x480, 3.5" 480x320 (and its portrait), 4.3" 480x272.
    const Panel panels[] = {{800, 480, 7.0}, {480, 320, 3.5}, {320, 480, 3.5}, {480, 272, 4.3}};
    lv_display_t* disp = lv_display_get_default();
    // The shared swatch-grid tokens every preset grid sizes from.
    const auto token = [](const char* name) {
        const char* v = lv_xml_get_const(nullptr, name);
        REQUIRE(v != nullptr);
        return std::atoi(v);
    };
    for (const Panel& p : panels) {
        CAPTURE(p.w, p.h);
        {
            ScopedResolution res(disp, p.w, p.h);
            theme_manager_refresh_layout_constants(disp);
            const int disc = token("swatch_grid_size");
            const int gap = token("swatch_grid_gap");
            const int hit = token("swatch_grid_gap_half");
            const double dpi = std::hypot(p.w, p.h) / p.inches;
            const double floor_px = 7.0 / 25.4 * dpi;
            CAPTURE(disc, gap, hit, dpi, floor_px);
            CHECK(disc + gap >= floor_px);
            // The reach tiles the gap exactly: adjacent targets meet, never overlap.
            CHECK(2 * hit == gap);
            // The six-column grids are exactly six swatches and five gaps wide.
            CHECK(token("swatch_grid_width") == 6 * disc + 5 * gap);
        }
    }
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE("color_panes_fit shows both panes only where one form fits the view",
          "[ams_edit_overlay][color]") {
    // Pane sizes and content boxes measured from the colour view at each class.
    // 800x480: side by side, 252 + 20 + 302 = 574 of 672 wide, 235 of 372 tall.
    CHECK(AmsEditOverlay::color_panes_fit(672, 372, 252, 235, 302, 209, 20));
    // 480x320: 258 + 8 + 262 = 528 > 398 wide; stacked 229 + 8 + 175 = 412 > 259.
    CHECK_FALSE(AmsEditOverlay::color_panes_fit(398, 259, 258, 229, 262, 175, 8));
    // 480x272: 474 > 410 wide; stacked 355 > 204.
    CHECK_FALSE(AmsEditOverlay::color_panes_fit(410, 204, 246, 219, 220, 128, 8));
    // 320x480: 556 > 292 wide; stacked 443 > 326.
    CHECK_FALSE(AmsEditOverlay::color_panes_fit(292, 326, 258, 229, 290, 206, 8));
    // 600x1024 portrait: too narrow side by side (640 > 536), stacked 536 of 752.
    CHECK(AmsEditOverlay::color_panes_fit(536, 752, 252, 238, 364, 274, 24));
    // Exactly full fits; one pixel over does not.
    CHECK(AmsEditOverlay::color_panes_fit(100, 50, 45, 50, 45, 50, 10));
    CHECK_FALSE(AmsEditOverlay::color_panes_fit(99, 50, 45, 50, 45, 50, 10));
    CHECK(AmsEditOverlay::color_panes_fit(50, 110, 50, 50, 50, 50, 10));
    CHECK_FALSE(AmsEditOverlay::color_panes_fit(50, 109, 50, 50, 50, 50, 10));
    // Side by side needs the height as well as the width.
    CHECK_FALSE(AmsEditOverlay::color_panes_fit(500, 60, 100, 61, 100, 40, 10));
}

namespace {
bool shown(lv_obj_t* obj) {
    return obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
int color_subject(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}
/// Nothing in the colour view scrolls, and every shown swatch of the grid
/// lies inside the overlay's content area.
void check_color_view_static(AmsEditOverlayViewTestAccess& access) {
    lv_obj_t* content = access.widget("overlay_content");
    lv_obj_t* view = access.widget("color_view");
    REQUIRE(content != nullptr);
    REQUIRE(view != nullptr);
    lv_obj_update_layout(content);
    CHECK(lv_obj_get_scroll_bottom(content) <= 0);
    CHECK(lv_obj_get_scroll_bottom(view) <= 0);
    CHECK(lv_obj_get_scroll_right(view) <= 0);
    lv_area_t area;
    lv_obj_get_coords(content, &area);
    lv_obj_t* grid = access.widget("color_swatch_grid");
    if (!shown(grid) || !shown(access.widget("color_presets"))) {
        return;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
        lv_obj_t* tile = lv_obj_get_child(grid, static_cast<int32_t>(i));
        if (!shown(tile)) {
            continue;
        }
        lv_area_t t;
        lv_obj_get_coords(tile, &t);
        CAPTURE(i, t.x1, t.y1, t.x2, t.y2, area.x1, area.y1, area.x2, area.y2);
        CHECK(area_within(t, area));
    }
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the colour view shows presets and picker together only where both fit",
                 "[ams_edit_overlay][color][touch]") {
    struct Size {
        int32_t w;
        int32_t h;
        bool together;
    };
    lv_display_t* disp = lv_display_get_default();
    for (const Size sz : {Size{800, 480, true}, Size{480, 320, false}, Size{480, 272, false},
                          Size{320, 480, false}}) {
        CAPTURE(sz.w, sz.h);
        {
            ScopedResolution res(disp, sz.w, sz.h);
            theme_manager_refresh_layout_constants(disp);
            auto& overlay = get_ams_edit_overlay();
            AmsEditOverlayViewTestAccess access(overlay);
            REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
            UpdateQueue::instance().drain();
            process_lvgl(10);
            const int ring = color_subject("ams_edit_color_selected");
            access.call_open_color_view();
            process_lvgl(10);
            const std::string hex = access.field_text("ams_color_hex_input");

            CHECK(color_subject("ams_edit_color_fits") == (sz.together ? 1 : 0));
            lv_obj_t* presets = access.widget("color_presets");
            lv_obj_t* custom = access.widget("color_custom");
            REQUIRE(presets != nullptr);
            REQUIRE(custom != nullptr);
            check_color_view_static(access);

            if (sz.together) {
                // Both panes, apart, and no pane switch on offer.
                CHECK(shown(presets));
                CHECK(shown(custom));
                CHECK_FALSE(shown(access.widget("btn_custom_color_lead")));
                CHECK_FALSE(shown(access.widget("btn_color_presets")));
                lv_area_t p;
                lv_area_t c;
                lv_obj_get_coords(presets, &p);
                lv_obj_get_coords(custom, &c);
                CAPTURE(p.x1, p.y1, p.x2, p.y2, c.x1, c.y1, c.x2, c.y2);
                CHECK_FALSE(areas_overlap(p, c));
            } else {
                // The presets alone, led by the custom swatch.
                CHECK(shown(presets));
                CHECK_FALSE(shown(custom));
                REQUIRE(shown(access.widget("btn_custom_color_lead")));

                access.click("btn_custom_color_lead");
                process_lvgl(10);
                CHECK_FALSE(shown(presets));
                CHECK(shown(custom));
                CHECK(shown(access.widget("btn_color_presets")));
                check_color_view_static(access);
                // The custom colour and the editor's ring ride through the swap.
                CHECK(access.field_text("ams_color_hex_input") == hex);
                CHECK(color_subject("ams_edit_color_selected") == ring);

                access.click("btn_color_presets");
                process_lvgl(10);
                CHECK(shown(presets));
                CHECK_FALSE(shown(custom));
                check_color_view_static(access);
                CHECK(access.field_text("ams_color_hex_input") == hex);
                CHECK(color_subject("ams_edit_color_selected") == ring);
            }
            CHECK(access.view() == AmsEditOverlay::kViewColor);
            close_editor_overlay();
        }
        theme_manager_refresh_layout_constants(disp);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "reopening the colour view keeps its preset grid instead of rebuilding it",
                 "[ams_edit_overlay][color]") {
    // 480x320 is a one-pane size: the fit verdict is 0, the subject's
    // registered default is 1, so a remeasure on every open would flip it.
    lv_display_t* disp = lv_display_get_default();
    {
        ScopedResolution res(disp, 480, 320);
        theme_manager_refresh_layout_constants(disp);
        auto& overlay = get_ams_edit_overlay();
        AmsEditOverlayViewTestAccess access(overlay);
        REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);
        access.call_open_color_view();
        process_lvgl(10);
        REQUIRE(color_subject("ams_edit_color_fits") == 0);

        lv_obj_t* grid = lv_obj_find_by_name(access.widget("color_presets"), "color_swatch_grid");
        REQUIRE(grid != nullptr);
        static bool deleted;
        deleted = false;
        lv_obj_add_event_cb(grid, [](lv_event_t*) { deleted = true; }, LV_EVENT_DELETE, nullptr);

        access.call_set_view(AmsEditOverlay::kViewEditor);
        process_lvgl(10);
        access.call_open_color_view();
        process_lvgl(10);
        CHECK_FALSE(deleted);
        access.call_repopulate();
        process_lvgl(10);
        CHECK_FALSE(deleted);
        CHECK(color_subject("ams_edit_color_fits") == 0);
        CHECK(lv_obj_find_by_name(access.widget("color_presets"), "color_swatch_grid") == grid);
        close_editor_overlay();
    }
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE("grid_preset_count fills whole rows less the custom tile",
          "[ams_edit_overlay][color][grid]") {
    // 704 wide at a 32px tile and 12px gap: 16 per row.
    CHECK(AmsEditOverlay::grid_preset_count(704, 32, 12, 1, 30) == 15);
    CHECK(AmsEditOverlay::grid_preset_count(704, 32, 12, 2, 30) == 30); // capped at the palette
    // A 132px column at a 30px tile and 14px gap: three per row.
    CHECK(AmsEditOverlay::grid_preset_count(132, 30, 14, 3, 30) == 8);
    // Narrower than one tile still holds the custom swatch alone.
    CHECK(AmsEditOverlay::grid_preset_count(10, 30, 14, 1, 30) == 0);
    CHECK(AmsEditOverlay::grid_preset_count(132, 0, 14, 3, 30) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the colour grid wraps inside the editor's width",
                 "[ams_edit_overlay][color][grid]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* grid = access.widget("color_grid");
    REQUIRE(grid != nullptr);
    lv_obj_update_layout(grid);
    // The grid's own box grows with a non-wrapping row; the layout it sits
    // in is the editor's width.
    lv_area_t box;
    lv_obj_get_coords(lv_obj_get_parent(grid), &box);
    const uint32_t tiles = lv_obj_get_child_count(grid);
    REQUIRE(tiles > 1);
    for (uint32_t i = 0; i < tiles; ++i) {
        lv_area_t tile;
        lv_obj_get_coords(lv_obj_get_child(grid, static_cast<int32_t>(i)), &tile);
        INFO("tile " << i);
        CHECK(tile.x2 <= box.x2);
    }
    close_editor_overlay();
}

TEST_CASE("format_spool_price shows two decimals and blanks no price",
          "[ams_edit_overlay][more][price]") {
    CHECK(format_spool_price(19.5) == "19.50");
    CHECK(format_spool_price(19.995) == "20.00"); // the editor keeps 19.995 behind it
    CHECK(format_spool_price(0).empty());
    CHECK(format_spool_price(-1).empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a Spoolman save commits the slot even if the editor closed first",
                 "[ams_edit_overlay][save][session]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.filament_id = 3;
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    std::vector<AmsEditOverlay::EditResult> results;
    REQUIRE(
        overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                              [&](const AmsEditOverlay::EditResult& r) { results.push_back(r); }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    SlotInfo edited = access.working_info();
    edited.material = "PETG";
    access.set_working_info(edited);
    access.call_do_spoolman_save(SpoolmanSlotSaver::LinkIntent::UpdateLinked);
    access.call_fire_completion(false); // a navbar tap closes it mid-save
    for (int i = 0; i < 4; ++i) {
        UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    bool committed = false;
    for (const auto& r : results) {
        if (r.saved && r.slot_info.material == "PETG") {
            committed = true;
        }
    }
    CHECK(committed);
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Save keeps More edits whose spool record never loaded",
                 "[ams_edit_overlay][more][save]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = tracked_slot();
    info.spoolman_id = 98765; // Spoolman has no such record
    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity, const std::string& msg, uint32_t) { toasts.push_back(msg); });
    access.set_detail_field("detail_field_comment", "keep me");
    REQUIRE(access.save_disabled() == 0);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    helix::ui::set_test_toast_hook(nullptr);

    CHECK_FALSE(fired);                          // still open
    CHECK(access.detail_comment() == "keep me"); // edits kept
    REQUIRE(toasts.size() == 1);                 // and the user is told
    CHECK(toasts[0].find("haven't loaded") != std::string::npos);
    access.set_detail_field("detail_field_comment", "");
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a spool record landing after Reset to tag keeps the reset",
                 "[ams_edit_overlay][tag][relink]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    // The record is queued; the user resets to the tag before it lands.
    helix::ams::TagReading tag;
    tag.material = "PETG";
    tag.brand = "Polymaker";
    tag.color_rgb = 0x112233;
    access.set_tag(tag);
    access.call_handle_reset_to_tag();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.working_info().material == "PETG");
    CHECK(access.working_info().brand == "Polymaker");
    CHECK(access.working_info().color_rgb == 0x112233);
    CHECK(access.sel().family == "PETG");
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE("color_ring_index rings custom only for a set colour no visible preset holds",
          "[ams_edit_overlay][color]") {
    const std::vector<uint32_t> palette = {0x111111, 0x222222, 0x333333};
    CHECK(AmsEditOverlay::color_ring_index(0x222222, true, 3, palette) == 1);
    CHECK(AmsEditOverlay::color_ring_index(0x333333, true, 2, palette) == -2); // not shown
    CHECK(AmsEditOverlay::color_ring_index(0xABCDEF, true, 3, palette) == -2);
    CHECK(AmsEditOverlay::color_ring_index(0xABCDEF, false, 3, palette) == -1);

    SlotInfo unset;
    CHECK_FALSE(AmsEditOverlay::slot_has_colour(unset)); // the grey default
    SlotInfo bare_black;
    bare_black.color_rgb = 0x000000;
    CHECK_FALSE(AmsEditOverlay::slot_has_colour(bare_black));
    bare_black.material = "PLA";
    CHECK(AmsEditOverlay::slot_has_colour(bare_black)); // black PLA is a colour
}

TEST_CASE_METHOD(LVGLUITestFixture, "an unset slot rings no colour tile",
                 "[ams_edit_overlay][color]") {
    auto& overlay = get_ams_edit_overlay();
    SlotInfo info = untracked_slot();
    info.material.clear();
    info.color_name.clear();
    info.color_rgb = kAmsDefaultSlotColor;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(subject_int("ams_edit_color_selected") == -1);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a More-field save commits the slot's other edits even if the editor closed first",
                 "[ams_edit_overlay][save][session]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.filament_id = 3;
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    a.comment = "";
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    std::vector<AmsEditOverlay::EditResult> results;
    REQUIRE(
        overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                              [&](const AmsEditOverlay::EditResult& r) { results.push_back(r); }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_apply_weight(321);                          // slot-side
    access.set_detail_field("detail_field_comment", "dry"); // spool record
    access.call_handle_save();
    access.call_fire_completion(false); // a navbar tap closes it while the PATCH runs
    for (int i = 0; i < 4; ++i) {
        UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    bool committed = false;
    for (const auto& r : results) {
        if (r.saved && r.slot_info.remaining_weight_g == 321) {
            committed = true;
        }
    }
    CHECK(committed);
    bool patched = false;
    for (const auto& upd : client.spoolman_mock().spool_updates) {
        patched = patched || upd.patch.dump().find("dry") != std::string::npos;
    }
    CHECK(patched);
    close_editor_overlay();
    set_spoolman_available(false);
}

namespace {
/// A linked slot 0 on spool 7, Spoolman reachable, the record landed.
void open_linked_editor(LVGLUITestFixture& fixture, MoonrakerClientMock& client,
                        MoonrakerAPIMock& api, AmsEditOverlay::CompletionCallback cb) {
    SpoolInfo& a = mock_spool(client, 7);
    a.filament_id = 3;
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    a.comment = "";
    set_spoolman_available(true);
    REQUIRE(get_ams_edit_overlay().show_for_slot(fixture.test_screen(), 0, tracked_slot(), &api,
                                                 std::move(cb)));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

void settle(LVGLUITestFixture& fixture) {
    for (int i = 0; i < 4; ++i) {
        UpdateQueue::instance().drain();
        fixture.process_lvgl(20);
    }
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a slot with a save still running does not reopen until it lands",
                 "[ams_edit_overlay][save][session]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    std::vector<AmsEditOverlay::EditResult> results;
    open_linked_editor(*this, client, api,
                       [&](const AmsEditOverlay::EditResult& r) { results.push_back(r); });

    SlotInfo edited = access.working_info();
    edited.material = "PETG";
    access.set_working_info(edited);
    access.call_do_spoolman_save(SpoolmanSlotSaver::LinkIntent::UpdateLinked);
    access.call_fire_completion(false); // closed mid-save

    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity, const std::string& msg, uint32_t) { toasts.push_back(msg); });
    CHECK_FALSE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    helix::ui::set_test_toast_hook(nullptr);
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].find("Still saving") != std::string::npos);

    settle(*this);
    close_editor_overlay();
    REQUIRE_FALSE(results.empty());
    REQUIRE(results.back().saved);
    // Delivered: the slot reopens, on what the save committed.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, results.back().slot_info, &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.working_info().material == "PETG");
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a chained More PATCH that fails says so and still commits",
                 "[ams_edit_overlay][save][more]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    bool saved = false;
    open_linked_editor(*this, client, api,
                       [&](const AmsEditOverlay::EditResult& r) { saved = saved || r.saved; });

    SlotInfo edited = access.working_info();
    edited.material = "PETG";
    access.set_working_info(edited);
    access.set_detail_field("detail_field_comment", "lost");
    client.spoolman_mock().refuse_spool_patch_key = "comment";
    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity, const std::string& msg, uint32_t) { toasts.push_back(msg); });
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    lv_obj_send_event(lv_obj_find_by_name(dlg, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    settle(*this);
    helix::ui::set_test_toast_hook(nullptr);

    CHECK(saved);
    bool told = false;
    for (const auto& t : toasts) {
        told = told || t.find("notes didn't save") != std::string::npos;
    }
    CHECK(told);
    if (!saved) {
        close_editor_overlay();
    }
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a save that fails after its More PATCH landed says what did save",
                 "[ams_edit_overlay][save][more]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    open_linked_editor(*this, client, api, nullptr);

    access.call_apply_weight(321);
    access.set_detail_field("detail_field_comment", "kept");
    client.spoolman_mock().refuse_spool_patch_key = "remaining_weight";
    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity, const std::string& msg, uint32_t) { toasts.push_back(msg); });
    access.call_handle_save();
    settle(*this);
    helix::ui::set_test_toast_hook(nullptr);

    bool notes_patched = false;
    for (const auto& upd : client.spoolman_mock().spool_updates) {
        notes_patched = notes_patched || upd.patch.dump().find("kept") != std::string::npos;
    }
    REQUIRE(notes_patched);
    bool honest = false;
    bool false_claim = false;
    for (const auto& t : toasts) {
        honest = honest || t.find("saved the price, location, lot and notes") != std::string::npos;
        false_claim = false_claim || t.find("Nothing changed on this printer") != std::string::npos;
    }
    CHECK(honest);
    CHECK_FALSE(false_claim);
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "with Spoolman offline Save commits the slot and keeps the More edits",
                 "[ams_edit_overlay][save][more]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    // A linked printer whose Spoolman is unreachable: the record never loads.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
    std::vector<AmsEditOverlay::EditResult> results;
    SlotInfo info = tracked_slot();
    info.spoolman_id = 98765;
    REQUIRE(
        overlay.show_for_slot(test_screen(), 0, info, &api,
                              [&](const AmsEditOverlay::EditResult& r) { results.push_back(r); }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_apply_weight(321);
    access.set_detail_field("detail_field_comment", "later");
    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity, const std::string& msg, uint32_t) { toasts.push_back(msg); });
    access.call_handle_save();
    settle(*this);
    helix::ui::set_test_toast_hook(nullptr);

    REQUIRE(results.size() == 1);
    CHECK(results[0].saved);
    CHECK(results[0].slot_info.remaining_weight_g == 321);
    CHECK(access.view() == AmsEditOverlay::kViewEditor); // still open
    CHECK(access.detail_comment() == "later");
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].find("offline") != std::string::npos);
    access.set_detail_field("detail_field_comment", "");
    close_editor_overlay();
    set_spoolman_available(false);
}

// ============================================================================
// The lane's own settings: Assigned Tool and the Endless Spool backup
// ============================================================================

namespace {

/// Open the editor on @p slot as the AMS panels do, with the slot as the
/// backend holds it. @p completions counts the completion callbacks and
/// @p last_saved records the last one's verdict.
void show_editor_on_lane(LVGLUITestFixture& fx, AmsBackendMock& backend, int slot,
                         int* completions = nullptr, bool* last_saved = nullptr) {
    auto& overlay = get_ams_edit_overlay();
    REQUIRE(overlay.show_for_slot(fx.test_screen(), slot, backend.get_slot_info(slot), nullptr,
                                  [completions, last_saved](const AmsEditOverlay::EditResult& r) {
                                      if (completions) {
                                          ++*completions;
                                      }
                                      if (last_saved) {
                                          *last_saved = r.saved;
                                      }
                                  }));
    UpdateQueue::instance().drain();
    fx.process_lvgl(10);
}

void set_material(AmsBackendMock& backend, int slot, const char* material) {
    SlotInfo info = backend.get_slot_info(slot);
    info.material = material;
    info.status = SlotStatus::AVAILABLE;
    REQUIRE(helix::test::apply_edit(backend, slot, info).success());
}

} // namespace

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "the editor stages Assigned Tool and writes it through the backend on Save",
                 "[ams_edit_overlay][lane_settings]") {
    REQUIRE(helix::ui::lane_settings::show_tool_row(backend));
    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity sev, const std::string& msg, uint32_t) {
        toasts.emplace_back(sev, msg);
    });

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    int completions = 0;
    bool saved = true;
    show_editor_on_lane(*this, *backend, 2, &completions, &saved);

    REQUIRE(access.shown("tool_dropdown_row"));
    const int before = helix::ui::lane_settings::assigned_tool(*backend, 2);
    REQUIRE(before == 2);
    CHECK(access.dropdown_selected("tool_dropdown") == "T2");
    CHECK(access.save_disabled() == 1);

    // Staged only: the backend still maps T0 to its own lane.
    REQUIRE(access.pick_dropdown("tool_dropdown", "T0"));
    CHECK(backend->get_tool_mapping()[0] == 0);
    CHECK(access.is_dirty());
    CHECK(access.save_disabled() == 0);

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The same backend call the slot menu made, and the shared-lane warning
    // naming both tools by their labels.
    CHECK(backend->get_tool_mapping()[0] == 2);
    helix::ui::set_test_toast_hook(nullptr);
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::WARNING);
    CHECK(toasts[0].second.find("T0") != std::string::npos);
    CHECK(toasts[0].second.find("T2") != std::string::npos);
    // Nothing of the slot's own changed, so no slot edit is committed.
    CHECK(completions == 1);
    CHECK_FALSE(saved);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "picker greys spools other lanes hold, never the lane's own, on a remapped lane",
                 "[ams_edit_overlay][picker][tool-state]") {
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "extruder1", "extruder2", "heater_bed"}));
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 3);

    // Lane 2 feeds T0, so its spool sits in ToolState under tool 0.
    REQUIRE(backend->set_tool_mapping(0, 2).success());
    UpdateQueue::instance().drain();
    REQUIRE(backend->get_slot_info(2).mapped_tool == 0);
    ts.assign_spool(0, 9001, "Lane 2 spool");
    ts.assign_spool(2, 9002, "T2 spool");

    std::vector<SpoolInfo> spools(2);
    spools[0].id = 9001;
    spools[0].material = "PLA";
    spools[1].id = 9002;
    spools[1].material = "PETG";

    const auto disabled = [](lv_obj_t* list, int spool_id) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
            lv_obj_t* row = lv_obj_get_child(list, static_cast<int32_t>(i));
            if (reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)) == spool_id) {
                return lv_obj_has_state(row, LV_STATE_DISABLED);
            }
        }
        FAIL("no row for spool " << spool_id);
        return false;
    };

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // The external spool feeds through no lane, so every lane's spool is taken.
    bool external = false;
    SECTION("the lane's own editor") {
        SlotInfo lane = backend->get_slot_info(2);
        lane.spoolman_id = 9001;
        REQUIRE(overlay.show_for_slot(test_screen(), 2, lane, nullptr, nullptr));
    }
    SECTION("the external spool's editor") {
        external = true;
        SlotInfo info;
        info.slot_index = kExternalSpoolSlot;
        REQUIRE(overlay.show_for_slot(test_screen(), kExternalSpoolSlot, info, nullptr, nullptr));
    }
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(spools);
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    CHECK(disabled(list, 9001) == external);
    CHECK(disabled(list, 9002));

    close_editor_overlay();
    ts.deinit_subjects();
}

// A box that owns its tool table (CFS, like the mock) feeds every lane through
// one extruder, yet ToolState takes a tool per entry of that table from the
// backend's topology, so a loaded lane on T2 holds its spool under tool 2.
TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "picker on a one-extruder box greys every other lane's spool, never its own",
                 "[ams_edit_overlay][picker][tool-state]") {
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    ts.init_tools(hw);

    const auto link = [this](int slot, int spool_id) {
        SlotInfo info = backend->get_slot_info(slot);
        info.spoolman_id = spool_id;
        backend->sync_external_identity(slot, info);
    };
    link(0, 9001);
    link(2, 9003);
    backend->force_slot_status(2, SlotStatus::LOADED);
    AmsStateTestAccess::backend_frame(AmsState::instance());
    UpdateQueue::instance().drain();
    REQUIRE(backend->get_slot_info(2).mapped_tool == 2);
    REQUIRE(ts.tool_count() == 4);
    REQUIRE(ts.tools()[0].spoolman_id == 9001);
    REQUIRE(ts.tools()[2].spoolman_id == 9003);

    std::vector<SpoolInfo> spools(2);
    spools[0].id = 9001;
    spools[0].material = "PLA";
    spools[1].id = 9003;
    spools[1].material = "PETG";

    const auto disabled = [](lv_obj_t* list, int spool_id) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
            lv_obj_t* row = lv_obj_get_child(list, static_cast<int32_t>(i));
            if (reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)) == spool_id) {
                return lv_obj_has_state(row, LV_STATE_DISABLED);
            }
        }
        FAIL("no row for spool " << spool_id);
        return false;
    };

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    int slot = 0;
    SECTION("the lane on the tool") {
        slot = 0;
    }
    SECTION("a loaded lane numbered past the tools") {
        slot = 2;
    }
    REQUIRE(
        overlay.show_for_slot(test_screen(), slot, backend->get_slot_info(slot), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(spools);
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    CHECK(disabled(list, 9001) == (slot != 0));
    CHECK(disabled(list, 9003) == (slot != 2));

    close_editor_overlay();
    ts.deinit_subjects();
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "the editor stages the Endless Spool backup and writes it on Save",
                 "[ams_edit_overlay][lane_settings][endless_spool]") {
    set_material(*backend, 1, "PLA");
    set_material(*backend, 2, "PLA");
    set_material(*backend, 3, "ABS");
    REQUIRE(helix::ui::lane_settings::backup_of(*backend, 1) == -1);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    show_editor_on_lane(*this, *backend, 1);

    REQUIRE(access.shown("backup_dropdown_row"));
    lv_obj_t* label = access.widget("backup_label");
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Backup Gate");
    CHECK(access.dropdown_selected("backup_dropdown") == "None");

    SECTION("a compatible lane is staged, then written") {
        REQUIRE(access.pick_dropdown("backup_dropdown", "Gate 3"));
        CHECK(helix::ui::lane_settings::backup_of(*backend, 1) == -1);
        CHECK(access.save_disabled() == 0);
        access.call_handle_save();
        UpdateQueue::instance().drain();
        process_lvgl(10);
        CHECK(helix::ui::lane_settings::backup_of(*backend, 1) == 2);
    }

    SECTION("an incompatible lane is refused at the pick and nothing is staged") {
        std::vector<std::pair<ToastSeverity, std::string>> toasts;
        helix::ui::set_test_toast_hook([&](ToastSeverity sev, const std::string& msg, uint32_t) {
            toasts.emplace_back(sev, msg);
        });
        REQUIRE(access.pick_dropdown("backup_dropdown", "Gate 4 (incompatible)"));
        helix::ui::set_test_toast_hook(nullptr);
        REQUIRE(toasts.size() == 1);
        CHECK(toasts[0].first == ToastSeverity::ERROR);
        CHECK(access.dropdown_selected("backup_dropdown") == "None");
        CHECK_FALSE(access.is_dirty());
        CHECK(access.save_disabled() == 1);
        close_editor_overlay();
        CHECK(helix::ui::lane_settings::backup_of(*backend, 1) == -1);
    }
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "the lane rows stay hidden where the backend has no such capability",
                 "[ams_edit_overlay][lane_settings]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SECTION("both supported: both rows") {
        show_editor_on_lane(*this, *backend, 1);
        CHECK(access.shown("tool_dropdown_row"));
        CHECK(access.shown("backup_dropdown_row"));
    }
    SECTION("a mapping table the backend cannot write: no Assigned Tool") {
        backend->set_remap_strategy(AmsBackend::RemapStrategy::PrePrintSend);
        show_editor_on_lane(*this, *backend, 1);
        CHECK_FALSE(access.shown("tool_dropdown_row"));
        CHECK(access.shown("backup_dropdown_row"));
    }
    SECTION("a remap route that is not ready: no Assigned Tool") {
        backend->set_remap_ready(false);
        show_editor_on_lane(*this, *backend, 1);
        CHECK_FALSE(access.shown("tool_dropdown_row"));
    }
    SECTION("no Endless Spool: no backup row") {
        backend->set_endless_spool_supported(false);
        show_editor_on_lane(*this, *backend, 1);
        CHECK(access.shown("tool_dropdown_row"));
        CHECK_FALSE(access.shown("backup_dropdown_row"));
    }
    SECTION("neither: the whole row goes") {
        backend->set_remap_strategy(AmsBackend::RemapStrategy::PrePrintSend);
        backend->set_endless_spool_supported(false);
        show_editor_on_lane(*this, *backend, 1);
        CHECK_FALSE(access.shown("lane_row"));
    }
    SECTION("a read-only backup is shown disabled") {
        set_material(*backend, 1, "PLA");
        set_material(*backend, 2, "PLA");
        // Set while it is still editable: a read-only backup is the firmware's.
        REQUIRE(backend->set_endless_spool_backup(1, 2).success());
        backend->set_endless_spool_editable(false);
        show_editor_on_lane(*this, *backend, 1);
        REQUIRE(access.shown("backup_dropdown_row"));
        CHECK(lv_obj_has_state(access.widget("backup_dropdown"), LV_STATE_DISABLED));
        CHECK(access.dropdown_selected("backup_dropdown") == "Gate 3");
    }
    SECTION("the external spool is not a lane") {
        REQUIRE(overlay.show_for_slot(test_screen(), -2, SlotInfo{}, nullptr, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);
        CHECK_FALSE(access.shown("lane_section"));
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture, "the lane rows sit in view when the editor opens",
                 "[ams_edit_overlay][lane_settings][layout]") {
    lv_display_t* disp = lv_display_get_default();
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    for (const auto& [w, h] :
         {std::pair<int32_t, int32_t>{800, 480}, {480, 320}, {480, 272}, {320, 480}}) {
        {
            ScopedResolution res(disp, w, h);
            theme_manager_refresh_layout_constants(disp);
            show_editor_on_lane(*this, *backend, 1);
            lv_obj_t* content = access.widget("overlay_content");
            REQUIRE(content != nullptr);
            lv_obj_update_layout(content);
            lv_area_t box;
            lv_obj_get_coords(content, &box);
            CAPTURE(w, h);
            CHECK(lv_obj_get_scroll_right(content) <= 0);
            for (const char* name : {"tool_dropdown", "backup_dropdown"}) {
                INFO(name);
                REQUIRE(access.shown(name));
                lv_area_t dd;
                lv_obj_get_coords(access.widget(name), &dd);
                CHECK(dd.x1 >= box.x1);
                CHECK(dd.x2 <= box.x2);
                // Reachable by scrolling, never past the scroller's extent.
                CHECK(dd.y2 <= box.y2 + lv_obj_get_scroll_bottom(content));
                // In view when the editor opens wherever one row holds both.
                if (w > h) {
                    CHECK(dd.y2 <= box.y2);
                }
            }
            close_editor_overlay();
        }
        theme_manager_refresh_layout_constants(disp);
    }
}

// A spool that ran out has to come off the slot even while Spoolman is down,
// and a printer with no filament system has no lane menu to do it from.
TEST_CASE_METHOD(LVGLUITestFixture, "Clear takes a linked external spool off with Spoolman offline",
                 "[ams_edit_overlay][spool_edit][clear_spool]") {
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);
    get_printer_state().capabilities_state().set_spoolman_available(false);
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_backend(nullptr);
    UpdateQueue::instance().drain();

    SlotInfo linked = tracked_slot();
    linked.slot_index = -2;
    linked.global_index = -2;
    AmsState::instance().set_external_spool_info(linked);
    REQUIRE(AmsState::instance().get_external_spool_info().has_value());

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), -2, linked, nullptr,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::kViewEditor);
    CHECK(access.clear_visible() == 1);

    access.click("btn_clear_spool");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The clear is the commit; the caller must not commit the editor's copy over it.
    REQUIRE(fired);
    CHECK_FALSE(captured.saved);
    const auto ext = AmsState::instance().get_external_spool_info();
    CHECK((!ext || (ext->spoolman_id == 0 && ext->material.empty())));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Clear is not offered for a slot with nothing on it",
                 "[ams_edit_overlay][spool_edit][clear_spool]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo empty;
    empty.slot_index = -2;
    empty.global_index = -2;
    REQUIRE(overlay.show_for_slot(test_screen(), -2, empty, nullptr,
                                  [](const AmsEditOverlay::EditResult&) {}));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.clear_visible() == 0);
    close_editor_overlay();
}
