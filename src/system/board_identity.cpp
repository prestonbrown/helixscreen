// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_identity.h"

#include "helix_fs.h"
#include "platform_info.h"
#include "platform_table.h"

#include <array>
#include <sys/stat.h>

namespace helix::board {

namespace {

// Row order is enumerator order; info() relies on it and the static_assert below
// keeps the table the size of the enum.
//                        key             display_name                 legacy   cam    jz     fx dim
//                        fw-pkg   installer-root
constexpr std::array<Info, kBoardCount> kRows = {{
    {Board::Generic, "generic", "", nullptr, true, false, false, false, nullptr, nullptr},
    {Board::Ad5m, "ad5m", "FlashForge Adventurer 5M", nullptr, false, false, false, true, nullptr,
     nullptr},
    {Board::Ad5x, "ad5x", "FlashForge Adventurer 5X", "ad5x", false, true, false, true, nullptr,
     nullptr},
    {Board::Creator5, "creator5", "FlashForge Creator 5", nullptr, false, true, false, true,
     nullptr, nullptr},
    {Board::Creator5Pro, "creator5-pro", "FlashForge Creator 5 Pro", nullptr, false, true, false,
     true, nullptr, nullptr},
    {Board::K1, "k1", "Creality K1", "k1", true, false, false, false, nullptr, nullptr},
    {Board::K2, "k2", "Creality K2", nullptr, true, false, true, false, nullptr, nullptr},
    {Board::Cc1, "cc1", "Elegoo Centauri Carbon", nullptr, true, false, false, false, nullptr,
     nullptr},
    // PAXX Extended Firmware's extended-pkg unpacks HelixScreen into a versioned dir under
    // /oem/apps/helixscreen and points `latest` at it; its lmd hook bind-mounts that
    // binary over /usr/bin/gui. On stock firmware our installer owns /userdata/helixscreen.
    {Board::SnapmakerU1, "snapmaker-u1", "Snapmaker U1", nullptr, true, false, false, false,
     "/oem/apps/helixscreen", "/userdata/helixscreen"},
}};

constexpr bool rows_follow_enum_order() {
    for (int i = 0; i < kBoardCount; ++i) {
        if (static_cast<int>(kRows[static_cast<std::size_t>(i)].board) != i ||
            kRows[static_cast<std::size_t>(i)].key == nullptr)
            return false;
    }
    return true;
}
static_assert(rows_follow_enum_order(), "every Board needs a row, in enumerator order");

std::optional<Board> s_override;

} // namespace

Board resolve(const std::string& platform_key, const std::string& probe_root) {
    if (platform_key == "ad5m")
        return Board::Ad5m;
    if (platform_key == "ad5x")
        return Board::Ad5x;
    if (platform_key == "k1")
        return Board::K1;
    if (platform_key == "k2")
        return Board::K2;
    if (platform_key == "cc1")
        return Board::Cc1;
    if (platform_key == "snapmaker-u1")
        return Board::SnapmakerU1;
    if (platform_key != "mips")
        return Board::Generic;

    if (const auto machine = flashforge_machine_id(probe_root)) {
        switch (*machine) {
        case FlashForgeMachine::Ad5x:
            return Board::Ad5x;
        case FlashForgeMachine::Creator5:
            return Board::Creator5;
        case FlashForgeMachine::Creator5Pro:
            return Board::Creator5Pro;
        }
    }
    // The Forge-X chroot carries no machine id but is always an AD5X; a MIPS host with
    // no FlashForge tree at all is a K1.
    return flashforge_mod_tree(probe_root) != FlashForgeModTree::None ? Board::Ad5x : Board::K1;
}

Board current() {
    if (s_override)
        return *s_override;
    static const Board cached = resolve(platform::current_key(), "/");
    return cached;
}

// The parentheses stop the ESP32 spdlog shim's info() macro expanding this declarator.
const Info&(info)(Board board) {
    return kRows[static_cast<std::size_t>(board)];
}

namespace {

/// @p abs_path under @p probe_root ("/" or empty probes the real path).
std::string rooted(const std::string& probe_root, const char* abs_path) {
    std::string root = probe_root;
    while (!root.empty() && root.back() == '/')
        root.pop_back();
    return root + abs_path;
}

bool same_file(const struct ::stat& a, const std::string& path) {
    struct ::stat b {};
    return ::stat(path.c_str(), &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

} // namespace

bool firmware_manages_updates(Board board, const std::string& running_exe,
                              const std::string& probe_root) {
    const Info& row = info(board);
    if (row.firmware_package_dir == nullptr)
        return false;
    const std::string package_dir = rooted(probe_root, row.firmware_package_dir);
    if (!helix::fs::is_directory(package_dir))
        return false;

    struct ::stat exe_st {};
    if (::stat(running_exe.c_str(), &exe_st) == 0) {
        if (const auto entries = helix::fs::list_dir(package_dir)) {
            for (const auto& entry : *entries) {
                if (entry.is_dir && same_file(exe_st, entry.path + "/bin/helix-screen"))
                    return true;
            }
        }
    }

    if (row.installer_root == nullptr)
        return true;
    const auto exe = helix::fs::canonical(running_exe);
    const auto ours = helix::fs::canonical(rooted(probe_root, row.installer_root));
    return !(exe && ours && exe->compare(0, ours->size() + 1, *ours + "/") == 0);
}

void set_current_override(std::optional<Board> board) {
    s_override = board;
}

} // namespace helix::board
