// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for helix::board: the one answer to "which printer board is this process on".

#include "board_identity.h"
#include "helix_test_fixture.h"
#include "platform_info.h"
#include "platform_table.h"
#include "test_helpers/flashforge_layout_fixture.h"
#include "test_helpers/install_tree_fixture.h"
#include "test_helpers/scoped_board.h"
#include "test_helpers/unique_temp_dir.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using helix::board::Board;
using helix::board::info;
using helix::board::resolve;
using helix::test::make_flashforge_layout;

TEST_CASE("only FlashForge firmware re-asserts brightness after boot", "[board]") {
    for (Board b : {Board::Ad5m, Board::Ad5x, Board::Creator5, Board::Creator5Pro})
        CHECK(info(b).firmware_dims_backlight_after_boot);
    for (Board b : {Board::Cc1, Board::K1, Board::K2, Board::SnapmakerU1, Board::Generic})
        CHECK_FALSE(info(b).firmware_dims_backlight_after_boot);
}

TEST_CASE("no board defaults to external updates without its firmware's package running",
          "[board][update]") {
    // The test binary is nobody's firmware package, so even the U1 row self-updates.
    helix::set_external_updates_default_override(-1);
    for (int i = 0; i < helix::board::kBoardCount; ++i) {
        const auto b = static_cast<Board>(i);
        helix::test::ScopedBoard scoped(b);
        INFO("board: " << info(b).key);
        CHECK_FALSE(helix::platform_defaults_to_external_updates());
    }
}

TEST_CASE("the AD5M has no jz_pwm piezo", "[board]") {
    CHECK_FALSE(info(Board::Ad5m).has_jz_pwm_piezo);
}

