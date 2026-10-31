// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_edit_page_swipe.cpp
 * @brief Page flipping and cross-page drag in home-grid edit mode (prestonbrown/helixscreen#1638).
 *
 * On capacitive hardware the swipe is the only way to reach other pages and the
 * next-page slot during edit mode, so the carousel stays swipeable unless a
 * widget gesture (an armed press, a drag or a resize) holds the pointer.
 *
 * The carousel cases run on EditHomeFixture: HomePanel's own build_carousel() on
 * a stand-in for home_panel.xml's root, so the pages and the next-page slot come
 * from the home widget config and its XML components, page changes set the page
 * subject the panel observes, and a commit that changes the page set rebuilds
 * the carousel. Pointer input comes from real indev reads, at the device's read
 * cadence wherever time matters.
 * Pages are populated by HomePanel::populate_page(), and edit mode's rebuild is
 * the one finalize_setup() wires, so every rebuild clears a page's children and
 * lays its widgets out again from config. Only the widgets themselves stand in:
 * the registry factories of the ids the config names build a plain named tile,
 * so a case depends on no printer state. A rebuild replaces a widget's object,
 * so a case looks a widget up by name after one.
 */

#include "ui_breakpoint.h"
#include "ui_carousel.h"
#include "ui_dialog.h"
#include "ui_drag_surface.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_home.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_widget_catalog_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/home_panel_test_access.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/mock_config_storage.h"
#include "../test_helpers/scoped_animations_enabled.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "../test_helpers/scoped_widget_factory.h"
#include "../test_helpers/widget_in_tree.h"
#include "app_globals.h"
#include "config.h"
#include "display_settings_manager.h"
#include "grid_edit_cross_page.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "home_edit_mode_test_helpers.h"
#include "indev/lv_indev_private.h" // LV_INDEV_VECT_HIST_SIZE: no public accessor
#include "lvgl_test_fixture.h"
#include "misc/lv_event_private.h" // lv_event_dsc_t::filter: no public accessor
#include "misc/lv_timer_private.h" // lv_timer_t::timer_cb, repeat_count: no public accessor
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "scope_exit.h"
#include "setting_group.h"
#include "theme_manager.h"
#include "xml_registration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::GridEditModeTestAccess;
using helix::ui::CarouselSwipe;
using helix_test::ScopedInputSettings;
using helix_test::ScopedLockState;
using helix_test::ScopedPointerIndev;
using helix_test::ScopedWidgetFactory;

namespace {

constexpr uint32_t kReadPeriodMs = ScopedPointerIndev::kReadPeriodMs;
constexpr int kCellTracks = helix::GridLayout::TRACKS_PER_CELL;

bool is_hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

bool is_clickable(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

/// Installs a known "home" widget config and puts the prior one back however the
/// test exits. Page 0 holds 'temperature' and page 1 'fan', each at cell (0,0)
/// and one cell tall, on pages counted as populated; every further page is
/// empty. Populated pages are what the badge's populated-page count reads.
/// Every other registry widget is listed disabled on page 0, so population
/// places only the widgets named here.
class ScopedHomeConfig {
  public:
    explicit ScopedHomeConfig(int populated_pages, int total_pages = 2,
                              int temperature_colspan = kCellTracks) {
        auto* cfg = helix::Config::get_instance();
        const std::string key = cfg->df() + "panel_widgets/home";
        had_prior_ = cfg->exists(key);
        if (had_prior_) {
            prior_ = cfg->get<nlohmann::json>(key);
        }
        nlohmann::json pages = nlohmann::json::array();
        pages.push_back(
            page_json("main", "temperature", populated_pages >= 1, temperature_colspan));
        // A different widget id: cross-page drag tests move 'temperature' and
        // must not trip over a same-id twin seeded on the second page.
        if (total_pages >= 2) {
            pages.push_back(page_json("second", "fan", populated_pages >= 2, kCellTracks));
        }
        for (int extra = 2; extra < total_pages; ++extra) {
            pages.push_back(page_json("extra", "clock", false, kCellTracks));
        }
        // Load appends every registry widget the main page does not list, enabled
        // wherever the registry enables it by default, and population would then
        // place those beside this config's own.
        std::set<std::string> listed;
        for (const auto& page : pages) {
            for (const auto& widget : page.at("widgets")) {
                listed.insert(widget.at("id").get<std::string>());
            }
        }
        for (const auto& def : helix::get_all_widget_defs()) {
            if (listed.count(def.id) == 0) {
                pages[0]["widgets"].push_back({{"id", def.id}, {"enabled", false}});
            }
        }
        cfg->set<nlohmann::json>(
            key, nlohmann::json{{"main_page_index", 0}, {"next_page_id", 3}, {"pages", pages}});
        auto& mgr = helix::PanelWidgetManager::instance();
        mgr.get_widget_config("home").mark_dirty();
        mgr.clear_panel_config("home");
    }

    ~ScopedHomeConfig() {
        auto* cfg = helix::Config::get_instance();
        const std::string key = cfg->df() + "panel_widgets/home";
        if (had_prior_) {
            cfg->set<nlohmann::json>(key, prior_);
        } else {
            // No erase API on Config; an empty object reloads as "no configured
            // pages", which is where append_registry_defaults rebuilds page 0.
            cfg->set<nlohmann::json>(key, nlohmann::json::object());
        }
        auto& mgr = helix::PanelWidgetManager::instance();
        mgr.get_widget_config("home").mark_dirty();
        mgr.clear_panel_config("home");
    }

    ScopedHomeConfig(const ScopedHomeConfig&) = delete;
    ScopedHomeConfig& operator=(const ScopedHomeConfig&) = delete;

  private:
    static nlohmann::json page_json(const char* page_id, const char* widget_id, bool populated,
                                    int colspan) {
        nlohmann::json widgets = nlohmann::json::array();
        if (populated) {
            widgets.push_back({{"id", widget_id},
                               {"enabled", true},
                               {"col", 0},
                               {"row", 0},
                               {"colspan", colspan},
                               {"rowspan", kCellTracks}});
        }
        return {{"id", page_id}, {"widgets", widgets}};
    }

    bool had_prior_ = false;
    nlohmann::json prior_;
};

/// A panel widget that offers edit-mode configuration, so its selection chrome
/// carries the configure button beside the remove button.
class ConfigurableTestWidget : public helix::PanelWidget {
  public:
    void attach(lv_obj_t* /*widget_obj*/, lv_obj_t* /*parent_screen*/) override {}
    void detach() override {}
    bool has_edit_configure() const override {
        return true;
    }
    const char* id() const override {
        return "temperature";
    }
};

/// The real widget catalog, for a case in which edit mode opens it: its XML
/// components, and navigation seeded with root panels, since opening the
/// catalog pushes an overlay. Closes a catalog left open, then releases
/// navigation, on exit.
class ScopedWidgetCatalog {
  public:
    ScopedWidgetCatalog() {
        setting_group_register();
        for (const char* file :
             {"A:ui_xml/setting_action_row.xml", "A:ui_xml/widget_catalog_overlay.xml",
              "A:ui_xml/widget_catalog_category_overlay.xml"}) {
            INFO(file);
            REQUIRE(lv_xml_register_component_from_file(file) == LV_RESULT_OK);
        }
        NavigationManager::instance().init();
        for (auto& panel : root_panels_) {
            // Zero-sized, so no root panel takes a press meant for the carousel.
            panel = lv_obj_create(lv_screen_active());
            lv_obj_set_size(panel, 0, 0);
        }
        NavigationManager::instance().set_panels(root_panels_.data());
    }

    ~ScopedWidgetCatalog() {
        auto& nav = NavigationManager::instance();
        for (int i = 0; i < 4 && WidgetCatalogOverlay::active_root(); ++i) {
            nav.go_back();
            settle_queue();
        }
        settle_queue();
        nav.deinit_subjects();
    }

    ScopedWidgetCatalog(const ScopedWidgetCatalog&) = delete;
    ScopedWidgetCatalog& operator=(const ScopedWidgetCatalog&) = delete;

    /// Close the open catalog the way back navigation does.
    static void close() {
        REQUIRE(WidgetCatalogOverlay::active_root() != nullptr);
        NavigationManager::instance().go_back();
        settle_queue();
        REQUIRE(WidgetCatalogOverlay::active_root() == nullptr);
    }

    /// Let a push or a pop land: both run through UpdateQueue, and a pop's
    /// teardown queues a further step.
    static void settle_queue() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler_safe();
        }
    }

  private:
    std::array<lv_obj_t*, kUiPanelCount> root_panels_{};
};

/// Registers the global HomePanel as navigation's Home panel for a scope, as the
/// app does at startup, so a hot-reload rebuild of the active panel reaches it.
/// Declare it after the ScopedWidgetCatalog that seeds navigation.
class ScopedHomePanelInstance {
  public:
    explicit ScopedHomePanelInstance(HomePanel& panel) {
        NavigationManager::instance().register_panel_instance(helix::PanelId::Home, &panel);
    }
    ~ScopedHomePanelInstance() {
        NavigationManager::instance().register_panel_instance(helix::PanelId::Home, nullptr);
    }

    ScopedHomePanelInstance(const ScopedHomePanelInstance&) = delete;
    ScopedHomePanelInstance& operator=(const ScopedHomePanelInstance&) = delete;
};

/// The XML component every stand-in tile is created from: a plain object, not
/// scrollable, so the only thing population gives a tile is its name and cell.
constexpr const char* kStandInComponent = "edit_swipe_stand_in_widget";

/// A stand-in tile holding one button named kFocusControl: a control of a class
/// that joins the default input group as it is created and scrolls itself into
/// view when it gains focus, as a real widget's buttons do.
constexpr const char* kFocusableStandInComponent = "edit_swipe_focusable_stand_in_widget";
constexpr const char* kFocusControl = "focus_control";

/// The widget population places for an id the home config names: its registry
/// definition (spans, snap steps, resizability) is the real one, and its tile is
/// created from @p component.
class StandInWidget : public helix::PanelWidget {
  public:
    StandInWidget(std::string id, const char* component)
        : id_(std::move(id)), component_(component) {}
    void attach(lv_obj_t* /*widget_obj*/, lv_obj_t* /*parent_screen*/) override {}
    void detach() override {}
    const char* id() const override {
        return id_.c_str();
    }
    void on_edit_mode_exited() override {
        ++edit_mode_exits;
    }
    void on_size_changed(int colspan, int rowspan, int /*width_px*/, int /*height_px*/) override {
        last_colspan = colspan;
        last_rowspan = rowspan;
    }

    /// The span the last on_size_changed() carried.
    int last_colspan = 0;
    int last_rowspan = 0;
    /// Times on_edit_mode_exited() ran.
    int edit_mode_exits = 0;
    std::string get_component_name() const override {
        return component_;
    }

  private:
    std::string id_;
    const char* component_;
};

helix::WidgetFactory stand_in_factory(const char* component = kStandInComponent) {
    return [component](const std::string& id) -> std::unique_ptr<helix::PanelWidget> {
        return std::make_unique<StandInWidget>(id, component);
    };
}

/// A default input group for the length of a scope, as DisplayManager installs
/// one for the keyboard: every object of a focusable class joins it as it is
/// created, in creation order.
class ScopedDefaultGroup {
  public:
    ScopedDefaultGroup() : previous_(lv_group_get_default()), group_(lv_group_create()) {
        lv_group_set_default(group_);
    }

    ~ScopedDefaultGroup() {
        lv_group_set_default(previous_);
        lv_group_delete(group_);
    }

    ScopedDefaultGroup(const ScopedDefaultGroup&) = delete;
    ScopedDefaultGroup& operator=(const ScopedDefaultGroup&) = delete;

    lv_group_t* get() const {
        return group_;
    }

    lv_obj_t* focused() const {
        return lv_group_get_focused(group_);
    }

  private:
    lv_group_t* previous_;
    lv_group_t* group_;
};

/// Records every text the page badge (home_page_badge) shows while it lives.
/// The badge names the panel's page, so it changes with every page the panel
/// passes through.
class ScopedBadgeRecorder {
  public:
    ScopedBadgeRecorder() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "home_page_badge");
        REQUIRE(subject != nullptr);
        observer_ = lv_subject_add_observer(subject, record, &shown_);
        shown_.clear(); // adding the observer reports the current text once
    }

    ~ScopedBadgeRecorder() {
        lv_observer_remove(observer_);
    }

    ScopedBadgeRecorder(const ScopedBadgeRecorder&) = delete;
    ScopedBadgeRecorder& operator=(const ScopedBadgeRecorder&) = delete;

    const std::vector<std::string>& shown() const {
        return shown_;
    }

  private:
    static void record(lv_observer_t* observer, lv_subject_t* subject) {
        static_cast<std::vector<std::string>*>(lv_observer_get_user_data(observer))
            ->push_back(lv_subject_get_string(subject));
    }

    lv_observer_t* observer_ = nullptr;
    std::vector<std::string> shown_;
};

/// Records where every animated scroll of the home carousel's page strip is
/// headed, while it lives. It listens on carousel_host, where the strip's scroll
/// events bubble, so it outlasts the strip a rebuild replaces and sees the
/// rebuilt strip's scrolls from the first one.
class ScopedCarouselSlideRecorder {
  public:
    explicit ScopedCarouselSlideRecorder(lv_obj_t* host) : host_(host) {
        REQUIRE(host_ != nullptr);
        lv_obj_add_event_cb(host_, record, LV_EVENT_SCROLL_BEGIN, &destinations_);
    }

    ~ScopedCarouselSlideRecorder() {
        lv_obj_remove_event_cb_with_user_data(host_, record, &destinations_);
    }

    ScopedCarouselSlideRecorder(const ScopedCarouselSlideRecorder&) = delete;
    ScopedCarouselSlideRecorder& operator=(const ScopedCarouselSlideRecorder&) = delete;

    /// The scroll offset each animated scroll of the page strip headed for, in
    /// order.
    const std::vector<int32_t>& destinations() const {
        return destinations_;
    }

  private:
    static void record(lv_event_t* e) {
        // A swipe's drag, or an offset set outright, carries no animation.
        const auto* anim = static_cast<lv_anim_t*>(lv_event_get_param(e));
        const CarouselState* state =
            ui_carousel_get_state(HomePanelTestAccess::carousel(get_global_home_panel()));
        // Scrolls inside the pages bubble here too; only the strip's own count.
        if (!anim || !state || lv_event_get_target_obj(e) != state->scroll_container) {
            return;
        }
        // A scroll animation runs over the negated scroll offset.
        static_cast<std::vector<int32_t>*>(lv_event_get_user_data(e))->push_back(-anim->end_value);
    }

    lv_obj_t* host_;
    std::vector<int32_t> destinations_;
};

/// HomePanel's carousel, built by the panel on a stand-in for home_panel.xml's
/// root. carousel_host carries every grid handler the XML lists, and page events
/// reach them by bubbling: shield -> page container -> tile -> scroll container
/// -> carousel -> host.
///
/// A case seeds the config and builds with build_home(), then drives input
/// through @ref indev. Teardown leaves edit mode, runs the deferred work the
/// case left pending, releases what the build created and settles the deferred
/// deletes before the next case.
class EditHomeFixture : public LVGLTestFixture {
    ScopedLockState lock_state_;
    ScopedInputSettings input_settings_; // documented default: edit mode enabled
    // The ids ScopedHomeConfig places, each built as a StandInWidget.
    ScopedWidgetFactory temperature_factory_{"temperature", stand_in_factory()};
    ScopedWidgetFactory fan_factory_{"fan", stand_in_factory()};
    ScopedWidgetFactory clock_factory_{"clock", stand_in_factory()};
    std::optional<ScopedHomeConfig> home_config_;
    lv_obj_t* root_ = nullptr;

  public:
    ScopedPointerIndev indev;

