// tests/unit/test_xml_bind_compose.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A control may have more than one reason to be disabled, or hidden, and each
// reason gets its own binding line in the panel XML. The engine composes them:
// the property is applied while any binding holds it and cleared only once none
// does, so the outcome does not depend on which subject notified last.
//
// The composition mechanism is engine behaviour and is covered exhaustively in
// lib/helix-xml/tests/cases/test_bind_compose.c, on synthetic widgets. What
// these tests add is the REAL inventory: every widget in ui_xml/ that actually
// carries two or more bindings on one property, enumerated by
// scripts/scan_double_binds.py and driven here. A synthetic widget proves the
// mechanism; the inventory proves no real spelling was missed.
//
// Regenerate the table after adding or removing such a widget:
//
//     python3 scripts/scan_double_binds.py --emit-cpp
//
// The count assertion at the bottom is what notices that you did not.

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "printer_state.h"

#include <filesystem>
#include <map>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

enum class BindKind { State, Flag };

/// A subject one of the group's bindings reads, and a value that releases every
/// binding reading it.
struct GroupSubject {
    const char* name;
    int quiet;
};

/// One binding's demand: which of the group's subjects it reads, and a value of
/// that subject which makes this binding ask for the property.
struct GroupHold {
    int subject;
    int value;
};

struct BindGroup {
    const char* component;
    const char* widget;
    BindKind kind;
    const char* bits;
    GroupSubject subjects[4];
    int subject_count;
    GroupHold holds[4];
    int hold_count;
    bool orders_testable;
};

#include "double_bind_widgets.inc"

lv_state_t state_from_name(const std::string& name) {
    if (name == "disabled")
        return LV_STATE_DISABLED;
    if (name == "checked")
        return LV_STATE_CHECKED;
    if (name == "pressed")
        return LV_STATE_PRESSED;
    FAIL("unhandled state name in the generated table: " << name);
    return LV_STATE_DEFAULT;
}

lv_obj_flag_t flag_from_name(const std::string& name) {
    if (name == "hidden")
        return LV_OBJ_FLAG_HIDDEN;
    if (name == "clickable")
        return LV_OBJ_FLAG_CLICKABLE;
    FAIL("unhandled flag name in the generated table: " << name);
    return LV_OBJ_FLAG_HIDDEN;
}

/**
 * Builds one component from the inventory and drives the subjects its widget
 * binds.
 *
 * Most of these subjects are published by PrinterState and already in the XML
 * scope. The rest belong to a C++ panel no XML-only test constructs
 * (controls_operation_in_progress, led_command_in_flight, home_edit_mode, the
 * wizard flags), and for those the fixture registers an int subject of its own
 * so the binding resolves against something drivable instead of silently
 * binding nothing.
 *
 * Those registrations are process-lifetime on purpose: lv_xml_register_subject
 * borrows the pointer and the global scope keeps the name for the rest of the
 * run, so a fixture member would leave every later test that builds one of these
 * panels reading a dead stack slot.
 */
struct InventoryFixture : public XMLTestFixture {
    static std::map<std::string, lv_subject_t>& owned() {
        static std::map<std::string, lv_subject_t> subjects;
        return subjects;
    }

    /// The subject the XML scope will hand this binding, creating one if the app
    /// does not publish it here.
    static lv_subject_t* resolve(const char* name) {
        if (lv_subject_t* existing = lv_xml_get_subject(nullptr, name))
            return existing;
        lv_subject_t& mine = owned()[name];
        lv_subject_init_int(&mine, 0);
        lv_xml_register_subject(nullptr, name, &mine);
        return &mine;
    }

    ~InventoryFixture() override {
        // Drop the component's observers before ~XMLTestFixture tears the state
        // down under them.
        if (root != nullptr) {
            lv_obj_delete(root);
            root = nullptr;
        }
    }

    void set_and_settle(lv_subject_t* subject, int value) {
        lv_subject_set_int(subject, value);
        for (int pass = 0; pass < 8; ++pass) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    bool applied(const BindGroup& g, lv_obj_t* obj) const {
        return g.kind == BindKind::State ? lv_obj_has_state(obj, state_from_name(g.bits))
                                         : lv_obj_has_flag(obj, flag_from_name(g.bits));
    }

    /// Register the base components once, so a panel whose target widget sits
    /// inside a nested component still builds far enough to expose it.
    ///
    /// The variant directories are deliberately excluded. LVGL names a component
    /// by its FILE BASENAME, and eight names exist in both the base tree and a
    /// variant, so registering ui_xml/micro/controls_panel.xml would silently
    /// replace `controls_panel` for every build after it. Each row registers its
    /// own file just before building, which is what makes a variant row get the
    /// variant and a base row get the base.
    static void register_base_components() {
        static bool done = false;
        if (done)
            return;
        done = true;
        for (const char* dir : {"ui_xml", "ui_xml/components"}) {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".xml")
                    lv_xml_register_component_from_file(("A:" + entry.path().string()).c_str());
            }
        }
    }

    /// Build `g.component` and return its `g.widget`, or nullptr if this build
    /// cannot reach it.
    lv_obj_t* build(const BindGroup& g) {
        // Resolve every subject BEFORE creating the view: a binding whose subject
        // is missing at create time never installs, and the widget would then sit
        // at its attribute default and pass every assertion below vacuously.
        for (int i = 0; i < g.subject_count; ++i)
            resolve(g.subjects[i].name);

        register_base_components();
        const std::string path = std::string("A:ui_xml/") + g.component + ".xml";
        if (lv_xml_register_component_from_file(path.c_str()) != LV_RESULT_OK)
            return nullptr;

        // Built by basename, which is the only name the engine knows it by.
        const std::string name = std::filesystem::path(g.component).filename().string();
        root = create_component(name.c_str());
        if (root == nullptr)
            return nullptr;

        // lv_obj_find_by_name searches descendants only, and a component whose
        // bindings sit on its own <view> puts them on the root we just built.
        const char* root_name = lv_obj_get_name(root);
        if (root_name != nullptr && std::string(root_name) == g.widget)
            return root;
        return lv_obj_find_by_name(root, g.widget);
    }

    lv_obj_t* root = nullptr;
};

