// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_notification.h"

#include "../../fake_moonraker_client.h"
#include "../../lvgl_ui_test_fixture.h"
#include "../../test_helpers/gcode_error_router_test_access.h"
#include "../../test_helpers/print_state_test_drivers.h"
#include "../../ui_test_utils.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "error_classify.h"
#include "fault_surface_correlation.h"
#include "gcode_error_router.h"
#include "helix_test_fixture.h"
#include "printer_discovery.h"
#include "printer_event_journal.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"

#include <string>
#include <utility>
#include <vector>

#include "../../catch_amalgamated.hpp"

using helix::GcodeErrorRouter;

namespace {
struct GcodeErrorRouterTest : public HelixTestFixture {};
} // namespace

// clean_error_text must handle K2's two `!!` payload shapes.
//
// Shape 1 — pure JSON (CFS box driver errors, observed key849):
//   `{"code":"key849","msg":"retrude error...","values":[1,"A"]}`
//
// Shape 2 — embedded JSON after a prefix (Klipper connect failure on
// MCU shutdown, observed K2 Plus 2026-05-24 in gcode_store as):
//   `Internal error during connect: !{"code":"key298","msg":"Can not
//    update MCU rpi config as it is shutdown","values":["rpi"]}`
//   `Once the underlying issue is corrected, use the "RESTART"...`
//
// Both shapes must surface `code` so process_line can route to the
// right modal/recovery branch.

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text extracts pure JSON shape",
                 "[gcode_error_router]") {
    std::string text =
        R"({"code":"key849","msg":"retrude error, failed to exit connections","values":[1,"A"]})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key849");
    // CFS decoder rewrites to friendly text — exact phrasing depends on
    // CFS_ERROR_TABLE entry; just check the slot locator was spliced in.
    REQUIRE(text.find("unit 1 slot A") != std::string::npos);
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text passes the firmware msg to key843",
                 "[gcode_error_router][1387]") {
    // key843's table text asserts one cause ("can't read the tag") for a code
    // whose causes vary; most often the box answering busy while
    // mid-operation (#1387). The firmware's own msg carries the real cause, so
    // the decoder must receive it instead of dropping it on the table hit.
    std::string text =
        R"({"code":"key843","msg":"read rfid failed, box is busy","values":[1,"B"]})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key843");
    REQUIRE(text.find("box is busy") != std::string::npos);
    REQUIRE(text.find("Can't read filament RFID tag") == std::string::npos);
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text extracts embedded JSON after prefix",
                 "[gcode_error_router]") {
    // Exact shape observed on K2 Plus 2026-05-24 when klipper_mcu was
    // shutdown. The `!` between the prefix and the JSON is Klipper's
    // own emit convention — we don't reproduce it on output.
    std::string text =
        R"(Internal error during connect: !{"code":"key298","msg":"Can not update MCU rpi config as it is shutdown","values":["rpi"]})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key298");
    // CFS decoder has a key298 entry — friendly translation should win
    // over the raw msg.
    REQUIRE(text.find("MCU bridge") != std::string::npos);
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text handles embedded JSON with trailing comment garbage",
                 "[gcode_error_router]") {
    // Real K2 emission: the JSON is followed by Klipper's standard
    // "Once the underlying issue is corrected..." footer text.
    std::string text =
        R"(Internal error during connect: !{"code":"key298","msg":"Can not update MCU rpi config as it is shutdown","values":["rpi"]}
//
// Once the underlying issue is corrected, use the "RESTART"
// command to reload the config and restart the host software.)";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    // Brace-balance must terminate at the JSON's close brace and ignore
    // the trailing comment lines.
    REQUIRE(code == "key298");
    // A table hit replaces the whole text with the curated wording: spliced
    // into the firmware's prose it would mix languages, and the toast's
    // truncation could cut the hint.
    REQUIRE(text == "MCU bridge daemon is shut down. Tap Firmware Restart to recover — on K2 "
                    "this also bounces the rpi MCU bridge");
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text keeps the prose around an envelope the table does not know",
                 "[gcode_error_router][firmware_error_envelope]") {
    std::string text =
        R"(Internal error during connect: !{"code":"key77777","msg":"Bridge gone"}
Once the underlying issue is corrected, use the "RESTART" command.)";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key77777");
    REQUIRE(text == "Internal error during connect: Bridge gone\nOnce the underlying issue is "
                    "corrected, use the \"RESTART\" command.");
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text leaves a table miss with no msg untouched",
                 "[gcode_error_router][firmware_error_envelope]") {
    const std::string raw = R"(Internal error: {"code":"key77777","values":[1]} tail)";
    std::string text = raw;
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key77777");
    REQUIRE(text == raw);
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text decodes a U1 shutdown envelope",
                 "[gcode_error_router][firmware_error_envelope]") {
    std::string text =
        R"({"coded": "0003-0522-0002-0008", "oneshot": 0, "msg":"Lost communication with MCU 'e0'"})"
        "\nOnce the underlying issue is corrected, use the\n"
        "\"FIRMWARE_RESTART\" command to reset the firmware, reload the\n"
        "config, and restart the host software.\n"
        "Printer is shutdown\n";
    std::string code;
    helix::FirmwareCodeScheme scheme = helix::FirmwareCodeScheme::ErrorTable;
    GcodeErrorRouter::clean_error_text(text, code, &scheme);

    REQUIRE(code == "0003-0522-0002-0008");
    REQUIRE(scheme == helix::FirmwareCodeScheme::Opaque);
    REQUIRE(text.rfind("Lost communication with MCU 'e0'\nOnce the underlying issue", 0) == 0);
    REQUIRE(text.find("Printer is shutdown") != std::string::npos);
    REQUIRE(text.find('{') == std::string::npos);
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text tolerates spaces in a K2 envelope",
                 "[gcode_error_router][firmware_error_envelope]") {
    std::string text = R"({ "code" : "key9001", "msg" : "MCU 'mcu' shutdown" })";
    std::string code;
    helix::FirmwareCodeScheme scheme = helix::FirmwareCodeScheme::Opaque;
    GcodeErrorRouter::clean_error_text(text, code, &scheme);

    REQUIRE(code == "key9001");
    REQUIRE(scheme == helix::FirmwareCodeScheme::ErrorTable);
    REQUIRE(text == "MCU 'mcu' shutdown");
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text never looks an opaque code up in the error table",
                 "[gcode_error_router][firmware_error_envelope]") {
    // A dashed code that happened to spell a table key must still show the
    // firmware's own msg: only an error-table envelope indexes the table.
    std::string text = R"({"coded":"key298","msg":"firmware wording"})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key298");
    REQUIRE(text == "firmware wording");
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text falls through when text has no code",
                 "[gcode_error_router]") {
    std::string text = "Generic Klipper error with no code marker";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code.empty());
    // Text untouched (no heuristic matches either).
    REQUIRE(text == "Generic Klipper error with no code marker");
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "a must-home rejection reads worded on the console path, not raw",
                 "[gcode_error_router]") {
    // clean_error_text only decodes envelopes; Klipper's prose is worded by the
    // error_wording table the console classifier asks.
    std::string text = "Must home axis first";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);
    REQUIRE(code.empty());
    CHECK(text == "Must home axis first");

    helix::ClassifyContext ctx;
    const auto e = helix::error_classify::classify("!! Must home axis first", ctx);
    REQUIRE(e.has_value());
    CHECK_FALSE(e->title.empty());
    CHECK(e->detail != "Must home axis first");
    // The dedup identity stays Klipper's own line.
    CHECK(e->raw_detail == "Must home axis first");
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text recovers from malformed JSON after prefix",
                 "[gcode_error_router]") {
    // Embedded `{"code":` substring but with broken JSON (missing close
    // quote). Brace-balance can still find an end, but parse must throw
    // and we fall through without leaking partial state.
    std::string text = R"(Internal error: {"code":"key298, "msg":"broken"})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code.empty());
    // Text is unmodified because parse threw before the friendly rewrite.
    REQUIRE(text.find("Internal error") != std::string::npos);
}