    EditHomeFixture() {
        REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
        REQUIRE(carousel() == nullptr);

        // The carousel's pages are XML components styled with design tokens, so
        // the XML engine needs the app's globals, theme and widgets; the theme
        // wants no screen present while it initializes.
        lv_obj_delete(m_test_screen);
        m_test_screen = nullptr;
        XMLTestFixture::setup_global_xml_registrations_once();
        create_test_screen();
        REQUIRE(lv_xml_register_component_from_file(
                    "A:ui_xml/components/home_page_container.xml") == LV_RESULT_OK);
        REQUIRE(lv_xml_register_component_from_file(
                    "A:ui_xml/components/home_next_page_slot.xml") == LV_RESULT_OK);
        REQUIRE(lv_xml_register_component_from_data(
                    kStandInComponent, "<component><view extends=\"lv_obj\" scrollable=\"false\" "
                                       "style_pad_all=\"0\"/></component>") == LV_RESULT_OK);
        // The panel's XML event callbacks, as the app registers them before any
        // of its XML is created; once per process.
        if (!panel().are_subjects_initialized()) {
            panel().init_subjects();
        }

        root_ = lv_obj_create(test_screen());
        lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* host = lv_obj_create(root_);
        lv_obj_set_name(host, "carousel_host");
        lv_obj_set_size(host, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(host, 0, LV_PART_MAIN);
        lv_obj_remove_flag(host, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(host, LV_OBJ_FLAG_SCROLLABLE);
        for (const auto& handler : HomePanelTestAccess::grid_handlers()) {
            lv_obj_add_event_cb(host, handler.cb, handler.code, nullptr);
        }

        HomePanelTestAccess::set_panel_root(panel(), root_);
        HomePanelTestAccess::wire_grid_edit_page_callbacks(panel());
    }

    ~EditHomeFixture() override {
        HomePanel& home = panel();
        home.exit_grid_edit_mode();
        // Deferred work the case left pending, such as the page-set rebuild its
        // last drop scheduled or the rebuild the exit schedules, runs against
        // this case's carousel_host before the release below.
        helix::ui::UpdateQueue::instance().drain();
        lv_timer_handler_safe();
        // Back to a panel finalize_setup() has not wired: the global panel
        // outlives this case, and later cases seed pages of their own.
        grid().set_rebuild_callback(nullptr);
        grid().set_relayout_callback(nullptr);
        grid().set_delete_page_callback(nullptr);
        HomePanelTestAccess::release_finalize(home);
        HomePanelTestAccess::release_carousel(home);
        HomePanelTestAccess::set_panel_root(home, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        lv_timer_handler_safe();
    }

    EditHomeFixture(const EditHomeFixture&) = delete;
    EditHomeFixture& operator=(const EditHomeFixture&) = delete;

    /// Seed the home widget config (see ScopedHomeConfig), then run the panel's
    /// carousel build on it.
    void build_home(int populated_pages = 2, int total_pages = 2,
                    int temperature_colspan = kCellTracks) {
        REQUIRE_FALSE(home_config_.has_value());
        home_config_.emplace(populated_pages, total_pages, temperature_colspan);
        HomePanelTestAccess::build_carousel(panel());
        REQUIRE(carousel() != nullptr);
        REQUIRE(page_count() == total_pages);
        lv_obj_update_layout(root_);
    }

    /// build_home() with three pages that each hold one widget: 'temperature' on
    /// page 0, 'fan' on page 1 and 'clock' on page 2.
    void build_home_of_three_pages() {
        build_home(2, 3);
        REQUIRE(config().place_entry("clock", 2, 0, 0, kCellTracks, kCellTracks) >= 0);
        repopulate();
        REQUIRE(HomePanelTestAccess::recount_populated_pages(panel()) == 3);
    }

    HomePanel& panel() {
        return get_global_home_panel();
    }

    /// Register kFocusableStandInComponent, for a ScopedWidgetFactory that
    /// builds a widget holding a focusable control.
    static void register_focusable_stand_in() {
        REQUIRE(lv_xml_register_component_from_data(
                    kFocusableStandInComponent,
                    "<component><view extends=\"lv_obj\" scrollable=\"false\" style_pad_all=\"0\">"
                    "<lv_button name=\"focus_control\" width=\"50%\" height=\"50%\"/>"
                    "</view></component>") == LV_RESULT_OK);
    }

    helix::GridEditMode& grid() {
        return HomePanelTestAccess::grid_edit(panel());
    }

    helix::PanelWidgetConfig& config() {
        return helix::PanelWidgetManager::instance().get_widget_config("home");
    }

    lv_obj_t* carousel() {
        return HomePanelTestAccess::carousel(panel());
    }

    CarouselState* carousel_state() {
        CarouselState* state = ui_carousel_get_state(carousel());
        REQUIRE(state != nullptr);
        return state;
    }

    lv_obj_t* scroller() {
        return carousel_state()->scroll_container;
    }

    int current_page() {
        return ui_carousel_get_current_page(carousel());
    }

    int page_count() {
        return static_cast<int>(HomePanelTestAccess::page_containers(panel()).size());
    }

    /// The container of page @p index.
    lv_obj_t* page(int index) {
        const auto& pages = HomePanelTestAccess::page_containers(panel());
        REQUIRE(index >= 0);
        REQUIRE(index < static_cast<int>(pages.size()));
        return pages[static_cast<size_t>(index)];
    }

    /// The next-page slot's page container, or nullptr at the page cap. Its tile
    /// sits at index page_count().
    /// No event callback on @p obj carries the edit session as its user data.
    void check_no_callback_into_session(lv_obj_t* obj) {
        for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
            lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
            REQUIRE(dsc != nullptr);
            CHECK(lv_event_dsc_get_user_data(dsc) != static_cast<void*>(&grid()));
        }
    }

    /// Where a dragged widget and its selection outline live while the drag
    /// lasts: the top layer, outside every page's grid.
    lv_obj_t* drag_layer() {
        return lv_display_get_layer_top(lv_obj_get_display(root_));
    }

    lv_obj_t* slot_container() {
        return HomePanelTestAccess::next_page_container(panel());
    }

    /// A point on the slot's grid, clear of its centred + group, while the
    /// carousel rests on the slot's tile.
    lv_point_t slot_empty_spot() {
        lv_area_t a;
        lv_obj_get_coords(slot_container(), &a);
        return {static_cast<int32_t>(a.x1 + 20), static_cast<int32_t>(a.y1 + 20)};
    }

    /// The carousel tile holding the next-page slot, at index page_count().
    lv_obj_t* slot_tile() {
        const std::vector<lv_obj_t*>& tiles = carousel_state()->real_tiles;
        REQUIRE(static_cast<int>(tiles.size()) > page_count());
        return tiles[static_cast<size_t>(page_count())];
    }

    /// The next-page slot can be reached: the carousel shows its tile, which it
    /// hides while the slot is out of reach.
    bool slot_in_reach() {
        return !is_hidden(slot_tile());
    }

    /// The carousel swipes: its scroll container scrolls horizontally.
    bool scroll_live() {
        lv_obj_t* scroll = scroller();
        return lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE) &&
               lv_obj_get_scroll_dir(scroll) == LV_DIR_HOR;
    }

    /// The swipe policy the panel last stated to the carousel.
    CarouselSwipe swipe_policy() {
        return carousel_state()->swipe;
    }

    static lv_area_t area_of(lv_obj_t* obj) {
        lv_obj_update_layout(obj);
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        return area;
    }

    static lv_point_t center_of(lv_obj_t* obj) {
        const lv_area_t area = area_of(obj);
        return {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    }

    /// Track geometry of page @p index, derived independently of the edit
    /// session: the breakpoint's track counts for the content box, as population
    /// lays the page out, and the home grid's gutter.
    helix::CellMetrics page_metrics(int index, lv_area_t* content = nullptr) {
        lv_obj_t* container = page(index);
        lv_obj_update_layout(container);
        lv_area_t area;
        lv_obj_get_content_coords(container, &area);
        const int w = lv_area_get_width(&area);
        const int h = lv_area_get_height(&area);
        lv_subject_t* bp = theme_manager_get_breakpoint_subject();
        const UiBreakpoint breakpoint =
            bp ? as_breakpoint(lv_subject_get_int(bp)) : UiBreakpoint::Medium;
        const helix::GridDimensions dims = helix::GridLayout::get_dimensions(breakpoint, w, h);
        if (content) {
            *content = area;
        }
        return helix::grid_cell_metrics(w, h, dims.cols, dims.rows, helix::GridLayout::gutter_px());
    }

    /// Widget @p id as population placed it on page @p index, laid out at the
    /// cell its config entry holds. A rebuild replaces the object, so look it up
    /// again after one.
    lv_obj_t* widget_on(int index, const char* id) {
        lv_obj_update_layout(root_);
        lv_obj_t* obj = lv_obj_get_child_by_name(page(index), id);
        INFO("widget '" << id << "' on page " << index);
        REQUIRE(obj != nullptr);
        return obj;
    }

    /// Populate every page again from the config, as the panel does after a
    /// config change made outside edit mode.
    void repopulate() {
        REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
        panel().populate_widgets();
        lv_obj_update_layout(root_);
    }

    /// Lay out and tear down the panel's current XML root, once a hot-reload
    /// rebuild has replaced the root this fixture built on.
    void adopt_panel_root() {
        root_ = HomePanelTestAccess::panel_root(panel());
        REQUIRE(root_ != nullptr);
        lv_obj_update_layout(root_);
    }

    /// A point on page @p index that no named object covers: the middle of the
    /// page's bottom-right cell.
    lv_point_t empty_spot(int index) {
        lv_area_t content;
        const helix::CellMetrics m = page_metrics(index, &content);
        const lv_point_t spot{
            content.x2 -
                static_cast<int>(helix::grid_track_extent(m.cell_w, m.gutter, kCellTracks) / 2),
            content.y2 -
                static_cast<int>(helix::grid_track_extent(m.cell_h, m.gutter, kCellTracks) / 2)};
        lv_obj_t* container = page(index);
        for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
            lv_obj_t* child = lv_obj_get_child(container, static_cast<int32_t>(i));
            if (!lv_obj_get_name(child)) {
                continue;
            }
            const lv_area_t area = area_of(child);
            INFO("'" << lv_obj_get_name(child) << "' covers " << spot.x << "," << spot.y);
            REQUIRE_FALSE(
                (spot.x >= area.x1 && spot.x <= area.x2 && spot.y >= area.y1 && spot.y <= area.y2));
        }
        return spot;
    }

    /// Show page @p index with no animation through the carousel's goto, which
    /// sets the page subject the panel observes.
    void show_page(int index) {
        if (current_page() != index) {
            ui_carousel_goto_page(carousel(), index, /*animate=*/false);
        }
        REQUIRE(current_page() == index);
        REQUIRE(HomePanelTestAccess::active_page(panel()) == index);
    }

    /// End a swipe on tile @p tile: the scroll comes to rest there and the scroll
    /// container sends SCROLL_END, from which the carousel sets its page.
    void settle_swipe_on(int tile) {
        lv_obj_update_layout(root_);
        const int32_t tile_w = lv_obj_get_content_width(scroller());
        lv_obj_scroll_to_x(scroller(), tile * tile_w, LV_ANIM_OFF);
        REQUIRE(current_page() == tile);
    }

    /// A real swipe from @p start toward the next tile, released two thirds of a
    /// page along, and settled. The snap that ends it is an animated scroll the
    /// swipe's pointer drives. LVGL flings by the last LV_INDEV_VECT_HIST_SIZE
    /// read vectors, and these moves advance no time, so the finger rests for
    /// that many reads before the lift: the snap carries no fling and lands on
    /// the nearest tile the carousel can scroll to.
    void swipe_toward_next_tile(lv_point_t start) {
        constexpr int kMoves = 8;
        const int32_t step = (2 * lv_obj_get_content_width(scroller()) / 3) / kMoves;
        REQUIRE(step > 0);
        indev.press(start.x, start.y);
        int32_t x = start.x;
        for (int i = 0; i < kMoves; ++i) {
            x -= step;
            indev.move(x, start.y);
        }
        indev.hold(LV_INDEV_VECT_HIST_SIZE * kReadPeriodMs);
        REQUIRE(lv_indev_get_scroll_obj(indev.indev()) == scroller());
        indev.release(x, start.y);
        settle();
    }

    /// A commit that changes the page set, watched from before the input that
    /// makes it: the carousel its rebuild replaces, every text the page badge
    /// shows from then on, and every animated scroll of the page strip.
    struct PageSetChange {
        const lv_obj_t* carousel_before = nullptr;
        std::unique_ptr<ScopedBadgeRecorder> badge;
        std::unique_ptr<ScopedCarouselSlideRecorder> slides;
    };

    PageSetChange watch_page_set_change() {
        return {carousel(), std::make_unique<ScopedBadgeRecorder>(),
                std::make_unique<ScopedCarouselSlideRecorder>(
                    lv_obj_find_by_name(root_, "carousel_host"))};
    }

    /// Run the tick a page-set change rebuilds the carousel on, and check where
    /// the rebuilt carousel lands. It comes up showing @p shown. When @p focus is
    /// another page, one slide to it is under way as the tick ends, and this
    /// settles it there; otherwise no scroll animation runs. Either way the page
    /// strip runs no other animated scroll, and the panel's page, which the badge
    /// names, passes through @p shown, then @p focus, and no other page.
    void run_page_set_rebuild(const PageSetChange& change, int shown, int focus) {
        REQUIRE(carousel() == change.carousel_before); // the rebuild waits for the next tick
        lv_timer_handler_safe();
        REQUIRE(carousel() != change.carousel_before);
        lv_obj_update_layout(root_);
        const int32_t tile_w = lv_obj_get_content_width(scroller());
        CHECK(lv_obj_get_scroll_x(scroller()) == shown * tile_w);
        CHECK(current_page() == focus);
        CHECK(HomePanelTestAccess::active_page(panel()) == focus);
        std::vector<std::string> pages_passed{badge_text(shown)};
        if (shown == focus) {
            CHECK(lv_anim_get(scroller(), nullptr) == nullptr);
            CHECK(change.slides->destinations().empty());
        } else {
            CHECK(lv_anim_get(scroller(), nullptr) != nullptr);
            settle();
            CHECK(lv_obj_get_scroll_x(scroller()) == focus * tile_w);
            CHECK(change.slides->destinations() == std::vector<int32_t>{focus * tile_w});
            pages_passed.push_back(badge_text(focus));
        }
        // A text written again for the page it already names counts once.
        std::vector<std::string> badge = change.badge->shown();
        badge.erase(std::unique(badge.begin(), badge.end()), badge.end());
        CHECK(badge == pages_passed);
    }

    /// run_page_set_rebuild() for a rebuild that comes up on @p focus itself, with
    /// nothing to slide.
    void run_page_set_rebuild(const PageSetChange& change, int focus) {
        run_page_set_rebuild(change, focus, focus);
    }

    /// The page badge's text for page @p page of the carousel's pages.
    std::string badge_text(int page) {
        return std::to_string(page + 1) + " / " + std::to_string(page_count());
    }

    /// Enter edit mode on page @p index the way a finger does: show the page,
    /// hold still on an empty spot of it until the long press lands, and lift.
    /// Entry resets the indev, so the lift reaches no handler.
    void enter_edit_mode(int index = 0) {
        show_page(index);
        const lv_point_t spot = empty_spot(index);
        indev.press(spot.x, spot.y);
        indev.hold(indev.long_press_hold_ms());
        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        indev.release(spot.x, spot.y);
        REQUIRE(grid().page_index() == index);
        REQUIRE(grid().selected_widget() == nullptr);
        // The geometry this fixture places and aims with is the session's.
        const helix::CellMetrics live = GridEditModeTestAccess::cell_metrics(grid());
        const helix::CellMetrics derived = page_metrics(index);
        REQUIRE(live.cols == derived.cols);
        REQUIRE(live.rows == derived.rows);
        REQUIRE(live.gutter == derived.gutter);
    }

    /// Pump virtual time, with deferred work and one-shot timers run, until no
    /// animation runs (a page slide, a scroll throw).
    void settle() {
        constexpr uint32_t kSettleLimitMs = 5000;
        uint32_t waited = 0;
        do {
            process_lvgl(static_cast<int>(kReadPeriodMs));
            waited += kReadPeriodMs;
        } while (lv_anim_count_running() > 0 && waited < kSettleLimitMs);
        REQUIRE(lv_anim_count_running() == 0);
    }

    /// After a flip or a drop, once settled: the carousel shows the page the
    /// session is scoped to, that page's container (past the last page, the
    /// slot's) is the one at rest in the viewport, the session's shield,
    /// selection and chrome are valid and live in it, and the next-page slot is
    /// within reach exactly while a drag is live or the session is on the slot.
    void check_session_on_screen() {
        lv_obj_update_layout(root_);
        const int session_page = grid().page_index();
        CHECK(current_page() == session_page);
        REQUIRE(session_page >= 0);
        REQUIRE(session_page <= page_count());
        const bool on_slot = session_page == page_count();
        lv_obj_t* container = on_slot ? slot_container() : page(session_page);
        REQUIRE(container != nullptr);
        CHECK(GridEditModeTestAccess::container(grid()) == container);
        lv_area_t viewport;
        lv_obj_get_content_coords(scroller(), &viewport);
        CHECK(area_of(container).x1 == viewport.x1);
        lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
        REQUIRE(shield != nullptr);
        // Before anything reads it: a shield the session outlived is freed.
        REQUIRE(helix::test::widget_in_tree(shield));
        CHECK(lv_obj_get_parent(shield) == container);
        lv_obj_t* selection_home =
            GridEditModeTestAccess::dragging(grid()) ? drag_layer() : container;
        for (lv_obj_t* obj :
             {grid().selected_widget(), GridEditModeTestAccess::selection_overlay(grid())}) {
            if (obj) {
                REQUIRE(helix::test::widget_in_tree(obj));
                CHECK(lv_obj_get_parent(obj) == selection_home);
            }
        }
        if (slot_container()) {
            // The slot's tile is the + affordance: always shown, drag or not.
            CHECK(slot_in_reach());
        }
    }

    /// A tap on widget @p id on the page the session is scoped to selects it,
    /// which redraws the lattice on the session's shield.
    void select_by_tap(const char* id) {
        lv_obj_t* target = widget_on(grid().page_index(), id);
        const lv_point_t c = center_of(target);
        indev.press(c.x, c.y);
        indev.release(c.x, c.y);
        REQUIRE(grid().selected_widget() == target);
    }

    /// Pointer travel from a press that crosses the drag threshold.
    static int past_drag_threshold() {
        return GridEditModeTestAccess::drag_threshold_px() + 1;
    }

    /// The content area a page occupies with the carousel at rest: the carousel
    /// viewport inset by the page container's padding. Derived from the
    /// carousel, not read back from the edit session that measures against it.
    lv_area_t settled_page_area() {
        lv_obj_update_layout(root_);
        lv_area_t area;
        lv_obj_get_content_coords(scroller(), &area);
        area.x1 += lv_obj_get_style_space_left(page(0), LV_PART_MAIN);
        area.x2 -= lv_obj_get_style_space_right(page(0), LV_PART_MAIN);
        return area;
    }

    /// The edge zone the edit session derives from its live grid.
    int edge_zone_px() {
        return helix::cross_page_edge_zone_px(GridEditModeTestAccess::cell_metrics(grid()).cell_w);
    }

    /// The edge push speed the edit session derives from its live grid.
    int push_px_per_s() {
        return helix::cross_page_push_px_per_s(GridEditModeTestAccess::cell_metrics(grid()).cell_w);
    }

    /// Track-to-track pitch of the live grid, in px.
    float track_pitch_px() {
        const helix::CellMetrics m = GridEditModeTestAccess::cell_metrics(grid());
        return m.cell_w + static_cast<float>(m.gutter);
    }

    /// Pointer travel that carries a dragged widget @p tracks columns, in px.
    int tracks_px(int tracks) {
        return static_cast<int>(std::lround(track_pitch_px() * static_cast<float>(tracks)));
    }

    int count_on_page(size_t index, const char* id) {
        const auto& entries = config().page_entries(index);
        return static_cast<int>(std::count_if(entries.begin(), entries.end(),
                                              [&](const auto& e) { return e.id == id; }));
    }

    /// A copy of widget @p id's config entry on page @p index.
    helix::PanelWidgetEntry entry_on_page(size_t index, const char* id) {
        const auto& entries = config().page_entries(index);
        const auto entry =
            std::find_if(entries.begin(), entries.end(),
                         [&](const helix::PanelWidgetEntry& e) { return e.id == id; });
        REQUIRE(entry != entries.end());
        return *entry;
    }

    /// Timers of the edit session that can still fire: the session is their
    /// user data, and they keep a callback and a run. A cancel neuters a timer
    /// and leaves it linked until lv_timer_handler() runs, and the test pump
    /// never deletes one.
    int armed_session_timer_count() {
        int count = 0;
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
            if (lv_timer_get_user_data(t) == &grid() && t->timer_cb != nullptr &&
                t->repeat_count != 0) {
                ++count;
            }
        }
        return count;
    }

    /// Hold still on an empty spot of the session's page until the long press
    /// opens the widget catalog at that cell, and lift. Needs a
    /// ScopedWidgetCatalog. Returns the spot.
    lv_point_t open_catalog_by_hold() {
        const lv_point_t spot = empty_spot(grid().page_index());
        indev.press(spot.x, spot.y);
        indev.hold(indev.long_press_hold_ms());
        indev.release(spot.x, spot.y);
        ScopedWidgetCatalog::settle_queue();
        REQUIRE(grid().is_catalog_open());
        REQUIRE(WidgetCatalogOverlay::active_root() != nullptr);
        return spot;
    }

    /// Delete navigation's dismiss backdrop behind the open widget catalog, the
    /// state a backdrop whose snapshot could not be allocated leaves, so a press
    /// at @p beside, a point the catalog leaves uncovered, reaches the home grid.
    void remove_dismiss_backdrop(lv_point_t beside) {
        const auto screen_child_of = [](lv_obj_t* obj) {
            REQUIRE(obj != nullptr);
            REQUIRE(obj != lv_screen_active());
            while (lv_obj_get_parent(obj) != lv_screen_active()) {
                obj = lv_obj_get_parent(obj);
            }
            return obj;
        };
        lv_obj_t* backdrop = screen_child_of(lv_indev_search_obj(lv_screen_active(), &beside));
        REQUIRE(backdrop != root_);
        REQUIRE(backdrop != WidgetCatalogOverlay::active_root());
        lv_obj_delete(backdrop);
        CHECK(screen_child_of(lv_indev_search_obj(lv_screen_active(), &beside)) == root_);
    }

    /// Grab @p widget's bottom edge, drag it one cell down and lift: a resize its
    /// release commits, easing into its cell while animations are on.
    void resize_down_one_cell(lv_obj_t* widget) {
        const lv_area_t wa = area_of(widget);
        const int x = (wa.x1 + wa.x2) / 2;
        const int y = wa.y2 - GridEditModeTestAccess::edge_hit_band(grid()) / 2;
        REQUIRE(grid().detect_resize_edge(x, y, wa) == helix::GridEditMode::ResizeEdge::Bottom);
        indev.grab(x, y);
        const helix::CellMetrics m = GridEditModeTestAccess::cell_metrics(grid());
        const int cell_px = static_cast<int>(std::lround(kCellTracks * (m.cell_h + m.gutter)));
        indev.move(x, y + past_drag_threshold());
        REQUIRE(GridEditModeTestAccess::resizing(grid()));
        indev.move(x, y + cell_px);
        indev.release(x, y + cell_px);
    }

    /// Hold a live drag's pointer at height @p y in the edge zone toward @p dir
    /// until the dwell flips a page and the slide settles. Returns the pointer.
    lv_point_t dwell_flip(int dir, int y) {
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        const lv_area_t frame = settled_page_area();
        const int zone = edge_zone_px();
        const lv_point_t pointer{dir > 0 ? frame.x2 - zone / 2 : frame.x1 + zone / 2, y};
        const int landing = grid().page_index() + dir;
        indev.move(pointer.x, pointer.y);
        process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
        settle();
        REQUIRE(grid().page_index() == landing);
        REQUIRE(current_page() == landing);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        return pointer;
    }

    /// Grab @p widget, cross the drag threshold and carry it one cell right, so
    /// the drag is live with its snap preview drawn and the swipe locked.
    /// Returns the pointer, still down.
    lv_point_t drag_one_cell(lv_obj_t* widget) {
        const lv_point_t c = center_of(widget);
        indev.grab(c.x, c.y);
        const int start_x = c.x + past_drag_threshold();
        indev.move(start_x, c.y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        const lv_point_t carried{start_x + tracks_px(kCellTracks), c.y};
        indev.move(carried.x, carried.y);
        REQUIRE(GridEditModeTestAccess::snap_preview(grid()) != nullptr);
        REQUIRE_FALSE(scroll_live());
        return carried;
    }

    /// The gesture on the widget @p origin was taken from ended with nothing
    /// committed: no press or drag is armed, the snap preview and the selection
    /// are gone, the swipe is live again, the widget on page 0 does not float,
    /// and its entry there still holds @p origin's cell, which is where the
    /// rebuild the end schedules lays it out.
    void check_ended_uncommitted(const helix::PanelWidgetEntry& origin) {
        CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
        CHECK_FALSE(GridEditModeTestAccess::dragging(grid()));
        CHECK(GridEditModeTestAccess::snap_preview(grid()) == nullptr);
        CHECK(grid().selected_widget() == nullptr);
        CHECK(scroll_live());
        CHECK_FALSE(lv_obj_has_flag(widget_on(0, origin.id.c_str()), LV_OBJ_FLAG_FLOATING));
        const helix::PanelWidgetEntry now = entry_on_page(0, origin.id.c_str());
        CHECK(now.col == origin.col);
        CHECK(now.row == origin.row);
    }

    /// What a drag carried onto the next-page slot saw on the way.
    struct SlotDrag {
        lv_point_t start{};     ///< Where the drag crossed the drag threshold
        lv_point_t pointer{};   ///< Still pressed here, at the last page's right edge
        int carousel_flips = 0; ///< Changes of the carousel's page during the hold
        int session_flips = 0;  ///< Changes of the session's page during the hold
    };

    /// Grab @p widget on the last page, carry it to the page's right edge, and
    /// hold there at the read cadence for long enough that the push crosses the
    /// border, the crossing flips onto the slot, the slide settles, and a dwell
    /// could have run out after it. Returns with the drag live on the slot.
    SlotDrag drag_onto_slot(lv_obj_t* widget) {
        REQUIRE(slot_container() != nullptr);
        const lv_point_t c = center_of(widget);
        indev.grab(c.x, c.y);
        indev.move(c.x + past_drag_threshold(), c.y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));

        SlotDrag drag;
        drag.start = {c.x + past_drag_threshold(), c.y};
        drag.pointer = {settled_page_area().x2, c.y};
        int carousel_page = current_page();
        int session_page = grid().page_index();
        indev.move(drag.pointer.x, drag.pointer.y);
        indev.hold(2 * helix::kCrossPageDwellMs, [&]() {
            if (current_page() != carousel_page) {
                ++drag.carousel_flips;
                carousel_page = current_page();
            }
            if (grid().page_index() != session_page) {
                ++drag.session_flips;
                session_page = grid().page_index();
            }
        });
        REQUIRE(grid().page_index() == page_count());
        REQUIRE(lv_anim_count_running() == 0);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        return drag;
    }

    /// Grab @p widget on the first page, carry it to the page's left edge, and
    /// hold there at the read cadence for long enough that the push crosses the
    /// border and a dwell could have run out after it. Nothing sits before the
    /// first page, so no flip happens. Returns with the drag live past the
    /// first page's left border.
    SlotDrag drag_past_left_border(lv_obj_t* widget) {
        const lv_point_t c = center_of(widget);
        indev.grab(c.x, c.y);
        indev.move(c.x - past_drag_threshold(), c.y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));

        SlotDrag drag;
        drag.start = {c.x - past_drag_threshold(), c.y};
        drag.pointer = {settled_page_area().x1, c.y};
        int carousel_page = current_page();
        int session_page = grid().page_index();
        indev.move(drag.pointer.x, drag.pointer.y);
        indev.hold(2 * helix::kCrossPageDwellMs, [&]() {
            if (current_page() != carousel_page) {
                ++drag.carousel_flips;
                carousel_page = current_page();
            }
            if (grid().page_index() != session_page) {
                ++drag.session_flips;
                session_page = grid().page_index();
            }
        });
        REQUIRE(grid().page_index() == 0);
        REQUIRE(lv_anim_count_running() == 0);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        return drag;
    }
};

} // namespace

