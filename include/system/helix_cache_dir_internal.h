// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file helix_cache_dir_internal.h
 * @brief Internals of the cache cascade, exposed so the sweep is testable.
 *
 * Which board rung exists is a question of the board this process is on, so a
 * host build can only ever exercise the desktop case through the public entry
 * point. Exposing the decision as pure functions over a candidate list lets a
 * test build the embedded shapes directly instead of mirroring the logic.
 */

#include "board_identity.h"
#include "platform_info.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix::cache_internal {

/// One rung of the cache cascade.
struct CacheCandidate {
    std::string path;
    /// Labels the rung in the log; nullptr means resolve quietly. Non-null also
    /// marks the rung deliberate - chosen on purpose, never reclaimed.
    const char* tier = nullptr;
    /// True for the board's own rung. Its presence marks an embedded board; it
    /// need not win, since every device hook exports HELIX_CACHE_DIR.
    bool platform = false;
};

/// Env override, config setting, or board path. Never reclaimed.
inline bool is_deliberate(const CacheCandidate& c) {
    return c.tier != nullptr;
}

/**
 * @brief The board's own state roots, in priority order: the directories beside
 * the payload where cache/ and logs/ live (platforms.json storage.state_root).
 *
 * Empty for Board::Generic. @p tree matters only to the FlashForge MIPS boards,
 * whose mod (Z-Mod or Forge-X) decides which durable directory holds state.
 */
std::vector<std::string> board_state_roots(board::Board board, FlashForgeModTree tree);

/**
 * @brief Where app_get_state_root() looks, in priority order: the board's state
 * roots, then @p state_dir (the systemd StateDirectory), then @p home_state_dir.
 * Pure: enumerating touches nothing on disk.
 */
std::vector<std::string> state_root_candidates(board::Board board, FlashForgeModTree tree,
                                               const std::string& state_dir,
                                               const std::string& home_state_dir);

/**
 * @brief The board's own cache rungs for one subdir, in priority order.
 *
 * Empty for Board::Generic. Each rung is labelled with the board key and marked as
 * the platform rung. @p tree matters only to the FlashForge MIPS boards, whose mod
 * (Z-Mod or Forge-X) decides which durable directory holds state.
 */
std::vector<CacheCandidate> board_cache_candidates(board::Board board, FlashForgeModTree tree,
                                                   const std::string& subdir);

/**
 * @brief Paths safe to reclaim, given one subdir's candidate list.
 *
 * Empty unless the list contains a platform rung. Returns the non-deliberate
 * rungs below the first viable one: those above were rejected as unusable,
 * those below were never probed.
 *
 * @param candidates The cascade for one subdir, in priority order.
 * @param viable     Predicate deciding whether a candidate could be used.
 */
std::vector<std::string> select_stale_paths(const std::vector<CacheCandidate>& candidates,
                                            const std::function<bool(const std::string&)>& viable);

/**
 * @brief Rename a state root whose name changed, keeping its contents.
 *
 * Moves @p legacy_root to @p current_root when only the old one exists. When
 * both exist (a half-finished migration), carries the known state subtrees
 * ("cache", "logs") across without clobbering, then drops the legacy side only
 * where it is empty — a platform hook still on the old layout recreates the
 * legacy "logs" dir at every pre-start, so an empty legacy tree must not
 * outlive the boot that renamed it. Never deletes a non-empty legacy subtree.
 *
 * Pure filesystem logic over the two paths: which roots a platform uses, and
 * any environment repair that has to go with the rename, live with the caller.
 *
 * @return Number of actions taken (renames, subtree moves, removals).
 */
int migrate_state_root(const std::string& legacy_root, const std::string& current_root);

/**
 * @brief The state root a board's identity has to leave, and where it goes.
 *
 * A Forge-X chroot can also carry /usr/prog, which reads as Z-Mod to a build
 * that does not probe Forge-X first, so its identity can sit under the Z-Mod
 * root while its state root is the mod's config dir (#2092). Returns
 * {Z-Mod root, Forge-X root} for a Forge-X tree on a board where the two
 * differ, nullopt everywhere else.
 */
std::optional<std::pair<std::string, std::string>> state_identity_migration(board::Board board,
                                                                            FlashForgeModTree tree);

/**
 * @brief Move the telemetry identity from @p legacy_root into @p current_root.
 *
 * Only the identity moves; the cache rebuilds wherever the root is. Creates
 * @p current_root when the legacy side holds an identity, and follows
 * telemetry::move_device_identity's rules: synced before the old copy goes,
 * never replacing an identity already there.
 *
 * @return true when the identity now lives only in @p current_root.
 */
bool migrate_state_identity(const std::string& legacy_root, const std::string& current_root);

} // namespace helix::cache_internal
