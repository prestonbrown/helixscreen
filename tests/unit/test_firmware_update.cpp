// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_update.h"
#include "firmware_update_runner.h"
#include "update_manager_status.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::fwupdate;
using helix::board::Board;
using json = nlohmann::json;

namespace {

HostFacts host(Board board, const char* mod = "none", const char* type = "") {
    HostFacts h;
    h.board = board;
    h.host_mod = mod;
    h.printer_type = type;
    return h;
}

std::string id_of(const HostFacts& h) {
    const Provider* p = provider_for(h);
    return p != nullptr ? provider_id(*p) : "";
}

const Provider& u1_stock() {
    return *provider_for(host(Board::SnapmakerU1));
}

std::filesystem::path fresh_dir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("helix_fwupdate_" + std::to_string(::getpid()) + "_" + tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void make_file(const std::filesystem::path& path, uint64_t size) {
    std::ofstream(path, std::ios::binary | std::ios::trunc).put('x');
    std::filesystem::resize_file(path, size);
}

constexpr uint64_t kMiB = 1024ull * 1024ull;

ApplyInputs idle_inputs() {
    ApplyInputs in;
    in.lifecycle = PrintState::Idle;
    in.print_stats_state = "standby";
    return in;
}

} // namespace

// ============================================================================
// Detection
// ============================================================================

TEST_CASE("Firmware update: each host gets its own firmware's row", "[fwupdate]") {
    CHECK(id_of(host(Board::SnapmakerU1)) == "snapmaker_u1_stock");
    CHECK(id_of(host(Board::SnapmakerU1, "paxx")) == "snapmaker_u1_paxx");
    CHECK(id_of(host(Board::K1, "simple_af")) == "creality_k1");
    CHECK(id_of(host(Board::K2)) == "creality_k2");
    CHECK(id_of(host(Board::Ad5m, "forge_x")) == "flashforge_ad5m");
    CHECK(id_of(host(Board::Ad5x, "zmod")) == "flashforge_ad5x");
    CHECK(id_of(host(Board::Creator5)) == "flashforge_creator5");
    CHECK(id_of(host(Board::Creator5Pro)) == "flashforge_creator5_pro");
    CHECK(id_of(host(Board::Cc1, "cosmos")) == "elegoo_cc1");
    CHECK(id_of(host(Board::Generic, "none", "QIDI Max 4")) == "qidi");
    CHECK(id_of(host(Board::Generic, "none", "Voron 2.4")).empty());
}

TEST_CASE("Firmware update: only the stock U1 can apply, and only behind the flag", "[fwupdate]") {
    const Provider& stock = u1_stock();
    REQUIRE(provider_readiness(stock) == Readiness::NeedsDeviceVerification);
    CHECK_FALSE(apply_enabled(stock, false));
    CHECK(apply_enabled(stock, true));

    // Rows with no apply plan stay off whatever the flag says.
    for (const HostFacts& h :
         {host(Board::SnapmakerU1, "paxx"), host(Board::K1), host(Board::K2), host(Board::Ad5x),
          host(Board::Cc1), host(Board::Generic, "none", "QIDI Q2")}) {
        INFO(id_of(h));
        CHECK_FALSE(apply_enabled(*provider_for(h), true));
        CHECK(apply_plan(*provider_for(h), "/media/usb0/x", "/tmp/log").argv.empty());
    }
}

TEST_CASE("Firmware update: the unverified-apply flag is exactly HELIX_FWUPDATE_UNVERIFIED_APPLY=1",
          "[fwupdate]") {
    ::unsetenv("HELIX_FWUPDATE_UNVERIFIED_APPLY");
    CHECK_FALSE(unverified_apply_allowed());
    ::setenv("HELIX_FWUPDATE_UNVERIFIED_APPLY", "0", 1);
    CHECK_FALSE(unverified_apply_allowed());
    ::setenv("HELIX_FWUPDATE_UNVERIFIED_APPLY", "1", 1);
    CHECK(unverified_apply_allowed());
    ::unsetenv("HELIX_FWUPDATE_UNVERIFIED_APPLY");
}

// ============================================================================
// Images
// ============================================================================

TEST_CASE("Firmware update: image files are checked against the row's rule", "[fwupdate]") {
    const Provider& u1 = u1_stock();
    CHECK(check_image(u1, {"/m/U1_1.2.bin", "U1_1.2.bin", 200 * kMiB}) == ImageVerdict::Ok);
    CHECK(check_image(u1, {"/m/U1.BIN", "U1.BIN", 200 * kMiB}) == ImageVerdict::Ok);
    CHECK(check_image(u1, {"/m/U1.bin", "U1.bin", 49 * kMiB}) == ImageVerdict::TooSmall);
    CHECK(check_image(u1, {"/m/U1.img", "U1.img", 200 * kMiB}) == ImageVerdict::WrongType);
    CHECK(check_image(u1, {"/m/bin", "bin", 200 * kMiB}) == ImageVerdict::WrongType);

    const Provider& ad5x = *provider_for(host(Board::Ad5x));
    CHECK(check_image(ad5x, {"/m/AD5X-1.1.7.tgz", "AD5X-1.1.7.tgz", kMiB}) == ImageVerdict::Ok);
    CHECK(check_image(ad5x, {"/m/Adventurer5M-3.tgz", "Adventurer5M-3.tgz", kMiB}) ==
          ImageVerdict::WrongModel);
    const Provider& c5 = *provider_for(host(Board::Creator5));
    CHECK(check_image(c5, {"/m/Creator5Pro-2.tgz", "Creator5Pro-2.tgz", kMiB}) ==
          ImageVerdict::WrongModel);

    const Provider& cc1 = *provider_for(host(Board::Cc1));
    CHECK(check_image(cc1, {"/m/x.swu", "x.swu", kMiB}) == ImageVerdict::WrongType);
}

TEST_CASE("Firmware update: images are found in each stick's root only, in name order",
          "[fwupdate]") {
    const auto a = fresh_dir("usb_a");
    const auto b = fresh_dir("usb_b");
    make_file(a / "zz_U1.bin", 60 * kMiB);
    make_file(a / "small.bin", kMiB);
    make_file(a / "notes.txt", 60 * kMiB);
    std::filesystem::create_directories(a / "sub");
    make_file(a / "sub" / "deep.bin", 60 * kMiB);
    make_file(b / "aa_U1.bin", 60 * kMiB);

    const auto found = find_images(u1_stock(), {a.string(), b.string(), "/nonexistent/usb"});
    REQUIRE(found.size() == 2);
    CHECK(found[0].name == "aa_U1.bin");
    CHECK(found[0].path == (b / "aa_U1.bin").string());
    CHECK(found[0].size == 60 * kMiB);
    CHECK(found[1].name == "zz_U1.bin");

    std::filesystem::remove_all(a);
    std::filesystem::remove_all(b);
}

// ============================================================================
// Preconditions
// ============================================================================

TEST_CASE("Firmware update: a job, an unknown print state, a running update or no room blocks",
          "[fwupdate]") {
    CHECK(first_blocker(idle_inputs()) == Blocker::None);

    for (PrintState s : {PrintState::Preparing, PrintState::Printing, PrintState::Paused}) {
        ApplyInputs in = idle_inputs();
        in.lifecycle = s;
        CHECK(first_blocker(in) == Blocker::JobRunning);
    }
    for (const char* s : {"printing", "paused"}) {
        ApplyInputs in = idle_inputs();
        in.print_stats_state = s;
        CHECK(first_blocker(in) == Blocker::JobRunning);
    }
    for (const char* s : {"", "startup", "PRINTING"}) {
        ApplyInputs in = idle_inputs();
        in.print_stats_state = s;
        CHECK(first_blocker(in) == Blocker::PrintStateUnknown);
    }
    for (const char* s : {"complete", "cancelled", "error"}) {
        ApplyInputs in = idle_inputs();
        in.print_stats_state = s;
        CHECK(first_blocker(in) == Blocker::None);
    }

    ApplyInputs busy = idle_inputs();
    busy.update_running = true;
    CHECK(first_blocker(busy) == Blocker::UpdateRunning);

    ApplyInputs space = idle_inputs();
    space.required_bytes = 100;
    CHECK(first_blocker(space) == Blocker::SpaceUnknown);
    space.free_bytes = 99;
    CHECK(first_blocker(space) == Blocker::NotEnoughSpace);
    space.free_bytes = 100;
    CHECK(first_blocker(space) == Blocker::None);

    // A source that needs no local space never reads free space.
    ApplyInputs none_needed = idle_inputs();
    none_needed.free_bytes = 0;
    CHECK(first_blocker(none_needed) == Blocker::None);
}

TEST_CASE("Firmware update: the stock U1 confirm says HelixScreen is removed", "[fwupdate]") {
    CHECK(confirm_points(u1_stock()) ==
          std::vector<ConfirmPoint>{ConfirmPoint::KeepPowerOn, ConfirmPoint::StopsKlipper,
                                    ConfirmPoint::PrinterReboots,
                                    ConfirmPoint::RemovesHelixScreen});
    CHECK(confirm_points(*provider_for(host(Board::Cc1))).empty());
}

// ============================================================================
// Apply plan and progress
// ============================================================================

TEST_CASE("Firmware update: the stock U1 plan launches systemUpgrade.sh detached", "[fwupdate]") {
    const ApplyPlan plan = apply_plan(u1_stock(), "/media/usb0/U1 1.2.bin", "/tmp/fw.log");
    CHECK(plan.argv ==
          std::vector<std::string>{"/bin/sh", "-c",
                                   "setsid \"$1\" upgrade all \"$2\" </dev/null >\"$3\" 2>&1 &",
                                   "fwupdate", "/home/lava/bin/systemUpgrade.sh",
                                   "/media/usb0/U1 1.2.bin", "/tmp/fw.log"});
    CHECK(plan.log_path == "/tmp/fw.log");
    CHECK(plan.lock_path == "/tmp/.system_upgrade.lock");
    CHECK(plan.unpack_dir == "/tmp");
}

TEST_CASE("Firmware update: stock U1 script lines map to stages", "[fwupdate]") {
    const Provider& u1 = u1_stock();
    struct Case {
        const char* line;
        Stage stage;
        int percent; // -1: none
        int index;
        bool ponr;
    };
    // Lines as systemUpgrade.sh prints them.
    const Case cases[] = {
        {"[UPG] upgrade_all: Upgrade all start: 2026-11-20 10:00:00", Stage::Unpacking, 1, 0,
         false},
        {"[UPG] upgrade_all: Unpack duration: 12s", Stage::None, -1, 0, false},
        {"[UPG] upgrade_all: Shutdown klipper process.", Stage::MainBoard, 9, 0, false},
        {"[UPG] upgrade_all: The MCU0 upgrade was successful.", Stage::Toolhead, 18, 1, false},
        {"[UPG] upgrade_all: The Print head head0 upgrade was successful.", Stage::Toolhead, 37, 2,
         false},
        {"[UPG] upgrade_all: Print head head1 offline, do not upgrade.", Stage::Toolhead, 56, 3,
         false},
        {"[UPG] upgrade_all: The Print head head2 upgrade was successful.", Stage::Toolhead, 75, 4,
         false},
        {"[UPG] upgrade_all: The Print head head3 upgrade was successful.", Stage::MainSystem, 93,
         0, false},
        {"[UPG] upgrade_all: Prepare to upgrade SOC.", Stage::MainSystem, 93, 0, false},
        {"[UPG] upgrade_soc: upgrade soc finish, prepare to reboot.", Stage::Rebooting, 99, 0,
         true},
        {"[UPG] upgrade_all: Main MCU upgrade failed, revert to old version. Upgrade abort.",
         Stage::Failed, -1, 0, false},
        {"[UPG] upgrade_all: Unpack upgrade file failed. Upgrade abort.", Stage::Failed, -1, 0,
         false},
        {"The upgrade is in progress.", Stage::Failed, -1, 0, false},
        {"sh: setsid: not found", Stage::Failed, -1, 0, false},
    };
    for (const Case& c : cases) {
        INFO(c.line);
        const ProgressEvent ev = parse_progress(u1, c.line);
        CHECK(ev.stage == c.stage);
        CHECK(ev.percent.value_or(-1) == c.percent);
        CHECK(ev.index == c.index);
        CHECK(ev.point_of_no_return == c.ponr);
    }
    // A row with no flasher reports nothing for any line.
    CHECK(parse_progress(*provider_for(host(Board::K1)), "ota: data processed: 50%").stage ==
          Stage::None);
}

// ============================================================================
// Starting a vendor update
// ============================================================================

namespace {
struct FakeRunner {
    std::vector<std::vector<std::string>> calls;
    helix::CommandResult answer{true, false, 0, "", ""};
    helix::ProcessRunner runner() {
        return [this](const std::vector<std::string>& argv, int) {
            calls.push_back(argv);
            return answer;
        };
    }
};

ApplyPlan test_plan(const std::filesystem::path& dir) {
    ApplyPlan plan = apply_plan(u1_stock(), "/media/usb0/U1.bin", (dir / "fw.log").string());
    plan.lock_path = (dir / "lock").string();
    plan.unpack_dir = dir.string();
    return plan;
}
} // namespace

TEST_CASE("Firmware update: the flasher starts only when enabled and nothing blocks",
          "[fwupdate]") {
    const auto dir = fresh_dir("start");
    const ApplyPlan plan = test_plan(dir);
    REQUIRE_FALSE(plan.argv.empty());
    FakeRunner fake;

    SECTION("flag off: the launcher is never run") {
        const StartResult r =
            start_vendor_apply(u1_stock(), plan, idle_inputs(), false, fake.runner());
        CHECK(r.outcome == StartOutcome::ApplyDisabled);
        CHECK(fake.calls.empty());
    }
    SECTION("a row without an apply plan never runs it, flag or not") {
        const StartResult r = start_vendor_apply(*provider_for(host(Board::K1)), plan,
                                                 idle_inputs(), true, fake.runner());
        CHECK(r.outcome == StartOutcome::ApplyDisabled);
        CHECK(fake.calls.empty());
    }
    SECTION("printing: blocked") {
        ApplyInputs in = idle_inputs();
        in.print_stats_state = "printing";
        const StartResult r = start_vendor_apply(u1_stock(), plan, in, true, fake.runner());
        CHECK(r.outcome == StartOutcome::Blocked);
        CHECK(r.blocker == Blocker::JobRunning);
        CHECK(fake.calls.empty());
    }
    SECTION("the vendor's lock file means an update is already running") {
        make_file(plan.lock_path, 0);
        const StartResult r =
            start_vendor_apply(u1_stock(), plan, idle_inputs(), true, fake.runner());
        CHECK(r.outcome == StartOutcome::Blocked);
        CHECK(r.blocker == Blocker::UpdateRunning);
        CHECK(fake.calls.empty());
    }
    SECTION("free space where the flasher unpacks is read and weighed") {
        ApplyInputs in = idle_inputs();
        in.required_bytes = UINT64_MAX;
        const StartResult r = start_vendor_apply(u1_stock(), plan, in, true, fake.runner());
        CHECK(r.blocker == Blocker::NotEnoughSpace);
        CHECK(fake.calls.empty());
    }
    SECTION("clear: the launcher runs once with the plan's argv") {
        ApplyInputs in = idle_inputs();
        in.required_bytes = 1;
        const StartResult r = start_vendor_apply(u1_stock(), plan, in, true, fake.runner());
        CHECK(r.outcome == StartOutcome::Started);
        REQUIRE(fake.calls.size() == 1);
        CHECK(fake.calls[0] == plan.argv);
    }
    SECTION("a launcher that fails is reported with its output") {
        fake.answer = {true, false, 127, "sh: setsid: not found\n", ""};
        const StartResult r =
            start_vendor_apply(u1_stock(), plan, idle_inputs(), true, fake.runner());
        CHECK(r.outcome == StartOutcome::LaunchFailed);
        CHECK(r.error == "sh: setsid: not found");
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("Firmware update: the log tail returns whole new lines only", "[fwupdate]") {
    const auto dir = fresh_dir("tail");
    const auto path = dir / "fw.log";
    LogTail tail(path.string());
    CHECK(tail.poll().empty()); // not created yet

    std::ofstream(path, std::ios::app) << "one\ntw";
    CHECK(tail.poll() == std::vector<std::string>{"one"});
    CHECK(tail.poll().empty());
    std::ofstream(path, std::ios::app) << "o\r\nthree\n";
    CHECK(tail.poll() == std::vector<std::string>{"two", "three"});

    // Truncated and rewritten: read from the start again.
    std::ofstream(path, std::ios::trunc) << "new\n";
    CHECK(tail.poll() == std::vector<std::string>{"new"});
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Moonraker update_manager
// ============================================================================

TEST_CASE("Update manager: entries, what each has available, HelixScreen's own left out",
          "[fwupdate]") {
    const json result = json::parse(R"({
      "busy": false,
      "github_rate_limit": 60,
      "version_info": {
        "system": {"package_count": 2, "package_list": ["a", "b"]},
        "moonraker": {"configured_type": "git_repo", "channel": "dev", "is_valid": true,
                      "corrupt": false, "version": "v0.9.3-12", "remote_version": "v0.9.3-12",
                      "commits_behind": [], "commits_behind_count": 0},
        "klipper": {"configured_type": "git_repo", "channel": "dev", "is_valid": true,
                    "corrupt": false, "version": "v0.13.0-100", "remote_version": "v0.13.0-103",
                    "commits_behind": [{}, {}, {}], "commits_behind_count": 3},
        "mainsail": {"configured_type": "web", "channel": "stable", "is_valid": true,
                     "version": "v2.12.0", "remote_version": "v2.13.0"},
        "fluidd": {"configured_type": "web", "channel": "stable", "is_valid": true,
                   "version": "v1.30.0", "remote_version": "v1.30.0"},
        "broken": {"configured_type": "git_repo", "is_valid": false, "version": "?",
                   "remote_version": "v1"},
        "oldgit": {"configured_type": "git_repo", "version": "v1.0", "remote_version": "v1.1"},
        "unknownremote": {"configured_type": "zip", "version": "v1.0", "remote_version": "?"},
        "helixscreen": {"configured_type": "web", "version": "v1.0", "remote_version": "v1.1"}
      }
    })");
    const auto status = parse_update_status(result);
    REQUIRE(status.has_value());
    CHECK_FALSE(status->busy);

    std::map<std::string, ManagedEntry> by_name;
    for (const auto& e : status->entries) {
        by_name[e.name] = e;
    }
    CHECK(by_name.count("helixscreen") == 0);
    REQUIRE(by_name.size() == 8);
    CHECK(by_name["system"].kind == "system");
    CHECK(by_name["system"].package_count == 2);
    CHECK(by_name["system"].update_available);
    CHECK_FALSE(by_name["moonraker"].update_available);
    CHECK(by_name["klipper"].update_available);
    CHECK(by_name["klipper"].version == "v0.13.0-100");
    CHECK(by_name["klipper"].remote_version == "v0.13.0-103");
    CHECK(by_name["mainsail"].update_available);
    CHECK_FALSE(by_name["fluidd"].update_available);
    CHECK_FALSE(by_name["broken"].valid);
    CHECK_FALSE(by_name["broken"].update_available);
    CHECK(by_name["oldgit"].update_available);
    CHECK_FALSE(by_name["unknownremote"].update_available);
}

TEST_CASE("Update manager: a status without busy reads as busy; junk is no status", "[fwupdate]") {
    const auto status = parse_update_status(json{{"version_info", json::object()}});
    REQUIRE(status.has_value());
    CHECK(status->busy);
    CHECK_FALSE(parse_update_status(json::array()).has_value());
    CHECK_FALSE(parse_update_status(json{{"busy", false}}).has_value());
}

TEST_CASE("Update manager: notify_update_response lines are read", "[fwupdate]") {
    const json msg = {{"jsonrpc", "2.0"},
                      {"method", "notify_update_response"},
                      {"params",
                       {{{"application", "klipper"},
                         {"proc_id", 7},
                         {"message", "Receiving objects:  45% (12/27)"},
                         {"complete", false}}}}};
    const auto r = parse_update_response(msg);
    REQUIRE(r.has_value());
    CHECK(r->application == "klipper");
    CHECK(r->message == "Receiving objects:  45% (12/27)");
    CHECK_FALSE(r->complete);
    CHECK_FALSE(response_reports_failure(*r));

    json no_complete = msg;
    no_complete["params"][0].erase("complete");
    CHECK_FALSE(parse_update_response(no_complete).has_value());
    CHECK_FALSE(parse_update_response(json{{"params", json::array()}}).has_value());

    UpdateResponse failed{"klipper", "Error updating klipper: git pull failed", true};
    CHECK(response_reports_failure(failed));
}

TEST_CASE("Update manager: the percentage in a progress message", "[fwupdate]") {
    CHECK(percent_in_message("Receiving objects:  45% (12/27)") == 45);
    CHECK(percent_in_message("Resolving deltas: 100% (5/5), done.") == 100);
    CHECK(percent_in_message("Counting 10%, then 55%") == 55);
    CHECK_FALSE(percent_in_message("Updating Repo...").has_value());
    CHECK_FALSE(percent_in_message("1000%").has_value());
    CHECK_FALSE(percent_in_message("% done").has_value());
}
