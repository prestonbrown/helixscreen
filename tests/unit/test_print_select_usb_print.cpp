// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_usb_print.cpp
 * @brief Acting on a file from the USB tab addresses the stick file, never a
 *        same-named printer file.
 *
 * Run with: ./build/bin/helix-tests "[usb][print_select]"
 *
 * Moonraker cannot see a stick HelixScreen mounted itself, and Moonraker's own
 * storage can hold a file of the same name. A print copies the stick file to
 * Moonraker first and hands Moonraker the copy's path; delete is not offered.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "thumbnail_processor.h"
#include "usb_backend_mock.h"
#include "usb_manager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

namespace fs = std::filesystem;

/// The real panel over MoonrakerAPIMock, with a mock USB drive holding one real
/// file in a subfolder, listed on the USB tab and selected.
class UsbPrintFixture : public LVGLUITestFixture {
  public:
    UsbPrintFixture()
        : mock_client_(MoonrakerClientMock::PrinterType::VORON_24, /*speedup_factor=*/100.0),
          usb_(true) {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        mock_client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPIMock>(mock_client_, get_printer_state());

        panel_ = std::make_unique<PrintSelectPanel>(get_printer_state(), api_.get());
        panel_->init_subjects();
        panel_obj_ =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_select_panel", nullptr));
        REQUIRE(panel_obj_ != nullptr);
        panel_->setup(panel_obj_, test_screen());

        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        panels[static_cast<int>(PanelId::PrintSelect)] = panel_obj_;
        auto& nav = NavigationManager::instance();
        nav.set_panels(panels);
        nav.register_panel_instance(PanelId::PrintSelect, panel_.get());
        nav.set_active(PanelId::PrintSelect);
        drain();

        fs::remove_all(root_);
        fs::create_directories(root_ / "projects");
        std::ofstream(local_path()) << "G28\nG1 X10 Y10\n";

        REQUIRE(usb_.start());
        auto* backend = static_cast<UsbBackendMock*>(usb_.get_backend());
        REQUIRE(backend != nullptr);
        backend->simulate_drive_insert(UsbDrive(root_.string(), "/dev/sda1", "STICK"));
        backend->set_mock_files(root_.string(), {{local_path(), kName, 15, 1000}});

        panel_->set_usb_manager(&usb_);
        panel_->on_source_usb_clicked();
        drain();
        REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, kName));
        REQUIRE(panel_->select_file_by_name(kName));
        drain();

        // A running print makes PrintStartController::initiate() refuse
        // after set_file(), so a test reads the hand-off without driving a
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

        auto& nav = NavigationManager::instance();
        nav.register_panel_instance(PanelId::PrintSelect, nullptr);
        drain();
        panel_.reset();
        api_.reset();
        mock_client_.stop_temperature_simulation();
        mock_client_.disconnect();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        auto& processor = helix::ThumbnailProcessor::instance();
        auto& queue = helix::ui::UpdateQueue::instance();
        for (int pass = 0; pass < 4; ++pass) {
            processor.wait_for_completion();
            if (processor.pending_tasks() == 0 &&
                helix::ui::UpdateQueueTestAccess::queue_empty(queue)) {
                break;
            }
            helix::ui::UpdateQueueTestAccess::drain_all(queue);
        }
    }

    std::string local_path() const {
        return (root_ / "projects" / kName).string();
    }

    /// Script server.files.list for the copy folder: @p entries as listed, or
    /// an error carrying @p error_code.
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

    static std::string copy_path(const std::string& name) {
        return std::string(PrintSelectPanel::kUsbCopyDir) + "/" + name;
    }

    static int user_errors(const LogCapture& log) {
        return log.count_containing("[USER]");
    }

    static constexpr const char* kName = "usb_print_part.gcode";

    MoonrakerClientMock mock_client_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<PrintSelectPanel> panel_;
    lv_obj_t* panel_obj_ = nullptr;
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
    fs::path root_ = fs::temp_directory_path() / "helix_usb_print_stick";
    UsbManager usb_;
};

} // namespace

TEST_CASE_METHOD(UsbPrintFixture,
                 "a USB file offers no delete and asks Moonraker to delete nothing",
                 "[usb][print_select][usb_delete]") {
    // A printer file with the stick file's name: a delete built from the name
    // would remove this one.
    PlantedGcode printer_file(kName);
    REQUIRE(printer_file.on_disk());

    lv_obj_t* del = lv_obj_find_by_name(test_screen(), "delete_button");
    REQUIRE(del != nullptr);
    CHECK(lv_obj_has_flag(del, LV_OBJ_FLAG_HIDDEN));

    // The long-press path reaches these without the button.
    panel_->show_delete_confirmation();
    panel_->delete_file();
    drain();

    CHECK(printer_file.on_disk());
    CHECK(fs::exists(local_path()));
}

TEST_CASE_METHOD(UsbPrintFixture,
                 "a USB file still open after its stick is pulled asks Moonraker to delete nothing",
                 "[usb][print_select][usb_delete]") {
    PlantedGcode printer_file(kName);
    REQUIRE(printer_file.on_disk());

    // Pulling the last stick puts the panel back on the Printer source while
    // the USB file stays selected.
    static_cast<UsbBackendMock*>(usb_.get_backend())->simulate_drive_remove(root_.string());
    panel_->on_usb_drive_removed();
    drain();
    lv_subject_t* is_usb = lv_xml_get_subject(nullptr, "print_source_is_usb");
    REQUIRE(is_usb != nullptr);
    REQUIRE(lv_subject_get_int(is_usb) == 0);

    panel_->show_delete_confirmation();
    panel_->delete_file();
    drain();

    CHECK(printer_file.on_disk());
}

