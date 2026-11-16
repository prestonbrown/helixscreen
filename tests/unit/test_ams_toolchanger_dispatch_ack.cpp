// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_toolchanger_dispatch_ack.cpp
 * @brief klipper-toolchanger optimistic dispatch + macro-ack resolution (#1183).
 *
 * The AFC half of #1183 was fixed by resolving the operation on the macro's
 * gcode ack instead of waiting for a status transition that a no-op never
 * produces. klipper-toolchanger was never given the same treatment, and it has
 * the same hole for the same reason.
 *
 * change_tool() stamps AmsAction::SELECTING optimistically and sends
 * `SELECT_TOOL TOOL="tool T<n>"` through the ack-discarding 1-arg execute_gcode(). The
 * only code that can move the action off SELECTING is parse_toolchanger_state()
 * receiving a frame carrying `toolchanger.status`. Moonraker only pushes fields
 * whose VALUE CHANGED, and klipper-toolchanger short-circuits a SELECT_TOOL on
 * the tool already on the carriage ("Tool tool T4 already selected") without
 * touching `status`. No frame is sent, the action never leaves SELECTING, and
 * two things follow:
 *
 *   - The filament panel's completion observer only fires on IDLE/ERROR, so the
 *     Load button spins until OperationTimeoutGuard expires at 120 s.
 *   - is_busy() is `action != IDLE && action != ERROR`, and
 *     check_preconditions() refuses on it, so EVERY subsequent AMS operation
 *     returns busy("Selecting") until the app is restarted.
 *
 * Observed on a 5-tool toolchanger, v0.99.106, debug bundle 9KRXZ62P: T4 was
 * already the mounted tool, `[AMS ToolChanger] G-code executed successfully`
 * came back in 1 ms, and the action never changed again across the following
 * four minutes of log.
 *
 * The ack is the only completion signal that exists for a no-op, which is
 * exactly what AmsBackendAfc::finalize_dispatch_after_macro() concluded.
 */

#include "ams_backend_toolchanger.h"
#include "ams_types.h"
#include "core/update_queue.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// Real backend with the gcode ack held so the test decides when the macro
/// "completes". client_ is null, so ensure_homed_then() routes straight to
/// execute_gcode() and these overrides capture everything.
class ToolChangerDispatchHelper : public helix::AmsBackendToolChanger {
  public:
    explicit ToolChangerDispatchHelper(int tool_count)
        : helix::AmsBackendToolChanger(nullptr, nullptr) {
        std::vector<std::string> names;
        names.reserve(static_cast<size_t>(tool_count));
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));

        // check_preconditions() refuses everything while the backend is stopped.
        running_ = true;

        set_event_callback([this](const std::string& event, const std::string&) {
            if (event == EVENT_STATE_CHANGED) {
                // Safe only because change_tool() emits OUTSIDE mutex_.
                trace_.push_back(get_current_action());
            }
        });
    }

    ~ToolChangerDispatchHelper() override {
        // Any dispatch left un-acked still queued work; draining keeps
        // scripts/check_update_queue_leaks.py quiet.
        helix::ui::UpdateQueue::instance().drain();
    }

    helix::AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        pending_acks_.emplace_back(nullptr); // keep indices aligned with sent_
        return helix::AmsErrorHelper::success();
    }

    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete) override {
        sent_.push_back(gcode);
        pending_acks_.push_back(std::move(on_complete));
        return helix::AmsErrorHelper::success();
    }
    // A dispatch that unwinds on a rejection sends through this form.
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                                  bool /*silent*/) override {
        return execute_gcode(gcode, std::move(on_complete));
    }

    /// Fire the ack for the Nth dispatched gcode, then drain — the production
    /// callback arrives on a background thread and hops to the main thread via
    /// LifetimeToken::defer, so the work lands in the queue, not inline.
    void ack(size_t index) {
        REQUIRE(index < pending_acks_.size());
        REQUIRE(pending_acks_[index] != nullptr);
        pending_acks_[index]();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// True when the Nth dispatch went out through the completion-callback form.
    [[nodiscard]] bool ack_available(size_t index) const {
        return index < pending_acks_.size() && pending_acks_[index] != nullptr;
    }

    /// Feed one notify_status_update params object.
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    /// Put tool `n` on the carriage with the toolchanger idle.
    void seat_tool(int n) {
        feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", n}}}});
        trace_.clear(); // seeding is not part of the assertion
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }
    [[nodiscard]] const std::vector<helix::AmsAction>& trace() const {
        return trace_;
    }
    [[nodiscard]] helix::AmsAction action() const {
        return get_current_action();
    }

  private:
    std::vector<std::string> sent_;
    std::vector<helix::SharedCallback<void()>> pending_acks_;
    std::vector<helix::AmsAction> trace_;
};

bool trace_contains(const std::vector<helix::AmsAction>& trace, helix::AmsAction a) {
    return std::find(trace.begin(), trace.end(), a) != trace.end();
}

} // namespace