TEST_CASE_METHOD(EditHomeFixture, "edit mode keeps the carousel swipeable with nothing selected",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();

    // Precondition: two real pages resolve the swipe policy to swiping, so the
    // flag this case checks is one edit mode could take away.
    REQUIRE(carousel_state()->real_page_count == 2);
    REQUIRE(scroll_live());

    enter_edit_mode();

    // With no widget holding the pointer, the swipe surface stays live so an
    // empty-area drag can flip pages.
    CHECK(scroll_live());
    CHECK(swipe_policy() == CarouselSwipe::Auto);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a single-page home swipes to the next-page slot's +, in edit mode or out of it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1);

    // The only page has the slot's + tile past it, so the strip swipes.
    REQUIRE(carousel_state()->real_page_count == 1);
    REQUIRE(slot_container() != nullptr);
    CHECK(scroll_live());
    CHECK(slot_in_reach());

    enter_edit_mode();
    REQUIRE_FALSE(grid().owns_gesture());

    // Edit mode swipes to the + too: the slot carries the affordance that adds
    // a page, and stays a drag's drop target.
    CHECK(scroll_live());
    CHECK(slot_in_reach());

    panel().exit_grid_edit_mode();
    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(scroll_live());
    CHECK(slot_in_reach());
}

TEST_CASE_METHOD(EditHomeFixture, "leaving edit mode re-enables the carousel swipe",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // A held gesture, so the re-enable below has an actual lock to release.
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    REQUIRE_FALSE(scroll_live());

    // Either exit must release the held gesture, not leave the swipe dead until
    // the next edit session.
    SECTION("Done") {
        panel().exit_grid_edit_mode();
    }
    SECTION("deactivation") {
        panel().on_deactivating(DeactivateReason::NavigateAway);
    }
    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(scroll_live());
    CHECK(swipe_policy() == CarouselSwipe::Auto);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "focus moving onto a control on another page leaves the carousel on its page",
                 "[1638][edit-swipe][home]") {
    register_focusable_stand_in();
    ScopedDefaultGroup group;
    // A button outside the carousel, as the navbar's Done is. Created first, it
    // leads the group's order, so focus it passes on lands on the next control
    // created: the one population places on page 0.
    lv_obj_t* outside = lv_button_create(test_screen());
    ScopedWidgetFactory focusable_temperature{"temperature",
                                              stand_in_factory(kFocusableStandInComponent)};
    build_home();
    const int32_t tile_w = lv_obj_get_content_width(scroller());
    REQUIRE(tile_w > 0);

    // A focused object that hides passes focus to the next in the group, as Done
    // does when leaving edit mode hides it.
    auto pass_focus_on_from_outside = [&]() {
        lv_obj_t* control = lv_obj_find_by_name(widget_on(0, "temperature"), kFocusControl);
        REQUIRE(control != nullptr);
        REQUIRE(lv_obj_get_group(control) == group.get());
        lv_group_focus_obj(outside);
        REQUIRE(group.focused() == outside);
        lv_obj_add_flag(outside, LV_OBJ_FLAG_HIDDEN);
        REQUIRE(group.focused() == control);
    };

    SECTION("outside edit mode") {
        show_page(1);
        pass_focus_on_from_outside();
        settle();
        CHECK(current_page() == 1);
        CHECK(HomePanelTestAccess::active_page(panel()) == 1);
        CHECK(lv_obj_get_scroll_x(scroller()) == tile_w);
    }

    SECTION("in edit mode") {
        enter_edit_mode(1);
        pass_focus_on_from_outside();
        settle();
        CHECK(grid().page_index() == 1);
        check_session_on_screen();
    }

    SECTION("in edit mode, onto a control on the next-page slot") {
        enter_edit_mode(1);
        REQUIRE(slot_container() != nullptr);
        lv_obj_t* slot_control = lv_button_create(slot_container());
        lv_group_focus_obj(slot_control);
        REQUIRE(group.focused() == slot_control);
        settle();
        CHECK(grid().page_index() == 1);
        check_session_on_screen();
    }

    SECTION("a control on the page shown still scrolls into view inside that page") {
        show_page(1);
        lv_obj_t* box = lv_obj_create(widget_on(1, "fan"));
        lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
        lv_obj_t* below_fold = lv_button_create(box);
        lv_obj_update_layout(box);
        lv_obj_set_y(below_fold, 2 * lv_obj_get_content_height(box));
        lv_obj_update_layout(box);
        REQUIRE(lv_obj_get_scroll_y(box) == 0);
        REQUIRE(lv_obj_get_scroll_bottom(box) > 0);
        lv_group_focus_obj(below_fold);
        settle();
        CHECK(group.focused() == below_fold);
        CHECK(lv_obj_get_scroll_y(box) > 0);
        CHECK(current_page() == 1);
    }
}

TEST_CASE_METHOD(EditHomeFixture, "a pointer swipe past half a page settles on the next page",
                 "[1638][edit-swipe][home]") {
    build_home();
    REQUIRE(scroll_live());
    REQUIRE(current_page() == 0);
    const int32_t tile_w = lv_obj_get_content_width(scroller());
    REQUIRE(tile_w > 0);

    swipe_toward_next_tile(empty_spot(0));

    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(current_page() == 1);
    CHECK(HomePanelTestAccess::active_page(panel()) == 1);
    CHECK(lv_obj_get_scroll_x(scroller()) == tile_w);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a widget press owns the gesture and locks the swipe until release",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    REQUIRE(scroll_live());

    // The two-step grab: one press selects the widget, and a fresh press on the
    // selected widget arms on its first PRESSING - the moment the lock must
    // engage, before any movement could make LVGL adopt the scroll instead.
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    CHECK_FALSE(scroll_live());
    CHECK(swipe_policy() == CarouselSwipe::Disabled);
    // An armed press is no drag yet: the + tile stays within reach regardless.
    CHECK(slot_in_reach());

    // Still pressed, moved less than the drag threshold: the lock holds.
    const int nudge = GridEditModeTestAccess::drag_threshold_px() / 2;
    indev.move(c.x + nudge, c.y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    CHECK_FALSE(scroll_live());

    // Release returns the swipe to the carousel.
    indev.release(c.x + nudge, c.y);
    CHECK(scroll_live());
    CHECK(swipe_policy() == CarouselSwipe::Auto);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a session exited with a press armed starts the next session with no gesture",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // The session's own exit with the grab's second press still armed, as its
    // destructor runs it: nothing ends the gesture before the exit does.
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    grid().exit();
    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    indev.release(c.x, c.y);
    // The rebuild the exit schedules runs on the next tick, before any later read.
    settle();

    // A hold enters the next session. Its press reached no session, so only the
    // exit can have cleared the armed press, and the swipe policy the entry
    // states reads what the exit left.
    const lv_point_t spot = empty_spot(0);
    indev.press(spot.x, spot.y);
    indev.hold(indev.long_press_hold_ms());
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK_FALSE(grid().owns_gesture());
    CHECK(swipe_policy() == CarouselSwipe::Auto);
    indev.release(spot.x, spot.y);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "an indev reset mid-drag ends the drag uncommitted with no release",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");

    const lv_point_t pointer = drag_one_cell(widget);

    // Every pointer forgets its press target with the finger still down, as
    // navigation does on a panel switch or an overlay push or pop. No read
    // follows, so no RELEASED can be what ends the drag.
    lv_indev_reset(nullptr, nullptr);
    check_ended_uncommitted(origin);

    indev.release(pointer.x, pointer.y);
    settle();
    check_ended_uncommitted(origin);
    CHECK(count_on_page(0, "temperature") == 1);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "an indev reset ends an armed press, and the next press does not inherit it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    REQUIRE_FALSE(GridEditModeTestAccess::dragging(grid()));
    REQUIRE_FALSE(scroll_live());

    // The press is armed and not yet a drag when the indev resets.
    lv_indev_reset(indev.indev(), nullptr);
    CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
    CHECK(grid().selected_widget() == nullptr);
    CHECK(scroll_live());
    indev.release(c.x, c.y);

    // A fresh press on empty area, then a move past the drag threshold: nothing
    // classifies it against the press that ended.
    const lv_point_t spot = empty_spot(0);
    indev.prime(spot.x, spot.y);
    indev.move(spot.x, spot.y - past_drag_threshold());
    CHECK_FALSE(GridEditModeTestAccess::dragging(grid()));
    CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
    CHECK(grid().selected_widget() == nullptr); // tapped empty
    CHECK(scroll_live());
    indev.release(spot.x, spot.y - past_drag_threshold());
}

TEST_CASE_METHOD(EditHomeFixture, "a press lost to a wait for release ends the drag at the lift",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");

    const lv_point_t pointer = drag_one_cell(widget);

    // Input waits for the lift: reads while the finger stays down reach no
    // handler, and the drag stays live until the lift.
    lv_indev_wait_release(indev.indev());
    indev.move(pointer.x + tracks_px(kCellTracks), pointer.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    REQUIRE_FALSE(scroll_live());

    // The lift sends PRESS_LOST to the press target in place of RELEASED.
    indev.release(pointer.x, pointer.y);
    check_ended_uncommitted(origin);
    settle();
    CHECK(count_on_page(0, "temperature") == 1);
}

TEST_CASE_METHOD(EditHomeFixture, "a modal closed mid-drag ends the drag uncommitted",
                 "[1638][edit-swipe][home][grid_edit]") {
    constexpr const char* kModalComponent = "edit_swipe_test_modal";
    REQUIRE(lv_xml_register_component_from_data(
                kModalComponent,
                "<component><view extends=\"lv_obj\" width=\"50%\" height=\"50%\"/></component>") ==
            LV_RESULT_OK);
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");

    const lv_point_t pointer = drag_one_cell(widget);

    // Opening a modal over the drag leaves it live: the press stays locked to
    // its target under the new backdrop.
    lv_obj_t* dialog = Modal::show(kModalComponent);
    REQUIRE(dialog != nullptr);
    indev.move(pointer.x, pointer.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Closing it resets input.
    Modal::hide(dialog);
    check_ended_uncommitted(origin);

    indev.release(pointer.x, pointer.y);
    settle();
    CHECK(count_on_page(0, "temperature") == 1);
}

TEST_CASE_METHOD(
    EditHomeFixture,
    "an indev reset during a drag held over the next-page slot leaves the widget on its "
    "page",
    "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const SlotDrag drag = drag_onto_slot(widget);
    REQUIRE(lv_obj_get_parent(widget) == drag_layer());

    lv_indev_reset(nullptr, nullptr);
    CHECK_FALSE(GridEditModeTestAccess::dragging(grid()));
    CHECK(grid().page_index() == 0);
    CHECK(lv_obj_get_parent(widget) == page(0));
    // The drag went home with the slot still in reach: the carousel is on
    // page 0 and slides there from the slot.
    CHECK(current_page() == 0);
    CHECK(slot_in_reach());
    CHECK(lv_anim_get(scroller(), nullptr) != nullptr);

    indev.release(drag.pointer.x, drag.pointer.y);
    settle();

    // The rebuild the end scheduled laid the widget out on its page again.
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    widget = widget_on(0, "temperature");
    CHECK_FALSE(lv_obj_has_flag(widget, LV_OBJ_FLAG_FLOATING));
    CHECK(lv_obj_get_child_by_name(slot_container(), "temperature") == nullptr);
    CHECK(current_page() == 0);
    CHECK(grid().page_index() == 0);
    CHECK(config().page_count() == 1);
    CHECK(count_on_page(0, "temperature") == 1);
    CHECK(slot_in_reach());
    CHECK(scroll_live()); // the + tile past the single page swipes
}

TEST_CASE_METHOD(EditHomeFixture, "a drag and its drop leave no timer of the edit session behind",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    REQUIRE(armed_session_timer_count() == 0);
    const lv_area_t frame = settled_page_area();

    SECTION("a drop inside the page") {
        const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");
        const lv_point_t pointer = drag_one_cell(widget);
        CHECK(armed_session_timer_count() == 0);
        indev.release(pointer.x, pointer.y);
        settle();
        // The drop committed, so the release ran its whole path.
        REQUIRE(entry_on_page(0, "temperature").col == origin.col + kCellTracks);
    }
    SECTION("a drop while the edge dwell counts") {
        // Grabbed at its center, the widget's majority stays inside the page
        // with the pointer in the edge zone.
        const lv_point_t c = center_of(widget);
        indev.grab(c.x, c.y);
        indev.move(c.x + past_drag_threshold(), c.y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        const lv_point_t zone{frame.x2 - edge_zone_px() / 2, c.y};
        indev.move(zone.x, zone.y);
        REQUIRE(armed_session_timer_count() == 1);
        indev.release(zone.x, zone.y);
    }
    SECTION("a drop while a crossing's flip is pending") {
        // Grabbed left of center, so one move to the page's right edge carries
        // the widget's majority past the border.
        const lv_area_t wa = area_of(widget);
        const int grab_x = wa.x1 + lv_area_get_width(&wa) * 3 / 10;
        const int y = (wa.y1 + wa.y2) / 2;
        indev.grab(grab_x, y);
        indev.move(grab_x - past_drag_threshold(), y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        indev.move(frame.x2, y);
        REQUIRE(armed_session_timer_count() == 1);
        REQUIRE(grid().page_index() == 0);
        indev.release(frame.x2, y);
    }
    CHECK(armed_session_timer_count() == 0);
    settle();
    CHECK(armed_session_timer_count() == 0);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drag after an indev reset and its rebuild presses a new shield and commits",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");

    // A reset ends a drag, and the rebuild that end schedules replaces the
    // page's children, the shield among them.
    lv_point_t pointer = drag_one_cell(widget);
    lv_indev_reset(nullptr, nullptr);
    indev.release(pointer.x, pointer.y);
    settle();
    REQUIRE(entry_on_page(0, "temperature").col == origin.col);
    widget = lv_obj_get_child_by_name(page(0), "temperature");
    REQUIRE(widget != nullptr);

    // A fresh grab on the rebuilt widget drags with its press on the session's
    // shield, where a further reset would reach it, and the release commits.
    pointer = drag_one_cell(widget);
    lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);
    CHECK(lv_obj_get_parent(shield) == page(0));
    CHECK(indev.indev()->pointer.act_obj == shield);
    indev.release(pointer.x, pointer.y);
    settle();
    CHECK(entry_on_page(0, "temperature").col == origin.col + kCellTracks);
}

TEST_CASE_METHOD(EditHomeFixture, "the event shield carries no callback into the edit session",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    const lv_point_t c = center_of(widget_on(0, "temperature"));
    enter_edit_mode();

    // The shield outlives the session whenever the rebuild that deletes it
    // never runs, so it must hold nothing that calls into the session: its
    // events reach the grid handlers by bubbling to carousel_host, and the
    // callbacks it does carry (the lattice draw and its cleanup) read only
    // what the shield itself owns.
    lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);
    check_no_callback_into_session(shield);

    // The same holds for the shield a cancel's rebuild creates.
    indev.grab(c.x, c.y);
    REQUIRE(grid().owns_gesture());
    lv_indev_reset(nullptr, nullptr);
    indev.release(c.x, c.y);
    settle();
    shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);
    check_no_callback_into_session(shield);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "an input reset scoped to another subtree leaves the drag to its release",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");
    lv_point_t pointer = drag_one_cell(widget);

    // Toast teardown resets input this way, scoped to the dying toast.
    lv_obj_t* other = lv_obj_create(test_screen());
    lv_obj_add_flag(other, LV_OBJ_FLAG_HIDDEN);
    helix::ui::reset_input_within(other);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    REQUIRE_FALSE(scroll_live());

    // The drag carries on one more cell, and its release commits there.
    pointer.x += tracks_px(kCellTracks);
    indev.move(pointer.x, pointer.y);
    indev.release(pointer.x, pointer.y);
    settle();
    CHECK(entry_on_page(0, "temperature").col == origin.col + 2 * kCellTracks);
}

TEST_CASE_METHOD(EditHomeFixture, "leaving edit mode re-arms every page without rebuilding it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    lv_obj_t* far_widget = widget_on(1, "fan");
    REQUIRE(is_clickable(widget));
    REQUIRE(is_clickable(far_widget));
    enter_edit_mode();
    REQUIRE_FALSE(is_clickable(widget));
    REQUIRE_FALSE(is_clickable(far_widget));
    lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);

    panel().exit_grid_edit_mode();
    settle();

    // The pages keep the objects the session arranged; only its own go.
    CHECK(widget_on(0, "temperature") == widget);
    CHECK(widget_on(1, "fan") == far_widget);
    CHECK(is_clickable(widget));
    CHECK(is_clickable(far_widget));
    CHECK_FALSE(helix::test::widget_in_tree(shield));
    // Every page's widgets get to re-apply interactivity the restore overwrote.
    for (lv_obj_t* tile : {widget, far_widget}) {
        auto* stand_in = static_cast<StandInWidget*>(lv_obj_get_user_data(tile));
        REQUIRE(stand_in != nullptr);
        CHECK(stand_in->edit_mode_exits == 1);
    }
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a widget's own config change during edit mode rebuilds the pages at exit",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    HomePanelTestAccess::register_config_rebuild_callback(panel());
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // A thermistor switching between single and carousel: the widget list is
    // unchanged, but the tile needs a different component.
    config().set_widget_config("temperature", {{"mode", "carousel"}});
    config().save();
    helix::PanelWidgetManager::instance().notify_config_changed("home");
    settle();
    REQUIRE(widget_on(0, "temperature") == widget); // waits out the session

    panel().exit_grid_edit_mode();
    settle();
    CHECK(widget_on(0, "temperature") != widget);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a layout reset during edit mode that keeps the widget list applies at exit",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    HomePanelTestAccess::register_config_rebuild_callback(panel());
    enter_edit_mode();

    // The catalog's reset with defaults naming the same widgets: positions move,
    // the page's id list does not.
    const int new_col = 2 * kCellTracks;
    REQUIRE(config().place_entry("temperature", 0, new_col, 0, kCellTracks, kCellTracks) >= 0);
    config().save();
    helix::PanelWidgetManager::instance().notify_config_changed("home");
    settle();

    panel().exit_grid_edit_mode();
    settle();
    lv_obj_t* widget = widget_on(0, "temperature");
    REQUIRE(widget != nullptr);
    CHECK(lv_obj_get_style_grid_cell_column_pos(widget, LV_PART_MAIN) == new_col);
}

namespace {

/// The column the stored home layout gives @p id on page 0, or -1.
int stored_home_col(const char* id) {
    auto* cfg = Config::get_instance();
    const auto node = cfg->get<nlohmann::json>(cfg->df() + "panel_widgets/home", nlohmann::json());
    if (!node.is_object() || !node.contains("pages") || node["pages"].empty()) {
        return -1;
    }
    for (const auto& w : node["pages"][0]["widgets"]) {
        if (w.value("id", "") == id) {
            return w.value("col", -1);
        }
    }
    return -1;
}

} // namespace

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop's save waits for edits to settle, and every end writes it",
                 "[1638][edit-swipe][home][grid_edit][deferred_save]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const int stored_before = stored_home_col("temperature");
    const lv_point_t pointer = drag_one_cell(widget);
    indev.release(pointer.x, pointer.y);
    settle();
    const int moved_col = entry_on_page(0, "temperature").col;
    REQUIRE(moved_col != stored_before);

    // A flash write stalls a slow board for hundreds of ms per drop, so the
    // drop only asks for one.
    REQUIRE(config().save_pending());
    CHECK(stored_home_col("temperature") == stored_before);

    SECTION("leaving edit mode") {
        panel().exit_grid_edit_mode();
    }
    SECTION("a hot-reload rebuild of the panel") {
        panel().on_deactivating(DeactivateReason::Rebuild);
    }
    SECTION("the edits settling") {
        process_lvgl(static_cast<int>(helix::PanelWidgetConfig::kSaveSettleMs) + 100);
    }
    CHECK_FALSE(config().save_pending());
    CHECK(stored_home_col("temperature") == moved_col);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "leaving edit mode before a drop's relayout runs still lays out the drop",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const lv_point_t pointer = drag_one_cell(widget);
    // The release commits the move and leaves the re-seat for the next tick;
    // the session ends first.
    indev.release(pointer.x, pointer.y);
    const int committed_col = entry_on_page(0, "temperature").col;
    panel().exit_grid_edit_mode();
    settle();

    lv_obj_t* landed = widget_on(0, "temperature");
    REQUIRE(landed != nullptr);
    CHECK(lv_obj_get_style_grid_cell_column_pos(landed, LV_PART_MAIN) == committed_col);
}

TEST_CASE_METHOD(EditHomeFixture, "entering edit mode disarms clicks on every page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* far_widget = widget_on(1, "fan");
    REQUIRE(is_clickable(far_widget));

    enter_edit_mode();

    // The page this session did not enter is still part of the carousel a swipe
    // can settle on mid-flight; its widgets must not fire real handlers until
    // the session ends.
    CHECK_FALSE(is_clickable(far_widget));
}

