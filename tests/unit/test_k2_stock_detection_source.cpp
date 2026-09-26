// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "k2_stock_detection_source.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>

#include "../catch_amalgamated.hpp"

using helix::detection::DetectionEvent;
using helix::detection::DetectionKind;
using helix::detection::K2StockDetectionSource;
using json = nlohmann::json;

TEST_CASE("run_detection_with_deadline bounds the child", "[detection][k2]") {
    SECTION("a child that outlives the deadline is killed, no result") {
        helix::TextLogCapture capture;
        std::string out;
        const int rc = helix::detection::run_detection_with_deadline({"/bin/sleep", "30"}, out, 1);
        CHECK(rc == -1);
        CHECK(out.empty());
        CHECK(capture.contains("killed"));
    }
    SECTION("a well-behaved child: exit code and captured stdout") {
        std::string out;
        const int rc =
            helix::detection::run_detection_with_deadline({"/bin/echo", "prob: 0.9"}, out, 20);
        CHECK(rc == 0);
        CHECK(out.find("prob: 0.9") != std::string::npos);
    }
}

TEST_CASE("max_spaghetti_probability parses /usr/bin/detection stdout", "[detection][k2]") {
    SECTION("positive: duplicated detection lines yield the max prob") {
        // Verbatim from a K2 Plus run on a real spaghetti photo (#1378).
        const char* out = "label: 1 prob: 0.903177 x:151.378 y:302.575 w:744.139 h:569.242\n"
                          "label: 1 prob: 0.903177 x:151.378 y:302.575 w:744.139 h:569.242\n";
        const auto p = helix::detection::max_spaghetti_probability(out);
        REQUIRE(p.has_value());
        REQUIRE(*p == Catch::Approx(0.903177f).epsilon(0.001));
    }
    SECTION("positive: two different boxes, highest wins") {
        const auto p = helix::detection::max_spaghetti_probability(
            "label: 1 prob: 0.710000 x:1 y:2 w:3 h:4\n"
            "label: 1 prob: 0.830000 x:5 y:6 w:7 h:8\n");
        REQUIRE(p.has_value());
        REQUIRE(*p == Catch::Approx(0.83f).epsilon(0.001));
    }
    SECTION("negative: zero detections print nothing") {
        REQUIRE_FALSE(helix::detection::max_spaghetti_probability("").has_value());
    }
    SECTION("garbage: ai_engine's format and junk are not detection lines") {
        REQUIRE_FALSE(
            helix::detection::max_spaghetti_probability("num 1 re_label 1 re_prob 0.9\nnum 1\n")
                .has_value());
        REQUIRE_FALSE(
            helix::detection::max_spaghetti_probability("label: 1 prob: abc x:1 y:2 w:3 h:4\n")
                .has_value());
        REQUIRE_FALSE(helix::detection::max_spaghetti_probability("random stdout\n").has_value());
    }
}

namespace {

/// Drive one poll round synchronously: poll_tick (gates + submit), the
/// injected submitter runs the work inline, then the deferred handle_result is
/// drained out of the UpdateQueue onto the main thread.
struct PollHarness {
    K2StockDetectionSource src;
    int fetch_calls = 0;
    bool fetch_ok = true;
    std::string runner_stdout; // empty + exit 0 models "no detections"
    int runner_exit = 0;
    std::vector<DetectionEvent> events;

    explicit PollHarness(helix::PrinterState* state, const std::string& config_path = {})
        : src(state) {
        src.set_fetcher([this](const std::string&, const std::string&) {
            ++fetch_calls;
            return fetch_ok;
        });
        src.set_runner([this](const std::vector<std::string>&, std::string& out) {
            out = runner_stdout;
            return runner_exit;
        });
        src.set_submitter([](std::function<void()> work) { work(); });
        src.set_callback([this](const DetectionEvent& e) { events.push_back(e); });
        if (!config_path.empty())
            src.set_config_path_for_test(config_path);
        // start() probes the real config, so force capability after it.
        src.start();
        src.set_capable_for_test(true);
    }

