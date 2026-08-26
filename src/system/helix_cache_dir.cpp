// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file helix_cache_dir.cpp
 * @brief Cache-directory resolution cascade.
 *
 * Lifted out of app_globals.cpp so the TEST BINARY can link the real thing.
 * app_globals.o is excluded from the test link (mk/tests.mk) because it carries
 * the global subjects and app state, and tests/ui_test_utils.cpp used to supply
 * a hand-written get_helix_cache_dir() in its place — a two-rung fake that
 * honoured HELIX_CACHE_DIR and otherwise returned "/tmp/helix_test_<subdir>",
 * a shape this cascade never produces. Every assertion in
 * tests/unit/test_cache_dir.cpp landed on that fake, so rungs 2-7 (config,
 * platform, XDG, HOME, /var/tmp, /tmp) were never under test at all. Same drift
 * mk/tests.mk documents for ui_text_input.o and ui_emergency_stop.o.
 *
 * This translation unit has no global subjects and no app state — only Config,
 * helix::paths and spdlog — so it links into the test binary cleanly and the
 * stub is gone. Test isolation now comes from HELIX_CACHE_DIR, which is rung 1
 * of the real cascade, rather than from a second implementation of it.
 *
 * Declarations stay in app_globals.h: the split is about what links, not about
 * churning 28 call sites.
 */

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

/// The cache cascade for `subdir`, in priority order. Pure: enumerating the
/// candidates touches nothing on disk.
static std::vector<CacheCandidate> cache_path_candidates(const std::string& subdir) {
    std::vector<CacheCandidate> out;

    // 1. HELIX_CACHE_DIR env var (explicit override)
    const char* helix_cache = std::getenv("HELIX_CACHE_DIR");
    if (helix_cache && helix_cache[0] != '\0')
        out.push_back({std::string(helix_cache) + "/" + subdir, "HELIX_CACHE_DIR", false, false});

    // 2. Config /cache/base_directory
    if (helix::Config* config = helix::Config::get_instance()) {
        std::string base = config->get<std::string>("/cache/base_directory", "");
        if (!base.empty())
            out.push_back({base + "/" + subdir, "config", false, false});
    }

    // 3. Platform-specific compile-time paths
#if defined(HELIX_PLATFORM_AD5M)
    out.push_back({"/data/helixscreen/cache/" + subdir, "AD5M", false, true});
#elif defined(HELIX_PLATFORM_CC1)
    out.push_back({"/opt/helixscreen/cache/" + subdir, "CC1", false, true});
#elif defined(HELIX_PLATFORM_K2)
    // The K2 mounts its bulk storage at /mnt/UDISK (27.5GB). /usr/data is
    // on the root overlay, which is only ~240MB and shared with the
    // install itself, so caching thumbnails and modified gcode there
    // competes with the firmware for the smallest partition on the box.
    // Probed in order so a unit without the mount still gets a cache.
    // Mirrors CREALITY_DATA_ROOTS in plr_backend.cpp.
    for (const char* root : {"/mnt/UDISK", "/usr/data"})
        out.push_back({std::string(root) + "/helixscreen/cache/" + subdir, "K2", false, true});
#elif defined(HELIX_PLATFORM_MIPS)
    // K1 series: /usr/data IS the large user partition here, unlike on the K2.
    out.push_back({"/usr/data/helixscreen/cache/" + subdir, "MIPS", false, true});
#elif defined(HELIX_PLATFORM_ANDROID) || defined(__ANDROID__)
    // Use SDL's Android internal storage path (app-private, no permissions needed)
    if (const char* android_path = SDL_AndroidGetInternalStoragePath())
        out.push_back({std::string(android_path) + "/cache/" + subdir, "Android", false, true});
#endif

    // 4/5. XDG cache base: $XDG_CACHE_HOME then $HOME/.cache (try each in order
    // so an uncreatable XDG dir still falls through to $HOME/.cache).
    for (const std::string& base : helix::paths::xdg_cache_bases())
        out.push_back({base + "/helix/" + subdir, nullptr, false, false});

    // 6. /var/tmp (persistent, often larger than /tmp on embedded)
    out.push_back({"/var/tmp/helix_" + subdir, nullptr, false, false});

    // 7. Last resort: /tmp (may be RAM-backed tmpfs)
    out.push_back({"/tmp/helix_" + subdir, nullptr, true, false});

    return out;
}