// Replay age gate (#991). On (re)connect we replay the last latched `!!`
// from the gcode_store, but a stale error (e.g. surfaced 287s after a UI
// restart mid-paused-print) must NOT be re-shown as a blocking modal.
// should_surface_replay encodes the decision: fresh -> surface, stale ->
// suppress, unknown age -> surface (never suppress on missing data).

TEST_CASE_METHOD(GcodeErrorRouterTest, "should_surface_replay surfaces a fresh error",
                 "[gcode_error_router]") {
    const double now = 1'000'000.0;
    // 5s old — well within the 30s window; the brief-reconnect case.
    REQUIRE(GcodeErrorRouter::should_surface_replay(now - 5.0, now));
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "should_surface_replay surfaces an error exactly at the threshold",
                 "[gcode_error_router]") {
    const double now = 1'000'000.0;
    // age == 30s is still considered fresh (<=, not <).
    REQUIRE(GcodeErrorRouter::should_surface_replay(now - 30.0, now));
}

TEST_CASE_METHOD(GcodeErrorRouterTest, "should_surface_replay suppresses a stale error",
                 "[gcode_error_router]") {
    const double now = 1'000'000.0;
    // 287s old — the exact #991 stale-after-restart case. Must suppress.
    REQUIRE_FALSE(GcodeErrorRouter::should_surface_replay(now - 287.0, now));
    // And anything just past the boundary.
    REQUIRE_FALSE(GcodeErrorRouter::should_surface_replay(now - 30.001, now));
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "should_surface_replay surfaces when the timestamp is unavailable",
                 "[gcode_error_router]") {
    const double now = 1'000'000.0;
    // entry_time == 0 (GcodeStoreEntry default / absent timestamp): age is
    // not positively determinable, so we must NOT suppress — a real fresh
    // error with a missing stamp would otherwise be silently swallowed.
    REQUIRE(GcodeErrorRouter::should_surface_replay(0.0, now));
    REQUIRE(GcodeErrorRouter::should_surface_replay(-1.0, now));
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "should_surface_replay treats negative age (clock skew) as fresh",
                 "[gcode_error_router]") {
    const double now = 1'000'000.0;
    // Entry stamped slightly in the future (NTP step / skew): negative age
    // must not be mistaken for stale.
    REQUIRE(GcodeErrorRouter::should_surface_replay(now + 10.0, now));
}