TEST_CASE_METHOD(EditHomeFixture, "a multi-frame resize commits and tears down its preview",
                 "[1638][edit-swipe][home][grid_edit]") {
    // With animations off the commit deletes the preview itself; with them on
    // it hands the preview to the snap animation, and the rebuild deletes it.
    helix::ui::ScopedAnimationsEnabled animations(false);
    REQUIRE_FALSE(DisplaySettingsManager::instance().get_animations_enabled());
    build_home(1); // temperature at (0,0) on page 0
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const auto orig = config().page_entries(0)[0];
    REQUIRE(orig.id == "temperature");

    // Press inside the bottom edge's grab band and drag the edge down one cell,
    // one read per frame - the shape a real finger produces and a single-frame
    // synthetic drag never exercises.
    const lv_area_t wa = area_of(widget);
    const int x = (wa.x1 + wa.x2) / 2;
    const int y = wa.y2 - GridEditModeTestAccess::edge_hit_band(grid()) / 2;
    REQUIRE(grid().detect_resize_edge(x, y, wa) == helix::GridEditMode::ResizeEdge::Bottom);
    indev.grab(x, y);
    REQUIRE(grid().selected_widget() == widget);

    // The first frame crosses the drag threshold and starts the resize; the rest
    // carry the edge the remainder of the cell.
    const helix::CellMetrics m = GridEditModeTestAccess::cell_metrics(grid());
    const int cell_px = static_cast<int>(std::lround(kCellTracks * (m.cell_h + m.gutter)));
    const int start_y = y + past_drag_threshold();
    REQUIRE(cell_px > start_y - y);
    indev.move(x, start_y);
    REQUIRE(GridEditModeTestAccess::resizing(grid()));
    lv_obj_t* preview = GridEditModeTestAccess::resize_preview(grid());
    REQUIRE(preview != nullptr);
    constexpr int kFrames = 5;
    for (int frame = 1; frame <= kFrames; ++frame) {
        indev.move(x, start_y + (y + cell_px - start_y) * frame / kFrames);
    }
    REQUIRE(GridEditModeTestAccess::resizing(grid()));

    indev.release(x, y + cell_px);
    settle();

    const auto committed = config().page_entries(0)[0];
    CHECK(committed.rowspan == orig.rowspan + kCellTracks); // one cell taller
    CHECK(GridEditModeTestAccess::resize_preview(grid()) == nullptr);
    CHECK_FALSE(helix::test::widget_in_tree(preview)); // nothing it drew is left
    CHECK(GridEditModeTestAccess::snap_preview(grid()) == nullptr);

    // The resized tile keeps its tree: it moves to its new span, its widget
    // hears the span through on_size_changed(), and it is selected again.
    lv_obj_t* resized = widget_on(0, "temperature");
    CHECK(resized == widget);
    CHECK(lv_obj_get_style_grid_cell_row_span(resized, LV_PART_MAIN) == committed.rowspan);
    auto* stand_in = static_cast<StandInWidget*>(lv_obj_get_user_data(resized));
    REQUIRE(stand_in != nullptr);
    CHECK(stand_in->last_rowspan == committed.rowspan);
    CHECK_FALSE(is_clickable(resized));
    CHECK(grid().selected_widget() == resized);
}

TEST_CASE_METHOD(EditHomeFixture, "a multi-frame drag commits its landing cell",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const auto orig = config().page_entries(0)[0];
    REQUIRE(orig.id == "temperature");

    // Two cells right of its origin: the widget follows the pointer from where
    // the drag starts, so a travel of that many track pitches lands it there.
    const int expected_col = orig.col + 2 * kCellTracks;
    const int travel = static_cast<int>(
        std::lround(track_pitch_px() * static_cast<float>(expected_col - orig.col)));

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    const int start_x = c.x + past_drag_threshold();
    indev.move(start_x, c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    constexpr int kFrames = 6;
    for (int frame = 1; frame <= kFrames; ++frame) {
        indev.move(start_x + travel * frame / kFrames, c.y);
    }
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) == expected_col);
    indev.release(start_x + travel, c.y);
    settle();

    const auto committed = config().page_entries(0)[0];
    CHECK(committed.id == "temperature");
    CHECK(committed.col == expected_col);
    CHECK(committed.row == orig.row);

    // A move within its page re-seats the page in place: the widget keeps its
    // object, laid out at the landing cell, and is selected again.
    lv_obj_t* moved = widget_on(0, "temperature");
    CHECK(moved == widget);
    CHECK(lv_obj_get_style_grid_cell_column_pos(moved, LV_PART_MAIN) == expected_col);
    CHECK(grid().selected_widget() == moved);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "dragging a widget to the screen edge flips the page and the drop moves it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // temperature on page 0
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // Two-step grab, then cross the threshold to arm the move-drag.
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Hold the widget in the right edge zone: the dwell flips one page while the
    // drag survives, and the slide settles.
    const lv_area_t frame = settled_page_area();
    indev.move(frame.x2 - edge_zone_px() / 2, c.y);
    process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
    settle();
    CHECK(current_page() == 1);
    CHECK(grid().page_index() == 1);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    check_session_on_screen();
    // The session is scoped to page 1 while the entry still lives on page 0:
    // the dragged widget's id resolves regardless, so its snap step holds.
    CHECK(GridEditModeTestAccess::selected_widget_id(grid()) == "temperature");

    // Carry the widget into page 1, where the move draws the landing page's
    // preview, and drop it: the entry moves there.
    const int drop_x = (frame.x1 + frame.x2) / 2;
    indev.move(drop_x, c.y);
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) >= 0);
    indev.release(drop_x, c.y);
    settle();

    CHECK(count_on_page(0, "temperature") == 0);
    CHECK(count_on_page(1, "temperature") == 1);
    CHECK(config().page_count() == 2); // the emptied page is the main page, which stays
    check_session_on_screen();

    // The moved widget is selected again after the commit.
    lv_obj_t* reselected = grid().selected_widget();
    REQUIRE(reselected != nullptr);
    CHECK(std::string(lv_obj_get_name(reselected)) == "temperature");
}

TEST_CASE_METHOD(EditHomeFixture, "a drag that flips forth and back drops on the origin page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    const lv_area_t frame = settled_page_area();
    const int zone = edge_zone_px();
    // Flip right to page 1...
    indev.move(frame.x2 - zone / 2, c.y);
    process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
    settle();
    REQUIRE(current_page() == 1);
    // ...and back left to page 0.
    indev.move(frame.x1 + zone / 2, c.y);
    process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
    settle();
    REQUIRE(current_page() == 0);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    check_session_on_screen();

    indev.release(c.x, c.y);
    settle();

    CHECK(count_on_page(0, "temperature") == 1); // exactly once, on the origin page
    CHECK(count_on_page(1, "temperature") == 0);
}

TEST_CASE_METHOD(EditHomeFixture, "a move that empties a page removes it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'temperature' on page 0, 'fan' on page 1
    widget_on(0, "temperature");
    lv_obj_t* fan = widget_on(1, "fan");
    REQUIRE(config().page_count() == 2);
    REQUIRE(HomePanelTestAccess::recount_populated_pages(panel()) == 2);

    // Edit page 1, where 'fan' lives.
    enter_edit_mode(1);

    const lv_point_t c = center_of(fan);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == fan);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Hold in the left edge zone past the dwell: the session flips to page 0
    // with the drag alive.
    const lv_area_t frame = settled_page_area();
    const int zone_x = frame.x1 + edge_zone_px() / 2;
    indev.move(zone_x, c.y);
    process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
    settle();
    REQUIRE(current_page() == 0);
    REQUIRE(grid().page_index() == 0);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Carry the widget clear of 'temperature' and drop it.
    const auto temperature = config().page_entries(0)[0];
    REQUIRE(temperature.id == "temperature");
    const int drop_x = (frame.x1 + frame.x2) / 2;
    constexpr int kFrames = 4;
    for (int f = 1; f <= kFrames; ++f) {
        indev.move(zone_x + (drop_x - zone_x) * f / kFrames, c.y);
    }
    const int snap_col = GridEditModeTestAccess::snap_col(grid());
    const int snap_row = GridEditModeTestAccess::snap_row(grid());
    INFO("drop cell " << snap_col << "," << snap_row << " at x=" << drop_x);
    REQUIRE(snap_col >= temperature.col + temperature.colspan);
    REQUIRE(snap_row >= 0);
    const lv_obj_t* carousel_before = carousel();
    indev.release(drop_x, c.y);
    CHECK(carousel() == carousel_before); // the rebuild waits for the next tick
    settle();

    CHECK(config().page_count() == 1); // page 1 emptied and was removed
    CHECK(count_on_page(0, "fan") == 1);
    CHECK(grid().page_index() == 0);
    CHECK(HomePanelTestAccess::recount_populated_pages(panel()) == 1);

    // The carousel was rebuilt around the remaining page, and the session and
    // the carousel are both on it.
    CHECK(carousel() != carousel_before);
    CHECK(page_count() == 1);
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop on a same-size widget swaps the two and keeps the dragged one selected",
                 "[1638][1503][edit-swipe][home][grid_edit]") {
    build_home();
    // 'fan' one cell right of 'temperature' on page 0, where the drag aims.
    REQUIRE(config().place_entry("fan", 0, kCellTracks, 0, kCellTracks, kCellTracks) >= 0);
    repopulate();
    lv_obj_t* temperature = widget_on(0, "temperature");
    widget_on(0, "fan");
    enter_edit_mode();
    const helix::PanelWidgetEntry temperature_before = entry_on_page(0, "temperature");
    const helix::PanelWidgetEntry fan_before = entry_on_page(0, "fan");

    const lv_point_t c = center_of(temperature);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == temperature);
    const lv_point_t drop{c.x + past_drag_threshold() + tracks_px(kCellTracks), c.y};

    SECTION("carried in one read") {
        indev.move(c.x + past_drag_threshold(), c.y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        indev.move(drop.x, drop.y);
    }
    SECTION("carried no further than the scroll limit per read") {
        // A slow drag: no read moves the pointer far enough for LVGL to flag
        // the press as moved.
        const int step = indev.indev()->scroll_limit;
        REQUIRE(step > 0);
        for (int x = c.x + step; x < drop.x; x += step) {
            indev.move(x, c.y);
        }
        indev.move(drop.x, drop.y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        REQUIRE_FALSE(lv_indev_get_press_moved(indev.indev()));
    }

    // The drop aims at the cell 'fan' holds, and the lift lands on 'fan'.
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) == fan_before.col);
    REQUIRE(GridEditModeTestAccess::snap_row(grid()) == fan_before.row);
    indev.release(drop.x, drop.y);
    settle();

    CHECK(entry_on_page(0, "temperature").col == fan_before.col);
    CHECK(entry_on_page(0, "temperature").row == fan_before.row);
    CHECK(entry_on_page(0, "fan").col == temperature_before.col);
    CHECK(entry_on_page(0, "fan").row == temperature_before.row);
    lv_obj_t* selected = grid().selected_widget();
    REQUIRE(selected != nullptr);
    CHECK(std::string(lv_obj_get_name(selected)) == "temperature");
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop whose swap does not fit leaves both widgets and keeps the dragged one "
                 "selected",
                 "[1638][1503][edit-swipe][home][grid_edit]") {
    build_home();
    // A two-cell 'fan' one cell right of 'temperature': swapped, it would span
    // the cell 'temperature' lands on.
    REQUIRE(config().place_entry("fan", 0, kCellTracks, 0, 2 * kCellTracks, kCellTracks) >= 0);
    repopulate();
    lv_obj_t* temperature = widget_on(0, "temperature");
    widget_on(0, "fan");
    enter_edit_mode();
    const helix::PanelWidgetEntry temperature_before = entry_on_page(0, "temperature");
    const helix::PanelWidgetEntry fan_before = entry_on_page(0, "fan");
    REQUIRE(fan_before.colspan == 2 * kCellTracks);

    const lv_point_t c = center_of(temperature);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == temperature);
    const lv_point_t drop{c.x + past_drag_threshold() + tracks_px(kCellTracks), c.y};
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    indev.move(drop.x, drop.y);
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) == fan_before.col);
    REQUIRE(GridEditModeTestAccess::snap_row(grid()) == fan_before.row);
    indev.release(drop.x, drop.y);
    settle();

    CHECK(entry_on_page(0, "temperature").col == temperature_before.col);
    CHECK(entry_on_page(0, "temperature").row == temperature_before.row);
    CHECK(entry_on_page(0, "fan").col == fan_before.col);
    CHECK(entry_on_page(0, "fan").row == fan_before.row);
    lv_obj_t* selected = grid().selected_widget();
    REQUIRE(selected != nullptr);
    CHECK(std::string(lv_obj_get_name(selected)) == "temperature");
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a tap on a widget inside the selected widget's grab band selects it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    REQUIRE(config().place_entry("fan", 0, kCellTracks, 0, kCellTracks, kCellTracks) >= 0);
    repopulate();
    lv_obj_t* temperature = widget_on(0, "temperature");
    lv_obj_t* fan = widget_on(0, "fan");
    enter_edit_mode();

    const lv_point_t c = center_of(temperature);
    indev.prime(c.x, c.y);
    indev.release(c.x, c.y);
    REQUIRE(grid().selected_widget() == temperature);

    // A point on 'fan' that the selected widget's grab band still covers: the
    // press arms 'temperature', so only the click can select 'fan'.
    const lv_area_t fan_area = area_of(fan);
    const lv_point_t tap{fan_area.x1 + 1, (fan_area.y1 + fan_area.y2) / 2};
    REQUIRE(GridEditModeTestAccess::press_owns_widget(grid(), tap, area_of(temperature)));
    indev.press(tap.x, tap.y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    REQUIRE(grid().selected_widget() == temperature);

    indev.release(tap.x, tap.y);
    CHECK(grid().selected_widget() == fan);
}

TEST_CASE_METHOD(EditHomeFixture, "a press that cannot arm selects only where it lands",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    // 'fan' one cell below 'temperature' on page 0: a finger sliding between
    // them travels across the swipe axis, where the carousel cannot take the
    // gesture from the grid handlers.
    REQUIRE(config().place_entry("fan", 0, 0, kCellTracks, kCellTracks, kCellTracks) >= 0);
    repopulate();
    lv_obj_t* temperature = widget_on(0, "temperature");
    lv_obj_t* fan = widget_on(0, "fan");
    const lv_point_t t = center_of(temperature);
    const lv_point_t f = center_of(fan);

    SECTION("a press on an unselected widget") {
        enter_edit_mode();
        indev.press(t.x, t.y);
        REQUIRE(grid().selected_widget() == temperature);
    }
    SECTION("the hold that enters edit mode") {
        indev.press(t.x, t.y);
        indev.hold(indev.long_press_hold_ms());
        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        REQUIRE(grid().selected_widget() == temperature);
    }
    REQUIRE_FALSE(GridEditModeTestAccess::press_armed(grid()));

    // The finger slides onto 'fan' and rests there for another read.
    REQUIRE_FALSE(GridEditModeTestAccess::press_owns_widget(grid(), f, area_of(temperature)));
    indev.move(f.x, f.y);
    indev.move(f.x, f.y);
    REQUIRE(lv_indev_get_scroll_obj(indev.indev()) == nullptr);
    CHECK(grid().selected_widget() == temperature);
    CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
    indev.release(f.x, f.y);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a continuous press that selects mid-gesture stays a swipe, not a grab",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // One continuous press: its first read's PRESSING selects the widget, the
    // rest drag the finger across it. Selecting mid-gesture must not arm a grab
    // - a packed grid depends on this gesture reading as a swipe.
    const lv_point_t c = center_of(widget);
    indev.press(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    constexpr int kMoves = 5;
    int x = c.x;
    for (int i = 0; i < kMoves; ++i) {
        x += past_drag_threshold();
        indev.move(x, c.y);
    }
    CHECK_FALSE(GridEditModeTestAccess::dragging(grid()));
    CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
    CHECK(scroll_live()); // the swipe surface never locked
    // And the carousel's scroll took the gesture.
    CHECK(lv_indev_get_scroll_obj(indev.indev()) == scroller());
    indev.release(x, c.y);
    settle();
}

TEST_CASE_METHOD(EditHomeFixture, "a hold on an unselected widget grabs it after selection",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // One gesture: press the unselected widget, hold past a long press (the
    // hold grabs what the press selected), keep holding and move. The drag
    // must start without a release in between.
    const lv_point_t c = center_of(widget);
    indev.press(c.x, c.y); // selects: no arm, still a swipe candidate
    REQUIRE(grid().selected_widget() == widget);
    CHECK(scroll_live());

    indev.hold(indev.long_press_hold_ms()); // its last read dispatches LONG_PRESSED: grab
    CHECK(GridEditModeTestAccess::dragging(grid()));
    CHECK_FALSE(scroll_live());

    int x = c.x;
    for (int i = 0; i < 3; ++i) {
        x += past_drag_threshold();
        indev.move(x, c.y);
    }
    CHECK(GridEditModeTestAccess::dragging(grid()));
    indev.release(x, c.y);
    CHECK(scroll_live());
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a widget crossed by its leading side flips with the pointer clear of the edge "
                 "zone",
                 "[1638][edit-swipe][home][grid_edit]") {
    // Two cells wide, so its majority can cross the border while the grab point
    // is still outside the edge zone.
    build_home(2, 2, 2 * kCellTracks);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_area_t wa = area_of(widget);
    const int width = lv_area_get_width(&wa);
    const int cy = (wa.y1 + wa.y2) / 2;

    // Grab the widget's left part, clear of its resize band, and cross the
    // threshold leftward: the drag offset is measured at that point.
    const int grab_x = wa.x1 + width * 3 / 10;
    REQUIRE(grid().detect_resize_edge(grab_x, cy, wa) == helix::GridEditMode::ResizeEdge::None);
    indev.grab(grab_x, cy);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    const int threshold_x = grab_x - past_drag_threshold();
    indev.move(threshold_x, cy);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    const int grab_offset = threshold_x - wa.x1;

    // A pointer spot where the widget's majority is past the settled page's
    // right edge while the pointer is still outside the edge zone, so no dwell
    // runs and only the crossing can flip.
    const lv_area_t frame = settled_page_area();
    const int crossed_from = frame.x2 - width / 2 + grab_offset + 1;
    const int zone_from = frame.x2 - edge_zone_px();
    INFO("crossing from x=" << crossed_from << ", edge zone from x=" << zone_from);
    REQUIRE(crossed_from < zone_from);
    const int x = (crossed_from + zone_from - 1) / 2;

    indev.move(x, cy);
    indev.hold(helix::kCrossPageCrossingDelayMs + 3 * kReadPeriodMs);
    CHECK(current_page() == 1);
    CHECK(grid().page_index() == 1);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    indev.release(x, cy);
}

TEST_CASE_METHOD(EditHomeFixture, "a crossing held in the edge zone flips exactly one page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(2, 3); // a third page, so a chained flip has a page to land on
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Pinned inside the right edge zone: the push carries the widget's majority
    // across, the page slides in animated under a pointer that never moves, and
    // the hold runs long past both the crossing and the dwell.
    const uint32_t hold_ms = 5 * helix::kCrossPageDwellMs / 2;
    const int x = settled_page_area().x2 - edge_zone_px() / 2;

    int carousel_page = current_page();
    int session_page = grid().page_index();
    int carousel_flips = 0;
    int session_flips = 0;
    indev.move(x, c.y);
    indev.hold(hold_ms, [&]() {
        const int now_carousel = current_page();
        if (now_carousel != carousel_page) {
            ++carousel_flips;
            carousel_page = now_carousel;
        }
        if (grid().page_index() != session_page) {
            ++session_flips;
            session_page = grid().page_index();
        }
    });
    CHECK(carousel_flips == 1);
    CHECK(session_flips == 1);
    CHECK(current_page() == 1);
    CHECK(grid().page_index() == 1);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    indev.release(x, c.y);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a crossing released before its flip leaves the next drag able to cross",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const lv_area_t frame = settled_page_area();

    // Grab left of center and cross the drag threshold leftward. The drag offset
    // is taken where the threshold is crossed, so the first move to the page's
    // right edge already has the widget's majority past the border: no move
    // with the widget inside comes first to re-arm the crossing.
    auto grab_and_cross_right = [&]() {
        const lv_area_t wa = area_of(widget);
        const int grab_x = wa.x1 + lv_area_get_width(&wa) * 3 / 10;
        const int y = (wa.y1 + wa.y2) / 2;
        indev.grab(grab_x, y);
        REQUIRE(GridEditModeTestAccess::press_armed(grid()));
        indev.move(grab_x - past_drag_threshold(), y);
        REQUIRE(GridEditModeTestAccess::dragging(grid()));
        indev.move(frame.x2, y); // majority past the border
        return y;
    };

    // First drag: the crossing requests its flip, and the release lands before
    // the crossing delay runs out. The ended gesture's flip never happens.
    int cy = grab_and_cross_right();
    indev.release(frame.x2, cy);
    settle();
    REQUIRE(current_page() == 0);
    REQUIRE(grid().page_index() == 0);

    // The commit's rebuild laid the widget out at the cell the release chose.
    widget = widget_on(0, "temperature");

    // Second drag goes straight past the same border. Its crossing flips once
    // the crossing delay passes, before the edge dwell could.
    static_assert(helix::kCrossPageCrossingDelayMs + kReadPeriodMs < helix::kCrossPageDwellMs);
    cy = grab_and_cross_right();
    process_lvgl(static_cast<int>(helix::kCrossPageCrossingDelayMs + kReadPeriodMs));
    CHECK(current_page() == 1);
    CHECK(grid().page_index() == 1);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    indev.release((frame.x1 + frame.x2) / 2, cy);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a gesture ended by an indev reset leaves no stale arming verdict",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // Select, grab, then reset the indev with the finger still down. Whatever
    // arming verdict that gesture latched must end with it.
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    lv_indev_reset(indev.indev(), nullptr);
    CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
    CHECK(grid().selected_widget() == nullptr);

    // The finger stays down and moves on: a press-move that selects mid-gesture
    // must read as a swipe, not inherit the dead gesture's can-arm.
    indev.prime(c.x, c.y); // PRESSING only: selects
    int x = c.x;
    for (int i = 0; i < 4; ++i) {
        x += past_drag_threshold();
        indev.move(x, c.y);
    }
    CHECK_FALSE(GridEditModeTestAccess::dragging(grid()));
    CHECK(scroll_live());
    indev.release(x, c.y);
    settle();

    // And the two-step path still arms after the abort, on the widget the
    // abort's rebuild laid out again.
    widget = widget_on(0, "temperature");
    indev.prime(c.x, c.y);
    indev.release(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    indev.prime(c.x, c.y);
    CHECK(GridEditModeTestAccess::press_armed(grid()));
    indev.release(c.x, c.y);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a trailing-side grab crosses the border by edge push before the dwell",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_area_t wa = area_of(widget);
    const int width = lv_area_get_width(&wa);
    const int cy = (wa.y1 + wa.y2) / 2;

    // Grab right of center but clear of the resize edge band.
    const int grab_x = wa.x1 + (wa.x2 - wa.x1) * 7 / 10;
    REQUIRE(grid().detect_resize_edge(grab_x, cy, wa) == helix::GridEditMode::ResizeEdge::None);
    indev.grab(grab_x, cy);
    const int threshold_x = grab_x + past_drag_threshold();
    indev.move(threshold_x, cy);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    const int grab_offset = threshold_x - wa.x1;

    // Pinned at the page's edge, inside the edge zone, the pointer alone leaves
    // the widget's majority inside; the push has to carry it the rest of the
    // way. The hold's last cycle ends before the dwell's period elapses, so a
    // flip in time can only come from the push's crossing - and the push must
    // be able to make it.
    const lv_area_t frame = settled_page_area();
    const int x = frame.x2;
    const int push_needed_px = (frame.x2 - width / 2) - (x - grab_offset) + 1;
    REQUIRE(push_needed_px > 0);
    const int speed = push_px_per_s();
    const uint32_t crossed_flip_by_ms = static_cast<uint32_t>(push_needed_px * 1000 / speed) +
                                        helix::kCrossPageCrossingDelayMs + 3 * kReadPeriodMs;
    const uint32_t hold_ms = helix::kCrossPageDwellMs - kReadPeriodMs;
    INFO("push " << push_needed_px << "px at " << speed << "px/s, flip by " << crossed_flip_by_ms
                 << "ms, hold " << hold_ms << "ms");
    REQUIRE(crossed_flip_by_ms < hold_ms);

    indev.move(x, cy);
    indev.hold(hold_ms);
    CHECK(current_page() == 1);
    CHECK(grid().page_index() == 1);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    indev.release(x, cy);
}

TEST_CASE_METHOD(EditHomeFixture, "a left crossing flips before the edge dwell could",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'fan' on page 1
    lv_obj_t* fan = widget_on(1, "fan");

    // Edit page 1, so there is a page to the left.
    enter_edit_mode(1);

    const lv_area_t wa = area_of(fan);
    const int width = lv_area_get_width(&wa);
    const lv_point_t c = center_of(fan);
    indev.grab(c.x, c.y);
    REQUIRE(grid().selected_widget() == fan);
    const int threshold_x = c.x - past_drag_threshold();
    indev.move(threshold_x, c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    const int grab_offset = threshold_x - wa.x1;

    // Pinned in the left edge zone with the widget's majority still inside the
    // page. Held for less than the dwell, so only the push's crossing can flip.
    const lv_area_t frame = settled_page_area();
    const int x = frame.x1 + edge_zone_px() / 2;
    const int push_needed_px = (x - grab_offset) - (frame.x1 - width / 2) + 1;
    REQUIRE(push_needed_px > 0);
    const int speed = push_px_per_s();
    const uint32_t crossed_flip_by_ms = static_cast<uint32_t>(push_needed_px * 1000 / speed) +
                                        helix::kCrossPageCrossingDelayMs + 3 * kReadPeriodMs;
    const uint32_t hold_ms = helix::kCrossPageDwellMs - kReadPeriodMs;
    INFO("push " << push_needed_px << "px at " << speed << "px/s, flip by " << crossed_flip_by_ms
                 << "ms, hold " << hold_ms << "ms");
    REQUIRE(crossed_flip_by_ms < hold_ms);

    indev.move(x, c.y);
    indev.hold(hold_ms);
    CHECK(current_page() == 0);
    CHECK(grid().page_index() == 0);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    indev.release(x, c.y);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drag held past the last page's border rides onto the next-page slot",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1); // a single page with temperature on it, and the slot past it
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    REQUIRE(slot_in_reach()); // the + tile is shown before any drag too

    const SlotDrag drag = drag_onto_slot(widget);

    // One flip, onto the slot, and nothing further through the slide and a
    // dwell's worth of holding after it.
    CHECK(drag.carousel_flips == 1);
    CHECK(drag.session_flips == 1);
    CHECK(current_page() == 1);
    CHECK(config().page_count() == 1); // nothing is created mid-drag
    CHECK(lv_obj_get_parent(widget) == drag_layer());
    CHECK(slot_in_reach());
    check_session_on_screen();

    indev.release(drag.pointer.x, drag.pointer.y);
    settle();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop on the next-page slot creates the page and lands the widget on it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const SlotDrag drag = drag_onto_slot(widget);
    REQUIRE(config().page_count() == 1); // creation waits for the drop
    REQUIRE(GridEditModeTestAccess::drop_wants_new_page(grid()));

    // The drop creates the page and lands the entry on it, and the panel
    // rebuilds the carousel around the new page set, on the new page, which
    // sits where the slot the drop landed on did.
    const PageSetChange change = watch_page_set_change();
    indev.release(drag.pointer.x, drag.pointer.y);
    // The commit creates the page inside the release; the carousel rebuild
    // waits for the next tick, outside the input dispatch. Until then the
    // carousel rests on the slot's tile, still within reach, so the dropped
    // widget stays on screen.
    CHECK(config().page_count() == 2);
    CHECK(current_page() == 1);
    CHECK(slot_in_reach());
    run_page_set_rebuild(change, 1);
    settle();

    CHECK(grid().page_index() == 1);
    CHECK(count_on_page(0, "temperature") == 0);
    CHECK(count_on_page(1, "temperature") == 1); // the drop landed on the page it created

    // The rebuilt carousel has a container per config page and a new slot past
    // them, its + tile in reach with no drag; the session and the carousel are
    // on the new page.
    CHECK(page_count() == 2);
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    REQUIRE(slot_container() != nullptr);
    CHECK(slot_in_reach());
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop anywhere on the next-page slot creates the page at the dropped cell",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const auto orig = config().page_entries(0)[0];
    REQUIRE(orig.id == "temperature");

    const SlotDrag drag = drag_onto_slot(widget);

    // Carried into the middle of the slot, clear of both edge zones, two cells
    // right of where the drag started. The slot previews that cell, which is not
    // the rightmost columns a drop past the last page's border clamps to.
    const int expected_col = orig.col + 2 * kCellTracks;
    const int x = drag.start.x + tracks_px(2 * kCellTracks);
    const lv_area_t frame = settled_page_area();
    REQUIRE(x > frame.x1 + edge_zone_px());
    REQUIRE(x < frame.x2 - edge_zone_px());
    const helix::CellMetrics m = GridEditModeTestAccess::cell_metrics(grid());
    REQUIRE(expected_col != m.cols - orig.colspan);
    indev.move(x, drag.start.y);
    REQUIRE(grid().page_index() == 1);
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) == expected_col);
    REQUIRE(GridEditModeTestAccess::snap_preview(grid()) != nullptr);

    indev.release(x, drag.start.y);
    settle();

    CHECK(config().page_count() == 2);
    CHECK(count_on_page(0, "temperature") == 0);
    REQUIRE(count_on_page(1, "temperature") == 1);
    const auto landed = config().page_entries(1)[0];
    INFO("landed at col " << landed.col << ", row " << landed.row);
    CHECK(landed.col == expected_col);
    CHECK(landed.row == orig.row);
    CHECK(grid().page_index() == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drag pulled back across onto its page drops there and creates nothing",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const auto orig = config().page_entries(0)[0];
    REQUIRE(orig.id == "temperature");

    const SlotDrag drag = drag_onto_slot(widget);

    // Held in the slot's left edge zone until the drag flips back onto its page
    // and the slide settles.
    const lv_area_t frame = settled_page_area();
    indev.move(frame.x1 + edge_zone_px() / 2, drag.start.y);
    indev.hold(2 * helix::kCrossPageDwellMs);
    REQUIRE(grid().page_index() == 0);
    REQUIRE(current_page() == 0);
    REQUIRE(lv_anim_count_running() == 0);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Onto a free cell of its page, two cells right of where the drag started.
    const int expected_col = orig.col + 2 * kCellTracks;
    const int x = drag.start.x + tracks_px(2 * kCellTracks);
    indev.move(x, drag.start.y);
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) == expected_col);

    indev.release(x, drag.start.y);
    settle();

    CHECK(config().page_count() == 1);
    REQUIRE(count_on_page(0, "temperature") == 1);
    const auto entry = config().page_entries(0)[0];
    CHECK(entry.col == expected_col);
    CHECK(entry.row == orig.row);
    CHECK(grid().page_index() == 0);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "leaving edit mode mid-drag over the next-page slot leaves the widget on its page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const SlotDrag drag = drag_onto_slot(widget);
    REQUIRE(lv_obj_get_parent(widget) == drag_layer());

    SECTION("Done") {
        panel().exit_grid_edit_mode();
    }
    SECTION("deactivation") {
        panel().on_deactivating(DeactivateReason::NavigateAway);
    }

    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    // The drag went home before the session ended.
    CHECK(lv_obj_get_parent(widget) == page(0));
    indev.release(drag.pointer.x, drag.pointer.y);
    settle();

    // The exit's rebuild laid the widget out on its page again.
    widget = widget_on(0, "temperature");
    CHECK_FALSE(lv_obj_has_flag(widget, LV_OBJ_FLAG_FLOATING));
    CHECK(lv_obj_get_child_by_name(slot_container(), "temperature") == nullptr);
    CHECK(current_page() == 0);
    CHECK(config().page_count() == 1);
    CHECK(count_on_page(0, "temperature") == 1);
    CHECK(slot_in_reach());
}

