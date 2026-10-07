// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_panel_widget_card_merge.cpp
 * @brief A merged card background must line up with the widgets it backs.
 *
 * populate_widgets() draws one card object behind each connected run of
 * widgets that opt into the shared background. The flood fill and the grid are
 * both addressed in TRACKS, so a widget parked on an odd track - reachable by
 * drag for any widget whose registry def supports half-cell resolution - is
 * backed like any other. The fill used to work in cells, which truncated such a
 * position, so those widgets were dropped from the merge and drew no background
 * at all.
 *
 * Two invariants. Containment: a card that overlaps a widget at all must cover
 * it completely — a card offset by half a cell leaves the widget's far edge
 * hanging outside its own background, which is the visible artifact. Coverage:
 * a widget that asked for the shared card must actually be behind one.
 */

#include "../test_fixtures.h"
#include "../test_helpers/scoped_widget_factory.h"
#include "config.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix_test::ScopedWidgetFactory;

namespace {

/// Minimal stand-in so the test does not depend on any real widget's XML.
struct StubWidget : helix::PanelWidget {
    std::string id_;
    explicit StubWidget(std::string id) : id_(std::move(id)) {}
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return id_.c_str();
    }
    std::string get_component_name() const override {
        return "test_card_merge_stub";
    }
};

/// A registry factory building a StubWidget for the id it is asked for.
WidgetFactory stub_factory() {
    return [](const std::string& wid) { return std::unique_ptr<PanelWidget>(new StubWidget(wid)); };
}

/// Every direct child of the container that is not one of the widget objects is
/// a card background — populate_widgets() names each widget object after its
/// widget id and leaves the cards unnamed.
std::vector<lv_obj_t*> card_backgrounds(lv_obj_t* container,
                                        const std::vector<std::string>& widget_ids) {
    std::vector<lv_obj_t*> widgets;
    for (const auto& id : widget_ids) {
        if (lv_obj_t* w = lv_obj_find_by_name(container, id.c_str())) {
            widgets.push_back(w);
        }
    }
    std::vector<lv_obj_t*> cards;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        lv_obj_t* child = lv_obj_get_child(container, i);
        bool is_widget = false;
        for (lv_obj_t* w : widgets) {
            if (w == child) {
                is_widget = true;
                break;
            }
        }
        if (!is_widget) {
            cards.push_back(child);
        }
    }
    return cards;
}

bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

/// `outer` covers `inner` entirely. Tolerance absorbs the grid allocator's
/// per-track remainder distribution (lv_grid.c), which can shift an edge by a
/// pixel; the misalignment this guards against is a whole half-cell.
bool area_contains(const lv_area_t& outer, const lv_area_t& inner, int tol = 2) {
    return outer.x1 <= inner.x1 + tol && outer.y1 <= inner.y1 + tol && outer.x2 + tol >= inner.x2 &&
           outer.y2 + tol >= inner.y2;
}

/// Seed a page holding exactly `widgets`, populate it, and assert the card
/// invariant. Returns the number of card backgrounds produced.
size_t check_card_containment(const std::string& panel_id, const nlohmann::json& widgets,
                              const std::vector<std::string>& ids, lv_obj_t* screen) {
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "spy"}, {"widgets", widgets}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, 800, 480);
    lv_obj_update_layout(container);

    auto held = mgr.populate_widgets(panel_id, container, /*page_index=*/1);
    lv_obj_update_layout(container);

    auto cards = card_backgrounds(container, ids);
    std::vector<bool> backed(ids.size(), false);
    for (lv_obj_t* card : cards) {
        lv_area_t card_area;
        lv_obj_get_coords(card, &card_area);
        for (size_t i = 0; i < ids.size(); i++) {
            lv_obj_t* w = lv_obj_find_by_name(container, ids[i].c_str());
            if (!w) {
                continue;
            }
            lv_area_t w_area;
            lv_obj_get_coords(w, &w_area);
            if (!areas_overlap(card_area, w_area)) {
                continue;
            }
            backed[i] = true;
            INFO("widget " << ids[i] << " at [" << w_area.x1 << "," << w_area.y1 << " " << w_area.x2
                           << "," << w_area.y2 << "] vs card [" << card_area.x1 << ","
                           << card_area.y1 << " " << card_area.x2 << "," << card_area.y2 << "]");
            CHECK(area_contains(card_area, w_area));
        }
    }

    // Coverage. Without this the containment loop passes vacuously for any
    // widget the merge pass declined to back - which is exactly how the
    // cell-coordinate version looked correct while drawing nothing.
    for (size_t i = 0; i < ids.size(); i++) {
        const auto* def = helix::find_widget_def(ids[i]);
        if (!def || !def->merges_into_card || !lv_obj_find_by_name(container, ids[i].c_str())) {
            continue;
        }
        INFO("widget " << ids[i] << " asked for the shared card");
        CHECK(backed[i]);
    }

    mgr.clear_panel_config(panel_id);
    return cards.size();
}

