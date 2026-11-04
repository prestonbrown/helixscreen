// tests/unit/test_snapmaker_error_classify.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "fault_surface_correlation.h"
#include "firmware_fault_codes.h"
#include "gcode_error_router.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "rpc_error_correlation.h"
#include "snapmaker_exceptions.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

PrinterDiscovery hardware_with_objects(const std::vector<std::string>& names) {
    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& n : names) {
        objects.push_back(n);
    }
    hw.parse_objects(objects);
    return hw;
}

PrinterDiscovery coded_firmware() {
    return hardware_with_objects({"exception_manager", "print_task_config"});
}

} // namespace

TEST_CASE("a known fault classifies from its code", "[snapmaker][classify]") {
    auto e = faultcodes::classify(coded_firmware(),
                                  "!! 0003-0530-0000-0011 The plate has not been removed");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("Remove the PEI sheet") != std::string::npos);
    REQUIRE(e->source == ErrorSource::SNAPMAKER);
}

TEST_CASE("classification does not depend on the firmware's wording", "[snapmaker][classify]") {
    // The whole point: the code is the identity. Firmware reworded, or a
    // locale we do not read, must still classify.
    auto e = faultcodes::classify(coded_firmware(), "!! 0003-0530-0000-0011 platen nicht entfernt");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("Remove the PEI sheet") != std::string::npos);
}

TEST_CASE("an unknown code keeps the firmware's own text", "[snapmaker][classify]") {
    auto e = faultcodes::classify(coded_firmware(),
                                  "!! 0002-0999-0000-0007 something we have no wording for");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("something we have no wording for") != std::string::npos);
}

TEST_CASE("a line with no code is left to the generic path", "[snapmaker][classify]") {
    REQUIRE_FALSE(faultcodes::classify(coded_firmware(), "!! Must home Z axis first").has_value());
}

TEST_CASE("a firmware without the code channel classifies nothing", "[snapmaker][classify]") {
    // The capability question gates this, not the vendor name: a printer that
    // does not publish exception_manager must fall through untouched even if a
    // line happens to look code-shaped.
    PrinterDiscovery plain = hardware_with_objects({"bed_mesh", "quad_gantry_level"});
    REQUIRE_FALSE(faultcodes::firmware_reports_fault_codes(plain));
    REQUIRE_FALSE(
        faultcodes::classify(plain, "!! 0003-0530-0000-0011 The plate has not been removed")
            .has_value());
}

TEST_CASE("a power-loss shutdown's coded payload classifies", "[snapmaker][classify]") {
    // The firmware reports a mains loss through invoke_shutdown with the code
    // in a JSON "coded" field, not through exception_manager. The decoder
    // scans anywhere in the line, so an error-prefixed line carrying that
    // payload reads our wording rather than an unrecognised shutdown's.
    auto e = faultcodes::classify(coded_firmware(),
                                  R"(error: {"coded": "0003-0522-0000-0017", )"
                                  R"("msg":"mcu: Power loss triggered", "oneshot": 0})");
    REQUIRE(e.has_value());
    REQUIRE(e->code == "0003-0522-0000-0017");
    REQUIRE(e->detail.find("Power was lost") != std::string::npos);
    REQUIRE(e->severity == ErrorSeverity::CRITICAL);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the router surfaces our wording for a coded fault",
                 "[snapmaker][classify]") {
    // Install the capability on the singleton the router reads, then drive the
    // REAL process_line. The surfaced record must carry OUR wording for the
    // fault: the generic classifier can only produce the firmware's own
    // sentence, so this is what pins the router's faultcodes pass.
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    REQUIRE_NOTHROW(GcodeErrorRouterTestAccess::process_line(
        router, "!! 0003-0530-0000-0011 The plate has not been removed"));
    helix::ui::UpdateQueue::instance().drain();

    const auto wording =
        snapmaker::exception_message(*snapmaker::decode_exception_code("0003-0530-0000-0011"));
    REQUIRE_FALSE(wording.empty());
    REQUIRE(fault_surface_correlation::was_recently_surfaced(std::string(wording)));

    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "the router decodes an unknown coded fault's envelope",
                 "[snapmaker][classify][firmware_error_envelope]") {
    // An unknown code keeps the firmware's text, which on this firmware is a
    // JSON envelope; the user must get its msg, not the raw object.
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    REQUIRE_NOTHROW(GcodeErrorRouterTestAccess::process_line(
        router,
        R"(!! {"coded": "0002-0999-0000-0007", "oneshot": 0, "msg":"no wording for this"})"));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(fault_surface_correlation::was_recently_surfaced("no wording for this"));

    get_printer_state().set_hardware(PrinterDiscovery{});
}

