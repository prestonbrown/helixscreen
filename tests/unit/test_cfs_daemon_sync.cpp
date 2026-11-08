// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/loopback_ws_server.h"
#include "ams_backend_cfs.h"
#include "cfs_daemon_sync.h"
#include "filament_op_dispatch.h"
#include "moonraker_error.h"
#include "test_helpers/backend_harness.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using helix::cfs::DaemonEndpoint;
using helix::cfs::DaemonMaterial;
using helix::cfs::DaemonSendResult;
using helix::test::LoopbackWsServer;
using Clock = std::chrono::steady_clock;
using helix::AmsError;
using helix::AmsErrorHelper;

namespace {

long ms_since(Clock::time_point since) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

DaemonEndpoint loopback(int port) {
    DaemonEndpoint ep;
    ep.port = port;
    return ep;
}

} // namespace

TEST_CASE("CFS daemon address: units T1..T4 are boxId 1..4, bays A..D are id 0..3, the "
          "external spool is box 0",
          "[cfs][daemon_sync]") {
    const int ext = helix::ui::kExternalSpoolSlot;
    auto a = helix::cfs::daemon_address(0, ext);
    REQUIRE(a);
    CHECK(a->box_id == 1);
    CHECK(a->slot_id == 0);
    a = helix::cfs::daemon_address(6, ext); // T2C
    REQUIRE(a);
    CHECK(a->box_id == 2);
    CHECK(a->slot_id == 2);
    a = helix::cfs::daemon_address(15, ext); // T4D
    REQUIRE(a);
    CHECK(a->box_id == 4);
    CHECK(a->slot_id == 3);
    a = helix::cfs::daemon_address(ext, ext);
    REQUIRE(a);
    CHECK(a->box_id == 0);
    CHECK(a->slot_id == 0);
    CHECK_FALSE(helix::cfs::daemon_address(16, ext));
    CHECK_FALSE(helix::cfs::daemon_address(-1, ext));
}

TEST_CASE("CFS daemon color is '#' plus tn_data's 0RRGGBB", "[cfs][daemon_sync]") {
    // The boxsInfo capture reports tn_data's own 7-digit color behind a '#'.
    CHECK(helix::cfs::daemon_color(0xFFFFFF) == "#0ffffff");
    CHECK(helix::cfs::daemon_color(0xFFF014) == "#0fff014");
    CHECK(helix::cfs::daemon_color(0x000000) == "#0000000");
    // The alpha byte of a 32-bit color never leaks into the leading nibble.
    CHECK(helix::cfs::daemon_color(0xAA112233) == "#0112233");
}

TEST_CASE("CFS daemon modifyMaterial frame carries every field of the capture",
          "[cfs][daemon_sync]") {
    DaemonMaterial m;
    m.box_id = 1;
    m.slot_id = 0;
    m.rfid = "04001";
    m.type = "PLA";
    m.vendor = "Creality";
    m.name = "CR-PLA";
    m.color_rgb = 0xFFFFFF;
    m.min_temp_c = 190;
    m.max_temp_c = 240;

    const json frame = json::parse(helix::cfs::encode_modify_material(m), nullptr, false);
    REQUIRE_FALSE(frame.is_discarded());
    CHECK(frame["method"] == "set");
    const json& r = frame["params"]["modifyMaterial"];
    CHECK(r.size() == 10);
    CHECK(r["boxId"] == 1);
    CHECK(r["id"] == 0);
    CHECK(r["rfid"] == "04001");
    CHECK(r["type"] == "PLA");
    CHECK(r["vendor"] == "Creality");
    CHECK(r["name"] == "CR-PLA");
    CHECK(r["color"] == "#0ffffff");
    CHECK(r["minTemp"].is_number_float());
    CHECK(r["minTemp"] == 190.0);
    CHECK(r["maxTemp"] == 240.0);
    CHECK(r["pressure"] == "0.04");
}

TEST_CASE("CFS daemon send gives up fast when nothing listens", "[cfs][daemon_sync]") {
    auto ep = loopback(LoopbackWsServer::closed_port());
    const auto start = Clock::now();
    CHECK(helix::cfs::send_daemon_frame(ep, "{}") == DaemonSendResult::ConnectFailed);
    CHECK(ms_since(start) < ep.connect_timeout_ms + 100);
}

