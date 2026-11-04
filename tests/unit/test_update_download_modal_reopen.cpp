// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_update_download_modal_reopen.cpp
 * @brief A failed update is shown even after its download dialog was dismissed
 *
 * update_download_modal is the only thing bound to download_status, and a
 * backdrop tap, ESC or a modal-stack sweep closes it without stopping the
 * download worker. An error reported after that has nowhere to appear unless
 * the overlay brings the dialog back (prestonbrown/helixscreen#1727).
 */

#include "ui_modal.h"
#include "ui_settings_updates.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "system/update_checker.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::settings::get_updates_settings_overlay;
using DownloadStatus = UpdateChecker::DownloadStatus;

namespace {

constexpr const char* kDownloadModal = "update_download_modal";

void settle() {
    helix::ui::UpdateQueue::instance().drain();
    lv_timer_handler();
    helix::ui::UpdateQueue::instance().drain();
}

bool download_modal_on_top() {
    return ModalStack::instance().top_component_name() == kDownloadModal;
}

/// Closes the dialog the way a backdrop tap does: through the modal system,
/// never through UpdatesSettingsOverlay::hide_update_download_modal().
void dismiss_by_backdrop() {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    Modal::hide(dialog, ModalCloseReason::BackdropTap);
    settle();
}

std::string download_text() {
    return lv_subject_get_string(UpdateChecker::instance().download_text_subject());
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Update download dialog returns to show a later failure",
                 "[update_checker][download][modal][1727]") {
    auto& checker = UpdateChecker::instance();
    checker.init();
    auto& overlay = get_updates_settings_overlay();

    overlay.show_update_download_modal();
    settle();
    REQUIRE(download_modal_on_top());

    SECTION("a backdrop-dismissed dialog reopens on the error, showing its text") {
        checker.report_download_status(DownloadStatus::Downloading, 40, "Downloading...");
        settle();
        dismiss_by_backdrop();
        REQUIRE_FALSE(download_modal_on_top());

        checker.report_download_status(DownloadStatus::Error, 0, "Error: Download failed",
                                       "Failed to download update file");
        settle();

        CHECK(download_modal_on_top());
        CHECK(lv_subject_get_int(checker.download_status_subject()) ==
              static_cast<int>(DownloadStatus::Error));
        CHECK(download_text() == "Error: Download failed");
    }

    SECTION("a dialog the user closed stays closed") {
        overlay.hide_update_download_modal();
        settle();
        REQUIRE_FALSE(download_modal_on_top());

        checker.report_download_status(DownloadStatus::Error, 0, "Error: Download failed");
        settle();

        CHECK_FALSE(download_modal_on_top());
    }

    SECTION("an error while the dialog is up opens no second one") {
        checker.report_download_status(DownloadStatus::Error, 0, "Error: Download failed");
        settle();

        CHECK(download_modal_on_top());
        lv_obj_t* top = ModalStack::instance().top_dialog();
        Modal::hide(top, ModalCloseReason::ButtonPress);
        settle();
        CHECK_FALSE(download_modal_on_top());
    }

    SECTION("progress after a dismissal does not bring the dialog back") {
        checker.report_download_status(DownloadStatus::Downloading, 10, "Downloading...");
        settle();
        dismiss_by_backdrop();

        checker.report_download_status(DownloadStatus::Downloading, 60, "Downloading...");
        checker.report_download_status(DownloadStatus::Verifying, 100, "Verifying...");
        settle();

        CHECK_FALSE(download_modal_on_top());
    }

    overlay.hide_update_download_modal();
    settle();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Update notification shows again after a backdrop dismissal",
                 "[update_checker][modal][widget_ref]") {
    // show_update_notification() opens the dialog only while its handle is
    // empty. A backdrop tap closes the dialog without reaching
    // hide_update_notification(), so the handle has to notice the delete
    // itself or the next check's notification never appears.
    auto& checker = UpdateChecker::instance();
    checker.init();

    checker.show_update_notification();
    settle();
    REQUIRE(ModalStack::instance().top_component_name() == "update_notify_modal");

    dismiss_by_backdrop();
    process_lvgl(400); // the exit animation ends and the tree is deleted
    settle();
    REQUIRE(ModalStack::instance().top_component_name().empty());

    checker.show_update_notification();
    settle();
    CHECK(ModalStack::instance().top_component_name() == "update_notify_modal");

    checker.hide_update_notification();
    settle();
}