    void poll() {
        src.poll_tick();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

void set_print_state(helix::PrinterState& state, const char* s) {
    state.update_from_status(json{{"print_stats", {{"state", s}}}});
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "K2StockSource gates polling on print state", "[detection][k2]") {
    PollHarness h(&state());

    SECTION("standby: no fetch, no event") {
        set_print_state(state(), "standby");
        h.poll();
        CHECK(h.fetch_calls == 0);
        CHECK(h.events.empty());
    }
    SECTION("paused counts as an active job") {
        set_print_state(state(), "paused");
        h.poll();
        CHECK(h.fetch_calls == 1);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "K2StockSource gates polling on the detection toggle",
                 "[detection][k2]") {
    // SettingsManager subjects start uninitialized in the unit suite (an
    // uninitialized subject reads as the true fallback and ignores setters),
    // so rebuild them from Config the way test_detection_settings.cpp does.
    ConfigTestAccess::data(*Config::get_instance()).erase("detection");
    SettingsManager::instance().deinit_subjects();
    SettingsManager::instance().init_subjects();

    PollHarness h(&state());
    set_print_state(state(), "printing");
    SettingsManager::instance().set_detection_enabled(false);
    h.poll();
    CHECK(h.fetch_calls == 0);
    CHECK(h.events.empty());
    SettingsManager::instance().set_detection_enabled(true);
    h.poll();
    CHECK(h.fetch_calls == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "K2StockSource thresholds and edge triggering",
                 "[detection][k2]") {
    PollHarness h(&state());
    set_print_state(state(), "printing");

    SECTION("below threshold: fetch runs, no event") {
        h.runner_stdout = "label: 1 prob: 0.700000 x:1 y:2 w:3 h:4\n";
        h.poll();
        CHECK(h.fetch_calls == 1);
        CHECK(h.events.empty());
    }
    SECTION("detection tool failure: no event, busy cleared (next poll runs)") {
        h.runner_exit = 1;
        h.poll();
        CHECK(h.fetch_calls == 1);
        CHECK(h.events.empty());
        h.runner_exit = 0;
        h.poll();
        CHECK(h.fetch_calls == 2); // proved not latched busy
    }
    SECTION("snapshot fetch failure: no event, busy cleared") {
        h.fetch_ok = false;
        h.poll();
        CHECK(h.fetch_calls == 1);
        CHECK(h.events.empty());
        h.fetch_ok = true;
        h.poll();
        CHECK(h.fetch_calls == 2);
    }
    SECTION("at threshold fires once; persists quietly; re-arms on negative") {
        h.runner_stdout = "label: 1 prob: 0.780000 x:1 y:2 w:3 h:4\n"; // pastaTruth 77.5
        h.poll();
        REQUIRE(h.events.size() == 1);
        const auto& e = h.events.front();
        CHECK(e.kind == DetectionKind::Spaghetti);
        CHECK(e.source_id == "k2_stock");
        CHECK(e.attributable);
        CHECK_FALSE(e.already_paused);
        REQUIRE(e.confidence.has_value());
        CHECK(*e.confidence == Catch::Approx(0.78f).epsilon(0.001));
        CHECK(e.message.find("78%") != std::string::npos);

        h.poll(); // still spaghetti: no second alert
        CHECK(h.events.size() == 1);

        h.runner_stdout.clear(); // spaghetti gone
        h.poll();
        CHECK(h.events.size() == 1);

        h.runner_stdout = "label: 1 prob: 0.900000 x:1 y:2 w:3 h:4\n"; // and back
        h.poll();
        CHECK(h.events.size() == 2);
    }
    SECTION("a new print re-arms even if the last job ended positive") {
        h.runner_stdout = "label: 1 prob: 0.900000 x:1 y:2 w:3 h:4\n";
        h.poll();
        REQUIRE(h.events.size() == 1);

        set_print_state(state(), "standby"); // job over, observer resets the edge
        set_print_state(state(), "printing");
        h.poll();
        CHECK(h.events.size() == 2);
    }
    SECTION("resume with the spaghetti still present fires again") {
        h.runner_stdout = "label: 1 prob: 0.900000 x:1 y:2 w:3 h:4\n";
        h.poll();
        REQUIRE(h.events.size() == 1);

        set_print_state(state(), "paused"); // the pause the detection's response produced
        h.poll();                           // paused polls; no re-fire while held
        CHECK(h.events.size() == 1);

        set_print_state(state(), "printing"); // resume resets the debounce
        h.poll();
        CHECK(h.events.size() == 2);
    }
    SECTION("a detection while the job is already paused says so (prestonbrown/helixscreen#1378)") {
        h.runner_stdout = "label: 1 prob: 0.900000 x:1 y:2 w:3 h:4\n";
        set_print_state(state(), "paused"); // the user paused; nothing has fired yet
        h.poll();
        REQUIRE(h.events.size() == 1);
        CHECK(h.events.front().already_paused);
    }
    SECTION("a round that outlives its job does not fire") {
        h.runner_stdout = "label: 1 prob: 0.900000 x:1 y:2 w:3 h:4\n";
        set_print_state(state(), "printing");
        h.src.poll_tick(); // round runs; its result is deferred, not applied

        set_print_state(state(), "standby"); // job ended before the result landed
        CHECK(h.events.empty());

        h.runner_stdout.clear();
        set_print_state(state(), "printing");
        h.poll(); // busy_ cleared by the skipped round: the next tick runs
        CHECK(h.fetch_calls == 2);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "K2StockSource reports the printer's ai_control preference",
                 "[detection][k2]") {
    // start() reads the ai_control file once, so each section writes the value
    // it exercises and builds the source against that file.
    static constexpr const char* AI_JSON = "/tmp/helix_k2_aicontrol_test.json";
    auto make_harness = [&](std::optional<int> ai_switch, std::optional<int> pause_print) {
        json ai{{"pastaTime", 25}, {"pastaTruth", 77}};
        if (ai_switch.has_value())
            ai["switch"] = *ai_switch;
        if (pause_print.has_value())
            ai["pausePrint"] = *pause_print;
        // Closed before the source is built: start() reads the file in the
        // PollHarness constructor, and an unflushed ofstream is an empty file.
        {
            std::ofstream f(AI_JSON);
            f << json{{"ai_control", ai}}.dump();
        }
        return std::make_unique<PollHarness>(&state(), AI_JSON);
    };

    SECTION("switch 0: preference says detection off") {
        auto h = make_harness(0, std::nullopt);
        const auto pref = h->src.printer_preference();
        REQUIRE(pref.has_value());
        CHECK_FALSE(pref->enabled);
    }
    SECTION("pausePrint 0: preference says do not pause") {
        auto h = make_harness(std::nullopt, 0);
        const auto pref = h->src.printer_preference();
        REQUIRE(pref.has_value());
        CHECK(pref->enabled);
        CHECK_FALSE(pref->pause);
    }
    SECTION("both present: both carried") {
        auto h = make_harness(1, 1);
        const auto pref = h->src.printer_preference();
        REQUIRE(pref.has_value());
        CHECK(pref->enabled);
        CHECK(pref->pause);
    }

    std::remove(AI_JSON);
}

TEST_CASE_METHOD(XMLTestFixture, "K2StockSource reports, never pauses the print itself",
                 "[detection][k2]") {
    static constexpr const char* AI_JSON = "/tmp/helix_k2_aicontrol_test.json";
    {
        std::ofstream f(AI_JSON);
        f << json{{"ai_control", {{"pastaTime", 25}, {"pastaTruth", 77}, {"pausePrint", 1}}}}
                 .dump();
    }
    PollHarness h(&state(), AI_JSON);
    h.runner_stdout = "label: 1 prob: 0.903177 x:1 y:2 w:3 h:4\n";
    set_print_state(state(), "printing");
    // The pause decision belongs to the settings and the presenter above the
    // source; pausePrint 1 here must not make the source send a pause.
    helix::TextLogCapture capture;
    h.poll();
    REQUIRE(h.events.size() == 1);
    CHECK_FALSE(h.events[0].already_paused);
    CHECK_FALSE(capture.contains("[Moonraker API] Pausing print"));
    // HelixScreen does the pausing for this source, so the pause-on-detect
    // setting governs it (drives the settings row's visibility subject).
    CHECK_FALSE(h.src.self_pauses());
    std::remove(AI_JSON);
}

TEST_CASE("K2StockSource refresh_capability re-probes without a restart", "[detection][k2]") {
    K2StockDetectionSource src(nullptr);
    // The env override is the probe's deterministic half on a dev box (which
    // is never a K2), so it pins the re-probe: the capability must follow a
    // value that changed after construction.
    unsetenv("HELIX_MOCK_DETECTION_CAPABLE");
    REQUIRE(setenv("HELIX_MOCK_DETECTION_CAPABLE", "1", 1) == 0);
    src.refresh_capability();
    CHECK(src.available());
    REQUIRE(setenv("HELIX_MOCK_DETECTION_CAPABLE", "0", 1) == 0);
    src.refresh_capability();
    CHECK_FALSE(src.available());
    unsetenv("HELIX_MOCK_DETECTION_CAPABLE");
}