// ============================================================================
// Translation: the fault table holds the English source; the wording becomes
// user-facing at the detail/message assignment, which is where lv_tr() lives.
// ============================================================================

#include "translation_loader.h"

TEST_CASE_METHOD(LVGLUITestFixture, "the known fault wording reaches the user translated",
                 "[snapmaker][classify][i18n]") {
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    // The code is the identity, so a German-locale user classifies from the
    // code whatever the firmware's sentence says; the surfaced detail must be
    // the catalog's German sentence, which only happens if the table's
    // wording goes through lv_tr.
    auto e =
        faultcodes::classify(coded_firmware(), "!! 0003-0530-0000-0011 platten nicht entfernt");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("Entferne die PEI-Platte") != std::string::npos);

    lv_translation_set_language(helix::ui::kIdentityLocale);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a standing fault's message stays the firmware's own",
                 "[snapmaker][exceptions][i18n]") {
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    // The reader runs off the main thread (status frames arrive on the
    // WebSocket thread), so it must not translate: the wording becomes
    // user-facing only at the render path, which the classify i18n case above
    // and the standing-fault cases below pin.
    nlohmann::json s = nlohmann::json::object();
    s["exception_manager"] = {{"exceptions",
                               {{{"id", 530},
                                 {"index", 0},
                                 {"code", 11},
                                 {"level", 3},
                                 {"message", "The plate has not been removed"},
                                 {"is_persistent", 0}}}}};
    const auto v = snapmaker::read_active_exceptions(s);
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].message == "The plate has not been removed");

    lv_translation_set_language(helix::ui::kIdentityLocale);
}

// ============================================================================
// Standing faults: the exception_manager list reaches the screen through the
// same classify -> wording -> presentation path a console line takes.
// ============================================================================

namespace {

nlohmann::json standing_frame(std::vector<nlohmann::json> entries) {
    return {{"exception_manager", {{"exceptions", std::move(entries)}}}};
}

nlohmann::json power_loss_entry() {
    // The payload the hardware raises for a mains loss during a print: level 1.
    return {{"id", 522},
            {"index", 0},
            {"code", 17},
            {"level", 1},
            {"message", "exception id:522 index:0 code:17 power loss"},
            {"is_persistent", 0}};
}

/// The notification shape register_notify_update delivers: params[0] is the
/// status delta, params[1] the eventtime.
nlohmann::json notify_frame(const nlohmann::json& status) {
    return {{"method", "notify_status_update"}, {"params", nlohmann::json::array({status, 0.0})}};
}

} // namespace

TEST_CASE("read_standing_faults tells silent apart from empty", "[snapmaker][standing]") {
    const PrinterDiscovery hw = coded_firmware();

    // A delta frame that omits the fault object says nothing about faults.
    nlohmann::json delta = nlohmann::json::object();
    delta["toolhead"] = {{"homed_axes", "xyz"}};
    REQUIRE_FALSE(faultcodes::read_standing_faults(hw, delta).has_value());

    // An explicit empty list reports that none stand.
    const auto empty = faultcodes::read_standing_faults(hw, standing_frame({}));
    REQUIRE(empty.has_value());
    REQUIRE(empty->empty());

    // A printer without the capability never speaks for the frame either.
    const PrinterDiscovery plain = hardware_with_objects({"bed_mesh", "quad_gantry_level"});
    REQUIRE_FALSE(
        faultcodes::read_standing_faults(plain, standing_frame({power_loss_entry()})).has_value());
}

