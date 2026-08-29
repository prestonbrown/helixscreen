// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_shaper_save_config.cpp
 * @brief End-to-end coverage for InputShaperPanel::save_configuration()
 *
 * This is the one path in the panel that rewrites the user's printer config, so
 * "it looked right on screen" is not enough - what matters is the bytes that
 * land back in conf.d/options.cfg. These cases drive the real
 * KlipperConfigEditor::safe_multi_edit() chain against the in-memory config root
 * the Moonraker mock serves, and assert on the uploaded file.
 *
 * Two things make that possible without a printer:
 *
 *  - MoonrakerClientTestAccess::force_connection_state(CONNECTED). The health
 *    monitor safe_multi_edit() runs after FIRMWARE_RESTART polls is_connected()
 *    every 500ms; a mock that never reports a disconnect makes Phase 1 run to
 *    the restart timeout and then take the "fast restart" success path.
 *  - A short restart timeout, injected through InputShaperPanelTestAccess. The
 *    panel's own 30s default would make every case a 30s wait.
 *
 * The health monitor runs on HttpExecutor::fast() and captures the panel's
 * config_editor_ and this fixture's MoonrakerAPIMock by reference, so no case
 * may return while it is still in flight. settle() is the join, and the fixture
 * destructor repeats it unconditionally.
 *
 * That poll costs each async case about 1.5s of real time. Deliberately NOT
 * tagged [slow]: `make test-run` filters [slow] out, and a path that rewrites
 * the user's printer config should not be absent from the default run to save
 * a second and a half.
 */

#include "ui_panel_input_shaper.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/input_shaper_panel_test_access.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../ui_test_utils.h"
#include "http_executor.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

constexpr const char* kRootPath = "printer.cfg";
constexpr const char* kOptionsPath = "conf.d/options.cfg";

constexpr const char* kRootCfg = "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n";

constexpr const char* kOptionsCfg = "[input_shaper]\nshaper_freq_x: 47.4\nshaper_type_x: mzv\n"
                                    "shaper_freq_y: 35.0\nshaper_type_y: mzv\n";

/// conf.d/options.cfg for a printer that has never been shaper-calibrated.
constexpr const char* kOptionsCfgNoShaper = "[stepper_x]\nstep_pin: PA1\n"
                                            "[stepper_y]\nstep_pin: PA2\n";

/// The Voron-shaped X result: firmware recommends 2hump_ei, but mzv is one of
/// the fitted options and is what the user's chip pick refers to.
InputShaperResult make_x_result() {
    InputShaperResult r;
    r.axis = 'X';
    r.shaper_type = "2hump_ei";
    r.shaper_freq = 52.8f;
    r.vibrations = 0.4f;
    r.max_accel = 4100.0f;
    r.all_shapers = {
        {"mzv", 41.6f, 1.8f, 0.09f, 6800.0f},
        {"ei", 47.2f, 1.1f, 0.12f, 5200.0f},
        {"2hump_ei", 52.8f, 0.4f, 0.16f, 4100.0f},
    };
    return r;
}

InputShaperResult make_y_result() {
    InputShaperResult r;
    r.axis = 'Y';
    r.shaper_type = "3hump_ei";
    r.shaper_freq = 44.9f;
    r.vibrations = 0.3f;
    r.max_accel = 3300.0f;
    r.all_shapers = {
        {"mzv", 32.6f, 2.4f, 0.11f, 5100.0f},
        {"ei", 38.0f, 1.5f, 0.14f, 4400.0f},
        {"3hump_ei", 44.9f, 0.3f, 0.19f, 3300.0f},
    };
    return r;
}

/// CSV-side curves. Deliberately NOT in the same order as all_shapers, and
/// deliberately carrying the rounded CSV frequency - resolve_selected_shaper()
/// joins by name and prefers the console's number, and a test that seeded them
/// identically could not tell the difference.
std::vector<ShaperResponseCurve> make_curves() {
    return {
        {"EI", 47.0f, {}},
        {"MZV", 42.0f, {}},
    };
}

