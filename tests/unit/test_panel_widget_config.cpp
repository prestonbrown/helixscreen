// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <set>

#include "../catch_amalgamated.hpp"

using namespace helix;

/// Count of widget defs included in the default grid.
/// build_default_grid() includes ALL defs (multi-instance widgets appear once as base ID).
static size_t default_grid_widget_count() {
    return get_all_widget_defs().size();
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

// ============================================================================
// Registry tests
// ============================================================================

TEST_CASE("PanelWidgetRegistry: returns all widget definitions", "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    // At least the core widgets must exist; exact count grows as widgets are added
    REQUIRE(defs.size() >= 14);
    REQUIRE(defs.size() == widget_def_count());
}

TEST_CASE("PanelWidgetRegistry: all widget IDs are unique", "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    std::set<std::string> ids;
    for (const auto& def : defs) {
        REQUIRE(ids.insert(def.id).second); // insert returns false if duplicate
    }
}

TEST_CASE("PanelWidgetRegistry: can look up widget by ID", "[panel_widget][widget_config]") {
    const auto* def = find_widget_def("temperature");
    REQUIRE(def != nullptr);
    REQUIRE(std::string(def->display_name) == "Nozzle Temperature");
}

TEST_CASE("PanelWidgetRegistry: unknown ID returns nullptr", "[panel_widget][widget_config]") {
    const auto* def = find_widget_def("nonexistent_widget");
    REQUIRE(def == nullptr);
}

TEST_CASE("PanelWidgetRegistry: widget_def_count matches vector size",
          "[panel_widget][widget_config]") {
    REQUIRE(widget_def_count() == get_all_widget_defs().size());
}

// ============================================================================
// Config tests — default behavior
// ============================================================================

TEST_CASE_METHOD(
    PanelWidgetConfigFixture,
    "PanelWidgetConfig: default config produces all widgets with correct enabled state",
    "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto& entries = wc.entries();
    const auto& defs = get_all_widget_defs();
    REQUIRE(entries.size() == default_grid_widget_count());

    // Default grid places anchors first, then remaining widgets.
    // Verify all widgets are present. bed_temperature's enabled state is
    // overridden by build_default_grid() based on breakpoint/AMS, so skip it.
    for (const auto& def : defs) {
        bool found = false;
        for (const auto& entry : entries) {
            if (entry.id == def.id) {
                if (def.id != std::string("bed_temperature")) {
                    // bed_temperature enabled state overridden by build_default_grid()
                    if (def.id != std::string("bed_temperature")) {
                        REQUIRE(entry.enabled == def.default_enabled);
                    }
                }
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

// ============================================================================
// Config tests — load from explicit JSON
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: load from explicit JSON preserves order and enabled state",
                 "[panel_widget][widget_config]") {
    // Include grid coords to prevent pre-grid migration reset
    json widgets = json::array({
        {{"id", "temperature"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "led"}, {"enabled", false}, {"col", 1}, {"row", 0}},
        {{"id", "network"}, {"enabled", true}, {"col", 2}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto& entries = wc.entries();
    // 3 explicit + remaining from registry appended
    REQUIRE(entries.size() == default_grid_widget_count());

    // First 3 should match our explicit order
    REQUIRE(entries[0].id == "temperature");
    REQUIRE(entries[0].enabled == true);
    REQUIRE(entries[1].id == "led");
    REQUIRE(entries[1].enabled == false);
    REQUIRE(entries[2].id == "network");
    REQUIRE(entries[2].enabled == true);

    // Remaining should be appended with their default_enabled value
    for (size_t i = 3; i < entries.size(); ++i) {
        const auto* def = find_widget_def(entries[i].id);
        REQUIRE(def);
        REQUIRE(entries[i].enabled == def->default_enabled);
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
    REQUIRE(saved.size() == default_grid_widget_count());

    // Each entry should have id and enabled
    for (const auto& item : saved) {
        REQUIRE(item.contains("id"));
        REQUIRE(item.contains("enabled"));
        REQUIRE(item["id"].is_string());
        REQUIRE(item["enabled"].is_boolean());
    }

    // The third entry should be disabled
    REQUIRE(saved[2]["enabled"].get<bool>() == false);
}

// ============================================================================
// Config tests — round-trip
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: round-trip load-save-reload preserves state",
                 "[panel_widget][widget_config]") {
    setup_empty_config();

    // First load + customize
    PanelWidgetConfig wc1("home", config);
    wc1.load();
    wc1.set_enabled(1, false);
    wc1.reorder(0, 3);
    wc1.save();

    // Second load from same config
    PanelWidgetConfig wc2("home", config);
    wc2.load();

    const auto& e1 = wc1.entries();
    const auto& e2 = wc2.entries();
    REQUIRE(e1.size() == e2.size());

    for (size_t i = 0; i < e1.size(); ++i) {
        REQUIRE(e1[i].id == e2[i].id);
        REQUIRE(e1[i].enabled == e2[i].enabled);
    }
}

// ============================================================================
// Config tests — reorder
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reorder moves item from index 2 to index 0",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    std::string moved_id = wc.entries()[2].id;
    std::string was_first = wc.entries()[0].id;
    wc.reorder(2, 0);

    REQUIRE(wc.entries()[0].id == moved_id);
    REQUIRE(wc.entries()[1].id == was_first);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reorder moves item from index 0 to index 3",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    std::string moved_id = wc.entries()[0].id;
    std::string was_at_1 = wc.entries()[1].id;
    wc.reorder(0, 3);

    // After removing from 0 and inserting at 3, old index 1 becomes 0
    REQUIRE(wc.entries()[0].id == was_at_1);
    REQUIRE(wc.entries()[3].id == moved_id);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: reorder same index is no-op",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    auto before = wc.entries();
    wc.reorder(2, 2);
    auto after = wc.entries();

    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        REQUIRE(before[i].id == after[i].id);
    }
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: reorder out of bounds is no-op",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    auto before = wc.entries();
    wc.reorder(100, 0);
    auto after = wc.entries();

    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        REQUIRE(before[i].id == after[i].id);
    }
}

// ============================================================================
// Config tests — toggle enabled
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: toggle disable a widget",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.entries()[0].enabled == true);
    wc.set_enabled(0, false);
    REQUIRE(wc.entries()[0].enabled == false);
    REQUIRE(wc.is_enabled(wc.entries()[0].id) == false);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: toggle re-enable a widget",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    wc.set_enabled(0, false);
    REQUIRE(wc.entries()[0].enabled == false);

    wc.set_enabled(0, true);
    REQUIRE(wc.entries()[0].enabled == true);
    REQUIRE(wc.is_enabled(wc.entries()[0].id) == true);
}

// ============================================================================
// Config tests — new widget appended
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: new registry widget gets appended with default_enabled",
                 "[panel_widget][widget_config]") {
    // Save config with only a subset of widgets (include grid coords to prevent pre-grid reset)
    json widgets = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "network"}, {"enabled", false}, {"col", 1}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Should have all registry widgets
    REQUIRE(wc.entries().size() == default_grid_widget_count());

    // First two should match saved order/state
    REQUIRE(wc.entries()[0].id == "shutdown");
    REQUIRE(wc.entries()[0].enabled == true);
    REQUIRE(wc.entries()[1].id == "network");
    REQUIRE(wc.entries()[1].enabled == false);

    // Rest should be appended with their default_enabled value
    for (size_t i = 2; i < wc.entries().size(); ++i) {
        const auto* def = find_widget_def(wc.entries()[i].id);
        REQUIRE(def != nullptr);
        REQUIRE(wc.entries()[i].enabled == def->default_enabled);
    }
}

// ============================================================================
// Config tests — unknown widget IDs dropped
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: unknown widget ID in saved JSON gets dropped",
                 "[panel_widget][widget_config]") {
    // Include grid coords to prevent pre-grid migration reset
    json widgets = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "bogus_widget"}, {"enabled", true}, {"col", 1}, {"row", 0}},
        {{"id", "network"}, {"enabled", false}, {"col", 2}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // bogus_widget should be dropped, so total is still widget_def_count
    REQUIRE(wc.entries().size() == default_grid_widget_count());

    // First should be power, second should be network (bogus skipped)
    REQUIRE(wc.entries()[0].id == "shutdown");
    REQUIRE(wc.entries()[1].id == "network");
}