TEST_CASE("truncate_for_toast keeps a message within the bound and cuts on a codepoint",
          "[gcode_error_router][errors][toast_cap]") {
    constexpr size_t kMax = GcodeErrorRouter::kToastTextMaxBytes;
    const std::string fits(kMax, 'a');
    CHECK(GcodeErrorRouter::truncate_for_toast(fits) == fits);

    const std::string cut = GcodeErrorRouter::truncate_for_toast(std::string(kMax + 1, 'a'));
    CHECK(cut == std::string(kMax - 3, 'a') + "...");

    // Two-byte "é" and three-byte "混" runs whose plain byte cut lands mid-codepoint.
    for (const auto& [lead, glyph] : {std::pair<std::string, std::string>{"", "é"},
                                      std::pair<std::string, std::string>{"x", "混"}}) {
        std::string text = lead;
        while (text.size() <= kMax) {
            text += glyph;
        }
        const std::string out = GcodeErrorRouter::truncate_for_toast(text);
        INFO("glyph " << glyph << " -> " << out.size() << " bytes");
        REQUIRE(out.size() <= kMax);
        REQUIRE(out.size() >= kMax - 3 - glyph.size() + 1);
        REQUIRE(out.substr(out.size() - 3) == "...");
        const std::string kept = out.substr(0, out.size() - 3);
        CHECK(kept.compare(0, lead.size(), lead) == 0);
        CHECK((kept.size() - lead.size()) % glyph.size() == 0);
    }
}

namespace {

/// A router as the app wires it, over a printer whose discovery a case sets,
/// with every notification the router raises recorded.
class RouterSurfaceFixture : public LVGLUITestFixture {
  public:
    RouterSurfaceFixture() {
        helix::fault_surface_correlation::clear_for_test();
        helix::ui::set_test_notification_event_hook(
            [this](const helix::ui::NotificationEvent& e) { events.push_back(e); });
    }
    ~RouterSurfaceFixture() override {
        helix::ui::set_test_notification_event_hook(nullptr);
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
        helix::ui::UpdateQueue::instance().drain();
        helix::fault_surface_correlation::clear_for_test();
    }

    static helix::PrinterDiscovery qidi_box_printer() {
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array(
            {"box_extras", "box_stepper slot0", "box_stepper slot1", "extruder", "heater_bed"}));
        return hw;
    }

    std::vector<helix::ui::NotificationEvent> errors() const {
        std::vector<helix::ui::NotificationEvent> out;
        for (const auto& e : events) {
            if (e.severity == ToastSeverity::ERROR) {
                out.push_back(e);
            }
        }
        return out;
    }

    std::vector<helix::ui::NotificationEvent> events;
};

} // namespace

TEST_CASE_METHOD(RouterSurfaceFixture, "a QIDI pause code raised mid-print blocks at once",
                 "[gcode_error_router][qidi]") {
    get_printer_state().set_hardware(qidi_box_printer());
    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::process_line(
        router, "!!Code:QDE_004_005; Message:Please load the filament to 1B first.");
    helix::ui::UpdateQueue::instance().drain();

    // The blocking printer-fault alert, not a held toast: QIDI's printing
    // severity reaches the router.
    REQUIRE(errors().size() == 1);
    CHECK(errors()[0].title.find("Slot 2") != std::string::npos);
    process_lvgl(250);
    CHECK(events.size() == 1);
    CHECK(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 0);
}

TEST_CASE_METHOD(RouterSurfaceFixture,
                 "an uncoded fault on QIDI firmware blocks at once with no twin hold",
                 "[gcode_error_router][qidi]") {
    // QIDI sends no code-stripped prose copy of its coded faults, so nothing
    // is worth waiting for.
    get_printer_state().set_hardware(qidi_box_printer());
    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::process_line(router,
                                             "!! Move out of range: 300.000 0.000 10.000 [0.000]");
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(errors().size() == 1);
    process_lvgl(250);
    CHECK(events.size() == 1);
    CHECK(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 0);
}

TEST_CASE_METHOD(RouterSurfaceFixture, "a replayed fault reads with the wording a live one gets",
                 "[gcode_error_router][replay]") {
    helix::test::FakeMoonrakerClient client;
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    GcodeErrorRouter router(nullptr, &client, presenter);
    GcodeErrorRouterTestAccess::on_connected(router);
    REQUIRE(client.last_gcode_store_cb);

    client.last_gcode_store_cb({{"!! Must home axis first: 0.000 0.000 0.000 [0.000]",
                                 helix::wall_now_s() - 5, "response"}});
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(errors().size() == 1);
    CHECK(errors()[0].title == "The printer needs homing first");
    CHECK(errors()[0].message.find("A move was sent before the axes were homed") !=
          std::string::npos);
}
