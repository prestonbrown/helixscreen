// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file helix_cache_dir.cpp
 * @brief Cache-directory resolution cascade.
 *
 * This translation unit has no global subjects and no app state - only Config,
 * helix::paths and spdlog. Tests isolate themselves through HELIX_CACHE_DIR,
 * rung 1 of the cascade. Declarations live in app_globals.h.
 */

#include "app_constants.h"
#include "app_globals.h"

#ifdef __ANDROID__
#include <SDL.h>
#endif

#include "config.h"
#include "system/helix_cache_dir_internal.h"
#include "system/helix_paths.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

static bool try_create_dir(const std::string& path) {
    return helix::paths::ensure_dir(path);
}

/// Is this candidate usable, WITHOUT creating anything to find out?
///
/// The cascade used to answer this by calling ensure_dir() and reading success
/// as the verdict, which meant asking where the cache lives MADE it live there.
/// Any caller that only wanted the path — a sweep deciding which stale
/// directories are safe to delete, a diagnostic printing the resolved location
/// — materialized a directory tree as a side effect of the question, and on a
/// device where a lower tier had been the real cache that silently split the
/// cache across two locations.
///
/// get_helix_cache_dir() still falls through when the real creation fails, so
/// races, quotas and ENOSPC keep their old behavior.
static bool cache_candidate_viable(const std::string& path) {
    return helix::paths::can_create_dir(path);
}

using helix::cache_internal::CacheCandidate;
using helix::cache_internal::is_deliberate;

namespace helix::cache_internal {

std::vector<std::string> board_state_roots(board::Board board, FlashForgeModTree tree) {
    switch (board) {
    case board::Board::Ad5m:
        // Dot-prefixed on purpose: /data is the only large writable partition on
        // this board and the vendor symlinks it whole into Moonraker's gcodes
        // root, so a plain-named directory shows up in the print-file picker.
        // Moonraker's listings hide dot-entries (the vendor's own .mod and .thumbs
        // rely on exactly that), no moonraker.conf edit needed.
        return {"/data/.helixscreen"};
    case board::Board::Cc1:
        // /user-resource is the 6.3GB ext4 partition. / is a read-only squashfs with
        // no /opt, so anything rooted there falls through to RAM-backed /tmp. The
        // -state sibling keeps state off the payload, which every update deletes.
        return {"/user-resource/helixscreen-state"};
    case board::Board::K2:
        // /mnt/UDISK is the 27.5GB user partition and carries both the payload and
        // its state; state sits in the -state sibling because the payload is
        // what an update replaces. /usr/data is on the root overlay, only ~240MB
        // and shared with the firmware, so it is the fallback for a unit without
        // the mount. Same two roots plr_backend.cpp probes for Creality data.
        return {"/mnt/UDISK/helixscreen-state", "/usr/data/helixscreen-state"};
    case board::Board::K1:
        // /usr/data IS the large user partition here, unlike on the K2. State
        // sits in a sibling of the payload rather than inside it, because the
        // payload is what an update replaces.
        return {"/usr/data/helixscreen-state"};
    case board::Board::Ad5x:
    case board::Board::Creator5:
    case board::Board::Creator5Pro:
        // Z-Mod keeps state under /srv; the Forge-X chroot has neither /srv nor
        // /usr/data and keeps it in the mod's durable config directory.
        if (tree == FlashForgeModTree::ForgeX)
            return {"/opt/config/mod_data/helixscreen-state"};
        return {"/srv/helixscreen-state"};
    case board::Board::SnapmakerU1:
        return {"/userdata/helixscreen-state"};
    case board::Board::Generic:
        break;
    }
    return {};
}

std::vector<CacheCandidate> board_cache_candidates(board::Board board, FlashForgeModTree tree,
                                                   const std::string& subdir) {
    const char* tier = board::info(board).key;
    std::vector<CacheCandidate> out;
    for (const std::string& root : board_state_roots(board, tree))
        out.push_back(CacheCandidate{root + "/cache/" + subdir, tier, true});
    return out;
}

std::vector<std::string> state_root_candidates(board::Board board, FlashForgeModTree tree,
                                               const std::string& state_dir,
                                               const std::string& home_state_dir) {
    std::vector<std::string> out = board_state_roots(board, tree);
    out.push_back(state_dir);
    out.push_back(home_state_dir);
    return out;
}

} // namespace helix::cache_internal