TEST_CASE_METHOD(EditHomeFixture, "a release right after a carried flip drops on the landing page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // The dwell flips with no further read, and the release follows in the same
    // read period, with the landing page still sliding in.
    const int x = settled_page_area().x2 - edge_zone_px() / 2;
    indev.move(x, c.y);
    process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
    REQUIRE(grid().page_index() == 1);
    REQUIRE(lv_anim_count_running() > 0);
    indev.release(x, c.y);
    settle();

    CHECK(count_on_page(0, "temperature") == 0);
    CHECK(count_on_page(1, "temperature") == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a release right after a flip back off the next-page slot creates nothing",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1); // page 0 is the last page
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");

    const SlotDrag drag = drag_onto_slot(widget);
    REQUIRE(GridEditModeTestAccess::drop_wants_new_page(grid()));

    // One move into the slot's left edge zone, clear of a crossing; the dwell
    // then flips back onto page 0 with no further read, and the release follows
    // in the same read period, the widget far from page 0's right border.
    const lv_area_t frame = settled_page_area();
    const int x = frame.x1 + edge_zone_px() / 2;
    indev.move(x, drag.start.y);
    process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
    REQUIRE(grid().page_index() == 0);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    indev.release(x, drag.start.y);
    settle();

    CHECK(config().page_count() == 1);
    REQUIRE(count_on_page(0, "temperature") == 1);
    const helix::PanelWidgetEntry now = entry_on_page(0, "temperature");
    CHECK(now.col == origin.col);
    CHECK(now.row == origin.row);
    CHECK(grid().page_index() == 0);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "at the page cap there is no slot and a drop past the last border stays on its "
                 "page",
                 "[1638][edit-swipe][home][grid_edit]") {
    const int cap = static_cast<int>(helix::kMaxPages);
    const int last = cap - 1;
    build_home(1, cap);
    // The widget moves to the last page, where the drag starts.
    REQUIRE(config().place_entry("temperature", static_cast<size_t>(last), 0, 0, kCellTracks,
                                 kCellTracks) >= 0);
    repopulate();
    lv_obj_t* widget = widget_on(last, "temperature");
    REQUIRE_FALSE(config().can_add_page());

    CHECK(slot_container() == nullptr);
    CHECK(static_cast<int>(carousel_state()->real_tiles.size()) == cap);

    enter_edit_mode(last);
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Held at the right edge long enough to cross the border and outlast a dwell.
    const int x = settled_page_area().x2;
    int flips = 0;
    indev.move(x, c.y);
    indev.hold(2 * helix::kCrossPageDwellMs, [&]() {
        if (current_page() != last || grid().page_index() != last) {
            ++flips;
        }
    });
    CHECK(flips == 0);
    CHECK_FALSE(GridEditModeTestAccess::drop_wants_new_page(grid()));

    indev.release(x, c.y);
    settle();

    CHECK(config().page_count() == static_cast<size_t>(cap));
    CHECK(count_on_page(static_cast<size_t>(last), "temperature") == 1);
    CHECK(grid().page_index() == last);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop on the next-page slot that reaches the page cap comes up on the new page",
                 "[1638][edit-swipe][home][grid_edit]") {
    const int cap = static_cast<int>(helix::kMaxPages);
    const int last = cap - 2; // the last page, one below the cap
    build_home(2, cap - 1);
    // Both widgets move to the last page, so the drop leaves its origin page
    // populated and prunes nothing.
    REQUIRE(config().place_entry("temperature", static_cast<size_t>(last), 0, 0, kCellTracks,
                                 kCellTracks) >= 0);
    REQUIRE(config().place_entry("fan", static_cast<size_t>(last), 0, kCellTracks, kCellTracks,
                                 kCellTracks) >= 0);
    repopulate();
    REQUIRE(config().can_add_page());
    enter_edit_mode(last);

    const SlotDrag drag = drag_onto_slot(widget_on(last, "temperature"));
    const PageSetChange change = watch_page_set_change();
    indev.release(drag.pointer.x, drag.pointer.y);
    REQUIRE(config().page_count() == static_cast<size_t>(cap));

    // At the cap the rebuilt carousel has no slot past the new page.
    run_page_set_rebuild(change, cap - 1);
    CHECK(slot_container() == nullptr);
    settle();
    CHECK(count_on_page(static_cast<size_t>(cap - 1), "temperature") == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a tap on the + the next-page slot carries adds a page and lands on it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // two pages, below the page cap
    show_page(1);

    // The + sits centered on the slot's tile, one tile past the last page.
    helix::ui::carousel_goto_tile(carousel(), page_count(), /*animate=*/false);
    settle();
    lv_obj_t* plus = lv_obj_find_by_name(slot_tile(), "add_page_button");
    REQUIRE(plus != nullptr);
    const lv_point_t c = center_of(plus);

    const PageSetChange change = watch_page_set_change();
    indev.press(c.x, c.y);
    indev.release(c.x, c.y);
    REQUIRE(config().page_count() == 3);

    // The tap went through the same landing every page-set change takes: the
    // carousel comes up on the added page, and nothing else scrolls.
    run_page_set_rebuild(change, 2);
    CHECK(HomePanelTestAccess::active_page(panel()) == 2);
    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    // The added page is empty, and a slot sits past it.
    CHECK(count_on_page(2, "temperature") == 0);
    CHECK(count_on_page(2, "fan") == 0);
    REQUIRE(slot_container() != nullptr);
    CHECK(carousel_state()->real_page_count == 3);
    // The pages the tap did not touch kept their widgets.
    CHECK(count_on_page(0, "temperature") == 1);
    CHECK(count_on_page(1, "fan") == 1);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a tap on the + in edit mode adds a page and re-scopes the session onto it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    enter_edit_mode(1);

    // Resting on the slot's tile, the session stays scoped to the page it
    // edited while the + sits tappable on the slot.
    helix::ui::carousel_goto_tile(carousel(), page_count(), /*animate=*/false);
    settle();
    CHECK(grid().page_index() == 1);
    lv_obj_t* plus = lv_obj_find_by_name(slot_tile(), "add_page_button");
    REQUIRE(plus != nullptr);
    const lv_point_t c = center_of(plus);

    const PageSetChange change = watch_page_set_change();
    indev.press(c.x, c.y);
    indev.release(c.x, c.y);
    REQUIRE(config().page_count() == 3);

    run_page_set_rebuild(change, 2);
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    // The session followed the landing onto the added page.
    CHECK(grid().page_index() == 2);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture, "at the page cap there is no + to tap and no slot to drop on",
                 "[1638][edit-swipe][home][grid_edit]") {
    const int cap = static_cast<int>(helix::kMaxPages);
    build_home(2, cap);
    REQUIRE_FALSE(config().can_add_page());

    // No slot component exists at the cap, so no + rides on any tile.
    REQUIRE(slot_container() == nullptr);
    CHECK(lv_obj_find_by_name(HomePanelTestAccess::panel_root(panel()), "add_page_button") ==
          nullptr);
    CHECK(carousel_state()->real_page_count == cap);

    // Neither edge creates a page: a drag past the first page's left border
    // leaves the widget on its page.
    enter_edit_mode();
    const SlotDrag drag = drag_past_left_border(widget_on(0, "temperature"));
    const PageSetChange change = watch_page_set_change();
    indev.release(drag.pointer.x, drag.pointer.y);
    settle();
    CHECK(config().page_count() == static_cast<size_t>(cap));
    CHECK(count_on_page(0, "temperature") == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drag past the first page's left border creates a page before it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // temperature on page 0, fan on page 1
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const SlotDrag drag = drag_past_left_border(widget);
    // Nothing sits before the first page, so the hold flips nothing.
    CHECK(drag.carousel_flips == 0);
    CHECK(drag.session_flips == 0);
    REQUIRE(config().page_count() == 2); // nothing is created mid-drag

    const PageSetChange change = watch_page_set_change();
    indev.release(drag.pointer.x, drag.pointer.y);
    REQUIRE(config().page_count() == 3);

    // The carousel was on the old first page, which the rebuild comes up on
    // renumbered; one slide lands on the page the drop created before it.
    run_page_set_rebuild(change, 1, 0);
    // The widget landed on the created page; the pages shifted one later,
    // their widgets intact.
    CHECK(count_on_page(0, "temperature") == 1);
    CHECK(count_on_page(2, "fan") == 1);
    CHECK(config().page_id(0) == "page_3"); // generate_page_id() minted it
    CHECK(config().page_id(1) == "main");   // the old first page
    CHECK(config().page_id(2) == "second");
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    check_session_on_screen();
    settle();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drag past the first page's left border on a single-page home creates page 0",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1); // temperature alone on the main page
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const SlotDrag drag = drag_past_left_border(widget);
    const PageSetChange change = watch_page_set_change();
    indev.release(drag.pointer.x, drag.pointer.y);

    // The origin page is the main page, so the prune that empties a moved-from
    // page spares it: the created page takes index 0, the old one index 1.
    REQUIRE(config().page_count() == 2);
    run_page_set_rebuild(change, 1, 0);
    CHECK(count_on_page(0, "temperature") == 1);
    CHECK(count_on_page(1, "temperature") == 0);
    CHECK(config().page_id(0) == "page_3");
    CHECK(config().page_id(1) == "main");
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "outside edit mode a swipe or goto reaches the next-page slot's +",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // two pages, below the page cap
    REQUIRE(slot_container() != nullptr);
    show_page(1);
    const int32_t tile_w = lv_obj_get_content_width(scroller());
    REQUIRE(tile_w > 0);

    SECTION("a swipe from the last page") {
        swipe_toward_next_tile(empty_spot(1));
    }
    SECTION("a goto to the slot's tile") {
        helix::ui::carousel_goto_tile(carousel(), page_count(), /*animate=*/false);
        settle();
    }

    // The slot's tile holds the + that adds a page, so it is within reach
    // outside edit mode too; the panel's page stays the last config page.
    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(current_page() == page_count());
    CHECK(HomePanelTestAccess::active_page(panel()) == 1);
    CHECK(lv_obj_get_scroll_x(scroller()) == page_count() * tile_w);

    // The slot's grid is not a page: a hold on its empty area enters no edit
    // mode, which would otherwise scope the session to the off-screen last page.
    const lv_point_t spot = slot_empty_spot();
    indev.press(spot.x, spot.y);
    indev.hold(indev.long_press_hold_ms());
    indev.release(spot.x, spot.y);
    settle();
    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(current_page() == page_count());
}

TEST_CASE_METHOD(EditHomeFixture,
                 "in edit mode an idle swipe reaches the next-page slot, and a drag drops on it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'fan' alone on page 1, the last page, below the page cap
    enter_edit_mode(1);
    REQUIRE(slot_container() != nullptr);
    REQUIRE(scroll_live()); // two pages swipe in edit mode
    const int32_t tile_w = lv_obj_get_content_width(scroller());
    REQUIRE(tile_w > 0);

    SECTION("a swipe from the last page") {
        swipe_toward_next_tile(empty_spot(1));
    }
    SECTION("a goto to the slot's tile") {
        helix::ui::carousel_goto_tile(carousel(), page_count(), /*animate=*/false);
        settle();
    }

    // The carousel rests on the slot, whose + tile is in reach with no drag;
    // the session stays scoped to the last page, which is a config page.
    CHECK(current_page() == page_count());
    CHECK(lv_obj_get_scroll_x(scroller()) == page_count() * tile_w);
    CHECK(grid().page_index() == 1);
    CHECK(slot_in_reach());

    // A hold on the slot's empty area opens no widget catalog: the session's
    // grid is the off-screen last page.
    const lv_point_t spot = slot_empty_spot();
    indev.press(spot.x, spot.y);
    indev.hold(indev.long_press_hold_ms());
    indev.release(spot.x, spot.y);
    settle();
    CHECK_FALSE(grid().is_catalog_open());
    CHECK(grid().page_index() == 1);

    // A drag carries a widget onto the slot as before.
    show_page(1);
    const SlotDrag drag = drag_onto_slot(widget_on(1, "fan"));
    CHECK(drag.carousel_flips == 1);
    CHECK(current_page() == page_count());
    check_session_on_screen();

    indev.release(drag.pointer.x, drag.pointer.y);
    settle();
}

TEST_CASE_METHOD(EditHomeFixture, "a page flip during edit mode re-scopes the edited page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    REQUIRE(grid().page_index() == 0);

    // A selection, so the re-scope has selection state to drop.
    grid().select_widget(widget);
    REQUIRE(grid().selected_widget() == widget);

    // Entry disarmed page 1 too; a widget refreshing its tile on an async update
    // can arm its clicks again, and then only the re-scope disarms it.
    lv_obj_t* fan = widget_on(1, "fan");
    REQUIRE_FALSE(is_clickable(fan));
    lv_obj_add_flag(fan, LV_OBJ_FLAG_CLICKABLE);

    // A swipe comes to rest on page 1, and the page observer re-scopes.
    settle_swipe_on(1);
    CHECK(grid().page_index() == 1);
    CHECK(grid().selected_widget() == nullptr);
    lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);
    CHECK(lv_obj_get_parent(shield) == page(1));
    CHECK_FALSE(is_clickable(fan));

    // The edit session survives; leaving it still restores the swipe.
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    panel().exit_grid_edit_mode();
    CHECK(scroll_live());
    CHECK(swipe_policy() == CarouselSwipe::Auto);
}

TEST_CASE_METHOD(EditHomeFixture, "a press that follows a swipe begun on a widget arms that widget",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // A swipe that starts on the unselected widget: its first PRESSING selects
    // it, then the move hands the gesture to the carousel scroll, and the grid
    // handlers ignore that scroll's PRESSING and RELEASED.
    lv_point_t c = center_of(widget);
    indev.press(c.x, c.y);
    REQUIRE(grid().selected_widget() == widget);
    int x = c.x;
    for (int i = 0; i < 3; ++i) {
        x -= past_drag_threshold();
        indev.move(x, c.y);
    }
    REQUIRE(lv_indev_get_scroll_obj(indev.indev()) == scroller()); // the carousel took it
    // Held still for a whole pointer velocity history before the lift, so the
    // release throws nothing and the carousel settles back on the page the
    // session is scoped to.
    indev.hold(LV_INDEV_VECT_HIST_SIZE * kReadPeriodMs);
    indev.release(x, c.y);
    settle();
    REQUIRE(current_page() == 0);
    REQUIRE(grid().page_index() == 0);
    REQUIRE(grid().selected_widget() == widget);

    // A fresh press on the widget, now selected, is the grab's second step.
    c = center_of(widget);
    indev.prime(c.x, c.y);
    CHECK(GridEditModeTestAccess::press_armed(grid()));
    CHECK_FALSE(scroll_live());
    indev.release(c.x, c.y);
}

TEST_CASE_METHOD(EditHomeFixture, "re-scoping to the page already held changes nothing",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* fan = widget_on(1, "fan");
    enter_edit_mode();

    // A swipe comes to rest on page 1, and the page observer re-scopes there.
    settle_swipe_on(1);
    REQUIRE(grid().page_index() == 1);
    lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);
    REQUIRE(lv_obj_get_parent(shield) == page(1));
    REQUIRE(is_clickable(shield));
    grid().select_widget(fan);
    REQUIRE(grid().selected_widget() == fan);

    // A drag flip asks for the same scope twice: through the page observer when
    // its goto sets the page, and again from its own re-scope after the goto.
    grid().switch_page(page(1), 1);
    CHECK(grid().page_index() == 1);
    CHECK(GridEditModeTestAccess::shield(grid()) == shield);
    CHECK(lv_obj_get_parent(shield) == page(1));
    CHECK(is_clickable(shield));
    CHECK(grid().selected_widget() == fan);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drag carried across a flip brings its chrome and previews on the landing page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // temperature on page 0
    lv_obj_t* widget = widget_on(0, "temperature");
    ConfigurableTestWidget configurable;
    lv_obj_set_user_data(widget, &configurable);
    enter_edit_mode();

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    lv_obj_t* overlay = GridEditModeTestAccess::selection_overlay(grid());
    lv_obj_t* remove_btn = GridEditModeTestAccess::remove_button(grid());
    lv_obj_t* configure_btn = GridEditModeTestAccess::configure_button(grid());
    REQUIRE(overlay != nullptr);
    REQUIRE(remove_btn != nullptr);
    REQUIRE(configure_btn != nullptr);

    // One move into the right edge zone previews a cell on the origin page; the
    // dwell then flips with no further read, and the slide settles.
    const lv_area_t frame = settled_page_area();
    const int x = frame.x2 - edge_zone_px() / 2;
    indev.move(x, c.y);
    REQUIRE(GridEditModeTestAccess::snap_preview(grid()) != nullptr);
    const int previewed_col = GridEditModeTestAccess::snap_col(grid());
    const int previewed_row = GridEditModeTestAccess::snap_row(grid());
    process_lvgl(static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs));
    settle();
    REQUIRE(grid().page_index() == 1);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    check_session_on_screen();

    CHECK(lv_obj_get_parent(widget) == drag_layer());
    CHECK(lv_obj_get_parent(overlay) == drag_layer());
    CHECK(lv_obj_get_parent(remove_btn) == page(1));
    CHECK(lv_obj_get_parent(configure_btn) == page(1));
    CHECK(is_clickable(remove_btn));
    CHECK(is_clickable(configure_btn));

    // The landing page previews the cell the origin page previewed with no
    // further move, and the next move keeps that target.
    CHECK(GridEditModeTestAccess::snap_col(grid()) == previewed_col);
    CHECK(GridEditModeTestAccess::snap_row(grid()) == previewed_row);
    lv_obj_t* preview = GridEditModeTestAccess::snap_preview(grid());
    REQUIRE(preview != nullptr);
    // The preview lives on the top layer, drawn where the landing page settles.
    CHECK(lv_obj_get_parent(preview) == lv_display_get_layer_top(lv_obj_get_display(page(1))));
    lv_obj_update_layout(preview);
    const lv_area_t drawn = area_of(preview);
    CHECK(drawn.x1 >= frame.x1);
    CHECK(drawn.x1 <= frame.x2);
    indev.move(x, c.y);
    CHECK(GridEditModeTestAccess::snap_col(grid()) == previewed_col);
    CHECK(GridEditModeTestAccess::snap_row(grid()) == previewed_row);

    const int drop_x = (frame.x1 + frame.x2) / 2;
    indev.move(drop_x, c.y);
    indev.release(drop_x, c.y);
    settle();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a dragged widget is drawn where the snap and drop rules measure it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    // The page container pads its grid, so its outer corner is not where the
    // grid, and a widget laid out in it, begins.
    REQUIRE(lv_obj_get_style_space_left(page(0), LV_PART_MAIN) > 0);
    REQUIRE(lv_obj_get_style_space_top(page(0), LV_PART_MAIN) > 0);

    // The drag starts where the pointer crosses the drag threshold, with the
    // widget still laid out, and carries the widget by that point.
    const lv_area_t at_rest = area_of(widget);
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    const lv_point_t start{c.x + past_drag_threshold(), c.y};
    indev.move(start.x, start.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    const auto check_drawn_where_measured = [&](const char* when) {
        INFO(when);
        const lv_area_t drawn = area_of(widget);
        const lv_point_t measured = GridEditModeTestAccess::drag_widget_pos(grid());
        CHECK(drawn.x1 == measured.x);
        CHECK(drawn.y1 == measured.y);
    };

    // A move clear of both edge zones: the snap target and the drop measure the
    // widget at the pointer less the grab offset.
    const lv_point_t carried{start.x + tracks_px(kCellTracks), start.y + past_drag_threshold()};
    REQUIRE(carried.x < settled_page_area().x2 - edge_zone_px());
    indev.move(carried.x, carried.y);
    const lv_point_t measured = GridEditModeTestAccess::drag_widget_pos(grid());
    REQUIRE(measured.x == at_rest.x1 + (carried.x - start.x));
    REQUIRE(measured.y == at_rest.y1 + (carried.y - start.y));
    check_drawn_where_measured("after a drag move");

    // A page switch carries the drag onto another page, a page to the right
    // with the carousel unmoved, and the widget stays where the pointer holds it.
    grid().switch_page(page(1), 1);
    REQUIRE(lv_obj_get_parent(widget) == drag_layer());
    check_drawn_where_measured("carried onto page 1");
    grid().switch_page(page(0), 0);
    REQUIRE(lv_obj_get_parent(widget) == drag_layer());
    check_drawn_where_measured("carried back onto page 0");

    // The drop puts it back into the page the session is scoped to, at its
    // laid-out size.
    indev.release(carried.x, carried.y);
    settle();
    REQUIRE(helix::test::widget_in_tree(widget));
    CHECK(lv_obj_get_parent(widget) == page(0));
    lv_obj_update_layout(widget);
    const lv_area_t landed = area_of(widget);
    CHECK(lv_area_get_width(&landed) == lv_area_get_width(&at_rest));
    CHECK(lv_area_get_height(&landed) == lv_area_get_height(&at_rest));
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a flip that carries a drag leaves the widget where the pointer holds it",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // One move into the right edge zone places the widget under the pointer;
    // time then runs a few ms at a time, with no read, until the dwell flips.
    const int x = settled_page_area().x2 - edge_zone_px() / 2;
    indev.move(x, c.y);
    const lv_area_t placed = area_of(widget);
    constexpr int kStepMs = 5;
    int waited = 0;
    while (grid().page_index() == 0 &&
           waited < static_cast<int>(helix::kCrossPageDwellMs + kReadPeriodMs)) {
        process_lvgl(kStepMs);
        waited += kStepMs;
    }
    REQUIRE(grid().page_index() == 1);
    REQUIRE(lv_obj_get_parent(widget) == drag_layer());
    REQUIRE(lv_anim_count_running() > 0); // the landing page is still sliding in

    // Before the next read the widget is where the last read put it, give or
    // take the few ms the slide ran since the flip, not a page's width away
    // with the page sliding in.
    const lv_area_t carried = area_of(widget);
    const int32_t page_w = lv_obj_get_content_width(scroller());
    INFO("placed at x=" << placed.x1 << ", carried to x=" << carried.x1 << ", page " << page_w
                        << "px wide");
    CHECK(std::abs(carried.x1 - placed.x1) < page_w / 4);
    CHECK(carried.y1 == placed.y1);

    indev.release(x, c.y);
    settle();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a page change under an armed press leaves the widget on its page and drops the "
                 "press",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    REQUIRE_FALSE(GridEditModeTestAccess::dragging(grid()));
    REQUIRE_FALSE(scroll_live());

    // The carousel comes to rest on page 1 while the press is armed and not yet
    // dragging, as a swipe's snap finishing under a new press delivers it.
    settle_swipe_on(1);
    REQUIRE(grid().page_index() == 1);
    CHECK(lv_obj_get_parent(widget) == page(0));
    CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
    CHECK(grid().selected_widget() == nullptr);
    CHECK(scroll_live());
    indev.release(c.x, c.y);
}

TEST_CASE_METHOD(EditHomeFixture, "a drop's page-creating intent ends with its gesture",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1); // a single page: page 0 is the last one
    lv_obj_t* widget = widget_on(0, "temperature");
    REQUIRE(config().page_count() == 1);
    REQUIRE(config().can_add_page());
    enter_edit_mode();

    lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));

    // Held at the page's right edge until the push carries the widget's majority
    // past it: a release now would create a page.
    const int edge_x = settled_page_area().x2;
    indev.move(edge_x, c.y);
    indev.hold(helix::kCrossPageDwellMs);
    REQUIRE(GridEditModeTestAccess::drop_wants_new_page(grid()));

    // The session ends before the release, which then lands on no session.
    panel().exit_grid_edit_mode();
    indev.release(edge_x, c.y);
    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    settle();

    // A new session, on the widget the exit's rebuild laid out again.
    enter_edit_mode();
    widget = widget_on(0, "temperature");
    c = center_of(widget);

    // A hold selects the widget and grabs it, released where it grabbed.
    indev.press(c.x, c.y);                  // selects
    indev.hold(indev.long_press_hold_ms()); // its last read dispatches LONG_PRESSED: grab
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    indev.release(c.x, c.y);
    settle();

    CHECK(config().page_count() == 1);
}