/// Every subject of the group at a value that releases all of its bindings.
void quiet_all(InventoryFixture& fx, const BindGroup& g) {
    for (int i = 0; i < g.subject_count; ++i)
        fx.set_and_settle(InventoryFixture::resolve(g.subjects[i].name), g.subjects[i].quiet);
}

} // namespace

// Deliberately a plain TEST_CASE and not TEST_CASE_METHOD: each group needs its
// own fixture, and two XMLTestFixtures alive at once would leave the global XML
// subject registry pointing at the inner one's destroyed PrinterState.
TEST_CASE("every ui_xml widget with two bindings on one property composes them",
          "[ui][xml][bind_compose][inventory]") {
    int checked = 0;
    int unreachable = 0;
    std::string unreachable_names;

    for (const BindGroup& g : kDoubleBindWidgets) {
        InventoryFixture fx;
        INFO("component " << g.component << ", widget " << g.widget << ", " << g.bits);

        lv_obj_t* obj = fx.build(g);
        if (obj == nullptr) {
            // The component did not build far enough to expose the widget in a
            // bare fixture (a nested component this test does not register).
            // Counted, not silently skipped.
            ++unreachable;
            unreachable_names += std::string(" ") + g.component + "/" + g.widget;
            continue;
        }
        ++checked;

        // Baseline: nothing asks for the property.
        quiet_all(fx, g);
        REQUIRE_FALSE(fx.applied(g, obj));

        // Each binding on its own must be able to apply it. A binding that lost
        // its share, or never installed, shows up here.
        for (int h = 0; h < g.hold_count; ++h) {
            INFO("binding " << h << " alone");
            quiet_all(fx, g);
            fx.set_and_settle(InventoryFixture::resolve(g.subjects[g.holds[h].subject].name),
                              g.holds[h].value);
            REQUIRE(fx.applied(g, obj));
        }

        if (!g.orders_testable)
            continue;

        // Raise every binding, then release them one at a time. The property must
        // survive until the last one lets go - forwards, and then backwards,
        // which is the order that passes against a last-writer-wins engine.
        for (int direction = 0; direction < 2; ++direction) {
            INFO("release order " << (direction == 0 ? "forwards" : "backwards"));
            quiet_all(fx, g);
            for (int h = 0; h < g.hold_count; ++h)
                fx.set_and_settle(InventoryFixture::resolve(g.subjects[g.holds[h].subject].name),
                                  g.holds[h].value);
            REQUIRE(fx.applied(g, obj));

            for (int n = 0; n < g.hold_count; ++n) {
                const int h = direction == 0 ? n : g.hold_count - 1 - n;
                const GroupSubject& s = g.subjects[g.holds[h].subject];
                fx.set_and_settle(InventoryFixture::resolve(s.name), s.quiet);
                INFO("released " << n + 1 << " of " << g.hold_count);
                // Still held by the others until the last one goes.
                REQUIRE(fx.applied(g, obj) == (n + 1 < g.hold_count));
            }
        }
    }

    // The inventory has to actually have been exercised. Without this the loop
    // above passes with every component unreachable, which is exactly what a
    // renamed widget or a broken fixture would look like.
    INFO(checked << " groups exercised, " << unreachable << " unreachable of "
                 << std::size(kDoubleBindWidgets) << ";" << unreachable_names);
    REQUIRE(unreachable == 0);
    REQUIRE(checked == static_cast<int>(std::size(kDoubleBindWidgets)));
}

TEST_CASE("the double-bind inventory is regenerated when ui_xml changes",
          "[ui][xml][bind_compose][inventory]") {
    // The table is generated, so it can only go stale silently. Pin the totals it
    // was generated from: adding a widget with two bindings on one property, or
    // removing one, moves these and forces a regeneration and a re-run.
    REQUIRE(std::size(kDoubleBindWidgets) + kUndrivableGroups == static_cast<size_t>(kTotalGroups));
    REQUIRE(kTotalGroups == 35);
}
