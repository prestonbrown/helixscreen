// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_static_subject_registry.cpp
 * @brief A subject source that is rebuilt must not leave its old deinit behind.
 *
 * Entries are keyed by name because the name identifies a subject source, not a
 * registration event. A source that is torn down and re-created re-registers
 * under the same name; if both entries survived, shutdown would run a callback
 * closed over the destroyed instance alongside the live one.
 *
 * Cases use deinit_one() rather than deinit_all() so they never touch the other
 * entries the process-wide registry is holding for the rest of the suite; the
 * one deinit_all() case runs against an isolated registry.
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/scoped_subject_registry_isolation.h"
#include "static_subject_registry.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(HelixTestFixture, "StaticSubjectRegistry keeps one entry per name",
                 "[core][subjects]") {
    auto& registry = StaticSubjectRegistry::instance();
    static constexpr const char* kName = "StaticSubjectRegistryTestProbe";

    std::vector<std::string> ran;
    registry.register_deinit(kName, [&ran]() { ran.emplace_back("first"); });
    registry.register_deinit(kName, [&ran]() { ran.emplace_back("second"); });

    REQUIRE(registry.deinit_one(kName));
    // The live registration wins, and nothing from the superseded one is left.
    CHECK(ran == std::vector<std::string>{"second"});
    CHECK_FALSE(registry.deinit_one(kName));
}

