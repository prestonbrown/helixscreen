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
        out.push_back({std::string(helix_cache) + "/" + subdir, "HELIX_CACHE_DIR", false});

    // 2. Config /cache/base_directory
    if (helix::Config* config = helix::Config::get_instance()) {
        std::string base = config->get<std::string>("/cache/base_directory", "");
        if (!base.empty())
            out.push_back({base + "/" + subdir, "config", false});
    }

    // 3. Platform-specific compile-time paths
#if defined(HELIX_PLATFORM_AD5M)
    out.push_back({"/data/helixscreen/cache/" + subdir, "AD5M", true});
#elif defined(HELIX_PLATFORM_CC1)
    // /user-resource is the 6.3GB ext4 partition. / is a read-only squashfs with
    // no /opt, so anything rooted there falls through to RAM-backed /tmp. The
    // -state sibling keeps the cache off the payload, which every update deletes.
    out.push_back({"/user-resource/helixscreen-state/cache/" + subdir, "CC1", true});
#elif defined(HELIX_PLATFORM_K2)
    // /mnt/UDISK is the 27.5GB user partition and carries both the payload and
    // its state; the cache sits in the -state sibling because the payload is
    // what an update replaces. /usr/data is on the root overlay, only ~240MB
    // and shared with the firmware, so it is the fallback for a unit without
    // the mount. Same two roots plr_backend.cpp probes for Creality data.
    out.push_back({"/mnt/UDISK/helixscreen-state/cache/" + subdir, "K2", true});
    out.push_back({"/usr/data/helixscreen-state/cache/" + subdir, "K2", true});
#elif defined(HELIX_PLATFORM_MIPS)
    // K1 series: /usr/data IS the large user partition here, unlike on the K2.
    // The cache sits in a sibling of the payload rather than inside it, because
    // the payload is what an update replaces.
    out.push_back({"/usr/data/helixscreen-state/cache/" + subdir, "MIPS", true});
#elif defined(HELIX_PLATFORM_ANDROID) || defined(__ANDROID__)
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
/// call sites: ThumbnailCache::CACHE_SUBDIR, ThumbnailProcessor, the gcode temp
/// and modified-file caches, ToolsUsedCache, and prerendered printer images. A
/// name missing here means a stale directory survives, which is harmless; a
/// name that is wrong would target something we do not own, which is not, so
/// this list is explicit rather than a glob.
static const char* const HELIX_CACHE_SUBDIRS[] = {
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