/// Index of the mzv chip within make_curves().
constexpr int kMzvChip = 1;

bool has_substr(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Collects the toasts ToastManager raises while it is alive. The real manager
/// is excluded from the test build, so the stub's hook is the only way to see
/// whether the user was actually told anything.
class ToastRecorder {
  public:
    ToastRecorder() {
        set_test_toast_hook(
            [this](ToastSeverity, const std::string& msg) { messages_.push_back(msg); });
    }
    ~ToastRecorder() {
        set_test_toast_hook(nullptr);
    }
    ToastRecorder(const ToastRecorder&) = delete;
    ToastRecorder& operator=(const ToastRecorder&) = delete;

    [[nodiscard]] bool saw(const std::string& needle) const {
        for (const auto& m : messages_) {
            if (has_substr(m, needle))
                return true;
        }
        return false;
    }

    [[nodiscard]] const std::vector<std::string>& messages() const {
        return messages_;
    }

  private:
    std::vector<std::string> messages_;
};

class SaveConfigFixture : public LVGLTestFixture {
  public:
    SaveConfigFixture() : api_(client_, state_), panel_(get_global_input_shaper_panel()) {
        // The health monitor treats "still connected" as a fast restart, which
        // is the success path. Without this the mock reads DISCONNECTED, Phase 2
        // waits for a reconnect that never comes, and every case would exercise
        // the revert branch instead.
        MoonrakerClientTestAccess::force_connection_state(client_, ConnectionState::CONNECTED);

        seed_config({{kRootPath, kRootCfg}, {kOptionsPath, kOptionsCfg}});

        panel_.init_subjects();
        panel_.set_api(&client_, &api_);
        ::InputShaperPanelTestAccess::set_save_restart_timeout_ms(panel_, kRestartTimeoutMs);
        ::InputShaperPanelTestAccess::clear_results(panel_);
        UpdateQueue::instance().drain();
    }

    ~SaveConfigFixture() override {
        // Never leave a worker holding &api_ / &panel_.config_editor_ - the
        // fixture members below are about to be destroyed. A case that already
        // called settle() finds this a no-op.
        join_worker(kJoinTimeoutMs);
        UpdateQueue::instance().drain();

        panel_.on_deactivate();
        panel_.set_api(nullptr, nullptr);
        ::InputShaperPanelTestAccess::clear_results(panel_);
        UpdateQueue::instance().drain();
    }

    void seed_config(std::map<std::string, std::string> files) {
        api_.set_config_files(std::move(files));
    }

    /// Wait for the whole async chain to finish and apply everything it queued
    /// back onto the main thread. Returns false if the worker outlived the
    /// budget, which every case asserts on rather than reading a half-written
    /// config.
    bool settle() {
        bool joined = join_worker(kJoinTimeoutMs);
        UpdateQueue::instance().drain();
        return joined;
    }

    std::string uploaded(const char* path) const {
        auto content = api_.get_uploaded_config(path);
        return content.value_or("<absent>");
    }

    MoonrakerClientMock client_;
    PrinterState state_;
    MoonrakerAPIMock api_;
    InputShaperPanel& panel_;

  private:
    /// Phase 1 of the health monitor polls for kRestartTimeoutMs before it
    /// concludes "fast restart", so the join budget has to clear that with room
    /// for a loaded CI box.
    static constexpr uint32_t kRestartTimeoutMs = 1200;
    static constexpr uint32_t kJoinTimeoutMs = 20000;

    bool join_worker(uint32_t timeout_ms) {
        return wait_until([] { return helix::http::HttpExecutor::fast().inflight() == 0; },
                          timeout_ms);
    }
};

} // namespace

// ============================================================================
// The chip the user picked is what gets written
// ============================================================================