/// The mod tree only decides the FlashForge MIPS boards' state root, so no
/// other board stats for it.
static helix::FlashForgeModTree mod_tree_for(helix::board::Board board) {
    const bool flashforge_mips = board == helix::board::Board::Ad5x ||
                                 board == helix::board::Board::Creator5 ||
                                 board == helix::board::Board::Creator5Pro;
    return flashforge_mips ? helix::flashforge_mod_tree() : helix::FlashForgeModTree::None;
}

/// The cache cascade for `subdir`, in priority order. Pure: enumerating the
/// candidates touches nothing on disk.
static std::vector<CacheCandidate> cache_path_candidates(const std::string& subdir) {
    std::vector<CacheCandidate> out;

    // 1. HELIX_CACHE_DIR env var (explicit override)
    const char* helix_cache = std::getenv("HELIX_CACHE_DIR");
    if (helix_cache && helix_cache[0] != '\0')
        out.push_back({std::string(helix_cache) + "/" + subdir, "HELIX_CACHE_DIR", false});

    // 2. Config /cache/base_directory
    helix::Config* config = helix::Config::get_instance();
    std::string base = config->get<std::string>("/cache/base_directory", "");
    if (!base.empty())
        out.push_back({base + "/" + subdir, "config", false});

    // 3. The board's own rung
    const helix::board::Board board = helix::board::current();
    for (CacheCandidate& c :
         helix::cache_internal::board_cache_candidates(board, mod_tree_for(board), subdir))
        out.push_back(std::move(c));
#if defined(HELIX_PLATFORM_ANDROID) || defined(__ANDROID__)
    // Use SDL's Android internal storage path (app-private, no permissions needed)
    if (const char* android_path = SDL_AndroidGetInternalStoragePath())
        out.push_back({std::string(android_path) + "/cache/" + subdir, "Android", true});
#endif

    // 4/5. XDG cache base: $XDG_CACHE_HOME then $HOME/.cache (try each in order
    // so an uncreatable XDG dir still falls through to $HOME/.cache).
    for (const std::string& base : helix::paths::xdg_cache_bases())
        out.push_back({base + "/helix/" + subdir, nullptr, false});

    // 6. /var/tmp. Persistent and larger than /tmp on a general-purpose host,
    //    but on a Yocto/buildroot device it is often a symlink into the same
    //    tmpfs /tmp reaches, so reaching this rung is not proof of storage.
    //    get_helix_cache_dir() measures the chosen path rather than trusting
    //    the ordering here.
    out.push_back({"/var/tmp/helix_" + subdir, nullptr, false});

    // 7. Last resort: /tmp, RAM-backed on most embedded devices.
    out.push_back({"/tmp/helix_" + subdir, nullptr, false});

    return out;
}

std::string peek_helix_cache_dir(const std::string& subdir, const char** tier_out) {
    for (const CacheCandidate& c : cache_path_candidates(subdir)) {
        if (cache_candidate_viable(c.path)) {
            if (tier_out != nullptr)
                *tier_out = c.tier;
            return c.path;
        }
    }
    if (tier_out != nullptr)
        *tier_out = nullptr;
    return "";
}

std::string get_helix_cache_dir(const std::string& subdir) {
    for (const CacheCandidate& c : cache_path_candidates(subdir)) {
        // Viability is checked first so a candidate we cannot use is skipped
        // without leaving a directory behind as the cost of finding out.
        if (!cache_candidate_viable(c.path) || !try_create_dir(c.path)) {
            // A deliberate rung names a location someone chose on purpose, so
            // one that cannot be used is a misconfiguration, not a fallback.
            // Skipping it silently is how a path the device does not have stays
            // unnoticed while the cascade lands somewhere worse.
            if (is_deliberate(c))
                spdlog::warn("[CacheDir] {} path unusable, falling through: {}", c.tier, c.path);
            continue;
        }
        // Measured, not declared. A per-rung flag cannot see that /var/tmp is a
        // symlink into tmpfs on some devices, and bytes cached on tmpfs are
        // memory the app is otherwise trying to protect.
        if (helix::paths::is_ram_backed(c.path))
            spdlog::warn("[CacheDir] Cache dir is RAM-backed - cached bytes are memory: {}",
                         c.path);
        else if (c.tier)
            spdlog::info("[CacheDir] Cache dir ({}): {}", c.tier, c.path);
        return c.path;
    }

    spdlog::error("[CacheDir] Failed to create cache directory for '{}'", subdir);
    return "";
}