TEST_CASE("fault_status_subset keeps only fault objects", "[snapmaker][standing]") {
    nlohmann::json frame = standing_frame({power_loss_entry()});
    frame["toolhead"] = {{"homed_axes", "xyz"}};
    const nlohmann::json subset = faultcodes::fault_status_subset(frame);
    REQUIRE(subset.size() == 1);
    REQUIRE(subset == standing_frame({power_loss_entry()}));

    nlohmann::json unrelated;
    unrelated["toolhead"] = {{"homed_axes", "xyz"}};
    REQUIRE(faultcodes::fault_status_subset(unrelated).empty());
    REQUIRE(faultcodes::fault_status_subset(nlohmann::json::array()).empty());
}

TEST_CASE("read_standing_faults emits console-equivalent coded lines", "[snapmaker][standing]") {
    const auto lines =
        faultcodes::read_standing_faults(coded_firmware(), standing_frame({power_loss_entry()}));
    REQUIRE(lines.has_value());
    REQUIRE(lines->size() == 1);
    // The line must parse through the same classify path a console line takes:
    // the `!!` prefix and the four {:04d} groups are that path's contract.
    REQUIRE((*lines)[0] == "!! 0001-0522-0000-0017 exception id:522 index:0 code:17 power loss");
    auto e = faultcodes::classify(coded_firmware(), (*lines)[0]);
    REQUIRE(e.has_value());
    REQUIRE(e->code == "0001-0522-0000-0017");
}

TEST_CASE_METHOD(LVGLUITestFixture, "a standing power-loss fault surfaces our wording",
                 "[snapmaker][standing]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::on_notify_status_update(
        router, notify_frame(standing_frame({power_loss_entry()})));
    helix::ui::UpdateQueue::instance().drain();

    // Level 1 alone would classify INFO and surface nothing; a standing fault
    // is current by definition, so the path floors it to a toast.
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);
    const auto wording =
        snapmaker::exception_message(*snapmaker::decode_exception_code("0001-0522-0000-0017"));
    REQUIRE_FALSE(wording.empty());
    REQUIRE(fault_surface_correlation::was_recently_surfaced(std::string(wording)));

    // Fire the deferred-toast timer so its context is freed before teardown.
    process_lvgl(250);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "an unchanged standing list does not surface twice",
                 "[snapmaker][standing]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    const nlohmann::json msg = notify_frame(standing_frame({power_loss_entry()}));
    GcodeErrorRouterTestAccess::on_notify_status_update(router, msg);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);

    // The same list again: every line is already in the seen set.
    GcodeErrorRouterTestAccess::on_notify_status_update(router, msg);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);
    REQUIRE(GcodeErrorRouterTestAccess::standing_seen_count(router) == 1);

    process_lvgl(250);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a delta frame without the fault object changes nothing",
                 "[snapmaker][standing]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::on_notify_status_update(
        router, notify_frame(standing_frame({power_loss_entry()})));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_seen_count(router) == 1);

    nlohmann::json delta = nlohmann::json::object();
    delta["toolhead"] = {{"homed_axes", "xyz"}};
    GcodeErrorRouterTestAccess::on_notify_status_update(router, notify_frame(delta));
    helix::ui::UpdateQueue::instance().drain();

    // The frame said nothing about faults; the seen set must not read as
    // "all cleared".
    REQUIRE(GcodeErrorRouterTestAccess::standing_seen_count(router) == 1);
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);

    process_lvgl(250);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a cleared and re-raised standing fault surfaces again",
                 "[snapmaker][standing]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::on_notify_status_update(
        router, notify_frame(standing_frame({power_loss_entry()})));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);

    // The firmware clears the list: the line leaves the seen set.
    GcodeErrorRouterTestAccess::on_notify_status_update(router, notify_frame(standing_frame({})));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_seen_count(router) == 0);
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);

    // Re-raised: not in the (now empty) set, so it surfaces again.
    GcodeErrorRouterTestAccess::on_notify_status_update(
        router, notify_frame(standing_frame({power_loss_entry()})));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 2);
    REQUIRE(GcodeErrorRouterTestAccess::standing_seen_count(router) == 1);

    process_lvgl(250);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a reconnect re-surfaces standing faults",
                 "[snapmaker][standing]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    const nlohmann::json msg = notify_frame(standing_frame({power_loss_entry()}));
    GcodeErrorRouterTestAccess::on_notify_status_update(router, msg);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);

    // A fresh connection resets the seen set, so the subscription's first full
    // frame surfaces a fault that survived the disconnect (or the restart).
    GcodeErrorRouterTestAccess::on_connected(router);
    REQUIRE(GcodeErrorRouterTestAccess::standing_seen_count(router) == 0);
    GcodeErrorRouterTestAccess::on_notify_status_update(router, msg);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 2);

    process_lvgl(250);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a standing fault renders translated",
                 "[snapmaker][standing]") {
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    // The render path owns translation: expect exactly what classify produces
    // for the same code under this locale.
    auto expected = faultcodes::classify(get_printer_state().get_discovery(),
                                         "!! 0003-0530-0000-0011 platten nicht entfernt");
    REQUIRE(expected.has_value());
    REQUIRE(expected->detail.find("Entferne die PEI-Platte") != std::string::npos);

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::on_notify_status_update(
        router, notify_frame(standing_frame({{{"id", 530},
                                              {"index", 0},
                                              {"code", 11},
                                              {"level", 3},
                                              {"message", "The plate has not been removed"},
                                              {"is_persistent", 0}}})));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);
    REQUIRE(fault_surface_correlation::was_recently_surfaced(expected->detail));

    lv_translation_set_language(helix::ui::kIdentityLocale);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

