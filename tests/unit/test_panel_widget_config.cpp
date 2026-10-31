// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "config.h"
#include "data_root_resolver.h"
#include "home_layout_share.h"
#include "layout_port.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <utility>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

/// Count of widget defs included in the default grid.
/// build_default_grid() includes ALL defs (multi-instance widgets appear once as base ID).
static size_t default_grid_widget_count() {
    return get_all_widget_defs().size();
}

/// The enabled state build_default_grid() actually produces for `id`.
///
/// Deliberately not the registry's default_enabled. Two things legitimately
/// override it: bed_temperature is flipped on when no AMS is present, and the
/// shipped layout's per-breakpoint "disabled" map switches a widget off on a
/// tier with no room for it. A test that means "config loading did not disturb
/// this widget" therefore has to compare against the defaults builder, not the
/// registry table — comparing against the registry only ever worked because the
/// shipped layout did not yet disable anything.
static bool default_enabled_for(const std::string& id) {
    for (const auto& e : PanelWidgetConfig::build_default_grid()) {
        if (e.id == id) {
            return e.enabled;
        }
    }
    return false;
}

/// What a save-and-reload must preserve: every widget's enabled state, and the
/// order of the enabled ones. Disabled unplaced widgets may come back in
/// registry order, since nothing reads their position.
static void require_same_widget_state(const std::vector<PanelWidgetEntry>& a,
                                      const std::vector<PanelWidgetEntry>& b) {
    REQUIRE(a.size() == b.size());
    std::map<std::string, bool> state_a, state_b;
    std::vector<std::string> enabled_a, enabled_b;
    for (const auto& e : a) {
        state_a[e.id] = e.enabled;
        if (e.enabled)
            enabled_a.push_back(e.id);
    }
    for (const auto& e : b) {
        state_b[e.id] = e.enabled;
        if (e.enabled)
            enabled_b.push_back(e.id);
    }
    REQUIRE(state_a == state_b);
    REQUIRE(enabled_a == enabled_b);
}

// ============================================================================
// Test fixture — access Config internals via ConfigTestAccess
// ============================================================================

namespace helix {
class PanelWidgetConfigFixture {
  protected:
    Config config;

    void setup_empty_config() {
        ConfigTestAccess::data(config) = json::object();
    }

    /// Set up per-panel config under /printers/default/panel_widgets/<panel_id>
    /// Accepts either a flat JSON array (legacy format) or a pages object (new format)
    void setup_with_widgets(const json& widgets_json, const std::string& panel_id = "home") {
        ConfigTestAccess::data(config) = json::object();
        ConfigTestAccess::data(config)["printers"]["default"]["panel_widgets"][panel_id] =
            widgets_json;
    }

    /// Set up multi-page config in new format
    void setup_with_pages(const std::vector<std::pair<std::string, json>>& pages,
                          size_t main_page_index = 0, int next_page_id = -1,
                          const std::string& panel_id = "home") {
        ConfigTestAccess::data(config) = json::object();
        json root;
        json pages_arr = json::array();
        for (const auto& [id, widgets] : pages) {
            json page;
            page["id"] = id;
            page["widgets"] = widgets;
            pages_arr.push_back(std::move(page));
        }
        root["pages"] = std::move(pages_arr);
        root["main_page_index"] = main_page_index;
        root["next_page_id"] = next_page_id >= 0 ? next_page_id : static_cast<int>(pages.size());
        ConfigTestAccess::data(config)["printers"]["default"]["panel_widgets"][panel_id] = root;
    }

    /// Set up legacy flat home_widgets key (for migration testing)
    void setup_with_legacy_widgets(const json& widgets_json) {
        ConfigTestAccess::data(config) = json::object();
        ConfigTestAccess::data(config)["home_widgets"] = widgets_json;
    }

    json& get_data() {
        return ConfigTestAccess::data(config);
    }

    /// Get the per-printer data where PanelWidgetConfig reads/writes
    json& get_printer_data() {
        return ConfigTestAccess::data(config)["printers"]["default"];
    }

    /// Get the saved data after save(), as a JSON object with pages format
    json get_saved_root(const std::string& panel_id = "home") {
        return get_printer_data()["panel_widgets"][panel_id];
    }

    /// Get the saved widgets array for page 0 after save()
    json get_saved_page0_widgets(const std::string& panel_id = "home") {
        auto root = get_saved_root(panel_id);
        if (root.is_object() && root.contains("pages") && root["pages"].is_array() &&
            !root["pages"].empty()) {
            return root["pages"][0].value("widgets", json::array());
        }
        return json::array();
    }
};
} // namespace helix

namespace {

/// Find an entry by id within a vector. Returns nullptr if missing.
const PanelWidgetEntry* find_entry(const std::vector<PanelWidgetEntry>& v, const char* id) {
    auto it =
        std::find_if(v.begin(), v.end(), [id](const PanelWidgetEntry& e) { return e.id == id; });
    return it == v.end() ? nullptr : &*it;
}

} // namespace

// ============================================================================
// Registry tests
// ============================================================================

TEST_CASE("PanelWidgetRegistry: every def is complete, unique and findable by id",
          "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    // At least the core widgets must exist; exact count grows as widgets are added
    REQUIRE(defs.size() >= 14);
    REQUIRE(defs.size() == widget_def_count());

    std::set<std::string> ids;
    for (const auto& def : defs) {
        CAPTURE(def.id);
        REQUIRE(def.id != nullptr);
        REQUIRE(def.display_name != nullptr);
        REQUIRE(def.icon != nullptr);
        REQUIRE(def.description != nullptr);
        REQUIRE(std::string_view(def.id).size() > 0);
        REQUIRE(std::string_view(def.display_name).size() > 0);
        REQUIRE(std::string_view(def.icon).size() > 0);
        REQUIRE(std::string_view(def.description).size() > 0);
        REQUIRE(ids.insert(def.id).second);
        const auto* found = find_widget_def(def.id);
        REQUIRE(found != nullptr);
        REQUIRE(found->id == std::string_view(def.id));
        // A gate subject and its hint come together or not at all.
        REQUIRE((def.hardware_gate_subject != nullptr) == (def.hardware_gate_hint != nullptr));
        REQUIRE(find_widget_category(def.category) != nullptr);
    }

    const auto* temperature = find_widget_def("temperature");
    REQUIRE(temperature != nullptr);
    REQUIRE(std::string(temperature->display_name) == "Nozzle Temperature");
    REQUIRE(find_widget_def("nonexistent_widget") == nullptr);
}

TEST_CASE("PanelWidgetRegistry: hardware widgets are gated and always-available ones are not",
          "[panel_widget][widget_config]") {
    struct Row {
        const char* id;
        bool gated;
    };
    const Row rows[] = {
        {"power_device", true},
        {"ams", true},
        {"led", true},
        {"humidity", true},
        {"width_sensor", true},
        {"filament", true},
        {"thermistor", true},
        {"network", false},
        {"firmware_restart", false},
        {"temperature", false},
        {"notifications", false},
    };
    for (const auto& row : rows) {
        CAPTURE(row.id);
        const auto* def = find_widget_def(row.id);
        REQUIRE(def != nullptr);
        REQUIRE((def->hardware_gate_subject != nullptr) == row.gated);
    }
}

// ============================================================================
// Registry tests — category taxonomy
// ============================================================================

TEST_CASE("PanelWidgetRegistry: the category table is complete and partitions the defs",
          "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    std::set<int> ids;
    std::set<std::string> names;
    size_t total = 0;
    for (const auto& cat : get_widget_categories()) {
        REQUIRE(cat.display_name != nullptr);
        REQUIRE(cat.translation_tag != nullptr);
        REQUIRE(cat.icon != nullptr);
        CAPTURE(cat.display_name);
        REQUIRE(std::string_view(cat.display_name).size() > 0);
        REQUIRE(std::string_view(cat.translation_tag).size() > 0);
        REQUIRE(std::string_view(cat.icon).size() > 0);
        REQUIRE(ids.insert(static_cast<int>(cat.id)).second);
        REQUIRE(names.insert(cat.display_name).second);

        const auto* found = find_widget_category(cat.id);
        REQUIRE(found != nullptr);
        REQUIRE(found->id == cat.id);
        REQUIRE(std::string_view(found->display_name) == cat.display_name);

        const auto count = static_cast<size_t>(
            std::count_if(defs.begin(), defs.end(),
                          [&cat](const PanelWidgetDef& def) { return def.category == cat.id; }));
        total += count;
        // Plugins holds only runtime definitions, so a printer with no plugin
        // widgets has an empty category; the catalog hides the row.
        if (cat.id != WidgetCategory::Plugins) {
            REQUIRE(count > 0);
        }
    }
    REQUIRE(total == widget_def_count());
    // An out-of-range value resolves to nothing rather than the first entry.
    REQUIRE(find_widget_category(static_cast<WidgetCategory>(9999)) == nullptr);
}

TEST_CASE("PanelWidgetRegistry: known widgets keep their category",
          "[panel_widget][widget_config]") {
    struct Expected {
        const char* id;
        WidgetCategory category;
    };
    // Spot pins — a careless re-shuffle of the table should trip at least one.
    const Expected pinned[] = {
        {"print_status", WidgetCategory::PrintStatus},
        {"control_buttons", WidgetCategory::PrintStatus},
        {"temperature", WidgetCategory::Temperature},
        {"preheat", WidgetCategory::Temperature},
        // Fans fold into Temperature & Cooling rather than carrying a
        // two-widget category of their own.
        {"fan_stack", WidgetCategory::Temperature},
        {"fan", WidgetCategory::Temperature},
        {"ams", WidgetCategory::Filament},
        // Humidity reads as a filament-drying concern, not a climate one.
        {"humidity", WidgetCategory::Filament},
        // LEDs are something you actuate, so they live with the other controls.
        {"led", WidgetCategory::Controls},
        {"macros", WidgetCategory::Controls},
        {"clock", WidgetCategory::System},
        {"shutdown", WidgetCategory::System},
    };
    for (const auto& e : pinned) {
        CAPTURE(e.id);
        const auto* def = find_widget_def(e.id);
        REQUIRE(def != nullptr);
        REQUIRE(def->category == e.category);
    }
}

// ============================================================================
// Config tests — the default set, and the inputs that fall back to it
// ============================================================================

/// Every registry widget is present once, with the enabled state the defaults
/// builder gives it (bed_temperature's is breakpoint- and AMS-dependent).
static void require_default_set(const std::vector<PanelWidgetEntry>& entries) {
    REQUIRE(entries.size() == default_grid_widget_count());
    for (const auto& def : get_all_widget_defs()) {
        CAPTURE(def.id);
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [&def](const PanelWidgetEntry& e) { return e.id == def.id; });
        REQUIRE(it != entries.end());
        if (def.id != std::string("bed_temperature")) {
            REQUIRE(it->enabled == default_enabled_for(def.id));
        }
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: the default set comes from no config, a reset, or an unusable "
                 "saved value",
                 "[panel_widget][widget_config]") {
    std::string panel = "home";
    bool customize_then_reset = false;
    SECTION("no config") {
        setup_empty_config();
    }
    SECTION("no config for a non-home panel") {
        setup_empty_config();
        panel = "controls";
    }
    SECTION("a customized layout reset to defaults") {
        setup_empty_config();
        customize_then_reset = true;
    }
    SECTION("panel_widgets value is not an array") {
        get_printer_data()["panel_widgets"]["home"] = "corrupted";
    }
    SECTION("an empty array") {
        setup_with_widgets(json::array());
    }

    PanelWidgetConfig wc(panel, config);
    wc.load();
    if (customize_then_reset) {
        wc.set_enabled(0, false);
        wc.reorder(0, 5);
        wc.reset_to_defaults();
    }
    require_default_set(wc.entries());
}