// ============================================================================
// Config tests — reset to defaults
// ============================================================================

TEST_CASE_METHOD(
    PanelWidgetConfigFixture,
    "PanelWidgetConfig: reset to defaults restores all widgets with correct enabled state",
    "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    // Customize
    wc.set_enabled(0, false);
    wc.reorder(0, 5);

    // Reset
    wc.reset_to_defaults();

    const auto& entries = wc.entries();
    const auto& defs = get_all_widget_defs();
    REQUIRE(entries.size() == default_grid_widget_count());

    // All widgets present with correct enabled state
    for (const auto& def : defs) {
        bool found = false;
        for (const auto& entry : entries) {
            if (entry.id == def.id) {
                // bed_temperature enabled state overridden by build_default_grid()
                if (def.id != std::string("bed_temperature")) {
                    REQUIRE(entry.enabled == def.default_enabled);
                }
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

// ============================================================================
// Config tests — duplicate IDs in saved JSON
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: duplicate IDs in saved JSON keeps only first occurrence",
                 "[panel_widget][widget_config]") {
    // Include grid coords to prevent pre-grid migration reset
    json widgets = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "network"}, {"enabled", true}, {"col", 1}, {"row", 0}},
        {{"id", "shutdown"}, {"enabled", false}, {"col", 2}, {"row", 0}}, // duplicate
        {{"id", "temperature"}, {"enabled", true}, {"col", 3}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.entries().size() == default_grid_widget_count());

    // power should appear once, with enabled=true (first occurrence)
    REQUIRE(wc.entries()[0].id == "shutdown");
    REQUIRE(wc.entries()[0].enabled == true);

    // Verify no duplicate power entries
    int power_count = 0;
    for (const auto& e : wc.entries()) {
        if (e.id == "shutdown") {
            ++power_count;
        }
    }
    REQUIRE(power_count == 1);
}

// ============================================================================
// Config tests — is_enabled convenience
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: is_enabled returns false for unknown ID",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.is_enabled("nonexistent") == false);
}

