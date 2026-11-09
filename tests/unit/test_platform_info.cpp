// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for helix::flashforge_mod_tree — the FlashForge mod-tree
// recognition probe (ZMOD markers or a reachable Forge-X mod tree) that
// gates the log collector's mod_data paths and, with the machine id, tells the
// boards sharing the unified MIPS build apart. Pinned to the SAME truth table as the launcher's
// heap-diag gate (tests/shell/test_helix_launcher_env.bats, "heap diag:"
// cases): a Forge-X chroot carries none of the ZMOD markers, so the reachable
// mod git tree (.shell/platform.sh) is the recognition evidence there.

#include "platform_info.h"
#include "test_helpers/flashforge_layout_fixture.h"
#include "test_helpers/unique_temp_dir.h"

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
        path = helix::test::unique_temp_dir("helix-layout-test");
        fs::create_directories(path);
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("flashforge_mod_tree names each layout", "[platform_info]") {
    using helix::FlashForgeModTree;
    using helix::test::make_flashforge_layout;
    TempDirGuard tmp;
    // The rig's chroot binds /usr/data at /opt, so the Forge-X mod tree is reachable
    // under exactly one of the two spellings depending on which side of the chroot
    // the process runs. Both must count.
    CHECK(helix::flashforge_mod_tree(make_flashforge_layout(tmp.path, "zmod-marker").string()) ==
          FlashForgeModTree::Zmod);
    CHECK(helix::flashforge_mod_tree(make_flashforge_layout(tmp.path, "zmod-prog").string()) ==
          FlashForgeModTree::Zmod);
    CHECK(helix::flashforge_mod_tree(make_flashforge_layout(tmp.path, "forgex").string()) ==
          FlashForgeModTree::ForgeX);
    CHECK(helix::flashforge_mod_tree(make_flashforge_layout(tmp.path, "forgex-host").string()) ==
          FlashForgeModTree::ForgeX);
    CHECK(helix::flashforge_mod_tree(make_flashforge_layout(tmp.path, "plain").string()) ==
          FlashForgeModTree::None);

    CHECK(std::string(helix::flashforge_mod_tree_name(FlashForgeModTree::None)) == "none");
    CHECK(std::string(helix::flashforge_mod_tree_name(FlashForgeModTree::Zmod)) == "ZMOD");
    CHECK(std::string(helix::flashforge_mod_tree_name(FlashForgeModTree::ForgeX)) == "Forge-X");
}

TEST_CASE("flashforge_mod_tree reads Forge-X's own tree before /usr/prog", "[platform_info]") {
    // #2092: a Forge-X chroot can carry /usr/prog too (the AD5X rig's does), so the
    // generic FlashForge dir must not outrank the mod's own descriptor.
    TempDirGuard tmp;
    CHECK(helix::flashforge_mod_tree(
              helix::test::make_flashforge_layout(tmp.path, "forgex-prog").string()) ==
          helix::FlashForgeModTree::ForgeX);
    // The /ZMOD marker file is Z-Mod's own; with no Forge-X tree it still wins.
    auto zmod = helix::test::make_flashforge_layout(tmp.path, "zmod-marker");
    fs::create_directories(zmod / "usr/prog");
    CHECK(helix::flashforge_mod_tree(zmod.string()) == helix::FlashForgeModTree::Zmod);
}

TEST_CASE("flashforge_mod_tree requires the marker kinds, not just names", "[platform_info]") {
    // /ZMOD as a DIRECTORY is not the Z-Mod marker file. The probe checks
    // S_ISREG here and the launcher uses `-f` — the two must stay in step or
    // a stray directory arms FlashForge handling on the wrong host.
    TempDirGuard tmp;
    auto root = tmp.path / "zmod-dir";
    fs::create_directories(root / "ZMOD");
    REQUIRE(helix::flashforge_mod_tree(root.string()) == helix::FlashForgeModTree::None);
}

