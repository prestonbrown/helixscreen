// SPDX-License-Identifier: GPL-3.0-or-later

// The Firmware Update page's start path: a confirmed Install goes through a fresh
// print_stats query, and only one start can be in flight. The printer is a stock
// U1 with the unverified-apply flag set, the USB stick a temp directory, and the
// flasher a fake runner: nothing here runs a vendor script.

#include "ui_settings_firmware_update.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "board_identity.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_client_mock.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::settings::FirmwareUpdateOverlay;
using json = nlohmann::json;

namespace {

/// Answers print_stats queries with a chosen state, now or when released, and
/// serves everything else through the mock's real handlers.
class PrintStateClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "printer.objects.query" && params.contains("objects") &&
            params["objects"].contains("print_stats")) {
            ++queries;
            if (hold) {
                held.push_back(success_cb);
            } else if (success_cb) {
                success_cb(answer());
            }
            return 1;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    json answer() const {
        return json{{"jsonrpc", "2.0"},
                    {"result", {{"status", {{"print_stats", {{"state", state}}}}}}}};
    }

    void release() {
        for (auto& cb : held) {
            if (cb) {
                cb(answer());
            }
        }
        held.clear();
    }

    std::string state = "standby";
    bool hold = false;
    int queries = 0;
    std::vector<helix::SharedCallback<void(const json&)>> held;
};

struct PageFx : LVGLUITestFixture {
    ScopedEnv flag{"HELIX_FWUPDATE_UNVERIFIED_APPLY", "1"};
    PrintStateClient client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::IMoonrakerClient* previous_client = nullptr;
    std::filesystem::path usb;
    std::atomic<int> launches{0};
    FirmwareUpdateOverlay page;

    PageFx() {
        helix::board::set_current_override(helix::board::Board::SnapmakerU1);
        previous_client = get_moonraker_client();
        set_moonraker_client(&client);
        usb = std::filesystem::temp_directory_path() /
              ("helix_fwupdate_page_" + std::to_string(::getpid()));
        std::filesystem::remove_all(usb);
        std::filesystem::create_directories(usb);
        std::ofstream(usb / "U1_1.2.3.bin", std::ios::binary).put('x');
        std::filesystem::resize_file(usb / "U1_1.2.3.bin", 64ull * 1024 * 1024);

        page.ensure_subjects();
        page.set_image_roots([this] { return std::vector<std::string>{usb.string()}; });
        // The launcher's stand-in: counts the start and records itself as the
        // running flasher, as the real launcher's pid file would.
        page.set_process_runner([this](const std::vector<std::string>& argv, int) {
            ++launches;
            std::ofstream(argv.back(), std::ios::trunc) << ::getpid() << "\n";
            return helix::CommandResult{true, false, 0, "", ""};
        });
        page.refresh(false);
        drain();
    }

    ~PageFx() override {
        drain();
        set_moonraker_client(previous_client);
        helix::board::set_current_override(std::nullopt);
        std::filesystem::remove_all(usb);
    }

    static void drain() {
        for (int i = 0; i < 20; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// Every start handed to the worker pool has run and reported back.
    void settle() {
        REQUIRE(wait_until([] { return helix::http::HttpExecutor::fast().inflight() == 0; }));
        drain();
    }

    static int subject_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
};

} // namespace

TEST_CASE_METHOD(PageFx, "Firmware page: a fresh print_stats of printing starts nothing",
                 "[fwupdate][fwupdate_page]") {
    REQUIRE(subject_int("fwupdate_image_count") == 1);
    client.state = "printing";

    page.request_image_install(0);
    helix::test::answer_top_modal("btn_primary");
    settle();

    CHECK(client.queries == 1);
    CHECK(launches.load() == 0);
    CHECK(subject_int("fwupdate_view") == FirmwareUpdateOverlay::kViewList);
}

TEST_CASE_METHOD(PageFx, "Firmware page: two confirmed installs start one flash",
                 "[fwupdate][fwupdate_page]") {
    REQUIRE(subject_int("fwupdate_image_count") == 1);
    client.hold = true;

    // Both dialogs open before either is answered, as two quick taps make them.
    page.request_image_install(0);
    page.request_image_install(0);
    helix::test::answer_top_modal("btn_primary");
    helix::test::answer_top_modal("btn_primary");
    client.release();
    settle();

    CHECK(launches.load() == 1);
    CHECK(subject_int("fwupdate_view") == FirmwareUpdateOverlay::kViewProgress);
    CHECK(subject_int("fwupdate_run_state") == FirmwareUpdateOverlay::kRunRunning);
}
