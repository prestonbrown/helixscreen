// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_usb_print.cpp
 * @brief Printing a file from the USB tab copies it to Moonraker first.
 *
 * Run with: ./build/bin/helix-tests "[usb][usb_print]"
 *
 * Moonraker cannot read a stick HelixScreen mounted itself, so the USB tab's
 * file has to reach Moonraker's gcodes root before Moonraker is told to print
 * it, and the path Moonraker is given has to be the copy's.
 */

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/usb_scan_wait.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "job_queue_state.h"
#include "moonraker_api_mock.h"
#include "usb_backend_mock.h"
#include "usb_manager.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

namespace fs = std::filesystem;

/// Collects the user-facing error toasts: toasts are compiled out of the
/// test build, so the hook is the only observable.
struct ErrorLog {
    std::vector<std::string> messages;
    ErrorLog() {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& message) { messages.push_back(message); });
    }
    ~ErrorLog() {
        helix::ui::set_test_notification_error_hook(nullptr);
    }
    ErrorLog(const ErrorLog&) = delete;
    ErrorLog& operator=(const ErrorLog&) = delete;
};

/// The real panel over MoonrakerAPIMock, with a mock USB drive holding one
/// real file in a subfolder, listed on the USB tab.
class UsbPrintFixture : private helix::PrintSelectGlobalStateReset,
                        public helix::PrintSelectPanelFixture {
  public:
    UsbPrintFixture()
        : helix::PrintSelectPanelFixture(helix::PrintSelectFilelistHandler::Unregistered,
                                         helix::PrintSelectVisit::Immediate,
                                         helix::PrintSelectApi::Mock),
          usb_(true) {
        fs::remove_all(root_);
        fs::create_directories(root_ / "projects");
        std::ofstream(local_path()) << "G28\nG1 X10 Y10\n";

        REQUIRE(usb_.start());
        auto* backend = static_cast<UsbBackendMock*>(usb_.get_backend());
        REQUIRE(backend != nullptr);
        backend->simulate_drive_insert(UsbDrive(root_.string(), "/dev/sda1", "STICK"));
        backend->set_mock_files(root_.string(), {{local_path(), "part.gcode", 15, 1000}});

        panel_->set_usb_manager(&usb_);
        panel_->on_source_usb_clicked();
        helix::test::wait_for_usb_scan(
            [&] { return PrintSelectPanelTestAccess::usb_scanning(*panel_); });
        drain();
        REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, "part.gcode"));
        REQUIRE(panel_->select_file_by_name("part.gcode"));
        drain();

        // A running print makes PrintStartController::initiate() refuse
        // after set_file(), so the test reads the hand-off without driving a
        // real print start through the mock.
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        drain();
    }

    ~UsbPrintFixture() override {
        helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        drain();
        panel_->set_usb_manager(nullptr);
        usb_.stop();
        fs::remove_all(root_);
    }

    std::string local_path() const {
        return (root_ / "projects" / "part.gcode").string();
    }

    /// Make server.files.list for the copy folder answer @p entries
    /// (path relative to gcodes, size), or fail with @p error_code.
    void copy_folder_holds(std::vector<std::pair<std::string, uint64_t>> entries,
                           int error_code = 0) {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "server.files.list",
            [entries, error_code](MoonrakerClientMock*, const json&,
                                  std::function<void(const json&)> success_cb,
                                  std::function<void(const MoonrakerError&)> error_cb) -> bool {
                if (error_code != 0) {
                    MoonrakerError err =
                        MoonrakerError::unknown("listing refused", "server.files.list");
                    err.code = error_code;
                    if (error_cb) {
                        error_cb(err);
                    }
                    return true;
                }
                json result = json::array();
                for (const auto& [path, size] : entries) {
                    result.push_back({{"path", path}, {"size", size}, {"modified", 1.0}});
                }
                success_cb(json{{"result", result}});
                return true;
            });
    }

    MoonrakerFileTransferAPIMock& transfers() {
        return static_cast<MoonrakerAPIMock&>(*api_).transfers_mock();
    }

    fs::path root_ =
        fs::temp_directory_path() / ("helix_usb_print_stick_" + std::to_string(::getpid()));
    UsbManager usb_;
};

} // namespace