// ============================================================================
// Config tests — malformed field types
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: malformed field types skip entry but keep others",
                 "[panel_widget][widget_config]") {
    // Include grid coords on valid entries to prevent pre-grid migration reset
    json widgets = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", 42}, {"enabled", true}},         // id is not string
        {{"id", "network"}, {"enabled", "yes"}}, // enabled is not bool
        {{"id", "temperature"}, {"enabled", false}, {"col", 1}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Bad entries skipped, good entries kept, rest appended
    REQUIRE(wc.entries().size() == default_grid_widget_count());
    REQUIRE(wc.entries()[0].id == "shutdown");
    REQUIRE(wc.entries()[0].enabled == true);
    REQUIRE(wc.entries()[1].id == "temperature");
    REQUIRE(wc.entries()[1].enabled == false);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: panel_widgets key is not an array falls back to defaults",
                 "[panel_widget][widget_config]") {
    get_printer_data()["panel_widgets"]["home"] = "corrupted";

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto& defs = get_all_widget_defs();
    REQUIRE(wc.entries().size() == default_grid_widget_count());
    // build_default_grid() reorders anchors first, so check by content not position
    for (const auto& def : defs) {
        bool found = false;
        for (const auto& entry : wc.entries()) {
            if (entry.id == def.id) {
                // bed_temperature enabled state overridden by build_default_grid()
                if (def.id != std::string("bed_temperature")) {
                    REQUIRE(entry.enabled == def.default_enabled);
                }
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

// ============================================================================
// Config tests — set_enabled out of bounds
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: set_enabled out of bounds is a no-op",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    auto before = wc.entries();
    wc.set_enabled(999, false);
    REQUIRE(wc.entries() == before);
}

// ============================================================================
// Registry tests — field completeness
// ============================================================================

TEST_CASE("PanelWidgetRegistry: all defs have non-null required fields",
          "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    for (const auto& def : defs) {
        CAPTURE(def.id);
        REQUIRE(def.id != nullptr);
        REQUIRE(def.display_name != nullptr);
        REQUIRE(def.icon != nullptr);
        REQUIRE(def.description != nullptr);
        // hardware_gate_subject CAN be nullptr (always-available widgets)
    }
}

TEST_CASE("PanelWidgetRegistry: all IDs are non-empty strings", "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    for (const auto& def : defs) {
        REQUIRE(std::string_view(def.id).size() > 0);
        REQUIRE(std::string_view(def.display_name).size() > 0);
        REQUIRE(std::string_view(def.icon).size() > 0);
        REQUIRE(std::string_view(def.description).size() > 0);
    }
}

TEST_CASE("PanelWidgetRegistry: can find every registered widget by ID",
          "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    for (const auto& def : defs) {
        const auto* found = find_widget_def(def.id);
        REQUIRE(found != nullptr);
        REQUIRE(found->id == std::string_view(def.id));
    }
}

TEST_CASE("PanelWidgetRegistry: hardware_gate_hint consistent with hardware_gate_subject",
          "[panel_widget][widget_config]") {
    const auto& defs = get_all_widget_defs();
    for (const auto& def : defs) {
        CAPTURE(def.id);
        if (def.hardware_gate_subject != nullptr) {
            REQUIRE(def.hardware_gate_hint != nullptr);
        } else {
            REQUIRE(def.hardware_gate_hint == nullptr);
        }
    }
}

TEST_CASE("PanelWidgetRegistry: known hardware-gated widgets have gate subjects",
          "[panel_widget][widget_config]") {
    // These widgets require specific hardware
    const char* gated[] = {"power_device", "ams",      "led",       "humidity",
                           "width_sensor", "filament", "thermistor"};
    for (const auto* id : gated) {
        CAPTURE(id);
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        REQUIRE(def->hardware_gate_subject != nullptr);
    }
}

TEST_CASE("PanelWidgetRegistry: always-available widgets have no gate subject",
          "[panel_widget][widget_config]") {
    const char* always[] = {"network", "firmware_restart", "temperature", "notifications"};
    for (const auto* id : always) {
        CAPTURE(id);
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        REQUIRE(def->hardware_gate_subject == nullptr);
    }
}

// ============================================================================
// Config tests — reorder edge cases
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: reorder to last position works",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    size_t last = wc.entries().size() - 1;
    std::string moved_id = wc.entries()[0].id;
    wc.reorder(0, last);

    REQUIRE(wc.entries()[last].id == moved_id);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: reorder from last to first works",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    size_t last = wc.entries().size() - 1;
    std::string moved_id = wc.entries()[last].id;
    wc.reorder(last, 0);

    REQUIRE(wc.entries()[0].id == moved_id);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: reorder preserves enabled state of moved item",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    wc.set_enabled(3, false);
    std::string moved_id = wc.entries()[3].id;
    wc.reorder(3, 0);

    REQUIRE(wc.entries()[0].id == moved_id);
    REQUIRE(wc.entries()[0].enabled == false);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: multiple reorders produce correct final order",
                 "[panel_widget][widget_config]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    // Capture IDs for first 4
    std::string id0 = wc.entries()[0].id;
    std::string id1 = wc.entries()[1].id;
    std::string id2 = wc.entries()[2].id;
    std::string id3 = wc.entries()[3].id;

    // Move 0→2, then 3→1
    wc.reorder(0, 2); // [id1, id2, id0, id3, ...]
    wc.reorder(3, 1); // [id1, id3, id2, id0, ...]

    REQUIRE(wc.entries()[0].id == id1);
    REQUIRE(wc.entries()[1].id == id3);
    REQUIRE(wc.entries()[2].id == id2);
    REQUIRE(wc.entries()[3].id == id0);
}