TEST_CASE_METHOD(HelixTestFixture, "StaticSubjectRegistry re-registration keeps its slot",
                 "[core][subjects]") {
    // A slot is the source's position in registration order, and deinit_all()
    // walks that in reverse so late registrants (widgets) tear down before the
    // early ones whose subjects they observe. Re-registering must not move the
    // entry: a core singleton that re-runs init_subjects() — a soft restart, a
    // printer switch — would land at the tail and be deinitialized FIRST, ahead
    // of every widget observing it.
    //
    // Read the order rather than firing deinit_all(): the registry is
    // process-wide and holds entries left by earlier fixtures.
    auto& registry = StaticSubjectRegistry::instance();
    static constexpr const char* kEarly = "StaticSubjectRegistrySlotEarly";
    static constexpr const char* kMiddle = "StaticSubjectRegistrySlotMiddle";
    static constexpr const char* kLate = "StaticSubjectRegistrySlotLate";

    auto index_of = [&registry](const char* name) {
        const std::vector<std::string> names = registry.names();
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name)
                return static_cast<long>(i);
        }
        return -1L;
    };

    std::vector<std::string> ran;
    registry.register_deinit(kEarly, [&ran]() { ran.emplace_back("early:first"); });
    registry.register_deinit(kMiddle, [&ran]() { ran.emplace_back("middle"); });
    registry.register_deinit(kLate, [&ran]() { ran.emplace_back("late"); });

    const long early_slot = index_of(kEarly);
    REQUIRE(early_slot >= 0);
    REQUIRE(early_slot < index_of(kMiddle));
    REQUIRE(index_of(kMiddle) < index_of(kLate));

    const size_t before = registry.count();
    registry.register_deinit(kEarly, [&ran]() { ran.emplace_back("early:second"); });

    // Still one entry, still in its original slot ahead of the two later ones.
    CHECK(registry.count() == before);
    CHECK(index_of(kEarly) == early_slot);
    CHECK(index_of(kEarly) < index_of(kMiddle));
    CHECK(index_of(kEarly) < index_of(kLate));

    // And the live registration is the one that runs.
    REQUIRE(registry.deinit_one(kEarly));
    CHECK(ran == std::vector<std::string>{"early:second"});
    CHECK_FALSE(registry.deinit_one(kEarly));

    REQUIRE(registry.deinit_one(kMiddle));
    REQUIRE(registry.deinit_one(kLate));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "StaticSubjectRegistry printer switch honours each entry's lifetime",
                 "[core][subjects]") {
    // A switch only re-runs init on the rebuild path, so it must leave alone what
    // boot-time registration owns and still release per-widget state that observes
    // printer subjects. Exit runs everything.
    using Lifetime = StaticSubjectRegistry::Lifetime;
    using Scope = StaticSubjectRegistry::Scope;
    auto& registry = StaticSubjectRegistry::instance();
    static constexpr const char* kPrinter = "StaticSubjectRegistryLifetimePrinter";
    static constexpr const char* kProcess = "StaticSubjectRegistryLifetimeProcess";
    static constexpr const char* kRerun = "StaticSubjectRegistryLifetimeRerun";

    auto registered = [&registry](const char* name) {
        const std::vector<std::string> names = registry.names();
        return std::find(names.begin(), names.end(), name) != names.end();
    };

    std::vector<std::string> ran;
    registry.register_deinit(kPrinter, [&ran]() { ran.emplace_back("printer"); });
    registry.register_deinit(
        kProcess, [&ran]() { ran.emplace_back("process"); }, Lifetime::Process);
    registry.register_deinit(
        kRerun, [&ran]() { ran.emplace_back("rerun"); }, Lifetime::ProcessRunOnSwitch);

    REQUIRE(registry.deinit_one(kPrinter, Scope::PrinterSwitch));
    REQUIRE(registry.deinit_one(kProcess, Scope::PrinterSwitch));
    REQUIRE(registry.deinit_one(kRerun, Scope::PrinterSwitch));
    CHECK(ran == std::vector<std::string>{"printer", "rerun"});

    // The printer entry is gone and free to come back from the rebuild path.
    CHECK_FALSE(registered(kPrinter));
    CHECK(registered(kProcess));
    CHECK(registered(kRerun));
    registry.register_deinit(kPrinter, [&ran]() { ran.emplace_back("printer:rebuilt"); });

    // A second switch re-runs the run-on-switch cleanup; exit then runs every entry once.
    ran.clear();
    REQUIRE(registry.deinit_one(kRerun, Scope::PrinterSwitch));
    REQUIRE(registry.deinit_one(kPrinter, Scope::ProcessExit));
    REQUIRE(registry.deinit_one(kProcess, Scope::ProcessExit));
    REQUIRE(registry.deinit_one(kRerun, Scope::ProcessExit));
    CHECK(ran == std::vector<std::string>{"rerun", "printer:rebuilt", "process", "rerun"});
    CHECK_FALSE(registered(kPrinter));
    CHECK_FALSE(registered(kProcess));
    CHECK_FALSE(registered(kRerun));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "StaticSubjectRegistry deinit_all at a printer switch keeps what the rebuild "
                 "does not re-register",
                 "[core][subjects]") {
    // The one case here that fires deinit_all(): it walks an isolated registry, so only
    // this case's entries run and the process-wide ones come back afterwards.
    using Lifetime = StaticSubjectRegistry::Lifetime;
    using Scope = StaticSubjectRegistry::Scope;
    ScopedSubjectRegistryIsolation isolated;
    auto& registry = StaticSubjectRegistry::instance();

    std::vector<std::string> ran;
    registry.register_deinit("Printer", [&ran]() { ran.emplace_back("printer"); });
    registry.register_deinit(
        "Process", [&ran]() { ran.emplace_back("process"); }, Lifetime::Process);
    registry.register_deinit(
        "Rerun", [&ran]() { ran.emplace_back("rerun"); }, Lifetime::ProcessRunOnSwitch);
    // A callback that re-registers a kept entry and adds a new one mid-walk.
    registry.register_deinit("Rebuilder", [&ran, &registry]() {
        ran.emplace_back("rebuilder");
        registry.register_deinit(
            "Process", [&ran]() { ran.emplace_back("process:new"); }, Lifetime::Process);
        registry.register_deinit("Fresh", [&ran]() { ran.emplace_back("fresh"); });
    });

    registry.deinit_all(Scope::PrinterSwitch);

    // Reverse order, and the Process entry is neither run nor dropped.
    CHECK(ran == std::vector<std::string>{"rebuilder", "rerun", "printer"});
    // Survivors keep their slots ahead of what the walk registered.
    CHECK(registry.names() == std::vector<std::string>{"Process", "Rerun", "Fresh"});

    // Exit runs everything left, and the re-registered callback is the one that runs.
    ran.clear();
    registry.deinit_all(Scope::ProcessExit);
    CHECK(ran == std::vector<std::string>{"fresh", "rerun", "process:new"});
    CHECK(registry.count() == 0);

    // An emptied registry takes fresh registrations.
    ran.clear();
    registry.register_deinit("Again", [&ran]() { ran.emplace_back("again"); });
    registry.deinit_all();
    CHECK(ran == std::vector<std::string>{"again"});
}
