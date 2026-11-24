// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_notice_router.cpp
 * @brief A published notice becomes the toast it describes, on the thread rules
 * ToastManager already has, through the subscriber the UI installs for itself.
 */

#include "ui_button.h"
#include "ui_emergency_stop.h"
#include "ui_notification.h"
#include "ui_notification_history.h"
#include "ui_toast_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/toast_log.h"
#include "../ui_test_utils.h"
#include "ams_error.h"
#include "core/error_reporting.h"
#include "core/update_queue.h"
#include "hardware_validator.h"
#include "notice.h"
#include "print_completion.h"
#include "print_start_navigation.h"
#include "u1_stock_screen.h"

#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Notice;
using helix::NoticeSeverity;
using helix::publish_notice;
using helix::publish_ui_request;
using helix::UiRequest;
using helix::UiRequestKind;
using helix::test::ToastLog;

namespace {

void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

std::vector<Notice> g_captured;

void capture_notice(const Notice& notice) {
    g_captured.push_back(notice);
}

/// Swaps the UI's subscriber out for one that keeps the notices as data.
struct CapturedNotices {
    helix::NoticeSubscriber previous;
    CapturedNotices() : previous(helix::set_notice_subscriber(capture_notice)) {
        g_captured.clear();
    }
    ~CapturedNotices() {
        helix::set_notice_subscriber(previous);
    }
};

std::vector<UiRequest> g_requests;
std::thread::id g_request_thread;

/// Swaps @p kind's handler out for one that keeps the requests as data.
struct CapturedRequests {
    UiRequestKind kind;
    helix::UiRequestHandler previous;
    explicit CapturedRequests(UiRequestKind k)
        : kind(k), previous(helix::set_ui_request_handler(k, [](const UiRequest& request) {
              g_requests.push_back(request);
              g_request_thread = std::this_thread::get_id();
          })) {
        g_requests.clear();
    }
    ~CapturedRequests() {
        helix::set_ui_request_handler(kind, previous);
    }
};

int g_action_runs = 0;
void* g_action_data = nullptr;

void counting_action(void* user_data) {
    ++g_action_runs;
    g_action_data = user_data;
}

} // namespace

TEST_CASE("A notice reaches the toast of its severity and duration", "[toast][notification]") {
    ToastLog log;
    publish_notice({NoticeSeverity::INFO, "info", 1000});
    publish_notice({NoticeSeverity::SUCCESS, "success", 2000});
    publish_notice({NoticeSeverity::WARNING, "warning", 3000});
    publish_notice({NoticeSeverity::ERROR, "error"});
    drain();

    REQUIRE(log.shown.size() == 4);
    CHECK(log.shown[0].severity == ToastSeverity::INFO);
    CHECK(log.shown[0].message == "info");
    CHECK(log.shown[0].duration_ms == 1000);
    CHECK(log.shown[1].severity == ToastSeverity::SUCCESS);
    CHECK(log.shown[2].severity == ToastSeverity::WARNING);
    CHECK(log.shown[3].severity == ToastSeverity::ERROR);
    // Matches ToastManager::show()'s own default.
    CHECK(log.shown[3].duration_ms == 4000);

    const helix::NoticeSubscriber previous = helix::set_notice_subscriber(nullptr);
    publish_notice({NoticeSeverity::ERROR, "nobody is listening"});
    helix::set_notice_subscriber(previous);
    drain();
    CHECK(log.shown.size() == 4);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A notice published off the main thread builds its toast only on the main thread",
                 "[toast][notification]") {
    auto& toasts = ToastManager::instance();
    toasts.init();
    ToastLog log;

    std::thread publisher(
        [] { publish_notice({NoticeSeverity::WARNING, "from the socket thread", 5000}); });
    const std::thread::id publisher_id = publisher.get_id();
    publisher.join();

    log.expect_one(ToastSeverity::WARNING, "from the socket thread", 5000);
    CHECK(log.shown[0].thread == publisher_id);
    CHECK_FALSE(toasts.is_visible());

    drain();
    CHECK(toasts.is_visible());

    toasts.deinit_subjects();
    drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "A notice's action becomes the toast's button",
                 "[toast][notification]") {
    auto& toasts = ToastManager::instance();
    toasts.init();
    g_action_runs = 0;
    int target = 0;

    publish_notice(
        {NoticeSeverity::INFO, "has an action", 8000, "Do it", counting_action, &target});
    drain();

    lv_obj_t* button = lv_obj_find_by_name(lv_layer_top(), "toast_action_btn");
    REQUIRE(button != nullptr);
    CHECK(std::string(lv_label_get_text(ui_button_get_label(button))) == "Do it");

    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK(g_action_runs == 1);
    CHECK(g_action_data == &target);

    toasts.deinit_subjects();
    drain();
}