namespace {

struct TempDirGuard {
    fs::path path;
    TempDirGuard() {
        path = helix::test::unique_temp_dir("helix-board-test");
        fs::create_directories(path);
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("board::resolve names every FlashForge MIPS board from its machine id", "[board]") {
    TempDirGuard tmp;
    CHECK(resolve("mips", make_flashforge_layout(tmp.path / "a", "zmod-prog", "AD5X").string()) ==
          Board::Ad5x);
    CHECK(
        resolve("mips", make_flashforge_layout(tmp.path / "b", "zmod-prog", "Creator5").string()) ==
        Board::Creator5);
    CHECK(resolve("mips",
                  make_flashforge_layout(tmp.path / "c", "zmod-prog", "Creator5Pro").string()) ==
          Board::Creator5Pro);
}

TEST_CASE("board::resolve calls a mod tree with no machine id an AD5X and nothing at all a K1",
          "[board]") {
    TempDirGuard tmp;
    CHECK(resolve("mips", make_flashforge_layout(tmp.path, "forgex").string()) == Board::Ad5x);
    CHECK(resolve("mips", make_flashforge_layout(tmp.path, "plain").string()) == Board::K1);
}

TEST_CASE("board::resolve never probes for a compile-time board", "[board]") {
    TempDirGuard tmp;
    const std::string c5 =
        make_flashforge_layout(tmp.path / "c5", "zmod-prog", "Creator5").string();
    const std::string ad5x = make_flashforge_layout(tmp.path / "x", "zmod-prog", "AD5X").string();
    const std::string plain = make_flashforge_layout(tmp.path, "plain").string();

    CHECK(resolve("k2", c5) == Board::K2);
    CHECK(resolve("pi", ad5x) == Board::Generic);
    CHECK(resolve("ad5m", plain) == Board::Ad5m);
    CHECK(resolve("esp32", ad5x) == Board::Generic);
    CHECK(resolve("k1", ad5x) == Board::K1);
    CHECK(resolve("cc1", ad5x) == Board::Cc1);
    CHECK(resolve("snapmaker-u1", ad5x) == Board::SnapmakerU1);
    CHECK(resolve("ad5x", plain) == Board::Ad5x);
}

TEST_CASE("every Board has a row and its key round-trips", "[board]") {
    const std::set<std::string> expected = {
        "generic", "ad5m", "ad5x", "creator5", "creator5-pro", "k1", "k2", "cc1", "snapmaker-u1"};
    std::set<std::string> seen;
    for (int i = 0; i < helix::board::kBoardCount; ++i) {
        const auto b = static_cast<Board>(i);
        INFO("board index " << i);
        CHECK(info(b).board == b);
        seen.insert(info(b).key);
    }
    CHECK(seen == expected);
}

TEST_CASE("board rows carry the spec's values", "[board]") {
    // Camera probe: false on every FlashForge board, whose kernel crashes when a camera
    // streamer is woken on v4l2_open.
    for (Board b : {Board::Ad5m, Board::Ad5x, Board::Creator5, Board::Creator5Pro})
        CHECK_FALSE(info(b).local_camera_probe_safe);
    for (Board b : {Board::Generic, Board::K1, Board::K2, Board::Cc1, Board::SnapmakerU1})
        CHECK(info(b).local_camera_probe_safe);

    // The jz_pwm piezo is on the AD5X family only.
    for (Board b : {Board::Ad5x, Board::Creator5, Board::Creator5Pro})
        CHECK(info(b).has_jz_pwm_piezo);
    for (Board b :
         {Board::Generic, Board::Ad5m, Board::K1, Board::K2, Board::Cc1, Board::SnapmakerU1})
        CHECK_FALSE(info(b).has_jz_pwm_piezo);

    CHECK(info(Board::K2).reduced_effects);
    for (Board b : {Board::Generic, Board::Ad5m, Board::Ad5x, Board::Creator5, Board::Creator5Pro,
                    Board::K1, Board::Cc1, Board::SnapmakerU1})
        CHECK_FALSE(info(b).reduced_effects);

    for (Board b : {Board::Ad5m, Board::Ad5x, Board::Creator5, Board::Creator5Pro})
        CHECK(info(b).firmware_dims_backlight_after_boot);
    for (Board b : {Board::Generic, Board::K1, Board::K2, Board::Cc1, Board::SnapmakerU1})
        CHECK_FALSE(info(b).firmware_dims_backlight_after_boot);

    CHECK(std::string(info(Board::SnapmakerU1).firmware_package_dir) == "/oem/apps/helixscreen");
    CHECK(std::string(info(Board::SnapmakerU1).installer_root) == "/userdata/helixscreen");
    for (Board b : {Board::Generic, Board::Ad5m, Board::Ad5x, Board::Creator5, Board::Creator5Pro,
                    Board::K1, Board::K2, Board::Cc1})
        CHECK(info(b).firmware_package_dir == nullptr);

    CHECK(std::string(info(Board::Ad5x).legacy_release_asset) == "ad5x");
    CHECK(std::string(info(Board::K1).legacy_release_asset) == "k1");
    for (Board b : {Board::Generic, Board::Ad5m, Board::Creator5, Board::Creator5Pro, Board::K2,
                    Board::Cc1, Board::SnapmakerU1})
        CHECK(info(b).legacy_release_asset == nullptr);

    CHECK(std::string(info(Board::Generic).display_name).empty());
    CHECK(std::string(info(Board::Creator5Pro).display_name) == "FlashForge Creator 5 Pro");
    CHECK(std::string(info(Board::Cc1).display_name) == "Elegoo Centauri Carbon");
}

TEST_CASE("the override wins and a fixture resets it", "[board]") {
    const Board real = resolve(helix::platform::current_key());
    {
        HelixTestFixture fixture;
        helix::board::set_current_override(Board::Creator5Pro);
        CHECK(helix::board::current() == Board::Creator5Pro);
        CHECK(std::string(helix::board::current_info().key) == "creator5-pro");
    }
    // The fixture's destructor ran reset_all(): the next test sees the build's real board.
    CHECK(helix::board::current() == real);

    helix::board::set_current_override(Board::K2);
    HelixTestFixture next;
    CHECK(helix::board::current() == real);
}

namespace {

fs::path touch_exe(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "elf";
    return p;
}

/// PAXX's extended-pkg layout: a versioned dir with `latest` pointing at it.
fs::path make_paxx_package(const fs::path& root) {
    const fs::path apps = root / "oem/apps/helixscreen";
    const fs::path binary = touch_exe(apps / "helixscreen-1.0.5/bin/helix-screen");
    fs::create_directory_symlink("helixscreen-1.0.5", apps / "latest");
    return binary;
}

} // namespace

TEST_CASE("a stock-firmware U1 install manages its own updates", "[board][update]") {
    using helix::board::firmware_manages_updates;
    TempDirGuard tmp;
    const fs::path ours = touch_exe(tmp.path / "userdata/helixscreen/bin/helix-screen");

    CHECK_FALSE(firmware_manages_updates(Board::SnapmakerU1, ours.string(), tmp.path.string()));

    // A PAXX package on the same box does not own an install of ours that runs beside it.
    make_paxx_package(tmp.path);
    CHECK_FALSE(firmware_manages_updates(Board::SnapmakerU1, ours.string(), tmp.path.string()));
}

TEST_CASE("PAXX's own HelixScreen package is firmware-managed", "[board][update]") {
    using helix::board::firmware_manages_updates;
    TempDirGuard tmp;
    const fs::path package = make_paxx_package(tmp.path);

    // Launched by its path, as `extended-pkg helixscreen exec` does.
    CHECK(firmware_manages_updates(Board::SnapmakerU1, package.string(), tmp.path.string()));
    CHECK(firmware_manages_updates(
        Board::SnapmakerU1, (tmp.path / "oem/apps/helixscreen/latest/bin/helix-screen").string(),
        tmp.path.string()));

    // Launched through the lmd hook's bind mount over /usr/bin/gui: same file, other path.
    // A hard link gives the test that same device-and-inode identity.
    const fs::path gui = tmp.path / "usr/bin/gui";
    fs::create_directories(gui.parent_path());
    fs::create_hard_link(package, gui);
    CHECK(firmware_manages_updates(Board::SnapmakerU1, gui.string(), tmp.path.string()));
    CHECK(firmware_manages_updates(Board::SnapmakerU1, gui.string(), tmp.path.string() + "/"));

    // Only the U1 row names a package; the same layout means nothing to another board.
    CHECK_FALSE(firmware_manages_updates(Board::K1, package.string(), tmp.path.string()));
    CHECK_FALSE(firmware_manages_updates(Board::Generic, package.string(), tmp.path.string()));
}

TEST_CASE("a PAXX version dir that latest does not name is still firmware-managed",
          "[board][update]") {
    using helix::board::firmware_manages_updates;
    TempDirGuard tmp;
    make_paxx_package(tmp.path);
    // An older version dir left beside the one `latest` names, launched by its own path.
    const fs::path older =
        touch_exe(tmp.path / "oem/apps/helixscreen/helixscreen-1.0.4/bin/helix-screen");
    CHECK(firmware_manages_updates(Board::SnapmakerU1, older.string(), tmp.path.string()));

    // The same file reached through another path, as the lmd hook's bind mount does.
    const fs::path gui = tmp.path / "usr/bin/gui";
    fs::create_directories(gui.parent_path());
    fs::create_hard_link(older, gui);
    CHECK(firmware_manages_updates(Board::SnapmakerU1, gui.string(), tmp.path.string()));
}

TEST_CASE("an unidentifiable executable beside a PAXX package is firmware-managed",
          "[board][update]") {
    using helix::board::firmware_manages_updates;
    TempDirGuard tmp;

    // No PAXX package: a stat failure cannot be PAXX's install, so it self-updates.
    CHECK_FALSE(firmware_manages_updates(Board::SnapmakerU1, (tmp.path / "missing").string(),
                                         tmp.path.string()));

    // With /oem/apps/helixscreen present, anything not provably ours is left to the firmware.
    fs::create_directories(tmp.path / "oem/apps/helixscreen");
    CHECK(firmware_manages_updates(Board::SnapmakerU1, (tmp.path / "missing").string(),
                                   tmp.path.string()));
    const fs::path elsewhere = touch_exe(tmp.path / "opt/helix/bin/helix-screen");
    CHECK(firmware_manages_updates(Board::SnapmakerU1, elsewhere.string(), tmp.path.string()));

    // Our own install root stays ours even then.
    const fs::path ours = touch_exe(tmp.path / "userdata/helixscreen/bin/helix-screen");
    CHECK_FALSE(firmware_manages_updates(Board::SnapmakerU1, ours.string(), tmp.path.string()));
}

TEST_CASE("firmware_package_root names the PAXX version dir its binary runs from",
          "[board][install_root]") {
    using helix::board::firmware_package_root;
    helix::test::InstallTreeFixture tree;
    const fs::path gui = tree.make_paxx_bind_mount("1.1.0");
    const std::string version_dir = tree.root + "/oem/apps/helixscreen/1.1.0";

    // Through the bind mount, and by the package paths: always the real version dir,
    // never `latest`, which the next package upgrade re-points.
    CHECK(firmware_package_root(Board::SnapmakerU1, gui.string(), tree.root) == version_dir);
    CHECK(firmware_package_root(Board::SnapmakerU1,
                                tree.root + "/oem/apps/helixscreen/latest/bin/helix-screen",
                                tree.root) == version_dir);

    // Our own install beside it is not the package, and no other board has one.
    const fs::path ours = tree.make_tree("userdata/helixscreen");
    CHECK_FALSE(firmware_package_root(Board::SnapmakerU1, ours.string(), tree.root));
    CHECK_FALSE(firmware_package_root(Board::SnapmakerU1, tree.root + "/missing", tree.root));
    CHECK_FALSE(firmware_package_root(Board::K1, gui.string(), tree.root));
}
