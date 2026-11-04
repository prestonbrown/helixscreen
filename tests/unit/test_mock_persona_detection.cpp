// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/api/moonraker_client_mock_internal.h"
#include "../helix_test_fixture.h"
#include "core/update_queue.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "test_helpers/mock_personas.h"
#include "test_helpers/printer_capture.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"

namespace {

using helix::test::PersonaEnv;

/// What the app does under --test: the real discovery sequence over the mock.
helix::PrinterDiscovery discover(const helix::mock::PersonaEntry& p) {
    helix::PrinterDiscovery hw;
    {
        MoonrakerClientMock mock(p.type);
        mock.connect("ws://mock/websocket", [] {}, [] {});
        bool done = false;
        mock.discover_printer([&done] { done = true; });
        REQUIRE(done);
        hw = mock.hardware();
    }
    // Discovery queues UI-thread callbacks; run them before the test ends.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    return hw;
}

struct Expectation {
    std::string_view id;
    std::string_view type_name; ///< "" = no printer asserted (generic personas)
    std::string_view preset;
    bool autosaves;
};

// clang-format off
constexpr Expectation kExpected[] = {
    {"voron_24",           "Voron 2.4",                 "",             true},
    {"voron_trident",      "Voron Trident",             "",             false},
    {"k1",                 "Creality K1C",              "k1c",          true},
    {"k1max",              "Creality K1 Max",           "k1",           true},
    {"ad5m",               "FlashForge Adventurer 5M",  "ad5m",         true},
    {"creator5",           "FlashForge Creator 5 Pro",  "creator5_pro", true},
    {"creator5_zmod",      "FlashForge Creator 5 Pro",  "creator5_pro", true},
    {"generic_corexy",     "",                          "",             false},
    {"generic_bedslinger", "",                          "",             false},
    {"multi_extruder",     "",                          "",             false},
    {"delta",              "",                          "",             false},
    {"snapmaker_u1",       "Snapmaker U1",              "snapmaker_u1", true},
    {"cc1",                "Elegoo Centauri Carbon",    "cc1",          true},
    {"ad5x",               "FlashForge Adventurer 5X",  "ad5x",         true},
    {"k2",                 "Creality K2 Plus",          "k2",           true},
};
// clang-format on

TEST_CASE("The Voron personas model real centre-origin beds", "[mock][persona]") {
    const auto find_persona = [](std::string_view id) -> const helix::mock::PersonaEntry* {
        for (const auto& p : helix::mock::kPersonas) {
            if (p.id == id) {
                return &p;
            }
        }
        return nullptr;
    };
    const auto* voron = find_persona("voron_24");
    REQUIRE(voron != nullptr);
    const auto& v24 = helix::mock::descriptor(voron->type);
    // A real 2.4 homes to the bed centre: the demo files sliced for
    // centre-origin printers must fit the default persona.
    CHECK(v24.axis_min.x == -175.0);
    CHECK(v24.axis_min.y == -175.0);
    CHECK(v24.axis_max.x == 175.0);
    CHECK(v24.axis_max.y == 175.0);
    const auto* trident = find_persona("voron_trident");
    REQUIRE(trident != nullptr);
    const auto& vt = helix::mock::descriptor(trident->type);
    CHECK(vt.axis_min.x == -125.0);
    CHECK(vt.axis_max.x == 125.0);
    // The generic personas stay corner-origin.
    const auto* corexy = find_persona("generic_corexy");
    REQUIRE(corexy != nullptr);
    const auto& gc = helix::mock::descriptor(corexy->type);
    CHECK(gc.axis_min.x == 0.0);
    CHECK(gc.axis_max.x == 250.0);
    // The steppers the fit check and the volume parser read, through the real
    // discovery path.
    const auto hw = discover(*voron);
    CHECK(hw.build_volume().x_min == -175.0f);
    CHECK(hw.build_volume().x_max == 175.0f);
    CHECK(hw.build_volume().y_min == -175.0f);
    CHECK(hw.build_volume().y_max == 175.0f);
}

/// Every object, heater and fan the real machine reported is one the persona reports.
void check_mirrors_capture(const helix::PrinterDiscovery& hw, const std::string& slug) {
    const auto cap = helix::test::load_printer_capture(slug);
    auto contains = [](const auto& list, const std::string& v) {
        return std::find(list.begin(), list.end(), v) != list.end();
    };
    for (const auto& o : cap.value("printer_objects", nlohmann::json::array())) {
        INFO(slug << " object " << o);
        CHECK(contains(hw.printer_objects(), o.get<std::string>()));
    }
    for (const auto& h : cap.value("heaters", nlohmann::json::array())) {
        INFO(slug << " heater " << h);
        CHECK(contains(hw.heaters(), h.get<std::string>()));
    }
    for (const auto& f : cap.value("fans", nlohmann::json::array())) {
        INFO(slug << " fan " << f);
        CHECK(contains(hw.fans(), f.get<std::string>()));
    }
    CHECK(hw.hostname() == cap.value("hostname", std::string{}));
}

} // namespace