TEST_CASE_METHOD(SaveConfigFixture,
                 "save_configuration writes the selected shaper, not the "
                 "firmware recommendation",
                 "[input_shaper][save_config]") {
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', make_x_result(), make_curves(), kMzvChip);
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', make_y_result(), make_curves(), kMzvChip);

    ::InputShaperPanelTestAccess::save(panel_);
    REQUIRE(settle());

    const std::string written = uploaded(kOptionsPath);

    CHECK(has_substr(written, "shaper_type_x: mzv"));
    CHECK(has_substr(written, "shaper_freq_x: 41.6"));
    CHECK(has_substr(written, "shaper_type_y: mzv"));
    CHECK(has_substr(written, "shaper_freq_y: 32.6"));

    // The firmware's own pick must not survive anywhere in the file - not as a
    // type and not as its frequency.
    CHECK_FALSE(has_substr(written, "2hump_ei"));
    CHECK_FALSE(has_substr(written, "3hump_ei"));
    CHECK_FALSE(has_substr(written, "52.8"));
    CHECK_FALSE(has_substr(written, "44.9"));
}

// ============================================================================
// No chip selected falls back to the recommendation
// ============================================================================

TEST_CASE_METHOD(SaveConfigFixture,
                 "save_configuration falls back to the recommendation when no chip is selected",
                 "[input_shaper][save_config]") {
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', make_x_result(), make_curves(), -1);
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', make_y_result(), make_curves(), -1);

    ::InputShaperPanelTestAccess::save(panel_);
    REQUIRE(settle());

    const std::string written = uploaded(kOptionsPath);

    CHECK(has_substr(written, "shaper_type_x: 2hump_ei"));
    CHECK(has_substr(written, "shaper_freq_x: 52.8"));
    CHECK(has_substr(written, "shaper_type_y: 3hump_ei"));
    CHECK(has_substr(written, "shaper_freq_y: 44.9"));

    // The old values are replaced in place, not left behind alongside the new.
    CHECK_FALSE(has_substr(written, "shaper_freq_x: 47.4"));
    CHECK_FALSE(has_substr(written, "shaper_freq_y: 35.0"));
}

// ============================================================================
// Nothing valid writes nothing at all
// ============================================================================

TEST_CASE_METHOD(SaveConfigFixture, "save_configuration with no valid result uploads nothing",
                 "[input_shaper][save_config]") {
    SECTION("both axes empty") {
        // clear_results() in the fixture already left both axes default-constructed.
    }

    SECTION("a result with a type but no frequency") {
        InputShaperResult bad;
        bad.shaper_type = "mzv";
        bad.shaper_freq = 0.0f;
        ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', bad, {}, -1);
        ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', bad, {}, -1);
    }

    SECTION("a result with a frequency but no type") {
        InputShaperResult bad;
        bad.shaper_freq = 41.6f;
        ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', bad, {}, -1);
        ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', bad, {}, -1);
    }

    REQUIRE_NOTHROW(::InputShaperPanelTestAccess::save(panel_));
    REQUIRE(settle());

    // Byte-identical to what was seeded: no upload, no backup rewrite, no
    // half-written axis.
    CHECK(uploaded(kOptionsPath) == std::string(kOptionsCfg));
    CHECK(uploaded(kRootPath) == std::string(kRootCfg));
}

// ============================================================================
// A printer that has never been calibrated has no [input_shaper] to edit
// ============================================================================

