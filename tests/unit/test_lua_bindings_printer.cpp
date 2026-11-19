// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_fixtures.h"
#include "../test_helpers/plugin_test_support.h"
#include "connection_state.h"
#include "lua_bindings.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE_METHOD(XMLTestFixture, "printer.get maps the stable names",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    lv_subject_set_int(lv_xml_get_subject(nullptr, "bed_temp"), 605);
    lv_subject_set_string(lv_xml_get_subject(nullptr, "print_state"), "printing");
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                       static_cast<int>(helix::ConnectionState::CONNECTED));
    REQUIRE(b.t.run(R"(
        bed = helix.printer.get("bed_temp")
        state = helix.printer.get("print_state")
        conn = helix.printer.get("connected")
    )"));
    CHECK(b.t.global("bed") == "60.5");
    CHECK(b.t.global("state") == "printing");
    CHECK(b.t.global("conn") == "true");
}

TEST_CASE_METHOD(XMLTestFixture, "printer.get refuses names outside the table",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    CHECK_FALSE(b.t.run(R"(helix.printer.get("print_state_enum"))"));
    CHECK_FALSE(b.t.run(R"(helix.printer.get("chamber_effective_target"))"));
}

TEST_CASE_METHOD(XMLTestFixture, "printer.watch reports changes in Lua units",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    REQUIRE(b.t.run(R"(
        seen = {}
        helix.printer.watch("extruder_target", function(v) seen[#seen + 1] = v end)
    )"));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "extruder_target"), 2150);
    REQUIRE(b.t.run("r = table.concat(seen, ',')"));
    CHECK(b.t.global("r") == "215.0");
}

TEST_CASE_METHOD(XMLTestFixture, "subject observers and printer watches share one quota",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_ui_bindings, &install_printer_bindings});
    REQUIRE(b.t.run(R"(
        local s = helix.subject.int("n", 0)
        for i = 1, 255 do s:observe(function() end) end
        helix.printer.watch("bed_temp", function() end)
        ok1, e1 = pcall(s.observe, s, function() end)
        ok2, e2 = pcall(helix.printer.watch, "bed_temp", function() end)
    )"));
    CHECK(b.t.global("ok1") == "false");
    CHECK(b.t.global("e1").find("at most 256") != std::string::npos);
    CHECK(b.t.global("ok2") == "false");
    CHECK(b.t.global("e2").find("at most 256") != std::string::npos);
}

TEST_CASE_METHOD(XMLTestFixture, "printer watchers detach when the runtime closes",
                 "[plugin][bindings][printer]") {
    {
        BoundRuntime b({&install_printer_bindings});
        REQUIRE(b.t.run(R"(helix.printer.watch("bed_temp", function() end))"));
    }
    lv_subject_set_int(lv_xml_get_subject(nullptr, "bed_temp"), 1);
    SUCCEED(); // ASAN is what proves the closed runtime was not called
}

TEST_CASE_METHOD(XMLTestFixture, "klippy_state reports Klipper's own state names",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    lv_subject_t* klippy = lv_xml_get_subject(nullptr, "klippy_state");
    REQUIRE(klippy);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                       static_cast<int>(helix::ConnectionState::CONNECTED));
    lv_subject_set_int(klippy, static_cast<int>(helix::KlippyState::READY));
    REQUIRE(b.t.run(R"(
        first = helix.printer.get("klippy_state")
        seen = {}
        helix.printer.watch("klippy_state", function(v) seen[#seen + 1] = v end)
    )"));
    for (auto st : {helix::KlippyState::SHUTDOWN, helix::KlippyState::STARTUP,
                    helix::KlippyState::ERROR, helix::KlippyState::READY})
        lv_subject_set_int(klippy, static_cast<int>(st));
    REQUIRE(b.t.run("r = table.concat(seen, ',')"));
    CHECK(b.t.global("first") == "ready");
    CHECK(b.t.global("r") == "shutdown,startup,error,ready");
}

TEST_CASE_METHOD(XMLTestFixture, "klippy_state reads nil while Moonraker is disconnected",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    lv_subject_t* conn = lv_xml_get_subject(nullptr, "printer_connection_state");
    lv_subject_t* klippy = lv_xml_get_subject(nullptr, "klippy_state");
    REQUIRE(conn);
    REQUIRE(klippy);
    lv_subject_set_int(conn, static_cast<int>(helix::ConnectionState::DISCONNECTED));
    lv_subject_set_int(klippy, static_cast<int>(helix::KlippyState::SHUTDOWN));
    REQUIRE(b.t.run(R"(
        before = helix.printer.get("klippy_state")
        seen = {}
        helix.printer.watch("klippy_state", function(v) seen[#seen + 1] = tostring(v) end)
    )"));
    lv_subject_set_int(klippy, static_cast<int>(helix::KlippyState::STARTUP));
    lv_subject_set_int(conn, static_cast<int>(helix::ConnectionState::CONNECTED));
    lv_subject_set_int(klippy, static_cast<int>(helix::KlippyState::READY));
    REQUIRE(b.t.run(R"(
        after = helix.printer.get("klippy_state")
        r = table.concat(seen, ',')
    )"));
    CHECK(b.t.global("before") == "nil");
    CHECK(b.t.global("r") == "nil,ready");
    CHECK(b.t.global("after") == "ready");
}

TEST_CASE("the stable printer table", "[plugin][bindings][printer]") {
    REQUIRE(printer_fields().size() == 11);
    CHECK(std::string(printer_fields()[0].lua_name) == "connected");
    CHECK(std::string(printer_fields()[1].lua_name) == "klippy_state");
}

#endif // HELIX_HAS_PLUGINS