// =============================================================================
// The reported defect: a no-op SELECT_TOOL must still end the operation
// =============================================================================

TEST_CASE("Toolchanger mount of the already-mounted tool resolves on the macro ack",
          "[ams][toolchanger][dispatch][1183]") {
    ToolChangerDispatchHelper h(5);
    h.seat_tool(4); // T4 already on the carriage — SELECT_TOOL of T4 is a firmware no-op

    REQUIRE(h.change_tool(4).success());
    REQUIRE(h.sent().size() == 1);
    CHECK(h.sent()[0] == "SELECT_TOOL TOOL=\"tool T4\"");

    // The optimistic busy leg is what the filament panel's completion observer
    // needs to see START before it can ever see one end.
    CHECK(trace_contains(h.trace(), helix::AmsAction::SELECTING));

    // The dispatch must go out through the completion-callback form. Today it
    // uses the 1-arg overload and discards the ack, so there is nothing to fire.
    REQUIRE(h.ack_available(0));

    h.ack(0);

    // Asserting the end value alone would pass without the fix — IDLE is where
    // this started. The busy leg above is the load-bearing half of the pair.
    CHECK(h.trace().back() == helix::AmsAction::IDLE);
    CHECK(h.action() == helix::AmsAction::IDLE);
}

TEST_CASE("Toolchanger no-op mount does not lock out the next operation",
          "[ams][toolchanger][dispatch][1183]") {
    ToolChangerDispatchHelper h(5);
    h.seat_tool(4);

    REQUIRE(h.change_tool(4).success());
    REQUIRE(h.ack_available(0));
    h.ack(0);

    // is_busy() is `action != IDLE && action != ERROR`, and check_preconditions()
    // refuses on it. Leaving the action at SELECTING makes every later op fail
    // with busy("Selecting") until restart — the half of this bug that outlives
    // the 120 s UI guard.
    REQUIRE_FALSE(h.get_system_info().is_busy());

    helix::AmsError second = h.change_tool(2);
    CHECK(second.success());
    CHECK(h.sent().size() == 2);
}

// =============================================================================
// Guard rails on the fix itself
// =============================================================================

TEST_CASE("Toolchanger macro ack does not truncate a real tool change",
          "[ams][toolchanger][dispatch][1183]") {
    ToolChangerDispatchHelper h(5);
    h.seat_tool(0);

    REQUIRE(h.change_tool(3).success());
    REQUIRE(h.ack_available(0));

    // Firmware picked the change up and is mid-swap. SELECT_TOOL's ack lands
    // when the macro returns, which for a real change is BEFORE the carriage
    // finishes; resolving on it here would report done while the tool is still
    // moving.
    h.feed(json{{"toolchanger", {{"status", "changing"}}}});
    REQUIRE(h.action() == helix::AmsAction::SELECTING);

    h.ack(0);
    CHECK(h.action() == helix::AmsAction::SELECTING);

    // Only the firmware's own terminal frame ends it.
    h.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 3}}}});
    CHECK(h.action() == helix::AmsAction::IDLE);
}

TEST_CASE("Toolchanger never takes the swap arm, so a remap cannot mount the wrong tool",
          "[ams][toolchanger][dispatch][1199]") {
    // PARALLEL topology: every tool is its own independent path, so loading one
    // never requires unloading another. The base needs_unload_before_load() rule
    // is SERIAL and, because current_slot tracks the mounted tool, answered true
    // permanently here.
    //
    // That matters because the swap arm dispatches change_tool(mapped_tool),
    // while change_tool() validates its argument as a SLOT and emits
    // `SELECT_TOOL TOOL="tool <name>"`, naming the physical tool, to bypass
    // ASSIGN_TOOL remapping: a tap mounts the tool the user tapped, not whatever
    // the slicer's T{n} command was remapped to. Feeding it a remapped number would
    // mount the wrong toolhead. Answering false keeps loads on the plain arm,
    // where the argument is the slot the user actually tapped.
    ToolChangerDispatchHelper h(5);
    h.seat_tool(0);

    helix::AmsSystemInfo seated = h.get_system_info();
    REQUIRE(seated.current_slot == 0); // a tool is always on the carriage
    CHECK_FALSE(h.needs_unload_before_load(seated, /*target_slot=*/3));

    REQUIRE(h.change_tool(3).success());
    REQUIRE(h.sent().size() == 1);
    CHECK(h.sent()[0] == "SELECT_TOOL TOOL=\"tool T3\""); // the tapped tool, whatever the map says
}

TEST_CASE("Toolchanger dispatch failure reverts the optimistic action",
          "[ams][toolchanger][dispatch][1183]") {
    ToolChangerDispatchHelper h(5);
    h.seat_tool(1);

    // An out-of-range tool never reaches the wire.
    helix::AmsError err = h.change_tool(99);
    CHECK_FALSE(err.success());
    CHECK(h.sent().empty());
    CHECK(h.action() == helix::AmsAction::IDLE);
    CHECK_FALSE(h.get_system_info().is_busy());
}
