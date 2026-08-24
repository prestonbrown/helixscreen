// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_idle_timeout_debounce_e2e.cpp
 * @brief The idle_timeout debounce, driven the way Moonraker drives it.
 *
 * test_idle_timeout_busy.cpp covers the settle logic as pure arithmetic, and the
 * BusyGuardFixture cases cover the gate with the window already elapsed. Neither
 * exercises the seam between them: that PrinterCalibrationState::update_from_status
 * actually feeds IdleTimeoutBusy, and that the g-code guard reads the debounced
 * view rather than the raw subject. Wiring either of those to the wrong place
 * would leave both existing suites green.
 *
 * So this drives real status payloads and asserts on the real refusal path.
 * Tagged [slow] because it waits out a genuine 1 s settle; CI runs the [slow]
 * job separately (nightly.yml), so it stays covered without slowing iteration.
 */

#include "../busy_guard_fixture.h"

#include "../catch_amalgamated.hpp"

namespace {

nlohmann::json idle_timeout(const char* state) {
    return nlohmann::json{{"idle_timeout", {{"state", state}}}};
}

} // namespace

TEST_CASE_METHOD(helix::BusyGuardFixture,
                 "A fresh idle_timeout Printing does not block until it settles",
                 "[blocking_op][idle_timeout][e2e][slow]") {
    state.update_from_status(idle_timeout("Printing"));

    // Anti-vacuity: the payload MUST have been parsed. Without this the next
    // assertion would also hold if update_from_status ignored idle_timeout
    // entirely, which is the failure this test exists to catch.
    REQUIRE(lv_subject_get_int(state.get_idle_timeout_printing_subject()) == 1);

    // Parsed, but not yet blocking — the whole point of the debounce.
    CHECK_FALSE(state.is_blocking_operation_active());
    CHECK_FALSE(state.is_external_blocking_operation_active());

    SECTION("and then does block once the window elapses") {
        REQUIRE(wait_until([this] { return state.is_blocking_operation_active(); }, 3000));
        CHECK(state.is_external_blocking_operation_active());

        // The real consumer: a discretionary jog is refused, and never reaches
        // the client. No app-initiated motion has happened in this test, so the
        // self-busy carve-out in is_external_blocking_operation_active() cannot
        // be what allows or refuses it.
        api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK(error_called);
        CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
        CHECK(mock_client.gcode_script_history().empty());
    }

    SECTION("a jog inside the settle window still goes through") {
        // The false-refusal this fixes: on a printer whose housekeeping macros
        // hold idle_timeout for under a second, this is every jog the user makes.
        api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        CHECK_FALSE(mock_client.gcode_script_history().empty());
    }
}

TEST_CASE_METHOD(helix::BusyGuardFixture,
                 "A short housekeeping burst never reaches the blocking state",
                 "[blocking_op][idle_timeout][e2e][regression][slow]") {
    // The L53W5PKG shape end to end: Printing, then Ready ~0.7 s later, forever.
    // Replayed at full speed here — the burst is shorter than the settle, so the
    // guard must never engage no matter how many cycles run.
    for (int cycle = 0; cycle < 3; ++cycle) {
        INFO("cycle " << cycle);
        state.update_from_status(idle_timeout("Printing"));
        REQUIRE(lv_subject_get_int(state.get_idle_timeout_printing_subject()) == 1);
        CHECK_FALSE(state.is_blocking_operation_active());

        state.update_from_status(idle_timeout("Ready"));
        REQUIRE(lv_subject_get_int(state.get_idle_timeout_printing_subject()) == 0);
        CHECK_FALSE(state.is_blocking_operation_active());
    }

    // And a jog still works after all of it.
    api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });
    CHECK_FALSE(error_called);
    CHECK_FALSE(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(helix::BusyGuardFixture, "Clearing idle_timeout releases the guard immediately",
                 "[blocking_op][idle_timeout][e2e][slow]") {
    // Asymmetric by design: no settle on the way down. Making the user wait an
    // extra second after a real homing finishes would be its own bug.
    state.update_from_status(idle_timeout("Printing"));
    REQUIRE(wait_until([this] { return state.is_blocking_operation_active(); }, 3000));

    state.update_from_status(idle_timeout("Ready"));
    CHECK_FALSE(state.is_blocking_operation_active());

    api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });
    CHECK_FALSE(error_called);
    CHECK_FALSE(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(helix::BusyGuardFixture, "Repeated Printing reports do not re-arm the settle",
                 "[blocking_op][idle_timeout][e2e][regression][slow]") {
    // Moonraker re-sends idle_timeout in status batches. If every repeat
    // restarted the window, a printer reporting faster than SETTLE would never
    // reach the blocking state at all — the debounce would be a hole in the
    // guard rather than a filter. Driven through the parse path because that is
    // where the repeats actually arrive.
    const auto start = std::chrono::steady_clock::now();
    bool blocked = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        state.update_from_status(idle_timeout("Printing"));
        if (state.is_blocking_operation_active()) {
            blocked = true;
            break;
        }
        process_lvgl(20);
    }
    CHECK(blocked);
}
