// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// PrinterCacheRegistry — the structural replacement for the single hardcoded
// clear_all_panel_configs() call that fixed #804. Components that memoize
// Config::df()-derived state self-register an invalidator; every active-printer
// change fires all of them.
//
// The contract these tests pin, and why each half matters:
//   - every registrant fires, in registration order
//   - registrations SURVIVE invalidate_all() (unlike StaticSubjectRegistry::deinit_all),
//     because a user can switch printers any number of times
//   - registration is idempotent by name, because an init path that re-runs after a
//     soft restart would otherwise accumulate a duplicate on every switch
//   - unregister() exists for owners with a bounded lifetime
//   - one throwing invalidator must not stop the rest — a component silently left
//     stale by another component's bad cleanup is the exact bug being prevented

#include "../helix_test_fixture.h"
#include "printer_cache_registry.h"
#include "printer_state.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrinterCacheRegistry;

namespace {

/// Registry entries are process-wide and persist by design, so every test starts
/// from a cleared registry and leaves one behind.
class PrinterCacheRegistryFixture : public HelixTestFixture {
  public:
    PrinterCacheRegistryFixture() {
        PrinterCacheRegistry::instance().clear();
    }
    ~PrinterCacheRegistryFixture() override {
        PrinterCacheRegistry::instance().clear();
    }
};

} // namespace