TEST_CASE_METHOD(LVGLTestFixture, "Hardware validation publishes its notice with a View action",
                 "[toast][hardware][validator]") {
    CapturedNotices notices;
    HardwareValidator validator;
    HardwareValidationResult result;

    SECTION("missing critical hardware is an error") {
        result.critical_missing.push_back(
            HardwareIssue::critical("heater_bed", HardwareType::HEATER, "Bed heater not found"));
        validator.notify_user(result);
        REQUIRE(g_captured.size() == 1);
        CHECK(g_captured[0].severity == NoticeSeverity::ERROR);
        CHECK(g_captured[0].message == "Critical hardware missing: heater_bed");
    }
    SECTION("new hardware is information") {
        result.newly_discovered.push_back(
            HardwareIssue::info("neopixel chamber", HardwareType::LED, "New LED found"));
        validator.notify_user(result);
        REQUIRE(g_captured.size() == 1);
        CHECK(g_captured[0].severity == NoticeSeverity::INFO);
        CHECK(g_captured[0].message == "LED strip available for lighting control");
    }

    CHECK(g_captured[0].duration_ms == 8000);
    CHECK(g_captured[0].action_text == "View");
    REQUIRE(g_captured[0].action != nullptr);
    CHECK(g_captured[0].action_user_data == nullptr);

    CapturedRequests requests(UiRequestKind::OpenHardwareHealth);
    g_captured[0].action(g_captured[0].action_user_data);
    CHECK(g_requests.size() == 1);
}

TEST_CASE("A UI request reaches its kind's handler with its fields, on the calling thread",
          "[notification][notice]") {
    CapturedRequests requests(UiRequestKind::OfferClearAfterUnverifiedInsert);
    auto* const api = reinterpret_cast<helix::IMoonrakerAPI*>(&g_requests);

    std::thread([api] {
        publish_ui_request({UiRequestKind::OfferClearAfterUnverifiedInsert, 3, api});
    }).join();

    REQUIRE(g_requests.size() == 1);
    CHECK(g_requests[0].kind == UiRequestKind::OfferClearAfterUnverifiedInsert);
    CHECK(g_requests[0].slot == 3);
    CHECK(g_requests[0].api == api);
    CHECK(g_request_thread != std::this_thread::get_id());

    // A kind with no handler is dropped, and reaches no other kind's.
    const auto previous = helix::set_ui_request_handler(UiRequestKind::OpenHardwareHealth, nullptr);
    publish_ui_request({UiRequestKind::OpenHardwareHealth});
    helix::set_ui_request_handler(UiRequestKind::OpenHardwareHealth, previous);
    CHECK(g_requests.size() == 1);
}

