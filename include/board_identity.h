// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>

namespace helix::board {

/// Which printer board this process runs on, or Generic when it drives a printer
/// somewhere else. The unified `mips` build serves several boards, so the
/// platform key alone cannot answer it; resolve() adds the runtime probe.
enum class Board {
    Generic, ///< Drives a printer somewhere else: Pi, x86, desktop, Android, K-Touch
    Ad5m,    ///< Adventurer 5M and 5M Pro, one board
    Ad5x,
    Creator5,
    Creator5Pro,
    K1, ///< K1, K1C, K1 Max
    K2, ///< K2, K2 Pro, K2 Plus
    Cc1,
    SnapmakerU1, ///< Keep last: kBoardCount is derived from it.
};

inline constexpr int kBoardCount = static_cast<int>(Board::SnapmakerU1) + 1;

/// One row per board. Small per-board facts live here; capability modules that own
/// large per-board data (touch fits, panel geometry, cache paths) key their own
/// tables on Board.
struct Info {
    Board board;
    const char* key;                  ///< "creator5-pro": logs, bundles, crash reports
    const char* display_name;         ///< printer-database spelling; "" for Generic
    const char* legacy_release_asset; ///< pre-unified release asset for this board, or nullptr
    bool local_camera_probe_safe;     ///< discovery may probe loopback camera endpoints
    bool has_jz_pwm_piezo;            ///< a piezo driven through the Ingenic PWM device
    bool reduced_effects;             ///< decorative animation is too heavy for the SoC
    bool firmware_dims_backlight_after_boot; ///< stock firmware undoes our brightness
    /// The directory a firmware package manager installs HelixScreen into, one
    /// subdirectory per version, or nullptr when no firmware ships it.
    const char* firmware_package_dir;
    /// Where our own installer puts HelixScreen on this board, or nullptr.
    const char* installer_root;
};

/// The board a build with platform key @p platform_key is on. "ad5m", "ad5x", "k1", "k2",
/// "cc1" and "snapmaker-u1" map to their board with no filesystem access, and every other
/// key except "mips" is Generic. For "mips" the FlashForge machine id names Ad5x, Creator5
/// or Creator5Pro; otherwise a FlashForge mod tree without a machine id (the Forge-X
/// chroot) is an Ad5x; otherwise the board is a K1. Probes resolve under @p probe_root.
Board resolve(const std::string& platform_key, const std::string& probe_root = "/");

/// resolve(platform::current_key(), "/"), computed once. The cached value never changes
/// after the first call; the override below is the only thing that can shadow it.
Board current();

/// The row for @p board. Every Board has one.
const Info& info(Board board);

inline const Info& current_info() {
    return info(current());
}

/// True when @p board's firmware owns the HelixScreen install that is running, so the
/// in-app updater must leave it alone. Decided under @p probe_root, failing toward
/// "managed" because self-updating a firmware's package is the worse mistake:
///  - no firmware_package_dir, or that directory absent: false;
///  - @p running_exe is the same file (device and inode) as `<version dir>/bin/helix-screen`
///    in ANY subdirectory of it, `latest` included: true. Identity rather than a path,
///    because a firmware can launch the package through a bind mount over another path;
///  - otherwise true unless @p running_exe resolves inside installer_root, so an exe that
///    cannot be stat'd or resolved counts as the firmware's.
bool firmware_manages_updates(Board board, const std::string& running_exe = "/proc/self/exe",
                              const std::string& probe_root = "/");

/// The version directory of @p board's firmware package that @p running_exe runs from:
/// the subdirectory of firmware_package_dir whose `bin/helix-screen` is the same file
/// (device and inode) as @p running_exe, so a launch through a bind mount over another
/// path still names it. A link to it (`latest`) wins over the directory itself: the
/// firmware re-points the link at each package version it installs, so a path through
/// it outlives the version directory an upgrade deletes. nullopt
/// when the board has no firmware package or @p running_exe is not one of its binaries.
/// Paths probe under @p probe_root and come back with it prefixed.
std::optional<std::string> firmware_package_root(Board board,
                                                 const std::string& running_exe = "/proc/self/exe",
                                                 const std::string& probe_root = "/");

/// Test-only, main-thread-only: make current() answer @p board until reset with
/// std::nullopt. HelixTestFixture::reset_all() resets it.
void set_current_override(std::optional<Board> board);

} // namespace helix::board
