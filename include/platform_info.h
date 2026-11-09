// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>

namespace helix {

/// Returns true when running on Android (compile-time on real builds, overridable for tests)
bool is_android_platform();

/// True when this platform offers the host power controls (shutdown/reboot
/// dialog, screen power actions). Android is an app on someone's tablet: there
/// is no init system to reboot and the host-power RPCs are not wanted there.
/// The single source of this rule — the platform_host_power_supported subject
/// is seeded from it and the shutdown widget/dialog gate on that.
bool platform_host_power_supported();

/// True when this platform's software is owned by something other than HelixScreen
/// by DEFAULT, so the in-app updater must stay quiet unless explicitly turned on.
///
/// The Snapmaker U1 is the case: the PAXX Extended Firmware ships HelixScreen as a
/// selectable component, downloading a pinned, sha256-verified tarball into
/// /oem/apps/helixscreen via extended-pkg. Self-updating there rewrites a package the
/// firmware believes it owns. The firmware hook does not export
/// HELIX_DISABLE_AUTO_UPDATES, so without this default every U1 install would check
/// for updates and raise the update modal.
///
/// The single source of this rule; updates_externally_managed() consults it for the
/// default and an explicit HELIX_DISABLE_AUTO_UPDATES (either direction) still wins,
/// which is how a dev box force-enables self-update.
bool platform_defaults_to_external_updates();

/// True when HelixScreen runs ON the printer it drives, rather than beside it.
///
/// Every printer-embedded package is cross-built with its own HELIX_PLATFORM_* define,
/// and on all of them Moonraker is reachable at 127.0.0.1 because it is the same box.
/// A desktop or Pi build has no such guarantee: the printer is somewhere else on the
/// network and the user genuinely has to say where.
///
/// The single source of this rule. Consumers ask the capability question rather than
/// testing platform macros of their own, so adding a printer means editing this one
/// function. Currently used by the first-run wizard to decide whether asking for a
/// Moonraker address is a real question or a wasted step.
bool is_printer_embedded();

/// Which FlashForge mod's filesystem tree the environment carries. A tree
/// fact, not a board fact: an AD5X and a Creator 5 both carry FlashForge's
/// /usr/prog.
enum class FlashForgeModTree {
    None,   ///< no FlashForge mod tree (Pi, dev box, K1)
    Zmod,   ///< the /ZMOD marker file or FlashForge's /usr/prog dir
    ForgeX, ///< a Forge-X chroot, which has neither but keeps its mod git tree reachable
};

/// Probes the mod tree: ZMOD (the /ZMOD marker file or FlashForge's /usr/prog dir) or
/// Forge-X, whose chroot has neither but whose mod git tree stays reachable
/// (/opt/config/mod in-chroot, /usr/data/config/mod host-side, proven by
/// .shell/platform.sh). Same rule as the launcher's heap-diag gate
/// (scripts/helix-launcher.sh); the two test suites pin both to one truth
/// table. All probes resolve under `probe_root` (default "/") so tests can feed
/// a fake layout without touching the real root filesystem. stat-only; never
/// opens anything. One platform key can serve a K1 and both FlashForge firmware
/// populations, so the tree is what tells three filesystem layouts apart in a
/// diagnostic.
FlashForgeModTree flashforge_mod_tree(const std::string& probe_root = "/");

/// "none", "ZMOD" or "Forge-X".
const char* flashforge_mod_tree_name(FlashForgeModTree tree);

/// The three FlashForge boards the unified MIPS payload ships on. Answered by
/// flashforge_machine_id() below.
enum class FlashForgeMachine {
    Ad5x,
    Creator5,
    Creator5Pro,
};

/// The board a FlashForge MIPS firmware names for itself, from the one of two
/// places it is written that this process can reach: the `MACHINE=` line of the
/// stock /usr/prog/app_startup.sh on the host, or — inside the Z-Mod chroot,
/// whose prepare.sh bind-mounts only /usr/prog/config — the first word of
/// `VERSION_CODENAME` in the chroot's os-release (start.sh writes
/// `VERSION_CODENAME="<MACHINE> <version>"` on every boot). Whole-value match:
/// Creator5 is a prefix of Creator5Pro. The same rule the installer's
/// ff_machine_id() applies (scripts/lib/installer/platform.sh) — keep the two
/// and their tests in step.
///
/// nullopt when neither source carries a recognized value (a Creality K1
/// carries neither shape), so the caller keeps its own fallback. All probes
/// resolve under `probe_root` (default "/") for tests.
std::optional<FlashForgeMachine> flashforge_machine_id(const std::string& probe_root = "/");

/// The printer-database spelling of a board ("FlashForge Creator 5 Pro"), the
/// same names ad5x_board_name() prints in the installer.
const char* flashforge_machine_display_name(FlashForgeMachine machine);

/// Test helper: override the platform check. Pass -1 to reset to compile-time default.
void set_platform_override(int override_value);

/// Test helper: override the printer-embedded answer. Pass -1 to reset to the
/// compile-time platform answer.
void set_printer_embedded_override(int override_value);

/// Test helper: override the external-updates default. Pass -1 to reset to the
/// compile-time platform answer.
void set_external_updates_default_override(int override_value);

/// Short human-readable host arch line for the About screen and debug bundles.
/// Format: "<kernel-arch> · <N>-bit userspace" — surfaces the common
/// "aarch64 kernel + 32-bit userspace" Pi configuration without coercing the
/// user to migrate.
std::string host_arch_string();

} // namespace helix
