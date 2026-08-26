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

namespace {
/// One rung of the cache cascade. `tier` labels the rung in the log so the
/// resolved location stays diagnosable; nullptr means resolve quietly.
struct CacheCandidate {
    std::string path;
    const char* tier;
    bool ram_backed;
};
} // namespace

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
    out.push_back({"/data/helixscreen/cache/" + subdir, "AD5M", false});
#elif defined(HELIX_PLATFORM_CC1)
    out.push_back({"/opt/helixscreen/cache/" + subdir, "CC1", false});
#elif defined(HELIX_PLATFORM_K2)
    // The K2 mounts its bulk storage at /mnt/UDISK (27.5GB). /usr/data is
    // on the root overlay, which is only ~240MB and shared with the
    // install itself, so caching thumbnails and modified gcode there
    // competes with the firmware for the smallest partition on the box.
    // Probed in order so a unit without the mount still gets a cache.
    // Mirrors CREALITY_DATA_ROOTS in plr_backend.cpp.
    for (const char* root : {"/mnt/UDISK", "/usr/data"})
        out.push_back({std::string(root) + "/helixscreen/cache/" + subdir, "K2", false});
#elif defined(HELIX_PLATFORM_MIPS)
    // K1 series: /usr/data IS the large user partition here, unlike on the K2.
    out.push_back({"/usr/data/helixscreen/cache/" + subdir, "MIPS", false});
#elif defined(HELIX_PLATFORM_ANDROID) || defined(__ANDROID__)
    // Use SDL's Android internal storage path (app-private, no permissions needed)
    if (const char* android_path = SDL_AndroidGetInternalStoragePath())
        out.push_back({std::string(android_path) + "/cache/" + subdir, "Android", false});
#endif

    // 4/5. XDG cache base: $XDG_CACHE_HOME then $HOME/.cache (try each in order
    // so an uncreatable XDG dir still falls through to $HOME/.cache).
    for (const std::string& base : helix::paths::xdg_cache_bases())
        out.push_back({base + "/helix/" + subdir, nullptr, false});

    // 6. /var/tmp (persistent, often larger than /tmp on embedded)
    out.push_back({"/var/tmp/helix_" + subdir, nullptr, false});

    // 7. Last resort: /tmp (may be RAM-backed tmpfs)
    out.push_back({"/tmp/helix_" + subdir, nullptr, true});

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