TEST_CASE_METHOD(SaveConfigFixture, "save_configuration creates [input_shaper] when it is absent",
                 "[input_shaper][save_config]") {
    seed_config({{kRootPath, kRootCfg}, {kOptionsPath, kOptionsCfgNoShaper}});

    ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', make_x_result(), make_curves(), kMzvChip);
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', make_y_result(), make_curves(), kMzvChip);

    ::InputShaperPanelTestAccess::save(panel_);
    REQUIRE(settle());

    // Whichever file the editor decided owns the new section, exactly one of
    // them must now carry a complete [input_shaper].
    const std::string root = uploaded(kRootPath);
    const std::string options = uploaded(kOptionsPath);
    const std::string written = has_substr(options, "[input_shaper]") ? options : root;

    INFO("printer.cfg:\n" << root << "\nconf.d/options.cfg:\n" << options);

    CHECK(has_substr(written, "[input_shaper]"));
    CHECK(has_substr(written, "shaper_type_x: mzv"));
    CHECK(has_substr(written, "shaper_freq_x: 41.6"));
    CHECK(has_substr(written, "shaper_type_y: mzv"));
    CHECK(has_substr(written, "shaper_freq_y: 32.6"));

    // Creating the section must not eat what was already in the file.
    CHECK(has_substr(options, "[stepper_x]"));
    CHECK(has_substr(options, "step_pin: PA2"));
}

// ============================================================================
// Saving is a config rewrite, never SAVE_CONFIG
// ============================================================================

TEST_CASE_METHOD(SaveConfigFixture, "save_configuration never issues SAVE_CONFIG",
                 "[input_shaper][save_config]") {
    client_.clear_gcode_script_history();

    ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', make_x_result(), make_curves(), kMzvChip);
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', make_y_result(), make_curves(), kMzvChip);

    ::InputShaperPanelTestAccess::save(panel_);
    REQUIRE(settle());

    // SAVE_CONFIG would persist whatever SHAPER_CALIBRATE staged - the
    // firmware's recommendation - and silently overwrite the chip the user
    // picked, which is the whole reason this path rewrites the file itself.
    for (const auto& script : client_.gcode_script_history()) {
        INFO("gcode issued: " << script);
        CHECK_FALSE(has_substr(script, "SAVE_CONFIG"));
    }

    // Guard against the assertion above passing because nothing was recorded at
    // all: the config really was rewritten on this run.
    CHECK(has_substr(uploaded(kOptionsPath), "shaper_type_x: mzv"));
}

// ============================================================================
// The panel is already gone when the verdict lands
//
// handle_save_clicked() calls save_configuration() and then immediately
// clear_results() / set_state(IDLE) / go_back(), so every safe_multi_edit()
// callback fires against an expired panel token. Routing the toast through that
// token dropped the whole callback - including the one message that tells the
// user Klipper failed to restart and their config was rolled back.
// ============================================================================

TEST_CASE_METHOD(SaveConfigFixture, "the auto-revert warning survives the panel closing",
                 "[input_shaper][save_config]") {
    // Never reconnecting after FIRMWARE_RESTART is what drives safe_multi_edit()
    // into its revert branch, which is the only path that produces this warning.
    MoonrakerClientTestAccess::force_connection_state(client_, ConnectionState::DISCONNECTED);

    ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', make_x_result(), make_curves(), kMzvChip);
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', make_y_result(), make_curves(), kMzvChip);

    ToastRecorder toasts;

    ::InputShaperPanelTestAccess::save(panel_);

    // Exactly what handle_save_clicked() does the instant save_configuration()
    // returns: OverlayBase::on_deactivate() invalidates lifetime_, expiring
    // every token the async chain is holding.
    panel_.on_deactivate();

    REQUIRE(settle());

    INFO("toasts seen: " << toasts.messages().size());
    CHECK(toasts.saw("Failed to save configuration"));
}

TEST_CASE_METHOD(SaveConfigFixture, "the success toast survives the panel closing",
                 "[input_shaper][save_config]") {
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'X', make_x_result(), make_curves(), kMzvChip);
    ::InputShaperPanelTestAccess::seed_axis(panel_, 'Y', make_y_result(), make_curves(), kMzvChip);

    ToastRecorder toasts;

    ::InputShaperPanelTestAccess::save(panel_);
    panel_.on_deactivate();

    REQUIRE(settle());

    CHECK(toasts.saw("Input shaper settings applied!"));

    // The write itself still happened - the toast is not standing in for it.
    CHECK(has_substr(uploaded(kOptionsPath), "shaper_type_x: mzv"));
}
