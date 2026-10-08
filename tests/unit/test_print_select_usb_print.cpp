// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_usb_print.cpp
 * @brief Acting on a file from the USB tab never touches a same-named printer file.
 *
 * Run with: ./build/bin/helix-tests "[usb][print_select]"
 *
 * Moonraker cannot see a stick HelixScreen mounted itself, and Moonraker's own
 * storage can hold a file of the same name, so every action on a USB file has
 * to address the stick file, not a Moonraker path built from its name.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_select_panel_test_access.h"
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
    }

    ~UsbPrintFixture() override {
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