TEST_CASE_METHOD(UsbPrintFixture, "a USB file is copied to Moonraker before it prints",
                 "[usb][usb_print]") {
    panel_->start_print(/*force=*/true);
    drain();

    const auto& uploads = transfers().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].root == "gcodes");
    CHECK(uploads[0].dest_path == std::string(PrintSelectPanel::kUsbCopyDir) + "/part.gcode");
    CHECK(uploads[0].local_path == local_path());
    CHECK(uploads[0].content == "G28\nG1 X10 Y10\n");

    // Moonraker is asked to print the copy, not a same-named file elsewhere.
    const auto [filename, dir] = PrintSelectPanelTestAccess::controller_file(*panel_);
    CHECK(filename == "part.gcode");
    CHECK(dir == PrintSelectPanel::kUsbCopyDir);
}

TEST_CASE_METHOD(UsbPrintFixture, "a failed USB copy says so and starts nothing",
                 "[usb][usb_print]") {
    ErrorLog errors;
    transfers().mock_fail_path_uploads();

    panel_->start_print(/*force=*/true);
    drain();

    REQUIRE(errors.messages.size() == 1);
    CHECK(errors.messages[0].find("part.gcode") != std::string::npos);
    CHECK(transfers().path_uploads().size() == 1);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());

    // The failure releases the in-flight guard: a retry copies again.
    transfers().mock_fail_path_uploads(false);
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(transfers().path_uploads().size() == 2);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first == "part.gcode");
}

TEST_CASE_METHOD(UsbPrintFixture, "a USB print keeps what was selected when Print was tapped",
                 "[usb][usb_print]") {
    PrintSelectPanelTestAccess::set_selected_colors(*panel_, {"#FF0000"});
    transfers().mock_hold_path_uploads();
    panel_->start_print(/*force=*/true);
    drain();

    // Another file opened while the copy runs.
    PrintSelectPanelTestAccess::set_selected_colors(*panel_, {"#00FF00"});
    transfers().release_held_path_uploads();
    drain();

    CHECK(PrintSelectPanelTestAccess::controller_colors(*panel_) ==
          std::vector<std::string>{"#FF0000"});
}

TEST_CASE_METHOD(UsbPrintFixture, "a USB file added to the queue is queued as its copy",
                 "[usb][usb_print]") {
    JobQueueState* previous = get_job_queue_state();
    auto jqs = std::make_unique<JobQueueState>(api_.get(), &mock_client_);
    set_job_queue_state(jqs.get());
    drain();

    PrintSelectPanelTestAccess::add_to_queue(*panel_);
    drain();

    REQUIRE(transfers().path_uploads().size() == 1);
    const std::string copy = std::string(PrintSelectPanel::kUsbCopyDir) + "/part.gcode";
    int queued = 0;
    api_->queue().get_queue_status(
        [&](const JobQueueStatus& status) {
            for (const auto& job : status.queued_jobs) {
                queued += job.filename == copy ? 1 : 0;
            }
        },
        [](const MoonrakerError& err) { FAIL(err.message); });
    CHECK(queued == 1);

    set_job_queue_state(previous);
    jqs.reset();
    drain();
}

TEST_CASE_METHOD(UsbPrintFixture, "a same-size copy already on the printer is reused, not uploaded",
                 "[usb][usb_print]") {
    copy_folder_holds({{"usb_prints/part.gcode", 15}});
    panel_->start_print(/*force=*/true);
    drain();

    CHECK(transfers().path_uploads().empty());
    const auto [filename, dir] = PrintSelectPanelTestAccess::controller_file(*panel_);
    CHECK(filename == "part.gcode");
    CHECK(dir == PrintSelectPanel::kUsbCopyDir);
}