TEST_CASE("CFS daemon send stops at its budget when the daemon never answers",
          "[cfs][daemon_sync]") {
    LoopbackWsServer server(LoopbackWsServer::Mode::Silent);
    auto ep = loopback(server.port());
    ep.total_budget_ms = 300;
    const auto start = Clock::now();
    CHECK(helix::cfs::send_daemon_frame(ep, "{}") == DaemonSendResult::TimedOut);
    const long took = ms_since(start);
    CHECK(took >= 250);
    CHECK(took < ep.total_budget_ms + 150);
}

TEST_CASE("CFS daemon send delivers one masked text frame after the upgrade",
          "[cfs][daemon_sync]") {
    LoopbackWsServer server;
    const std::string payload = R"({"method":"set","params":{}})";
    CHECK(helix::cfs::send_daemon_frame(loopback(server.port()), payload) ==
          DaemonSendResult::Delivered);
    CHECK(server.wait_for_text_frame(std::chrono::seconds(2)) == payload);
}

namespace {

/// Holds every queued notice job until the test runs it, and records what
/// reaches the transport.
struct HeldDaemonJobs {
    std::vector<std::function<void()>> jobs;
    std::vector<std::string> sent;

    void post(const helix::cfs::DaemonAddress& bay, const std::string& payload) {
        helix::cfs::post_daemon_frame(
            DaemonEndpoint{}, bay, payload,
            [this](std::function<void()> job) { jobs.push_back(std::move(job)); },
            [this](const DaemonEndpoint&, const std::string& frame) {
                sent.push_back(frame);
                return DaemonSendResult::Delivered;
            });
    }
};

} // namespace

TEST_CASE("CFS daemon notices for one bay end on the newest edit", "[cfs][daemon_sync]") {
    HeldDaemonJobs held;
    held.post({2, 1}, "older");
    held.post({2, 1}, "newer");
    REQUIRE(held.jobs.size() == 2);

    SECTION("the newer job runs first: the held older one drops itself") {
        held.jobs[1]();
        held.jobs[0]();
        REQUIRE(held.sent.size() == 1);
        CHECK(held.sent[0] == "newer");
    }

    SECTION("in order: the older job, already overtaken when it runs, sends nothing") {
        held.jobs[0]();
        held.jobs[1]();
        REQUIRE(held.sent.size() == 1);
        CHECK(held.sent[0] == "newer");
    }
}

TEST_CASE("CFS daemon notices for different bays never supersede each other",
          "[cfs][daemon_sync]") {
    HeldDaemonJobs held;
    held.post({2, 1}, "T2B");
    held.post({2, 2}, "T2C");
    held.post({0, 0}, "external");
    REQUIRE(held.jobs.size() == 3);
    for (auto it = held.jobs.rbegin(); it != held.jobs.rend(); ++it) {
        (*it)();
    }
    CHECK(held.sent == std::vector<std::string>{"external", "T2C", "T2B"});
}

// ---------------------------------------------------------------------------
// The backend's stock write path

namespace {

json stock_unit_box(const std::vector<std::string>& material_types,
                    const std::vector<std::string>& color_values) {
    json box = json::parse(R"({
        "state": "connect", "filament": 0, "auto_refill": 1, "enable": 1,
        "filament_useup": 1,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {"state": "connect", "filament": "None", "temperature": "27",
               "dry_and_humidity": "48", "version": "1.1.3", "sn": "SERIAL",
               "change_color_num": ["-1", "-1", "-1", "-1"],
               "vender": ["Generic", "Generic", "-1", "-1"],
               "remain_len": ["100", "100", "-1", "-1"]}
    })");
    box["T1"]["material_type"] = material_types;
    box["T1"]["color_value"] = color_values;
    return json{{"params", json::array({json{{"box", box}}, 0})}};
}

class DaemonCaptureCfs : public helix::printer::AmsBackendCfs {
  public:
    using AmsBackendCfs::AmsBackendCfs;
    using AmsBackendCfs::push_slot_identity_to_firmware;

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        captured.push_back(gcode);
        if (klipper_rejects) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::JSON_RPC_ERROR;
            err.message = "rejected";
            if (on_error)
                on_error(err);
        } else if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }
    void notify_slot_daemon(const DaemonMaterial& material) override {
        notices.push_back(material);
        if (send_for_real)
            AmsBackendCfs::notify_slot_daemon(material);
    }
    void point_daemon_at(int port) {
        daemon_endpoint_.port = port;
        send_for_real = true;
    }

    std::vector<std::string> captured;
    std::vector<DaemonMaterial> notices;
    bool klipper_rejects = false;
    bool send_for_real = false;
};

} // namespace

