// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_macro_param_defaults.cpp
 * @brief Per-printer saved macro parameter defaults: the store behind "Default
 *        Parameters" and the "Ask for parameters" toggle.
 *
 * Run with: ./build/bin/helix-tests "[macro][defaults]"
 *
 * Records live under the active printer's settings subtree, keyed by the
 * macro's lowercase name, so two printers never see each other's values. An
 * empty value string means "use the macro's own default" and is never stored;
 * a record that holds nothing and still asks is indistinguishable from no
 * record and is removed.
 */

#include "../helix_test_fixture.h"
#include "config.h"
#include "macro_param_defaults.h"

#include <map>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::MacroParamDefaultRecord;
using helix::MacroParamDefaults;
using Values = std::map<std::string, std::string>;

namespace {

/// Seed two printers so set_active_printer() can switch between real subtrees.
void seed_two_printers() {
    auto* config = helix::Config::get_instance();
    config->set<json>("/printers/voron", json::object());
    config->set<json>("/printers/k2", json::object());
    REQUIRE(config->set_active_printer("voron"));
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "a missing record reads as empty and asking",
                 "[macro][defaults]") {
    helix::Config::get_instance()->reset_to_defaults();
    auto record = MacroParamDefaults::instance().get("CLEAN_NOZZLE");
    CHECK(record.values.empty());
    CHECK(record.ask_for_params);
}

TEST_CASE_METHOD(HelixTestFixture, "records round-trip, keyed case-insensitively",
                 "[macro][defaults]") {
    helix::Config::get_instance()->reset_to_defaults();

    MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}, {"SPEED", "50"}};
    record.ask_for_params = false;
    MacroParamDefaults::instance().set("CLEAN_NOZZLE", record);

    // MacroParamCache keys names lowercase; the store must agree with it.
    auto back = MacroParamDefaults::instance().get("clean_nozzle");
    CHECK(back.values == Values({{"SPEED", "50"}, {"TEMP", "210"}}));
    CHECK_FALSE(back.ask_for_params);
}

TEST_CASE_METHOD(HelixTestFixture, "empty value strings are not stored", "[macro][defaults]") {
    helix::Config::get_instance()->reset_to_defaults();

    MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}, {"SPEED", ""}};
    MacroParamDefaults::instance().set("CLEAN_NOZZLE", record);

    auto back = MacroParamDefaults::instance().get("CLEAN_NOZZLE");
    CHECK(back.values == Values({{"TEMP", "210"}}));
}

TEST_CASE_METHOD(HelixTestFixture, "a record with no values that still asks is removed",
                 "[macro][defaults]") {
    helix::Config::get_instance()->reset_to_defaults();

    MacroParamDefaultRecord record; // no values, ask true: the default record
    MacroParamDefaults::instance().set("CLEAN_NOZZLE", record);
    CHECK(MacroParamDefaults::instance().get("CLEAN_NOZZLE").values.empty());
    CHECK(MacroParamDefaults::instance().get("CLEAN_NOZZLE").ask_for_params);

    // Saving every field back to empty restores the same shape: no record.
    MacroParamDefaultRecord had_values;
    had_values.values = {{"TEMP", "210"}};
    MacroParamDefaults::instance().set("CLEAN_NOZZLE", had_values);
    REQUIRE_FALSE(MacroParamDefaults::instance().get("CLEAN_NOZZLE").values.empty());

    had_values.values.clear(); // user cleared the only field
    MacroParamDefaults::instance().set("CLEAN_NOZZLE", had_values);
    CHECK(MacroParamDefaults::instance().get("CLEAN_NOZZLE").values.empty());
    CHECK(MacroParamDefaults::instance().get("CLEAN_NOZZLE").ask_for_params);
}

TEST_CASE_METHOD(HelixTestFixture, "an ask-off record with no values persists",
                 "[macro][defaults]") {
    helix::Config::get_instance()->reset_to_defaults();

    MacroParamDefaultRecord record; // "never ask, everything at its own default"
    record.ask_for_params = false;
    MacroParamDefaults::instance().set("PURGE", record);

    auto back = MacroParamDefaults::instance().get("purge");
    CHECK(back.values.empty());
    CHECK_FALSE(back.ask_for_params);
}

TEST_CASE_METHOD(HelixTestFixture, "clear removes one record and nothing else",
                 "[macro][defaults]") {
    helix::Config::get_instance()->reset_to_defaults();

    MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}};
    MacroParamDefaults::instance().set("CLEAN_NOZZLE", record);
    MacroParamDefaults::instance().set("PURGE", record);

    MacroParamDefaults::instance().clear("clean_nozzle");
    CHECK(MacroParamDefaults::instance().get("CLEAN_NOZZLE").values.empty());
    CHECK_FALSE(MacroParamDefaults::instance().get("PURGE").values.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "a printer switch never leaks another printer's records",
                 "[macro][defaults]") {
    helix::Config::get_instance()->reset_to_defaults();
    seed_two_printers();

    MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}};
    record.ask_for_params = false;
    MacroParamDefaults::instance().set("LOAD_FILAMENT", record);

    REQUIRE(helix::Config::get_instance()->set_active_printer("k2"));
    auto other = MacroParamDefaults::instance().get("LOAD_FILAMENT");
    CHECK(other.values.empty());
    CHECK(other.ask_for_params);

    // Writing under the second printer leaves the first printer's record alone.
    MacroParamDefaultRecord k2_record;
    k2_record.values = {{"TEMP", "180"}};
    MacroParamDefaults::instance().set("LOAD_FILAMENT", k2_record);

    REQUIRE(helix::Config::get_instance()->set_active_printer("voron"));
    auto back = MacroParamDefaults::instance().get("LOAD_FILAMENT");
    CHECK(back.values == Values({{"TEMP", "210"}}));
    CHECK_FALSE(back.ask_for_params);
}
