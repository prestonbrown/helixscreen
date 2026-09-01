// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for helix::platform::ad5x_mod_layout_present — the AD5X mod-tree
// recognition predicate (ZMOD markers or a reachable Forge-X mod tree) that
// gates the log collector's mod_data paths and drives update_checker's mips
// K1/AD5X runtime split. Pinned to the SAME truth table as the launcher's
// heap-diag gate (tests/shell/test_helix_launcher_env.bats, "heap diag:"
// cases): a Forge-X chroot carries none of the ZMOD markers, so the reachable
// mod git tree (.shell/platform.sh) is the recognition evidence there.

#include "system/device_layout.h"
#include "test_helpers/ad5x_layout_fixture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

namespace {

struct TempDirGuard {
    fs::path path;
    TempDirGuard() {
        path = fs::temp_directory_path() / ("helix-layout-test-" + std::to_string(::getpid()) +
                                            "-" + std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("helix::platform::ad5x_mod_layout_present detects ZMOD by marker file",
          "[device_layout]") {
    TempDirGuard tmp;
    REQUIRE(helix::platform::ad5x_mod_layout_present(
        helix::test::make_ad5x_layout(tmp.path, "zmod-marker").string()));
}

TEST_CASE("helix::platform::ad5x_mod_layout_present detects ZMOD by FlashForge /usr/prog dir",
          "[device_layout]") {
    TempDirGuard tmp;
    REQUIRE(helix::platform::ad5x_mod_layout_present(
        helix::test::make_ad5x_layout(tmp.path, "zmod-prog").string()));
}

TEST_CASE("helix::platform::ad5x_mod_layout_present detects a Forge-X chroot via the mod tree",
          "[device_layout]") {
    TempDirGuard tmp;
    // The rig's chroot binds /usr/data at /opt, so the mod tree is reachable
    // under exactly one of the two spellings depending on which side of the
    // chroot the process runs. Both must count.
    REQUIRE(helix::platform::ad5x_mod_layout_present(
        helix::test::make_ad5x_layout(tmp.path, "forgex").string()));
    REQUIRE(helix::platform::ad5x_mod_layout_present(
        helix::test::make_ad5x_layout(tmp.path, "forgex-host").string()));
}

TEST_CASE("helix::platform::ad5x_mod_layout_present rejects a plain host", "[device_layout]") {
    TempDirGuard tmp;
    REQUIRE_FALSE(helix::platform::ad5x_mod_layout_present(
        helix::test::make_ad5x_layout(tmp.path, "plain").string()));
}

TEST_CASE("helix::platform::ad5x_mod_layout_present requires the marker kinds, not just names",
          "[device_layout]") {
    // /ZMOD as a DIRECTORY is not the Z-Mod marker file. The predicate checks
    // S_ISREG here and the launcher uses `-f` — the two must stay in step or
    // a stray directory arms AD5X handling on the wrong host.
    TempDirGuard tmp;
    auto root = tmp.path / "zmod-dir";
    fs::create_directories(root / "ZMOD");
    REQUIRE_FALSE(helix::platform::ad5x_mod_layout_present(root.string()));
}