TEST_CASE("Every persona has a detection expectation", "[mock][persona][detect]") {
    for (const auto& p : helix::mock::kPersonas) {
        bool listed = false;
        for (const auto& e : kExpected)
            listed = listed || e.id == p.id;
        INFO(p.id);
        CHECK(listed);
    }
    for (const auto& e : kExpected) {
        INFO(e.id);
        CHECK(helix::mock::find_persona(e.id) != nullptr);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "Each mock persona auto-detects as the printer it impersonates",
                 "[mock][persona][detect]") {
    for (const auto& e : kExpected) {
        DYNAMIC_SECTION(e.id) {
            const auto* p = helix::mock::find_persona(e.id);
            REQUIRE(p != nullptr);
            helix::test::PersonaEnv env(e.id);
            const auto r = PrinterDetector::auto_detect(discover(*p));
            INFO(r.type_name << " " << r.confidence << "% margin " << r.margin() << ": "
                             << r.reason);
            CHECK(PrinterDetector::meets_autosave_threshold(r) == e.autosaves);
            if (!e.type_name.empty()) {
                CHECK(r.type_name == e.type_name);
                CHECK(r.preset == e.preset);
            }
            // A persona that declares its type declares the one detection picks.
            if (!p->saved_type.empty() && e.autosaves) {
                CHECK(p->saved_type == r.type_name);
            }
        }
    }
}

TEST_CASE_METHOD(HelixTestFixture, "The cc1 persona mirrors the real CC1 capture",
                 "[mock][persona][cc1]") {
    PersonaEnv env("cc1");
    check_mirrors_capture(discover(*helix::mock::find_persona("cc1")), "elegoo_centauri_carbon");
}

TEST_CASE_METHOD(HelixTestFixture, "An env override restores an omitted default",
                 "[mock][persona][cc1]") {
    PersonaEnv env("cc1");
    const auto* p = helix::mock::find_persona("cc1");
    {
        const auto objs = discover(*p).printer_objects();
        CHECK(std::find(objs.begin(), objs.end(), "cartographer") == objs.end());
    }
    helix::ScopedEnv probe("HELIX_MOCK_PROBE_TYPE", "cartographer");
    helix::ScopedEnv sensors("HELIX_MOCK_FILAMENT_SENSORS", "switch:extra_runout");
    const auto objs = discover(*p).printer_objects();
    CHECK(std::find(objs.begin(), objs.end(), "filament_switch_sensor extra_runout") != objs.end());
    const auto probe_objects = helix::sim::mock_probe_status(p->type);
    REQUIRE(probe_objects.contains("cartographer"));
    for (auto it = probe_objects.begin(); it != probe_objects.end(); ++it) {
        INFO(it.key());
        CHECK(std::find(objs.begin(), objs.end(), it.key()) != objs.end());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "The ad5x persona carries the AD5X preset's expected hardware",
                 "[mock][persona][ad5x]") {
    PersonaEnv env("ad5x");
    const auto hw = discover(*helix::mock::find_persona("ad5x"));
    std::ifstream f("assets/config/presets/ad5x.json");
    REQUIRE(f.good());
    const auto preset = nlohmann::json::parse(f);
    const auto& expected = preset["printer"]["hardware"]["expected"];
    REQUIRE_FALSE(expected.empty());
    const auto& objs = hw.printer_objects();
    for (const auto& name : expected) {
        INFO(name);
        CHECK(std::find(objs.begin(), objs.end(), name.get<std::string>()) != objs.end());
    }
    CHECK(std::find(objs.begin(), objs.end(), "gcode_macro SET_EXTRUDER_SLOT") != objs.end());
}

TEST_CASE_METHOD(HelixTestFixture, "The ad5x persona does not stand up the production IFS backend",
                 "[mock][persona][ad5x][ams]") {
    PersonaEnv env("ad5x");
    const auto hw = discover(*helix::mock::find_persona("ad5x"));
    CHECK(hw.mmu_type() != helix::AmsType::AD5X_IFS);
    for (const auto& o : hw.printer_objects()) {
        INFO(o);
        CHECK(o.rfind("ifs", 0) != 0);
        CHECK(o.rfind("zmod_ifs", 0) != 0);
        CHECK(o.find("_ifs_port_sensor") == std::string::npos);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "The k2 persona mirrors the K2 Plus capture",
                 "[mock][persona][k2]") {
    PersonaEnv env("k2");
    check_mirrors_capture(discover(*helix::mock::find_persona("k2")), "creality_k2_plus");
}

TEST_CASE_METHOD(HelixTestFixture, "HELIX_MOCK_AMS=none removes the k2 persona's box",
                 "[mock][persona][k2][ams]") {
    PersonaEnv env("k2");
    CHECK(helix::mock::effective_mock_ams(nullptr, "k2") == "cfs");
    {
        const auto hw = discover(*helix::mock::find_persona("k2"));
        const auto& objs = hw.printer_objects();
        CHECK(std::find(objs.begin(), objs.end(), "box") != objs.end());
    }
    helix::ScopedEnv none("HELIX_MOCK_AMS", "none");
    CHECK(helix::mock::effective_mock_ams("none", "k2") == "none");
    const auto hw = discover(*helix::mock::find_persona("k2"));
    const auto& objs = hw.printer_objects();
    CHECK(std::find(objs.begin(), objs.end(), "box") == objs.end());
}

TEST_CASE_METHOD(HelixTestFixture, "The snapmaker_u1 persona mirrors the U1 capture",
                 "[mock][persona][snapmaker]") {
    PersonaEnv env("snapmaker_u1");
    const auto hw = discover(*helix::mock::find_persona("snapmaker_u1"));
    check_mirrors_capture(hw, "snapmaker_u1");

    size_t extruders = 0;
    for (const auto& h : hw.heaters()) {
        extruders += h.rfind("extruder", 0) == 0;
    }
    CHECK(extruders == 4);
    CHECK(helix::mock::effective_mock_ams(nullptr, "snapmaker_u1") == "snapmaker");
    const auto& objs = hw.printer_objects();
    CHECK(std::find(objs.begin(), objs.end(), "filament_detect") == objs.end());
}
