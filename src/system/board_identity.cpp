// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_identity.h"

#include "platform_info.h"
#include "platform_table.h"

#include <array>

namespace helix::board {

namespace {

// Row order is enumerator order; info() relies on it and the static_assert below
// keeps the table the size of the enum.
//                        key             display_name                 legacy   cam    jz     fx dim
//                        fw-upd
constexpr std::array<Info, kBoardCount> kRows = {{
    {Board::Generic, "generic", "", nullptr, true, false, false, false, false},
    {Board::Ad5m, "ad5m", "FlashForge Adventurer 5M", nullptr, false, false, false, true, false},
    {Board::Ad5x, "ad5x", "FlashForge Adventurer 5X", "ad5x", false, true, false, true, false},
    {Board::Creator5, "creator5", "FlashForge Creator 5", nullptr, false, true, false, true, false},
    {Board::Creator5Pro, "creator5-pro", "FlashForge Creator 5 Pro", nullptr, false, true, false,
     true, false},
    {Board::K1, "k1", "Creality K1", "k1", true, false, false, false, false},
    {Board::K2, "k2", "Creality K2", nullptr, true, false, true, false, false},
    {Board::Cc1, "cc1", "Elegoo Centauri Carbon", nullptr, true, false, false, false, false},
    {Board::SnapmakerU1, "snapmaker-u1", "Snapmaker U1", nullptr, true, false, false, false, true},
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

void set_current_override(std::optional<Board> board) {
    s_override = board;
}

} // namespace helix::board