// ============================================================================
// Config tests — load from explicit JSON
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a saved list loads in order, drops what it cannot use, and "
                 "appends the rest of the registry",
                 "[panel_widget][widget_config]") {
    struct Lead {
        const char* id;
        bool enabled;
    };
    struct Row {
        const char* what;
        json widgets;
        std::vector<Lead> lead;
        bool appended_at_registry_default;
    };
    // Grid coords on usable entries keep the pre-grid reset away.
    const Row rows[] = {
        {"explicit order and enabled state",
         json::array({{{"id", "temperature"}, {"enabled", true}, {"col", 0}, {"row", 0}},
                      {{"id", "led"}, {"enabled", false}, {"col", 1}, {"row", 0}},
                      {{"id", "network"}, {"enabled", true}, {"col", 2}, {"row", 0}}}),
         {{"temperature", true}, {"led", false}, {"network", true}},
         true},
        {"a subset: the other registry widgets are appended",
         json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
                      {{"id", "network"}, {"enabled", false}, {"col", 1}, {"row", 0}}}),
         {{"shutdown", true}, {"network", false}},
         true},
        {"an unknown id is dropped",
         json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
                      {{"id", "bogus_widget"}, {"enabled", true}, {"col", 1}, {"row", 0}},
                      {{"id", "network"}, {"enabled", false}, {"col", 2}, {"row", 0}}}),
         {{"shutdown", true}, {"network", false}},
         false},
        {"a duplicate id keeps its first occurrence",
         json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
                      {{"id", "network"}, {"enabled", true}, {"col", 1}, {"row", 0}},
                      {{"id", "shutdown"}, {"enabled", false}, {"col", 2}, {"row", 0}},
                      {{"id", "temperature"}, {"enabled", true}, {"col", 3}, {"row", 0}}}),
         {{"shutdown", true}, {"network", true}, {"temperature", true}},
         false},
        {"a malformed id or enabled skips that entry",
         json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
                      {{"id", 42}, {"enabled", true}},
                      {{"id", "network"}, {"enabled", "yes"}},
                      {{"id", "temperature"}, {"enabled", false}, {"col", 1}, {"row", 0}}}),
         {{"shutdown", true}, {"temperature", false}},
         false},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.what) {
            setup_with_widgets(row.widgets);
            PanelWidgetConfig wc("home", config);
            wc.load();

            // One entry per registry widget: a duplicate or unknown id adds none.
            const auto& entries = wc.entries();
            REQUIRE(entries.size() == default_grid_widget_count());
            for (size_t i = 0; i < row.lead.size(); ++i) {
                CAPTURE(i);
                REQUIRE(entries[i].id == row.lead[i].id);
                REQUIRE(entries[i].enabled == row.lead[i].enabled);
            }
            if (row.appended_at_registry_default) {
                for (size_t i = row.lead.size(); i < entries.size(); ++i) {
                    CAPTURE(entries[i].id);
                    const auto* def = find_widget_def(entries[i].id);
                    REQUIRE(def != nullptr);
                    REQUIRE(entries[i].enabled == def->default_enabled);
                }
            }
        }
    }
}

// ============================================================================
// Config tests — save produces expected JSON
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: save produces expected JSON structure",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    // Disable one widget for variety
    wc.set_enabled(2, false);
    wc.save();

    // Check the JSON was written to config under per-panel path in pages format
    auto root = get_saved_root();
    REQUIRE(root.is_object());
    REQUIRE(root.contains("pages"));
    REQUIRE(root["pages"].is_array());
    REQUIRE(root["pages"].size() == 1);

    auto& saved = root["pages"][0]["widgets"];
    REQUIRE(saved.is_array());
    // A widget that is off by default and unplaced is left out: the reader
    // appends it from the registry, so writing it costs bytes and says nothing.
    REQUIRE(saved.size() < default_grid_widget_count());
    for (const auto& item : saved) {
        const auto* def = find_widget_def(item["id"].get<std::string>());
        REQUIRE(def != nullptr);
        CAPTURE(def->id);
        REQUIRE_FALSE(
            (!def->default_enabled && !item["enabled"].get<bool>() && item["col"].get<int>() < 0));
    }

    // Each entry should have id and enabled
    for (const auto& item : saved) {
        REQUIRE(item.contains("id"));
        REQUIRE(item.contains("enabled"));
        REQUIRE(item["id"].is_string());
        REQUIRE(item["enabled"].is_boolean());
    }

    // The third entry should be disabled
    const std::string third = wc.entries()[2].id;
    auto it = std::find_if(saved.begin(), saved.end(),
                           [&](const json& item) { return item["id"] == third; });
    REQUIRE(it != saved.end());
    REQUIRE((*it)["enabled"].get<bool>() == false);
}

TEST_CASE_METHOD(
    PanelWidgetConfigFixture,
    "PanelWidgetConfig: crash-loop safe mode shows defaults and keeps the saved layout",
    "[panel_widget][widget_config][crash_loop]") {
    json widgets = json::array({
        {{"id", "temperature"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "led"}, {"enabled", false}, {"col", 1}, {"row", 0}},
        {{"id", "network"}, {"enabled", true}, {"col", 2}, {"row", 0}},
    });
    setup_with_widgets(widgets);
    const json saved_before = get_saved_root();

    {
        ScopedRuntimeConfig scoped_config;
        get_runtime_config()->crash_loop_safe_mode = true;

        PanelWidgetConfig wc("home", config);
        wc.load();
        const auto defaults = PanelWidgetConfig::build_default_grid();
        REQUIRE(wc.entries().size() == defaults.size());
        for (size_t i = 0; i < defaults.size(); ++i) {
            CHECK(wc.entries()[i].id == defaults[i].id);
            CHECK(wc.entries()[i].enabled == defaults[i].enabled);
        }

        // Edits made in safe mode are not persisted over the user's layout.
        wc.set_enabled(0, !wc.entries()[0].enabled);
        wc.save();
        CHECK(get_saved_root() == saved_before);
    }

    // Normal mode reads the saved layout again.
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE_FALSE(wc.entries().empty());
    CHECK(wc.entries()[0].id == "temperature");
    CHECK(wc.entries()[1].id == "led");
    CHECK_FALSE(wc.entries()[1].enabled);
}

// ============================================================================
// Config tests: add_page
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: add_page inserts at a position, appends by default, and "
                 "refuses at the cap",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.page_count() == 1);

    // An append, then an insert before the first page. Names sit outside the
    // page_N series generate_page_id() mints, so no id repeats.
    REQUIRE(wc.add_page("extra_a") == 1);
    const std::string first_id = wc.page_id(0);
    const size_t main_before = wc.main_page_index();

    REQUIRE(wc.add_page("extra_b", 0) == 0);
    REQUIRE(wc.page_count() == 3);
    REQUIRE(wc.page_id(0) == "extra_b");
    REQUIRE(wc.page_id(1) == first_id);
    // The main page keeps its place in the numbering when a page lands before it.
    REQUIRE(wc.main_page_index() == main_before + 1);
    // The entry the first page held shifted with its page.
    REQUIRE_FALSE(wc.page_entries(1).empty());
    REQUIRE(wc.page_entries(0).empty());

    // A position past the end appends; a negative one is the default append.
    REQUIRE(wc.add_page("extra_c", 99) == 3);
    REQUIRE(wc.add_page("extra_d", -1) == 4);

    // Removing the prepended page shifts the main page's index back down.
    REQUIRE(wc.remove_page(0));
    REQUIRE(wc.main_page_index() == main_before);
    REQUIRE(wc.page_id(0) == first_id);

    while (wc.add_page() >= 0) {
    }
    REQUIRE(wc.page_count() == kMaxPages);
    // At the cap nothing is added, wherever it would land.
    REQUIRE(wc.add_page() == -1);
    REQUIRE(wc.add_page("", 0) == -1);
    REQUIRE(wc.main_page_index() == main_before);

    // A prepended page survives a save and reload in order.
    wc.save();
    PanelWidgetConfig reloaded("home", config);
    reloaded.load();
    REQUIRE(reloaded.page_count() == wc.page_count());
    for (size_t i = 0; i < wc.page_count(); ++i) {
        REQUIRE(reloaded.page_id(i) == wc.page_id(i));
    }
    REQUIRE(reloaded.main_page_index() == wc.main_page_index());
}

// ============================================================================
// Config tests — reorder
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: reorder moves one entry",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    const auto before = wc.entries();
    const int last = static_cast<int>(before.size()) - 1;

    // Each row lists (new index, old index) pairs the move must produce; an empty
    // list means the order is unchanged. -1 stands for the last index.
    struct Row {
        const char* what;
        int from, to;
        std::vector<std::pair<int, int>> expect;
    };
    const Row rows[] = {
        {"2 to 0", 2, 0, {{0, 2}, {1, 0}}},
        {"0 to 3: the entries between shift down", 0, 3, {{0, 1}, {3, 0}}},
        {"0 to the last index", 0, -1, {{-1, 0}}},
        {"the last index to 0", -1, 0, {{0, -1}}},
        {"same index is a no-op", 2, 2, {}},
        {"out of bounds is a no-op", 100, 0, {}},
    };
    const auto at = [last](int i) { return i < 0 ? last : i; };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.what) {
            wc.reorder(static_cast<size_t>(at(row.from)), static_cast<size_t>(at(row.to)));
            const auto& after = wc.entries();
            REQUIRE(after.size() == before.size());
            if (row.expect.empty()) {
                for (size_t i = 0; i < before.size(); ++i) {
                    REQUIRE(after[i].id == before[i].id);
                }
            }
            for (const auto& [now, was] : row.expect) {
                CAPTURE(now, was);
                REQUIRE(after[at(now)].id == before[at(was)].id);
            }
        }
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reorders compose and carry the moved entry's state",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    const std::string id0 = wc.entries()[0].id;
    const std::string id1 = wc.entries()[1].id;
    const std::string id2 = wc.entries()[2].id;
    const std::string id3 = wc.entries()[3].id;
    wc.set_enabled(3, false);

    wc.reorder(0, 2); // [id1, id2, id0, id3, ...]
    wc.reorder(3, 1); // [id1, id3, id2, id0, ...]

    REQUIRE(wc.entries()[0].id == id1);
    REQUIRE(wc.entries()[1].id == id3);
    REQUIRE(wc.entries()[1].enabled == false);
    REQUIRE(wc.entries()[2].id == id2);
    REQUIRE(wc.entries()[3].id == id0);
}

// ============================================================================
// Config tests — toggle enabled
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: set_enabled toggles an entry and ignores a bad index",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    const std::string id = wc.entries()[0].id;

    REQUIRE(wc.entries()[0].enabled == true);
    wc.set_enabled(0, false);
    REQUIRE(wc.entries()[0].enabled == false);
    REQUIRE(wc.is_enabled(id) == false);
    wc.set_enabled(0, true);
    REQUIRE(wc.entries()[0].enabled == true);
    REQUIRE(wc.is_enabled(id) == true);

    const auto before = wc.entries();
    wc.set_enabled(999, false);
    REQUIRE(wc.entries() == before);
    REQUIRE(wc.is_enabled("nonexistent") == false);
}

// ============================================================================
// Config tests — save-load round trip
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reorder + toggle + save + reload preserves everything",
                 "[panel_widget][widget_config]") {
    setup_empty_config();

    PanelWidgetConfig wc1("home", config);
    wc1.load();

    // Do several operations
    wc1.set_enabled(0, false);
    wc1.set_enabled(4, false);
    wc1.reorder(2, 8);
    wc1.reorder(0, 5);
    wc1.save();

    // Reload
    PanelWidgetConfig wc2("home", config);
    wc2.load();

    require_same_widget_state(wc1.entries(), wc2.entries());
}

// ============================================================================
// Per-panel config tests
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: different panels have independent configs",
                 "[panel_widget][widget_config]") {
    setup_empty_config();

    // Set up home config
    PanelWidgetConfig home_wc("home", config);
    home_wc.load();
    home_wc.set_enabled(0, false);
    home_wc.save();

    // Set up controls config (should be independent)
    PanelWidgetConfig ctrl_wc("controls", config);
    ctrl_wc.load();

    // Controls should still have defaults (not affected by home's changes)
    const auto& defs = get_all_widget_defs();
    REQUIRE(ctrl_wc.entries()[0].enabled == defs[0].default_enabled);

    // Home should have its customization
    PanelWidgetConfig home_wc2("home", config);
    home_wc2.load();
    REQUIRE(home_wc2.entries()[0].enabled == false);
}