// ============================================================================
// STALE CACHE RECLAMATION
// ============================================================================

/// Cache subdirectories this build creates. Mirrors the get_helix_cache_dir()
/// call sites: ThumbnailCache::kCacheSubdir, ThumbnailProcessor, the gcode temp
/// and modified-file caches, ToolsUsedCache, and prerendered printer images. A
/// name missing here means a stale directory survives, which is harmless; a
/// name that is wrong would target something we do not own, which is not, so
/// this list is explicit rather than a glob.
static const char* const kHelixCacheSubdirs[] = {
    "helix_thumbs", "gcode_temp", "gcode_mod", "tools_used", "printer_images",
};

/// Remove any of @p paths that exists and whose final component names @p subdir.
///
/// Split out from the gate in sweep_stale_helix_cache_dirs() so the reclaim is
/// reachable from a host build, which defines no platform rung. Exposed to
/// tests via tests/test_helpers/helix_cache_dir_test_access.h.
///
/// The subdir check is belt and braces: every path handed here was built by
/// this cascade, but remove_all should not run on an unchecked shape.
int reclaim_cache_paths(const std::vector<std::string>& paths, const char* subdir) {
    int removed = 0;

    // The cascade spells the leaf two ways: nested under a helix/ parent
    // ("<base>/helix/gcode_temp", rungs 4-5) and flattened with a prefix
    // ("/var/tmp/helix_gcode_temp", rungs 6-7). Both must match or the bottom
    // two rungs are never reclaimable.
    const std::string nested = std::string("/") + subdir;
    const std::string flattened = std::string("/helix_") + subdir;
    auto ends_with = [](const std::string& s, const std::string& tail) {
        return s.size() > tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    };

    for (const std::string& path : paths) {
        if (!ends_with(path, nested) && !ends_with(path, flattened))
            continue;

        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec))
            continue;

        const std::uintmax_t n = std::filesystem::remove_all(path, ec);
        if (ec) {
            spdlog::debug("[CacheDir] Could not reclaim {}: {}", path, ec.message());
            continue;
        }
        spdlog::info("[CacheDir] Reclaimed stale cache from an older layout: {} ({} entries)", path,
                     n);
        ++removed;

        // Drop the now-empty parent (e.g. ~/.cache/helix) if nothing else lives
        // there. Fails harmlessly while siblings remain.
        std::filesystem::remove(std::filesystem::path(path).parent_path(), ec);
    }

    return removed;
}

namespace helix::cache_internal {

std::vector<std::string> select_stale_paths(const std::vector<CacheCandidate>& candidates,
                                            const std::function<bool(const std::string&)>& viable) {
    // An embedded build is one that HAS a platform rung, not one where it wins:
    // every platform hook exports HELIX_CACHE_DIR, so rung 1 wins on all of
    // them. A desktop build has no platform rung, so a developer who redirects
    // HELIX_CACHE_DIR never has their ~/.cache/helix swept.
    bool embedded_build = false;
    for (const CacheCandidate& c : candidates)
        embedded_build = embedded_build || c.platform;
    if (!embedded_build)
        return {};

    // Rungs above the winner were rejected as unusable; rungs below it were
    // never probed. Only the latter are safe to reclaim.
    size_t winner = candidates.size();
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (viable(candidates[i].path)) {
            winner = i;
            break;
        }
    }
    if (winner == candidates.size())
        return {}; // nothing usable at all: do not guess

    // A deliberate rung below the winner is stated intent, not an abandoned
    // directory: a device with both HELIX_CACHE_DIR and a config
    // base_directory must keep the config path.
    std::vector<std::string> stale;
    for (size_t i = winner + 1; i < candidates.size(); ++i)
        if (!is_deliberate(candidates[i]))
            stale.push_back(candidates[i].path);
    return stale;
}