const int TPC = GridLayout::TRACKS_PER_CELL;

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Card merge: adjacent aligned widgets share one covering card",
                 "[manager][card_merge]") {
    helix::init_widget_registrations();
    lv_xml_register_component_from_data(
        "test_card_merge_stub",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    REQUIRE(theme_manager_get_spacing("space_xs") > 0);

    ScopedWidgetFactory a("shutdown", stub_factory());
    ScopedWidgetFactory b("lock", stub_factory());

    // Two whole-cell widgets side by side, both on cell boundaries.
    nlohmann::json widgets = {{{"id", "shutdown"},
                               {"enabled", true},
                               {"col", 0},
                               {"row", 0},
                               {"colspan", TPC},
                               {"rowspan", TPC}},
                              {{"id", "lock"},
                               {"enabled", true},
                               {"col", TPC},
                               {"row", 0},
                               {"colspan", TPC},
                               {"rowspan", TPC}}};

    size_t cards = check_card_containment("test_card_merge_aligned", widgets, {"shutdown", "lock"},
                                          test_screen());
    // They are adjacent and both single-cell, so the flood fill merges them.
    CHECK(cards == 1);
}

// A half-cell-capable widget snaps on a single track (snap_step_for), so it can
// legally sit on an odd track. Converting that position to cell coordinates
// truncated it, so the pass excluded the widget outright and it rendered on the
// bare panel background. In tracks it gets a card that lines up with it.
TEST_CASE_METHOD(XMLTestFixture, "Card merge: a widget on an odd track still gets its card",
                 "[manager][card_merge]") {
    helix::init_widget_registrations();
    lv_xml_register_component_from_data(
        "test_card_merge_stub",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    REQUIRE(theme_manager_get_spacing("space_xs") > 0);

    // The premise: this widget really is half-cell capable, so an odd track is
    // reachable by drag. If that ever changes the test is no longer meaningful.
    const auto* def = helix::find_widget_def("shutdown");
    REQUIRE(def != nullptr);
    REQUIRE(def->supports_half_col);
    REQUIRE(TPC > 1);

    ScopedWidgetFactory a("shutdown", stub_factory());
    ScopedWidgetFactory b("lock", stub_factory());

    // shutdown sits one track right of the cell boundary; lock is aligned and
    // close enough that a truncated card would land on top of it.
    nlohmann::json widgets = {{{"id", "shutdown"},
                               {"enabled", true},
                               {"col", TPC + 1},
                               {"row", 0},
                               {"colspan", TPC},
                               {"rowspan", TPC}},
                              {{"id", "lock"},
                               {"enabled", true},
                               {"col", 0},
                               {"row", 0},
                               {"colspan", TPC},
                               {"rowspan", TPC}}};

    // One empty track separates them, so they stay two components rather than
    // fusing across the gap - two cards, each covering its own widget.
    size_t cards =
        check_card_containment("test_card_merge_odd", widgets, {"shutdown", "lock"}, test_screen());
    CHECK(cards == 2);
}

// A widget sized to an odd number of tracks - 1.5 cells - is the other half of
// the same bug: the span truncated as well as the position.
TEST_CASE_METHOD(XMLTestFixture, "Card merge: a widget 1.5 cells wide gets a card that fits it",
                 "[manager][card_merge]") {
    helix::init_widget_registrations();
    lv_xml_register_component_from_data(
        "test_card_merge_stub",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    REQUIRE(theme_manager_get_spacing("space_xs") > 0);

    // The premise: clock resizes on odd track counts and wants the shared card.
    const auto* def = helix::find_widget_def("clock");
    REQUIRE(def != nullptr);
    REQUIRE(def->supports_half_col);
    REQUIRE(def->merges_into_card);
    REQUIRE(def->effective_max_colspan() >= 3);

    ScopedWidgetFactory a("clock", stub_factory());

    nlohmann::json widgets = {{{"id", "clock"},
                               {"enabled", true},
                               {"col", 0},
                               {"row", 0},
                               {"colspan", 3}, // 1.5 cells
                               {"rowspan", TPC}}};

    size_t cards =
        check_card_containment("test_card_merge_odd_span", widgets, {"clock"}, test_screen());
    CHECK(cards == 1);
}

// A merging widget two cells tall beside a one-cell readout row, with a
// widget that paints its own card below that row: the home layout of a
// console tile at 2x2. The carve-out below the readout row stops the first
// piece at the row boundary, and that boundary runs through the tall widget,
// so the piece must not end there with the widget half inside it.
TEST_CASE_METHOD(XMLTestFixture, "Card merge: a tall widget beside a short row is never split",
                 "[manager][card_merge]") {
    helix::init_widget_registrations();
    lv_xml_register_component_from_data(
        "test_card_merge_stub",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    REQUIRE(theme_manager_get_spacing("space_xs") > 0);

    const std::vector<std::string> merging = {"temperature",   "bed_temperature", "led",
                                              "notifications", "fan_stack",       "ams",
                                              "gcode_console"};
    for (const auto& id : merging) {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        INFO(id);
        REQUIRE(def->merges_into_card);
    }
    const auto* own = helix::find_widget_def("print_status");
    REQUIRE(own != nullptr);
    REQUIRE_FALSE(own->merges_into_card);

    std::vector<std::unique_ptr<ScopedWidgetFactory>> factories;
    for (const auto& id : merging) {
        factories.push_back(std::make_unique<ScopedWidgetFactory>(id.c_str(), stub_factory()));
    }
    factories.push_back(std::make_unique<ScopedWidgetFactory>("print_status", stub_factory()));

    auto at = [](const char* id, int col, int row, int colspan, int rowspan) {
        return nlohmann::json{{"id", id},   {"enabled", true},    {"col", col},
                              {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
    };
    const int C = TPC;
    nlohmann::json widgets = {
        at("temperature", 2 * C, 0, C, C),
        at("bed_temperature", 3 * C, 0, C, C),
        at("led", 4 * C, 0, C, C),
        at("notifications", 5 * C, 0, C, C),
        at("fan_stack", 2 * C, C, C, C),
        at("ams", 3 * C, C, C, C),
        at("gcode_console", 4 * C, C, 2 * C, 2 * C),
        at("print_status", 0, 2 * C, 4 * C, 2 * C),
    };

    std::vector<std::string> ids = merging;
    ids.push_back("print_status");
    size_t cards = check_card_containment("test_card_merge_tall", widgets, ids, test_screen());
    CHECK(cards >= 2);
}

// Edit mode re-seats a page in place after a move instead of populating it
// again. The tiles keep their objects, and a card the new arrangement still
// has keeps its object too: a replaced card repaints everything behind it.
TEST_CASE_METHOD(XMLTestFixture, "Card merge: an in-place relayout replaces only changed cards",
                 "[manager][card_merge]") {
    helix::init_widget_registrations();
    lv_xml_register_component_from_data(
        "test_card_merge_stub",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    REQUIRE(theme_manager_get_spacing("space_xs") > 0);

    ScopedWidgetFactory a("shutdown", stub_factory());
    ScopedWidgetFactory b("lock", stub_factory());

    // Two whole-cell widgets one cell apart: two components, two cards.
    const std::string panel_id = "test_card_merge_relayout";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "spy"},
                          {"widgets",
                           {{{"id", "shutdown"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", TPC},
                             {"rowspan", TPC}},
                            {{"id", "lock"},
                             {"enabled", true},
                             {"col", 2 * TPC},
                             {"row", 0},
                             {"colspan", TPC},
                             {"rowspan", TPC}}}}}}}});
    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    lv_obj_update_layout(container);
    auto held = mgr.populate_widgets(panel_id, container, /*page_index=*/1);
    lv_obj_update_layout(container);

    const std::vector<std::string> ids = {"shutdown", "lock"};
    lv_obj_t* shutdown = lv_obj_find_by_name(container, "shutdown");
    lv_obj_t* lock = lv_obj_find_by_name(container, "lock");
    REQUIRE(shutdown != nullptr);
    REQUIRE(lock != nullptr);
    auto card_behind = [&](lv_obj_t* widget) -> lv_obj_t* {
        lv_area_t w_area;
        lv_obj_get_coords(widget, &w_area);
        for (lv_obj_t* card : card_backgrounds(container, ids)) {
            lv_area_t c_area;
            lv_obj_get_coords(card, &c_area);
            if (area_contains(c_area, w_area)) {
                return card;
            }
        }
        return nullptr;
    };
    REQUIRE(card_backgrounds(container, ids).size() == 2);
    lv_obj_t* shutdown_card = card_behind(shutdown);
    REQUIRE(shutdown_card != nullptr);

    // lock moves one cell down: shutdown's card is unchanged, lock's moves.
    REQUIRE(mgr.get_widget_config(panel_id).place_entry("lock", 1, 2 * TPC, TPC, TPC, TPC) >= 0);
    REQUIRE(mgr.relayout_tiles(panel_id, container, 1, {"lock"}, "", held));
    lv_obj_update_layout(container);

    CHECK(lv_obj_find_by_name(container, "shutdown") == shutdown);
    CHECK(lv_obj_find_by_name(container, "lock") == lock);
    CHECK(lv_obj_get_style_grid_cell_row_pos(lock, LV_PART_MAIN) == TPC);
    CHECK(card_backgrounds(container, ids).size() == 2);
    CHECK(card_behind(shutdown) == shutdown_card);
    CHECK(card_behind(lock) != nullptr);

    // lock beside shutdown: the two fuse into one card behind both.
    REQUIRE(mgr.get_widget_config(panel_id).place_entry("lock", 1, TPC, 0, TPC, TPC) >= 0);
    REQUIRE(mgr.relayout_tiles(panel_id, container, 1, {"lock"}, "", held));
    lv_obj_update_layout(container);
    REQUIRE(card_backgrounds(container, ids).size() == 1);
    CHECK(card_behind(shutdown) == card_behind(lock));

    // A card queued for deletion is hidden, not gone: it backs nothing, so a
    // relayout that still wants its rectangle builds a live one in its place.
    lv_obj_t* condemned = card_behind(shutdown);
    REQUIRE(condemned != nullptr);
    lv_obj_add_flag(condemned, LV_OBJ_FLAG_HIDDEN);
    REQUIRE(mgr.relayout_tiles(panel_id, container, 1, {"lock"}, "", held));
    lv_obj_update_layout(container);
    bool live_card_behind_shutdown = false;
    {
        lv_area_t w_area;
        lv_obj_get_coords(shutdown, &w_area);
        for (lv_obj_t* card : card_backgrounds(container, ids)) {
            lv_area_t c_area;
            lv_obj_get_coords(card, &c_area);
            if (!lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN) && area_contains(c_area, w_area)) {
                live_card_behind_shutdown = true;
            }
        }
    }
    CHECK(live_card_behind_shutdown);

    // A tile the edit did not touch, seated off its entry (placement moved it
    // and did not write that back), refuses the in-place path: re-seating it at
    // its entry could land it on another tile.
    lv_obj_set_grid_cell(shutdown, LV_GRID_ALIGN_STRETCH, 0, TPC, LV_GRID_ALIGN_STRETCH, TPC, TPC);
    CHECK_FALSE(mgr.relayout_tiles(panel_id, container, 1, {"lock"}, "", held));
    CHECK(lv_obj_get_style_grid_cell_row_pos(shutdown, LV_PART_MAIN) == TPC);

    mgr.clear_panel_config(panel_id);
    held.clear();
    lv_obj_delete(container);
}

namespace {

/// StubWidget that records the spans it is told, to see a re-seat announce a resize.
struct SizedStub : StubWidget {
    explicit SizedStub(std::string id) : StubWidget(std::move(id)) {}
    void on_size_changed(int colspan, int rowspan, int, int) override {
        last_colspan = colspan;
        last_rowspan = rowspan;
        ++size_calls;
    }
    int last_colspan = -1;
    int last_rowspan = -1;
    int size_calls = 0;
};

} // namespace

// A printer switch between layouts that hold the same widgets differently: every tile
// moves at once, which an edit-mode relayout refuses and a re-seat does in place.
TEST_CASE_METHOD(XMLTestFixture, "Card merge: re-seating every tile keeps their objects",
                 "[manager][card_merge][reseat]") {
    helix::init_widget_registrations();
    lv_xml_register_component_from_data(
        "test_card_merge_stub",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    REQUIRE(theme_manager_get_spacing("space_xs") > 0);

    ScopedWidgetFactory a("shutdown", stub_factory());
    ScopedWidgetFactory b("lock", [](const std::string& wid) {
        return std::unique_ptr<PanelWidget>(new SizedStub(wid));
    });

    const std::string panel_id = "test_card_merge_reseat";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "spy"},
                          {"widgets",
                           {{{"id", "shutdown"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", TPC},
                             {"rowspan", TPC}},
                            {{"id", "lock"},
                             {"enabled", true},
                             {"col", 2 * TPC},
                             {"row", 0},
                             {"colspan", TPC},
                             {"rowspan", TPC}}}}}}}});
    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    lv_obj_update_layout(container);
    auto held = mgr.populate_widgets(panel_id, container, /*page_index=*/1);
    lv_obj_update_layout(container);
    lv_obj_t* shutdown = lv_obj_find_by_name(container, "shutdown");
    lv_obj_t* lock = lv_obj_find_by_name(container, "lock");
    REQUIRE(shutdown != nullptr);
    REQUIRE(lock != nullptr);
    SizedStub* lock_widget = nullptr;
    for (auto& w : held) {
        if (w && std::string(w->id()) == "lock") {
            lock_widget = static_cast<SizedStub*>(w.get());
        }
    }
    REQUIRE(lock_widget != nullptr);
    const int size_calls_before = lock_widget->size_calls;

    // Both move, and lock grows to two cells wide.
    auto& config = mgr.get_widget_config(panel_id);
    REQUIRE(config.place_entry("shutdown", 1, 0, TPC, TPC, TPC) >= 0);
    REQUIRE(config.place_entry("lock", 1, TPC, 0, 2 * TPC, TPC) >= 0);
    CHECK_FALSE(mgr.relayout_tiles(panel_id, container, 1, {"lock"}, "", held));

    REQUIRE(mgr.reseat_tiles(panel_id, container, 1, held));
    lv_obj_update_layout(container);
    CHECK(lv_obj_find_by_name(container, "shutdown") == shutdown);
    CHECK(lv_obj_find_by_name(container, "lock") == lock);
    CHECK(lv_obj_get_style_grid_cell_row_pos(shutdown, LV_PART_MAIN) == TPC);
    CHECK(lv_obj_get_style_grid_cell_column_pos(lock, LV_PART_MAIN) == TPC);
    CHECK(lv_obj_get_style_grid_cell_column_span(lock, LV_PART_MAIN) == 2 * TPC);
    CHECK(lock_widget->size_calls > size_calls_before);
    CHECK(lock_widget->last_colspan == 2 * TPC);

    mgr.clear_panel_config(panel_id);
}