// ============================================================================
// Migration tests — legacy home_widgets → panel_widgets.home
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migrates legacy home_widgets to panel_widgets.home",
                 "[panel_widget][widget_config][migration]") {
    // Set up old-style flat config (no grid coords — triggers pre-grid reset)
    json legacy = json::array({
        {{"id", "shutdown"}, {"enabled", true}},
        {{"id", "network"}, {"enabled", false}},
        {{"id", "temperature"}, {"enabled", true}},
    });
    setup_with_legacy_widgets(legacy);

    // Verify legacy key exists before migration
    REQUIRE(get_data().contains("home_widgets"));

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Migration moves data to /printers/default/panel_widgets/home and removes old key.
    // Legacy configs without grid coords are detected as pre-grid and reset to defaults,
    // which stay pending until READY: the pages format reaches disk on the next save.
    auto& printer_data = get_data()["printers"]["default"];
    REQUIRE(printer_data.contains("panel_widgets"));
    REQUIRE(printer_data["panel_widgets"].contains("home"));
    CHECK_FALSE(printer_data["panel_widgets"]["home"].is_object());
    wc.save();
    auto root = printer_data["panel_widgets"]["home"];
    REQUIRE(root.is_object());
    REQUIRE(root.contains("pages"));
    REQUIRE_FALSE(get_data().contains("home_widgets"));

    // After pre-grid reset, entries match default grid (all registry widgets present)
    REQUIRE(wc.entries().size() == default_grid_widget_count());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a flat array replaced by the defaults is a live default",
                 "[panel_widget][widget_config][migration][panel_widget_config]") {
    // Whatever the old array held is gone, so the arrangement is the builder's
    // own and waits for a measured grid like any fresh default.
    SECTION("pre-grid entries") {
        setup_with_widgets(json::array({
            {{"id", "temperature"}, {"enabled", true}},
            {{"id", "network"}, {"enabled", true}},
        }));
    }
    SECTION("an empty array") {
        setup_with_widgets(json::array());
    }

    PanelWidgetConfig wc("home", config);
    wc.load();
    CHECK(wc.is_unedited_default());
    CHECK(wc.has_pending_anchors());
    // Nothing is written before READY: the old array is still what is on disk.
    CHECK_FALSE(get_printer_data()["panel_widgets"]["home"].is_object());
    wc.save();
    const json saved = get_printer_data()["panel_widgets"]["home"];
    REQUIRE(saved.is_object());
    CHECK(saved.contains("default"));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a flat array with grid positions is the user's",
                 "[panel_widget][widget_config][migration][panel_widget_config]") {
    setup_with_widgets(json::array({
        {{"id", "temperature"}, {"enabled", true}, {"col", 0}, {"row", 0}},
    }));

    PanelWidgetConfig wc("home", config);
    wc.load();
    CHECK_FALSE(wc.is_unedited_default());
    CHECK_FALSE(wc.has_pending_anchors());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration does not trigger for non-home panels",
                 "[panel_widget][widget_config][migration]") {
    // Set up legacy home_widgets
    json legacy = json::array({
        {{"id", "shutdown"}, {"enabled", true}},
    });
    setup_with_legacy_widgets(legacy);

    // Loading "controls" should NOT migrate home_widgets
    PanelWidgetConfig wc("controls", config);
    wc.load();

    // Legacy key should still exist (untouched)
    REQUIRE(get_data().contains("home_widgets"));

    // Controls should get defaults
    REQUIRE(wc.entries().size() == default_grid_widget_count());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration skipped if panel_widgets.home already exists",
                 "[panel_widget][widget_config][migration]") {
    // Set up both legacy and new-style config (include grid coords to prevent pre-grid reset)
    json legacy = json::array({
        {{"id", "shutdown"}, {"enabled", false}},
    });
    json new_style = json::array({
        {{"id", "network"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "temperature"}, {"enabled", true}, {"col", 1}, {"row", 0}},
    });

    get_data() = json::object();
    get_data()["home_widgets"] = legacy;
    get_data()["printers"]["default"]["panel_widgets"]["home"] = new_style;

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Should use the new-style config, not the legacy one
    REQUIRE(wc.entries()[0].id == "network");
    REQUIRE(wc.entries()[1].id == "temperature");

    // Legacy key should still exist (not removed since no migration happened)
    REQUIRE(get_data().contains("home_widgets"));
}

// ============================================================================
// Grid coordinate tests
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: grid coordinates load, default to -1 when missing, and "
                 "round-trip",
                 "[panel_widget][widget_config][grid]") {
    // A placed entry keeps the pre-grid reset away from the unplaced one.
    json widgets = json::array({
        {{"id", "network"},
         {"enabled", true},
         {"col", 1},
         {"row", 0},
         {"colspan", 1},
         {"rowspan", 1}},
        {{"id", "temperature"},
         {"enabled", true},
         {"col", 2},
         {"row", 1},
         {"colspan", 2},
         {"rowspan", 2}},
        {{"id", "shutdown"}, {"enabled", true}},
    });
    setup_with_widgets(widgets);

    const auto check_placed = [](const PanelWidgetConfig& wc) {
        REQUIRE(wc.entries()[0].id == "network");
        CHECK(wc.entries()[0].col == 1);
        CHECK(wc.entries()[0].row == 0);
        CHECK(wc.entries()[0].colspan == 1);
        CHECK(wc.entries()[0].rowspan == 1);
        CHECK(wc.entries()[0].has_grid_position());
        REQUIRE(wc.entries()[1].id == "temperature");
        CHECK(wc.entries()[1].col == 2);
        CHECK(wc.entries()[1].row == 1);
        CHECK(wc.entries()[1].colspan == 2);
        CHECK(wc.entries()[1].rowspan == 2);
    };

    PanelWidgetConfig wc("home", config);
    wc.load();
    check_placed(wc);
    // No coordinates: unplaced, with the registry's span for the widget.
    const auto* shutdown = find_entry(wc.entries(), "shutdown");
    REQUIRE(shutdown != nullptr);
    CHECK(shutdown->col == -1);
    CHECK(shutdown->row == -1);
    CHECK(shutdown->colspan == 2);
    CHECK(shutdown->rowspan == 2);
    CHECK_FALSE(shutdown->has_grid_position());

    // Every entry writes col/row so a position survives a reload; an unplaced
    // one writes -1.
    wc.save();
    const json saved = get_saved_page0_widgets();
    const auto it = std::find_if(saved.begin(), saved.end(),
                                 [](const json& item) { return item["id"] == "shutdown"; });
    REQUIRE(it != saved.end());
    REQUIRE(it->contains("col"));
    REQUIRE(it->contains("row"));
    CHECK(it->at("col").get<int>() == -1);
    CHECK(it->at("row").get<int>() == -1);

    PanelWidgetConfig reloaded("home", config);
    reloaded.load();
    check_placed(reloaded);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: build_default_grid all enabled widgets get grid positions",
                 "[panel_widget][widget_config][grid]") {
    auto grid = PanelWidgetConfig::build_default_grid();
    REQUIRE(grid.size() == default_grid_widget_count());

    // Which ids the shipped table anchors is per-tier and belongs to the table,
    // not to this test — it used to hardcode five and broke the moment the
    // landscape layout was re-authored. What must hold everywhere: the two
    // widgets every tier anchors are placed, a disabled widget never is, and no
    // entry comes back half-placed.
    for (const char* id : {"printer_image", "print_status"}) {
        const auto it = std::find_if(grid.begin(), grid.end(),
                                     [&](const PanelWidgetEntry& e) { return e.id == id; });
        INFO("widget " << id);
        REQUIRE(it != grid.end());
        REQUIRE(it->has_grid_position());
    }
    for (const auto& entry : grid) {
        INFO("Widget " << entry.id << " enabled=" << entry.enabled << " col=" << entry.col
                       << " row=" << entry.row);
        if (!entry.enabled) {
            REQUIRE_FALSE(entry.has_grid_position());
        }
        if (!entry.has_grid_position()) {
            REQUIRE(entry.col == -1);
            REQUIRE(entry.row == -1);
        }
    }
}

TEST_CASE("PanelWidgetConfig: build_default_grid produces correct layout",
          "[panel_widget][widget_config][grid]") {
    // Pin the breakpoint this layout is asserted against. Through the guard, so
    // it is put back: a bare set left every later test building a Micro grid.
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
    auto entries = PanelWidgetConfig::build_default_grid();

    // Should include all registry widgets
    REQUIRE(entries.size() == default_grid_widget_count());

    // Collect enabled entries with grid positions
    std::vector<PanelWidgetEntry> placed;
    std::vector<PanelWidgetEntry> disabled;
    for (const auto& e : entries) {
        if (e.is_placed()) {
            placed.push_back(e);
        } else if (!e.enabled) {
            disabled.push_back(e);
        }
    }

    // Key layout assertions: fixed widgets at expected positions
    auto find_entry = [&](const std::string& id) -> const PanelWidgetEntry* {
        for (const auto& e : entries) {
            if (e.id == id)
                return &e;
        }
        return nullptr;
    };

    // The micro landscape table: printer image 3x2 cells top-left, six 1x1
    // readouts filling the rest of the top two rows, print_status (4x2 cells)
    // beside the temperature graph (2x2) below. Tracks are half-cells, so a
    // cell span of 3x2 reads as 6x4.
    auto* pi = find_entry("printer_image");
    REQUIRE(pi);
    REQUIRE(pi->enabled);
    REQUIRE(pi->col == 0);
    REQUIRE(pi->row == 0);
    REQUIRE(pi->colspan == 6);
    REQUIRE(pi->rowspan == 4);

    auto* ps = find_entry("print_status");
    REQUIRE(ps);
    REQUIRE(ps->enabled);
    REQUIRE(ps->col == 0);
    REQUIRE(ps->row == 4);
    REQUIRE(ps->colspan == 8);
    REQUIRE(ps->rowspan == 4);
    auto* graph = find_entry("temp_graph");
    REQUIRE(graph);
    REQUIRE(graph->enabled);
    REQUIRE(graph->col == 8);
    REQUIRE(graph->row == 4);
    REQUIRE(graph->colspan == 4);
    REQUIRE(graph->rowspan == 4);

    // Four cell rows only pay for the image block and the bottom band.
    auto* tips = find_entry("tips");
    REQUIRE(tips);
    REQUIRE_FALSE(tips->enabled);
    REQUIRE_FALSE(tips->has_grid_position());

    // Positions are all-or-nothing. Which ids the shipped table anchors is the
    // table's business and differs per tier; a half-placed entry is a bug.
    for (const auto& e : entries) {
        INFO("Widget " << e.id << " at (" << e.col << "," << e.row << ")");
        if (e.has_grid_position()) {
            REQUIRE(e.col >= 0);
            REQUIRE(e.row >= 0);
        } else {
            REQUIRE(e.col == -1);
            REQUIRE(e.row == -1);
        }
    }

    // The readout block: nozzle and bed lead the top row beside the image.
    auto* nozzle = find_entry("temperature");
    REQUIRE(nozzle);
    REQUIRE(nozzle->enabled);
    REQUIRE(nozzle->col == 6);
    REQUIRE(nozzle->row == 0);
    auto* bed_anchor = find_entry("bed_temperature");
    REQUIRE(bed_anchor);
    REQUIRE(bed_anchor->col == 8);
    REQUIRE(bed_anchor->row == 0);

    // Anchors must not overlap each other. Two entries claiming the same
    // track read as two chosen positions and only one of them survives
    // placement. (main asserted this alongside a free-cell count against a
    // fixed per-tier grid; that half does not carry over — the square-cell
    // grid derives its track counts from the measured content box.)
    {
        std::set<std::pair<int, int>> occupied;
        for (const auto& e : entries) {
            if (!e.is_placed())
                continue;
            for (int dc = 0; dc < e.colspan; ++dc) {
                for (int dr = 0; dr < e.rowspan; ++dr) {
                    INFO("anchor " << e.id << " covers " << (e.col + dc) << "," << (e.row + dr));
                    REQUIRE(occupied.insert({e.col + dc, e.row + dr}).second);
                }
            }
        }
    }

    // Disabled widgets should have no grid position
    for (const auto& e : disabled) {
        INFO("Disabled widget " << e.id);
        REQUIRE_FALSE(e.has_grid_position());
    }

    // fan_stack is multi_instance but included once as base ID in defaults
    auto* fs = find_entry("fan_stack");
    REQUIRE(fs);

    // notifications is enabled (default_enabled, no gate) and, since the
    // landscape rework, anchored top-right of the readout block rather than
    // left to auto-place.
    auto* notif = find_entry("notifications");
    REQUIRE(notif);
    REQUIRE(notif->enabled);
    REQUIRE(notif->has_grid_position());
    REQUIRE(notif->col == 10);
    REQUIRE(notif->row == 0);

    // bed_temperature: default_enabled=false in registry, but build_default_grid()
    // overrides to enabled when no AMS present (which is the case in tests).
    // It should also be the LAST entry (moved to end by grid builder).
    auto* bed = find_entry("bed_temperature");
    REQUIRE(bed);
    REQUIRE(bed->enabled);                           // No AMS in test env → enabled
    REQUIRE(entries.back().id == "bed_temperature"); // Moved to last position
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: is_grid_format detects grid entries",
                 "[panel_widget][widget_config][grid]") {
    // Config with grid coords
    json widgets_grid = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
    });
    setup_with_widgets(widgets_grid);
    PanelWidgetConfig wc1("home", config);
    wc1.load();
    REQUIRE(wc1.is_grid_format());

    // Config without grid coords — gets migrated to grid format
    json widgets_flat = json::array({
        {{"id", "shutdown"}, {"enabled", true}},
    });
    setup_with_widgets(widgets_flat);
    PanelWidgetConfig wc2("home", config);
    wc2.load();
    REQUIRE(wc2.is_grid_format()); // Pre-grid configs auto-migrate
}

// ============================================================================
// Multi-page tests — page_count and default page
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: default config creates single page",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.page_count() == 1);
    REQUIRE(wc.main_page_index() == 0);
    REQUIRE(wc.page_id(0) == "main");
    // entries() delegates to page 0
    REQUIRE(wc.entries().size() == default_grid_widget_count());
    REQUIRE(&wc.entries() == &wc.page_entries(0));
}

