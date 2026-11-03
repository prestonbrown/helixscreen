// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The M300 gate is fed by the print job state, and a preset's sound default reaches
// the live audio settings without a restart.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "sound_manager.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using nlohmann::json;

namespace {

class PrintGateFixture : public LVGLTestFixture {
  public:
    PrintGateFixture() {
        PrinterStateTestAccess::reset(state_);
        state_.init_subjects(false);
        SoundManager::instance().set_printer_busy(false);
    }
    ~PrintGateFixture() override {
        SoundManager::instance().set_printer_busy(false);
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    void print_stats(const char* state) {
        state_.update_from_status(json{{"print_stats", {{"state", state}}}});
    }

  private:
    PrinterState state_;
};

} // namespace

TEST_CASE_METHOD(PrintGateFixture, "M300 is held only while a print job is running",
                 "[sound][m300]") {
    auto& sound = SoundManager::instance();
    print_stats("printing");
    CHECK(sound.printer_busy());
    print_stats("paused");
    CHECK_FALSE(sound.printer_busy());
    print_stats("printing");
    CHECK(sound.printer_busy());
    print_stats("complete");
    CHECK_FALSE(sound.printer_busy());
    print_stats("printing");
    print_stats("standby");
    CHECK_FALSE(sound.printer_busy());
}

TEST_CASE_METHOD(LVGLTestFixture, "A preset's sound default reaches the live setting",
                 "[sound][preset]") {
    auto& audio = AudioSettingsManager::instance();
    audio.init_subjects();
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);
    audio.set_sounds_enabled(true);
    REQUIRE(audio.get_sounds_enabled());

    config->set<bool>("/sounds_enabled", false);
    reload_settings_after_preset();
    CHECK_FALSE(audio.get_sounds_enabled());

    audio.set_sounds_enabled(true);
}