int migrate_state_root(const std::string& legacy_root, const std::string& current_root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    int actions = 0;

    if (!fs::is_directory(legacy_root, ec))
        return 0;

    if (!fs::is_directory(current_root, ec)) {
        fs::rename(legacy_root, current_root, ec);
        if (ec) {
            spdlog::warn("[CacheDir] Could not move state root {} to {}: {}", legacy_root,
                         current_root, ec.message());
            return 0;
        }
        spdlog::info("[CacheDir] Moved state root {} -> {}", legacy_root, current_root);
        return 1;
    }

    // Both roots exist: a half-finished migration. Carry the known state
    // subtrees across, never clobbering — a subtree already on the new side
    // stays, and its legacy twin is left for the operator to inspect.
    for (const char* sub : {"cache", "logs"}) {
        const fs::path from = fs::path(legacy_root) / sub;
        const fs::path to = fs::path(current_root) / sub;
        if (!fs::is_directory(from, ec) || fs::exists(to, ec))
            continue;
        fs::rename(from, to, ec);
        if (!ec) {
            spdlog::info("[CacheDir] Moved state subtree {} -> {}", from.string(), to.string());
            ++actions;
        }
    }

    // An empty legacy tree is pure scaffolding: the pre-rename platform hook
    // recreates <legacy>/logs at every pre-start. fs::remove refuses a
    // non-empty directory, so anything with content survives on its own.
    for (const char* sub : {"cache", "logs"}) {
        const fs::path dir = fs::path(legacy_root) / sub;
        if (fs::is_empty(dir, ec)) {
            fs::remove(dir, ec);
            if (!ec)
                ++actions;
        }
    }
    fs::remove(legacy_root, ec);
    if (!ec)
        ++actions;
    return actions;
}

} // namespace helix::cache_internal

int sweep_stale_helix_cache_dirs() {
    int removed = 0;
    for (const char* subdir : kHelixCacheSubdirs) {
        const std::vector<std::string> stale = helix::cache_internal::select_stale_paths(
            cache_path_candidates(subdir), cache_candidate_viable);
        removed += reclaim_cache_paths(stale, subdir);
    }
    return removed;
}

std::string app_get_state_root() {
#if defined(HELIX_PLATFORM_ANDROID) || defined(__ANDROID__)
    // The redirected state dir is in Android's backup set, so per-device state
    // there would be cloned onto a restored device. The config dir is excluded.
    return {};
#else
    const helix::board::Board board = helix::board::current();
    for (const std::string& root : helix::cache_internal::state_root_candidates(
             board, mod_tree_for(board), helix::app_constants::update::state_dir(),
             helix::app_constants::update::backup_fallback_dir())) {
        if (helix::paths::can_create_dir(root) && helix::paths::ensure_dir(root) &&
            helix::paths::probe_writable(root))
            return root;
    }
    return {};
#endif
}

namespace helix {

void migrate_legacy_state_roots() {
    if (board::current() != board::Board::Ad5m)
        return;
    // /data is this board's only large writable partition and the vendor
    // symlinks it whole into Moonraker's gcodes root, so the state root has to
    // stay dot-prefixed to stay out of the print-file picker. A binary update
    // that lands without the installer (a dev deploy) still runs under the
    // pre-rename platform hook, whose exported defaults point at the old root;
    // those env values would recreate the visible directory on first use, so
    // they are repointed alongside the rename.
    constexpr const char* kLegacyRoot = "/data/helixscreen";
    constexpr const char* kCurrentRoot = "/data/.helixscreen";
    constexpr std::string_view kLegacyPrefix = "/data/helixscreen/";

    helix::cache_internal::migrate_state_root(kLegacyRoot, kCurrentRoot);

    for (const char* var : {"HELIX_CACHE_DIR", "HELIX_LOG_FILE"}) {
        const char* value = std::getenv(var);
        // string_view::starts_with is C++20; the build is -std=c++17.
        if (value == nullptr || std::string_view(value).rfind(kLegacyPrefix, 0) != 0)
            continue;
        const std::string repaired = std::string(kCurrentRoot) + (value + kLegacyPrefix.size() - 1);
        setenv(var, repaired.c_str(), 1);
        spdlog::info("[CacheDir] Repointed {} to the renamed state root: {}", var, repaired);
    }
}

} // namespace helix