TEST_CASE_METHOD(EditHomeFixture, "an edge long-press resize owns the gesture, not the swipe",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    // The entry hold below lands on the widget, not the page, so it bubbles.
    lv_obj_t* widget = widget_on(0, "temperature");

    // Two-step entry: a hold enters edit mode and SELECTS the widget under the
    // finger - it arms nothing, so the swipe stays live on a packed grid. Arming
    // (drag, or resize from the edge band) is a second press on the now-selected
    // widget.
    const lv_area_t wa = area_of(widget);
    const int mid_y = (wa.y1 + wa.y2) / 2;

    auto hold_to_select = [&](int x, int y) {
        indev.press(x, y);
        indev.hold(indev.long_press_hold_ms()); // its last read dispatches LONG_PRESSED
    };

    SECTION("entry hold selects without locking; edge press then resizes") {
        // Inside the left edge's grab band, which the session derives from the
        // cell size.
        const helix::CellMetrics m = page_metrics(0);
        const int edge_x =
            wa.x1 +
            GridEditModeTestAccess::edge_hit_band_for_cell(std::min(m.cell_w, m.cell_h)) / 2;
        hold_to_select(edge_x, mid_y);

        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        REQUIRE(grid().selected_widget() == widget);
        REQUIRE(grid().detect_resize_edge(edge_x, mid_y, wa) ==
                helix::GridEditMode::ResizeEdge::Left);
        CHECK(scroll_live()); // selection alone must not own the swipe

        indev.release(edge_x, mid_y);
        indev.prime(edge_x, mid_y); // a fresh press on the edge: arms pending
        CHECK(GridEditModeTestAccess::press_armed(grid()));
        CHECK_FALSE(scroll_live());
        indev.move(edge_x + past_drag_threshold(), mid_y); // cross the threshold
        CHECK(GridEditModeTestAccess::resizing(grid()));   // resize, not a move-drag
        CHECK(slot_in_reach()); // a resize drops nowhere, but the + tile stays shown

        indev.release(edge_x + past_drag_threshold(), mid_y);
        CHECK(scroll_live());
    }

    SECTION("entry hold selects without locking; center press then drags") {
        const int center_x = (wa.x1 + wa.x2) / 2;
        hold_to_select(center_x, mid_y);

        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        REQUIRE(grid().selected_widget() == widget);
        CHECK(scroll_live());

        indev.release(center_x, mid_y);
        indev.prime(center_x, mid_y); // a fresh press on the widget: arms pending
        CHECK(GridEditModeTestAccess::press_armed(grid()));
        CHECK_FALSE(scroll_live());
        indev.move(center_x + past_drag_threshold(), mid_y); // cross the threshold
        CHECK(GridEditModeTestAccess::dragging(grid()));     // move-drag, not a resize
        CHECK(slot_in_reach());                              // a drag can drop on the slot

        indev.release(center_x + past_drag_threshold(), mid_y);
        CHECK(scroll_live());
    }
}

TEST_CASE_METHOD(EditHomeFixture,
                 "removing a page's last widget lands on the page that takes the removed page's "
                 "index",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home_of_three_pages();

    // A tap selects widget @p id, the only one on the session's page, and its
    // remove button removes it; the page-set change that makes is watched.
    const auto remove_only_widget = [&](const char* id) {
        select_by_tap(id);
        lv_obj_t* remove_button = GridEditModeTestAccess::remove_button(grid());
        REQUIRE(remove_button != nullptr);
        lv_point_t r = center_of(remove_button);
        REQUIRE(lv_indev_search_obj(lv_screen_active(), &r) == remove_button);
        PageSetChange change = watch_page_set_change();
        indev.press(r.x, r.y);
        indev.release(r.x, r.y);
        return change;
    };

    SECTION("the last of three pages") {
        enter_edit_mode(2);
        const PageSetChange change = remove_only_widget("clock");
        // Page 2 is removed at once. Its index is past the new last page, so the
        // session goes to page 1 from the commit on, and the carousel is rebuilt
        // on page 1: the page on screen is gone, so nothing slides.
        CHECK(config().page_count() == 2);
        CHECK(grid().page_index() == 1);
        run_page_set_rebuild(change, 1);
        settle();

        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        CHECK(grid().page_index() == 1);
        check_session_on_screen();
        select_by_tap("fan");
        check_session_on_screen();
    }
    SECTION("the middle of three pages") {
        enter_edit_mode(1);
        const PageSetChange change = remove_only_widget("fan");
        // Page 1 is removed at once, and page 2 takes its index: the session goes
        // there from the commit on, and the carousel is rebuilt on it.
        CHECK(config().page_count() == 2);
        CHECK(grid().page_index() == 1);
        run_page_set_rebuild(change, 1);
        settle();

        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        CHECK(count_on_page(1, "clock") == 1);
        CHECK(grid().page_index() == 1);
        check_session_on_screen();
        select_by_tap("clock");
        check_session_on_screen();
    }
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a carousel rebuilt under the edit session leaves the session nothing of the old "
                 "carousel",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    enter_edit_mode();
    select_by_tap("temperature");
    REQUIRE(GridEditModeTestAccess::container(grid()) == page(0));
    REQUIRE(GridEditModeTestAccess::shield(grid()) != nullptr);
    REQUIRE(GridEditModeTestAccess::selection_overlay(grid()) != nullptr);

    // The rebuild a page-set change runs, before the session is shown a page of
    // the new carousel: the teardown condemned every object the session held.
    HomePanelTestAccess::rebuild_carousel(panel(), 0);
    CHECK(GridEditModeTestAccess::container(grid()) == nullptr);
    CHECK(GridEditModeTestAccess::shield(grid()) == nullptr);
    CHECK(grid().selected_widget() == nullptr);
    CHECK(GridEditModeTestAccess::selection_overlay(grid()) == nullptr);
    CHECK(GridEditModeTestAccess::remove_button(grid()) == nullptr);

    // Shown a page of the rebuilt carousel, the session is whole again.
    settle();
    settle_swipe_on(1);
    REQUIRE(grid().page_index() == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop on the next-page slot from a later page leaves the session live on the "
                 "new page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'temperature' on page 0, 'fan' alone on page 1
    enter_edit_mode(1);

    const SlotDrag drag = drag_onto_slot(widget_on(1, "fan"));
    indev.release(drag.pointer.x, drag.pointer.y);
    settle();

    // The drop created a page for 'fan' and page 1, emptied, was removed, so the
    // new page is page 1.
    REQUIRE(page_count() == 2);
    CHECK(count_on_page(1, "fan") == 1);
    CHECK(grid().page_index() == 1);
    check_session_on_screen();
    select_by_tap("fan");
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a drop rejected on a page other than its origin returns the widget to its "
                 "origin page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'temperature' at (0,0) on page 0, 'fan' at (0,0) on page 1
    lv_obj_t* temperature = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");
    const helix::PanelWidgetEntry fan = entry_on_page(1, "fan");

    const lv_area_t wa = area_of(temperature);
    const lv_point_t c = center_of(temperature);
    indev.grab(c.x, c.y);
    const int threshold_x = c.x + past_drag_threshold();
    indev.move(threshold_x, c.y);
    const int grab_offset = threshold_x - wa.x1;
    dwell_flip(1, c.y);

    // The widget carried over the cell 'fan' holds, with the pointer clear of
    // both edge zones.
    const lv_point_t drop{area_of(widget_on(1, "fan")).x1 + grab_offset, c.y};
    const lv_area_t frame = settled_page_area();
    REQUIRE(drop.x > frame.x1 + edge_zone_px());
    REQUIRE(drop.x < frame.x2 - edge_zone_px());
    indev.move(drop.x, drop.y);
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) == fan.col);
    REQUIRE(GridEditModeTestAccess::snap_row(grid()) == fan.row);
    indev.release(drop.x, drop.y);
    settle();

    // Nothing committed, and the drag resolved on its origin page: the widget,
    // the session and the carousel are back there, the widget still selected.
    CHECK(count_on_page(1, "temperature") == 0);
    const helix::PanelWidgetEntry now = entry_on_page(0, "temperature");
    CHECK(now.col == origin.col);
    CHECK(now.row == origin.row);
    CHECK(grid().page_index() == 0);
    CHECK(current_page() == 0);
    CHECK(lv_obj_get_parent(temperature) == page(0));
    CHECK_FALSE(lv_obj_has_flag(temperature, LV_OBJ_FLAG_FLOATING));
    CHECK(grid().selected_widget() == temperature);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture, "a commit saves the layout once when edits settle",
                 "[1638][edit-swipe][home][grid_edit]") {
    SECTION("a drop on the next-page slot creates a page and keeps its origin page") {
        build_home(1, 1); // 'temperature' alone on the main page, which never prunes
        lv_obj_t* widget = widget_on(0, "temperature");
        enter_edit_mode();
        const SlotDrag drag = drag_onto_slot(widget);

        ScopedConfigWriteCounter counter;
        indev.release(drag.pointer.x, drag.pointer.y);
        settle();
        REQUIRE(config().page_count() == 2);
        REQUIRE(count_on_page(1, "temperature") == 1);
        process_lvgl(static_cast<int>(helix::PanelWidgetConfig::kSaveSettleMs) + 100);
        CHECK(counter.writes() == 1);
    }
    SECTION("a move that empties its page removes the page") {
        build_home(); // 'fan' alone on page 1
        lv_obj_t* fan = widget_on(1, "fan");
        enter_edit_mode(1);
        const lv_point_t c = center_of(fan);
        indev.grab(c.x, c.y);
        indev.move(c.x + past_drag_threshold(), c.y);
        const lv_point_t zone = dwell_flip(-1, c.y);
        // Carried clear of 'temperature', a cell per read.
        const lv_area_t frame = settled_page_area();
        const int drop_x = (frame.x1 + frame.x2) / 2;
        constexpr int kFrames = 4;
        for (int f = 1; f <= kFrames; ++f) {
            indev.move(zone.x + (drop_x - zone.x) * f / kFrames, c.y);
        }
        const helix::PanelWidgetEntry temperature = entry_on_page(0, "temperature");
        REQUIRE(GridEditModeTestAccess::snap_col(grid()) >= temperature.col + temperature.colspan);

        ScopedConfigWriteCounter counter;
        indev.release(drop_x, c.y);
        settle();
        REQUIRE(config().page_count() == 1);
        REQUIRE(count_on_page(0, "fan") == 1);
        process_lvgl(static_cast<int>(helix::PanelWidgetConfig::kSaveSettleMs) + 100);
        CHECK(counter.writes() == 1);
    }
    SECTION("removing a page's last widget removes the page") {
        build_home(); // 'fan' alone on page 1
        enter_edit_mode(1);
        select_by_tap("fan");
        lv_obj_t* remove_button = GridEditModeTestAccess::remove_button(grid());
        REQUIRE(remove_button != nullptr);
        const lv_point_t r = center_of(remove_button);

        ScopedConfigWriteCounter counter;
        indev.press(r.x, r.y);
        indev.release(r.x, r.y);
        settle();
        REQUIRE(config().page_count() == 1);
        process_lvgl(static_cast<int>(helix::PanelWidgetConfig::kSaveSettleMs) + 100);
        CHECK(counter.writes() == 1);
    }
    SECTION("a move within its page") {
        build_home(1, 1);
        lv_obj_t* widget = widget_on(0, "temperature");
        enter_edit_mode();
        const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");
        const lv_point_t pointer = drag_one_cell(widget);

        ScopedConfigWriteCounter counter;
        indev.release(pointer.x, pointer.y);
        settle();
        REQUIRE(entry_on_page(0, "temperature").col == origin.col + kCellTracks);
        CHECK(counter.writes() == 0); // each write stalls a slow board's flash
        CHECK(config().save_pending());
        process_lvgl(static_cast<int>(helix::PanelWidgetConfig::kSaveSettleMs) + 100);
        process_lvgl(static_cast<int>(helix::PanelWidgetConfig::kSaveSettleMs) + 100);
        CHECK(counter.writes() == 1);
    }
}

