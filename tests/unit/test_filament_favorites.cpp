// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "filament_favorites.h"

#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

using helix::Config;

namespace {

/// Sandboxes HELIX_CONFIG_DIR into a temp directory and points the Config
/// singleton at a settings.json inside it, so toggle_favorite()'s save() has a
/// real file to write; a reload is simulated by re-running init() on the same
/// path. clear_path() in teardown drops the singleton's path before the
/// directory disappears (config.h documents exactly this fixture shape).
class FavoritesConfigFixture {
  protected:
    fs::path dir_;

    FavoritesConfigFixture() {
        dir_ = fs::temp_directory_path() / ("helix_favorites_test_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_env_ = prev;
            had_env_ = true;
        }
        setenv("HELIX_CONFIG_DIR", dir_.string().c_str(), 1);
        Config::get_instance()->init((dir_ / "settings.json").string());
    }

    ~FavoritesConfigFixture() {
        if (had_env_) {
            setenv("HELIX_CONFIG_DIR", saved_env_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        Config::get_instance()->clear_path();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

  private:
    std::string saved_env_;
    bool had_env_ = false;
};

} // namespace

TEST_CASE_METHOD(FavoritesConfigFixture, "toggle_favorite persists across a config reload",
                 "[favorites][filament_picker]") {
    REQUIRE_FALSE(helix::filament_favorites::is_favorite("generic-pla"));

    CHECK(helix::filament_favorites::toggle_favorite("generic-pla"));
    CHECK(helix::filament_favorites::is_favorite("generic-pla"));
    // In-config representation: the id list under /filament/favorite_ids.
    CHECK(Config::get_instance()->get_string_array(helix::filament_favorites::kFavoriteIdsPath) ==
          std::vector<std::string>{"generic-pla"});

    // Reload: favorites are read from Config on every query, so an app reboot
    // (init() re-reading settings.json) must still see the star.
    Config::get_instance()->init((dir_ / "settings.json").string());
    CHECK(helix::filament_favorites::is_favorite("generic-pla"));

    // Toggle off removes the id, durably.
    CHECK_FALSE(helix::filament_favorites::toggle_favorite("generic-pla"));
    CHECK_FALSE(helix::filament_favorites::is_favorite("generic-pla"));
    Config::get_instance()->init((dir_ / "settings.json").string());
    CHECK_FALSE(helix::filament_favorites::is_favorite("generic-pla"));
}

TEST_CASE_METHOD(FavoritesConfigFixture,
                 "favorite ids keep insertion order and star state is per id",
                 "[favorites][filament_picker]") {
    CHECK(helix::filament_favorites::toggle_favorite("generic-pla"));
    CHECK(helix::filament_favorites::toggle_favorite("sunlu-pla-plus-2-0"));
    CHECK(Config::get_instance()->get_string_array(helix::filament_favorites::kFavoriteIdsPath) ==
          std::vector<std::string>{"generic-pla", "sunlu-pla-plus-2-0"});

    // Removing one leaves the other untouched.
    CHECK_FALSE(helix::filament_favorites::toggle_favorite("generic-pla"));
    CHECK(helix::filament_favorites::is_favorite("sunlu-pla-plus-2-0"));
    CHECK(Config::get_instance()->get_string_array(helix::filament_favorites::kFavoriteIdsPath) ==
          std::vector<std::string>{"sunlu-pla-plus-2-0"});

    // An empty id is never a valid membership key.
    CHECK_FALSE(helix::filament_favorites::toggle_favorite(""));
    CHECK_FALSE(helix::filament_favorites::is_favorite(""));
}