// ============================================================================
// Config tests — save-load round trip with reorder
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

    REQUIRE(wc1.entries().size() == wc2.entries().size());
    for (size_t i = 0; i < wc1.entries().size(); ++i) {
        CAPTURE(i);
        REQUIRE(wc1.entries()[i].id == wc2.entries()[i].id);
        REQUIRE(wc1.entries()[i].enabled == wc2.entries()[i].enabled);
    }
}

// ============================================================================
// Config tests — empty array in JSON
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: empty array in JSON falls back to defaults",
                 "[panel_widget][widget_config]") {
    setup_with_widgets(json::array());

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto& defs = get_all_widget_defs();
    REQUIRE(wc.entries().size() == default_grid_widget_count());
    // build_default_grid() reorders anchors first, so check by content not position
    for (const auto& def : defs) {
        bool found = false;
        for (const auto& entry : wc.entries()) {
            if (entry.id == def.id) {
                // bed_temperature enabled state overridden by build_default_grid()
                if (def.id != std::string("bed_temperature")) {
                    REQUIRE(entry.enabled == def.default_enabled);
                }
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

// ============================================================================
// Per-panel config tests
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: per-panel load/save uses panel_widgets path",
                 "[panel_widget][widget_config]") {
    // Include grid coords to prevent pre-grid migration reset
    json widgets = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "network"}, {"enabled", false}, {"col", 1}, {"row", 0}},
    });
    setup_with_widgets(widgets, "home");

    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.entries()[0].id == "shutdown");
    REQUIRE(wc.entries()[0].enabled == true);
    REQUIRE(wc.entries()[1].id == "network");
    REQUIRE(wc.entries()[1].enabled == false);

    // Save and verify it writes to /printers/default/panel_widgets/home in pages format
    wc.save();
    REQUIRE(get_printer_data().contains("panel_widgets"));
    REQUIRE(get_printer_data()["panel_widgets"].contains("home"));
    auto root = get_saved_root();
    REQUIRE(root.is_object());
    REQUIRE(root.contains("pages"));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: non-home panel starts with defaults when no config exists",
                 "[panel_widget][widget_config]") {
    setup_empty_config();

    PanelWidgetConfig wc("controls", config);
    wc.load();

    // Should get defaults from registry (build_default_grid reorders anchors first)
    const auto& defs = get_all_widget_defs();
    REQUIRE(wc.entries().size() == default_grid_widget_count());
    for (const auto& def : defs) {
        bool found = false;
        for (const auto& entry : wc.entries()) {
            if (entry.id == def.id) {
                // bed_temperature enabled state overridden by build_default_grid()
                if (def.id != std::string("bed_temperature")) {
                    REQUIRE(entry.enabled == def.default_enabled);
                }
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

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
    // Legacy configs without grid coords are detected as pre-grid and reset to defaults.
    // After migration, data is saved in the new pages format.
    auto& printer_data = get_data()["printers"]["default"];
    REQUIRE(printer_data.contains("panel_widgets"));
    REQUIRE(printer_data["panel_widgets"].contains("home"));
    auto root = printer_data["panel_widgets"]["home"];
    REQUIRE(root.is_object());
    REQUIRE(root.contains("pages"));
    REQUIRE_FALSE(get_data().contains("home_widgets"));

    // After pre-grid reset, entries match default grid (all registry widgets present)
    REQUIRE(wc.entries().size() == default_grid_widget_count());
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

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration preserves per-widget config",
                 "[panel_widget][widget_config][migration]") {
    // Legacy configs without grid coords trigger pre-grid reset to defaults,
    // so per-widget config from legacy format is lost. Verify that migration
    // with grid coords preserves per-widget config via the new-style path.
    json widgets = json::array({
        {{"id", "temperature"},
         {"enabled", true},
         {"config", {{"sensor", "extruder"}}},
         {"col", 0},
         {"row", 0}},
        {{"id", "shutdown"}, {"enabled", true}, {"col", 1}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Per-widget config should survive load
    auto widget_cfg = wc.get_widget_config("temperature");
    REQUIRE(widget_cfg.contains("sensor"));
    REQUIRE(widget_cfg["sensor"] == "extruder");
}

// ============================================================================
// Grid coordinate tests
// ============================================================================

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: grid coordinates load from JSON",
                 "[panel_widget][widget_config][grid]") {
    json widgets = json::array({
        {{"id", "shutdown"},
         {"enabled", true},
         {"col", 0},
         {"row", 0},
         {"colspan", 1},
         {"rowspan", 1}},
        {{"id", "network"},
         {"enabled", true},
         {"col", 1},
         {"row", 0},
         {"colspan", 1},
         {"rowspan", 1}},
        {{"id", "temperature"},
         {"enabled", true},
         {"col", 2},
         {"row", 0},
         {"colspan", 1},
         {"rowspan", 1}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.entries()[0].col == 0);
    REQUIRE(wc.entries()[0].row == 0);
    REQUIRE(wc.entries()[0].colspan == 1);
    REQUIRE(wc.entries()[0].rowspan == 1);
    REQUIRE(wc.entries()[1].col == 1);
    REQUIRE(wc.entries()[1].row == 0);
    REQUIRE(wc.entries()[2].col == 2);
    REQUIRE(wc.entries()[2].row == 0);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: missing grid coords default to -1",
                 "[panel_widget][widget_config][grid]") {
    // Include one entry with grid coords to prevent pre-grid migration,
    // plus one entry without coords to test the default behavior
    json widgets = json::array({
        {{"id", "printer_image"},
         {"enabled", true},
         {"col", 0},
         {"row", 0},
         {"colspan", 2},
         {"rowspan", 2}},
        {{"id", "shutdown"}, {"enabled", true}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // power (entry index 1) has no grid coords — should default to -1
    auto find_entry = [&](const std::string& id) -> const PanelWidgetEntry* {
        for (const auto& e : wc.entries()) {
            if (e.id == id)
                return &e;
        }
        return nullptr;
    };
    auto* power = find_entry("shutdown");
    REQUIRE(power);
    REQUIRE(power->col == -1);
    REQUIRE(power->row == -1);
    REQUIRE(power->colspan == 1);
    REQUIRE(power->rowspan == 1);
    REQUIRE_FALSE(power->has_grid_position());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: grid coordinates round-trip save-load",
                 "[panel_widget][widget_config][grid]") {
    json widgets = json::array({
        {{"id", "shutdown"},
         {"enabled", true},
         {"col", 2},
         {"row", 1},
         {"colspan", 2},
         {"rowspan", 2}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc1("home", config);
    wc1.load();
    wc1.save();

    PanelWidgetConfig wc2("home", config);
    wc2.load();

    REQUIRE(wc2.entries()[0].col == 2);
    REQUIRE(wc2.entries()[0].row == 1);
    REQUIRE(wc2.entries()[0].colspan == 2);
    REQUIRE(wc2.entries()[0].rowspan == 2);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: save omits grid fields for auto-placed entries",
                 "[panel_widget][widget_config][grid]") {
    // One entry with grid coords (prevents pre-grid migration),
    // one entry without (should not get col/row in saved JSON)
    json widgets = json::array({
        {{"id", "printer_image"},
         {"enabled", true},
         {"col", 0},
         {"row", 0},
         {"colspan", 2},
         {"rowspan", 2}},
        {{"id", "shutdown"}, {"enabled", true}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();

    auto saved = get_saved_page0_widgets();
    // Find the power entry in saved JSON
    json* power_saved = nullptr;
    for (auto& item : saved) {
        if (item["id"] == "shutdown") {
            power_saved = &item;
            break;
        }
    }
    REQUIRE(power_saved != nullptr);
    REQUIRE(power_saved->contains("id"));
    // All entries always write col/row to JSON so positions survive reload.
    // Auto-placed entries that haven't been placed yet will have col=-1, row=-1.
    REQUIRE(power_saved->contains("col"));
    REQUIRE(power_saved->contains("row"));
    CHECK(power_saved->at("col").get<int>() == -1);
    CHECK(power_saved->at("row").get<int>() == -1);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: has_grid_position returns true for placed widgets",
                 "[panel_widget][widget_config][grid]") {
    json widgets = json::array({
        {{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "network"}, {"enabled", true}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.entries()[0].has_grid_position());
    REQUIRE_FALSE(wc.entries()[1].has_grid_position());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: build_default_grid all enabled widgets get grid positions",
                 "[panel_widget][widget_config][grid]") {
    auto grid = PanelWidgetConfig::build_default_grid();
    REQUIRE(grid.size() == default_grid_widget_count());

    // Anchor widgets (printer_image, print_status, tips, temperature, bed_temperature)
    // get explicit grid positions. All other widgets get col=-1, row=-1 (auto-place).
    const std::set<std::string> anchors = {"printer_image", "print_status", "tips", "temperature",
                                           "bed_temperature"};
    for (const auto& entry : grid) {
        INFO("Widget " << entry.id << " enabled=" << entry.enabled << " col=" << entry.col
                       << " row=" << entry.row);
        if (anchors.count(entry.id)) {
            REQUIRE(entry.has_grid_position());
        } else {
            REQUIRE_FALSE(entry.has_grid_position());
        }
    }
}

TEST_CASE("PanelWidgetConfig: build_default_grid produces correct layout",
          "[panel_widget][widget_config][grid]") {
    // Force tiny breakpoint (0) — a prior test may have set it to a larger value
    lv_subject_t* bp = theme_manager_get_breakpoint_subject();
    if (bp) {
        lv_subject_set_int(bp, 0);
    }
    auto entries = PanelWidgetConfig::build_default_grid();

    // Should include all registry widgets
    REQUIRE(entries.size() == default_grid_widget_count());

    // Collect enabled entries with grid positions
    std::vector<PanelWidgetEntry> placed;
    std::vector<PanelWidgetEntry> disabled;
    for (const auto& e : entries) {
        if (e.enabled && e.has_grid_position()) {
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

    // Printer image: top-left, 2×2
    auto* pi = find_entry("printer_image");
    REQUIRE(pi);
    REQUIRE(pi->enabled);
    REQUIRE(pi->col == 0);
    REQUIRE(pi->row == 0);
    REQUIRE(pi->colspan == 2);
    REQUIRE(pi->rowspan == 2);

    // Print status: below printer image, 2×2
    auto* ps = find_entry("print_status");
    REQUIRE(ps);
    REQUIRE(ps->enabled);
    REQUIRE(ps->col == 0);
    REQUIRE(ps->row == 2);
    REQUIRE(ps->colspan == 2);
    REQUIRE(ps->rowspan == 2);

    // Tips: right of printer image, dimensions depend on breakpoint
    // In tests, breakpoint subject is uninitialized (0 = tiny), so layout uses tiny breakpoint
    auto* tips = find_entry("tips");
    REQUIRE(tips);
    REQUIRE(tips->enabled);
    REQUIRE(tips->col == 2);
    REQUIRE(tips->row == 0);
    // Tiny breakpoint from default_layout.json: 2×2
    REQUIRE(tips->colspan == 2);
    REQUIRE(tips->rowspan == 2);

    // Non-anchor enabled widgets should NOT have grid positions (auto-placed at populate time)
    const std::set<std::string> anchors = {"printer_image", "print_status", "tips", "temperature",
                                           "bed_temperature"};
    for (const auto& e : entries) {
        if (anchors.count(e.id))
            continue;
        INFO("Widget " << e.id << " at (" << e.col << "," << e.row << ")");
        REQUIRE_FALSE(e.has_grid_position());
    }

    // New temperature anchors: nozzle at (2,2), bed stacked below at (2,3) on tiny.
    auto* nozzle = find_entry("temperature");
    REQUIRE(nozzle);
    REQUIRE(nozzle->enabled);
    REQUIRE(nozzle->col == 2);
    REQUIRE(nozzle->row == 2);
    auto* bed_anchor = find_entry("bed_temperature");
    REQUIRE(bed_anchor);
    REQUIRE(bed_anchor->col == 2);
    REQUIRE(bed_anchor->row == 3);

    // Disabled widgets should have no grid position
    for (const auto& e : disabled) {
        INFO("Disabled widget " << e.id);
        REQUIRE_FALSE(e.has_grid_position());
    }

    // fan_stack is multi_instance but included once as base ID in defaults
    auto* fs = find_entry("fan_stack");
    REQUIRE(fs);

    // notifications must be enabled (default_enabled, no gate) but NOT placed
    auto* notif = find_entry("notifications");
    REQUIRE(notif);
    REQUIRE(notif->enabled);
    REQUIRE_FALSE(notif->has_grid_position());

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

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: add_page creates new empty page",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.page_count() == 1);

    int idx = wc.add_page("my_page");
    REQUIRE(idx == 1);
    REQUIRE(wc.page_count() == 2);
    REQUIRE(wc.page_id(1) == "my_page");
    REQUIRE(wc.page_entries(1).empty());

    // Page 0 still has its widgets
    REQUIRE(wc.entries().size() == default_grid_widget_count());
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: add_page with auto-generated ID",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    int idx = wc.add_page();
    REQUIRE(idx >= 0);
    REQUIRE(wc.page_count() == 2);
    // Auto-generated ID should be "page_N"
    auto id = wc.page_id(static_cast<size_t>(idx));
    REQUIRE(id.substr(0, 5) == "page_");
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: add_page refuses beyond cap",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    // Add pages up to the cap
    for (size_t i = 1; i < MAX_PAGES; ++i) {
        int idx = wc.add_page();
        REQUIRE(idx >= 0);
    }
    REQUIRE(wc.page_count() == MAX_PAGES);

    // Adding one more should fail
    int idx = wc.add_page();
    REQUIRE(idx == -1);
    REQUIRE(wc.page_count() == MAX_PAGES);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: remove_page removes non-main page",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    wc.add_page("second");
    wc.add_page("third");
    REQUIRE(wc.page_count() == 3);

    bool removed = wc.remove_page(1);
    REQUIRE(removed);
    REQUIRE(wc.page_count() == 2);
    REQUIRE(wc.page_id(0) == "main");
    REQUIRE(wc.page_id(1) == "third");
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: cannot remove last page",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.page_count() == 1);
    bool removed = wc.remove_page(0);
    REQUIRE_FALSE(removed);
    REQUIRE(wc.page_count() == 1);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: remove_page adjusts main_page_index",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    wc.add_page("second");
    wc.add_page("third");

    // Set main page to index 2 (third)
    // Need to use pages format to set this up properly
    // Instead, just verify the adjustment logic:

    SECTION("removing page before main shifts main index down") {
        // Main is at index 2
        // We can set up with pages format
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

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: delete_entry searches all pages",
                 "[panel_widget][widget_config][multipage]") {
    // Use multi-instance widget IDs on page 1 to avoid registry-default overlap with page 0
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({
        {{"id", "thermistor:1"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "thermistor:2"}, {"enabled", true}, {"col", 1}, {"row", 0}},
    });
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();

    REQUIRE(wc.page_entries(1).size() == 2);

    // Delete entry from page 1 (multi-instance ID only exists on page 1)
    wc.delete_entry("thermistor:1");

    // Should have been removed from page 1
    REQUIRE(wc.page_entries(1).size() == 1);
    REQUIRE(wc.page_entries(1)[0].id == "thermistor:2");

    // Page 0 unchanged
    REQUIRE(wc.page_entries(0)[0].id == "shutdown");
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: mint_instance_id scans all pages",
                 "[panel_widget][widget_config][multipage]") {
    json w0 = json::array({
        {{"id", "thermistor:1"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "thermistor:2"}, {"enabled", true}, {"col", 1}, {"row", 0}},
    });
    json w1 = json::array({
        {{"id", "thermistor:5"}, {"enabled", true}, {"col", 0}, {"row", 0}},
    });
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Should find max across all pages (5) and return 6
    auto new_id = wc.mint_instance_id("thermistor");
    REQUIRE(new_id == "thermistor:6");
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "PanelWidgetConfig: is_enabled searches all pages",
                 "[panel_widget][widget_config][multipage]") {
    // Use multi-instance IDs on page 1 to avoid registry-default overlap with page 0
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", false}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({
        {{"id", "thermistor:1"}, {"enabled", true}, {"col", 0}, {"row", 0}},
        {{"id", "thermistor:2"}, {"enabled", false}, {"col", 1}, {"row", 0}},
    });
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // power is on page 0 but disabled
    REQUIRE_FALSE(wc.is_enabled("shutdown"));
    // thermistor:1 is only on page 1, enabled
    REQUIRE(wc.is_enabled("thermistor:1"));
    // thermistor:2 is only on page 1, disabled
    REQUIRE_FALSE(wc.is_enabled("thermistor:2"));
    REQUIRE_FALSE(wc.is_enabled("nonexistent"));
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: get/set_widget_config searches all pages",
                 "[panel_widget][widget_config][multipage]") {
    // Use multi-instance widget ID on page 1 to avoid registry-default overlap with page 0
    json w0 = json::array({{{"id", "shutdown"}, {"enabled", true}, {"col", 0}, {"row", 0}}});
    json w1 = json::array({{{"id", "thermistor:1"},
                            {"enabled", true},
                            {"config", {{"sensor", "bed"}}},
                            {"col", 0},
                            {"row", 0}}});
    setup_with_pages({{"p0", w0}, {"p1", w1}}, 0, 2);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Get config from page 1 (multi-instance ID only on page 1)
    auto cfg = wc.get_widget_config("thermistor:1");
    REQUIRE(cfg.contains("sensor"));
    REQUIRE(cfg["sensor"] == "bed");

    // Set config for widget on page 1
    wc.set_widget_config("thermistor:1", {{"sensor", "extruder"}});
    auto cfg2 = wc.get_widget_config("thermistor:1");
    REQUIRE(cfg2["sensor"] == "extruder");
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

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: remove_page out of bounds returns false",
                 "[panel_widget][widget_config][multipage]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    wc.add_page("second");
    REQUIRE_FALSE(wc.remove_page(99));
    REQUIRE(wc.page_count() == 2);
}

// ============================================================================
// migrate_stuck_ams_filament_swap — repair for pre-ef580203d stuck state
// ============================================================================

namespace {

/// Find an entry by id within a vector. Returns nullptr if missing.
const PanelWidgetEntry* find_entry(const std::vector<PanelWidgetEntry>& v, const char* id) {
    auto it =
        std::find_if(v.begin(), v.end(), [id](const PanelWidgetEntry& e) { return e.id == id; });
    return it == v.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: stuck ams(-1,-1) + filament placed → swap",
                 "[panel_widget][widget_config][migration]") {
    // Exact pattern from bundle C4XWB3ET: ams enabled but unplaced,
    // filament enabled at (3,3) grid position.
    json widgets = json::array({
        {{"id", "ams"}, {"enabled", true}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", true}, {"col", 3}, {"row", 3}},
        {{"id", "printer_image"}, {"enabled", true}, {"col", 0}, {"row", 0}},
    });
    setup_with_pages({{"main", widgets}});

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto* ams = find_entry(wc.entries(), "ams");
    const auto* fil = find_entry(wc.entries(), "filament");
    REQUIRE(ams);
    REQUIRE(fil);
    REQUIRE(ams->enabled);
    REQUIRE(ams->col == 3);
    REQUIRE(ams->row == 3);
    REQUIRE_FALSE(fil->enabled);
    REQUIRE(fil->col == -1);
    REQUIRE(fil->row == -1);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration is self-gating on subsequent load",
                 "[panel_widget][widget_config][migration]") {
    json widgets = json::array({
        {{"id", "ams"}, {"enabled", true}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", true}, {"col", 3}, {"row", 3}},
    });
    setup_with_pages({{"main", widgets}});

    PanelWidgetConfig wc1("home", config);
    wc1.load();

    // Second load on the now-migrated config should leave state untouched.
    PanelWidgetConfig wc2("home", config);
    wc2.load();

    const auto* ams = find_entry(wc2.entries(), "ams");
    const auto* fil = find_entry(wc2.entries(), "filament");
    REQUIRE(ams);
    REQUIRE(fil);
    REQUIRE(ams->col == 3);
    REQUIRE(ams->row == 3);
    REQUIRE_FALSE(fil->enabled);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration no-op when ams is disabled",
                 "[panel_widget][widget_config][migration]") {
    json widgets = json::array({
        {{"id", "ams"}, {"enabled", false}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", true}, {"col", 3}, {"row", 3}},
    });
    setup_with_pages({{"main", widgets}});

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto* fil = find_entry(wc.entries(), "filament");
    REQUIRE(fil);
    REQUIRE(fil->enabled);
    REQUIRE(fil->col == 3);
    REQUIRE(fil->row == 3);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration no-op when ams already placed",
                 "[panel_widget][widget_config][migration]") {
    json widgets = json::array({
        {{"id", "ams"}, {"enabled", true}, {"col", 2}, {"row", 1}},
        {{"id", "filament"}, {"enabled", true}, {"col", 3}, {"row", 3}},
    });
    setup_with_pages({{"main", widgets}});

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto* ams = find_entry(wc.entries(), "ams");
    const auto* fil = find_entry(wc.entries(), "filament");
    REQUIRE(ams);
    REQUIRE(fil);
    REQUIRE(ams->col == 2);
    REQUIRE(ams->row == 1);
    REQUIRE(fil->enabled);
    REQUIRE(fil->col == 3);
    REQUIRE(fil->row == 3);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration no-op when filament is disabled",
                 "[panel_widget][widget_config][migration]") {
    json widgets = json::array({
        {{"id", "ams"}, {"enabled", true}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", false}, {"col", 3}, {"row", 3}},
    });
    setup_with_pages({{"main", widgets}});

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto* ams = find_entry(wc.entries(), "ams");
    REQUIRE(ams);
    REQUIRE(ams->col == -1);
    REQUIRE(ams->row == -1);
}

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration no-op when filament is unplaced",
                 "[panel_widget][widget_config][migration]") {
    json widgets = json::array({
        {{"id", "ams"}, {"enabled", true}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", true}, {"col", -1}, {"row", -1}},
    });
    setup_with_pages({{"main", widgets}});

    PanelWidgetConfig wc("home", config);
    wc.load();

    const auto* ams = find_entry(wc.entries(), "ams");
    const auto* fil = find_entry(wc.entries(), "filament");
    REQUIRE(ams);
    REQUIRE(fil);
    REQUIRE(ams->col == -1);
    REQUIRE(fil->col == -1);
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

TEST_CASE_METHOD(PanelWidgetConfigFixture,
                 "PanelWidgetConfig: migration does not run for non-home panels",
                 "[panel_widget][widget_config][migration]") {
    // Same stuck pattern but for a non-home panel should be a no-op —
    // only home grid has the filament/ams swap convention.
    json widgets = json::array({
        {{"id", "ams"}, {"enabled", true}, {"col", -1}, {"row", -1}},
        {{"id", "filament"}, {"enabled", true}, {"col", 3}, {"row", 3}},
    });
    setup_with_pages({{"main", widgets}}, 0, -1, "other_panel");

    PanelWidgetConfig wc("other_panel", config);
    wc.load();

    const auto* ams = find_entry(wc.entries(), "ams");
    const auto* fil = find_entry(wc.entries(), "filament");
    REQUIRE(ams);
    REQUIRE(fil);
    REQUIRE(ams->col == -1);
    REQUIRE(fil->enabled);
    REQUIRE(fil->col == 3);
}
