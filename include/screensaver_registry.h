// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

/**
 * @file screensaver_registry.h
 * @brief The screensavers the app knows, as data
 *
 * Always compiled, including in builds without screensavers, so settings code can clamp a
 * stored type without the saver sources. The saver implementations are created in
 * src/ui/screensaver/screensaver_manager.cpp.
 */

/**
 * @brief Available screensaver types
 *
 * In-app identity only. Config, HELIX_SCREENSAVER_NOW, ctl and the level store name a saver
 * by its stable kScreensavers name instead, so reordering or compiling out a saver never
 * changes what a stored value means.
 */
enum class ScreensaverType : int {
    OFF = 0,
    FLYING_TOASTERS = 1,
    STARFIELD = 2,
    PIPES_3D = 3,
    BOUNCING_PRINTER = 4,
    FIREWORKS = 5,
    BOUNCING_LOGO = 6,
    MAZE_3D = 7,
    HYPERSPACE = 8,
    FLIP_CLOCK = 9,
    FIREFLIES = 10,
    AQUA = 11,
    HIGHFLY = 12,
    /// Not a saver: each run starts a different registered saver the board can run. Kept clear
    /// of the row range, so savers added later never collide with it.
    RANDOM = 100,
};

namespace helix::ui {

/// Color depths a saver draws at, as bits of ScreensaverInfo::depths.
enum SaverDepth : uint8_t {
    SAVER_DEPTH_16 = 1u << 0,
    SAVER_DEPTH_32 = 1u << 1,
};

/// One registered screensaver.
struct ScreensaverInfo {
    ScreensaverType type;
    /// Stable name: the level store key and a HELIX_SCREENSAVER_NOW value.
    const char* name;
    /// Translation key, which is also the English label in the settings dropdown.
    const char* label_key;
    /// SaverDepth bits the saver draws at.
    uint8_t depths;
};

/// Every screensaver, in type order starting at 1: row i has type i + 1. The settings
/// dropdown in ui_xml/settings_display_overlay.xml lists "Off" and then these labels in
/// this order, which a test checks.
inline constexpr ScreensaverInfo kScreensavers[] = {
    {ScreensaverType::FLYING_TOASTERS, "toasters", "Flying Toasters",
     SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::STARFIELD, "starfield", "Starfield", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::PIPES_3D, "pipes", "3D Pipes", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::BOUNCING_PRINTER, "bounce", "Bouncing Printer",
     SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::FIREWORKS, "fireworks", "Fireworks", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::BOUNCING_LOGO, "logo", "Bouncing Logo", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::MAZE_3D, "maze", "3D Maze", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::HYPERSPACE, "hyperspace", "Hyperspace", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::FLIP_CLOCK, "flipclock", "Flip Clock", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::FIREFLIES, "fireflies", "Fireflies", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::AQUA, "aqua", "Aqua Surface", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::HIGHFLY, "highfly", "High Fly", SAVER_DEPTH_16 | SAVER_DEPTH_32},
};

inline constexpr size_t kScreensaverCount = std::size(kScreensavers);

/// Translation key and English label of the dropdown's first option.
inline constexpr const char* kScreensaverOffLabelKey = "Off";

/// Stable name of OFF, the persisted value of a screensaver turned off.
inline constexpr const char* kScreensaverOffName = "off";

/// Translation key and English label of the dropdown's last option.
inline constexpr const char* kScreensaverRandomLabelKey = "Random";

/// Stable name of RANDOM.
inline constexpr const char* kScreensaverRandomName = "random";

/// What each integer /display/screensaver_type value meant, indexed by that number. Frozen:
/// these numbers live on in old configs and backups, so the table never reorders or grows,
/// whatever happens to ScreensaverType or kScreensavers.
inline constexpr const char* kLegacyScreensaverTypeNames[] = {
    "off",       "toasters", "starfield", "pipes",      "bounce",
    "fireworks", "logo",     "maze",      "hyperspace", "flipclock",
};

/// Type a fresh install prefers, when this build can draw it. The screensaver gate measures it
/// and steps it down, or runs a lighter saver in its place, when it costs too much.
inline constexpr ScreensaverType kDefaultScreensaverType = ScreensaverType::FLIP_CLOCK;

/// Highest valid type value.
constexpr int screensaver_last_type() {
    return static_cast<int>(kScreensavers[kScreensaverCount - 1].type);
}

/// A stored type made valid: below OFF reads as OFF, past the last type as the last type, and
/// RANDOM stays RANDOM.
constexpr int clamp_screensaver_type(int value) {
    if (value == static_cast<int>(ScreensaverType::RANDOM)) {
        return value;
    }
    if (value < 0) {
        return 0;
    }
    if (value > screensaver_last_type()) {
        return screensaver_last_type();
    }
    return value;
}

/// The row for `type`, or nullptr for OFF and unregistered values.
constexpr const ScreensaverInfo* find_screensaver(ScreensaverType type) {
    for (const ScreensaverInfo& info : kScreensavers) {
        if (info.type == type) {
            return &info;
        }
    }
    return nullptr;
}

/// Type a fresh install runs at `build_depth` (a SaverDepth bit): the preferred default when it
/// draws at that depth, otherwise the first registered saver that does, and OFF if none do.
/// A 16 bpp build excludes the 32 bpp-only savers, so a default naming one would start nothing.
constexpr ScreensaverType default_screensaver_type(uint8_t build_depth) {
    const ScreensaverInfo* preferred = find_screensaver(kDefaultScreensaverType);
    if (preferred != nullptr && (preferred->depths & build_depth) != 0U) {
        return kDefaultScreensaverType;
    }
    for (const ScreensaverInfo& row : kScreensavers) {
        if ((row.depths & build_depth) != 0U) {
            return row.type;
        }
    }
    return ScreensaverType::OFF;
}

/// The row whose stable name is `name`, or nullptr.
constexpr const ScreensaverInfo* find_screensaver_by_name(std::string_view name) {
    for (const ScreensaverInfo& info : kScreensavers) {
        if (name == info.name) {
            return &info;
        }
    }
    return nullptr;
}

/// Persisted name of `type`: its row's stable name, "random" for RANDOM, or "off" for OFF and
/// unregistered values.
constexpr const char* screensaver_name(ScreensaverType type) {
    if (type == ScreensaverType::RANDOM) {
        return kScreensaverRandomName;
    }
    const ScreensaverInfo* info = find_screensaver(type);
    return info != nullptr ? info->name : kScreensaverOffName;
}

/// Translation key of `type`'s dropdown option: its row's label, Random, or Off.
constexpr const char* screensaver_label_key(ScreensaverType type) {
    if (type == ScreensaverType::RANDOM) {
        return kScreensaverRandomLabelKey;
    }
    const ScreensaverInfo* info = find_screensaver(type);
    return info != nullptr ? info->label_key : kScreensaverOffLabelKey;
}

/// ASCII case-insensitive equality.
constexpr bool equals_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

/// The type a stored name selects, ignoring case: "off", "random" or a registered name.
/// nullopt for anything else.
constexpr std::optional<ScreensaverType> screensaver_from_name(std::string_view name) {
    if (equals_ignoring_case(name, kScreensaverOffName)) {
        return ScreensaverType::OFF;
    }
    if (equals_ignoring_case(name, kScreensaverRandomName)) {
        return ScreensaverType::RANDOM;
    }
    for (const ScreensaverInfo& info : kScreensavers) {
        if (equals_ignoring_case(name, info.name)) {
            return info.type;
        }
    }
    return std::nullopt;
}

/// The name a legacy integer screensaver_type value stood for, or nullptr out of range.
constexpr const char* legacy_screensaver_name(long long value) {
    if (value < 0 || value >= static_cast<long long>(std::size(kLegacyScreensaverTypeNames))) {
        return nullptr;
    }
    return kLegacyScreensaverTypeNames[value];
}

/**
 * @brief Saver a HELIX_SCREENSAVER_NOW value asks for
 *
 * A registered name starts that saver and "random" a random one. Any other value starts the
 * configured saver, or flying toasters when the configured type is OFF.
 */
constexpr ScreensaverType resolve_screensaver_now(std::string_view value,
                                                  ScreensaverType configured) {
    if (const ScreensaverInfo* info = find_screensaver_by_name(value)) {
        return info->type;
    }
    if (value == kScreensaverRandomName) {
        return ScreensaverType::RANDOM;
    }
    return configured != ScreensaverType::OFF ? configured : ScreensaverType::FLYING_TOASTERS;
}

/**
 * @brief The saver that runs when `configured` is chosen
 *
 * `configured` itself unless `too_heavy(type)` rejects it; then `preferred`, then the first
 * registered saver it does not reject. When it rejects every saver, `configured`,
 * which the manager shows as a static black screen. OFF stays OFF. The stored choice is never
 * rewritten, so a config moved to a faster board runs the saver it names.
 */
template <typename TooHeavy>
constexpr ScreensaverType
runnable_screensaver(ScreensaverType configured, TooHeavy&& too_heavy,
                     ScreensaverType preferred = kDefaultScreensaverType) {
    if (configured == ScreensaverType::OFF || !too_heavy(configured)) {
        return configured;
    }
    if (!too_heavy(preferred)) {
        return preferred;
    }
    for (const ScreensaverInfo& row : kScreensavers) {
        if (!too_heavy(row.type)) {
            return row.type;
        }
    }
    return configured;
}

/// What the settings dropdown offers with `configured` stored, in order: OFF, every registered
/// saver `too_heavy(type)` does not reject, then RANDOM. When every saver is too heavy the
/// configured one stays listed, and RANDOM only when it is the one stored, so the dropdown
/// shows what is stored and choosing Off is a change.
template <typename TooHeavy>
std::vector<ScreensaverType> screensaver_choices(TooHeavy&& too_heavy, ScreensaverType configured) {
    const ScreensaverType runs = runnable_screensaver(configured, too_heavy);
    std::vector<ScreensaverType> choices{ScreensaverType::OFF};
    bool any_runnable = false;
    for (const ScreensaverInfo& row : kScreensavers) {
        const bool runnable = !too_heavy(row.type);
        any_runnable = any_runnable || runnable;
        if (row.type == runs || runnable) {
            choices.push_back(row.type);
        }
    }
    if (any_runnable || configured == ScreensaverType::RANDOM) {
        choices.push_back(ScreensaverType::RANDOM);
    }
    return choices;
}

/**
 * @brief The saver a Random run starts
 *
 * One of the registered savers `too_heavy(type)` does not reject, chosen by `roll`, leaving out
 * `previous` while any other is runnable so consecutive runs differ. With every saver too heavy,
 * the default, which the manager then shows as its black screen.
 */
template <typename TooHeavy>
ScreensaverType random_screensaver(TooHeavy&& too_heavy, uint32_t roll, ScreensaverType previous) {
    std::vector<ScreensaverType> candidates;
    for (const ScreensaverInfo& row : kScreensavers) {
        if (row.type != previous && !too_heavy(row.type)) {
            candidates.push_back(row.type);
        }
    }
    if (candidates.empty()) {
        return find_screensaver(previous) != nullptr && !too_heavy(previous)
                   ? previous
                   : kDefaultScreensaverType;
    }
    return candidates[roll % candidates.size()];
}

/// Dropdown index of `runs` in `choices`, or 0 (Off) when it is missing, which choices built
/// for the same stored saver never are.
inline size_t screensaver_choice_index(const std::vector<ScreensaverType>& choices,
                                       ScreensaverType runs) {
    for (size_t i = 0; i < choices.size(); i++) {
        if (choices[i] == runs) {
            return i;
        }
    }
    return 0;
}

} // namespace helix::ui
