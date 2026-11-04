// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/holding_motion_api.h"
#include "app_globals.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "toolhead_move_sender.h"

#include "../catch_amalgamated.hpp"

using helix::AxisBounds;
using helix::AxisMove;
using helix::AxisTarget;
using helix::HoldingAPI;
using helix::ToolheadMoveSender;

namespace {

AxisBounds bounds_xyz() {
    AxisBounds b;
    b.has_x = b.has_y = b.has_z = true;
    b.x_min = 0.0f;
    b.x_max = 300.0f;
    b.y_min = 0.0f;
    b.y_max = 300.0f;
    b.z_min = 0.0f;
    b.z_max = 250.0f;
    return b;
}

AxisTarget xy(double x, double y) {
    AxisTarget t;
    t.x = x;
    t.y = y;
    return t;
}

class SenderFixture : public LVGLTestFixture {
  public:
    SenderFixture()
        : sender_(lifetime_, "SenderTest",
                  {[this]() { ++acks_; }, [this](const MoonrakerError&) { ++errors_; },
                   [this](const helix::JogCoalescer::CoalescedMove&) { return may_flush_; }}) {}

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void ack(size_t i) {
        held()[i].on_success();
        drain();
    }

    std::vector<helix::HoldingMotionAPI::Held>& held() {
        return api_.holding_.held;
    }

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    HoldingAPI api_{client_, get_printer_state()};
    helix::AsyncLifetimeGuard lifetime_;
    int acks_ = 0;
    int errors_ = 0;
    bool may_flush_ = true;
    ToolheadMoveSender sender_;
};

} // namespace

TEST_CASE_METHOD(SenderFixture, "move sender: one target in flight, the newest sent on ack",
                 "[move_sender]") {
    REQUIRE(sender_.send_target(&api_, xy(10, 10), bounds_xyz(), 5.0));
    REQUIRE(held().size() == 1);
    CHECK(sender_.send_target(&api_, xy(20, 20), bounds_xyz(), 5.0));
    CHECK(sender_.send_target(&api_, xy(30, 30), bounds_xyz(), 5.0));
    CHECK(held().size() == 1);

    ack(0);
    REQUIRE(held().size() == 2);
    CHECK(*held()[1].target.x == Catch::Approx(30.0));
    CHECK(*held()[1].target.y == Catch::Approx(30.0));
    CHECK(acks_ == 1);

    ack(1);
    CHECK(held().size() == 2);
    CHECK_FALSE(sender_.coalescer().in_flight());
}

TEST_CASE_METHOD(SenderFixture, "move sender: a delta after a pending target discards it",
                 "[move_sender]") {
    REQUIRE(sender_.send_target(&api_, xy(10, 10), bounds_xyz(), 5.0));
    REQUIRE(sender_.send_target(&api_, xy(20, 20), bounds_xyz(), 5.0));
    REQUIRE(sender_.send_delta(&api_, AxisMove{1.0, 0.0, 0.0}));

    ack(0);
    CHECK(held().size() == 1);
    REQUIRE(api_.holding_.held_relative.size() == 1);
    CHECK(api_.holding_.held_relative[0].dx == Catch::Approx(1.0));
}

TEST_CASE_METHOD(SenderFixture, "move sender: an error resets, reports once, and frees the queue",
                 "[move_sender]") {
    REQUIRE(sender_.send_target(&api_, xy(10, 10), bounds_xyz(), 5.0));
    REQUIRE(sender_.send_target(&api_, xy(20, 20), bounds_xyz(), 5.0));
    MoonrakerError err;
    err.message = "Move out of range";
    held()[0].on_error(err);
    drain();
    CHECK(errors_ == 1);
    CHECK_FALSE(sender_.coalescer().in_flight());
    // The pending target went with the reset.
    CHECK(held().size() == 1);

    REQUIRE(sender_.send_target(&api_, xy(40, 40), bounds_xyz(), 5.0));
    REQUIRE(held().size() == 2);
    CHECK(*held()[1].target.x == Catch::Approx(40.0));
}

TEST_CASE_METHOD(SenderFixture, "move sender: a Z target with no Z bounds is refused",
                 "[move_sender]") {
    auto bounds = bounds_xyz();
    bounds.has_z = false;
    AxisTarget t = xy(10, 10);
    t.z = 20.0;
    CHECK_FALSE(sender_.send_target(&api_, t, bounds, 5.0));
    CHECK(held().empty());
    CHECK_FALSE(sender_.coalescer().in_flight());
}

TEST_CASE_METHOD(SenderFixture, "move sender: a target outside the bounds is sent clamped",
                 "[move_sender]") {
    AxisTarget t = xy(-50, 400);
    t.z = 900.0;
    REQUIRE(sender_.send_target(&api_, t, bounds_xyz(), 5.0));
    REQUIRE(held().size() == 1);
    CHECK(*held()[0].target.x == Catch::Approx(0.0));
    CHECK(*held()[0].target.y == Catch::Approx(300.0));
    CHECK(*held()[0].target.z == Catch::Approx(250.0));
    // The script starts from the reported Z.
    REQUIRE(held()[0].start_z.has_value());
    CHECK(*held()[0].start_z == Catch::Approx(5.0));
}

TEST_CASE_METHOD(SenderFixture, "move sender: the next target starts at the in-flight Z",
                 "[move_sender]") {
    CHECK(sender_.target_start_z(5.0) == Catch::Approx(5.0));
    AxisTarget lift = xy(10, 10);
    lift.z = 43.0;
    REQUIRE(sender_.send_target(&api_, lift, bounds_xyz(), 5.0));
    CHECK(sender_.target_start_z(5.0) == Catch::Approx(43.0));

    // A target sent behind it starts its script where the lift leaves the head.
    REQUIRE(sender_.send_target(&api_, xy(20, 20), bounds_xyz(), 5.0));
    ack(0);
    REQUIRE(held().size() == 2);
    REQUIRE(held()[1].start_z.has_value());
    CHECK(*held()[1].start_z == Catch::Approx(43.0));
}

TEST_CASE_METHOD(SenderFixture, "move sender: no API sends nothing", "[move_sender]") {
    CHECK_FALSE(sender_.send_target(nullptr, xy(10, 10), bounds_xyz(), 5.0));
    CHECK_FALSE(sender_.send_delta(nullptr, AxisMove{1.0, 0.0, 0.0}));
    CHECK_FALSE(sender_.coalescer().in_flight());
}

TEST_CASE_METHOD(SenderFixture, "move sender: a coalesced move is dropped once moves stop",
                 "[move_sender]") {
    REQUIRE(sender_.send_target(&api_, xy(10, 10), bounds_xyz(), 5.0));
    REQUIRE(sender_.send_target(&api_, xy(20, 20), bounds_xyz(), 5.0));
    REQUIRE(held().size() == 1);

    may_flush_ = false;
    ack(0);
    CHECK(held().size() == 1);
    CHECK(acks_ == 1);
    CHECK_FALSE(sender_.coalescer().in_flight());

    // With the gate open again, the next target goes straight out.
    may_flush_ = true;
    REQUIRE(sender_.send_target(&api_, xy(30, 30), bounds_xyz(), 5.0));
    CHECK(held().size() == 2);
}