TEST_CASE_METHOD(EditHomeFixture, "the hold that enters edit mode only selects, however long",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    // One long press is the read that enters edit mode; three reach past the
    // second long press a reset press would deliver on the shield.
    const uint32_t presses = GENERATE(1u, 3u);
    CAPTURE(presses);
    const uint32_t hold_ms = presses * indev.long_press_hold_ms();

    SECTION("on a widget") {
        const lv_point_t c = center_of(widget);
        indev.press(c.x, c.y);
        indev.hold(hold_ms);
        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        CHECK(grid().selected_widget() == widget);
        // The same finger stays down: nothing arms, and the swipe stays live.
        indev.move(c.x, c.y);
        CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
        CHECK(scroll_live());
        // Moved across the swipe axis, where the carousel cannot take the gesture.
        indev.move(c.x, c.y + past_drag_threshold());
        CHECK_FALSE(GridEditModeTestAccess::dragging(grid()));
        CHECK_FALSE(GridEditModeTestAccess::press_armed(grid()));
        CHECK(scroll_live());
        indev.release(c.x, c.y + past_drag_threshold());
    }
    SECTION("on empty grid") {
        // A hold in a session on empty grid opens it; the entry hold must not.
        ScopedWidgetCatalog catalog;
        const lv_point_t spot = empty_spot(0);
        indev.press(spot.x, spot.y);
        indev.hold(hold_ms);
        REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
        CHECK(grid().selected_widget() == nullptr);
        CHECK_FALSE(grid().is_catalog_open());
        indev.release(spot.x, spot.y);
    }
}

TEST_CASE_METHOD(EditHomeFixture,
                 "the widget catalog holds the carousel and the session on their page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // two pages
    ScopedWidgetCatalog catalog;
    enter_edit_mode();
    // The catalog offers 'clock': its entry holds no cell on any page.
    REQUIRE(count_on_page(1, "clock") == 0);
    REQUIRE_FALSE(entry_on_page(0, "clock").is_placed());

    // A hold on empty grid with nothing selected opens the catalog at that cell.
    open_catalog_by_hold();

    // The home grid stays visible beside the catalog, lattice and all, and it
    // does not swipe.
    lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);
    CHECK_FALSE(is_hidden(shield));
    CHECK_FALSE(scroll_live());

    // A page change that lands anyway, as the arrow buttons page the carousel,
    // leaves the session on the page the catalog places onto.
    show_page(1);
    CHECK(grid().page_index() == 0);
    CHECK(GridEditModeTestAccess::container(grid()) == page(0));

    ScopedWidgetCatalog::close();
    REQUIRE_FALSE(grid().is_catalog_open());
    CHECK(scroll_live());

    // The catalog's placement, run as its selection runs it after closing.
    GridEditModeTestAccess::place_from_catalog(grid(), "clock");
    CHECK(count_on_page(0, "clock") == 1);
    CHECK(count_on_page(1, "clock") == 0);
}

TEST_CASE_METHOD(EditHomeFixture, "closing the widget catalog shows the page the session is on",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // two pages
    ScopedWidgetCatalog catalog;
    enter_edit_mode();
    open_catalog_by_hold();

    // The carousel pages while the catalog holds the session, as the arrow
    // buttons page it.
    show_page(1);
    REQUIRE(grid().page_index() == 0);

    ScopedWidgetCatalog::close();
    settle();
    CHECK(current_page() == 0);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a press beside the open widget catalog closes it and reaches no grid handler",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    ScopedWidgetCatalog catalog;
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // A hold on empty grid with nothing selected opens the catalog at that cell.
    open_catalog_by_hold();

    // 'temperature' shows beside the catalog, not under it. Navigation's
    // dismiss backdrop covers the rest of the screen behind the catalog and
    // takes the press there.
    lv_point_t c = center_of(widget);
    for (lv_obj_t* obj = lv_indev_search_obj(lv_screen_active(), &c); obj != nullptr;
         obj = lv_obj_get_parent(obj)) {
        REQUIRE(obj != WidgetCatalogOverlay::active_root());
    }

    SECTION("a tap") {
        indev.press(c.x, c.y);
        indev.release(c.x, c.y);
    }
    SECTION("a hold") {
        indev.press(c.x, c.y);
        indev.hold(indev.long_press_hold_ms());
        indev.release(c.x, c.y);
    }
    ScopedWidgetCatalog::settle_queue();

    CHECK(grid().selected_widget() == nullptr);
    CHECK_FALSE(grid().owns_gesture());
    CHECK_FALSE(grid().is_catalog_open());
    CHECK(WidgetCatalogOverlay::active_root() == nullptr);
    CHECK(HomePanelTestAccess::edit_mode_active(panel()));
}

TEST_CASE_METHOD(EditHomeFixture, "the next-page slot draws no delete-page button",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'fan' alone on page 1, the last page
    lv_obj_t* fan = widget_on(1, "fan");
    enter_edit_mode(1);
    // A config page other than the main page draws it.
    REQUIRE(GridEditModeTestAccess::delete_page_button(grid()) != nullptr);

    const SlotDrag drag = drag_onto_slot(fan);
    CHECK(GridEditModeTestAccess::delete_page_button(grid()) == nullptr);

    // Ended uncommitted, the drag goes back to page 1, which draws it again.
    lv_indev_reset(nullptr, nullptr);
    indev.release(drag.pointer.x, drag.pointer.y);
    settle();
    REQUIRE(grid().page_index() == 1);
    CHECK(GridEditModeTestAccess::delete_page_button(grid()) != nullptr);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a press during a resize snap finishes the snap and takes no grid action",
                 "[1638][edit-swipe][home][grid_edit]") {
    // The snap animates only with animations on. The selection chrome pulses
    // then too, so this case pumps time instead of settling.
    helix::ui::ScopedAnimationsEnabled animations(true);
    REQUIRE(animations.available());
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());
    build_home(1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry orig = entry_on_page(0, "temperature");

    resize_down_one_cell(widget);
    REQUIRE(entry_on_page(0, "temperature").rowspan == orig.rowspan + kCellTracks);
    REQUIRE(GridEditModeTestAccess::snap_animating(grid()));

    // Before the snap ends, a press lands on the resized widget and moves past
    // the drag threshold, across the swipe axis.
    const lv_point_t c = center_of(widget);
    indev.press(c.x, c.y);
    indev.move(c.x, c.y + past_drag_threshold());
    CHECK_FALSE(GridEditModeTestAccess::snap_animating(grid()));
    CHECK_FALSE(grid().owns_gesture());
    CHECK(scroll_live());

    // The finger stays down across the rebuild the snap owed, then lifts.
    indev.hold(10 * kReadPeriodMs);
    indev.release(c.x, c.y + past_drag_threshold());
    process_lvgl(static_cast<int>(2 * kReadPeriodMs));

    CHECK_FALSE(grid().owns_gesture());
    CHECK(scroll_live());
    CHECK(entry_on_page(0, "temperature").rowspan == orig.rowspan + kCellTracks);
    // The rebuild laid the resized widget out and selected it again.
    CHECK(grid().selected_widget() == widget_on(0, "temperature"));
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a carousel rebuilt in edit mode keeps the next-page slot in reach",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'fan' alone on page 1
    lv_obj_t* fan = widget_on(1, "fan");
    enter_edit_mode(1);

    // 'fan' carried to page 0 and dropped clear of 'temperature': page 1 empties
    // and the carousel is rebuilt around one page.
    const lv_point_t c = center_of(fan);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    dwell_flip(-1, c.y);
    const lv_area_t frame = settled_page_area();
    const int drop_x = (frame.x1 + frame.x2) / 2;
    indev.move(drop_x, c.y);
    indev.release(drop_x, c.y);
    settle();
    REQUIRE(page_count() == 1);
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));
    REQUIRE_FALSE(grid().owns_gesture());

    // The rebuilt carousel holds the edit policy with no gesture live: its
    // single page has the + tile past it to swipe to, in reach.
    REQUIRE(slot_container() != nullptr);
    CHECK(scroll_live());
    CHECK(slot_in_reach());
}

