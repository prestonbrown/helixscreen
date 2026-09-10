// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_notification_history_panel_refresh.cpp
 * @brief The open notification panel must show notifications that arrive while
 *        it is visible (prestonbrown/helixscreen#1525).
 *
 * The seam this pins: NotificationHistory keeps a revision counter bumped on
 * add()/clear(), NotificationManager publishes it into the
 * notification_history_version subject whenever the badge refreshes, and the
 * panel observes that subject and rebuilds its list. Killing either half of
 * that chain fails the arrival test below.
 */

#include "ui_notification_history.h"
#include "ui_notification_manager.h"
#include "ui_panel_notification_history.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/notification_history_panel_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <cstring>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

NotificationHistoryEntry make_entry(const char* title, const char* message) {
    NotificationHistoryEntry entry = {};
    entry.timestamp_ms = lv_tick_get();
    entry.severity = ToastSeverity::INFO;
    entry.was_modal = false;
    entry.was_read = false;
    strncpy(entry.title, title, sizeof(entry.title) - 1);
    strncpy(entry.message, message, sizeof(entry.message) - 1);
    return entry;
}

/// Owns the NotificationHistoryPanel built from the production XML component.
struct NotificationHistoryPanelFixture : public LVGLUITestFixture {
    NotificationHistoryPanelFixture() {
        helix::ui::notification_init_subjects();

        auto& history = NotificationHistory::instance();
        history.clear();

        panel_ = std::make_unique<NotificationHistoryPanel>(state(), nullptr);
        panel_->init_subjects();

        // The panel resolves its severity_card / header_bar dependency chain
        // through the production tree this fixture registers.
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(lv_screen_active(), "notification_history_panel", nullptr));
        if (root_) {
            panel_->setup(root_, lv_screen_active());
        }
    }

    ~NotificationHistoryPanelFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
    }

    /// The live item container, or nullptr if the tree did not build.
    lv_obj_t* content() const {
        return root_ ? lv_obj_find_by_name(root_, "overlay_content") : nullptr;
    }

    std::unique_ptr<NotificationHistoryPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(NotificationHistoryPanelFixture,
                 "NotificationHistoryPanel: arriving notification appears while the panel is open",
                 "[ui][notifications][1525]") {
    REQUIRE(root_ != nullptr);
    lv_obj_t* overlay = content();
    REQUIRE(overlay != nullptr);

    NotificationHistory::instance().add(make_entry("Entry One", "before the panel opened"));
    panel_->refresh();
    REQUIRE(lv_obj_get_child_count(overlay) == 1);

    // What production entry points do after writing the store.
    NotificationHistory::instance().add(make_entry("Entry Two", "arrived while open"));
    helix::ui::notification_refresh_from_history();
    UpdateQueue::instance().drain();

    REQUIRE(lv_obj_get_child_count(overlay) == 2);

    // The list is newest-first, so the freshly arrived entry is the first item.
    lv_obj_t* title = lv_obj_find_by_name(overlay, "item_title");
    REQUIRE(title != nullptr);
    REQUIRE(std::string(lv_label_get_text(title)) == "Entry Two");
}

TEST_CASE_METHOD(NotificationHistoryPanelFixture,
                 "NotificationHistoryPanel: repeating the same history version does not rebuild",
                 "[ui][notifications][1525]") {
    REQUIRE(root_ != nullptr);
    lv_obj_t* overlay = content();
    REQUIRE(overlay != nullptr);

    NotificationHistory::instance().add(make_entry("Entry One", "only entry"));
    panel_->refresh();
    REQUIRE(lv_obj_get_child_count(overlay) == 1);

    // A first application of a new version is a real change: it rebuilds the
    // list, so the item widget it created is a fresh object.
    NotificationHistoryPanelTestAccess::handle_history_version_change(*panel_, 7);
    lv_obj_t* item_after_first = lv_obj_find_by_name(overlay, "item_title");
    REQUIRE(item_after_first != nullptr);

    // Re-delivering the same version (the "double-publish" case: refresh()
    // already applied it directly, e.g. via Clear All, before the manager's
    // publish caught up) must not rebuild — the item identity must survive
    // unchanged.
    NotificationHistoryPanelTestAccess::handle_history_version_change(*panel_, 7);
    lv_obj_t* item_after_second = lv_obj_find_by_name(overlay, "item_title");
    CHECK(item_after_second == item_after_first);
}

TEST_CASE_METHOD(NotificationHistoryPanelFixture,
                 "NotificationHistoryPanel: a hidden panel does not eat the unread badge",
                 "[ui][notifications][1525]") {
    REQUIRE(root_ != nullptr);

    // NavigationManager::go_back() only hides an overlay's widget tree; it is
    // never destroyed and the version observer stays attached.
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    NotificationHistory::instance().add(make_entry("Entry One", "arrived while hidden"));
    helix::ui::notification_refresh_from_history();
    UpdateQueue::instance().drain();

    CHECK(NotificationHistory::instance().get_unread_count() == 1);
    CHECK(NotificationHistory::instance().get_highest_unread_severity() == ToastSeverity::INFO);

    auto entries = NotificationHistory::instance().get_all();
    REQUIRE_FALSE(entries.empty());
    CHECK_FALSE(entries.front().was_read);
}

TEST_CASE_METHOD(NotificationHistoryPanelFixture,
                 "NotificationHistoryPanel: destroying the panel withdraws its version observer",
                 "[ui][notifications][1525]") {
    REQUIRE(root_ != nullptr);

    lv_subject_t* subject = helix::ui::notification_history_version_subject();
    REQUIRE(subject != nullptr);

    // setup() attached exactly one observer on the manager's version subject.
    REQUIRE(lv_ll_get_len(&subject->subs_ll) == 1);

    // The observer defers through UpdateQueue; its removal must be immediate
    // so a notification arriving after teardown queues nothing into freed
    // state. Destroy in the same order as production: widgets first, then the
    // panel (whose dtor runs deinit_subjects()).
    lv_obj_delete(root_);
    root_ = nullptr;
    UpdateQueue::instance().drain();
    panel_.reset();
    UpdateQueue::instance().drain();

    REQUIRE(lv_ll_get_len(&subject->subs_ll) == 0);
}