TEST_CASE("The UI installs a handler for every request kind", "[notification][notice]") {
    for (int k = 0; k < static_cast<int>(UiRequestKind::kCount); ++k) {
        const auto kind = static_cast<UiRequestKind>(k);
        const auto installed = helix::set_ui_request_handler(kind, nullptr);
        helix::set_ui_request_handler(kind, installed);
        CAPTURE(k);
        CHECK(installed != nullptr);
    }
    const auto query = helix::set_print_status_shown_query(nullptr);
    helix::set_print_status_shown_query(query);
    CHECK(query != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "A print start asks the UI for the print status screen",
                 "[notification][notice][print][navigation]") {
    using helix::PrintJobState;
    CapturedRequests requests(UiRequestKind::OpenPrintStatus);
    ObserverGuard nav = helix::init_print_start_navigation_observer();
    drain();
    REQUIRE(g_requests.empty());
    helix::test::set_wire_state(state(), PrintJobState::PRINTING);
    helix::test::set_wire_state(state(), PrintJobState::PAUSED);
    CHECK(g_requests.empty()); // queued for the UI thread, not run inline
    drain();
    CHECK(g_requests.size() == 1);

    // A job already running when the observer starts asks too.
    nav = helix::init_print_start_navigation_observer();
    drain();
    CHECK(g_requests.size() == 2);
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    drain();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A power device going off or a Moonraker restart holds the recovery dialog back",
                 "[notification][notice][recovery]") {
    auto& estop = EmergencyStopOverlay::instance();
    for (UiRequestKind kind :
         {UiRequestKind::ExpectKlippyDisconnect, UiRequestKind::ExpectMoonrakerRestart}) {
        CAPTURE(static_cast<int>(kind));
        estop.suppress_recovery_dialog(0);
        REQUIRE_FALSE(estop.is_recovery_suppressed());
        publish_ui_request({kind});
        CHECK(estop.is_recovery_suppressed());
    }
    estop.suppress_recovery_dialog(0);
}

namespace {

/// Every ui_notification_* event reported while this is in scope.
struct NotificationEvents {
    std::vector<helix::ui::NotificationEvent> events;
    NotificationEvents() {
        helix::ui::set_test_notification_event_hook(
            [this](const helix::ui::NotificationEvent& e) { events.push_back(e); });
    }
    ~NotificationEvents() {
        helix::ui::set_test_notification_event_hook(nullptr);
    }
};

const NotificationHistoryEntry* history_row(const std::vector<NotificationHistoryEntry>& rows,
                                            const std::string& message) {
    for (const auto& row : rows) {
        if (message == row.message) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Each notice delivery reaches the notification path it names",
                 "[toast][notification][notice]") {
    auto& toasts = ToastManager::instance();
    toasts.init();
    NotificationHistory::instance().clear();
    NotificationEvents seen;
    ToastLog log;

    SECTION("a titled notification toasts title and message and keeps the title in history") {
        helix::publish_notification(NoticeSeverity::INFO, "loaded", "Startup");
        drain();
        log.expect_one(ToastSeverity::INFO, "Startup: loaded", 4000);
        REQUIRE(seen.events.size() == 1);
        CHECK(seen.events[0].title == "Startup");
        const auto rows = NotificationHistory::instance().get_all();
        const auto* row = history_row(rows, "loaded");
        REQUIRE(row != nullptr);
        CHECK(std::string(row->title) == "Startup");
    }
    SECTION("a warning notification stays up longer than an info one") {
        helix::publish_notification(NoticeSeverity::WARNING, "careful");
        drain();
        log.expect_one(ToastSeverity::WARNING, "careful", 5000);
    }
    SECTION("an error notification is a toast, not a dialog") {
        helix::publish_notification(NoticeSeverity::ERROR, "it broke", "Save");
        drain();
        log.expect_one(ToastSeverity::ERROR, "it broke", 6000);
        const auto rows = NotificationHistory::instance().get_all();
        const auto* row = history_row(rows, "it broke");
        REQUIRE(row != nullptr);
        CHECK_FALSE(row->was_modal);
    }
    SECTION("a detail rides the toast's second line; sticky never dismisses") {
        helix::publish_detail_notification(NoticeSeverity::WARNING, "refused", "fix it");
        helix::publish_detail_notification(NoticeSeverity::WARNING, "stuck", "read me", true);
        drain();
        REQUIRE(log.shown.size() == 2);
        CHECK(log.shown[0].message == "refused - fix it");
        CHECK(log.shown[0].duration_ms == 8000);
        CHECK(log.shown[1].message == "stuck - read me");
        CHECK(log.shown[1].duration_ms == 0);
        REQUIRE(seen.events.size() == 2);
        CHECK_FALSE(seen.events[0].sticky);
        CHECK(seen.events[1].sticky);
    }
    SECTION("a history entry adds a tappable row and no toast") {
        helix::publish_history_entry(NoticeSeverity::INFO, "Update Available", "v9 is out",
                                     "show_update_modal");
        drain();
        CHECK(log.shown.empty());
        REQUIRE(seen.events.size() == 1);
        CHECK(seen.events[0].title == "Update Available");
        CHECK(seen.events[0].action == "show_update_modal");
        const auto rows = NotificationHistory::instance().get_all();
        const auto* row = history_row(rows, "v9 is out");
        REQUIRE(row != nullptr);
        CHECK(std::string(row->action) == "show_update_modal");
    }
    SECTION("an alert is a dialog") {
        helix::publish_alert("Connection Failed", "unreachable");
        drain();
        CHECK(log.shown.empty());
        const auto rows = NotificationHistory::instance().get_all();
        const auto* row = history_row(rows, "unreachable");
        REQUIRE(row != nullptr);
        CHECK(row->was_modal);
    }

    NotificationHistory::instance().clear();
    toasts.deinit_subjects();
    drain();
}

TEST_CASE("The report macros publish notices rather than calling the UI",
          "[notification][notice]") {
    CapturedNotices notices;
    NOTIFY_WARNING("disk at {}%", 91);
    NOTIFY_ERROR_T("Save Failed", "could not write {}", "a.cfg");
    NOTIFY_ERROR_MODAL("Connection Failed", "no route");
    ErrorContext("Probe").warning("drifted");

    REQUIRE(g_captured.size() == 4);
    CHECK(g_captured[0].delivery == helix::NoticeDelivery::Notification);
    CHECK(g_captured[0].severity == NoticeSeverity::WARNING);
    CHECK(g_captured[0].message == "disk at 91%");
    CHECK(g_captured[0].title.empty());
    CHECK(g_captured[1].delivery == helix::NoticeDelivery::Notification);
    CHECK(g_captured[1].severity == NoticeSeverity::ERROR);
    CHECK(g_captured[1].title == "Save Failed");
    CHECK(g_captured[1].message == "could not write a.cfg");
    CHECK(g_captured[2].delivery == helix::NoticeDelivery::Alert);
    CHECK(g_captured[2].title == "Connection Failed");
    CHECK(g_captured[3].message == "Probe: drifted");
}

TEST_CASE("A filament backend's error publishes its suggestion as the notice's detail",
          "[notification][notice][ams]") {
    CapturedNotices notices;
    const helix::AmsError err(helix::AmsResult::BUSY, "op in flight", "Unit is busy",
                              "Wait for the current operation to finish");
    helix::ui::notify_ams_error(err);
    helix::ui::notify_ams_warning(err);
    helix::ui::notify_ams_error(helix::AmsError{});

    REQUIRE(g_captured.size() == 2);
    for (const Notice& n : g_captured) {
        CHECK(n.delivery == helix::NoticeDelivery::Notification);
        CHECK(n.message == helix::ui::ams_error_body(err, nullptr));
        CHECK(n.detail == "Wait for the current operation to finish");
        CHECK_FALSE(n.sticky);
    }
    CHECK(g_captured[0].severity == NoticeSeverity::ERROR);
    CHECK(g_captured[1].severity == NoticeSeverity::WARNING);
}

TEST_CASE("U1 SSH down publishes a sticky warning that says what to do",
          "[notification][notice][u1]") {
    CapturedNotices notices;
    ::setenv(helix::u1::kRootSshDownEnv, "1", 1);
    helix::u1::surface_root_ssh_down_from_hook();
    ::unsetenv(helix::u1::kRootSshDownEnv);
    helix::u1::surface_root_ssh_down_from_hook();

    REQUIRE(g_captured.size() == 1);
    CHECK(g_captured[0].delivery == helix::NoticeDelivery::Notification);
    CHECK(g_captured[0].severity == NoticeSeverity::WARNING);
    CHECK(g_captured[0].sticky);
    CHECK_FALSE(g_captured[0].detail.empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Notification notices published off the main thread present only on it",
                 "[toast][notification][notice]") {
    auto& toasts = ToastManager::instance();
    toasts.init();
    ui_notification_init(); // records this thread as the main thread
    NotificationHistory::instance().clear();
    ToastLog log;

    std::thread([] {
        helix::publish_notification(NoticeSeverity::WARNING, "from wifi");
        helix::publish_detail_notification(NoticeSeverity::WARNING, "refused", "fix it");
        helix::publish_history_entry(NoticeSeverity::INFO, "AFC", "lane 2 ready", "afc_message");
    }).join();

    CHECK(log.shown.empty());
    CHECK(NotificationHistory::instance().count() == 0);

    drain();
    REQUIRE(log.shown.size() == 2);
    for (const auto& shown : log.shown) {
        CHECK(shown.thread == std::this_thread::get_id());
    }
    CHECK(NotificationHistory::instance().count() == 3);

    ui_notification_deinit();
    NotificationHistory::instance().clear();
    toasts.deinit_subjects();
    drain();
}
