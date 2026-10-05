// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::lazy_global;
using helix::lazy_global_if_exists;

namespace {

std::vector<std::string> g_events;

struct Probe {
    Probe() : arg(0) {
        g_events.push_back("construct");
    }
    explicit Probe(int a) : arg(a) {
        g_events.push_back("construct " + std::to_string(a));
    }
    ~Probe() {
        g_events.push_back("destroy");
    }
    int arg;
};

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "lazy_global builds one instance per type on first use",
                 "[lazy_global]") {
    StaticPanelRegistry::instance().destroy_all();
    g_events.clear();
    REQUIRE(lazy_global_if_exists<Probe>() == nullptr);

    Probe& a = lazy_global<Probe>("Probe", 7);
    // A different argument list reaches the same slot and constructs nothing.
    Probe& b = lazy_global<Probe>("Probe");
    CHECK(&a == &b);
    CHECK(a.arg == 7);
    CHECK(lazy_global_if_exists<Probe>() == &a);
    CHECK(g_events == std::vector<std::string>{"construct 7"});

    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(HelixTestFixture,
                 "destroy_all frees a lazy_global and the next use builds a fresh one",
                 "[lazy_global]") {
    StaticPanelRegistry::instance().destroy_all();
    g_events.clear();

    lazy_global<Probe>("Probe");
    StaticPanelRegistry::instance().destroy_all();
    CHECK(lazy_global_if_exists<Probe>() == nullptr);
    CHECK(g_events == std::vector<std::string>{"construct", "destroy"});

    // A printer switch: the re-created instance registers again, so the next
    // destroy_all frees it too.
    lazy_global<Probe>("Probe");
    CHECK(StaticPanelRegistry::instance().count() == 1);
    StaticPanelRegistry::instance().destroy_all();
    CHECK(g_events == std::vector<std::string>{"construct", "destroy", "construct", "destroy"});
}

TEST_CASE_METHOD(HelixTestFixture,
                 "a subject deinit after destroy_all sees no instance and builds none",
                 "[lazy_global]") {
    StaticPanelRegistry::instance().destroy_all();
    g_events.clear();

    lazy_global<Probe>("Probe");
    StaticSubjectRegistry::instance().register_deinit("LazyGlobalProbe", [] {
        g_events.push_back(lazy_global_if_exists<Probe>() ? "deinit: alive" : "deinit: gone");
    });

    // Application::shutdown() order: panels first, then core subjects.
    StaticPanelRegistry::instance().destroy_all();
    REQUIRE(StaticSubjectRegistry::instance().deinit_one("LazyGlobalProbe"));

    CHECK(g_events == std::vector<std::string>{"construct", "destroy", "deinit: gone"});
    CHECK(lazy_global_if_exists<Probe>() == nullptr);
}