TEST_CASE_METHOD(UsbPrintFixture, "a USB file is copied to Moonraker before it prints",
                 "[usb][print_select][usb_print]") {
    panel_->start_print(/*force=*/true);
    drain();

    const auto& uploads = transfers().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].root == "gcodes");
    CHECK(uploads[0].dest_path == copy_path(kName));
    CHECK(uploads[0].local_path == local_path());
    CHECK(uploads[0].content == "G28\nG1 X10 Y10\n");

    // Moonraker is asked to print the copy, not a same-named file elsewhere.
    const auto [filename, dir] = PrintSelectPanelTestAccess::controller_file(*panel_);
    CHECK(filename == kName);
    CHECK(dir == PrintSelectPanel::kUsbCopyDir);
}

TEST_CASE_METHOD(UsbPrintFixture, "a failed USB copy says so and starts nothing",
                 "[usb][print_select][usb_print]") {
    LogCapture log;
    transfers().mock_fail_path_uploads();

    panel_->start_print(/*force=*/true);
    drain();

    CHECK(log.count_containing(kName) > 0);
    CHECK(user_errors(log) == 1);
    CHECK(transfers().path_uploads().size() == 1);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());

    // The failure releases the in-flight guard: a retry copies again.
    transfers().mock_fail_path_uploads(false);
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(transfers().path_uploads().size() == 2);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first == kName);
}

TEST_CASE_METHOD(UsbPrintFixture, "a USB print keeps what was selected when Print was tapped",
                 "[usb][print_select][usb_print]") {
    PrintSelectPanelTestAccess::set_selected_colors(*panel_, {"#FF0000"});
    transfers().mock_hold_path_uploads();
    panel_->start_print(/*force=*/true);
    drain();

    // A second tap while the copy runs starts no second copy.
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(transfers().path_uploads().size() == 1);

    // Another file opened while the copy runs.
    PrintSelectPanelTestAccess::set_selected_colors(*panel_, {"#00FF00"});
    transfers().release_held_path_uploads();
    drain();

    CHECK(PrintSelectPanelTestAccess::controller_colors(*panel_) ==
          std::vector<std::string>{"#FF0000"});
}

TEST_CASE_METHOD(UsbPrintFixture, "a same-size copy already on the printer is reused, not uploaded",
                 "[usb][print_select][usb_print]") {
    copy_folder_holds({{copy_path(kName), 15}});
    panel_->start_print(/*force=*/true);
    drain();

    CHECK(transfers().path_uploads().empty());
    const auto [filename, dir] = PrintSelectPanelTestAccess::controller_file(*panel_);
    CHECK(filename == kName);
    CHECK(dir == PrintSelectPanel::kUsbCopyDir);
}

TEST_CASE_METHOD(UsbPrintFixture,
                 "a different file under the same name is kept, the copy is suffixed",
                 "[usb][print_select][usb_print]") {
    // A same-size file elsewhere, and one in a subfolder, are not in the copy folder.
    copy_folder_holds(
        {{copy_path(kName), 999}, {kName, 15}, {copy_path(std::string("sub/") + kName), 15}});
    panel_->start_print(/*force=*/true);
    drain();

    REQUIRE(transfers().path_uploads().size() == 1);
    CHECK(transfers().path_uploads()[0].dest_path == copy_path("usb_print_part (2).gcode"));
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first == "usb_print_part (2).gcode");
}

TEST_CASE_METHOD(UsbPrintFixture, "a copy folder that does not exist yet is created by the upload",
                 "[usb][print_select][usb_print]") {
    copy_folder_holds({}, 404);
    panel_->start_print(/*force=*/true);
    drain();

    REQUIRE(transfers().path_uploads().size() == 1);
    CHECK(transfers().path_uploads()[0].dest_path == copy_path(kName));
}

TEST_CASE_METHOD(UsbPrintFixture, "an unreadable copy folder stops the copy rather than guess",
                 "[usb][print_select][usb_print]") {
    LogCapture log;
    copy_folder_holds({}, 500);
    panel_->start_print(/*force=*/true);
    drain();

    CHECK(transfers().path_uploads().empty());
    CHECK(user_errors(log) == 1);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());
}

TEST_CASE_METHOD(UsbPrintFixture, "a USB copy that stops making progress is abandoned",
                 "[usb][print_select][usb_print]") {
    LogCapture log;
    transfers().mock_hold_path_uploads();
    panel_->start_print(/*force=*/true);
    drain();
    REQUIRE(transfers().path_uploads().size() == 1);

    process_lvgl(PrintSelectPanel::kUsbCopyStallMs + 2000);
    drain();
    CHECK(user_errors(log) == 1);

    // The answer that finally arrives belongs to the abandoned copy.
    transfers().release_held_path_uploads();
    drain();
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());

    // Print works again.
    transfers().mock_hold_path_uploads(false);
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(transfers().path_uploads().size() == 2);
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first == kName);
}
