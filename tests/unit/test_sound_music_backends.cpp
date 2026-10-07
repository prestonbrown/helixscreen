// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_sound_music_backends.cpp
 * @brief Music plays only on backends that can carry it
 */

#ifdef HELIX_HAS_TRACKER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/sound_manager_test_access.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "sound_backend.h"
#include "sound_manager.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* kSong = "assets/sounds/crocketts_theme.mod";

/// A silent stand-in with a chosen voice count: 1 is a buzzer (PWM, M300),
/// 4 is the AD5X's chord engine.
class SilentBackend : public SoundBackend {
  public:
    explicit SilentBackend(int voices) : voices_(voices) {}
    void set_tone(float, float, float) override {}
    void silence() override {}
    int voice_count() const override {
        return voices_;
    }

  private:
    int voices_;
};

class MusicBackendFixture : public LVGLTestFixture {
  public:
    MusicBackendFixture() {
        Config::get_instance();
        AudioSettingsManager::instance().init_subjects();
        AudioSettingsManager::instance().set_sounds_enabled(true);
        SoundManager::instance().shutdown();
    }
    ~MusicBackendFixture() override {
        SoundManager::instance().shutdown();
        AudioSettingsManager::instance().set_sounds_enabled(false);
        AudioSettingsManager::instance().deinit_subjects();
    }

    void install(int voices) {
        auto& sm = SoundManager::instance();
        SoundManagerTestAccess::install_backend(sm, std::make_shared<SilentBackend>(voices));
        SoundManagerTestAccess::finalize(sm);
    }
};

} // namespace

TEST_CASE_METHOD(MusicBackendFixture, "a single-voice buzzer refuses music", "[sound][tracker]") {
    install(1);
    auto& sm = SoundManager::instance();
    CHECK_FALSE(sm.can_play_music());
    sm.play_file(kSong);
    CHECK_FALSE(sm.is_tracker_playing());
}

TEST_CASE_METHOD(MusicBackendFixture, "a multi-voice backend plays music", "[sound][tracker]") {
    install(4);
    auto& sm = SoundManager::instance();
    CHECK(sm.can_play_music());
    sm.play_file(kSong);
    CHECK(sm.is_tracker_playing()); // the song loads: the buzzer case's refusal was the gate
}

#endif // HELIX_HAS_TRACKER
