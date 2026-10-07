// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "grid_edit_mode.h"
#include "panel_widget.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct HomePanelTestAccess; // test-only friend (tests/test_helpers/)

/**
 * @brief Home panel - Main dashboard showing printer status and quick actions
 *
 * Pure grid container: all visible elements (printer image, tips, print status,
 * temperature, network, LED, power, etc.) are placed as PanelWidgets by
 * PanelWidgetManager. Widget-specific behavior lives in PanelWidget subclasses
 * which self-register their own XML callbacks, observers, and lifecycle.
 */

class HomePanel : public PanelBase {
  public:
    HomePanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);
    ~HomePanel() override;

    void init_subjects() override;
    void deinit_subjects();

    /**
     * @brief Death signal for the subjects this HomePanel owns.
     *
     * Pass to observe_*() from anything that can outlive this object's
     * deinit_subjects(): that path frees every observer node without bumping
     * the ObserverGuard invalidation epoch, so a guard without the token
     * dereferences a freed observer on its next reset().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    /// Complete the parts of setup that depend on the widget config / AMS
    /// detection state. Called after the first-run wizard finishes (so Moonraker
    /// has had a chance to report ams_slot_count), or immediately after setup
    /// when no wizard will run. Idempotent.
    void finalize_setup();
    /// setup() is deliberately minimal, so a rebuild leaves the fresh widget
    /// tree empty and the recycled panel widgets still bound to the old one.
    /// Re-run the finalize pass on the new tree.
    void repopulate() override;
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    const char* get_name() const override {
        return "Home Panel";
    }
    const char* get_xml_component_name() const override {
        return "home_panel";
    }

    /// Rebuild the widget list from current PanelWidgetConfig.
    /// @param force  If false (gate observer path), skip rebuild when the
    ///               visible widget ID list hasn't changed. If true (config
    ///               change, grid edit mode), always rebuild.
    void populate_widgets(bool force = true);

    /// Apply printer-level config (delegates to PrinterImageWidget)
    void apply_printer_config();

    /// Delegate printer image refresh to PrinterImageWidget if active
    void refresh_printer_image();

    /// Trigger a deferred runout check (delegates to PrintStatusWidget)
    void trigger_idle_runout_check();

    /// Navigate the carousel to page 0 (main page) if not already there
    void go_to_main_page();

    /// Leave grid edit mode: the one exit, used by the navbar Done button,
    /// deactivation and page deletion. A live gesture ends uncommitted first,
    /// so a drag over the next-page slot returns to its page before the
    /// session exits.
    void exit_grid_edit_mode();

    /// Open widget catalog overlay (called by navbar + button)
    void open_widget_catalog();

    /// Track counts page @p page is laid out with; {0, 0} for a page with no
    /// grid container.
    helix::GridDimensions page_grid(int page) const;

  private:
    SubjectManager subjects_;
    bool populating_widgets_ = false; // Reentrancy guard for populate_widgets()
    bool panel_active_ = false;       // Whether on_activate() has been called
    /// A widget config change asked for a rebuild while edit mode was live;
    /// leaving edit mode runs it.
    bool config_rebuild_deferred_ = false;
    bool finalized_ = false; // Whether finalize_setup() has run

    // Cached image path for skipping redundant refresh_printer_image() calls
    std::string last_printer_image_path_;

    // Grid edit mode state machine (long-press to rearrange widgets)
    helix::GridEditMode grid_edit_mode_;

    // Where the latest press on the grid landed, recorded on its PRESSED. A
    // press that drifted from there is not a hold, which edit-mode entry
    // requires to be stationary, and in edit mode it is not a click either.
    lv_point_t press_start_point_{};
    bool press_point_valid_ = false;
    /// True if the active pointer lies beyond the edit-mode cancel threshold
    /// from where the latest press landed. False when no press point is
    /// recorded.
    bool finger_drifted_since_press() const;

    // Image change observer (triggers printer image refresh)
    ObserverGuard image_changed_observer_;

    // Multi-page carousel state
    lv_obj_t* carousel_ = nullptr;
    lv_obj_t* carousel_host_ = nullptr;
    /// The next-page slot's page container, past the last page; nullptr at the
    /// page cap, where build_carousel() adds no slot.
    lv_obj_t* next_page_container_ = nullptr;
    lv_obj_t* arrow_left_ = nullptr;
    lv_obj_t* arrow_right_ = nullptr;

    /// One carousel page.
    struct CarouselPage {
        /// The page's grid container, a home_page_container component.
        lv_obj_t* container = nullptr;
        /// The widget instances placed in the container.
        std::vector<std::unique_ptr<helix::PanelWidget>> widgets;
        /// The visible widget ids the placement was computed from; empty when
        /// nothing is cached, so the next populate rebuilds even an unchanged list.
        std::optional<std::vector<std::string>> visible_ids;
        /// The runtime-widget generation visible_ids was built at. A plugin
        /// reload re-registers the same ids with factories bound to a new
        /// runtime, so an unchanged list alone must not skip the rebuild.
        uint64_t widget_gen = 0;
        /// Each placed widget's config, by id, as the page was built: with visible_ids,
        /// what a config change must match for reseat_widgets() to keep the tiles.
        std::map<std::string, nlohmann::json> built_configs;
        /// PanelWidgetManager::widget_config_saves() when built_configs was recorded.
        uint64_t config_saves = 0;
    };
    /// One per config page, in page order.
    std::vector<CarouselPage> pages_;
    int active_page_index_ = 0;
    lv_subject_t page_subject_{};
    ObserverGuard page_observer_;

    // Panel-lifetime subjects, construction-registered in the global XML scope
    // so the XML binds them by name: the ghost page badge ("N / M") and the
    // populated page count, which together show it only in edit mode with more
    // than one populated page. Kept in their OWN SubjectManager, not subjects_:
    // rebuild_carousel() calls subjects_.deinit_all(), and these outlive
    // carousel rebuilds (the badge label sits outside carousel_host).
    SubjectManager panel_subjects_;
    lv_subject_t page_badge_subject_{};
    char page_badge_buf_[16]{};
    lv_subject_t populated_pages_subject_{};
    void init_panel_subjects();
    void update_page_badge();
    void update_populated_pages_subject();

    /// Give edit mode the page work it asks the panel for: the rebuild that
    /// repopulates every page from config, the in-place relayout of the edited
    /// page (relayout_edit_page()), and the page-deletion confirmation.
    /// Part of finalize_setup(); the constructor wires the callbacks that must
    /// work before a carousel exists.
    void wire_grid_edit_page_callbacks();

    /// Rebuild every page when the home widget config changes (settings
    /// toggles, a widget's own config, the catalog's reset). During edit mode
    /// the rebuild waits for the session to end.
    void register_config_rebuild_callback();

    /// Build the carousel from config, showing @p initial_page (clamped to the
    /// config pages), and populate every page. Re-scopes no edit session: the
    /// population clears every page's children, so a caller rebuilding under a
    /// session shows its page with show_edit_page() once the build is done.
    void build_carousel(int initial_page);
    /// Tear the carousel down and build it again showing @p shown, clamped to
    /// the config pages. A live edit session forgets its scope first, since the
    /// teardown deletes it.
    void rebuild_carousel(int shown);
    /// Release everything build_carousel() created: the page widgets and page
    /// lists, the page observer, the carousel_host children (async-deleted),
    /// the object pointers into them and the page subject. carousel_host_ stays.
    void teardown_carousel();
    void on_page_changed(int new_page);
    /// The page-deletion confirmation's action: remove the edit session's page,
    /// leave edit mode, and rebuild the carousel on the next tick through
    /// on_edit_pages_changed(), on the deleted page's index.
    void delete_edit_page();
    /// The + on the next-page slot's action: append a page through the same
    /// PanelWidgetConfig::add_page() a drag's drop creates with, and land on it
    /// through the same on_edit_pages_changed() rebuild, on the next tick.
    void add_page_from_slot();
    void update_arrow_visibility(int page);
    void populate_page(int page_index, bool force);
    /// Re-seats every page in place when the new config holds the same widgets with the
    /// same config and only placement differs (a printer switch between printers whose
    /// layouts differ in placement only). False, having changed nothing, when any page
    /// differs in widgets or config; a page that cannot be re-seated is repopulated.
    bool reseat_widgets();
    std::map<std::string, nlohmann::json> configs_for(int page_index,
                                                      const std::vector<std::string>& ids) const;
    /// Re-seat the edit session's page in place after a move or resize placed
    /// @p changed_ids (PanelWidgetManager::relayout_tiles), giving the tile
    /// named @p resized_id (empty for none) its new span. False when the page
    /// needs a full populate instead.
    bool relayout_edit_page(const std::vector<std::string>& changed_ids,
                            const std::string& resized_id);

    /// Apply the carousel swipe policy for the edit session: Disabled while an
    /// edit gesture owns the pointer or the widget catalog is open, Auto (swipe
    /// by page count) otherwise, in edit mode or out of it. The next-page slot
    /// carries the + that adds a page and a drag's drop target, so its tile is
    /// within reach whenever the slot exists, drag or no drag
    /// (prestonbrown/helixscreen#1638). Flags only, so it is safe inside input
    /// dispatch.
    void apply_edit_swipe_policy();

    /// The container edit mode scopes to as page @p page: a config page's, the
    /// next-page slot's for the page past the last when the slot exists, and
    /// nullptr for any other page.
    lv_obj_t* edit_container(int page) const;

    /// Show @p page, animated, and re-scope a live edit session to it: the one
    /// way edit mode shows a page, for drag flips, a drag returned to its
    /// origin, the widget catalog closing and the focus of a rebuilt page set.
    /// No-op for a page with no container.
    void show_edit_page(int page);

    /// Re-scope a live edit session to edit_container(@p page), letting
    /// switch_page() decide what travels. No-op when edit mode is off or the
    /// page has no container.
    void rescope_edit_page(int page);

    /// The page set changed from edit mode (a drop-created page, a pruned empty
    /// page, a deleted page): rebuild the carousel on the page
    /// helix::page_set_landing() gives for the page on screen, then show the
    /// change's focus, re-scoping a live session there. The carousel comes up on
    /// the page that was on screen and slides once when the focus is another
    /// page.
    void on_edit_pages_changed(const helix::PageSetChange& change);

    /// True while the carousel's page is the scoped page: the edit session's
    /// page in edit mode, the active page outside it. False while the carousel
    /// rests on a page the session does not follow, one the arrow buttons paged
    /// to while the widget catalog holds the session, where the scoped page is
    /// off screen. A slide never reads false:
    /// the carousel's page is the one a goto is heading to or a swipe began
    /// from, and the handlers hit-test live coordinates. It gates the handlers
    /// that act on what a press lands on (long press, pressing, clicked); a
    /// press always begins a gesture and a release always ends one.
    bool carousel_on_scoped_page() const;

    // Grid and widget lifecycle
    void setup_widget_gate_observers();

    // Panel-level static callbacks
    static void on_home_grid_pressed(lv_event_t* e);
    static void on_home_grid_long_press(lv_event_t* e);
    static void on_home_grid_clicked(lv_event_t* e);
    static void on_home_grid_pressing(lv_event_t* e);
    static void on_home_grid_released(lv_event_t* e);
    /// PRESS_LOST and INDEV_RESET: LVGL took a press away without a RELEASED.
    static void on_home_grid_press_cancelled(lv_event_t* e);

    /// Guards the carousel rebuilds this panel runs on the next tick. Declared
    /// last, so it expires before any other member is destroyed.
    helix::AsyncLifetimeGuard lifetime_;

    friend struct ::HomePanelTestAccess;
};

// Global instance accessor (needed by main.cpp)
HomePanel& get_global_home_panel();
