// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_full_page_widgets.cpp
 * @brief Full-page home widgets (prestonbrown/helixscreen#1931)
 *
 * Run with: ./build/bin/helix-tests "[full_page]"
 *
 * A Motion tile that covers a whole page other than the main page swaps its
 * compact face for full_page_frame and, while that page is shown, hosts the
 * Motion overlay's own tree (OverlayBase::embed()). A Filament operations tile
 * does the same with the Filament main panel, whose one tree NavigationManager
 * lends to the page and takes back (PanelBase::embed()).
 */

#include "ui_carousel.h"
#include "ui_nav_manager.h"
#include "ui_panel_filament.h"
#include "ui_panel_home.h"
#include "ui_panel_motion.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "grid_layout.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/filament_ops_widget.h"
#include "src/ui/panel_widgets/heater_temp_widget.h"
#include "src/ui/panel_widgets/motion_widget.h"
#include "tool_state.h"

#include <array>
#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::cell_fills_extra_page;
using helix::TrackRect;

TEST_CASE("cell_fills_extra_page: only a whole grid on a page other than the main one",
          "[full_page][layout]") {
    constexpr int kCols = 12;
    constexpr int kRows = 8;
    const TrackRect whole{0, 0, kCols, kRows};

    CHECK(cell_fills_extra_page(1, 0, whole, kCols, kRows));
    // The main page is never a full page, wherever it sits in the page order.
    CHECK_FALSE(cell_fills_extra_page(0, 0, whole, kCols, kRows));
    CHECK_FALSE(cell_fills_extra_page(2, 2, whole, kCols, kRows));
    CHECK(cell_fills_extra_page(0, 2, whole, kCols, kRows));
    // One track short on either axis, or shifted off the origin, is a tile.
    CHECK_FALSE(cell_fills_extra_page(1, 0, {0, 0, kCols - 1, kRows}, kCols, kRows));
    CHECK_FALSE(cell_fills_extra_page(1, 0, {0, 0, kCols, kRows - 1}, kCols, kRows));
    CHECK_FALSE(cell_fills_extra_page(1, 0, {1, 0, kCols, kRows}, kCols, kRows));
    CHECK_FALSE(cell_fills_extra_page(1, 0, {0, 1, kCols, kRows}, kCols, kRows));
    CHECK_FALSE(cell_fills_extra_page(1, 0, {0, 0, 0, 0}, 0, 0));
}

namespace {

class FullPageFixture : public LVGLUITestFixture {
  public:
    FullPageFixture() {
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        helix::SettingsManager::instance().init_subjects();
        helix::init_widget_registrations();

        page_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(page_, 700, 440);
        drain();
    }

    ~FullPageFixture() override {
        get_global_motion_panel().release_embed();
        get_global_motion_panel().close();
        get_global_temp_graph_overlay().release_embed();
        drain();
        StaticPanelRegistry::instance().destroy_all();
        drain();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        process_lvgl(10);
    }

    static bool hidden(lv_obj_t* obj) {
        return lv_obj_is_hidden(obj);
    }

    static lv_obj_t* named(lv_obj_t* root, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    /// A Motion widget built into page_ the way the manager builds a tile, then
    /// told its placement.
    std::unique_ptr<helix::MotionWidget> make_tile(const char* instance_id, int page_index,
                                                   bool fills) {
        return place(std::make_unique<helix::MotionWidget>(instance_id), page_index, fills);
    }

    template <typename W>
    std::unique_ptr<W> place(std::unique_ptr<W> widget, int page_index, bool fills) {
        auto* root = static_cast<lv_obj_t*>(
            lv_xml_create(page_, widget->get_component_name().c_str(), widget->xml_attrs()));
        REQUIRE(root != nullptr);
        lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flag(root, helix::kPanelWidgetTileFlag, true);
        widget->attach_tile(root, lv_screen_active());
        widget->set_page_placement(page_index, fills);
        widget->notify_size_changed(24, 16, 700, 440);
        drain();
        return widget;
    }

    lv_obj_t* page_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(FullPageFixture, "embed builds the overlay's tree in a page, off the stack",
                 "[full_page][embed]") {
    MotionPanel& motion = get_global_motion_panel();
    REQUIRE(motion.embed(page_));
    drain();

    lv_obj_t* root = motion.get_root();
    REQUIRE(root != nullptr);
    CHECK(motion.is_embedded());
    CHECK(lv_obj_get_parent(root) == page_);
    CHECK_FALSE(hidden(root));
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    // A page has nothing to go back to; Motion keeps its header for the tabs.
    CHECK_FALSE(lv_obj_is_clickable(named(root, "back_button")));
    CHECK_FALSE(hidden(named(root, "overlay_header")));
    // A second embed while the tree exists builds nothing.
    CHECK_FALSE(motion.embed(page_));

    // The embedded controls drive the panel's own subjects, as the overlay's do.
    lv_subject_t* fine = lv_xml_get_subject(nullptr, "motion_jog_mode_fine_active");
    REQUIRE(fine != nullptr);
    REQUIRE(lv_subject_get_int(fine) == 0);
    lv_obj_send_event(named(root, "jog_mode_fine"), LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK(lv_subject_get_int(fine) == 1);

    // Opening the overlay takes the tree off the page and onto the stack.
    REQUIRE(motion.show(lv_screen_active()));
    drain();
    CHECK_FALSE(motion.is_embedded());
    REQUIRE(motion.get_root() != nullptr);
    CHECK(lv_obj_get_parent(motion.get_root()) == lv_screen_active());
    CHECK(NavigationManager::instance().has_open_overlays());
}

TEST_CASE_METHOD(FullPageFixture, "a Motion tile covering an extra page hosts the controls",
                 "[full_page][host]") {
    auto tile = make_tile("motion:1", 1, true);
    lv_obj_t* root = tile->root();
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "motion:1_full_page");
    REQUIRE(subject != nullptr);
    CHECK(lv_subject_get_int(subject) == 1);
    CHECK(hidden(named(root, "motion_compact")));
    CHECK_FALSE(hidden(named(root, "motion_full_page")));
    CHECK(helix::full_page_for(helix::kMotionFunction) == 1);

    // Shown: the overlay's tree is built inside the frame's body.
    lv_obj_t* body = named(root, "full_page_body");
    tile->on_activate();
    drain();
    MotionPanel& motion = get_global_motion_panel();
    REQUIRE(motion.is_embedded());
    CHECK(lv_obj_get_parent(motion.get_root()) == body);

    // Hidden: the tree is freed.
    tile->on_deactivate();
    drain();
    CHECK_FALSE(motion.is_embedded());
    CHECK(motion.get_root() == nullptr);

    // Shrunk to a tile on the same page: the compact face, and no jump target.
    tile->on_activate();
    tile->set_page_placement(1, false);
    tile->notify_size_changed(4, 4, 120, 120);
    drain();
    CHECK(lv_subject_get_int(subject) == 0);
    CHECK_FALSE(hidden(named(root, "motion_compact")));
    CHECK(hidden(named(root, "motion_full_page")));
    CHECK_FALSE(motion.is_embedded());
    CHECK_FALSE(helix::full_page_for(helix::kMotionFunction).has_value());
}

TEST_CASE_METHOD(FullPageFixture, "a tile tree deleted without detach releases its embed",
                 "[full_page][host]") {
    auto tile = make_tile("motion:1", 1, true);
    tile->on_activate();
    drain();
    MotionPanel& motion = get_global_motion_panel();
    REQUIRE(motion.is_embedded());
    REQUIRE(motion.is_visible());

    lv_obj_delete(tile->root());
    drain();
    // The overlay's deactivate ran: its observers no longer feed a freed tree.
    CHECK_FALSE(motion.is_visible());
    CHECK_FALSE(helix::full_page_for(helix::kMotionFunction).has_value());
    CHECK(motion.embed(page_));
}

TEST_CASE_METHOD(FullPageFixture, "a heater tile covering an extra page hosts its own heater",
                 "[full_page][host]") {
    const auto& bed = helix::bed_temp_config();
    auto tile = place(std::make_unique<helix::HeaterTempWidget>(get_printer_state(), nullptr, bed,
                                                                "bed_temperature:1"),
                      2, true);
    CHECK(hidden(named(tile->root(), "bed_temp_btn")));
    CHECK(helix::full_page_for(helix::heater_full_page_function(bed)) == 2);

    tile->on_activate();
    drain();
    TempGraphOverlay& graph = get_global_temp_graph_overlay();
    REQUIRE(graph.is_embedded());
    CHECK(lv_obj_get_parent(graph.get_root()) == named(tile->root(), "full_page_body"));
    // Its title-only header gives the page that height.
    CHECK(hidden(named(graph.get_root(), "overlay_header")));
    lv_subject_t* mode = lv_xml_get_subject(nullptr, "temp_graph_mode");
    REQUIRE(mode != nullptr);
    CHECK(lv_subject_get_int(mode) == static_cast<int>(TempGraphOverlay::Mode::Bed));

    tile->on_deactivate();
    drain();
    CHECK_FALSE(graph.is_embedded());
}

TEST_CASE_METHOD(FullPageFixture, "a compact Motion tile with no full page opens the overlay",
                 "[full_page][jump]") {
    auto tile = make_tile("motion", 0, false);
    REQUIRE_FALSE(helix::full_page_for(helix::kMotionFunction).has_value());
    lv_obj_send_event(named(tile->root(), "motion_button"), LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK_FALSE(get_global_motion_panel().is_embedded());
}

TEST_CASE_METHOD(FullPageFixture, "the swipe strip takes no press, so a drag reaches the pages",
                 "[full_page][strip]") {
    auto tile = make_tile("motion:1", 1, true);
    tile->on_activate();
    drain();
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t* strip = named(tile->root(), "full_page_swipe_strip");
    CHECK_FALSE(hidden(strip));
    CHECK(lv_obj_get_height(strip) > 0);

    lv_area_t area;
    lv_obj_get_coords(strip, &area);
    lv_point_t centre{(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &centre);
    // Nothing in the strip or the embedded controls answers a press there.
    for (lv_obj_t* o = hit; o; o = lv_obj_get_parent(o)) {
        CHECK(o != strip);
        CHECK(o != named(tile->root(), "full_page_body"));
    }
}

namespace {

/// A compact @p id tile on the main page and "<id>:1" spanning page 1 (clamped
/// to its whole grid), every other widget disabled; restores the prior layout.
class ScopedFullPageLayout {
  public:
    explicit ScopedFullPageLayout(const std::string& id = "motion") {
        auto* cfg = helix::Config::get_instance();
        key_ = cfg->df() + "panel_widgets/home";
        had_prior_ = cfg->exists(key_);
        if (had_prior_) {
            prior_ = cfg->get<nlohmann::json>(key_);
        }
        constexpr int kWhole = helix::GridLayout::kMaxTracks;
        nlohmann::json main = nlohmann::json::array({{{"id", id},
                                                      {"enabled", true},
                                                      {"col", 0},
                                                      {"row", 0},
                                                      {"colspan", 2},
                                                      {"rowspan", 2}}});
        for (const auto& def : helix::get_all_widget_defs()) {
            if (std::string(def.id) != id) {
                main.push_back({{"id", def.id}, {"enabled", false}});
            }
        }
        nlohmann::json page = nlohmann::json::array({{{"id", id + ":1"},
                                                      {"enabled", true},
                                                      {"col", 0},
                                                      {"row", 0},
                                                      {"colspan", kWhole},
                                                      {"rowspan", kWhole}}});
        cfg->set<nlohmann::json>(
            key_,
            {{"main_page_index", 0},
             {"next_page_id", 2},
             {"pages", {{{"id", "main"}, {"widgets", main}}, {{"id", "p1"}, {"widgets", page}}}}});
        reload();
    }

    ~ScopedFullPageLayout() {
        auto* cfg = helix::Config::get_instance();
        cfg->set<nlohmann::json>(key_, had_prior_ ? prior_ : nlohmann::json::object());
        reload();
    }

  private:
    static void reload() {
        auto& mgr = helix::PanelWidgetManager::instance();
        mgr.get_widget_config("home").mark_dirty();
        mgr.clear_panel_config("home");
    }

    std::string key_;
    bool had_prior_ = false;
    nlohmann::json prior_;
};

} // namespace

TEST_CASE_METHOD(FullPageFixture, "a compact tile jumps to its full page", "[full_page][jump]") {
    struct Case {
        const char* id;
        const char* button;
        const char* function;
    };
    const Case c =
        GENERATE(Case{"motion", "motion_button", helix::kMotionFunction},
                 Case{"filament_ops", "filament_ops_button", helix::kFilamentOpsFunction});
    CAPTURE(c.id);
    ScopedFullPageLayout layout(c.id);
    // The home panel's own carousel, built the way the app builds it.
    lv_obj_t* root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_t* host = lv_obj_create(root);
    lv_obj_set_name(host, "carousel_host");
    lv_obj_set_size(host, LV_PCT(100), LV_PCT(100));
    HomePanel& home = get_global_home_panel();
    if (!home.are_subjects_initialized()) {
        home.init_subjects();
    }
    home.setup(root, lv_screen_active());
    home.finalize_setup();
    drain();

    REQUIRE(helix::full_page_for(c.function) == 1);
    lv_obj_t* compact = lv_obj_find_by_name(root, c.button);
    REQUIRE(compact != nullptr);
    lv_obj_t* carousel = lv_obj_get_child(host, 0);
    REQUIRE(ui_carousel_get_current_page(carousel) == 0);

    lv_obj_send_event(compact, LV_EVENT_CLICKED, nullptr);
    process_lvgl(1000);
    drain();
    CHECK(ui_carousel_get_current_page(carousel) == 1);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
}

TEST_CASE_METHOD(FullPageFixture, "a full-page Motion instance survives a layout load and save",
                 "[full_page][persist]") {
    ScopedFullPageLayout layout;
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    config.load();
    REQUIRE(config.page_count() == 2);

    const auto find = [&](size_t page, const char* id) -> const helix::PanelWidgetEntry* {
        for (const auto& e : config.page_entries(page)) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    };
    // The second instance is a widget of its own, not an unknown id held aside.
    const auto* full = find(1, "motion:1");
    REQUIRE(full != nullptr);
    CHECK(full->is_placed());
    CHECK(full->colspan == helix::GridLayout::kMaxTracks);
    REQUIRE(find(0, "motion") != nullptr);

    config.save();
    const auto saved = helix::Config::get_instance()->get<nlohmann::json>(
        helix::Config::get_instance()->df() + "panel_widgets/home");
    const auto& widgets = saved.at("pages").at(1).at("widgets");
    REQUIRE(widgets.size() == 1);
    CHECK(widgets.at(0).at("id") == "motion:1");
    CHECK(widgets.at(0).at("rowspan") == helix::GridLayout::kMaxTracks);
}

namespace {

size_t observer_count(lv_subject_t* subject) {
    REQUIRE(subject != nullptr);
    size_t n = 0;
    for (void* o = lv_ll_get_head(&subject->subs_ll); o; o = lv_ll_get_next(&subject->subs_ll, o)) {
        ++n;
    }
    return n;
}

/// The global Filament panel built from its real XML into the fixture's panel
/// container and registered as the Filament main panel, the way PanelFactory
/// does it; unregistered again on the way out.
struct ScopedFilamentPanel {
    ScopedFilamentPanel() {
        helix::ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        FilamentPanel& panel = get_global_filament_panel();
        panel.init_subjects();
        root = static_cast<lv_obj_t*>(lv_xml_create(home, "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        auto& nav = NavigationManager::instance();
        // Only the Filament panel takes part; the panels the switches below
        // pass through have no instance, so they activate nothing.
        for (PanelId id : {PanelId::Home, PanelId::Settings}) {
            nav.register_panel_instance(id, nullptr);
        }
        nav.set_active(PanelId::Home);
        nav.replace_panel_widget(PanelId::Filament, root);
        nav.register_panel_instance(PanelId::Filament, &panel);
        panel.setup(root, lv_screen_active());
        lv_obj_set_hidden(root, true);
    }

    ~ScopedFilamentPanel() {
        auto& nav = NavigationManager::instance();
        nav.reclaim_panel(PanelId::Filament);
        nav.set_active(PanelId::Home);
        nav.register_panel_instance(PanelId::Filament, nullptr);
    }

    /// One observer per binding and per C++ watch: a second tree or a second
    /// setup() would add to both.
    static size_t observers() {
        return observer_count(lv_xml_get_subject(nullptr, "filament_material_selected")) +
               observer_count(AmsState::instance().get_ams_type_subject());
    }

    lv_obj_t* home = lv_screen_active();
    lv_obj_t* root = nullptr;
};

} // namespace

TEST_CASE_METHOD(FullPageFixture, "a Filament full page shows the Filament panel's own tree",
                 "[full_page][embed]") {
    ScopedFilamentPanel fp;
    drain();
    const size_t observers = ScopedFilamentPanel::observers();
    REQUIRE(observers > 0);
    auto& nav = NavigationManager::instance();

    auto tile =
        place(std::make_unique<helix::FilamentOpsWidget>("filament_ops:1"), 1, /*fills=*/true);
    CHECK(hidden(named(tile->root(), "filament_ops_compact")));
    CHECK(helix::full_page_for(helix::kFilamentOpsFunction) == 1);
    lv_obj_t* body = named(tile->root(), "full_page_body");

    // Shown: the one tree moves into the page; nothing is built or observed twice.
    tile->on_activate();
    drain();
    CHECK(lv_obj_get_parent(fp.root) == body);
    CHECK_FALSE(hidden(fp.root));
    CHECK(get_global_filament_panel().get_panel() == fp.root);
    CHECK(ScopedFilamentPanel::observers() == observers);
    CHECK(tile->full_page().hosts_tree());
    // The panel's tree does not paint over the frame's placeholder; it steps aside.
    lv_subject_t* state = lv_xml_get_subject(nullptr, "filament_ops:1_full_page");
    REQUIRE(state != nullptr);
    CHECK(lv_subject_get_int(state) == static_cast<int>(helix::FullPageState::Hosting));
    CHECK(hidden(named(tile->root(), "full_page_placeholder")));
    // The panel's XML tightens itself for the page by this subject.
    lv_subject_t* on_page = lv_xml_get_subject(nullptr, "filament_on_page");
    REQUIRE(on_page != nullptr);
    CHECK(lv_subject_get_int(on_page) == 1);

    // Switching between other panels leaves the lent tree on show.
    nav.set_active(PanelId::Settings);
    drain();
    CHECK(lv_obj_get_parent(fp.root) == body);
    CHECK_FALSE(hidden(fp.root));
    nav.set_active(PanelId::Home);

    // Edit mode disarms the page; handing the tree back re-arms what it took.
    helix::ui::disable_widget_clicks_recursive(tile->root());
    REQUIRE_FALSE(lv_obj_is_clickable(fp.root));
    tile->on_deactivate();
    drain();
    CHECK(lv_obj_get_parent(fp.root) == fp.home);
    CHECK(hidden(fp.root));
    CHECK(lv_obj_is_clickable(fp.root));
    CHECK(nav.panel_lent_to(PanelId::Filament) == nullptr);
    CHECK(lv_subject_get_int(state) == static_cast<int>(helix::FullPageState::Empty));
    CHECK_FALSE(hidden(named(tile->root(), "full_page_placeholder")));
    CHECK(lv_subject_get_int(on_page) == 0);
    CHECK(ScopedFilamentPanel::observers() == observers);

    // Opening the Filament panel itself while the page holds the tree takes it
    // back first, and while it is open the page does not get it.
    tile->on_activate();
    drain();
    REQUIRE(lv_obj_get_parent(fp.root) == body);
    nav.set_active(PanelId::Filament);
    drain();
    CHECK(lv_obj_get_parent(fp.root) == fp.home);
    CHECK_FALSE(hidden(fp.root));
    CHECK(lv_subject_get_int(on_page) == 0);
    tile->on_deactivate();
    tile->on_activate();
    drain();
    CHECK(lv_obj_get_parent(fp.root) == fp.home);
    CHECK_FALSE(tile->full_page().hosts_tree());
    CHECK(ScopedFilamentPanel::observers() == observers);
}

TEST_CASE_METHOD(FullPageFixture, "a Filament page deleted without detach keeps the panel's tree",
                 "[full_page][host]") {
    ScopedFilamentPanel fp;
    auto tile =
        place(std::make_unique<helix::FilamentOpsWidget>("filament_ops:1"), 1, /*fills=*/true);
    tile->on_activate();
    drain();
    REQUIRE(lv_obj_get_parent(fp.root) == named(tile->root(), "full_page_body"));

    lv_obj_delete(tile->root());
    drain();
    REQUIRE(lv_obj_is_in_widget_tree(fp.root));
    CHECK(lv_obj_get_parent(fp.root) == fp.home);
    CHECK(get_global_filament_panel().get_panel() == fp.root);
    CHECK(NavigationManager::instance().panel_lent_to(PanelId::Filament) == nullptr);
    CHECK_FALSE(helix::full_page_for(helix::kFilamentOpsFunction).has_value());
}
