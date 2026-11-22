// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The spool editor's view-test kit, shared by test_ams_edit_overlay_views.cpp
// and test_ams_edit_pages.cpp: the friend shim reaching AmsEditOverlay's
// private state, and the fixtures that open and close the editor.

#include "ui_ams_edit_overlay.h"
#include "ui_nav_manager.h"
#include "ui_obj_flags.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"

#include <memory>
#include <string>
#include <vector>

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
    int spool_disagrees() {
        return lv_subject_get_int(&overlay_.spool_disagrees_subject_);
    }
    std::string spool_disagreement_text() {
        return lv_subject_get_string(&overlay_.spool_disagreement_text_subject_);
    }
    std::string selected_family() {
        return overlay_.sel_.family;
    }
    bool dirty() {
        return overlay_.is_dirty();
    }
    int is_managed() {
        return lv_subject_get_int(&overlay_.is_managed_subject_);
    }
    int clear_visible() {
        return lv_subject_get_int(&overlay_.clear_visible_subject_);
    }
    void set_cached_spools(std::vector<SpoolInfo> spools) {
        overlay_.cached_spools_ = std::move(spools);
        overlay_.spools_from_spoolman_ = true;
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
    /// What a QR scan's result does to the open editor.
    void scan_spool(const SpoolInfo& spool) {
        overlay_.apply_scanned_spool(spool);
    }
    void tap_common_spool(size_t index) {
        overlay_.handle_common_spool(index);
    }
    void step_remaining(float delta) {
        overlay_.handle_remaining_step(delta);
    }
    void call_assign_local() {
        overlay_.handle_assign_local();
    }
    void call_populate_picker() {
        overlay_.populate_picker();
    }
    void call_different_spool() {
        overlay_.handle_different_spool();
    }
    int mode() {
        return lv_subject_get_int(&overlay_.mode_subject_);
    }
    /// The page shown (AmsEditOverlay::kPage*).
    int page() {
        return lv_subject_get_int(&overlay_.page_subject_);
    }
    /// Show @p page the way its opening tap does.
    void open_page(int page) {
        overlay_.set_page(page);
    }
    int adding() {
        return lv_subject_get_int(&overlay_.adding_subject_);
    }
    std::string subject_text(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }
    /// Tap Use on the listed row holding @p spool_id, listing the cached
    /// spools first when nothing is listed yet.
    void use_spool(int spool_id) {
        if (overlay_.listed_.empty()) {
            overlay_.render_spool_list("");
        }
        for (size_t row = 0; row < overlay_.listed_.size(); ++row) {
            if (overlay_.cached_spools_[overlay_.listed_[row].index].id == spool_id) {
                overlay_.handle_row_use(row);
                return;
            }
        }
        FAIL("spool " << spool_id << " is not listed");
    }
    /// The spool ids the list shows, in order.
    static constexpr int kSaveIdle = AmsEditOverlay::kSaveIdle;
    static constexpr int kSaveWaiting = AmsEditOverlay::kSaveWaiting;
    static constexpr int kSaveFailed = AmsEditOverlay::kSaveFailed;
    void call_set_save_state(int state, const std::string& error) {
        overlay_.set_save_state(state, error);
    }
    std::vector<int> listed_ids() {
        std::vector<int> ids;
        for (const auto& r : overlay_.listed_) {
            ids.push_back(overlay_.cached_spools_[r.index].id);
        }
        return ids;
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
    void stage_selection() {
        overlay_.selection_moved_ = true;
        overlay_.stage_selection_into_working();
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
            if (lv_obj_is_hidden(o)) {
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
    void call_open_color_page() {
        overlay_.open_color_page();
    }
    void call_recent_color(int index) {
        overlay_.handle_recent_color(index);
    }
    /// The color row's shown swatches after the spool's dots, in order.
    std::vector<lv_obj_t*> row_swatches() {
        std::vector<lv_obj_t*> out;
        lv_obj_t* row = overlay_.find_widget("color_row");
        REQUIRE(row != nullptr);
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
            lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
            const char* name = lv_obj_get_name(child);
            const bool fixed = name && (std::string(name) == "color_dots" ||
                                        std::string(name) == "btn_all_colors");
            if (!fixed && !lv_obj_is_hidden(child)) {
                out.push_back(child);
            }
        }
        return out;
    }
    const std::vector<uint32_t>& color_row() {
        return overlay_.color_row_;
    }
    void call_add_color() {
        overlay_.add_color();
    }
    void call_seg_tap(int i) {
        overlay_.handle_seg_tap(i);
    }
    void call_seg_hold(int i) {
        overlay_.handle_seg_hold(i);
    }
    void call_color_mode(int mode) {
        overlay_.handle_color_mode(mode);
    }
    std::vector<uint32_t> draft_colors() {
        return overlay_.draft_colors();
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
    int save_state() {
        return lv_subject_get_int(&overlay_.save_state_subject_);
    }
    std::string save_error() {
        return lv_subject_get_string(&overlay_.save_error_subject_);
    }
    std::string save_wait_text() {
        return lv_subject_get_string(&overlay_.save_wait_text_subject_);
    }
    bool closed() const {
        return overlay_.completion_fired_;
    }
    SlotInfo working_info() const {
        return overlay_.working_info_;
    }
    /// The slot a Save would commit now.
    SlotInfo saving() const {
        return overlay_.capture_save().saving;
    }
    static std::string not_saved_message(helix::ams::PendingOutcome outcome,
                                         const std::string& reason) {
        return AmsEditOverlay::not_saved_message(outcome, reason);
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
    /// Forget the session's completion callback: it captures the test's
    /// locals, which are gone by the time a fixture tears the editor down.
    void drop_completion() {
        overlay_.completion_callback_ = nullptr;
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

inline void close_editor_overlay() {
    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    lv_timer_handler(); // the overlay close callback runs on the next tick
}

/// Closes an editor a test left open (a failed REQUIRE ends the test before
/// its own close), so the next test never inherits a live session.
struct EditorFixture : LVGLUITestFixture {
    ~EditorFixture() override {
        auto& overlay = get_ams_edit_overlay();
        lv_obj_t* root = overlay.get_root();
        if (root && NavigationManager::instance().is_panel_in_stack(root)) {
            AmsEditOverlayViewTestAccess(overlay).drop_completion();
            close_editor_overlay();
        }
    }
};

inline void set_spoolman_available(bool available) {
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, available ? 1 : 0);
    get_printer_state().capabilities_state().set_spoolman_available(available);
    UpdateQueue::instance().drain();
}

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

struct OverlayConsumerCommitFixture : EditorFixture {
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