TEST_CASE_METHOD(EditHomeFixture,
                 "confirming a page deletion rebuilds the carousel on the next tick",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    widget_on(1, "fan");
    enter_edit_mode(1);
    const lv_obj_t* carousel_before = carousel();

    HomePanelTestAccess::confirm_delete_page(panel());

    // The page leaves the config and the session ends at once; the carousel
    // stays as it was until the next tick.
    CHECK(config().page_count() == 1);
    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(carousel() == carousel_before);
    CHECK(page_count() == 2);
    settle();

    CHECK(carousel() != carousel_before);
    CHECK(page_count() == 1);
    CHECK(current_page() == 0);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a confirmed page deletion lands on the page that takes the deleted page's index",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home_of_three_pages();

    SECTION("the last of three pages") {
        enter_edit_mode(2);
        const PageSetChange change = watch_page_set_change();
        HomePanelTestAccess::confirm_delete_page(panel());
        REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
        // Its index is past the new last page, so the carousel is rebuilt on page
        // 1: the page on screen is gone, so nothing slides.
        run_page_set_rebuild(change, 1);
        CHECK(page_count() == 2);
        widget_on(1, "fan");
    }
    SECTION("the middle of three pages") {
        enter_edit_mode(1);
        const PageSetChange change = watch_page_set_change();
        HomePanelTestAccess::confirm_delete_page(panel());
        REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
        // Page 2 takes its index, and the carousel is rebuilt on it.
        run_page_set_rebuild(change, 1);
        CHECK(page_count() == 2);
        widget_on(1, "clock");
    }
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a hot-reload rebuild with the widget catalog open ends the edit session first",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    ScopedWidgetCatalog catalog;
    // The rebuild creates the panel from home_panel.xml, as the hot reloader
    // does after registering it again, and the XML binds the global subjects.
    app_globals_init_subjects();
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/home_panel.xml") == LV_RESULT_OK);
    ScopedHomePanelInstance registered(panel());
    enter_edit_mode();
    open_catalog_by_hold();

    // Hidden, so the rebuild skips on_activate(), whose Spoolman polling and
    // first-run tour this case does not stand up.
    lv_obj_t* root_before = HomePanelTestAccess::panel_root(panel());
    lv_obj_add_flag(root_before, LV_OBJ_FLAG_HIDDEN);
    NavigationManager::instance().rebuild_active_views();
    lv_obj_t* rebuilt_root = HomePanelTestAccess::panel_root(panel());
    REQUIRE(rebuilt_root != root_before);
    // The session ended before the tree it pointed into was condemned, and took
    // the catalog it opened with it.
    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    ScopedWidgetCatalog::settle_queue();
    CHECK(WidgetCatalogOverlay::active_root() == nullptr);
    CHECK_FALSE(grid().is_catalog_open());

    // The rebuild appends the new root to its parent, here the screen the
    // catalog sat on; the app's panel parent holds panels only, below it.
    lv_obj_remove_flag(rebuilt_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(rebuilt_root);
    adopt_panel_root();
    settle();

    // The rebuilt panel starts a new session, and its grid input acts.
    enter_edit_mode();
    select_by_tap("temperature");
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture, "leaving edit mode closes the widget catalog the session opened",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    ScopedWidgetCatalog catalog;
    enter_edit_mode();
    open_catalog_by_hold();

    // The navigation bar's Done leaves through the one exit.
    panel().exit_grid_edit_mode();
    ScopedWidgetCatalog::settle_queue();

    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(WidgetCatalogOverlay::active_root() == nullptr);
    CHECK_FALSE(grid().is_catalog_open());
}

TEST_CASE_METHOD(EditHomeFixture,
                 "with no dismiss backdrop, a press beside the open widget catalog takes no grid "
                 "action",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    ScopedWidgetCatalog catalog;
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const lv_point_t origin_spot = open_catalog_by_hold();
    const lv_point_t c = center_of(widget);
    remove_dismiss_backdrop(c);

    SECTION("a tap") {
        indev.press(c.x, c.y);
        indev.release(c.x, c.y);
    }
    SECTION("a hold") {
        indev.press(c.x, c.y);
        indev.hold(indev.long_press_hold_ms());
        indev.release(c.x, c.y);
    }
    ScopedWidgetCatalog::settle_queue();

    CHECK(grid().selected_widget() == nullptr);
    CHECK_FALSE(grid().owns_gesture());
    CHECK(grid().is_catalog_open());
    REQUIRE(HomePanelTestAccess::edit_mode_active(panel()));

    // The catalog still places at the cell its opening hold landed on. The
    // widget placed spans one cell and snaps to half cells, so that cell holds
    // it as it stands.
    lv_area_t content;
    const helix::CellMetrics m = page_metrics(0, &content);
    const auto [col, row] = helix::GridEditMode::screen_to_grid_cell(
        origin_spot.x, origin_spot.y, content.x1, content.y1, lv_area_get_width(&content),
        lv_area_get_height(&content), m.cols, m.rows, m.gutter);
    const auto* def = helix::find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);
    REQUIRE(def->supports_half_col);
    REQUIRE(def->supports_half_row);
    REQUIRE(col + def->colspan <= m.cols);
    REQUIRE(row + def->rowspan <= m.rows);
    ScopedWidgetCatalog::close();
    GridEditModeTestAccess::place_from_catalog(grid(), "tool_switcher");
    const helix::PanelWidgetEntry placed = entry_on_page(0, "tool_switcher");
    CHECK(placed.col == col);
    CHECK(placed.row == row);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "with no dismiss backdrop, the selected widget beside the open widget catalog "
                 "takes no action",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    ScopedWidgetCatalog catalog;
    enter_edit_mode();
    select_by_tap("temperature");
    lv_obj_t* widget = grid().selected_widget();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");
    // The navigation bar's + opens the catalog with the selection standing.
    panel().open_widget_catalog();
    ScopedWidgetCatalog::settle_queue();
    REQUIRE(grid().is_catalog_open());
    lv_obj_t* remove_button = GridEditModeTestAccess::remove_button(grid());
    REQUIRE(remove_button != nullptr);
    const lv_point_t c = center_of(widget);
    remove_dismiss_backdrop(c);

    SECTION("a press on it moved past the drag threshold") {
        indev.press(c.x, c.y);
        indev.move(c.x, c.y + past_drag_threshold());
        CHECK_FALSE(grid().owns_gesture());
        indev.release(c.x, c.y + past_drag_threshold());
    }
    SECTION("a tap on its remove button") {
        lv_point_t r = center_of(remove_button);
        REQUIRE(lv_indev_search_obj(lv_screen_active(), &r) == remove_button);
        indev.press(r.x, r.y);
        indev.release(r.x, r.y);
    }
    ScopedWidgetCatalog::settle_queue();

    CHECK(grid().selected_widget() == widget);
    const helix::PanelWidgetEntry now = entry_on_page(0, "temperature");
    CHECK(now.is_placed());
    CHECK(now.col == origin.col);
    CHECK(now.row == origin.row);
    CHECK(grid().is_catalog_open());
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a pointer held in the edge zone at the read cadence flips when the dwell runs "
                 "out",
                 "[1638][edit-swipe][home][grid_edit]") {
    // Two cells wide and grabbed near its trailing side, so the edge push cannot
    // carry its majority across the border before the dwell runs out.
    build_home(2, 2, 2 * kCellTracks);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    const lv_area_t wa = area_of(widget);
    const int width = lv_area_get_width(&wa);
    const int cy = (wa.y1 + wa.y2) / 2;
    const int grab_x = wa.x1 + width * 8 / 10;
    REQUIRE(grid().detect_resize_edge(grab_x, cy, wa) == helix::GridEditMode::ResizeEdge::None);
    indev.grab(grab_x, cy);
    const int threshold_x = grab_x + past_drag_threshold();
    indev.move(threshold_x, cy);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    const int grab_offset = threshold_x - wa.x1;

    // At the zone's inner edge, where the push has furthest to carry the widget.
    const lv_area_t frame = settled_page_area();
    const int x = frame.x2 - edge_zone_px();
    const int push_needed_px = (frame.x2 - width / 2) - (x - grab_offset) + 1;
    const uint32_t crossing_flip_ms =
        static_cast<uint32_t>(push_needed_px * 1000 / push_px_per_s()) +
        helix::kCrossPageCrossingDelayMs;
    INFO("push " << push_needed_px << "px, crossing flip after " << crossing_flip_ms << "ms");
    REQUIRE(crossing_flip_ms > helix::kCrossPageDwellMs + 2 * kReadPeriodMs);

    // A read every period, for less than the dwell: nothing flips.
    indev.move(x, cy);
    indev.hold(helix::kCrossPageDwellMs - kReadPeriodMs);
    CHECK(grid().page_index() == 0);
    CHECK(current_page() == 0);

    // The reads carry on past the dwell, and it flips one page.
    indev.hold(2 * kReadPeriodMs);
    CHECK(grid().page_index() == 1);
    CHECK(current_page() == 1);
    CHECK(GridEditModeTestAccess::dragging(grid()));
    indev.release(x, cy);
    settle();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a move that empties a page before its landing page keeps the session with the "
                 "moved widget",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(2, 4); // 'fan' alone on page 1, pages 2 and 3 empty
    lv_obj_t* fan = widget_on(1, "fan");
    enter_edit_mode(1);

    const lv_point_t c = center_of(fan);
    indev.grab(c.x, c.y);
    indev.move(c.x + past_drag_threshold(), c.y);
    const lv_point_t zone = dwell_flip(1, c.y);
    REQUIRE(grid().page_index() == 2);

    // Carried out of the edge zone onto empty grid, a few cells per read.
    const lv_area_t frame = settled_page_area();
    const int drop_x = (frame.x1 + frame.x2) / 2;
    constexpr int kFrames = 4;
    for (int f = 1; f <= kFrames; ++f) {
        indev.move(zone.x + (drop_x - zone.x) * f / kFrames, c.y);
    }
    REQUIRE(GridEditModeTestAccess::snap_col(grid()) >= 0);
    const PageSetChange change = watch_page_set_change();
    indev.release(drop_x, c.y);
    // Page 1 emptied and was removed, so the page 'fan' landed on is page 1 now,
    // and the carousel is rebuilt on it.
    run_page_set_rebuild(change, 1);
    settle();

    // The session and the carousel are on the page 'fan' landed on.
    REQUIRE(config().page_count() == 3);
    CHECK(count_on_page(1, "fan") == 1);
    CHECK(grid().page_index() == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a release with the widget's majority past the last page's border creates the "
                 "page before any flip and slides there from its origin page",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(1, 1);
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry origin = entry_on_page(0, "temperature");

    // Grabbed left of center, so one move to the page's right edge carries the
    // widget's majority past the border.
    const lv_area_t wa = area_of(widget);
    const int grab_x = wa.x1 + lv_area_get_width(&wa) * 3 / 10;
    const int y = (wa.y1 + wa.y2) / 2;
    indev.grab(grab_x, y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    indev.move(grab_x - past_drag_threshold(), y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    const int edge_x = settled_page_area().x2;
    indev.move(edge_x, y);
    REQUIRE(GridEditModeTestAccess::snap_row(grid()) == origin.row);

    // The release follows in the same read period, before the crossing flips.
    REQUIRE(grid().page_index() == 0);
    REQUIRE(current_page() == 0);
    const PageSetChange change = watch_page_set_change();
    indev.release(edge_x, y);
    // The origin page survives, and it is the page on screen: the rebuilt
    // carousel comes up there, and one slide carries it to the new page, where
    // the session goes.
    run_page_set_rebuild(change, 0, 1);
    CHECK(grid().page_index() == 1);

    // The page past the last one is created, with the widget in its rightmost
    // columns on the row the drag previewed.
    CHECK(config().page_count() == 2);
    CHECK(count_on_page(0, "temperature") == 0);
    REQUIRE(count_on_page(1, "temperature") == 1);
    const helix::PanelWidgetEntry landed = entry_on_page(1, "temperature");
    const helix::CellMetrics m = page_metrics(1);
    CHECK(landed.col == m.cols - origin.colspan);
    CHECK(landed.row == origin.row);
    CHECK(grid().page_index() == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a release past the last page's border that empties its origin page lands the "
                 "new page on the origin's index with nothing to slide",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home(); // 'temperature' on page 0, 'fan' alone on page 1, the last page
    lv_obj_t* widget = widget_on(1, "fan");
    enter_edit_mode(1);
    const helix::PanelWidgetEntry origin = entry_on_page(1, "fan");

    // Grabbed left of center, so one move to the page's right edge carries the
    // widget's majority past the border, and the release follows in the same
    // read period, before the crossing flips.
    const lv_area_t wa = area_of(widget);
    const int grab_x = wa.x1 + lv_area_get_width(&wa) * 3 / 10;
    const int y = (wa.y1 + wa.y2) / 2;
    indev.grab(grab_x, y);
    indev.move(grab_x - past_drag_threshold(), y);
    REQUIRE(GridEditModeTestAccess::dragging(grid()));
    const int edge_x = settled_page_area().x2;
    indev.move(edge_x, y);
    REQUIRE(grid().page_index() == 1);
    REQUIRE(current_page() == 1);
    const PageSetChange change = watch_page_set_change();
    indev.release(edge_x, y);
    // Page 1 emptied and was removed, so the page the drop created is page 1, the
    // index of the page on screen: the carousel comes up on it with nothing to
    // slide.
    REQUIRE(config().page_count() == 2);
    run_page_set_rebuild(change, 1);
    settle();

    REQUIRE(count_on_page(1, "fan") == 1);
    const helix::PanelWidgetEntry landed = entry_on_page(1, "fan");
    CHECK(landed.col == page_metrics(1).cols - origin.colspan);
    CHECK(landed.row == origin.row);
    CHECK(grid().page_index() == 1);
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a press after a rebuild deleted a live gesture's shield ends that gesture and "
                 "frees the swipe",
                 "[1638][edit-swipe][home][grid_edit]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();

    // A reset ends a grab, and the rebuild its end schedules waits for the next
    // tick.
    const lv_point_t c = center_of(widget);
    indev.grab(c.x, c.y);
    lv_indev_reset(nullptr, nullptr);
    indev.release(c.x, c.y);
    REQUIRE_FALSE(grid().owns_gesture());

    // Before that tick, a fresh grab arms a press on the shield the rebuild
    // deletes.
    indev.grab(c.x, c.y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    REQUIRE_FALSE(scroll_live());
    const lv_obj_t* condemned = GridEditModeTestAccess::shield(grid());

    // The rebuild runs with the finger down. It moves the shield off its page
    // before the delete, so the reset the delete sends reaches no grid handler,
    // and the lift that follows reaches no object.
    process_lvgl(static_cast<int>(kReadPeriodMs));
    process_lvgl(static_cast<int>(kReadPeriodMs));
    REQUIRE(GridEditModeTestAccess::shield(grid()) != condemned);
    REQUIRE(indev.indev()->pointer.act_obj == nullptr);
    indev.release(c.x, c.y);
    REQUIRE(GridEditModeTestAccess::press_armed(grid()));
    REQUIRE_FALSE(scroll_live());

    // The next press lands on the new shield and ends the stranded gesture.
    const lv_point_t spot = empty_spot(0);
    indev.press(spot.x, spot.y);
    CHECK_FALSE(grid().owns_gesture());
    CHECK(scroll_live());
    indev.release(spot.x, spot.y);
    settle();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a page change during a resize snap stops the snap, and a press on the new page "
                 "acts",
                 "[1638][edit-swipe][home][grid_edit]") {
    // The snap animates only with animations on. The selection chrome pulses
    // then too, so this case pumps time instead of settling.
    helix::ui::ScopedAnimationsEnabled animations(true);
    REQUIRE(animations.available());
    build_home(); // 'fan' on page 1
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const helix::PanelWidgetEntry orig = entry_on_page(0, "temperature");
    resize_down_one_cell(widget);
    REQUIRE(entry_on_page(0, "temperature").rowspan == orig.rowspan + kCellTracks);
    REQUIRE(GridEditModeTestAccess::snap_animating(grid()));

    // Before the snap ends, a swipe comes to rest on page 1.
    settle_swipe_on(1);
    REQUIRE(grid().page_index() == 1);
    CHECK_FALSE(GridEditModeTestAccess::snap_animating(grid()));

    // A tap there selects, and the selection outlasts the time the snap would
    // have run.
    select_by_tap("fan");
    process_lvgl(static_cast<int>(10 * kReadPeriodMs));
    CHECK(grid().selected_widget() == widget_on(1, "fan"));
    check_session_on_screen();
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a resize whose snap a page change stopped shows its new size on its page",
                 "[1638][edit-swipe][home][grid_edit]") {
    // The snap animates only with animations on. The selection chrome pulses
    // then too, so this case pumps time instead of settling.
    helix::ui::ScopedAnimationsEnabled animations(true);
    REQUIRE(animations.available());
    build_home(); // 'fan' on page 1
    lv_obj_t* widget = widget_on(0, "temperature");
    enter_edit_mode();
    const lv_area_t before = area_of(widget);
    resize_down_one_cell(widget);
    REQUIRE(GridEditModeTestAccess::snap_animating(grid()));
    const lv_obj_t* shield = GridEditModeTestAccess::shield(grid());
    REQUIRE(shield != nullptr);

    // A swipe to page 1 before the snap ends. The session takes its own shield
    // there, and nothing on either page is rebuilt.
    settle_swipe_on(1);
    REQUIRE(grid().page_index() == 1);
    REQUIRE_FALSE(GridEditModeTestAccess::snap_animating(grid()));
    process_lvgl(static_cast<int>(10 * kReadPeriodMs));
    CHECK(GridEditModeTestAccess::shield(grid()) == shield);
    check_session_on_screen();
    CHECK(widget_on(0, "temperature") == widget);

    // And back.
    settle_swipe_on(0);
    REQUIRE(grid().page_index() == 0);
    process_lvgl(static_cast<int>(10 * kReadPeriodMs));

    const lv_area_t now = area_of(widget_on(0, "temperature"));
    INFO("height before " << lv_area_get_height(&before) << ", now " << lv_area_get_height(&now));
    CHECK(lv_area_get_height(&now) > lv_area_get_height(&before));
}

TEST_CASE_METHOD(EditHomeFixture,
                 "the delete-page button asks first, and its confirmation removes the page",
                 "[1638][edit-swipe][home][grid_edit]") {
    // The confirmation is the app's modal_dialog component.
    ui_dialog_register();
    for (const char* file : {"A:ui_xml/divider_horizontal.xml", "A:ui_xml/divider_vertical.xml",
                             "A:ui_xml/modal_dialog.xml"}) {
        INFO(file);
        REQUIRE(lv_xml_register_component_from_file(file) == LV_RESULT_OK);
    }
    helix::ui::modal_init_subjects();
    build_home();
    widget_on(1, "fan");
    enter_edit_mode(1);

    lv_obj_t* button = GridEditModeTestAccess::delete_page_button(grid());
    REQUIRE(button != nullptr);
    lv_point_t b = center_of(button);
    REQUIRE(lv_indev_search_obj(lv_screen_active(), &b) == button);
    indev.press(b.x, b.y);
    indev.release(b.x, b.y);
    process_lvgl(static_cast<int>(kReadPeriodMs));

    // The tap asks, and removes nothing yet.
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    CHECK(config().page_count() == 2);
    CHECK(HomePanelTestAccess::edit_mode_active(panel()));

    lv_obj_t* confirm = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(confirm != nullptr);
    lv_point_t p = center_of(confirm);
    REQUIRE(lv_indev_search_obj(lv_screen_active(), &p) == confirm);
    indev.press(p.x, p.y);
    indev.release(p.x, p.y);
    settle();

    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK(config().page_count() == 1);
    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    CHECK(page_count() == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "home_panel.xml gives carousel_host every grid handler",
                 "[1638][edit-swipe][home]") {
    // As at app startup: the global subjects the XML binds, and the panel's
    // callbacks registered before its XML is created.
    app_globals_init_subjects();
    HomePanel& panel = get_global_home_panel();
    if (!panel.are_subjects_initialized()) {
        panel.init_subjects();
    }
    REQUIRE(lv_xml_get_event_cb(nullptr, "on_home_grid_press_cancelled") != nullptr);

    REQUIRE(register_component("home_panel"));
    lv_obj_t* root = create_component("home_panel");
    REQUIRE(root != nullptr);
    lv_obj_t* host = lv_obj_find_by_name(root, "carousel_host");
    REQUIRE(host != nullptr);

    // EditHomeFixture wires its stand-in host from the same list.
    const auto handlers = HomePanelTestAccess::grid_handlers();
    for (const auto& handler : handlers) {
        int found = 0;
        for (uint32_t i = 0; i < lv_obj_get_event_count(host); ++i) {
            const lv_event_dsc_t* dsc = lv_obj_get_event_dsc(host, i);
            if (dsc->cb == handler.cb && dsc->filter == static_cast<uint32_t>(handler.code)) {
                ++found;
            }
        }
        INFO("event code " << static_cast<int>(handler.code));
        CHECK(found == 1);
    }
    CHECK(lv_obj_get_event_count(host) == handlers.size());
}

/// Resolves the live XML-registered subject, not any owner's copy — the same
/// isolation rule test_ui_panel_bindings.cpp states for its setter.
static void set_xml_subject(const char* name, int value) {
    lv_subject_t* subject = lv_xml_get_subject(NULL, name);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, value);
}

TEST_CASE_METHOD(XMLTestFixture, "the page badge tracks edit mode and populated pages",
                 "[1638][edit-swipe][home][bind_flag]") {
    // Two configured pages, or on_page_changed ignores the flip before the
    // badge could ever be written.
    ScopedHomeConfig home_config(2);

    // As at app startup: the global subjects, home_edit_mode among them, are
    // registered before any panel exists, and constructing the panel registers
    // the badge subjects before the XML binds.
    app_globals_init_subjects();
    HomePanel& panel = get_global_home_panel();

    REQUIRE(register_component("home_panel"));
    lv_obj_t* root = create_component("home_panel");
    REQUIRE(root != nullptr);
    lv_obj_t* badge = lv_obj_find_by_name(root, "page_badge");
    REQUIRE(badge != nullptr);

    lv_subject_t* edit_mode = lv_xml_get_subject(NULL, "home_edit_mode");
    REQUIRE(edit_mode != nullptr);

    SECTION("hidden outside edit mode even with several populated pages") {
        set_xml_subject("home_populated_pages", 2);
        lv_subject_set_int(edit_mode, 0);
        CHECK(is_hidden(badge));
    }

    SECTION("hidden in edit mode when only one page is populated") {
        set_xml_subject("home_populated_pages", 1);
        lv_subject_set_int(edit_mode, 1);
        CHECK(is_hidden(badge));
    }

    SECTION("shown in edit mode with several populated pages, tracking the page") {
        set_xml_subject("home_populated_pages", 2);
        lv_subject_set_int(edit_mode, 1);
        CHECK_FALSE(is_hidden(badge));

        HomePanelTestAccess::fire_page_changed(panel, 1);
        CHECK(std::string(lv_label_get_text(badge)) == "2 / 2");
        HomePanelTestAccess::fire_page_changed(panel, 0);
        CHECK(std::string(lv_label_get_text(badge)) == "1 / 2");
    }

    SECTION("the denominator counts all pages, not populated ones - deliberate") {
        // Gate and denominator disagree by design: visibility asks "is there
        // enough content to bother numbering", the text reports position among
        // real carousel pages (empty ones included, the next-page slot excluded) -
        // the same thing the indicator dots count.
        ScopedHomeConfig mixed(2, 3);
        set_xml_subject("home_populated_pages", 2);
        lv_subject_set_int(edit_mode, 1);
        CHECK_FALSE(is_hidden(badge));
        // The panel's active page may already sit at 0 from an earlier
        // section run; fire a real change so the badge actually updates.
        HomePanelTestAccess::fire_page_changed(panel, 1);
        CHECK(std::string(lv_label_get_text(badge)) == "2 / 3");
    }

    SECTION("the populated count comes from the config through populate_widgets") {
        // No page containers installed: the per-page populate loop is a no-op,
        // leaving exactly the count recomputation this pins. Zero the subject
        // first — an earlier section's run set it, and subject values outlive
        // Catch2's section re-runs.
        lv_subject_set_int(edit_mode, 1);
        set_xml_subject("home_populated_pages", 0);
        panel.populate_widgets();
        CHECK(lv_subject_get_int(lv_xml_get_subject(NULL, "home_populated_pages")) == 2);
        CHECK_FALSE(is_hidden(badge));
    }

    lv_subject_set_int(edit_mode, 0);
}

TEST_CASE_METHOD(XMLTestFixture, "deinit_subjects withdraws the page badge subjects",
                 "[1638][edit-swipe][home]") {
    app_globals_init_subjects();
    HomePanel& panel = get_global_home_panel();
    const bool subjects_were_initialized = panel.are_subjects_initialized();

    // Published by the constructor, not by init_subjects(), so deinit_subjects()
    // withdraws them whether or not init_subjects() ran.
    static constexpr std::array<const char*, 2> kPanelSubjects = {"home_page_badge",
                                                                  "home_populated_pages"};
    for (const char* name : kPanelSubjects) {
        INFO(name);
        REQUIRE(lv_xml_get_subject(NULL, name) != nullptr);
    }

    panel.deinit_subjects();
    for (const char* name : kPanelSubjects) {
        INFO(name);
        CHECK(lv_xml_get_subject(NULL, name) == nullptr);
    }

    // The global panel serves every later case: publish its subjects again, as
    // its constructor and init_subjects() did.
    HomePanelTestAccess::init_panel_subjects(panel);
    if (subjects_were_initialized) {
        panel.init_subjects();
    }
}

// Last in the file: LVGLUITestFixture's teardown deinitializes the global
// subjects the cases above re-init only where they need them.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "the app's XML registration registers the carousel's page components",
                 "[1638][edit-swipe][home]") {
    // build_carousel() creates every page and the next-page slot from these
    // components by name. EditHomeFixture registers them from file itself and
    // the registry and component loader are process-wide, so the case starts
    // from neither: the lookups below resolve only through what
    // register_xml_components() installs.
    static constexpr std::array<const char*, 2> kPageComponents = {"home_page_container",
                                                                   "home_next_page_slot"};
    lv_xml_set_component_loader(nullptr);
    helix::ScopeExit restore_loader([] { helix::register_xml_on_first_use(); });
    for (const char* name : kPageComponents) {
        INFO(name);
        lv_xml_component_unregister(name);
        REQUIRE(lv_xml_component_get_scope(name) == nullptr);
    }

    helix::register_xml_components();

    for (const char* name : kPageComponents) {
        INFO(name);
        CHECK(lv_xml_component_get_scope(name) != nullptr);
    }
}

// A page population drains queued work under its own frame. A drained callback
// that builds or rebuilds the carousel must leave the panel holding only the new
// carousel, with every page populated on it:
//  - a full population draining a rebuild;
//  - a population whose page only flips hardware gates (a partial path with a
//    drain of its own) draining a rebuild;
//  - a population draining finalize_setup()'s build, which has no teardown
//    before it, so the outer population must still read as stale.
TEST_CASE_METHOD(EditHomeFixture,
                 "a carousel built by work a page population drains ends up populated",
                 "[home][drain_reentrancy]") {
    struct Row {
        const char* what;
        bool gate_flip;  // record page 0 as built with its first widget gated
        bool full_build; // drain build_carousel() rather than rebuild_carousel()
    };
    const Row row = GENERATE(Row{"rebuild under a population", false, false},
                             Row{"rebuild under a gate-flip population", true, false},
                             Row{"build without teardown under a population", false, true});
    INFO(row.what);

    build_home();
    if (row.gate_flip) {
        auto ids = HomePanelTestAccess::built_ids(panel(), 0);
        REQUIRE_FALSE(ids.empty());
        ids[0] += helix::kGatedIdSuffix;
        HomePanelTestAccess::set_built_ids(panel(), 0, ids);
    }
    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::build_mid_populate", [this, &ran, row]() {
        ran = true;
        if (row.full_build) {
            HomePanelTestAccess::build_carousel(panel());
        } else {
            HomePanelTestAccess::rebuild_carousel(panel(), 0);
        }
    });

    if (row.gate_flip) {
        HomePanelTestAccess::populate_unforced(panel());
    } else {
        HomePanelTestAccess::populate(panel());
    }
    REQUIRE(ran);
    settle();

    REQUIRE(page_count() == 2);
    for (int i = 0; i < page_count(); ++i) {
        INFO("page " << i);
        REQUIRE(helix::test::widget_in_tree(page(i)));
        CHECK(helix::ui::is_on_active_screen(page(i)));
    }
    CHECK(widget_on(0, "temperature") != nullptr);
    CHECK(widget_on(1, "fan") != nullptr);
}

TEST_CASE_METHOD(EditHomeFixture,
                 "a carousel rebuilt by work a teardown drains is torn down with it",
                 "[home][drain_reentrancy]") {
    build_home();
    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::rebuild_mid_teardown", [this, &ran]() {
        ran = true;
        HomePanelTestAccess::rebuild_carousel(panel(), 0);
    });

    HomePanelTestAccess::release_carousel(panel());
    REQUIRE(ran);

    // Nothing of either carousel is left for the panel to reach.
    CHECK(page_count() == 0);
    CHECK(carousel() == nullptr);
    CHECK(slot_container() == nullptr);
    settle();
}

// A build whose page population drains a rebuild that shrinks the page set must
// stop: its own page count no longer describes the carousel.
TEST_CASE_METHOD(EditHomeFixture,
                 "a carousel build whose population drains a smaller rebuild stops there",
                 "[home][drain_reentrancy]") {
    build_home();
    REQUIRE(page_count() == 2);

    // Queued from inside the teardown's frozen drain, so it lands in the first
    // page population's drain of the build that follows.
    helix::ui::UpdateQueue::instance().queue("test::requeue_shrink", [this]() {
        helix::ui::UpdateQueue::instance().queue("test::shrink_and_rebuild", [this]() {
            REQUIRE(config().remove_page(1));
            HomePanelTestAccess::rebuild_carousel(panel(), 0);
        });
    });

    helix::LogCapture log(512);
    HomePanelTestAccess::rebuild_carousel(panel(), 0);
    settle();

    CHECK(log.count_containing("out of range") == 0);
    REQUIRE(page_count() == 1);
    CHECK(widget_on(0, "temperature") != nullptr);
}

// A page that cannot be re-seated in place is populated instead, with a drain of
// its own. A rebuild run by that drain owns the pages, so the re-seat stops
// rather than re-seating the rebuild's pages from its stale loop.
TEST_CASE_METHOD(EditHomeFixture,
                 "a re-seat whose page population drains a carousel rebuild stops there",
                 "[home][drain_reentrancy]") {
    build_home();
    settle();
    // An entry with no cell cannot be re-seated, so page 0 is populated.
    auto& entries = config().page_entries_mut(0);
    auto temp = std::find_if(entries.begin(), entries.end(), [](const helix::PanelWidgetEntry& e) {
        return e.id == "temperature";
    });
    REQUIRE(temp != entries.end());
    temp->col = -1;
    temp->row = -1;

    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::rebuild_mid_reseat", [this, &ran]() {
        ran = true;
        HomePanelTestAccess::rebuild_carousel(panel(), 0);
    });

    helix::LogCapture log(512);
    CHECK(HomePanelTestAccess::reseat(panel()));
    REQUIRE(ran);
    CHECK(log.count_containing("re-seated") == 0);
    settle();
    REQUIRE(page_count() == 2);
    CHECK(widget_on(1, "fan") != nullptr);
}

// ============================================================================
// Drag surfaces: a widget's drag-consuming object keeps the hold and the swipe
// ============================================================================

namespace {

/// A child filling @p widget, bubbling its events the way population makes a
/// widget's children bubble (set_event_bubble_recursive).
lv_obj_t* fill_child(lv_obj_t* widget, lv_obj_t* child) {
    lv_obj_set_size(child, LV_PCT(100), LV_PCT(100));
    lv_obj_center(child);
    lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_update_layout(widget);
    return child;
}

} // namespace

TEST_CASE_METHOD(EditHomeFixture, "a hold on a widget's drag surface does not enter edit mode",
                 "[home][drag_surface]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    lv_obj_t* surface = fill_child(widget, lv_obj_create(widget));

    SECTION("marked: the hold belongs to the surface") {
        helix::ui::mark_drag_surface(surface);
        const lv_point_t c = center_of(surface);
        indev.press(c.x, c.y);
        indev.hold(indev.long_press_hold_ms());
        CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
        indev.release(c.x, c.y);
    }

    SECTION("unmarked: the hold enters edit mode") {
        const lv_point_t c = center_of(surface);
        indev.press(c.x, c.y);
        indev.hold(indev.long_press_hold_ms());
        CHECK(HomePanelTestAccess::edit_mode_active(panel()));
        indev.release(c.x, c.y);
    }
}

TEST_CASE_METHOD(EditHomeFixture, "a hold on a fan arc's ring does not enter edit mode",
                 "[home][drag_surface][fan]") {
    build_home();
    lv_obj_t* widget = widget_on(0, "temperature");
    lv_obj_t* arc = fill_child(widget, lv_arc_create(widget));
    // The arc's knob track runs around its edge; press on the ring.
    const lv_point_t ring{center_of(arc).x, area_of(arc).y1 + 4};
    indev.press(ring.x, ring.y);
    indev.hold(indev.long_press_hold_ms());
    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
    indev.release(ring.x, ring.y);
}

TEST_CASE_METHOD(EditHomeFixture, "a horizontal drag on a drag surface leaves the page in place",
                 "[home][drag_surface]") {
    build_home();
    REQUIRE(scroll_live());
    lv_obj_t* widget = widget_on(0, "temperature");
    lv_obj_t* surface = fill_child(widget, lv_obj_create(widget));
    helix::ui::mark_drag_surface(surface);

    constexpr int kMoves = 8;
    const int32_t step = (2 * lv_obj_get_content_width(scroller()) / 3) / kMoves;
    REQUIRE(step > 0);
    const lv_point_t c = center_of(surface);
    indev.press(c.x, c.y);
    int32_t x = c.x;
    for (int i = 0; i < kMoves; ++i) {
        x -= step;
        indev.move(x, c.y);
    }
    indev.hold(LV_INDEV_VECT_HIST_SIZE * kReadPeriodMs);
    CHECK(lv_indev_get_scroll_obj(indev.indev()) == nullptr);
    indev.release(x, c.y);
    settle();

    CHECK(current_page() == 0);
    CHECK(lv_obj_get_scroll_x(scroller()) == 0);
    CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel()));
}

TEST_CASE_METHOD(LVGLTestFixture, "mark_drag_surface sets the flag and the input flags it needs",
                 "[home][drag_surface]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    REQUIRE_FALSE(helix::ui::is_drag_surface_hit(obj, parent));

    helix::ui::mark_drag_surface(obj);

    CHECK(lv_obj_has_flag(obj, helix::ui::kDragSurfaceFlag));
    CHECK(lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_has_flag(obj, LV_OBJ_FLAG_PRESS_LOCK));
    CHECK_FALSE(lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE));
    CHECK_FALSE(lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_HOR));
    CHECK_FALSE(lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    CHECK_FALSE(lv_obj_has_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE));

    // A press on a child of the surface is a hit; the walk stops at stop_at.
    lv_obj_t* child = lv_obj_create(obj);
    CHECK(helix::ui::is_drag_surface_hit(child, parent));
    CHECK_FALSE(helix::ui::is_drag_surface_hit(parent, parent));
    CHECK_FALSE(helix::ui::is_drag_surface_hit(child, child));
}