// ============================================================================
// Multi-page tests — legacy migration wraps in single page
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: legacy array format migrates to single-page format",
                 "[panel_widget][widget_config][multipage][migration]") {
    json widgets = json::array({
        {{"id", "temperature"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "shutdown"}, {"enabled", false}, {"col", 1}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Should have migrated to single page with id "main"
    REQUIRE(wc.page_count() == 1);
    REQUIRE(wc.page_id(0) == "main");
    REQUIRE(wc.entries()[0].id == "temperature");
    REQUIRE(wc.entries()[1].id == "shutdown");

    // Verify saved format is the new pages format
    auto root = get_saved_root();
    REQUIRE(root.is_object());
    REQUIRE(root.contains("pages"));
    REQUIRE(root["pages"].is_array());
    REQUIRE(root["pages"].size() == 1);
    REQUIRE(root["pages"][0]["id"] == "main");
    REQUIRE(root.contains("main_page_index"));
    REQUIRE(root["main_page_index"] == 0);
    REQUIRE(root.contains("next_page_id"));
}

// ============================================================================
// Multi-page tests — add and remove pages
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: add_page and remove_page keep the page list consistent",
                 "[panel_widget][widget_config][multipage][1638]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.page_count() == 1);
    CHECK(wc.can_add_page());

    // The only page cannot go.
    REQUIRE_FALSE(wc.remove_page(0));
    REQUIRE(wc.page_count() == 1);

    // A named page is appended empty; page 0 keeps its widgets.
    REQUIRE(wc.add_page("my_page") == 1);
    REQUIRE(wc.page_id(1) == "my_page");
    REQUIRE(wc.page_entries(1).empty());
    REQUIRE(wc.entries().size() == default_grid_widget_count());

    // An unnamed page gets a minted page_N id.
    const int idx = wc.add_page();
    REQUIRE(idx == 2);
    REQUIRE(wc.page_id(static_cast<size_t>(idx)).substr(0, 5) == "page_");

    // Removing a non-main page closes the gap; an index past the end removes nothing.
    REQUIRE(wc.remove_page(1));
    REQUIRE(wc.page_count() == 2);
    REQUIRE(wc.page_id(0) == "main");
    REQUIRE(wc.page_id(1).substr(0, 5) == "page_");
    REQUIRE_FALSE(wc.remove_page(99));
    REQUIRE(wc.page_count() == 2);

    // can_add_page tracks the cap both ways.
    while (wc.page_count() < kMaxPages) {
        REQUIRE(wc.add_page() >= 0);
    }
    CHECK_FALSE(wc.can_add_page());
    REQUIRE(wc.remove_page(wc.page_count() - 1));
    REQUIRE(wc.page_count() == kMaxPages - 1);
    CHECK(wc.can_add_page());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: remove_page adjusts main_page_index",
                 "[panel_widget][widget_config][multipage]") {
    SECTION("removing page before main shifts main index down") {
        json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
        json w1 = json::array({{{"id", "network"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
        json w2 = json::array({{{"id", "temperature"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
        setup_with_pages({{"p0", w0}, {"p1", w1}, {"p2", w2}}, 2, 3);

        PanelWidgetConfig wc2("home", config);
        wc2.load();
        REQUIRE(wc2.main_page_index() == 2);

        wc2.remove_page(0);
        REQUIRE(wc2.main_page_index() == 1);
        REQUIRE(wc2.page_id(1) == "p2");
    }

    SECTION("removing the main page is a no-op") {
        json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
        json w1 = json::array({{{"id", "network"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
        setup_with_pages({{"p0", w0}, {"p1", w1}}, 1, 2);

        PanelWidgetConfig wc2("home", config);
        wc2.load();
        REQUIRE(wc2.main_page_index() == 1);
        REQUIRE(wc2.page_count() == 2);

        REQUIRE_FALSE(wc2.remove_page(1));
        REQUIRE(wc2.page_count() == 2);
        REQUIRE(wc2.main_page_index() == 1);
    }
}

// ============================================================================
// Multi-page tests — cross-page operations
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: id lookups and edits search every page",
                 "[panel_widget][widget_config][multipage]") {
    // Multi-instance ids exist only on page 1, so a lookup that stops at page 0
    // finds nothing (or the registry default appended to page 0).
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", false}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({
        {{"id", "thermistor:1"},
         {"enabled", true},
         {"config", {{"sensor", "bed"}}},
         {"col", 0},
         {"row", 0}},
        {{"id", "thermistor:2"}, {"enabled", false}, {"col", 1}, {"row", 0}},
        {{"id", "thermistor:5"}, {"enabled", true}, {"col", 2}, {"row", 0}},
    });
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE_FALSE(wc.is_enabled("shutdown"));
    REQUIRE(wc.is_enabled("thermistor:1"));
    REQUIRE_FALSE(wc.is_enabled("thermistor:2"));
    REQUIRE_FALSE(wc.is_enabled("nonexistent"));

    auto cfg = wc.get_widget_config("thermistor:1");
    REQUIRE(cfg.contains("sensor"));
    REQUIRE(cfg["sensor"] == "bed");
    wc.set_widget_config("thermistor:1", {{"sensor", "extruder"}});
    REQUIRE(wc.get_widget_config("thermistor:1")["sensor"] == "extruder");

    // The highest instance number across all pages is 5.
    REQUIRE(wc.mint_instance_id("thermistor") == "thermistor:6");

    REQUIRE(wc.page_entries(1).size() == 3);
    wc.delete_entry("thermistor:1");
    REQUIRE(wc.page_entries(1).size() == 2);
    REQUIRE(wc.page_entries(1)[0].id == "thermistor:2");
    REQUIRE(wc.page_entries(0)[0].id == "shutdown");
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: remove_from_page deletes an added instance, parks a built-in",
                 "[panel_widget][widget_config][multipage]") {
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "thermistor:1"}, {"enabled", true}, {"col", 2}, {"row", 0}},
    });
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.page_entries(1).size() == 2);

    wc.remove_from_page(1, 1);
    REQUIRE(wc.page_entries(1).size() == 1);
    CHECK(wc.page_entries(1)[0].id == "shutdown");

    // A built-in stays listed for the catalog, off the grid, and only on the
    // page it was removed from.
    wc.remove_from_page(1, 0);
    REQUIRE(wc.page_entries(1).size() == 1);
    CHECK_FALSE(wc.page_entries(1)[0].enabled);
    CHECK_FALSE(wc.page_entries(1)[0].is_placed());
    CHECK(wc.page_entries(0)[0].enabled);
}

// ============================================================================
// Multi-page tests — placement and page population
// ============================================================================

/// The page of every entry named @p id, one element per entry, in page order.
static std::vector<size_t> pages_holding(const PanelWidgetConfig& wc, const std::string& id) {
    std::vector<size_t> found;
    for (size_t p = 0; p < wc.page_count(); ++p) {
        for (const auto& e : wc.page_entries(p)) {
            if (e.id == id) {
                found.push_back(p);
            }
        }
    }
    return found;
}

TEST_CASE("PanelWidgetEntry: is_placed needs both enabled and a grid cell",
          "[panel_widget][widget_config][multipage][1638]") {
    CHECK(PanelWidgetEntry{"a", true, {}, 0, 0, 1, 1}.is_placed());
    CHECK_FALSE(PanelWidgetEntry{"b", true, {}, -1, -1, 1, 1}.is_placed()); // enabled, no cell
    CHECK_FALSE(PanelWidgetEntry{"c", false, {}, 0, 0, 1, 1}.is_placed());  // a cell, disabled
    CHECK_FALSE(PanelWidgetEntry{"d", true, {}, 2, -1, 1, 1}.is_placed());  // half a coordinate
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: page_is_populated asks for a placed entry",
                 "[panel_widget][widget_config][multipage][1638]") {
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({
        {{"id", "thermistor:1"}, {"enabled", false}, {"col", 0}, {"row", 0}},
        {{"id", "thermistor:2"}, {"enabled", true}},
    });
    setup_with_pages({{"p0", w0}, {"p1", w1}, {"p2", json::array()}}, 0, 3);

    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.page_count() == 3);
    // Page 1 holds entries, and none of them is placed.
    REQUIRE(wc.page_entries(1).size() == 2);
    REQUIRE(wc.page_entries(1)[1].enabled);
    REQUIRE_FALSE(wc.page_entries(1)[1].has_grid_position());

    CHECK(wc.page_is_populated(0));
    CHECK_FALSE(wc.page_is_populated(1)); // a disabled entry with a cell, an enabled one without
    CHECK_FALSE(wc.page_is_populated(2)); // no entries at all
    CHECK_FALSE(wc.page_is_populated(3)); // past the last page

    wc.page_entries_mut(1)[1].col = 1;
    wc.page_entries_mut(1)[1].row = 0;
    CHECK(wc.page_is_populated(1));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: page_entries reads a page past the last as empty",
                 "[panel_widget][widget_config][multipage][1638]") {
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({{{"id", "thermistor:1"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.page_count() == 2);
    // The last page holds an entry, so a read past it that lands on a real
    // page shows up as a non-empty result.
    REQUIRE_FALSE(wc.page_entries(1).empty());

    CHECK(wc.page_entries(2).empty());  // the next-page slot's index
    CHECK(wc.page_entries(10).empty()); // further out
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: place_entry moves, updates or creates the entry on its page",
                 "[panel_widget][widget_config][multipage][1638]") {
    const json shutdown = json{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}};
    struct Row {
        const char* what;
        json w0, w1;
        const char* id;
        int col, row, colspan, rowspan;
        int landed;          // index on page 1
        size_t landing_size; // page 1 entries afterwards
    };
    const Row rows[] = {
        {"moved from another page: appended after the landing page's entries",
         json::array(
             {shutdown, {{"id", "thermistor:1"}, {"enabled", true}, {"col", 1}, {"row", 0}}}),
         json::array({{{"id", "thermistor:2"}, {"enabled", true}, {"col", 0}, {"row", 0}}}),
         "thermistor:1", 2, 1, 2, 2, 1, 2},
        {"already on the page: updated where it stands, not appended", json::array({shutdown}),
         json::array({{{"id", "thermistor:1"}, {"enabled", false}, {"col", -1}, {"row", -1}},
                      {{"id", "thermistor:2"}, {"enabled", true}, {"col", 0}, {"row", 0}}}),
         "thermistor:1", 3, 2, 2, 1, 0, 2},
        {"held by no page: created", json::array({shutdown}),
         json::array({{{"id", "thermistor:2"}, {"enabled", true}, {"col", 0}, {"row", 0}}}),
         "thermistor:7", 1, 0, 1, 1, 1, 2},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.what) {
            setup_with_pages({{"p0", row.w0}, {"p1", row.w1}}, 0, 2);
            PanelWidgetConfig wc("home", config);
            wc.load();

            const int landed =
                wc.place_entry(row.id, 1, row.col, row.row, row.colspan, row.rowspan);

            REQUIRE(landed == row.landed);
            CHECK(pages_holding(wc, row.id) == std::vector<size_t>{1});
            CHECK(wc.page_entries(1).size() == row.landing_size);
            const auto& e = wc.page_entries(1)[static_cast<size_t>(landed)];
            CHECK(e.id == row.id);
            CHECK(e.enabled);
            CHECK(e.col == row.col);
            CHECK(e.row == row.row);
            CHECK(e.colspan == row.colspan);
            CHECK(e.rowspan == row.rowspan);
            // The origin page loses only the moved entry.
            CHECK(wc.page_entries(0).front().id == "shutdown");
        }
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: place_entry keeps the widget's config with it",
                 "[panel_widget][widget_config][multipage][1638]") {
    const json shutdown = json{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}};

    SECTION("a moved entry brings its config") {
        json w0 = json::array({shutdown,
                               {{"id", "thermistor:1"},
                                {"enabled", true},
                                {"config", {{"sensor", "bed"}}},
                                {"col", 1},
                                {"row", 0}}});
        setup_with_pages({{"p0", w0}, {"p1", json::array()}}, 0, 2);
        PanelWidgetConfig wc("home", config);
        wc.load();

        const int landed = wc.place_entry("thermistor:1", 1, 0, 0, 1, 1);

        REQUIRE(landed == 0);
        CHECK(wc.page_entries(1)[0].config.value("sensor", "") == "bed");
    }

    SECTION("an entry on the landing page keeps its own config over a duplicate's") {
        json w0 = json::array({shutdown,
                               {{"id", "thermistor:1"},
                                {"enabled", true},
                                {"config", {{"sensor", "bed"}}},
                                {"col", 1},
                                {"row", 0}}});
        json w1 = json::array({{{"id", "thermistor:1"},
                                {"enabled", false},
                                {"config", {{"sensor", "chamber"}}},
                                {"col", -1},
                                {"row", -1}}});
        setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);
        PanelWidgetConfig wc("home", config);
        wc.load();

        const int landed = wc.place_entry("thermistor:1", 1, 0, 0, 1, 1);

        REQUIRE(landed == 0);
        CHECK(wc.page_entries(1)[0].config.value("sensor", "") == "chamber");
    }

    SECTION("an unconfigured entry on the landing page takes a duplicate's config") {
        json w0 = json::array({shutdown,
                               {{"id", "thermistor:1"},
                                {"enabled", true},
                                {"config", {{"sensor", "bed"}}},
                                {"col", 1},
                                {"row", 0}}});
        json w1 =
            json::array({{{"id", "thermistor:1"}, {"enabled", false}, {"col", -1}, {"row", -1}}});
        setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);
        PanelWidgetConfig wc("home", config);
        wc.load();

        const int landed = wc.place_entry("thermistor:1", 1, 0, 0, 1, 1);

        REQUIRE(landed == 0);
        CHECK(wc.page_entries(1)[0].config.value("sensor", "") == "bed");
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: place_entry leaves one entry per id across pages",
                 "[panel_widget][widget_config][multipage][1638]") {
    const json twin = json{{"id", "thermistor:1"}, {"enabled", false}, {"col", -1}, {"row", -1}};
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}, twin});
    json w1 = json::array({twin, {{"id", "thermistor:2"}, {"enabled", true}}});
    json w2 = json::array({twin});
    setup_with_pages({{"p0", w0}, {"p1", w1}, {"p2", w2}}, 0, 3);

    PanelWidgetConfig wc("home", config);
    wc.load();
    // load() drops an id repeated within one page, so the landing page's own
    // twin is added in memory.
    wc.page_entries_mut(1).push_back({"thermistor:1", false, json::object(), -1, -1, 1, 1});
    REQUIRE(pages_holding(wc, "thermistor:1") == std::vector<size_t>{0, 1, 1, 2});

    const int landed = wc.place_entry("thermistor:1", 1, 0, 0, 1, 1);

    CHECK(pages_holding(wc, "thermistor:1") == std::vector<size_t>{1});
    REQUIRE(landed == 0);
    CHECK(wc.page_entries(1)[0].is_placed());
    CHECK(wc.page_entries(1).size() == 2); // the landing page's own twin is gone too
    CHECK(wc.page_entries(2).empty());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: place_entry refuses a page past the last",
                 "[panel_widget][widget_config][multipage][1638]") {
    json w0 = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "thermistor:1"}, {"enabled", true}, {"col", 1}, {"row", 0}},
    });
    setup_with_pages({{"p0", w0}, {"p1", json::array()}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();
    const auto page0_before = wc.page_entries(0);

    CHECK(wc.place_entry("thermistor:1", 2, 0, 0, 1, 1) == -1);

    CHECK(wc.page_count() == 2);
    CHECK(wc.page_entries(0) == page0_before); // the entry was not taken from its page
    CHECK(wc.page_entries(1).empty());
}

// ============================================================================
// Multi-page tests — backward compatibility
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reorder and set_enabled operate on page 0",
                 "[panel_widget][widget_config][multipage]") {
    json w0 = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "network"}, {"enabled", true}, {"col", 1}, {"row", 0}},
        {{"id", "temperature"}, {"enabled", true}, {"col", 2}, {"row", 0}},
    });
    json w1 = json::array({{{"id", "thermistor:1"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Reorder on page 0
    wc.reorder(0, 2);
    REQUIRE(wc.entries()[2].id == "shutdown");

    // set_enabled on page 0
    wc.set_enabled(0, false);
    REQUIRE(wc.entries()[0].enabled == false);

    // Page 1 unchanged (multi-instance ID only on page 1)
    REQUIRE(wc.page_entries(1)[0].id == "thermistor:1");
    REQUIRE(wc.page_entries(1)[0].enabled == true);
}

// ============================================================================
// Multi-page tests — reset_to_defaults
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reset_to_defaults removes extra pages",
                 "[panel_widget][widget_config][multipage]") {
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({{{"id", "network"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 1, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.page_count() == 2);
    REQUIRE(wc.main_page_index() == 1);

    wc.reset_to_defaults();

    REQUIRE(wc.page_count() == 1);
    REQUIRE(wc.main_page_index() == 0);
    REQUIRE(wc.page_id(0) == "main");
    REQUIRE(wc.entries().size() == default_grid_widget_count());
}

// ============================================================================
// Multi-page tests — save/reload round-trip
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: multi-page save-reload round-trip",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc1("home", config);
    wc1.load();

    // Add a second page with some widgets
    int page_idx = wc1.add_page("second");
    REQUIRE(page_idx == 1);
    wc1.page_entries_mut(1).push_back({"shutdown", true, {}, 0, 0, 1, 1});
    wc1.page_entries_mut(1).push_back({"network", false, {}, 1, 0, 1, 1});
    wc1.save();

    // Reload from same config
    PanelWidgetConfig wc2("home", config);
    wc2.load();

    REQUIRE(wc2.page_count() == 2);
    REQUIRE(wc2.page_id(0) == "main");
    REQUIRE(wc2.page_id(1) == "second");
    REQUIRE(wc2.main_page_index() == 0);

    // Page 0 should have all default widgets
    REQUIRE(wc2.page_entries(0).size() == default_grid_widget_count());

    // Page 1 should have our two widgets
    REQUIRE(wc2.page_entries(1).size() == 2);
    REQUIRE(wc2.page_entries(1)[0].id == "shutdown");
    REQUIRE(wc2.page_entries(1)[0].enabled == true);
    REQUIRE(wc2.page_entries(1)[1].id == "network");
    REQUIRE(wc2.page_entries(1)[1].enabled == false);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: generate_page_id produces unique IDs",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    auto id1 = wc.generate_page_id();
    auto id2 = wc.generate_page_id();
    REQUIRE(id1 != id2);
    REQUIRE(id1.substr(0, 5) == "page_");
    REQUIRE(id2.substr(0, 5) == "page_");
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: load multi-page format with main_page_index",
                 "[panel_widget][widget_config][multipage]") {
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({{{"id", "network"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w2 = json::array({{{"id", "temperature"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    setup_with_pages({{"main", w0}, {"page_1", w1}, {"page_2", w2}}, 1, 3);

    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.page_count() == 3);
    REQUIRE(wc.main_page_index() == 1);
    REQUIRE(wc.page_id(0) == "main");
    REQUIRE(wc.page_id(1) == "page_1");
    REQUIRE(wc.page_id(2) == "page_2");
}

// ============================================================================
// migrate_stuck_ams_filament_swap: repairs an ams widget left unplaced beside a placed filament
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a stuck ams(-1,-1) beside a placed filament swaps, and only "
                 "that shape",
                 "[panel_widget][widget_config][migration]") {
    struct Slot {
        bool enabled;
        int col, row;
    };
    struct Row {
        const char* what;
        const char* panel;
        Slot ams_in, fil_in;
        Slot ams_out, fil_out;
    };
    // Rows after the first break one condition of the stuck shape: ams enabled
    // but unplaced, filament enabled and placed, on the home panel.
    const Row rows[] = {
        {"stuck: ams takes filament's cell",
         "home",
         {true, -1, -1},
         {true, 3, 3},
         {true, 3, 3},
         {false, -1, -1}},
        {"ams disabled", "home", {false, -1, -1}, {true, 3, 3}, {false, -1, -1}, {true, 3, 3}},
        {"ams already placed", "home", {true, 2, 1}, {true, 3, 3}, {true, 2, 1}, {true, 3, 3}},
        {"filament disabled", "home", {true, -1, -1}, {false, 3, 3}, {true, -1, -1}, {false, 3, 3}},
        {"filament unplaced",
         "home",
         {true, -1, -1},
         {true, -1, -1},
         {true, -1, -1},
         {true, -1, -1}},
        {"a non-home panel",
         "other_panel",
         {true, -1, -1},
         {true, 3, 3},
         {true, -1, -1},
         {true, 3, 3}},
    };
    const auto check = [](const PanelWidgetEntry* e, const Slot& want) {
        REQUIRE(e != nullptr);
        CHECK(e->enabled == want.enabled);
        CHECK(e->col == want.col);
        CHECK(e->row == want.row);
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.what) {
            const json widgets = json::array({
                {{"id", "ams"},
                 {"enabled", row.ams_in.enabled},
                 {"col", row.ams_in.col},
                 {"row", row.ams_in.row}},
                {{"id", "filament"},
                 {"enabled", row.fil_in.enabled},
                 {"col", row.fil_in.col},
                 {"row", row.fil_in.row}},
                {{"id", "printer_image"}, {"enabled", true}, {"col", 0}, {"row", 0}},
            });
            setup_with_pages({{"main", widgets}}, 0, -1, row.panel);

            PanelWidgetConfig wc(row.panel, config);
            wc.load();
            check(find_entry(wc.entries(), "ams"), row.ams_out);
            check(find_entry(wc.entries(), "filament"), row.fil_out);

            // A second load of the migrated config leaves it as it is.
            PanelWidgetConfig again(row.panel, config);
            again.load();
            check(find_entry(again.entries(), "ams"), row.ams_out);
            check(find_entry(again.entries(), "filament"), row.fil_out);
        }
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration per-page on multi-page home",
                 "[panel_widget][widget_config][migration][multipage]") {
    json page0 = json::array({
        {{"id", "ams"}, {"enabled", true}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", true}, {"col", 3}, {"row", 3}},
    });
    // Page 1 has filament placed but ams disabled — must be left alone
    json page1 = json::array({
        {{"id", "ams"}, {"enabled", false}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", true}, {"col", 1}, {"row", 2}},
    });
    setup_with_pages({{"main", page0}, {"extra", page1}});

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto& p0 = wc.page_entries(0);
    const auto& p1 = wc.page_entries(1);
    const auto* p0_ams = find_entry(p0, "ams");
    const auto* p0_fil = find_entry(p0, "filament");
    const auto* p1_fil = find_entry(p1, "filament");
    REQUIRE(p0_ams);
    REQUIRE(p0_fil);
    REQUIRE(p1_fil);
    REQUIRE(p0_ams->col == 3);
    REQUIRE(p0_ams->row == 3);
    REQUIRE_FALSE(p0_fil->enabled);
    // page 1 untouched
    REQUIRE(p1_fil->enabled);
    REQUIRE(p1_fil->col == 1);
    REQUIRE(p1_fil->row == 2);
}

// ============================================================================
// Preset seeds are default-layout overlays
// ============================================================================

namespace {

/// Writes panel_widgets/<preset>/home.json under the guard's config dir.
void write_seed(const ConfigDirGuard& guard, const std::string& preset, const json& seed) {
    std::filesystem::create_directories(guard.dir / "panel_widgets" / preset);
    std::ofstream out(guard.dir / "panel_widgets" / preset / "home.json");
    out << seed.dump();
}

const json* find_placed_widget(const json& widgets, const std::string& id) {
    for (const auto& w : widgets) {
        if (w.value("id", std::string{}) == id && w.value("enabled", false) &&
            w.value("col", -1) >= 0) {
            return &w;
        }
    }
    return nullptr;
}

/// A seed that removes tips and replaces the spool slot's large placement.
const json kOverlaySeed = {
    {"anchors",
     {{{"id", "tips"}, {"remove", true}},
      {{"slot", "spool_env"},
       {"placements", {{"large", {{"col", 8}, {"row", 0}, {"colspan", 4}, {"rowspan", 2}}}}},
       {"alternatives", {{{"cells", {{{"candidates", {"clock"}}, {"at", {0, 0, 4, 2}}}}}}}}}}},
    {"disabled", {{"large", {"tips"}}}}};

} // namespace

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "a preset seed overlay removes an anchor and replaces a slot",
                 "[panel_widget_config][preset]") {
    ConfigDirGuard guard("preset-overlay");
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);

    // Without a seed the large table places tips and no clock.
    setup_empty_config();
    PanelWidgetConfig plain("home", config);
    plain.load();
    plain.save();
    const json base = get_saved_page0_widgets();
    REQUIRE(find_placed_widget(base, "tips") != nullptr);
    REQUIRE(find_placed_widget(base, "clock") == nullptr);

    write_seed(guard, "testp", kOverlaySeed);
    setup_empty_config();
    ConfigTestAccess::data(config)["printers"]["default"]["preset"] = "testp";

    PanelWidgetConfig wc("home", config);
    wc.load();
    // The default block is what lets the layout re-resolve when a capability
    // changes and what a Reset re-arms; an unstamped seed would read as a user edit.
    CHECK(wc.is_unedited_default());
    wc.save();
    const json saved = get_printer_data()["panel_widgets"]["home"];
    REQUIRE(saved.contains("default"));
    CHECK(saved["default"].contains("hash"));

    const json widgets = get_saved_page0_widgets();
    CHECK(find_placed_widget(widgets, "tips") == nullptr);
    const json* clock = find_placed_widget(widgets, "clock");
    REQUIRE(clock != nullptr);
    CHECK((*clock)["col"] == 8);
    CHECK((*clock)["row"] == 0);
    CHECK((*clock)["colspan"] == 4);
    CHECK((*clock)["rowspan"] == 2);
    // The rest of the base table still stands.
    CHECK(find_placed_widget(widgets, "printer_image") != nullptr);
    CHECK(find_placed_widget(widgets, "print_status") != nullptr);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "a preset seed is parsed once per file stamp",
                 "[panel_widget_config][preset]") {
    ConfigDirGuard guard("preset-overlay-cache");
    write_seed(guard, "testp", json{{"anchors", {{{"id", "tips"}, {"remove", true}}}}});
    const auto path = guard.dir / "panel_widgets" / "testp" / "home.json";

    const auto first = PanelWidgetConfig::load_preset_overlay("testp");
    REQUIRE(first->is_object());
    const int reads = PanelWidgetConfig::preset_overlay_reads();
    const auto again = PanelWidgetConfig::load_preset_overlay("testp");
    CHECK(PanelWidgetConfig::preset_overlay_reads() == reads);
    CHECK(again.get() == first.get());

    const json changed{{"anchors", {{{"id", "clock"}, {"remove", true}}}}};
    write_seed(guard, "testp", changed);
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) +
                                               std::chrono::seconds(7));
    const auto reloaded = PanelWidgetConfig::load_preset_overlay("testp");
    CHECK(PanelWidgetConfig::preset_overlay_reads() == reads + 1);
    CHECK(*reloaded == changed);
    // A tree a caller still holds survives the reload.
    CHECK((*first)["anchors"][0]["id"] == "tips");
    PanelWidgetConfig::load_preset_overlay("testp");
    CHECK(PanelWidgetConfig::preset_overlay_reads() == reads + 1);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "a preset without a seed file falls back to the base",
                 "[panel_widget_config][preset]") {
    ConfigDirGuard guard("preset-no-seed");
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);

    setup_empty_config();
    PanelWidgetConfig plain("home", config);
    plain.load();
    const json base = get_saved_page0_widgets();

    setup_empty_config();
    ConfigTestAccess::data(config)["printers"]["default"]["preset"] = "no_such_seed";
    PanelWidgetConfig wc("home", config);
    wc.load();

    CHECK(get_saved_page0_widgets() == base);
}

namespace {

/// Seed files directly under @p root/<preset>/home.json that were examined, and
/// the problems found: a seed that is not an object, one carrying a page list,
/// or one naming a widget no default may place.
struct SeedAudit {
    int examined = 0;
    std::vector<std::string> problems;
};

SeedAudit audit_seed_dir(const std::filesystem::path& root) {
    SeedAudit audit;
    std::error_code ec;
    for (const auto& dir : std::filesystem::directory_iterator(root, ec)) {
        std::ifstream in(dir.path() / "home.json");
        if (!in.is_open()) {
            continue;
        }
        ++audit.examined;
        const std::string name = dir.path().filename().string();
        const json seed = json::parse(in, nullptr, false);
        if (!seed.is_object()) {
            audit.problems.push_back(name + ": not a JSON object");
            continue;
        }
        if (seed.contains("pages")) {
            audit.problems.push_back(name + ": carries a page list");
        }
        const std::string text = seed.dump();
        for (const char* id : {"toolhead", "host_health", "exclude_map"}) {
            if (text.find(std::string("\"") + id + "\"") != std::string::npos) {
                audit.problems.push_back(name + ": names " + id);
            }
        }
    }
    return audit;
}

} // namespace

TEST_CASE_METHOD(PanelWidgetConfigFixture, "preset seeds: the seed audit examines and can fail",
                 "[panel_widget_config][preset]") {
    ConfigDirGuard guard("preset-audit");
    write_seed(guard, "good", json{{"anchors", {{{"id", "tips"}, {"remove", true}}}}});
    write_seed(guard, "paged", json{{"pages", json::array()}});
    write_seed(guard, "banned", json{{"anchors", {{{"id", "toolhead"}}}}});

    const SeedAudit audit = audit_seed_dir(guard.dir / "panel_widgets");
    CHECK(audit.examined == 3);
    CHECK(audit.problems.size() == 2);
    CHECK(std::count_if(audit.problems.begin(), audit.problems.end(),
                        [](const std::string& p) { return p.rfind("good", 0) == 0; }) == 0);
}

TEST_CASE("preset seeds: shipped seeds are overlays that place no excluded widget",
          "[panel_widget_config][preset]") {
    const std::filesystem::path root =
        std::filesystem::path(find_readable("default_layout.json")).parent_path() / "panel_widgets";
    // No seed ships today, so there may be nothing to examine; each one that
    // does ship is held to the same audit as the fixtures above.
    const SeedAudit audit = audit_seed_dir(root);
    for (const auto& problem : audit.problems) {
        FAIL_CHECK(problem);
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "a seeded unedited default keeps its overlay through every rebuild path",
                 "[panel_widget_config][preset]") {
    ConfigDirGuard guard("preset-overlay-paths");
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);

    auto page_entries = [](const PanelWidgetConfig& wc) {
        std::vector<PanelWidgetEntry> placed;
        for (const auto& e : wc.page_entries(0)) {
            if (e.enabled && e.has_grid_position()) {
                placed.push_back(e);
            }
        }
        return placed;
    };
    auto has = [](const std::vector<PanelWidgetEntry>& v, const char* id) {
        return std::any_of(v.begin(), v.end(),
                           [&](const PanelWidgetEntry& e) { return e.id == id; });
    };
    auto expect_overlay = [&](const PanelWidgetConfig& wc) {
        const auto placed = page_entries(wc);
        CHECK_FALSE(has(placed, "tips"));
        REQUIRE(has(placed, "clock"));
        const auto it = std::find_if(placed.begin(), placed.end(),
                                     [](const PanelWidgetEntry& e) { return e.id == "clock"; });
        CHECK(it->col == 8);
        CHECK(it->colspan == 4);
    };

    // The same steps without a seed: tips is placed and the clock is not, so
    // the overlay assertions below cannot pass on an empty layout.
    {
        setup_empty_config();
        PanelWidgetConfig plain("home", config);
        plain.load();
        plain.apply_pending_anchors(24, 12);
        const auto placed = page_entries(plain);
        REQUIRE(has(placed, "tips"));
        REQUIRE_FALSE(has(placed, "clock"));
    }

    write_seed(guard, "testp", kOverlaySeed);
    setup_empty_config();
    ConfigTestAccess::data(config)["printers"]["default"]["preset"] = "testp";
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.is_unedited_default());
    expect_overlay(wc);

    SECTION("apply_pending_anchors") {
        REQUIRE(wc.has_pending_anchors());
        wc.apply_pending_anchors(24, 12);
        CHECK_FALSE(wc.has_pending_anchors());
        CHECK(wc.is_unedited_default());
        expect_overlay(wc);
    }

    SECTION("reresolve_default_if_changed on a grid change") {
        wc.apply_pending_anchors(24, 12);
        REQUIRE(wc.reresolve_default_if_changed(20, 12));
        CHECK(wc.is_unedited_default());
        CHECK_FALSE(has(page_entries(wc), "tips"));
    }

    SECTION("reset_to_defaults") {
        wc.apply_pending_anchors(24, 12);
        wc.add_page();
        REQUIRE_FALSE(wc.is_unedited_default());
        wc.reset_to_defaults();
        CHECK(wc.is_unedited_default());
        expect_overlay(wc);
        wc.apply_pending_anchors(24, 12);
        expect_overlay(wc);
    }
}

// ============================================================================
// A layout that names no coordinate system
// ============================================================================
//
// From config version 24 on, a panel node that places widgets also says which
// system its numbers count in: `grid` for tracks, `layout_units` for pre-v22
// cells. A node carrying `pages` with a placed widget and neither key is one a
// build older than its own version stamp re-serialized — those builds write
// `pages`/`main_page_index`/`next_page_id` and drop everything else, leaving
// coordinates behind with nothing to read them against.
//
// Detection is conservative in one direction: a missed case scrambles a
// dashboard, but a false one resets a dashboard that was fine. The negatives
// below are the shapes that legitimately reach load() and must be left alone.

namespace {

/// A widget the defaults builder turns ON, so a saved layout that turns it OFF
/// reads differently depending on whether the layout survived the load.
std::string an_enabled_default_widget() {
    for (const auto& e : PanelWidgetConfig::build_default_grid()) {
        if (e.enabled) {
            return e.id;
        }
    }
    return {};
}

/// One home node in the pages format, placing @p id at (3,5) and hiding it.
json placed_home_node(const std::string& id) {
    json widgets = json::array({json{
        {"id", id}, {"enabled", false}, {"col", 3}, {"row", 5}, {"colspan", 2}, {"rowspan", 2}}});
    return json{{"main_page_index", 0},
                {"next_page_id", 1},
                {"pages", json::array({json{{"id", "main"}, {"widgets", widgets}}})}};
}

} // namespace

TEST_CASE("PanelWidgetConfig: has_uninterpretable_coordinates flags only a placed, unnamed "
          "layout at version 24 or later",
          "[panel_widget_config][unnamed_units]") {
    struct Row {
        const char* what;
        json node;
        int version;
        bool detected;
    };
    const json placed = placed_home_node("printer_image");
    json with_grid = placed;
    with_grid["grid"] = "12x24";
    json with_cells = placed;
    with_cells["layout_units"] = "cells_v21";
    with_cells["legacy_rows"] = 14;
    json unplaced = placed;
    unplaced["pages"][0]["widgets"][0].erase("col");
    unplaced["pages"][0]["widgets"][0].erase("row");
    const Row rows[] = {
        {"placed, unnamed, version 24", placed, 24, true},
        // The keys stay mandatory as the version climbs: a later stamp is no escape.
        {"placed, unnamed, version 30", placed, 30, true},
        {"a grid signature names the tracks", with_grid, 24, false},
        {"a cells tag names pre-v22 units", with_cells, 24, false},
        {"below version 24 nothing is named, as expected", placed, 23, false},
        {"version 0", placed, 0, false},
        {"a fresh install has no panel node (null)", json(), 24, false},
        {"a fresh install has no panel node (object)", json::object(), 24, false},
        {"a fresh install has no panel node (array)", json::array(), 24, false},
        {"a layout that places nothing has no coordinate to misread", unplaced, 24, false},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        CHECK(PanelWidgetConfig::has_uninterpretable_coordinates(row.node, row.version) ==
              row.detected);
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a layout naming no coordinate system re-anchors from defaults",
                 "[panel_widget_config][unnamed_units]") {
    const std::string id = an_enabled_default_widget();
    REQUIRE_FALSE(id.empty());

    setup_with_widgets(placed_home_node(id));
    get_data()["config_version"] = 24;

    PanelWidgetConfig wc("home", config);
    wc.load();

    // The saved layout is gone, replaced by the defaults: the widget it hid is
    // back on, and the anchors are pending because no grid has been measured.
    CHECK(wc.is_enabled(id));
    CHECK(wc.has_pending_anchors());
    CHECK(wc.grid_signature().empty());

    // ...and the rebuild is what a later load reads, not a one-run repair.
    wc.save();
    auto root = get_saved_root();
    REQUIRE(root.is_object());
    REQUIRE(root.contains("default"));
    CHECK(root["default"].value("signature", "x").empty());
    CHECK_FALSE(root.contains("anchors"));
    bool found = false;
    for (const auto& w : get_saved_page0_widgets()) {
        if (w.value("id", "") == id) {
            found = true;
            CHECK(w.value("enabled", false));
        }
    }
    CHECK(found);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a named or pre-24 layout keeps what it saved",
                 "[panel_widget_config][unnamed_units]") {
    const std::string id = an_enabled_default_widget();
    REQUIRE_FALSE(id.empty());
    json node = placed_home_node(id);
    int version = 24;
    std::string grid;
    bool legacy_units = false;
    SECTION("a grid signature") {
        node["grid"] = "12x24";
        grid = "12x24";
    }
    SECTION("a cells tag") {
        node["layout_units"] = "cells_v21";
        node["legacy_rows"] = 14;
        legacy_units = true;
    }
    SECTION("below version 24") {
        version = 23;
    }
    setup_with_widgets(node);
    get_data()["config_version"] = version;

    PanelWidgetConfig wc("home", config);
    wc.load();

    // The widget the saved layout hid stays hidden: the layout survived.
    CHECK_FALSE(wc.is_enabled(id));
    CHECK_FALSE(wc.has_pending_anchors());
    if (!grid.empty()) {
        CHECK(wc.grid_signature() == grid);
    }
    if (legacy_units) {
        CHECK(wc.has_legacy_units());
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a fresh install at version 24 still seeds defaults",
                 "[panel_widget_config][unnamed_units]") {
    setup_empty_config();
    get_data()["config_version"] = 24;

    PanelWidgetConfig wc("home", config);
    wc.load();

    CHECK(wc.entries().size() == default_grid_widget_count());
    CHECK(wc.has_pending_anchors());
}

// ============================================================================
// A node that outlived the build that wrote it
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: save keeps panel keys this build does not know",
                 "[panel_widget_config][migration][1460]") {
    // A newer build keeps its layout state as siblings of "pages" in this same
    // node, and a config it wrote reaches this build whenever someone rolls
    // back an update channel. Nothing above this level protects the subtree:
    // the version guard leaves the document unmigrated, but save() runs on
    // roughly the first populate of every panel, so a node assigned wholesale
    // here drops those keys with no user action at all
    // (prestonbrown/helixscreen#1460).
    json node = placed_home_node("printer_image");
    node["grid"] = "12x24";
    node["future_layout_v99"] = json{{"rails", json::array({1, 2, 3})}, {"origin", "centre"}};
    node["future_scalar"] = 7;
    setup_with_widgets(node);

    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();

    // Each key is proven present before it is read: operator[] on a missing key
    // of a const object aborts, which would bury the name that went missing.
    const json saved = get_saved_root();
    REQUIRE(saved.contains("future_layout_v99"));
    CHECK(saved["future_layout_v99"]["origin"] == "centre");
    CHECK(saved["future_layout_v99"]["rails"].size() == 3);
    REQUIRE(saved.contains("future_scalar"));
    CHECK(saved["future_scalar"] == 7);

    // The keys this build owns still come from its own state, not from what
    // the node happened to arrive holding.
    REQUIRE(saved.contains("grid"));
    CHECK(saved["grid"] == "12x24");
    REQUIRE(saved.contains("pages"));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: save drops an owned key its state no longer justifies",
                 "[panel_widget_config][migration][1460]") {
    // Editing the node in place is only correct while every key this build owns
    // is still rewritten from member state. load() ignores a "parked_grids"
    // that is not an object, so member state says there are none, and a copy
    // left in the node would outlive the condition that wrote it.
    json node = placed_home_node("printer_image");
    node["grid"] = "12x24";
    node["parked_grids"] = "not-an-object";
    setup_with_widgets(node);

    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();

    CHECK_FALSE(get_saved_root().contains("parked_grids"));
    // The rest of the node is still written, so the absence above is a dropped
    // key and not a save that did nothing.
    CHECK(get_saved_root().contains("pages"));
}

// ============================================================================
// Unknown widget ids: a plugin's placement outlives the plugin
// ============================================================================
//
// A plugin widget id (gone-plug__tile) has a definition only while its plugin
// is loaded. The layout must keep the entry verbatim across any number of
// saves, so the widget returns to its cell when the plugin comes back.

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "an unknown widget id survives save, page removal and a parked-grid restore",
                 "[panel_widget][widget_config]") {
    const json unknown = {{"id", "gone-plug__tile"},
                          {"enabled", true},
                          {"col", 2},
                          {"row", 0},
                          {"colspan", 2},
                          {"rowspan", 2}};
    // How many saved entries, on any page, are exactly the unknown one.
    const auto saved_copies = [this, &unknown]() {
        int n = 0;
        const json root = get_saved_root();
        for (const auto& page : root["pages"]) {
            for (const auto& item : page["widgets"]) {
                n += item == unknown;
            }
        }
        return n;
    };

    SECTION("load and save, twice") {
        setup_with_pages(
            {{"main", json::array({{{"id", "print_status"}, {"enabled", true}}, unknown})}});
        PanelWidgetConfig wc("home", config);
        wc.load();
        for (const auto& e : wc.page_entries(0)) {
            CHECK(e.id != "gone-plug__tile"); // not rendered
        }
        wc.save();
        CHECK(saved_copies() == 1);

        PanelWidgetConfig again("home", config);
        again.load();
        again.save();
        CHECK(saved_copies() == 1); // kept once, not duplicated per save
    }
    SECTION("removing the page that holds it") {
        setup_with_pages({{"main", json::array()}, {"extra", json::array({unknown})}});
        PanelWidgetConfig wc("home", config);
        wc.load();
        REQUIRE(wc.remove_page(1));
        wc.save();
        CHECK(saved_copies() == 1);
    }
    SECTION("restoring a parked layout") {
        setup_with_pages(
            {{"main", json::array({{{"id", "print_status"}, {"enabled", true}}, unknown})}});
        // Stamp the grid the layout was arranged on, or the first switch adopts
        // it without parking anything.
        get_printer_data()["panel_widgets"]["home"]["grid"] = "24x12";
        PanelWidgetConfig wc("home", config);
        wc.load();
        wc.switch_to_grid(12, 6);  // parks the 24x12 arrangement, seeds 12x6 from it
        wc.switch_to_grid(24, 12); // restores the parked 24x12 arrangement
        CHECK(saved_copies() == 1);
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a new printer's default is written at READY, not at load",
                 "[panel_widget_config][unnamed_units][multi-printer]") {
    // A default built at load can hold the previous printer's capability values after a
    // live switch, so nothing is saved until the READY resolve; the next load must then
    // read what that wrote.
    setup_empty_config();
    get_data()["config_version"] = 30;
    PanelWidgetConfig first("home", config);
    first.load();
    CHECK(first.has_pending_anchors());
    CHECK_FALSE(get_printer_data().contains("panel_widgets"));

    REQUIRE(first.reresolve_default_if_changed(12, 8, /*persist=*/true));
    first.flush_pending_save();
    const json saved = get_printer_data()["panel_widgets"]["home"];
    REQUIRE(saved.contains("default"));
    REQUIRE_FALSE(saved["default"].value("signature", "").empty());
    REQUIRE(saved.contains("grid"));

    CHECK_FALSE(PanelWidgetConfig::has_uninterpretable_coordinates(saved, 30));
}

// ============================================================================
// Save omits only what the reader re-appends
// ============================================================================

namespace {
const PanelWidgetDef* first_def_where(bool default_enabled, bool multi_instance) {
    for (const auto& def : get_all_widget_defs()) {
        if (def.default_enabled == default_enabled && def.multi_instance == multi_instance) {
            return &def;
        }
    }
    return nullptr;
}

json widget_json(const std::string& id, bool enabled, int col, int row, int colspan, int rowspan,
                 const json& config = json()) {
    json item = {{"id", id},   {"enabled", enabled}, {"col", col},
                 {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
    if (!config.is_null()) {
        item["config"] = config;
    }
    return item;
}
} // namespace

// Each case breaks exactly one condition of the omitted shape (off by default,
// disabled, unplaced, unconfigured, registry spans, base id, first page), so the
// entry carries information and must survive a save and reload field for field.
TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: save keeps every entry that differs from the appended default",
                 "[panel_widget][widget_config]") {
    const auto* off_def = first_def_where(false, false);
    const auto* on_def = first_def_where(true, false);
    const auto* multi_def = first_def_where(false, true);
    REQUIRE(off_def != nullptr);
    REQUIRE(on_def != nullptr);
    REQUIRE(multi_def != nullptr);
    const std::string off = off_def->id;
    const int cs = off_def->colspan;
    const int rs = off_def->rowspan;

    struct Case {
        const char* why;
        json entry;
        size_t page;
    };
    const std::vector<Case> cases = {
        {"enabled, awaiting auto-place", widget_json(off, true, -1, -1, cs, rs), 0},
        {"column only", widget_json(off, false, 0, -1, cs, rs), 0},
        {"row only", widget_json(off, false, -1, 0, cs, rs), 0},
        {"configured", widget_json(off, false, -1, -1, cs, rs, {{"k", "v"}}), 0},
        {"non-default colspan", widget_json(off, false, -1, -1, cs + 1, rs), 0},
        {"non-default rowspan", widget_json(off, false, -1, -1, cs, rs + 1), 0},
        {"on by default", widget_json(on_def->id, false, -1, -1, on_def->colspan, on_def->rowspan),
         0},
        {"multi-instance id",
         widget_json(std::string(multi_def->id) + ":3", false, -1, -1, multi_def->colspan,
                     multi_def->rowspan),
         0},
        {"second page", widget_json(off, false, -1, -1, cs, rs), 1},
    };

    for (const auto& c : cases) {
        CAPTURE(c.why);
        const std::string id = c.entry["id"].get<std::string>();
        json page0 = json::array();
        json page1 = json::array();
        (c.page == 0 ? page0 : page1).push_back(c.entry);
        setup_with_pages({{"main", page0}, {"p2", page1}});

        PanelWidgetConfig wc1("home", config);
        wc1.load();
        wc1.save();

        const json saved = get_saved_root()["pages"][c.page]["widgets"];
        auto in_saved = std::find_if(saved.begin(), saved.end(),
                                     [&](const json& item) { return item["id"] == id; });
        REQUIRE(in_saved != saved.end());

        PanelWidgetConfig wc2("home", config);
        wc2.load();
        const auto& entries = wc2.page_entries(c.page);
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const PanelWidgetEntry& e) { return e.id == id; });
        REQUIRE(it != entries.end());
        CHECK(it->enabled == c.entry["enabled"].get<bool>());
        CHECK(it->col == c.entry["col"].get<int>());
        CHECK(it->row == c.entry["row"].get<int>());
        CHECK(it->colspan == c.entry["colspan"].get<int>());
        CHECK(it->rowspan == c.entry["rowspan"].get<int>());
        CHECK(it->config == c.entry.value("config", json::object()));
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: save omits a default-off widget left at its appended default",
                 "[panel_widget][widget_config]") {
    const auto* off_def = first_def_where(false, false);
    REQUIRE(off_def != nullptr);
    setup_with_pages({{"main", json::array({widget_json(off_def->id, false, -1, -1,
                                                        off_def->colspan, off_def->rowspan)})}});
    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();
    const auto saved = get_saved_page0_widgets();
    CHECK(std::none_of(saved.begin(), saved.end(),
                       [&](const json& item) { return item["id"] == off_def->id; }));
}

// ============================================================================
// The default block: which arrangement the defaults builder produced
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reset stamps a default block whose hash matches",
                 "[panel_widget_config][default_block]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.add_page();
    REQUIRE(wc.place_entry("clock", 1, 0, 0, 2, 2) >= 0);
    REQUIRE_FALSE(wc.is_unedited_default());

    wc.reset_to_defaults();
    CHECK(wc.is_unedited_default());
    CHECK(wc.has_pending_anchors());
    CHECK(wc.default_signature().empty());

    wc.save();
    const json saved = get_saved_root();
    REQUIRE(saved.contains("default"));
    REQUIRE(saved["default"].is_object());
    CHECK(saved["default"].value("hash", "") == helix::layout::arrangement_hash(saved));
    CHECK_FALSE(saved.contains("anchors"));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: applying the anchors stamps the signature and hash",
                 "[panel_widget_config][default_block]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.has_pending_anchors());

    wc.apply_pending_anchors(12, 8);
    CHECK_FALSE(wc.has_pending_anchors());
    CHECK_FALSE(wc.default_signature().empty());
    CHECK(wc.default_signature().find("|12|8|") != std::string::npos);
    CHECK(wc.is_unedited_default());

    // A reload reads the block back unchanged.
    PanelWidgetConfig again("home", config);
    again.load();
    CHECK(again.default_signature() == wc.default_signature());
    CHECK(again.is_unedited_default());

    // Moving a placed widget makes the layout the user's.
    const auto& page = again.page_entries(0);
    auto placed = std::find_if(page.begin(), page.end(),
                               [](const PanelWidgetEntry& e) { return e.is_placed(); });
    REQUIRE(placed != page.end());
    const std::string id = placed->id;
    again.place_entry(id, 0, placed->col, placed->row + 1, placed->colspan, placed->rowspan);
    CHECK_FALSE(again.is_unedited_default());
    // ...while per-widget config is outside the hash.
    PanelWidgetConfig third("home", config);
    third.load();
    third.set_widget_config(id, json{{"style", "x"}});
    CHECK(third.is_unedited_default());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: the older pending tag reads as a pending default block",
                 "[panel_widget_config][default_block]") {
    json node = placed_home_node("printer_image");
    node["anchors"] = "pending";
    setup_with_widgets(node);
    get_data()["config_version"] = 30;

    PanelWidgetConfig wc("home", config);
    wc.load();
    CHECK(wc.has_pending_anchors());
    CHECK(wc.default_signature().empty());

    wc.save();
    const json saved = get_saved_root();
    CHECK_FALSE(saved.contains("anchors"));
    REQUIRE(saved.contains("default"));
    CHECK(saved["default"].value("signature", "x").empty());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a default block saved before any grid is read as written",
                 "[panel_widget_config][default_block][unnamed_units]") {
    const std::string id = an_enabled_default_widget();
    REQUIRE_FALSE(id.empty());

    // The shape a pending default saves: placed widgets, a default block, no
    // grid. The block names the coordinates, so the layout loads as written.
    json node = placed_home_node(id);
    node["default"] = json{{"signature", ""}, {"hash", ""}};
    REQUIRE_FALSE(node.contains("grid"));
    setup_with_widgets(node);
    get_data()["config_version"] = 30;

    CHECK_FALSE(PanelWidgetConfig::has_uninterpretable_coordinates(node, 30));

    PanelWidgetConfig wc("home", config);
    wc.load();
    CHECK(wc.has_pending_anchors());
    CHECK_FALSE(wc.is_enabled(id));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: switching grids parks the default block and seeds none",
                 "[panel_widget_config][default_block]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.apply_pending_anchors(12, 8);
    const std::string sig = wc.default_signature();
    REQUIRE_FALSE(sig.empty());

    // The new grid carries no default block of its own; the old one is parked
    // with its grid.
    wc.switch_to_grid(10, 8);
    CHECK(wc.default_signature().empty());
    CHECK_FALSE(wc.has_pending_anchors());
    CHECK_FALSE(wc.is_unedited_default());
    CHECK_FALSE(get_saved_root().contains("default"));
    const json parked = get_saved_root()["parked_grids"]["12x8"];
    REQUIRE(parked.contains("default"));
    CHECK(parked["default"].value("signature", "") == sig);

    // Switching back restores it.
    wc.switch_to_grid(12, 8);
    CHECK(wc.default_signature() == sig);
    CHECK(wc.is_unedited_default());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: a layout without a default block is not an unedited default",
                 "[panel_widget_config][default_block]") {
    json node = placed_home_node("printer_image");
    node["grid"] = "12x8";
    setup_with_widgets(node);
    PanelWidgetConfig wc("home", config);
    wc.load();
    CHECK_FALSE(wc.is_unedited_default());
    CHECK(wc.default_signature().empty());
    CHECK_FALSE(wc.has_pending_anchors());
}

TEST_CASE("PanelWidgetRegistry: screen-scoped widgets are exactly the six",
          "[panel_widget][widget_scope]") {
    std::set<std::string> screen;
    for (const auto& def : get_all_widget_defs()) {
        if (def.scope == WidgetScope::Screen) {
            screen.insert(def.id);
        }
    }
    const std::set<std::string> expected = {"network", "clock",         "lock",
                                            "tips",    "notifications", "shutdown"};
    REQUIRE(screen == expected);
    REQUIRE(is_screen_scoped("clock"));
    REQUIRE_FALSE(is_screen_scoped("host_health"));
    REQUIRE_FALSE(is_screen_scoped("no_such_widget"));
}

// ============================================================================
// import_layout: a layout authored for another grid
// ============================================================================

namespace {

json import_widget(const char* id, int col, int row, int colspan, int rowspan,
                   const json& config = json::object()) {
    json w{{"id", id},   {"enabled", true},    {"col", col},
           {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
    if (!config.empty()) {
        w["config"] = config;
    }
    return w;
}

helix::home_layout::ParsedLayout make_import(int cols, int rows, const json& widgets,
                                             int main_page_index = 0) {
    json payload{{"pages", json::array({json{{"id", "main"}, {"widgets", widgets}}})},
                 {"main_page_index", main_page_index}};
    return {cols, rows, payload};
}

/// A 12x8 arrangement holding a widget with per-widget config.
json source_widgets_12x8() {
    return json::array({import_widget("printer_image", 0, 0, 4, 4),
                        import_widget("print_status", 4, 0, 6, 4, json{{"style", "compact"}}),
                        import_widget("temperature", 0, 4, 2, 2),
                        import_widget("fan", 2, 4, 2, 2)});
}

std::vector<LegacyPlacement> placements_of(const json& widgets) {
    std::vector<LegacyPlacement> out;
    for (const auto& w : widgets) {
        out.push_back({w["id"].get<std::string>(), w["col"].get<int>(), w["row"].get<int>(),
                       w["colspan"].get<int>(), w["rowspan"].get<int>()});
    }
    return out;
}

const PanelWidgetEntry* find_entry(const PanelWidgetConfig& wc, const std::string& id) {
    for (const auto& page : wc.pages()) {
        for (const auto& e : page.widgets) {
            if (e.id == id) {
                return &e;
            }
        }
    }
    return nullptr;
}

/// Everything the pages hold, for an exact before/after comparison.
json pages_json(const PanelWidgetConfig& wc) {
    json out = json::array();
    for (const auto& page : wc.pages()) {
        json widgets = json::array();
        for (const auto& e : page.widgets) {
            widgets.push_back({e.id, e.enabled, e.config, e.col, e.row, e.colspan, e.rowspan});
        }
        out.push_back({page.id, widgets, page.retained});
    }
    return json{{"pages", out}, {"main", wc.main_page_index()}};
}

/// A client holding its own default on 16x10, now showing 12x8: one parked grid.
void prepare_client_with_parked_grid(PanelWidgetConfig& wc) {
    wc.load();
    wc.apply_pending_anchors(16, 10);
    wc.switch_to_grid(12, 8);
}

} // namespace

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "import_layout: same grid copies placements one-to-one and retains unknown ids",
                 "[widget_config][import_layout]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    prepare_client_with_parked_grid(wc);

    const json src = source_widgets_12x8();
    json with_unknown = src;
    with_unknown.push_back(import_widget("no_such_widget", 10, 0, 2, 2));
    REQUIRE(wc.import_layout(make_import(12, 8, with_unknown), 12, 8));
    CHECK(wc.grid_signature() == "12x8");
    for (const auto& w : src) {
        const std::string id = w["id"].get<std::string>();
        INFO("widget " << id);
        const auto* e = find_entry(wc, id);
        REQUIRE(e != nullptr);
        CHECK(e->enabled);
        CHECK(e->col == w["col"].get<int>());
        CHECK(e->row == w["row"].get<int>());
        CHECK(e->colspan == w["colspan"].get<int>());
        CHECK(e->rowspan == w["rowspan"].get<int>());
        CHECK(e->config == w.value("config", json::object()));
    }
    CHECK(get_saved_root().value("grid", "") == "12x8");

    // An id with no definition is kept aside for its plugin, not placed.
    CHECK(find_entry(wc, "no_such_widget") == nullptr);
    const json& retained = wc.pages()[0].retained;
    REQUIRE(retained.is_array());
    REQUIRE(retained.size() == 1);
    CHECK(retained[0]["id"] == "no_such_widget");
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "import_layout: 12x8 to 10x8 and to 16x10 port through layout_port",
                 "[widget_config][import_layout]") {
    const auto [cols, rows] = GENERATE(std::pair<int, int>{10, 8}, std::pair<int, int>{16, 10});
    INFO("target " << cols << "x" << rows);
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    prepare_client_with_parked_grid(wc);

    const json src = source_widgets_12x8();
    REQUIRE(wc.import_layout(make_import(12, 8, src), cols, rows));
    CHECK(wc.grid_signature() == std::to_string(cols) + "x" + std::to_string(rows));

    const auto expected = port_legacy_layout(placements_of(src), 12, 8, cols, rows);
    for (const auto& p : expected) {
        INFO("widget " << p.id);
        const auto* e = find_entry(wc, p.id);
        REQUIRE(e != nullptr);
        CHECK(e->col == (p.seated ? p.col : -1));
        CHECK(e->row == (p.seated ? p.row : -1));
        if (p.seated) {
            CHECK(e->colspan == p.colspan);
            CHECK(e->rowspan == p.rowspan);
        }
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "import_layout: the import is the user's layout",
                 "[widget_config][import_layout]") {
    const auto [cols, rows] = GENERATE(std::pair<int, int>{12, 8}, std::pair<int, int>{10, 8});
    INFO("target " << cols << "x" << rows);
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.has_pending_anchors());

    REQUIRE(wc.import_layout(make_import(12, 8, source_widgets_12x8()), cols, rows));
    CHECK_FALSE(wc.is_unedited_default());
    CHECK_FALSE(wc.has_pending_anchors());
    CHECK(wc.default_signature().empty());
    CHECK_FALSE(wc.reresolve_default_if_changed(cols, rows));
    CHECK_FALSE(get_saved_root().contains("default"));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "import_layout: parked grids are cleared",
                 "[widget_config][import_layout]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    prepare_client_with_parked_grid(wc);
    REQUIRE(get_saved_root().contains("parked_grids"));

    const json src = source_widgets_12x8();
    REQUIRE(wc.import_layout(make_import(12, 8, src), 12, 8));
    CHECK_FALSE(get_saved_root().contains("parked_grids"));

    wc.switch_to_grid(16, 10);
    const auto expected = port_legacy_layout(placements_of(src), 12, 8, 16, 10);
    for (const auto& p : expected) {
        INFO("widget " << p.id);
        const auto* e = find_entry(wc, p.id);
        REQUIRE(e != nullptr);
        CHECK(e->enabled);
        CHECK(e->col == (p.seated ? p.col : -1));
        CHECK(e->row == (p.seated ? p.row : -1));
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "import_layout: client screen-scoped defaults are appended unplaced",
                 "[widget_config][import_layout]") {
    std::set<std::string> screen_defaults;
    for (const auto& e : PanelWidgetConfig::build_default_grid()) {
        if (e.enabled && is_screen_scoped(e.id)) {
            screen_defaults.insert(e.id);
        }
    }
    REQUIRE_FALSE(screen_defaults.empty());

    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    prepare_client_with_parked_grid(wc);
    REQUIRE(wc.import_layout(make_import(12, 8, source_widgets_12x8()), 10, 8));

    const auto& main = wc.pages()[wc.main_page_index()].widgets;
    for (const auto& id : screen_defaults) {
        INFO("widget " << id);
        const auto n = std::count_if(main.begin(), main.end(),
                                     [&](const PanelWidgetEntry& e) { return e.id == id; });
        CHECK(n == 1);
        CHECK(wc.is_enabled(id));
        CHECK_FALSE(wc.is_placed(id));
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "import_layout: an unusable import is rejected and changes nothing",
                 "[widget_config][import_layout]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    prepare_client_with_parked_grid(wc);

    const auto pages_before = pages_json(wc);
    const std::string grid_before = wc.grid_signature();
    const std::string signature_before = wc.default_signature();
    const bool unedited_before = wc.is_unedited_default();
    const json saved_before = get_saved_root();
    REQUIRE(saved_before.contains("parked_grids"));

    SECTION("import of only screen-scoped widgets is rejected") {
        CHECK_FALSE(wc.import_layout(
            make_import(12, 8, json::array({import_widget("clock", 0, 0, 4, 2)})), 12, 8));
    }
    SECTION("import whose pages are all empty is rejected") {
        json payload{{"pages", json::array({json{{"id", "main"}, {"widgets", json::array()}},
                                            json{{"id", "p2"}, {"widgets", json::array()}}})},
                     {"main_page_index", 0}};
        CHECK_FALSE(wc.import_layout({12, 8, payload}, 12, 8));
    }
    SECTION("import holding only unknown widgets is rejected") {
        CHECK_FALSE(wc.import_layout(
            make_import(12, 8, json::array({import_widget("no_such_widget", 0, 0, 2, 2)})), 12, 8));
    }
    SECTION("a non-positive grid is rejected") {
        CHECK_FALSE(wc.import_layout(make_import(12, 8, source_widgets_12x8()), 0, 8));
        CHECK_FALSE(wc.import_layout(make_import(12, 8, source_widgets_12x8()), 12, -1));
        CHECK_FALSE(wc.import_layout(make_import(0, 8, source_widgets_12x8()), 12, 8));
    }

    CHECK(pages_json(wc) == pages_before);
    CHECK(wc.grid_signature() == grid_before);
    CHECK(wc.default_signature() == signature_before);
    CHECK(wc.is_unedited_default() == unedited_before);
    CHECK(get_saved_root() == saved_before);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "import_layout: spans larger than the grid and negative origins are clamped, "
                 "never off-grid",
                 "[widget_config][import_layout]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    prepare_client_with_parked_grid(wc);

    const json src =
        json::array({import_widget("print_status", 0, 0, 999, 4),
                     import_widget("temperature", -5, 4, 2, 2), import_widget("fan", 4, 4, 2, 2)});
    REQUIRE(wc.import_layout(make_import(12, 8, src), 12, 8));
    int placed = 0;
    for (const auto& page : wc.pages()) {
        for (const auto& e : page.widgets) {
            if (!e.has_grid_position()) {
                continue;
            }
            INFO("widget " << e.id << " at " << e.col << "," << e.row << " span " << e.colspan
                           << "x" << e.rowspan);
            CHECK(e.col >= 0);
            CHECK(e.row >= 0);
            CHECK(e.col + e.colspan <= 12);
            CHECK(e.row + e.rowspan <= 8);
            ++placed;
        }
    }
    CHECK(placed > 0);
}