TEST_CASE("CFS slot write tells the daemon what Klipper accepted", "[cfs][daemon_sync]") {
    helix::test::StartedBackend<DaemonCaptureCfs> cfs;
    // Slot B carries the K1 Generic PETG code, so an edit to PETG writes it.
    cfs.fire_status(stock_unit_box({"-1", "000003", "-1", "-1"}, {"-1", "01B04AE", "-1", "-1"}));

    SECTION("material code and color, addressed to T1A") {
        cfs.push_slot_identity_to_firmware(0, "PETG", "Generic", "", 0xFF0000);
        REQUIRE(cfs.captured.size() == 2);
        REQUIRE(cfs.captured[0].find("PART=material_type DATA=000003") != std::string::npos);
        REQUIRE(cfs.notices.size() == 1);
        const auto& n = cfs.notices[0];
        CHECK(n.box_id == 1);
        CHECK(n.slot_id == 0);
        CHECK(n.rfid == "00003");
        CHECK(n.type == "PETG");
        CHECK(n.vendor == "Generic");
        CHECK(n.color_rgb == 0xFF0000);
        CHECK(n.min_temp_c > 0);
        CHECK(n.max_temp_c >= n.min_temp_c);
    }

    SECTION("a color-only write sends an empty rfid") {
        cfs.push_slot_identity_to_firmware(3, "Unobtanium", "ACME", "", 0x00FF00);
        REQUIRE(cfs.captured[0].find("PART=material_type") == std::string::npos);
        REQUIRE(cfs.notices.size() == 1);
        CHECK(cfs.notices[0].slot_id == 3);
        CHECK(cfs.notices[0].rfid.empty());
        CHECK(cfs.notices[0].name == "Unobtanium");
        CHECK(cfs.notices[0].vendor == "ACME");
    }

    SECTION("a raw tag UID is never sent as the vendor") {
        cfs.push_slot_identity_to_firmware(3, "Unobtanium", "04A1B2C3D4E5F6", "", 0x00FF00);
        REQUIRE(cfs.notices.size() == 1);
        CHECK(cfs.notices[0].vendor.empty());
    }

    SECTION("a write Klipper rejects tells the daemon nothing") {
        cfs.klipper_rejects = true;
        cfs.push_slot_identity_to_firmware(0, "PETG", "Generic", "", 0xFF0000);
        REQUIRE_FALSE(cfs.captured.empty());
        CHECK(cfs.notices.empty());
    }
}

TEST_CASE("CFS slot write completes at once with no daemon listening", "[cfs][daemon_sync]") {
    helix::test::StartedBackend<DaemonCaptureCfs> cfs;
    cfs.fire_status(stock_unit_box({"-1", "000003", "-1", "-1"}, {"-1", "01B04AE", "-1", "-1"}));
    cfs.point_daemon_at(LoopbackWsServer::closed_port());

    const auto start = Clock::now();
    cfs.push_slot_identity_to_firmware(0, "PETG", "Generic", "", 0xFF0000);
    // The send runs on a worker, so the write never waits on even the connect.
    CHECK(ms_since(start) < 50);
    REQUIRE(cfs.captured.size() == 2);
    CHECK(cfs.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
    REQUIRE(cfs.notices.size() == 1);
}

TEST_CASE("CFS slot write reaches a listening daemon as a modifyMaterial frame",
          "[cfs][daemon_sync]") {
    helix::test::StartedBackend<DaemonCaptureCfs> cfs;
    cfs.fire_status(stock_unit_box({"-1", "000003", "-1", "-1"}, {"-1", "01B04AE", "-1", "-1"}));
    LoopbackWsServer server;
    cfs.point_daemon_at(server.port());

    cfs.push_slot_identity_to_firmware(1, "PETG", "Generic", "", 0x123456);
    const auto text = server.wait_for_text_frame(std::chrono::seconds(3));
    REQUIRE(text);
    const json frame = json::parse(*text, nullptr, false);
    REQUIRE_FALSE(frame.is_discarded());
    CHECK(frame["params"]["modifyMaterial"]["boxId"] == 1);
    CHECK(frame["params"]["modifyMaterial"]["id"] == 1);
    CHECK(frame["params"]["modifyMaterial"]["color"] == "#0123456");
}