TEST_CASE_METHOD(UsbPrintFixture,
                 "a different file under the same name is kept, the copy is suffixed",
                 "[usb][usb_print]") {
    copy_folder_holds({{"usb_prints/part.gcode", 999}});
    panel_->start_print(/*force=*/true);
    drain();

    REQUIRE(transfers().path_uploads().size() == 1);
    CHECK(transfers().path_uploads()[0].dest_path == "usb_prints/part (2).gcode");
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first == "part (2).gcode");
}

TEST_CASE_METHOD(UsbPrintFixture, "a copy folder that does not exist yet is created by the upload",
                 "[usb][usb_print]") {
    copy_folder_holds({}, 404);
    panel_->start_print(/*force=*/true);
    drain();

    REQUIRE(transfers().path_uploads().size() == 1);
    CHECK(transfers().path_uploads()[0].dest_path == "usb_prints/part.gcode");
}

TEST_CASE_METHOD(UsbPrintFixture, "an unreadable copy folder stops the copy rather than guess",
                 "[usb][usb_print]") {
    ErrorLog errors;
    copy_folder_holds({}, 500);
    panel_->start_print(/*force=*/true);
    drain();

    CHECK(transfers().path_uploads().empty());
    CHECK(errors.messages.size() == 1);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());
}

TEST_CASE_METHOD(UsbPrintFixture, "a USB copy that stops making progress is abandoned",
                 "[usb][usb_print]") {
    ErrorLog errors;
    transfers().mock_hold_path_uploads();
    panel_->start_print(/*force=*/true);
    drain();
    REQUIRE(transfers().path_uploads().size() == 1);

    process_lvgl(PrintSelectPanel::kUsbCopyStallMs + 2000);
    drain();
    CHECK(errors.messages.size() == 1);

    // The answer that finally arrives belongs to the abandoned copy.
    transfers().release_held_path_uploads();
    drain();
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());

    // Print works again.
    transfers().mock_hold_path_uploads(false);
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(transfers().path_uploads().size() == 2);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first == "part.gcode");
}

TEST_CASE_METHOD(UsbPrintFixture, "a double tap on Print copies the USB file once",
                 "[usb][usb_print]") {
    panel_->start_print(/*force=*/true);
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(transfers().path_uploads().size() == 1);

    // A tap while the upload itself is running is ignored too.
    transfers().mock_hold_path_uploads();
    panel_->start_print(/*force=*/true);
    drain();
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(transfers().path_uploads().size() == 2);
    transfers().release_held_path_uploads();
    drain();
}

TEST_CASE_METHOD(UsbPrintFixture, "a double tap on Add to Queue copies the USB file once",
                 "[usb][usb_print]") {
    JobQueueState* previous = get_job_queue_state();
    auto jqs = std::make_unique<JobQueueState>(api_.get(), &mock_client_);
    set_job_queue_state(jqs.get());
    drain();

    transfers().mock_hold_path_uploads();
    PrintSelectPanelTestAccess::add_to_queue(*panel_);
    PrintSelectPanelTestAccess::add_to_queue(*panel_);
    drain();
    PrintSelectPanelTestAccess::add_to_queue(*panel_);
    drain();
    CHECK(transfers().path_uploads().size() == 1);
    transfers().release_held_path_uploads();
    drain();

    set_job_queue_state(previous);
    jqs.reset();
    drain();
}

TEST_CASE_METHOD(UsbPrintFixture,
                 "a USB file offers no delete and asks Moonraker to delete nothing",
                 "[usb][usb_delete]") {
    lv_obj_t* del = lv_obj_find_by_name(lv_screen_active(), "delete_button");
    REQUIRE(del != nullptr);
    CHECK(lv_obj_has_flag(del, LV_OBJ_FLAG_HIDDEN));

    // The long-press path reaches these without the button. Delete addresses
    // Moonraker storage by name, where a same-named printer file may live.
    panel_->show_delete_confirmation();
    panel_->delete_file();
    drain();

    CHECK(static_cast<MoonrakerAPIMock&>(*api_).files_mock().deleted_files().empty());
    CHECK(fs::exists(local_path()));
}