std::string peek_helix_cache_dir(const std::string& subdir) {
    for (const CacheCandidate& c : cache_path_candidates(subdir)) {
        if (cache_candidate_viable(c.path))
            return c.path;
    }
    return "";
}

std::string get_helix_cache_dir(const std::string& subdir) {
    for (const CacheCandidate& c : cache_path_candidates(subdir)) {
        // Viability is checked first so a candidate we cannot use is skipped
        // without leaving a directory behind as the cost of finding out.
        if (!cache_candidate_viable(c.path))
            continue;
        if (!try_create_dir(c.path))
            continue;
        if (c.ram_backed)
            spdlog::warn("[CacheDir] Using /tmp for cache - may be RAM-backed");
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
/// call sites: ThumbnailCache::CACHE_SUBDIR, ThumbnailProcessor, the gcode temp
/// and modified-file caches, ToolsUsedCache, and prerendered printer images. A
/// name missing here means a stale directory survives, which is harmless; a
/// name that is wrong would target something we do not own, which is not, so
/// this list is explicit rather than a glob.
static const char* const HELIX_CACHE_SUBDIRS[] = {
    "helix_thumbs", "gcode_temp", "gcode_mod", "tools_used", "printer_images",
};

/// Remove any of @p paths that exists and whose final component is @p subdir.
///
/// Split out from the platform gate below so the reclaim itself is reachable
/// from a host build: sweep_stale_helix_cache_dirs() only proceeds when a
/// compile-time platform rung wins the cascade, and no such rung exists on x86,
/// so the gate would otherwise make this code permanently untestable. Exposed
/// to tests via tests/test_helpers/helix_cache_dir_test_access.h.
///
/// The subdir suffix check is belt and braces. Every path handed here is one
/// this cascade built, but remove_all is not an operation to run on a path
/// whose shape nobody checked.
int reclaim_cache_paths(const std::vector<std::string>& paths, const char* subdir) {
    int removed = 0;
    const std::string suffix = std::string("/") + subdir;

    for (const std::string& path : paths) {
        if (path.size() <= suffix.size() ||
            path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
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
    // An embedded build is one that HAS a platform rung, not one where that rung
    // wins. Gating on winning is what the first cut did, and it made the sweep
    // dead code on every device we ship: each platform hook exports
    // HELIX_CACHE_DIR (hooks-k1.sh, hooks-k2.sh, hooks-cc1.sh, hooks-ad5m-*.sh,
    // hooks-ad5x.sh, hooks-m1.sh, hooks-snapmaker-u1.sh), so rung 1 always wins
    // and rung 3 never does. Confirmed on a K1 and a K2: both logged
    // "Cache dir (HELIX_CACHE_DIR)" and reclaimed nothing.
    //
    // Presence keeps the property that mattered: a desktop build has no platform
    // rung at all, so a developer who redirects HELIX_CACHE_DIR still never has
    // their ordinary ~/.cache/helix swept.
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

    // Deliberate rungs below the winner are somebody's stated intent, not an
    // abandoned directory - a device with both HELIX_CACHE_DIR and a config
    // base_directory set must keep the config path.
    std::vector<std::string> stale;
    for (size_t i = winner + 1; i < candidates.size(); ++i)
        if (!is_deliberate(candidates[i]))
            stale.push_back(candidates[i].path);
    return stale;
}

} // namespace helix::cache_internal

int sweep_stale_helix_cache_dirs() {
    int removed = 0;
    for (const char* subdir : HELIX_CACHE_SUBDIRS) {
        const std::vector<std::string> stale = helix::cache_internal::select_stale_paths(
            cache_path_candidates(subdir), cache_candidate_viable);
        removed += reclaim_cache_paths(stale, subdir);
    }
    return removed;
}