TEST_CASE_METHOD(PrinterCacheRegistryFixture, "PrinterCacheRegistry: registration",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    SECTION("starts empty after clear") {
        REQUIRE(reg.count() == 0);
        REQUIRE(reg.names().empty());
    }

    SECTION("registering adds one entry per distinct name") {
        reg.register_invalidator("A", []() {});
        reg.register_invalidator("B", []() {});

        REQUIRE(reg.count() == 2);
        REQUIRE(reg.names() == std::vector<std::string>{"A", "B"});
    }

    SECTION("clear() drops entries without running them") {
        int fired = 0;
        reg.register_invalidator("A", [&fired]() { fired++; });
        reg.clear();

        REQUIRE(reg.count() == 0);
        REQUIRE(fired == 0);
    }

    SECTION("a null name is refused rather than registered") {
        reg.register_invalidator(nullptr, []() {});
        REQUIRE(reg.count() == 0);
    }
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: invalidate_all fires every registrant",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    int a = 0;
    int b = 0;
    int c = 0;
    reg.register_invalidator("A", [&a]() { a++; });
    reg.register_invalidator("B", [&b]() { b++; });
    reg.register_invalidator("C", [&c]() { c++; });

    reg.invalidate_all();

    // The whole point: no registrant is skipped. A registry that fired only the
    // first (or only the last) would reproduce #804 for every other component.
    REQUIRE(a == 1);
    REQUIRE(b == 1);
    REQUIRE(c == 1);
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: firing order and repeatability",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    SECTION("callbacks run in registration order") {
        std::vector<std::string> order;
        reg.register_invalidator("first", [&order]() { order.emplace_back("first"); });
        reg.register_invalidator("second", [&order]() { order.emplace_back("second"); });
        reg.register_invalidator("third", [&order]() { order.emplace_back("third"); });

        reg.invalidate_all();

        REQUIRE(order == std::vector<std::string>{"first", "second", "third"});
    }

    SECTION("registrations survive invalidate_all and fire on every switch") {
        int fired = 0;
        reg.register_invalidator("A", [&fired]() { fired++; });

        reg.invalidate_all();
        reg.invalidate_all();
        reg.invalidate_all();

        // Not idempotent in the "runs once" sense — a user can switch printers
        // repeatedly and each switch must drop the caches again.
        REQUIRE(fired == 3);
        REQUIRE(reg.count() == 1);
    }

    SECTION("invalidate_all on an empty registry is a no-op") {
        REQUIRE_NOTHROW(reg.invalidate_all());
        REQUIRE(reg.count() == 0);
    }
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: re-registering a name replaces in place",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    int old_fired = 0;
    int new_fired = 0;
    std::vector<std::string> order;

    reg.register_invalidator("A", [&]() {
        old_fired++;
        order.emplace_back("A");
    });
    reg.register_invalidator("B", [&order]() { order.emplace_back("B"); });

    // A component's init_subjects() runs again after every soft restart. Without
    // replace-by-name the registry would grow an extra "A" on every printer switch.
    reg.register_invalidator("A", [&]() {
        new_fired++;
        order.emplace_back("A");
    });

    REQUIRE(reg.count() == 2);

    reg.invalidate_all();

    REQUIRE(old_fired == 0);
    REQUIRE(new_fired == 1);
    // Replacement keeps the original position, so ordering is stable across restarts.
    REQUIRE(order == std::vector<std::string>{"A", "B"});
    REQUIRE(reg.names() == std::vector<std::string>{"A", "B"});
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture, "PrinterCacheRegistry: unregister",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    int a = 0;
    int b = 0;
    reg.register_invalidator("A", [&a]() { a++; });
    reg.register_invalidator("B", [&b]() { b++; });

    SECTION("an unregistered callback never fires again") {
        REQUIRE(reg.unregister("A"));
        REQUIRE(reg.count() == 1);

        reg.invalidate_all();

        // Lifetime safety: PrinterState captures `this` and unregisters from
        // deinit_subjects(). A stale entry would be a call through freed memory.
        REQUIRE(a == 0);
        REQUIRE(b == 1);
    }

    SECTION("unregistering an unknown or null name reports failure and changes nothing") {
        REQUIRE_FALSE(reg.unregister("nope"));
        REQUIRE_FALSE(reg.unregister(nullptr));
        REQUIRE(reg.count() == 2);
    }

    SECTION("unregister does not run the callback") {
        reg.unregister("A");
        REQUIRE(a == 0);
    }
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture, "PrinterCacheRegistry: invalidate_one",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    int a = 0;
    int b = 0;
    reg.register_invalidator("A", [&a]() { a++; });
    reg.register_invalidator("B", [&b]() { b++; });

    SECTION("runs only the named entry and keeps it registered") {
        REQUIRE(reg.invalidate_one("A"));
        REQUIRE(a == 1);
        REQUIRE(b == 0);
        // Unlike StaticSubjectRegistry::deinit_one(), the entry is NOT consumed.
        REQUIRE(reg.count() == 2);

        reg.invalidate_all();
        REQUIRE(a == 2);
        REQUIRE(b == 1);
    }

    SECTION("an unknown or null name reports failure and runs nothing") {
        REQUIRE_FALSE(reg.invalidate_one("nope"));
        REQUIRE_FALSE(reg.invalidate_one(nullptr));
        REQUIRE(a == 0);
        REQUIRE(b == 0);
    }
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: a throwing invalidator does not strand the rest",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    int before = 0;
    int after = 0;
    reg.register_invalidator("before", [&before]() { before++; });
    reg.register_invalidator("throws", []() { throw std::runtime_error("boom"); });
    reg.register_invalidator("after", [&after]() { after++; });

    REQUIRE_NOTHROW(reg.invalidate_all());

    // Letting one component's failure skip the others would leave those others
    // serving the previous printer's data — the exact class of bug this registry exists
    // to make impossible.
    REQUIRE(before == 1);
    REQUIRE(after == 1);
    REQUIRE(reg.count() == 3);

    SECTION("invalidate_one also contains the throw") {
        REQUIRE_NOTHROW(reg.invalidate_one("throws"));
    }
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: a callback that registers during invalidate_all is safe",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();

    int late_fired = 0;
    int tail_fired = 0;

    // Enough entries that a push_back into the vector being iterated would
    // reallocate and dangle the loop's iterators.
    reg.register_invalidator("re-registers", [&reg, &late_fired]() {
        reg.register_invalidator("late", [&late_fired]() { late_fired++; });
    });
    reg.register_invalidator("tail", [&tail_fired]() { tail_fired++; });

    REQUIRE_NOTHROW(reg.invalidate_all());

    // The new entry lands for NEXT time; the current pass still completes.
    REQUIRE(tail_fired == 1);
    REQUIRE(late_fired == 0);
    REQUIRE(reg.count() == 3);

    reg.invalidate_all();
    REQUIRE(late_fired == 1);
    REQUIRE(tail_fired == 2);
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: instance is a stable singleton",
                 "[printer_cache_registry][core]") {
    REQUIRE(&PrinterCacheRegistry::instance() == &PrinterCacheRegistry::instance());
    // The destroyed flag only flips during static destruction; it must read false
    // while the process is running so static-destruction guards are meaningful.
    REQUIRE_FALSE(PrinterCacheRegistry::is_destroyed());
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: one name registered by two owners is two entries",
                 "[printer_cache_registry][core]") {
    auto& reg = PrinterCacheRegistry::instance();
    int first_owner = 0;
    int second_owner = 0;
    int a = 0;
    int b = 0;
    reg.register_invalidator("Shared", [&a]() { a++; }, &first_owner);
    reg.register_invalidator("Shared", [&b]() { b++; }, &second_owner);
    REQUIRE(reg.count() == 2);

    // Re-registering by the same owner still replaces in place.
    reg.register_invalidator("Shared", [&a]() { a += 10; }, &first_owner);
    REQUIRE(reg.count() == 2);

    // One owner leaving takes only its own entry.
    REQUIRE_FALSE(reg.unregister("Shared"));
    REQUIRE(reg.unregister("Shared", &second_owner));
    reg.invalidate_all();
    CHECK(a == 10);
    CHECK(b == 0);
}

TEST_CASE_METHOD(PrinterCacheRegistryFixture,
                 "PrinterCacheRegistry: a PrinterState that dies without deinit leaves no "
                 "invalidator and spares another instance's",
                 "[printer_cache_registry][core][printer_state]") {
    auto& reg = PrinterCacheRegistry::instance();
    int survivor_fired = 0;
    int survivor_owner = 0;
    reg.register_invalidator(
        "PrinterState", [&survivor_fired]() { survivor_fired++; }, &survivor_owner);

    {
        // MockPrinter's shape: initialized, then destroyed with its subjects still up.
        helix::PrinterState state;
        state.init_subjects(false);
        REQUIRE(reg.count() == 2);
    }

    REQUIRE(reg.count() == 1);
    reg.invalidate_all();
    CHECK(survivor_fired == 1);
}