// ============================================================================
// Raise notifications: the firmware pushes every raise on a websocket method
// the provider table names, and the coded copy must win over the console
// prose of the same fault.
// ============================================================================

namespace {

/// The oneshot the hardware raises for SET_PRINT_PREFERENCES during a print.
nlohmann::json prefs_entry() {
    return {{"id", 531},
            {"index", 0},
            {"code", 16},
            {"level", 1},
            {"message", "[print_task_config] not allow to set preferences during printing!"},
            {"is_persistent", 0}};
}

/// The frame shape the method callback delivers, method name as measured on a U1.
nlohmann::json raise_frame(const nlohmann::json& entry) {
    return {{"method", "notify_exception_notification"},
            {"params", nlohmann::json::array({entry})}};
}

constexpr const char* kPrefsCode = "0001-0531-0000-0016";
constexpr const char* kPrefsMethod = "notify_exception_notification";
constexpr const char* kPrefsMessage =
    "[print_task_config] not allow to set preferences during printing!";

} // namespace

TEST_CASE("the status list and the raise notification produce the same line",
          "[snapmaker][fault-notify]") {
    // One formatter serves both channels, so the same fault reads identically
    // whichever way it arrives -- and feeds one classify path.
    const auto standing =
        faultcodes::read_standing_faults(coded_firmware(), standing_frame({prefs_entry()}));
    const auto note = faultcodes::read_fault_notification(kPrefsMethod, prefs_entry());
    REQUIRE(standing.has_value());
    REQUIRE(note.has_value());
    REQUIRE(standing->size() == 1);
    REQUIRE((*standing)[0] == note->line);
    REQUIRE(note->line == std::string("!! ") + kPrefsCode + " " + kPrefsMessage);
    REQUIRE(note->message == kPrefsMessage);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a coded raise notification surfaces our wording",
                 "[snapmaker][fault-notify]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::on_notify_fault(router, kPrefsMethod, raise_frame(prefs_entry()));
    helix::ui::UpdateQueue::instance().drain();

    // The notification alone must surface our wording, not the console prose:
    // exactly one presentation, naming the coded spelling.
    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);
    REQUIRE(GcodeErrorRouterTestAccess::last_deferred_shown(router) == kPrefsCode);

    const auto wording =
        snapmaker::exception_message(*snapmaker::decode_exception_code(kPrefsCode));
    REQUIRE_FALSE(wording.empty());
    REQUIRE(fault_surface_correlation::was_recently_surfaced(std::string(wording)));
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "prose then its coded raise show only our wording",
                 "[snapmaker][fault-notify]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    // The console copy first: code-stripped, the generic classifier's to own.
    GcodeErrorRouterTestAccess::process_line(router, std::string("!! ") + kPrefsMessage);
    // The coded raise milliseconds later.
    GcodeErrorRouterTestAccess::on_notify_fault(router, kPrefsMethod, raise_frame(prefs_entry()));
    helix::ui::UpdateQueue::instance().drain();

    // Both spellings schedule a held presentation; only the coded one fires.
    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);
    REQUIRE(GcodeErrorRouterTestAccess::last_deferred_shown(router) == kPrefsCode);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "prose alone still surfaces", "[snapmaker][fault-notify]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    // Uncoded prose on a fault-code firmware: held in case a coded twin
    // follows, shown unchanged when none does.
    GcodeErrorRouterTestAccess::process_line(router, "!! Must home the printer first");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);
    REQUIRE(GcodeErrorRouterTestAccess::last_deferred_shown(router) ==
            "Must home the printer first");
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a coded raise after the prose fired shows a second toast",
                 "[snapmaker][fault-notify]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::process_line(router, std::string("!! ") + kPrefsMessage);
    helix::ui::UpdateQueue::instance().drain();
    // Past the 150ms hold the prose has shown; there is nothing pending to
    // stand down anymore.
    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);

    // The coded raise then surfaces ours. Accepted rather than suppressed: the
    // two spellings share no recorded identity (prose recorded its cleaned
    // text, the code carries ours), and wording ours over the firmware's
    // chinglish is worth one transient duplicate toast.
    GcodeErrorRouterTestAccess::on_notify_fault(router, kPrefsMethod, raise_frame(prefs_entry()));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 2);
    REQUIRE(GcodeErrorRouterTestAccess::last_deferred_shown(router) == kPrefsCode);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a standing fault and its notification surface once",
                 "[snapmaker][fault-notify][standing]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    // Notification first: it claims the fault...
    GcodeErrorRouterTestAccess::on_notify_fault(router, kPrefsMethod, raise_frame(prefs_entry()));
    helix::ui::UpdateQueue::instance().drain();
    // ...then the status list raises the same fault.
    GcodeErrorRouterTestAccess::on_notify_status_update(
        router, notify_frame(standing_frame({prefs_entry()})));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);

    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);
    REQUIRE(GcodeErrorRouterTestAccess::last_deferred_shown(router) == kPrefsCode);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a notification for an already-standing fault surfaces once",
                 "[snapmaker][fault-notify][standing]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    // Status list first: claims and schedules the presentation...
    GcodeErrorRouterTestAccess::on_notify_status_update(
        router, notify_frame(standing_frame({prefs_entry()})));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(GcodeErrorRouterTestAccess::standing_fed_count(router) == 1);
    // ...then the notification of the same raise arrives.
    GcodeErrorRouterTestAccess::on_notify_fault(router, kPrefsMethod, raise_frame(prefs_entry()));
    helix::ui::UpdateQueue::instance().drain();

    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);
    REQUIRE(GcodeErrorRouterTestAccess::last_deferred_shown(router) == kPrefsCode);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a caller-handled RPC error suppresses the notification",
                 "[snapmaker][fault-notify]") {
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    // The RPC response carries the firmware's bare message, which is the
    // notification's identity too, so the pre-check catches it.
    rpc_error_correlation::record_caller_handled(kPrefsMessage);

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::on_notify_fault(router, kPrefsMethod, raise_frame(prefs_entry()));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);

    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 0);
    get_printer_state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(LVGLUITestFixture, "a printer without the code channel ignores notifications",
                 "[snapmaker][fault-notify]") {
    PrinterDiscovery plain = hardware_with_objects({"bed_mesh", "quad_gantry_level"});
    get_printer_state().set_hardware(std::move(plain));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    GcodeErrorRouterTestAccess::on_notify_fault(router, kPrefsMethod, raise_frame(prefs_entry()));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);

    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 0);
    REQUIRE_FALSE(fault_surface_correlation::was_recently_surfaced(kPrefsMessage));
    get_printer_state().set_hardware(PrinterDiscovery{});
}
