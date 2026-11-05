// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_notice_router.cpp
 * @brief A published notice becomes the toast it describes, on the thread rules
 * ToastManager already has, through the subscriber the UI installs for itself.
 */

#include "ui_button.h"
#include "ui_toast_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/toast_log.h"
#include "core/update_queue.h"
#include "notice.h"

#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Notice;
using helix::NoticeSeverity;
using helix::publish_notice;
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
}

TEST_CASE("A notice with no subscriber is dropped", "[toast][notification]") {
    ToastLog log;
    const helix::NoticeSubscriber previous = helix::set_notice_subscriber(nullptr);
    publish_notice({NoticeSeverity::ERROR, "nobody is listening"});
    helix::set_notice_subscriber(previous);
    drain();
    CHECK(log.shown.empty());
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