namespace {

/// Writes @p text to @p rel under @p root, creating parent directories.
void write_probe_file(const fs::path& root, const fs::path& rel, const std::string& text) {
    fs::create_directories(root / rel.parent_path());
    std::ofstream out(root / rel, std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("helix::flashforge_machine_id reads MACHINE= from app_startup.sh",
          "[platform_info][machine-id]") {
    TempDirGuard tmp;
    write_probe_file(tmp.path, "usr/prog/app_startup.sh",
                     "#!/bin/sh\nMACHINE=Creator5Pro\nVER=1.9.9\n");
    REQUIRE(helix::flashforge_machine_id(tmp.path.string()) ==
            helix::FlashForgeMachine::Creator5Pro);

    // Whole-value match: Creator5 is a prefix of Creator5Pro, so a prefix test
    // would fold the Pro into the plain Creator 5. The shell rule this mirrors
    // (ff_machine_id in scripts/lib/installer/platform.sh) pins the same.
    write_probe_file(tmp.path, "usr/prog/app_startup.sh", "MACHINE=Creator5\n");
    REQUIRE(helix::flashforge_machine_id(tmp.path.string()) == helix::FlashForgeMachine::Creator5);

    write_probe_file(tmp.path, "usr/prog/app_startup.sh", "MACHINE=AD5X\n");
    REQUIRE(helix::flashforge_machine_id(tmp.path.string()) == helix::FlashForgeMachine::Ad5x);
}

TEST_CASE("helix::flashforge_machine_id falls back to the os-release codename",
          "[platform_info][machine-id]") {
    TempDirGuard tmp;
    // The Z-Mod chroot's start.sh writes VERSION_CODENAME="<MACHINE> <version>"
    // on every boot; the first word is the board, the rest is firmware version.
    write_probe_file(tmp.path, "etc/os-release",
                     "ID=buildroot\nVERSION_CODENAME=\"Creator5Pro 1.9.9\"\n");
    REQUIRE(helix::flashforge_machine_id(tmp.path.string()) ==
            helix::FlashForgeMachine::Creator5Pro);

    // A distro codename left over (bookworm & co) fails the whole-value match.
    write_probe_file(tmp.path, "etc/os-release", "VERSION_CODENAME=\"bookworm\"\n");
    REQUIRE_FALSE(helix::flashforge_machine_id(tmp.path.string()).has_value());
}

TEST_CASE("helix::flashforge_machine_id prefers app_startup.sh and answers nothing on a K1",
          "[platform_info][machine-id]") {
    TempDirGuard tmp;
    write_probe_file(tmp.path, "usr/prog/app_startup.sh", "MACHINE=AD5X\n");
    write_probe_file(tmp.path, "etc/os-release", "VERSION_CODENAME=\"Creator5 1.0.0\"\n");
    REQUIRE(helix::flashforge_machine_id(tmp.path.string()) == helix::FlashForgeMachine::Ad5x);

    // A Creality K1 carries neither file in a shape the probe reads, so it
    // answers nothing and the caller keeps its own fallback.
    TempDirGuard k1;
    write_probe_file(k1.path, "etc/os-release", "ID=debian\n");
    REQUIRE_FALSE(helix::flashforge_machine_id(k1.path.string()).has_value());
}

TEST_CASE("helix::flashforge_machine_display_name matches the printer DB spellings",
          "[platform_info][machine-id]") {
    CHECK(std::string(helix::flashforge_machine_display_name(helix::FlashForgeMachine::Ad5x)) ==
          "FlashForge Adventurer 5X");
    CHECK(std::string(helix::flashforge_machine_display_name(helix::FlashForgeMachine::Creator5)) ==
          "FlashForge Creator 5");
    CHECK(std::string(helix::flashforge_machine_display_name(
              helix::FlashForgeMachine::Creator5Pro)) == "FlashForge Creator 5 Pro");
}
TEST_CASE("make_flashforge_layout writes the machine id", "[platform_info][machine-id]") {
    TempDirGuard tmp;
    CHECK(helix::flashforge_machine_id(
              helix::test::make_flashforge_layout(tmp.path, "zmod-prog", "Creator5Pro").string()) ==
          helix::FlashForgeMachine::Creator5Pro);
}
